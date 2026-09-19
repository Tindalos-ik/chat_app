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
    response->set_uid(request->uid());
    response->set_touid(request->touid());

    if(session == nullptr){
        // 用户不在内存中直接返回
        return Status::OK;
    }

    // 申请方客户端需要认证者的完整资料来立即创建好友条目。
    UserInfo approverInfo = MysqlMgr::GetInstance()->GetUserInfo(request->uid());
    if (approverInfo.uid != request->uid()) {
        response->set_error(ErrorCode::UidInvalid);
        return Status::OK;
    }

    Json::Value rtvalue;
    rtvalue["error"] = ErrorCode::Success;
    rtvalue["uid"] = request->uid();
    rtvalue["touid"] = request->touid();
    rtvalue["name"] = approverInfo.user;
    rtvalue["nick"] = approverInfo.nick;
    rtvalue["desc"] = approverInfo.desc;
    rtvalue["sex"] = approverInfo.sex;
    rtvalue["icon"] = approverInfo.icon;
    rtvalue["bakname"] = approverInfo.user;

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
        text_array.append(element);
    }
    rtvalue["textArray"] = text_array;

    std::string return_str = rtvalue.toStyledString();

    std::cout << "text chat gRPC push, from = " << request->fromuid()
              << ", to = " << touid << std::endl;
    session->Send(return_str, ID_NOTIFY_TEXT_CHAT_MSG_REQ);
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
