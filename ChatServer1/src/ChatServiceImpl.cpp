#include "ChatServiceImpl.h"
#include "UserMgr.h"
#include "CSession.h"
#include <json.h>
#include <json-forwards.h>
#include "RedisMgr.h"
#include "MysqlMgr.h"
#include <iostream>

ChatServiceImpl::ChatServiceImpl(){

}

Status ChatServiceImpl::NotifyAddFriend(ServerContext* context, const AddFriendReq* request, AddFriendRsp* response){
    // 查询用户是否在本服务器
    auto touid = request->touid();
    auto session = UserMgr::GetInstance()->GetSession(touid);

    Defer defer([request, response](){
        response->set_error(ErrorCode::Success);
        response->set_applyuid(request->applyuid());
        response->set_touid(request->touid());
    });

    if(session == nullptr){
        // 用户不在内存中直接返回，让用户登录的时候自己读数据库就行
        return Status::OK;
    }

    // 在内存中直接发送通知
    Json::Value rtvalue;
    rtvalue["error"] = ErrorCode::Success;
    rtvalue["applyuid"] = request->applyuid();
    rtvalue["name"] = request->name();
    rtvalue["desc"] = request->desc();
    rtvalue["icon"] = request->icon();
    rtvalue["sex"] = request->sex();
    rtvalue["nick"] = request->nick();

    std::string return_str = rtvalue.toStyledString();
    session->Send(return_str, ID_NOTIFY_ADD_FRIEND_REQ); // 发送添加好友请求，通知客户端

    return Status::OK;
}


Status ChatServiceImpl::NotifyAuthFriend(ServerContext* context, const AuthFriendReq* request, AuthFriendRsp* response){
    auto touid = request->touid();
    auto session = UserMgr::GetInstance()->GetSession(touid);
    response->set_error(ErrorCode::Success);
    response->set_uid(request->fromuid());
    response->set_touid(request->touid());

    if(session == nullptr){
        // 用户不在内存中直接返回
        return Status::OK;
    }

    // 申请方客户端需要认证者的完整资料来立即创建好友条目。
    UserInfo approverInfo = MysqlMgr::GetInstance()->GetUserInfo(request->fromuid());
    if (approverInfo.uid != request->fromuid()) {
        response->set_error(ErrorCode::UidInvalid);
        return Status::OK;
    }

    Json::Value rtvalue;
    rtvalue["error"] = ErrorCode::Success;
    rtvalue["uid"] = request->fromuid();
    rtvalue["touid"] = request->touid();
    rtvalue["name"] = approverInfo.user;
    rtvalue["nick"] = approverInfo.nick;
    rtvalue["desc"] = approverInfo.desc;
    rtvalue["sex"] = approverInfo.sex;
    rtvalue["icon"] = approverInfo.icon;
    rtvalue["bakname"] = approverInfo.user;

    // gRPC 的 repeated AddFriendMsg 不能直接发送给 Qt；在 TCP JSON 中保留同名
    // textmsgs 字段和逐项字段名，客户端可按 message_id 幂等写入 SQLite。
    Json::Value textMessages(Json::arrayValue);
    for (const auto &message : request->textmsgs()) {
        Json::Value textMessage;
        textMessage["sender_id"] = message.sender_id();
        textMessage["unique_id"] = message.unique_id();
        textMessage["msg_id"] = message.msg_id();
        textMessage["thread_id"] = message.thread_id();
        textMessage["msgcontent"] = message.msgcontent();
        textMessages.append(std::move(textMessage));
    }
    rtvalue["textmsgs"] = textMessages;

    std::string return_str = rtvalue.toStyledString();
    session->Send(return_str, ID_NOTIFY_AUTH_FRIEND_REQ); // 发送认证好友请求

    return Status::OK;
}
    
