#pragma once
#ifndef STATUS_GRPC_CLIENT_H
#define STATUS_GRPC_CLIENT_H

#include "singleton.h"
#include "message.grpc.pb.h"
#include "message.pb.h"
#include <grpcpp/grpcpp.h>
#include "const.h"
#include "ConfigMgr.h"
#include <queue>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <atomic>
#include <iostream>

using grpc::Channel;
using grpc::Status;
using grpc::ClientContext;

using message::GetChatServerReq;
using message::GetChatServerRsp;
using message::StatusService;
using message::LoginReq;
using message::LoginRsp;


class StatusConPool{
public:
    StatusConPool(size_t poolsize,std::string host,std::string port);

    void Close();

    std::unique_ptr<StatusService::Stub> getConnection();

    void returnConnection(std::unique_ptr<StatusService::Stub> connection);

    ~StatusConPool();

private:
    size_t _poolsize;
    std::string _host;
    std::string _port;

    std::queue<std::unique_ptr<StatusService::Stub>> _connections;

    std::mutex _mutex;  
    std::condition_variable _cv;
    std::atomic<bool> _b_stop;

};

class StatusGrpcClient : public Singleton<StatusGrpcClient> {
    friend Singleton<StatusGrpcClient>;
public:
    ~StatusGrpcClient();

    //获取聊天服务器和ip和端口
    GetChatServerRsp GetChatServer(int uid);

    //登录
    LoginRsp Login(int uid, std::string token);


private:
    StatusGrpcClient();

    std::unique_ptr<StatusConPool> _pool;
};





#endif // STATUS_GRPC_CLIENT_H