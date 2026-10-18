// 文件作用：ChatServer跨实例通知客户端及stub池；群只转发已提交信封，池等待与RPC共用截止时间。
#pragma once
#ifndef CHAT_GRPC_CLIENT_H
#define CHAT_GRPC_CLIENT_H
#include "const.h"
#include <grpcpp/grpcpp.h>
#include "message.grpc.pb.h"
#include "message.pb.h"
#include "singleton.h"
#include "ConfigMgr.h"
#include "data.h"
#include <queue>
#include <json-forwards.h>
#include <json.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <string>
#include <chrono>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

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


// 连接池
class ChatConPool{
public:
    ChatConPool(size_t poolSize, std::string host, std::string port):
        _b_stop(false),_poolSize(poolSize),_host(host),_port(port){
        for(size_t i = 0; i < _poolSize; i++){
            std::shared_ptr<Channel> channel = grpc::CreateChannel(_host + ":" + _port, grpc::InsecureChannelCredentials());
            _pool.push(std::make_unique<ChatService::Stub>(channel));
        }
    }

    ~ChatConPool(){
        std::lock_guard<std::mutex> lock(_mutex); 
        Close();
        while(!_pool.empty()){
            _pool.pop();
        }
    }

    void Close(){
        _b_stop = true;
        _cond.notify_all();
    }

    std::unique_ptr<ChatService::Stub> getConnnection(){
        std::unique_lock<std::mutex> lock(_mutex); 
        _cond.wait(lock, [this](){
            return _b_stop || !_pool.empty();
        });
        if(_b_stop){
            return nullptr;
        }
        auto context = std::move(_pool.front());
        _pool.pop();
        return context;
    }

    // deadline为本次调用的绝对截止时间；超时/关闭返回空，不延长后续RPC剩余期限。
    std::unique_ptr<ChatService::Stub> getConnectionUntil(
        const std::chrono::system_clock::time_point& deadline){
        std::unique_lock<std::mutex> lock(_mutex);
        if(!_cond.wait_until(lock, deadline, [this](){
            return _b_stop || !_pool.empty();
        })){
            return nullptr;
        }
        if(_b_stop){
            return nullptr;
        }
        auto context = std::move(_pool.front());
        _pool.pop();
        return context;
    }

    void returnConnection(std::unique_ptr<ChatService::Stub> conn){
        std::lock_guard<std::mutex> lock(_mutex);
        if(_b_stop){
            return;
        }
        _pool.push(std::move(conn));
        _cond.notify_one();
    }

private:
    std::atomic<bool> _b_stop; // 停止标志
    size_t _poolSize; // 连接池大小
    std::string _host; // 对端地址
    std::string _port; // 对端端口
    std::queue<std::unique_ptr<ChatService::Stub>> _pool; // 连接池
    std::mutex _mutex; // 互斥锁
    std::condition_variable _cond; // 条件变量
}; 


class ChatGrpcClient : public Singleton<ChatGrpcClient>{
    friend class Singleton<ChatGrpcClient>;
public:
    ~ChatGrpcClient();

    AddFriendRsp NotifyAddFriend(std::string server_ip, const AddFriendReq& request);
    AuthFriendRsp NotifyAuthFriend(std::string server_ip, const AuthFriendReq& request);
    TextChatMsgRsp NotifyTextChatMsg(std::string server_ip, const TextChatMsgReq& request);
    ImageChatMsgRsp NotifyImageChatMsg(std::string server_ip, const ImageChatMsgReq& request);
    // 向目标 ChatServer 转发已持久化文件消息，deadline 防止路由线程无限等待。
    FileChatMsgRsp NotifyFileChatMsg(std::string server_ip, const FileChatMsgReq& request);
    MessageDisplayedRsp NotifyMessageDisplayed(std::string server_ip, const MessageDisplayedReq& request);
    ThreadReadRsp NotifyThreadRead(std::string server_ip, const ThreadReadReq& request);
    KickUserRsp NotifyKickUser(std::string server_ip, const KickUserReq& request);
    // 转发已提交群通知；server 为路由名，request 明确群/发送者/目标成员；
    // 连接池获取与 RPC 共用三秒截止时间，不参与消息持久化。
    message::GroupNotifyRsp NotifyGroup(const std::string& server, const message::GroupNotifyReq& request);
    // 专用视频信令RPC；server为Redis路由名，池获取与RPC共用三秒deadline。
    message::VideoTestNotifyRsp NotifyVideoTest(const std::string& server,
                                               const message::VideoTestNotifyReq& request);
    // 语音专用跨服投递：server为路由名，request绑定目标session；返回TrySend结果或rpc_failed。
    message::VoiceCallNotifyRsp NotifyVoiceCall(const std::string& server,
                                               const message::VoiceCallNotifyReq& request);

private:
    ChatGrpcClient();
    std::unordered_map<std::string, std::unique_ptr<ChatConPool>> _pools; // 连接池，key为server_name

};



#endif // CHAT_GRPC_CLIENT_H
