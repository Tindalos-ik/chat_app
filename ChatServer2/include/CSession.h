#pragma once
#ifndef CSESSION_H
#define CSESSION_H

#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <mutex>
#include <string>
#include <ctime>
#include "const.h"
#include "MsgNode.h"

class CServer; // 前置声明，避免 CServer.h <-> CSession.h 循环包含

using boost::asio::ip::tcp;

/*
 * CSession 会话类
 * 负责一条 TCP 连接上所有数据的收发。
 * 自定义协议：包头(4字节) = 消息id(2字节) + 消息体长度(2字节)，均为网络字节序。
 *   - 收包：先读满包头，解析出id和长度，再读满包体，投递给逻辑层处理
 *   - 发包：消息放入发送队列，通过 async_write 逐条发送，保证不交叉不乱序
 */
class CSession : public std::enable_shared_from_this<CSession> {
public:
    CSession(boost::asio::io_context& io_context, CServer* server);
    ~CSession();

    tcp::socket& GetSocket();        // 供 acceptor 把新连接绑定到本会话
    std::string& GetSessionId();     // 获取会话唯一id（uuid）
    void SetUserId(int uid);         // 登录成功后记录用户id
    int GetUserId();                 // 获取用户id

    void Start();                    // 连接建立后开始收包（先读包头）
    void Send(char* msg, short max_length, short msgid); // 发送char*消息
    void Send(std::string msg, short msgid);             // 发送std::string消息
    void Close();                    // 关闭连接
    // 幂等断线入口：关闭 socket，注销本机会话，并按 session_id 条件清理 Redis 在线路由
    void HandleDisconnect();
    std::shared_ptr<CSession> SharedSelf(); // 返回自身的shared_ptr，防止异步回调期间对象被提前析构

    void NotifyOffline(); // 通知用户下线

    bool isHeartbeatExpired() const;         // 使用单调时钟判断是否心跳超时
    void UpdateHeartbeat();                   // 收到完整 TCP 包后刷新活跃时间

private:
    void ReadHead(int head_len);     // 读包头
    void ReadBody(int body_len);     // 读包体
    void asyncReadFull(std::size_t maxLength,
        std::function<void(const boost::system::error_code&, std::size_t)> handler); // 从0开始读满maxLength字节
    void asyncReadLen(std::size_t read_len, std::size_t total_len,
        std::function<void(const boost::system::error_code&, std::size_t)> handler); // 循环读，直到读满total_len字节
    void HandleWrite(const boost::system::error_code& error, std::shared_ptr<CSession> shared_self); // 写完成回调

    tcp::socket _socket;              // 会话对应的socket
    std::string _session_id;          // 会话唯一标识（uuid字符串）
    CServer* _server;                 // 所属服务器指针（用于清除会话等）
    bool _b_close;                    // 连接是否已关闭
    char _data[MAX_LENGTH];           // 收发共用的缓冲区（直接用数组，避免手动new/delete导致泄漏）

    std::queue<std::shared_ptr<SendNode>> _send_que; // 待发送消息队列
    std::mutex _send_lock;                            // 保护发送队列
    bool _close_after_send;                           // 踢人通知写完后关闭连接，并拒绝继续排队
    std::mutex _session_mtx;                          // 保护关闭操作
    // Close 会同时唤醒读写回调；用原子标记避免这些回调重复清理、重复修改在线数
    std::atomic_bool _disconnect_handled;

    std::shared_ptr<RecvNode> _recv_msg_node; // 正在接收的消息体节点
    std::shared_ptr<MsgNode> _recv_head_node; // 收到的包头节点（用来解析id和长度）
    int _user_uid;                            // 登录后的用户id，未登录为0

    // 定时器线程读取、会话线程更新，必须使用原子变量避免数据竞争。
    std::atomic<std::int64_t> _last_heartbeat{0}; // steady_clock 毫秒值，不受系统时间调整影响
};

/*
 * LogicNode 逻辑节点
 * 会话层解析完一条完整消息后，把它封装成 LogicNode 投递到逻辑层队列，
 * 逻辑层根据 _recvnode 中的消息id分发到对应的处理函数。
 */
class LogicNode {
    friend class LogicSystem;
public:
    LogicNode(std::shared_ptr<CSession>, std::shared_ptr<RecvNode>);
private:
    std::shared_ptr<CSession> _session;  // 消息来自哪个会话
    std::shared_ptr<RecvNode> _recvnode; // 消息内容（id + 数据）
};

#endif // CSESSION_H
