#pragma once
#ifndef STATUS_SERVICE_IMPL_H
#define STATUS_SERVICE_IMPL_H

#include <grpcpp/grpcpp.h>
#include "message.grpc.pb.h"
#include "message.pb.h"
#include <iostream>
#include <unordered_map>
#include <mutex>
#include <vector>

using grpc::Server;              // gRPC 服务器类
using grpc::ServerBuilder;       // gRPC 服务器构建器
using grpc::ServerContext;       // 服务器上下文
using grpc::Status;              // RPC 调用状态
using message::GetChatServerReq; // 获取聊天服务器请求消息
using message::GetChatServerRsp; // 获取聊天服务器响应消息
using message::LoginReq;         // 登录请求消息
using message::LoginRsp;         // 登录响应消息
using message::StatusService;    // 状态服务接口（由 .proto 生成）

struct ChatServer{
    std::string host;       
    std::string port;       
    std::string name;       
    int con_count;          // 当前已连接的客户端数量（用于负载均衡）
};

/**
 * @class StatusServiceImpl
 * @brief 状态服务实现类
 * 
 * 实现 gRPC 状态服务，提供：
 * - 登录验证和 token 管理
 * - 为用户分配最佳的聊天服务器（负载均衡）
 * 
 * final 关键字：修饰类表示此类不能被其他类继承
 */
class StatusServiceImpl final : public StatusService::Service {
public:
    StatusServiceImpl();

    /**
     * @brief 获取一个可用的聊天服务器，获取服务地址，生成token并且存储，token用于后续的身份认证
     * 
     * @param context 服务器上下文
     * @param request 请求参数
     * @param reply 响应参数
     * @return Status 执行状态（OK 表示成功）
     */
    Status GetChatServer(ServerContext* context, const GetChatServerReq* request, GetChatServerRsp* reply) override;

    /**
     * @brief 用户登录
     */
    Status Login(ServerContext* context, const LoginReq* request, LoginRsp* reply) override;

private:

    /**
     * @brief 获取一个最优的聊天服务器
     * 
     * 遍历所有可用的聊天服务器，
     * 返回当前连接数最少的那一台（负载最低）。
     * 
     * @return ChatServer 选中的聊天服务器信息
     */
    ChatServer& getChatServer();

    
    std::unordered_map<int, std::string> _tokens;           // 用户ID -> token 映射表（用户会话管理）
    std::unordered_map<std::string,ChatServer> _servers;    // 这里得用哈希表存服务器列表，服务器名字->服务器信息，因为服务器是结构体，没有哈希函数
    std::unordered_map<std::string, std::string> _token_to_server; // token->服务器名称的映射表
    std::unordered_map<std::string,std::string> _server_tokens;     // 服务器->token的映射表
    
    std::mutex _server_mtx; 
    std::mutex _token_mtx;   

    
};

#endif // STATUS_SERVICE_IMPL_H