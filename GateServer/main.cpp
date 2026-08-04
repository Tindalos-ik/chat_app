#include "cserver.h"
#include "const.h"
#include "ConfigMgr.h"


/*使用线程池提高GateServer的性能，有两种方案
一种是一个io_context+多个线程
另一种是多个io_context+轮询处理，让多个io_context跑在不同的线程中
*/

int main() {
    try {
        auto& gConfigMgr = ConfigMgr::Inst();
        std::string gate_port_str = gConfigMgr["GateServer"]["port"];
        unsigned short gate_port = static_cast<unsigned short>(std::stoi(gate_port_str));
        net::io_context ioc{1};
        boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&ioc](boost::system::error_code const& error, int signal_number) {
            if (!error) {
                ioc.stop();
            }
        }); //实现优雅关闭机制
        
        auto server = std::make_shared<CServer>(ioc, gate_port);
        server->Start();
        std::cout << "Server is running on port " << gate_port << std::endl;
        
        ioc.run();
        
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    
    return 0;
}

/*流程
1. 创建一个io_context对象，用于处理IO事件
2. 创建一个信号集对象，关联到io_context，监听SIGINT和SIGTERM信号，实现优雅关闭机制
3. 创建一个CServer对象，并调用Start()方法启动服务器
4. 调用io_context对象的run()方法，一致运行直到stop()被调用

在服务器启动后，会异步监听端口，等待客户端连接。
当有客户端连接时，会创建一个HttpConnection对象，将连接形成的socket使用std::move传进去，用于处理客户端的请求和响应，然后server继续监听端口，等待下一个客户端连接。
在httpConnection对象中，会异步读取请求，调用HandleReq处理请求
在HandleReq中，会解析请求，根据请求类型调用不同的处理函数，如GET、POST等，处理完请求后，会异步写响应，然后关闭连接，

*/