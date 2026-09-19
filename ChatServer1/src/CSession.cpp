#include "CSession.h"
#include "CServer.h"
#include "LogicSystem.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <random>
#include <cstring>
#include "RedisMgr.h"
#include <json-forwards.h>
#include <json.h>

using namespace std;

// 生成一个32位的十六进制随机字符串作为会话id（效果等同uuid，保证每个连接唯一）
static std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;

    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) {
        ss << std::setw(8) << dis(gen);
        if (i < 3) ss << "-";
    }
    return ss.str();
}

CSession::CSession(boost::asio::io_context &io_context, std::weak_ptr<CServer> server)
    : _socket(io_context), _server(server), _b_close(false), _close_after_send(false),
      _disconnect_handled(false), _user_uid(0) {
    _session_id = generate_uuid(); // 每个会话分配一个唯一id，服务器用它管理会话
    _recv_head_node = std::make_shared<MsgNode>(HEAD_TOTAL_LEN); // 包头固定4字节
    std::cout << "session created, id = " << _session_id << std::endl;
}

CSession::~CSession() {
    std::cout << "session destroyed, id = " << _session_id << std::endl;
}

tcp::socket &CSession::GetSocket() {
    return _socket;
}

std::string &CSession::GetSessionId() {
    return _session_id;
}

void CSession::SetUserId(int uid) {
    _user_uid = uid;
}

int CSession::GetUserId() {
    return _user_uid;
}

// 连接建立后开始接收数据：先读4字节包头
void CSession::Start() {
    // Session 在等待 accept 前就已构造，超时计时应从真正接入时开始。
    UpdateHeartbeat();
    ReadHead(HEAD_TOTAL_LEN);
}

// 发送消息（std::string版本）
void CSession::Send(std::string msg, short msgid) {
    std::lock_guard<std::mutex> lock(_send_lock);
    if (_close_after_send) {
        return;
    }
    std::size_t send_que_size = _send_que.size();
    // 发送队列已满，说明对端消费能力不足，丢弃本次消息防止内存无限膨胀
    if (send_que_size > MAX_SENDQUE) {
        std::cout << "session: " << _session_id << " send que fulled, size is " << MAX_SENDQUE << endl;
        return;
    }

    // 封装成发送节点（构造时自动打包头部：id + 长度）
    _send_que.push(std::make_shared<SendNode>(msg.c_str(), static_cast<short>(msg.length()), msgid));

    // 队列里已经有消息在发送中，等前面发完再发，避免数据乱序
    if (send_que_size > 0) {
        return;
    }

    // 队列之前是空的，说明当前没有异步写在进行，立刻启动第一个写
    auto &msgnode = _send_que.front();
    boost::asio::async_write(_socket, boost::asio::buffer(msgnode->_data, msgnode->_total_len),
        std::bind(&CSession::HandleWrite, this, std::placeholders::_1, SharedSelf()));
}

// 发送消息（char*版本）
void CSession::Send(char *msg, short max_length, short msgid) {
    std::lock_guard<std::mutex> lock(_send_lock);
    if (_close_after_send) {
        return;
    }
    std::size_t send_que_size = _send_que.size();
    if (send_que_size > MAX_SENDQUE) {
        std::cout << "session: " << _session_id << " send que fulled, size is " << MAX_SENDQUE << endl;
        return;
    }

    _send_que.push(std::make_shared<SendNode>(msg, max_length, msgid));
    if (send_que_size > 0) {
        return;
    }

    auto &msgnode = _send_que.front();
    boost::asio::async_write(_socket, boost::asio::buffer(msgnode->_data, msgnode->_total_len),
        std::bind(&CSession::HandleWrite, this, std::placeholders::_1, SharedSelf()));
}

// 关闭连接：关闭socket后，进行中的异步读写会以error结束，从而触发清理流程
void CSession::Close() {
    // 加锁保证同一时刻只有一个线程执行Close
    std::lock_guard<std::mutex> lock(_session_mtx);
    _b_close = true;
    boost::system::error_code ec;
    _socket.close(ec); // 重复close不会抛异常，安全
}

