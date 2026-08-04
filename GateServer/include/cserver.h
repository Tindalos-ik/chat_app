#pragma once
#ifndef CSERVER_H
#define CSERVER_H

//使用boost库搭建http服务器
//cserver 监听端口，接收客户端请求，将请求转发给httpconnection处理

#include <boost/asio.hpp>
#include <memory>
#include "const.h"

//在start()里面修改，将与客户端通信的socket放入线程池中

namespace net = boost::asio;
using tcp = net::ip::tcp;

// 前向声明 HttpConnection，避免循环依赖
class HttpConnection;

class CServer : public std::enable_shared_from_this<CServer> {
public:
    //上下文用于实现异步回调操作
    // 注意：port 参数改为值传递，因为构造函数中会复制它
    CServer(boost::asio::io_context& ioc, unsigned short port);

    void Start();

private:

    tcp::acceptor _acceptor;
    boost::asio::io_context& _ioc;
};

#endif // CSERVER_H