#pragma once
#ifndef CSERVER_H
#define CSERVER_H

#include <boost/asio.hpp>
#include "CSession.h"
#include <memory>
#include <atomic>
#include <map>
#include <mutex>

/*
 * CServer 服务器类
 * 职责：
 *  1. 绑定端口并异步监听（acceptor 跑在主 io_context 上）
 *  2. 每来一个新连接，从 IO 线程池轮询取出一个 io_context 创建 CSession
 *  3. 用 map 管理所有存活会话，连接断开时负责清理
 */
class CServer : public std::enable_shared_from_this<CServer> {
public:
    CServer(boost::asio::io_context& io_context, short port);
    ~CServer();

    // 必须在 make_shared<CServer> 后调用：异步回调通过 weak_ptr 确认服务器仍然存活。
    void Start();
    // 退出时取消监听和定时器，并关闭当前会话；可重复调用。
    void Stop();
    void ClearSession(std::string session_id); // 会话断开/异常时从 map 中移除并销毁

    void on_timer(const boost::system::error_code& error); // 定时器回调，通过统一断线入口清理超时连接

private:
    void StartAccept();   // 发起一次异步接受连接
    // 注册下一次心跳扫描；回调只观察 CServer，执行时 lock 成 shared_ptr 才能访问成员。
    void StartHeartbeatTimer();
    void HandleAccept(std::shared_ptr<CSession> new_session,
                      const boost::system::error_code& error); // 接受连接完成后的回调

    boost::asio::io_context& _io_context;           // 主IO上下文（acceptor 跑在这里）
    short _port;                                    // 监听端口
    boost::asio::ip::tcp::acceptor _acceptor;       // 异步监听器

    std::map<std::string, std::shared_ptr<CSession>> _sessions; // session_id -> 会话
    std::mutex _mutex; // 保护 _sessions 的线程安全（接受回调在主线程，清理在IO线程）

    boost::asio::steady_timer _timer; // 定时器，用于检测空闲连接，让它跑在主 io_context 上
    std::atomic_bool _started{false};  // 防止重复注册 accept/timer
    std::atomic_bool _stopping{false}; // Stop 后禁止回调继续接收或重新注册定时器
};

#endif // CSERVER_H