// 统一断线清理入口，读写错误、协议异常、踢人通知发送完成和心跳超时都走这里。
// 清理顺序：
// 1. 原子标记保证多个异步回调同时报错时只清理一次，避免在线数重复减一；
// 2. 关闭 socket，并先注销本机 _sessions、UserMgr 和在线人数；
// 3. 已登录用户再获取登录锁，串行化“旧连接断开”和“同账号新连接登录”；
// 4. 只有 Redis 中的 session_id 仍属于本连接时才删除在线路由，避免误删新连接。
// Redis 或分布式锁暂时不可用时只影响远端路由清理，不得阻断本机会话注销。
void CSession::HandleDisconnect() {
    // close() 会令尚未完成的读写回调也收到错误，因此这里必须是幂等入口。
    if (_disconnect_handled.exchange(true)) {
        return;
    }

    // 本地资源不依赖 Redis，优先释放；CServer::ClearSession 本身也是幂等的。
    Close();
    if (auto server = _server.lock()) {
        // 服务器析构后 weak_ptr 会失效；此时析构流程已经清空会话表，无需再访问悬空对象。
        server->ClearSession(_session_id);
    }

    if (_user_uid == 0) {
        return; // 尚未登录的连接没有 Redis 在线路由
    }

    // 使用与登录流程相同的用户级分布式锁，避免断线清理和新登录并发改写路由。
    const auto uid_str = std::to_string(_user_uid);
    const auto lock_key = LOGIN_LOCK_PREFIX + uid_str;
    auto lock_result = RedisMgr::GetInstance()->acquireLock(
        lock_key, LOCK_TIME_OUT, ACQUIRE_TIME_OUT);
    if (lock_result.result != RedisLockResult::Acquired) {
        std::cout << "cleanup session failed to acquire login lock, uid = "
                  << _user_uid << ", session = " << _session_id << std::endl;
        return;
    }

    // 无论后续 Get/Del 是否成功，离开作用域时都释放本次持有的登录锁。
    const auto identifier = lock_result.identifier;
    Defer defer([identifier, lock_key]() {
        RedisMgr::GetInstance()->releaseLock(lock_key, identifier);
    });

    std::string redis_session_id;
    const bool found = RedisMgr::GetInstance()->Get(
        USER_SESSION_PREFIX + uid_str, redis_session_id);
    if (!found || redis_session_id != _session_id) {
        // ID 不一致说明同一 uid 已经建立了新会话，旧连接只能清理自己，不能动新路由。
        return;
    }

    // 两个 key 都由当前会话创建：一个记录 session，一个记录用户所在的 ChatServer。
    RedisMgr::GetInstance()->Del(USER_SESSION_PREFIX + uid_str);
    RedisMgr::GetInstance()->Del(USERIPPREFIX + uid_str);
}

std::shared_ptr<CSession> CSession::SharedSelf() {
    return shared_from_this();
}

// 写完成回调：弹出队首消息，若队列还有消息则继续发送下一条
void CSession::HandleWrite(const boost::system::error_code &error, std::shared_ptr<CSession> shared_self) {
    try {
        if (!error) {
            bool should_close = false;
            {
                std::lock_guard<std::mutex> lock(_send_lock);
                _send_que.pop();
                if (!_send_que.empty()) {
                    auto &msgnode = _send_que.front();
                    boost::asio::async_write(_socket, boost::asio::buffer(msgnode->_data, msgnode->_total_len),
                        std::bind(&CSession::HandleWrite, this, std::placeholders::_1, shared_self));
                } else if (_close_after_send) {
                    should_close = true;
                }
            }
            if (should_close) {
                HandleDisconnect();
            }
        } else {
            std::cout << "handle write failed, error is " << error.message() << endl;
            HandleDisconnect();
        }
    } catch (std::exception &e) {
        std::cerr << "Exception code : " << e.what() << endl;
    }
}

