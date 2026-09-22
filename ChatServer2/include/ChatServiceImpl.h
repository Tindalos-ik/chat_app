// rpc通信服务端 ， 继承service，重写服务函数
#pragma once
#ifndef CHATSERVER1_CHATSERVICEIMPL_H
#define CHATSERVER1_CHATSERVICEIMPL_H

#include <grpcpp/grpcpp.h>
#include "message.grpc.pb.h"
#include "message.pb.h"
#include <mutex>
#include "data.h"
#include "CServer.h"

using grpc::Channel;
using grpc::Status;
using grpc::ServerContext; // 服务端上下文

using message::AddFriendReq;
using message::AddFriendRsp;

using message::AuthFriendReq;
using message::AuthFriendRsp;

using message::GetChatServerRsp;
using message::LoginReq;
using message::LoginRsp;
using message::ChatService;

using message::TextChatMsgReq;
using message::TextChatMsgRsp;
using message::TextChatData;

using message::ImageChatMsgReq;
using message::ImageChatMsgRsp;
using message::ImageChatData;

using message::KickUserReq;
using message::KickUserRsp;


class ChatServiceImpl final : public ChatService::Service
// final 表示该类不能被继承
{
public:
    ChatServiceImpl();

    // 参数如何确定的呢，去 基类 看看即可
    Status NotifyAddFriend(ServerContext* context, const AddFriendReq* request, AddFriendRsp* response) override;
    Status NotifyAuthFriend(ServerContext* context, const AuthFriendReq* request, AuthFriendRsp* response) override;
    Status NotifyTextChatMsg(ServerContext* context, const TextChatMsgReq* request, TextChatMsgRsp* response) override;
    Status NotifyImageChatMsg(ServerContext* context, const ImageChatMsgReq* request, ImageChatMsgRsp* response) override;
    //bool GetBaseInfo(std::string base_key, int uid, std::shared_ptr<UserInfo>& userinfo);
    Status NotifyKickUser(ServerContext* context, const KickUserReq* request, KickUserRsp* response) override;

    void RegisterServer(std::shared_ptr<CServer> p_server);
private:
    std::shared_ptr<CServer> _p_server;
};




#endif // CHATSERVER1_CHATSERVICEIMPL_H
