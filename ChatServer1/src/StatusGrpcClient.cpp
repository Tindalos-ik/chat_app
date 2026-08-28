#include "StatusGrpcClient.h"

StatusGrpcClient::~StatusGrpcClient()
{

}

GetChatServerRsp StatusGrpcClient::GetChatServer(int uid)
{
    ClientContext context;
    GetChatServerReq request;
    GetChatServerRsp reply;
    request.set_uid(uid);

    auto stub = _pool->getConnection();
    Status status = stub->GetChatServer(&context, request, &reply);
    Defer defer_([&stub,this](){
        _pool->returnConnection(std::move(stub));
    });

    if(status.ok()){
        return reply;
    }else{
        reply.set_error(ErrorCode::RPCFaild);
        return reply;
    }

}

LoginRsp StatusGrpcClient::Login(int uid, std::string token)
{
    ClientContext context;
    LoginReq request;
    LoginRsp reply;
    request.set_uid(uid);
    request.set_token(token);

    auto stub = _pool->getConnection();
    Defer defer_([&stub,this](){
        _pool->returnConnection(std::move(stub));
    });

    Status status = stub->Login(&context, request, &reply);
    if(status.ok()){
        return reply;
    }else{
        reply.set_error(ErrorCode::RPCFaild);
        return reply;
    }
}

StatusGrpcClient::StatusGrpcClient()
{
    ConfigMgr& config = ConfigMgr::Inst();
    std::string host = config["StatusServer"]["host"];
    std::string port = config["StatusServer"]["port"];
    _pool.reset(new StatusConPool(10, host, port));
}

StatusConPool::StatusConPool(size_t poolsize, std::string host, std::string port):
    _poolsize(poolsize), _host(host), _port(port), _b_stop(false)
{
    for(size_t i=0; i < poolsize; i++){
        std::shared_ptr<Channel> channel = grpc::CreateChannel(host + ":" + port, grpc::InsecureChannelCredentials());
        _connections.push(StatusService::NewStub(channel));
    }
}

void StatusConPool::Close()
{
    _b_stop = true;
    _cv.notify_all();
}

std::unique_ptr<StatusService::Stub> StatusConPool::getConnection()
{
    std::unique_lock<std::mutex> lock(_mutex);
    //等到连接池中有连接
    _cv.wait(lock,[this]{
        if(_b_stop){
            return true;
        }
        return !_connections.empty();
    });
    if(_b_stop){
        return nullptr;
    }
    auto con = std::move(_connections.front());
    _connections.pop();
    return con;
}

void StatusConPool::returnConnection(std::unique_ptr<StatusService::Stub> connection)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _connections.push(std::move(connection));
    _cv.notify_one(); //通知等待的线程
}

StatusConPool::~StatusConPool()
{
    std::lock_guard<std::mutex> lock(_mutex);
    Close();
    while(!_connections.empty()){
        _connections.pop();
    }
}