// 读包头：包头固定4字节 = [消息id(2字节)] [消息体长度(2字节)]
void CSession::ReadHead(int head_len) {
    auto self = shared_from_this();
    asyncReadFull(head_len, [self, this](const boost::system::error_code &ec, std::size_t bytes_transfered) {
        try {
            if (ec) {
                std::cout << "handle read failed, error is " << ec.message() << endl;
                HandleDisconnect();
                return;
            }
            if (bytes_transfered < HEAD_TOTAL_LEN) {
                std::cout << "read length not match, read [" << bytes_transfered << "] , total ["
                          << HEAD_TOTAL_LEN << "]" << endl;
                HandleDisconnect();
                return;
            }

            _recv_head_node->Clear(); // 复用包头节点
            memcpy(_recv_head_node->_data, _data, bytes_transfered);

            // 解析消息id（网络字节序 -> 主机字节序）
            short msg_id = 0;
            memcpy(&msg_id, _recv_head_node->_data, HEAD_ID_LEN);
            msg_id = boost::asio::detail::socket_ops::network_to_host_short(msg_id);

            // 解析消息体长度（网络字节序 -> 主机字节序）
            short msg_len = 0;
            memcpy(&msg_len, _recv_head_node->_data + HEAD_ID_LEN, HEAD_DATA_LEN);
            msg_len = boost::asio::detail::socket_ops::network_to_host_short(msg_len);

            // 非法长度直接断开，防止恶意包导致内存越界
            if (msg_id <= 0 || msg_id > MAX_LENGTH || msg_len <= 0 || msg_len > MAX_LENGTH) {
                std::cout << "invalid msg id [" << msg_id << "] or length [" << msg_len << "]" << endl;
                HandleDisconnect();
                return;
            }

            // 根据包体长度创建接收节点，继续读包体
            _recv_msg_node = std::make_shared<RecvNode>(msg_len, msg_id);
            ReadBody(msg_len);
        } catch (std::exception &e) {
            std::cout << "read head exception : " << e.what() << endl;
            HandleDisconnect();
        }
    });
}

// 读包体：读满 body_len 字节后解析消息并投递到逻辑层
void CSession::ReadBody(int body_len) {
    auto self = shared_from_this();
    asyncReadFull(body_len, [self, this, body_len](const boost::system::error_code &ec, std::size_t bytes_transfered) {
        try {
            if (ec) {
                std::cout << "handle read failed, error is " << ec.message() << endl;
                HandleDisconnect();
                return;
            }
            if (bytes_transfered < static_cast<std::size_t>(body_len)) {
                std::cout << "read length not match, read [" << bytes_transfered << "] , total ["
                          << body_len << "]" << endl;
                HandleDisconnect();
                return;
            }

            // 把包体拷贝进接收节点，并补一个'\0'方便以字符串形式打印
            memcpy(_recv_msg_node->_data, _data, bytes_transfered);
            _recv_msg_node->_cur_len += static_cast<short>(bytes_transfered);
            _recv_msg_node->_data[_recv_msg_node->_total_len] = '\0';
            std::cout << "receive data is " << _recv_msg_node->_data << endl;

            // 心跳属于传输层控制帧，不进入单线程 LogicSystem 队列。
            // 身份认证仍由登录流程负责；这里仅检测连接是否能收发数据。
            if (_recv_msg_node->GetMsgId() == ID_HEART_BEAT_REQ) {
                Json::CharReaderBuilder reader;
                Json::Value request;
                std::string errors;
                std::istringstream body(std::string(_recv_msg_node->_data, body_len));
                if (!Json::parseFromStream(reader, body, &request, &errors) || !request.isObject()) {
                    // 错误心跳不续期，沿用协议异常的统一清理路径。
                    HandleDisconnect();
                    return;
                }
                UpdateHeartbeat();
                Json::Value heartbeat_rsp;
                heartbeat_rsp["error"] = ErrorCode::Success;
                heartbeat_rsp["server_time"] = Json::Int64(std::time(nullptr));
                Send(heartbeat_rsp.toStyledString(), ID_HEARTBEAT_RSP);
                ReadHead(HEAD_TOTAL_LEN);
                return;
            }

            // 业务帧读满也说明传输仍然活跃；业务 JSON 与权限仍由 LogicSystem 校验。
            // 半包和零散字节不续期，避免对端只发几个字节就无限占用连接。
            UpdateHeartbeat();

            // 封装成逻辑节点投递给逻辑层处理（登录校验、聊天转发等）
            LogicSystem::GetInstance()->PostMsgToQue(
                std::make_shared<LogicNode>(shared_from_this(), _recv_msg_node));

            // 处理完一条消息，继续读下一条的包头（长连接循环接收）
            ReadHead(HEAD_TOTAL_LEN);
        } catch (std::exception &e) {
            std::cout << "read body exception : " << e.what() << endl;
            HandleDisconnect();
        }
    });
}

