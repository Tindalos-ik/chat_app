#include "LogicSystem.h"
#include "AsioIOServicePool.h"
#include "CServer.h"
#include "ConfigMgr.h"
#include <csignal>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "RedisMgr.h"
#include "ChatServiceImpl.h"

bool bstop = false;

std::condition_variable cond_quit; // 退出条件变量
std::mutex mutex_quit; // 退出互斥锁


int main(){
    try{
        auto& config = ConfigMgr::Inst();
        auto server_name = config["SelfChatServer"]["name"];

        auto pool = AsioIOServicePool::GetInstance();

        // 初始化服务器的时候在redis中将登录数量设置成0
        RedisMgr::GetInstance()->HSet(LOGIN_COUNT, server_name, "0");

        // 定义一个grpcserver
        std::string server_address(config["SelfChatServer"]["host"] + ":" + config["SelfChatServer"]["rpcport"]);
        ChatServiceImpl service;
        grpc::ServerBuilder builder;
        // 监听端口和添加服务
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
        builder.RegisterService(&service);
        // 构建并启动grpc服务器
        std::unique_ptr<grpc::Server> grpc_server(builder.BuildAndStart());
        if (!grpc_server) {
            std::cerr << "rpc server start failed on " << server_address << std::endl;
            return 1;
        }
        std::cout << "rpc server listening on " << server_address << std::endl;

        // 单独启动一个线程处理grpc服务
        std::thread grpc_server_thread([&grpc_server](){
            grpc_server->Wait();
        });

        boost::asio::io_context io_context; //主线程用于接收新的连接

        boost::asio::signal_set signals(io_context, SIGINT, SIGTERM); //定义信号集跑在主线程上，捕捉退出信号，实现优雅退出
        signals.async_wait([&io_context, pool, &grpc_server](auto,auto){
            io_context.stop();
            pool->Stop(); //其实析构也会调用Stop，这里只是保险起见
            grpc_server->Shutdown(); //关闭grpc服务器
        }); //捕捉到退出信号后，停止主线程和IO线程池
        
        auto port_str = config["SelfChatServer"]["port"];
        CServer server(io_context, std::stoi(port_str));

        io_context.run(); //主线程开始运行，等待退出信号

        RedisMgr::GetInstance()->HDel(LOGIN_COUNT, server_name); //删除redis中的登录数量
        grpc_server_thread.join(); //等待grpc线程退出


    }catch (std::exception& e){
        std::cout << "Exception : " << e.what() << std::endl;
    }
}
