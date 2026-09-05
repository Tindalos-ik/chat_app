#include "ChatServiceImpl.h"
#include "UserMgr.h"
#include "CSession.h"
#include <json.h>
#include <json-forwards.h>
#include "RedisMgr.h"
#include "MysqlMgr.h"

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
    return Status::OK;
}
    
Status ChatServiceImpl::NotifyTextChatMsg(ServerContext* context, const TextChatMsgReq* request, TextChatMsgRsp* response){
    return Status::OK;
}
    
bool ChatServiceImpl::GetBaseInfo(std::string base_key, int uid, std::shared_ptr<UserInfo>& userinfo){
    return true;
}