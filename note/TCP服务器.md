# TCP 服务器搭建总结（ChatServer）

基于 Boost.Asio 实现 TCP 长连接服务器。
涉及文件：`ChatServer.cpp`、`CServer`、`AsioIOServicePool`、`CSession`、`MsgNode`、`LogicSystem`。

---

## 1. 整体架构

一个 TCP 服务器至少要处理三件事：**接连接、收数据、发数据**。本项目把它拆成了几个各司其职的组件：

| 组件 | 职责 |
| --- | --- |
| `ChatServer.cpp`（main） | 启动入口：创建主 `io_context`、线程池、CServer，并捕获退出信号优雅关闭 |
| `CServer` | 监听端口、接受新连接、用 `map` 管理所有存活会话 |
| `AsioIOServicePool` | IO 线程池：N 个 `io_context`，每个跑一个线程，用 round-robin 分配连接 |
| `CSession` | 单个连接上的全部收发逻辑：读包头→读包体→投递逻辑层，以及发送队列 |
| `MsgNode` / `RecvNode` / `SendNode` | 消息内存节点：存放包头和包体，负责组装/解析消息 |
| `LogicSystem` | 逻辑层（单例）：一个工作线程消费消息队列，按 `msg_id` 分发处理 |

**线程模型**（三层分工）：

```text
主线程:     io_context.run() ──→ acceptor 监听新连接
IO线程池:   20 个 io_context，每个线程一个 ──→ 连接的读写回调
逻辑线程:   LogicSystem 工作线程 ──→ 处理业务（登录校验、消息转发）
```

网络层（IO 线程）只负责收发，不碰业务；业务层通过队列解耦，避免耗时操作卡住 IO 线程。

---

## 2. 启动流程（main）

```cpp
int main() {
    try {
        auto& config = ConfigMgr::Inst();
        auto pool = AsioIOServicePool::GetInstance();      // 启动 IO 线程池
        boost::asio::io_context io_context;                // 主线程专用，负责 accept

        // 捕获 Ctrl+C / 终止信号，做优雅退出
        boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&io_context, pool](auto, auto) {
            io_context.stop();   // 停主线程
            pool->Stop();        // 停 IO 线程池（join 所有线程）
        });

        CServer server(io_context, std::stoi(config["SelfServer"]["port"]));
        io_context.run();        // 主线程阻塞在这里，等待连接或退出信号
    } catch (std::exception& e) {
        std::cout << "Exception : " << e.what() << std::endl;
    }
}
```

要点：主 `io_context` 只跑 acceptor，保证"接受连接"是串行的；真正处理数据的读写分散到线程池里的各个 `io_context`，充分利用多核。

---

## 3. CServer：监听与接受连接

```cpp
void CServer::StartAccept() {
    // 从线程池轮询取一个 io_context 创建会话（该连接的读写都绑定到这个线程）
    auto& io_context = AsioIOServicePool::GetInstance()->GetIOService();
    std::shared_ptr<CSession> new_session = std::make_shared<CSession>(io_context, this);
    // 在主 io_context 上异步接受，把 socket 交给会话保管
    _acceptor.async_accept(new_session->GetSocket(),
        std::bind(&CServer::HandleAccept, this, new_session, std::placeholders::_1));
}

void CServer::HandleAccept(std::shared_ptr<CSession> new_session,
                           const boost::system::error_code& error) {
    if (!error) {
        new_session->Start();                          // 成功：开始收包
        std::lock_guard<std::mutex> lock(_mutex);
        _sessions.insert({new_session->GetSessionId(), new_session}); // 登记，维持生命周期
    } else {
        std::cout << "session accept failed, error is " << error.message() << std::endl;
    }
    StartAccept(); // 无论成败都要继续接受下一个连接（长连接服务器）
}
```

- `_sessions`：`map<session_id, shared_ptr<CSession>>`，用互斥锁保护（accept 回调在主线程，清理在 IO 线程）。
- 会话断开时调用 `ClearSession(session_id)` 从 map 移除，`shared_ptr` 归零后自动析构。
- `_io_context` 引用保留下来，后续做心跳定时器（`steady_timer`）时会用到。

---

## 4. AsioIOServicePool：IO 线程池

```cpp
class AsioIOServicePool : public Singleton<AsioIOServicePool> {
    ...
    std::vector<boost::asio::io_context> _ioServices;              // N 个 io_context
    std::vector<executor_work_guard<...>> _workGuards;             // 工作保持器
    std::vector<std::thread> _threads;                             // N 个线程
    std::atomic<std::size_t> _nextIOService;                       // 轮询索引
};
```