// 清空缓冲区，并从0开始读满 maxLength 字节
void CSession::asyncReadFull(std::size_t maxLength,
                             std::function<void(const boost::system::error_code &, std::size_t)> handler) {
    ::memset(_data, 0, MAX_LENGTH);
    asyncReadLen(0, maxLength, handler);
}

// 循环读取，直到累计读满 total_len 字节才回调（解决TCP粘包/半包问题）
void CSession::asyncReadLen(std::size_t read_len, std::size_t total_len,
                            std::function<void(const boost::system::error_code &, std::size_t)> handler) {
    auto self = shared_from_this();
    _socket.async_read_some(boost::asio::buffer(_data + read_len, total_len - read_len),
        [read_len, total_len, handler, self](const boost::system::error_code &ec, std::size_t bytes_transfered) {
            if (ec) {
                handler(ec, read_len + bytes_transfered); // 出错直接回调，由上层统一处理
                return;
            }
            if (read_len + bytes_transfered >= total_len) {
                handler(ec, read_len + bytes_transfered); // 长度够了，回调
                return;
            }
            // 半包：继续读剩下的字节
            self->asyncReadLen(read_len + bytes_transfered, total_len, handler);
        });
}

LogicNode::LogicNode(std::shared_ptr<CSession> session, std::shared_ptr<RecvNode> recvnode)
    : _session(session), _recvnode(recvnode) {
}

void CSession::NotifyOffline(){
    Json::Value  rtvalue;
	rtvalue["error"] = ErrorCode::Success;
	rtvalue["uid"] = _user_uid;

	std::string return_str = rtvalue.toStyledString();

    // 控制消息必须进入队列；写完队列后由 HandleWrite 主动关闭 socket。
    // 设置标志后，普通 Send 会拒绝继续入队，避免已被踢会话继续收发业务消息。
    std::lock_guard<std::mutex> lock(_send_lock);
    if (_close_after_send) {
        return;
    }
    const bool write_in_progress = !_send_que.empty();
    _send_que.push(std::make_shared<SendNode>(return_str.c_str(),
        static_cast<short>(return_str.length()), ID_NOTIFY_OFF_LINE_REQ));
    _close_after_send = true;
    if (!write_in_progress) {
        auto &msgnode = _send_que.front();
        boost::asio::async_write(_socket, boost::asio::buffer(msgnode->_data, msgnode->_total_len),
            std::bind(&CSession::HandleWrite, this, std::placeholders::_1, SharedSelf()));
    }
	return;
}

bool CSession::isHeartbeatExpired() const {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return now - _last_heartbeat.load() >= HEARTBEAT_TIMEOUT * 1000LL;
}


void CSession::UpdateHeartbeat(){
    _last_heartbeat.store(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
