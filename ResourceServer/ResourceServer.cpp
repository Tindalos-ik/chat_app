#include "LogicSystem.h"
#include "AsioIOServicePool.h"
#include "CServer.h"
#include "ConfigMgr.h"
#include "ResourceServiceImpl.h"
#include <csignal>
#include <thread>
#include <mutex>
#include <condition_variable>

bool bstop = false;

std::condition_variable cond_quit; // 退出条件变量
std::mutex mutex_quit; // 退出互斥锁


int main(){
    try{
        auto& config = ConfigMgr::Inst();
        auto server_name = config["ResourceServer"]["name"];

        auto pool = AsioIOServicePool::GetInstance();

        boost::asio::io_context io_context; //主线程用于接收新的连接

        boost::asio::signal_set signals(io_context, SIGINT, SIGTERM); //定义信号集跑在主线程上，捕捉退出信号，实现优雅退出
        
        auto port_str = config["ResourceServer"]["port"];
        CServer server(io_context, std::stoi(port_str));

        // 资源上传/下载仍由 TCP 9090 处理；gRPC 仅供可信的 ChatServer 批量核验
        // 已发布图片，绝不暴露 ResourceServer 本机文件路径给聊天服务。
        const std::string rpc_port = config["ResourceServer"]["rpcport"].empty()
            ? "50057" : config["ResourceServer"]["rpcport"];
        const std::string rpc_host = config["ResourceServer"]["host"].empty()
            ? "127.0.0.1" : config["ResourceServer"]["host"];
        const std::string rpc_address = rpc_host + ":" + rpc_port;
        ResourceServiceImpl resource_service;
        grpc::ServerBuilder grpc_builder;
        grpc_builder.AddListeningPort(rpc_address, grpc::InsecureServerCredentials());
        grpc_builder.RegisterService(&resource_service);
        std::unique_ptr<grpc::Server> grpc_server(grpc_builder.BuildAndStart());
        if (!grpc_server) {
            std::cerr << "resource gRPC server start failed on " << rpc_address << std::endl;
            pool->Stop();
            return 1;
        }
        std::cout << "resource gRPC server listening on " << rpc_address << std::endl;
        std::thread grpc_thread([&grpc_server]() { grpc_server->Wait(); });

        signals.async_wait([&io_context, pool, &grpc_server](auto, auto) {
            io_context.stop();
            pool->Stop();
            grpc_server->Shutdown();
        });

        io_context.run(); //主线程开始运行，等待退出信号
        grpc_thread.join();


    }catch (std::exception& e){
        std::cout << "Exception : " << e.what() << std::endl;
    }
}
