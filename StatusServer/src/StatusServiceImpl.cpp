#include "StatusServiceImpl.h"
#include "const.h"
#include "ConfigMgr.h"
#include <boost/uuid/uuid.hpp>
#include <random>
#include <chrono>
#include <iomanip>

std::string generate_unique_string(){
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;
    
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    
    uint64_t random = dis(gen);
    uint64_t unique = timestamp ^ random;  // 异或混合
    
    std::stringstream ss;
    ss << std::hex << unique;
    return ss.str();
}

StatusServiceImpl::StatusServiceImpl()
{
    auto& config = ConfigMgr::Inst();
    
    ChatServer server;
    server.host = config["ChatServer1"]["host"];
    server.port = config["ChatServer1"]["port"];
    server.name = config["ChatServer1"]["name"];
    server.con_count = 0;
    _servers[server.name] = server;

    server.host = config["ChatServer2"]["host"];
    server.port = config["ChatServer2"]["port"];
    server.name = config["ChatServer2"]["name"];
    server.con_count = 0;
    _servers[server.name] = server;
    
}

Status StatusServiceImpl::GetChatServer(ServerContext *context, const GetChatServerReq *request, GetChatServerRsp *reply)
{
    std::cout << "StatusServer has been called to get chat server" << std::endl;
    ChatServer server = getChatServer();
    reply->set_host(server.host);
    reply->set_port(server.port);
    reply->set_token(generate_unique_string());
    reply->set_error(ErrorCode::Success); //reply设置好后，grpc会自动返回
    {
        std::lock_guard<std::mutex> lock(_token_mtx);
        _tokens[request->uid()] = reply->token();
    }
    {
        std::lock_guard<std::mutex> lock(_server_mtx);
        _token_to_server[reply->token()] = server.name;
        auto last = _server_tokens.find(server.name);
        if(last == _server_tokens.end()){
            //第一次登录插入即可
            _server_tokens[server.name] = reply->token();
        }else{
            _server_tokens[server.name] = reply->token();
            //清除旧的token
            _token_to_server.erase(last->second);
        }
    }
    return Status::OK;
}

Status StatusServiceImpl::Login(ServerContext *context, const LoginReq *request, LoginRsp *reply)
{
    auto uid = request->uid();
    auto token = request->token();
    std::lock_guard<std::mutex> lock(_token_mtx);
    auto iter = _tokens.find(uid);
    if(iter == _tokens.end()){
        reply->set_error(ErrorCode::UidInvalid);
        return Status::OK;
    }
    if(iter->second != token){
        reply->set_error(ErrorCode::TokenInvalid);
        return Status::OK;
    }
    reply->set_error(ErrorCode::Success);
    reply->set_token(token);
    reply->set_uid(uid);
    return Status::OK;
}


ChatServer& StatusServiceImpl::getChatServer()
{
    std::lock_guard<std::mutex> lock(_server_mtx);
    
    // 找到最小负载的服务器
    auto minIt = _servers.begin();
    for (auto it = _servers.begin(); it != _servers.end(); ++it) {
        if (it->second.con_count < minIt->second.con_count) {
            minIt = it;
        }
    }
    
    minIt->second.con_count++;
    return minIt->second;
}
