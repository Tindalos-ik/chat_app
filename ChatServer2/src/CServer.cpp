#include "CServer.h"
#include "AsioIOServicePool.h"
#include <iostream>
#include "UserMgr.h"
#include "RedisMgr.h"
#include "ConfigMgr.h"
#include <chrono>
#include <ctime>
#include <vector>

using boost::asio::ip::tcp;

CServer::CServer(boost::asio::io_context &io_context, short port)
    : _io_context(io_context), _port(port),
      _acceptor(io_context, tcp::endpoint(tcp::v4(), port)),
      _timer(io_context, std::chrono::seconds(HEARTBEAT_CHECK_INTERVAL)) {
    std::cout << "Server start success, listen on port : " << _port << std::endl;
}

CServer::~CServer() {
    std::cout << "Server destruct, listen on port : " << _port << std::endl;
    Stop();
    // 服务器析构时清空会话容器，会话对象由各自所在的IO线程持有引用
    std::lock_guard<std::mutex> lock(_mutex);
    _sessions.clear();
}

void CServer::Start() {
    if (_started.exchange(true)) {
        return;
    }

    _stopping.store(false);
    StartAccept();
    StartHeartbeatTimer();
}

void CServer::Stop() {
    if (_stopping.exchange(true)) {
        return;
    }

    // cancel 后回调仍可能被 io_context 调度，因此回调必须只捕获 weak_ptr。
    // 不在这里持有 _mutex 做断线清理，避免与异步回调的 ClearSession 形成锁重入。
    boost::system::error_code ec;
    _timer.cancel(ec);
    _acceptor.cancel(ec);
    _acceptor.close(ec);
}

// 发起一次异步接受：
// 先从IO线程池轮询取出一个 io_context 创建会话（该会话后续的所有读写都跑在这个IO线程上），
// 再让 acceptor 在主 io_context 上异步接受新连接，socket 交给会话保管。
void CServer::StartAccept() {
    if (_stopping.load()) {
        return;
    }

    auto &io_context = AsioIOServicePool::GetInstance()->GetIOService();
    // weak_ptr 不增加 CServer 引用计数；会话不会因为保存“所属服务器”而阻止服务器退出。
    std::shared_ptr<CSession> new_session = std::make_shared<CSession>(io_context, weak_from_this());
    std::weak_ptr<CServer> weak_server = weak_from_this();
    _acceptor.async_accept(new_session->GetSocket(),
        [weak_server, new_session](const boost::system::error_code& error) {
            // lock 成功时临时持有服务器，保证本次 HandleAccept 执行期间对象不会析构。
            // lock 失败说明服务器已释放，取消回调无需再做任何访问。
            if (auto server = weak_server.lock()) {
                server->HandleAccept(new_session, error);
            }
        });
}

// 接受连接回调：error 为空表示成功接入一个新连接
void CServer::HandleAccept(std::shared_ptr<CSession> new_session,
                           const boost::system::error_code &error) {
    if (_stopping.load()) {
        return;
    }

    if (!error) {
        // 先登记，再启动异步读取；否则读错误可能先清理、随后又把失效连接插回 map。
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _sessions.insert(std::make_pair(new_session->GetSessionId(), new_session));
        }
        new_session->Start();
    } else {
        std::cout << "session accept failed, error is " << error.message() << std::endl;
    }
    // 仅运行中的服务器继续接受下一个连接；Stop 后不能重新注册回调。
    StartAccept();
}

void CServer::StartHeartbeatTimer() {
    if (_stopping.load()) {
        return;
    }

    _timer.expires_after(std::chrono::seconds(HEARTBEAT_CHECK_INTERVAL));
    std::weak_ptr<CServer> weak_server = weak_from_this();
    _timer.async_wait([weak_server](const boost::system::error_code& error) {
        // 定时器取消后的 operation_aborted 回调也会先经过 lock，避免裸 this 悬空。
        if (auto server = weak_server.lock()) {
            server->on_timer(error);
        }
    });
}

// 会话断开/异常时调用：从 map 中移除会话，并把本服务器在线人数减一
void CServer::ClearSession(std::string session_id) {
    int uid = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto iter = _sessions.find(session_id);
        if (iter != _sessions.end()) {
            uid = iter->second->GetUserId();
            if (uid != 0) {
                // 移除用户与session的关联
                UserMgr::GetInstance()->RmvUserSession(uid, session_id);
            }
            _sessions.erase(iter);
        }
    }

    // 之前登录成功过的会话（uid != 0）断开时，在线人数减一，保证负载均衡的计数是"当前在线"
    if (uid != 0) {
        auto server_name = ConfigMgr::Inst()["SelfChatServer"]["name"];
        RedisMgr::GetInstance()->HIncrBy(LOGIN_COUNT, server_name, -1);
    }
}

void CServer::on_timer(const boost::system::error_code &error)
{
    if (error == boost::asio::error::operation_aborted || _stopping.load()) {
        return;
    }

    // 锁内只筛选并保存 shared_ptr；锁外调用 HandleDisconnect，避免 ClearSession 重入 _mutex。
    std::vector<std::shared_ptr<CSession>> expired_sessions;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto &entry : _sessions) {
            if (entry.second->isHeartbeatExpired()) {
                expired_sessions.push_back(entry.second);
            }
        }
    }

    for (const auto &session : expired_sessions) {
        // 投递到会话自己的 I/O 线程，并重新检查：从扫描到执行之间可能刚收到心跳。
        // 主 accept/timer 线程不在此同步等待 Redis 登录锁。
        boost::asio::post(session->GetSocket().get_executor(), [session] {
            if (session->isHeartbeatExpired()) {
                std::cout << "session heartbeat timeout, session_id = "
                          << session->GetSessionId() << std::endl;
                session->HandleDisconnect();
            }
        });
    }

    StartHeartbeatTimer();
}
