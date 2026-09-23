// 文件作用：实现 ChatServer1 对其他 ChatServer 提供的 gRPC 服务。
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
using message::FileChatMsgReq;
using message::FileChatMsgRsp;

using message::KickUserReq;
using message::KickUserRsp;
using message::MessageDisplayedReq;
using message::MessageDisplayedRsp;
using message::ThreadReadReq;
using message::ThreadReadRsp;


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
    // 将已持久化文件通知转换为接收用户的 1042 TCP JSON；request 为来源服消息，response 回传处理结果。
    Status NotifyFileChatMsg(ServerContext* context, const FileChatMsgReq* request, FileChatMsgRsp* response) override;
    Status NotifyMessageDisplayed(ServerContext* context, const MessageDisplayedReq* request,
                                  MessageDisplayedRsp* response) override;
    Status NotifyThreadRead(ServerContext* context, const ThreadReadReq* request,
                            ThreadReadRsp* response) override;
    //bool GetBaseInfo(std::string base_key, int uid, std::shared_ptr<UserInfo>& userinfo);
    Status NotifyKickUser(ServerContext* context, const KickUserReq* request, KickUserRsp* response) override;

    void RegisterServer(std::shared_ptr<CServer> p_server);
private:
    std::shared_ptr<CServer> _p_server;
};




#endif // CHATSERVER1_CHATSERVICEIMPL_H
