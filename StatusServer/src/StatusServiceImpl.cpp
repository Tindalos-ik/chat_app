#include "StatusServiceImpl.h"
#include "const.h"
#include "ConfigMgr.h"
#include <boost/uuid/uuid.hpp>
#include <random>
#include <chrono>
#include <iomanip>
#include "RedisMgr.h"

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
    auto server_list = config["ChatServers"]["servers"];

    // 将字符串按逗号分隔
    std::vector<std::string> words;
    std::stringstream ss(server_list);
    std::string word;

    while(std::getline(ss, word, ',')){
        words.push_back(word);
    }

    for(auto& name : words){
        if(config[name]["name"].empty()) continue;
        ChatServer server = {config[name]["host"], config[name]["port"], word, 0};
        _servers[name] = server;
    }

}

Status StatusServiceImpl::GetChatServer(ServerContext *context, const GetChatServerReq *request, GetChatServerRsp *reply)
{
    std::cout << "StatusServer has been called to get chatserver" << std::endl;
    ChatServer server = getChatServer();
    reply->set_host(server.host);
    reply->set_port(server.port);
    reply->set_token(generate_unique_string());
    reply->set_error(ErrorCode::Success); //reply设置好后，grpc会自动返回
    insertToken(request->uid(), reply->token()); // 将token插入redis
    return Status::OK;
}

Status StatusServiceImpl::Login(ServerContext *context, const LoginReq *request, LoginRsp *reply)
{
    auto uid = request->uid();
    auto token = request->token();
    // token 存在 redis 中
    std::string uid_str = std::to_string(uid);
    std::string token_key = USERTOKENFREFIX + uid_str;
    std::string token_value = "";
    bool success = RedisMgr::GetInstance()->Get(token_key, token_value);
    
    if(!success){
        reply->set_error(ErrorCode::UidInvalid);
        return Status::OK;
    }

    if(token_value != token){
        reply->set_error(ErrorCode::TokenInvalid);
        return Status::OK;
    }

    reply->set_error(ErrorCode::Success);
    reply->set_uid(uid);
    reply->set_token(token); // grpc会自动返回reply
    return Status::OK;
   
}


ChatServer StatusServiceImpl::getChatServer()
{
    std::lock_guard<std::mutex> lock(_server_mtx);
    auto minServer = _servers.begin()->second;
    // redis中获取服务器的连接数
    auto count_str = RedisMgr::GetInstance()->HGet(LOGIN_COUNT, minServer.name);
    if(count_str.empty()){
        // 不存在则默认设置为最大
        minServer.con_count = INT_MAX;
    }else{
        minServer.con_count = std::stoi(count_str);
    }

    for(auto& server : _servers){
        if(server.second.name == minServer.name) continue;
        auto count_str = RedisMgr::GetInstance()->HGet(LOGIN_COUNT, server.second.name);
        if(count_str.empty()){
            server.second.con_count = INT_MAX;
        }else{
            server.second.con_count = std::stoi(count_str);
        }

        if(server.second.con_count < minServer.con_count){
            minServer = server.second;
        }
    }
    return minServer;
}


void StatusServiceImpl::insertToken(int uid, std::string token){
    RedisMgr::GetInstance()->Set(USERTOKENFREFIX + std::to_string(uid), token);
}
