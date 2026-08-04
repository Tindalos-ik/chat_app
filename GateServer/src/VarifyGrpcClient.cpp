#include "VarifyGrpcClient.h"
#include "ConfigMgr.h"

RPConPool::RPConPool(std::size_t poolsize, std::string host, std::string port):
 _poolsize(poolsize), _host(host), _port(port),_b_stop(false)
{
    //初始化连接池，先创建连接通道，再将stub放入连接池
    for(size_t i = 0; i < poolsize; ++i){
        std::shared_ptr<Channel> channel = grpc::CreateChannel(host + ":" + port, grpc::InsecureChannelCredentials());
        _connections.push(VarifyService::NewStub(channel));
        //push unique_ptr 走的是移动语义，不会调用拷贝构造函数
    }
    std::cout << "RPConPool started with " << poolsize << "connections" << std::endl;
}

RPConPool::~RPConPool()
{
    std::lock_guard<std::mutex> lock(_mutex); //加锁，防止其他线程操作，也是等待其他线程操作完成再去关闭连接池
    Close();
    while(!_connections.empty()){
        _connections.pop();
    }
}

void RPConPool::Close()
{
    _b_stop = true; //通知所有线程停止
    _cond.notify_all(); //唤醒所有等待的线程    
}

std::unique_ptr<VarifyService::Stub> RPConPool::GetConnection()
{
    //为什么这里用unique_lock而不是lock_guard？
    //因为lock_guard是加锁后一直持有锁，而unique_lock是加锁后可以解锁，这里需要等待连接池中有连接可以使用或者连接池已经关闭，所以需要解锁
    std::unique_lock<std::mutex> lock(_mutex);
    //lambda判断为false时解锁，也就是等待连接池中有连接可以使用或者连接池已经关闭
    _cond.wait(lock, [this](){return _b_stop || !_connections.empty();}); 
    if(_b_stop){
        //连接池已经关闭
        return nullptr;
    }
    std::unique_ptr<VarifyService::Stub> con = std::move(_connections.front());
    _connections.pop();
    return con;
}

void RPConPool::ReleaseConnection(std::unique_ptr<VarifyService::Stub> connection)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if(_b_stop) return; //连接池已经关闭，直接返回
    _connections.push(std::move(connection));
    _cond.notify_one(); //唤醒一个等待的线程
}

GetVarifyRsp VarifyGrpcClient::GetVarifyCode(std::string email)
{
    ClientContext context; //创建上下文
    GetVarifyReq request; //请求
    GetVarifyRsp reply; //响应对象

    //设置请求参数
    request.set_email(email);
        
    auto stub = _pool->GetConnection();

    //调用服务，参数不知道跳到源码去看，通过stub调用远程服务
    Status status = stub->GetVarifyCode(&context, request, &reply);

    if (status.ok()) {
        //回收连接
        _pool->ReleaseConnection(std::move(stub));
        return reply;
    } 
    else {
        _pool->ReleaseConnection(std::move(stub));
        reply.set_error(ErrorCode::RPCFaild); //在const.h中定义的错误码
        return reply;
    }
}

VarifyGrpcClient::VarifyGrpcClient()
{
    auto& gConfigMgr = ConfigMgr::Inst();
    std::string host = gConfigMgr["VarifyServer"]["host"];
    std::string port = gConfigMgr["VarifyServer"]["port"];
    _pool.reset(new RPConPool(5, host, port));
}