三个关键点：

1. **work_guard（工作保持器）**：`io_context::run()` 在没有任务时会立刻返回。用 `make_work_guard(ioc)` 造一个"假任务"，让 `run()` 一直阻塞等待，即使当前没有连接也不退出。
2. **round-robin 分发**：`GetIOService()` 每次返回下一个 `io_context`（原子自增取模），把新连接平均分到不同线程。
3. **Stop()**：先 `reset()` 所有 work_guard，再 `stop()` 所有 `io_context`，最后 `join()` 线程。`reset` 和 `stop` 缺一不可：只 reset，已绑定的读写事件还会让 `run()` 阻塞；只 stop，后续无法正常收尾。

> 注意：老版本的 `io_context::work` 在新版 Boost 中已弃用，用 `executor_work_guard` 代替。

---

## 5. 自定义协议与粘包/半包处理

### 5.1 消息格式

TCP 是**字节流**，没有消息边界。所以双方约定一个固定格式：

```text
包头（4字节）         			包体（n字节）
┌─────────────┬────────────────────────┐
│ msg_id(2字节) │ msg_len(2字节) │ body  │
└─────────────┴────────────────────────┘
 消息id，网络字节序   包体长度，网络字节序
```

### 5.2 粘包 / 半包

- **粘包**：发送方连续发两条消息，接收方一次 `read` 可能读到两条粘在一起的数据。
- **半包**：一条消息太长，一次 `read` 只读到一部分。

解决思路（本项目做法）：

1. 先**循环读取**，读满固定的 4 字节包头；
2. 从包头解析出 `msg_id` 和 `msg_len`；
3. 校验长度合法（`0 < msg_len <= MAX_LENGTH`），再**循环读取** `msg_len` 字节包体；
4. 包体读满后处理消息，然后回到第 1 步继续读下一条。

核心函数 `asyncReadLen`（循环读直到读满 `total_len`）：

```cpp
void CSession::asyncReadLen(std::size_t read_len, std::size_t total_len, handler) {
    auto self = shared_from_this();
    _socket.async_read_some(boost::asio::buffer(_data + read_len, total_len - read_len),
        [read_len, total_len, handler, self](const boost::system::error_code& ec,
                                             std::size_t bytes_transfered) {
            if (ec) {
                handler(ec, read_len + bytes_transfered);   // 出错：交给上层统一处理
                return;
            }
            if (read_len + bytes_transfered >= total_len) {
                handler(ec, read_len + bytes_transfered);   // 读满：回调
                return;
            }
            // 半包：继续读剩余字节
            self->asyncReadLen(read_len + bytes_transfered, total_len, handler);
        });
}
```

### 5.3 防恶意包

解析出 `msg_len` 后必须校验：

```cpp
if (msg_id <= 0 || msg_id > MAX_LENGTH || msg_len <= 0 || msg_len > MAX_LENGTH) {
    Close();                                  // 非法长度直接断开
    _server->ClearSession(_session_id);
    return;
}
```

如果不校验，攻击者伪造一个超大 `msg_len`（比如 65535），`asyncReadLen` 就会疯狂申请/读取内存，最终越界崩溃。

---

## 6. MsgNode：消息节点的内存布局

### 6.1 基类 MsgNode

```cpp
class MsgNode {
public:
    MsgNode(short max_len) : _total_len(max_len), _cur_len(0) {
        _data = new char[_total_len + 1]();  // 多分配 1 字节，放字符串结束符 '\0'
        _data[_total_len] = '\0';
    }
    void Clear() { memset(_data, 0, _total_len); _cur_len = 0; } // 复用节点
    short _cur_len;    // 已读写字节数
    short _total_len;  // 缓冲区总长度
    char* _data;       // 数据
};
```

- `_total_len` 只存"内容长度"，发送节点实际数据 = 头部 4 字节 + 内容。
- `Clear()` 让节点可复用，减少频繁 `new/delete`。

### 6.2 RecvNode（接收节点）

```cpp
class RecvNode : public MsgNode {
    friend class LogicSystem;            // 逻辑层需要读 _msg_id 做路由
public:
    RecvNode(short max_len, short msg_id) : MsgNode(max_len), _msg_id(msg_id) {}
private:
    short _msg_id;                       // 消息id，来自包头
};
```

会话层读包头时创建：`_recv_msg_node = make_shared<RecvNode>(msg_len, msg_id);`，然后读包体填 `_data`。

### 6.3 SendNode（发送节点）

