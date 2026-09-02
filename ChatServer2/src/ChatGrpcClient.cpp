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
    return AddFriendRsp();
}
    
AuthFriendRsp ChatGrpcClient::NotifyAuthFriend(std::string server_ip, const AuthFriendReq& request){
    return AuthFriendRsp();
}

TextChatMsgRsp ChatGrpcClient::NotifyTextChatMsg(std::string server_ip, const TextChatMsgReq& request, const Json::Value& rtvalue){
    return TextChatMsgRsp();
}