Status ChatServiceImpl::NotifyTextChatMsg(ServerContext* context, const TextChatMsgReq* request, TextChatMsgRsp* response){

    //查找用户是否在本服务器
    auto touid = request->touid();
    auto session = UserMgr::GetInstance()->GetSession(touid);
    response->set_error(ErrorCode::Success);

    // Redis 路由说目标用户在本机，但会话已经不存在时，不能伪装成发送成功。
    if (session == nullptr) {
        response->set_error(ErrorCode::UidInvalid);
        return Status::OK;
    }

    //在内存中则直接发送通知对方
    Json::Value  rtvalue;
    rtvalue["error"] = ErrorCode::Success;
    rtvalue["fromuid"] = request->fromuid();
    rtvalue["touid"] = request->touid();

    //将聊天数据组织为数组
    Json::Value text_array;
    for (auto& msg : request->textmsgs()) {
        Json::Value element;
        element["content"] = msg.msgcontent();
        element["msgid"] = msg.msgid();
        // 跨服通知直接透传服务端确认后的元数据，Qt 收到后立即按 message_id 去重落库。
        element["message_id"] = static_cast<Json::UInt64>(msg.message_id());
        element["thread_id"] = static_cast<Json::UInt64>(msg.thread_id());
        element["sender_id"] = msg.sender_id();
        element["recv_id"] = msg.recv_id();
        element["created_at_ms"] = static_cast<Json::UInt64>(msg.created_at_ms());
        element["status"] = msg.status();
        text_array.append(element);
    }
    rtvalue["textArray"] = text_array;

    std::string return_str = rtvalue.toStyledString();

    std::cout << "text chat gRPC push, from = " << request->fromuid()
              << ", to = " << touid << std::endl;
    session->Send(return_str, ID_NOTIFY_TEXT_CHAT_MSG_REQ);
    return Status::OK;
}

Status ChatServiceImpl::NotifyImageChatMsg(ServerContext* context, const ImageChatMsgReq* request,
                                           ImageChatMsgRsp* response) {
    (void)context;
    response->set_error(ErrorCode::Success);
    response->set_fromuid(request->fromuid());
    response->set_touid(request->touid());
    const auto session = UserMgr::GetInstance()->GetSession(request->touid());
    if (!session) {
        // 发送方已经落库；会话在 RPC 到达前断开时返回失败，使其仅标记 delivered=false，
        // 接收者下次仍会从历史增量读取同一条消息。
        response->set_error(ErrorCode::UidInvalid);
        std::cout << "image chat gRPC target session missing, from=" << request->fromuid()
                  << ", to=" << request->touid() << std::endl;
        return Status::OK;
    }

    Json::Value notification;
    notification["error"] = ErrorCode::Success;
    notification["fromuid"] = request->fromuid();
    notification["touid"] = request->touid();
    notification["delivered"] = true;
    Json::Value imageArray(Json::arrayValue);
    for (const auto& image : request->imagemsgs()) {
        // gRPC 只转发源 ChatServer 已经核验并持久化的数据；这里不重新接受客户端输入。
        Json::Value item;
        item["msgid"] = image.msgid();
        item["resource_id"] = image.resource_id();
        item["name"] = image.name();
        item["mime_type"] = image.mime_type();
        item["file_size"] = static_cast<Json::UInt64>(image.file_size());
        item["width"] = image.width();
        item["height"] = image.height();
        item["message_id"] = static_cast<Json::UInt64>(image.message_id());
        item["thread_id"] = static_cast<Json::UInt64>(image.thread_id());
        item["sender_id"] = image.sender_id();
        item["recv_id"] = image.recv_id();
        item["created_at_ms"] = static_cast<Json::UInt64>(image.created_at_ms());
        item["status"] = image.status();
        imageArray.append(std::move(item));
    }
    notification["imageArray"] = imageArray;
    session->Send(notification.toStyledString(), ID_NOTIFY_IMAGE_CHAT_MSG_REQ);
    std::cout << "image chat gRPC push queued, from=" << request->fromuid()
              << ", to=" << request->touid()
              << ", image_count=" << request->imagemsgs_size() << std::endl;
    return Status::OK;
}