```cpp
class SendNode : public MsgNode {
public:
    SendNode(const char* msg, short max_len, short msg_id)
        : MsgNode(max_len + HEAD_TOTAL_LEN), _msg_id(msg_id) {
        short id   = host_to_network_short(msg_id);   // id 转网络字节序
        short len  = host_to_network_short(max_len);  // 长度转网络字节序
        memcpy(_data, &id, HEAD_ID_LEN);              // 写 id
        memcpy(_data + HEAD_ID_LEN, &len, HEAD_DATA_LEN); // 写长度
        memcpy(_data + HEAD_TOTAL_LEN, msg, max_len); // 写包体
    }
private:
    short _msg_id;
};
```

发送节点在**构造时就拼好整个包**（头部 + 包体），`async_write` 一次写出，避免发送时反复拼包。

---

## 7. 发送队列

### 7.1 为什么需要发送队列

`async_write` 是异步的，如果上层连续调用两次 `async_write`：

- 第一个写还没完成，第二个写就发起，同一 socket 的并发写**行为未定义**；
- 即使能写，两条消息的顺序也无法保证。

所以用队列串行化：**同一时刻只允许一个 `async_write` 在飞**。



队列起到一个排序的作用，将要发送的数据放在队列中，当有人要`send`的时候，发现队列不为空，就将消息加入到队列中，在回调函数中去`send`，妙啊



### 7.2 发送流程

```cpp
void CSession::Send(std::string msg, short msgid) {
    std::lock_guard<std::mutex> lock(_send_lock);
    std::size_t send_que_size = _send_que.size();
    if (send_que_size > MAX_SENDQUE) {           // 队列满：对端消费太慢，丢弃
        std::cout << "send que fulled" << std::endl;
        return;
    }
    _send_que.push(std::make_shared<SendNode>(msg.c_str(),
                   static_cast<short>(msg.length()), msgid));
    if (send_que_size > 0) {
        return;                                   // 已有写在进行，等前面的发完
    }
    // 队列之前是空的：当前没有写操作，立刻启动第一个
    auto& msgnode = _send_que.front();
    boost::asio::async_write(_socket,
        boost::asio::buffer(msgnode->_data, msgnode->_total_len),
        std::bind(&CSession::HandleWrite, this, std::placeholders::_1, SharedSelf()));
}

void CSession::HandleWrite(const boost::system::error_code& error,
                           std::shared_ptr<CSession> shared_self) {
    if (!error) {
        std::lock_guard<std::mutex> lock(_send_lock);
        _send_que.pop();                          // 弹出已发送的消息
        if (!_send_que.empty()) {                 // 还有消息就继续发下一条
            async_write(...);                     // 递归启动下一个写
        }
    } else {
        Close();                                  // 写失败：连接不可用
        _server->ClearSession(_session_id);
    }
}
```

流程一句话：**入队 → 队空则启动写 → 写完成弹队首 → 队列非空继续写**。`MAX_SENDQUE` 防止对端不消费时内存无限膨胀。

---

## 8. 接收队列（生产-消费者模型）

### 8.1 投递：CSession → LogicSystem

```cpp
// CSession 读满包体后：
LogicSystem::GetInstance()->PostMsgToQue(
    std::make_shared<LogicNode>(shared_from_this(), _recv_msg_node));
ReadHead(HEAD_TOTAL_LEN);   // 继续读下一条（长连接循环）
```

`LogicNode` 就是队列元素：`{ shared_ptr<CSession> _session; shared_ptr<RecvNode> _recvnode; }`，记录"谁发的 + 发了什么"。

### 8.2 消费：LogicSystem 工作线程

这个工作线程从逻辑队列中取数据，消费数据

队列为空，这个线程挂起，等待有逻辑队列唤醒

```cpp
void LogicSystem::PostMsgToQue(std::shared_ptr<LogicNode> msg) {
    std::unique_lock<std::mutex> unique_lk(_mutex);
    _msg_que.push(msg);
    if (_msg_que.size() == 1) {
        unique_lk.unlock();       // 队列从空变非空，唤醒工作线程
        _consume.notify_one();
    }
}

void LogicSystem::DealMsg() {              // 工作线程主循环
    for (;;) {
        std::unique_lock<std::mutex> unique_lk(_mutex);
        while (_msg_que.empty() && !_b_stop) {
            _consume.wait(unique_lk);      // 队列空则挂起
        }
        if (_b_stop) { /* 处理完剩余消息后 break */ }
        auto msg_node = _msg_que.front();
        auto iter = _fun_callbacks.find(msg_node->_recvnode->_msg_id);
        if (iter != _fun_callbacks.end()) {
            iter->second(msg_node->_session, msg_node->_recvnode->_msg_id,
                std::string(msg_node->_recvnode->_data, msg_node->_recvnode->_cur_len));
        }
        _msg_que.pop();
    }
}
```

