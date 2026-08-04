#pragma once
#ifndef VARIFY_GRPC_CLIENT_H
#define VARIFY_GRPC_CLIENT_H


#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include "message.grpc.pb.h" 
#include "message.pb.h"
#include "singleton.h"
#include "const.h" 
//使用stub连接池优化性能，因为前面我们用了线程池，这里自然也要优化，实现线程安全


using grpc::Channel;        // gRPC 通信通道
using grpc::ClientContext;  // 客户端上下文
using grpc::Status;         // RPC 调用状态（成功/失败）

using message::GetVarifyReq;   // 请求消息类型
using message::GetVarifyRsp;   // 响应消息类型
using message::VarifyService;   // 服务类型

class RPConPool{
public:
    //host是客户端地址，不是远程服务地址
    RPConPool(std::size_t poolsize, std::string host, std::string port);
    ~RPConPool();

    void Close();

    std::unique_ptr<VarifyService::Stub> GetConnection(); //获取连接

    void ReleaseConnection(std::unique_ptr<VarifyService::Stub> connection); //释放连接
private:
    std::atomic<bool> _b_stop; //标记是否要回收连接池
    std::size_t _poolsize;
    std::string _host;
    std::string _port;

    //用队列实现连接池，队列本身不是线程安全的，所以需要加锁，保证线程安全
    //存unique_ptr是因为NewStub返回的就是unique_ptr
    std::queue<std::unique_ptr<VarifyService::Stub>> _connections;

    std::mutex _mutex; 
    std::condition_variable _cond;
};

//也是单例模式
class VarifyGrpcClient : public Singleton<VarifyGrpcClient>
{
    friend class Singleton<VarifyGrpcClient>;

public:
    //获取验证码，参数邮箱，返回验证码
    GetVarifyRsp GetVarifyCode(std::string email);

private:
    VarifyGrpcClient();

    //stub可以理解为“远程服务的本地代理”，一个媒介，通过它来调用远程服务
    //std::unique_ptr<VarifyService::Stub> stub_; 

    std::unique_ptr<RPConPool> _pool;
};

#endif // VARIFY_GRPC_CLIENT_H