Status ChatServiceImpl::NotifyMessageDisplayed(ServerContext* context, const MessageDisplayedReq* request,
                                               MessageDisplayedRsp* response) {
    // RPC 只负责把已经持久化的展示结果投递给本机在线发送端，不能以 RPC 参数为准
    // 再次修改数据库；这样重试和跨服重复投递至多造成重复 UI 通知，不会篡改状态。
    (void)context;
    response->set_error(ErrorCode::Success);
    if (!request || request->sender_id() <= 0 || request->reader_id() <= 0 || request->thread_id() == 0
        || request->message_ids_size() == 0 || request->message_ids_size() > 100) {
        response->set_error(ErrorCode::Error_Json);
        return Status::OK;
    }
    const auto session = UserMgr::GetInstance()->GetSession(request->sender_id());
    if (!session) {
        return Status::OK; // 发送端离线时其下次 1030 同步会读取 displayed_at。
    }
    Json::Value notification;
    notification["error"] = ErrorCode::Success;
    notification["thread_id"] = static_cast<Json::UInt64>(request->thread_id());
    notification["reader_id"] = request->reader_id();
    notification["peer_displayed"] = true;
    Json::Value messageIds(Json::arrayValue);
    for (std::uint64_t id : request->message_ids()) {
        messageIds.append(static_cast<Json::UInt64>(id));
    }
    notification["message_ids"] = messageIds;
    session->Send(notification.toStyledString(), ID_NOTIFY_MESSAGE_DISPLAYED);
    return Status::OK;
}

Status ChatServiceImpl::NotifyThreadRead(ServerContext* context, const ThreadReadReq* request,
                                         ThreadReadRsp* response) {
    // 已读状态在来源 ChatServer 的事务路径中确认。本端仅作在线通知，离线时返回成功
    // 以避免来源端将“接收者不在线”误判为持久化失败并重试状态更新。
    (void)context;
    response->set_error(ErrorCode::Success);
    if (!request || request->sender_id() <= 0 || request->reader_id() <= 0 || request->thread_id() == 0
        || request->read_through_message_id() == 0) {
        response->set_error(ErrorCode::Error_Json);
        return Status::OK;
    }
    const auto session = UserMgr::GetInstance()->GetSession(request->sender_id());
    if (!session) {
        return Status::OK;
    }
    Json::Value notification;
    notification["error"] = ErrorCode::Success;
    notification["thread_id"] = static_cast<Json::UInt64>(request->thread_id());
    notification["reader_id"] = request->reader_id();
    notification["read_through_message_id"] = static_cast<Json::UInt64>(request->read_through_message_id());
    session->Send(notification.toStyledString(), ID_NOTIFY_THREAD_READ);
    return Status::OK;
}


Status ChatServiceImpl::NotifyKickUser(ServerContext* context, const KickUserReq* request, KickUserRsp* response){
    // 查询用户是否在本服务器
    auto uid = request->uid();
    response->set_uid(uid);
    response->set_error(ErrorCode::Success);
    auto session = UserMgr::GetInstance()->GetSession(uid);

    // 踢人接口采用幂等语义：会话已经不存在，也视为目标已下线。
    if(session == nullptr){
        std::cout << "kick target already offline, uid = " << uid << std::endl;
        return Status::OK;
    }

    // 请求必须命中发起方看到的那一代旧会话；陈旧或缺失的 session_id 不允许按 uid 盲踢。
    if(request->session_id().empty() || session->GetSessionId() != request->session_id()){
        std::cout << "reject stale kick request, uid = " << uid
                  << ", requested session = " << request->session_id()
                  << ", current session = " << session->GetSessionId() << std::endl;
        response->set_error(ErrorCode::ServerBusy);
        return Status::OK;
    }

    if(!_p_server){
        response->set_error(ErrorCode::RPCFaild);
        return Status::OK;
    }

    // 在内存中直接通知对方即可
    session->NotifyOffline();
    std::cout << "notify old client offline, uid = " << uid
              << ", session = " << session->GetSessionId() << std::endl;
    // 这里只立即注销旧连接的本地映射和在线数，保留 socket 让 1021 下线通知发送完成。
    // 队列排空后 HandleWrite 会调用 HandleDisconnect，统一关闭 socket 并条件清理 Redis。
    _p_server->ClearSession(session->GetSessionId());

    return Status::OK;
}

void ChatServiceImpl::RegisterServer(std::shared_ptr<CServer> p_server){
    _p_server = p_server;
}