这套模型的好处：

- **IO 线程只负责收发**，登录校验、数据库查询等耗时操作全部挪到逻辑线程；
- 消息**串行处理**，同一逻辑线程内不需要再考虑业务数据竞争；
- 注册新业务只需要 `RegisterCallBacks()` 里加一行映射，例如：

```cpp
_fun_callbacks[MSG_CHAT_LOGIN] = std::bind(&LogicSystem::LoginHandler, this,
    placeholders::_1, placeholders::_2, placeholders::_3);
```

---

## 9. 会话管理

### 9.1 生命周期

- 连接接入：`CServer::HandleAccept` 把 `shared_ptr<CSession>` 存进 `_sessions`；
- 连接断开：会话回调里 `Close()` → `_server->ClearSession(session_id)` 移除 map 条目；
- 最后一个 `shared_ptr` 消失时 `~CSession()` 自动执行。

### 9.2 为什么用 enable_shared_from_this

异步回调执行时，服务器可能已经清除了这个会话。如果回调里直接用裸 `this`，`this` 可能已被析构 → **悬垂指针崩溃**。

解决：每次发起异步读都先 `auto self = shared_from_this();`，回调捕获 `self`，保证回调执行期间会话一定存活：

```cpp
void CSession::ReadHead(int head_len) {
    auto self = shared_from_this();   // 关键！
    asyncReadFull(head_len, [self, this](...) { ... });
}
```

### 9.3 关闭与清理

```cpp
void CSession::Close() {
    std::lock_guard<std::mutex> lock(_session_mtx);
    _b_close = true;
    boost::system::error_code ec;
    _socket.close(ec);   // 关闭后，挂起的异步读写会以 error 回调结束
}
```

关闭 socket 后，正在进行的 `async_read_some` / `async_write` 会立刻以错误码回调，从而触发 `ClearSession`，形成完整的清理链。

---

## 10. 关键设计要点 

1. **字节序**：包头里的 `msg_id`、`msg_len` 一律网络字节序，收发都要用 `host_to_network_short` / `network_to_host_short` 转换。
2. **定长包头**：包头固定 4 字节是解决粘包的基础；先读满包头再读包体，两段都靠"循环读满"保证完整性。
3. **长度校验**：`msg_len > MAX_LENGTH` 必须断开连接，这是防内存越界的最低要求。
4. **错误分支要 return**：读包头/包体出错或数据不足时，`Close()` + `ClearSession()` 之后必须 `return`，否则会拿着不完整的数据继续解析。
5. **缓冲区**：`CSession` 直接用栈数组 `char _data[MAX_LENGTH]`，避免手动 `new/delete` 泄漏；`MsgNode` 构造多分配 1 字节存 `'\0'`，方便打印调试。
6. **发送队列**：同一 socket 同时只允许一个 `async_write`，消息用 `SendNode` 预先拼好整包再写出。
7. **接收队列**：网络层与业务层通过 `condition_variable + mutex` 队列解耦，业务单线程消费。
8. **线程安全**：`_sessions`、`_send_que`、`_msg_que` 各自的互斥锁各管各的，避免共享数据无保护。
9. **对象存活**：所有异步回调都要持有 `shared_from_this()`，防止会话被提前析构。
10. **后续扩展**：心跳检测（`steady_timer` + 最后活跃时间）、单服/多服踢人、token 校验后的用户与 session 绑定、断点续传等，都是在这个骨架上继续加逻辑。

---

## 一条消息的完整旅程

```text
客户端
  │  发送 [id|len|body]
  ▼
CServer::HandleAccept ──► CSession::Start
  ▼
CSession::ReadHead  ── 循环读满 4 字节包头，解析 id/len，校验
  ▼
CSession::ReadBody  ── 循环读满 len 字节包体
  ▼
PostMsgToQue(LogicNode) ──► LogicSystem 消息队列
  ▼
LogicSystem::DealMsg  ── 按 msg_id 找到处理函数（如 LoginHandler）
  ▼
处理函数调用 session->Send(回包, MSG_CHAT_LOGIN_RSP)
  ▼
Send 入发送队列 ── async_write ──► HandleWrite 弹出，继续发下一条
  ▼
客户端收到回包
```
