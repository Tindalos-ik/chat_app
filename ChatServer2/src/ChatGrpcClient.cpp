#include "ChatGrpcClient.h"
#include "RedisMgr.h"
#include "ConfigMgr.h"
#include "UserMgr.h"
#include "CSession.h"
#include "MysqlMgr.h"
#include <sstream>
#include <vector>

ChatGrpcClient::ChatGrpcClient()
{
    auto& cfg = ConfigMgr::Inst();
    auto server_list = cfg["PeerServer"]["servers"];

    // 将字符串按逗号分隔
    std::vector<std::string> words;
    std::stringstream ss(server_list);
    std::string word;

    while(std::getline(ss, word, ',')){
        words.push_back(word);
    }

    // 初始化连接池
    for(auto& word : words){
        if(cfg[word]["name"].empty()){
            continue;
        }
        _pools[cfg[word]["name"]] = std::make_unique<ChatConPool>(5, cfg[word]["host"], cfg[word]["port"]);
    }
}



ChatGrpcClient::~ChatGrpcClient()
{

}

bool ChatGrpcClient::GetBaseInfo(std::string base_key, int uid, std::shared_ptr<UserInfo>& userinfo)
{
    return false;
}

AddFriendRsp ChatGrpcClient::NotifyAddFriend(std::string server_ip, const AddFriendReq& request){
    AddFriendRsp rsp;
    Defer defer([&rsp, &request](){
        rsp.set_error(ErrorCode::Success);
        rsp.set_applyuid(request.applyuid());
        rsp.set_touid(request.touid());
    });

    auto find_iter = _pools.find(server_ip); // 找到对应的连接池
    if(find_iter == _pools.end()){
        return rsp;
    }

    auto& pool = find_iter->second;

    ClientContext context;
    // 获取连接池中的一个连接
    auto stub = pool->getConnnection();
    Status status = stub->NotifyAddFriend(&context, request, &rsp); // 调用远程方法
    Defer defercon([&pool, &stub, this](){
        pool->returnConnection(std::move(stub));
    });

    if(!status.ok()){
        rsp.set_error(ErrorCode::RPCFaild);
        return rsp;
    }
    
    return rsp;
}
    
AuthFriendRsp ChatGrpcClient::NotifyAuthFriend(std::string server_ip, const AuthFriendReq& request){
    return AuthFriendRsp();
}

TextChatMsgRsp ChatGrpcClient::NotifyTextChatMsg(std::string server_ip, const TextChatMsgReq& request, const Json::Value& rtvalue){
    return TextChatMsgRsp();
}
