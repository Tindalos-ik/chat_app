#pragma once 
#ifndef HTTP_CONNECTION_H
#define HTTP_CONNECTION_H

#include "LogicSystem.h"
#include "const.h"
#include "HttpConnection.h"
#include <boost/beast/http.hpp>
#include <boost/beast/core.hpp>
#include <boost/asio.hpp>
#include <memory>
#include <unordered_map>
#include <string>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

//httpconnection负责处理http请求，从链接到关闭连接，包括解析http请求，处理请求，返回响应

class LogicSystem;  // 前向声明，避免循环依赖

class HttpConnection : public std::enable_shared_from_this<HttpConnection> {
public:
    friend class LogicSystem;

    //为什么是移动构造，因为asio实现中socket只允许移动构造，不允许拷贝构造
    //socket本质是文件描述符，拷贝构造会导致文件描述符的重复使用，导致错误
    //HttpConnection(tcp::socket&& socket);

    //改用线程池，那就只需要接收一个io_context，然后自己创建socket
    //io_context也是不能拷贝的，所以这里用引用
    HttpConnection(net::io_context& ioc);
    void Start();

    //返回一个socket的引用，用于asio的异步操作
    tcp::socket& socket() {
        return _socket;
    }

private:
    tcp::socket _socket; //socket对象
    beast::flat_buffer _buffer{ 8192 }; //beast库提供的缓冲区，用于存储http请求和响应
    //dynamic_body代表任意类型的body，可以是字符串，也可以是二进制数据，也可以是HTML
    http::request<http::dynamic_body> _request;  //HTTP请求容器
    http::response<http::dynamic_body> _response;

    //使用成员初始化列表构造超时定时器，第一个参数是socket的执行器，告诉定时器在哪个上下文上工作，第二个参数是超时时间
    net::steady_timer deadline_{
        _socket.get_executor(), std::chrono::seconds(60)
    };

    void CheckDeadline(); //检查超时
    void WriteResponse(); //写响应
    void HandleReq(); //处理请求

    //实现get请求参数的解析
    std::string _get_url;
    std::unordered_map<std::string, std::string> _get_params;
    void PreParseGetParams(); //解析get请求参数
    
};

#endif // !HTTP_CONNECTION_H