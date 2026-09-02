#include "StatusServiceImpl.h"
#include "const.h"
#include "ConfigMgr.h"
#include <boost/uuid/uuid.hpp>
#include <random>
#include <chrono>
#include <iomanip>
#include <limits>
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
    // config.ini 里 [ChatServers] 下的键是 name（name = ChatServer1,ChatServer2）
    auto server_list = config["ChatServers"]["name"];

    // 将字符串按逗号分隔
    std::vector<std::string> words;
    std::stringstream ss(server_list);
    std::string word;

    while(std::getline(ss, word, ',')){
        words.push_back(word);
    }

    for(auto& name : words){
        if(config[name]["name"].empty()) continue;
        ChatServer server = {config[name]["host"], config[name]["port"], config[name]["name"], 0};
        _servers[name] = server;
    }

}

Status StatusServiceImpl::GetChatServer(ServerContext *context, const GetChatServerReq *request, GetChatServerRsp *reply)
{
    std::cout << "StatusServer has been called to get chatserver" << std::endl;
    ChatServer server = getChatServer();
    if (server.host.empty() || server.port.empty()) {
        // 配置里没有可用的 ChatServer，让 GateServer 按"获取聊天服务器失败"处理
        reply->set_error(ErrorCode::RPCFaild);
        return Status::OK;
    }
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
    if (_servers.empty()) {
        return ChatServer{}; // 调用方看到空 host/port 会返回 RPCFaild
    }

    // 负载 = redis 哈希 login_count 中各服务器实时上报的在线数
    // 读不到计数的服务器视为"负载最大"，不优先分配（可能还没启动/没上报过）
    long long min_count = std::numeric_limits<long long>::max();
    std::vector<std::string> candidates;
    for (const auto& item : _servers) {
        const auto& server = item.second;
        auto count_str = RedisMgr::GetInstance()->HGet(LOGIN_COUNT, server.name);
        long long count = std::numeric_limits<long long>::max();
        if (!count_str.empty()) {
            try {
                count = std::stoll(count_str);
            } catch (...) {
                count = std::numeric_limits<long long>::max();
            }
        }

        if (count < min_count) {
            min_count = count;
            candidates.clear();
            candidates.push_back(server.name);
        } else if (count == min_count) {
            candidates.push_back(server.name);
        }
    }

    // 负载相同（例如刚启动大家都是 0）时轮询选择，避免永远偏向 map 里的第一台
    auto name = candidates[_round_index % candidates.size()];
    ++_round_index;
    return _servers[name];
}


void StatusServiceImpl::insertToken(int uid, std::string token){
    RedisMgr::GetInstance()->Set(USERTOKENFREFIX + std::to_string(uid), token);
}
