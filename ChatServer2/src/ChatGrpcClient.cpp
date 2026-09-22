#include "ChatGrpcClient.h"
#include "RedisMgr.h"
#include "ConfigMgr.h"
#include "UserMgr.h"
#include "CSession.h"
#include "MysqlMgr.h"
#include <sstream>
#include <vector>
#include <iostream>

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
        _pools[cfg[word]["name"]] = std::make_unique<ChatConPool>(5, cfg[word]["host"], cfg[word]["rpcport"]); // 连接rpcport而不是tcpport！！！
    }
}



ChatGrpcClient::~ChatGrpcClient()
{

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
        std::cout << "NotifyKickUser RPC failed, code = " << status.error_code()
                  << ", message = " << status.error_message() << std::endl;
        rsp.set_error(ErrorCode::RPCFaild);
        return rsp;
    }
    
    return rsp;
}
    
AuthFriendRsp ChatGrpcClient::NotifyAuthFriend(std::string server_ip, const AuthFriendReq& request){
    AuthFriendRsp rsp;
    // RPC 传输失败和目标服务的业务成功必须区分。先初始化为 RPCFaild，只有远端
    // 明确写回 response 时才使用其 error，避免连接池不存在时被误报为成功。
    rsp.set_error(ErrorCode::RPCFaild);
    rsp.set_uid(request.fromuid());
    rsp.set_touid(request.touid());

    auto find_iter = _pools.find(server_ip); // 找到对应的连接池
    if(find_iter == _pools.end()){
        return rsp;
    }

    auto& pool = find_iter->second;

    ClientContext context;
    // 获取连接池中的一个连接
    auto stub = pool->getConnnection();
    if (!stub) {
        return rsp;
    }
    Status status = stub->NotifyAuthFriend(&context, request, &rsp); // 调用远程方法
    Defer defercon([&pool, &stub, this](){
        pool->returnConnection(std::move(stub));
    });

    if(!status.ok()){
        rsp.set_error(ErrorCode::RPCFaild);
        return rsp;
    }
    return rsp;
}

TextChatMsgRsp ChatGrpcClient::NotifyTextChatMsg(std::string server_ip, const TextChatMsgReq& request){
    TextChatMsgRsp rsp;
    rsp.set_error(ErrorCode::RPCFaild);

    auto find_iter = _pools.find(server_ip); // 找到对应的连接池
    if(find_iter == _pools.end()){
        return rsp;
    }

    auto& pool = find_iter->second;

    ClientContext context;
    // 获取连接池中的一个连接
    auto stub = pool->getConnnection();
    if (!stub) {
        return rsp;
    }
    Status status = stub->NotifyTextChatMsg(&context, request, &rsp); // 调用远程方法
    Defer defercon([&pool, &stub, this](){
        pool->returnConnection(std::move(stub));
    });

    if(!status.ok()){
        rsp.set_error(ErrorCode::RPCFaild);
        return rsp;
    }

    return rsp;
}

ImageChatMsgRsp ChatGrpcClient::NotifyImageChatMsg(std::string server_ip, const ImageChatMsgReq& request){
    ImageChatMsgRsp rsp;
    rsp.set_error(ErrorCode::RPCFaild);
    rsp.set_fromuid(request.fromuid());
    rsp.set_touid(request.touid());
    auto find_iter = _pools.find(server_ip);
    if(find_iter == _pools.end()){
        // Redis 中的服务名必须和 PeerServer 配置项的 name 完全一致，否则没有可用连接池。
        std::cout << "NotifyImageChatMsg pool not found, target_server=" << server_ip
                  << ", from=" << request.fromuid() << ", to=" << request.touid() << std::endl;
        return rsp;
    }
    auto& pool = find_iter->second;
    ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    auto stub = pool->getConnectionUntil(std::chrono::system_clock::now() + std::chrono::seconds(3));
    if (!stub) {
        // 连接池耗尽或停止时不调用 RPC；这里能与网络超时日志明确区分。
        std::cout << "NotifyImageChatMsg cannot obtain RPC connection, target_server=" << server_ip
                  << ", from=" << request.fromuid() << ", to=" << request.touid() << std::endl;
        return rsp;
    }
    Defer defercon([&pool, &stub](){ pool->returnConnection(std::move(stub)); });
    const Status status = stub->NotifyImageChatMsg(&context, request, &rsp);
    if(!status.ok()){
        // UNIMPLEMENTED 通常表示目标 ChatServer 未更新 proto/二进制；UNAVAILABLE 或
        // DEADLINE_EXCEEDED 则表示 rpcport、进程状态或网络不可达。
        std::cout << "NotifyImageChatMsg RPC failed, target_server=" << server_ip
                  << ", code=" << status.error_code()
                  << ", message=" << status.error_message() << std::endl;
        rsp.set_error(ErrorCode::RPCFaild);
    } else if (rsp.error() != ErrorCode::Success) {
        // RPC 成功但业务失败只可能是目标服务找不到接收者本地会话等可恢复情况。
        std::cout << "NotifyImageChatMsg remote business rejected, target_server=" << server_ip
                  << ", from=" << request.fromuid() << ", to=" << request.touid()
                  << ", error=" << rsp.error() << std::endl;
    } else {
        std::cout << "NotifyImageChatMsg RPC succeeded, target_server=" << server_ip
                  << ", from=" << request.fromuid() << ", to=" << request.touid() << std::endl;
    }
    return rsp;
}

KickUserRsp ChatGrpcClient::NotifyKickUser(std::string server_ip, const KickUserReq& request){
    KickUserRsp rsp;
    rsp.set_error(ErrorCode::RPCFaild);
    rsp.set_uid(request.uid());

    auto find_iter = _pools.find(server_ip); // 找到对应的连接池
    if(find_iter == _pools.end()){
        return rsp;
    }

    auto& pool = find_iter->second;

    ClientContext context;
    const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(3);
    context.set_deadline(deadline);
    // 获取连接池中的一个连接
    auto stub = pool->getConnectionUntil(deadline);
    if(!stub){
        return rsp;
    }
    Defer defercon([&pool, &stub, this](){
        pool->returnConnection(std::move(stub));
    });

    Status status = stub->NotifyKickUser(&context, request, &rsp); // 调用远程方法

    if(!status.ok()){
        rsp.set_error(ErrorCode::RPCFaild);
        return rsp;
    }

    return rsp;
}
