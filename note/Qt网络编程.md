# Qt网络编程

[官方文档](https://doc.qt.io/qt-6/qtnetwork-index.html)

> 对应项目代码：`chat_app desktop/httpmgr.h/cpp`（HTTP 客户端）、`chat_app desktop/tcpmgr.h/cpp`（TCP 客户端）
>

Qt 的网络模块做的事情和 asio 一样，都是把操作系统 socket 包一层，但封装方式完全不同：

* asio 用 `io_context` + 回调（回调里再开下一次异步操作）
* Qt 用**事件循环 + 信号槽**，网络事件（连上了、有数据了、出错了）都会变成信号，我们只需要 connect



1. **一切异步**——调用立即返回，结果通过信号告诉你；
2. **消息怎么封包、怎么解包**——TCP 是字节流，没有消息边界。

本项目把这两件事都用到了：HTTP 短连接（登录/注册/验证码）+ TCP 长连接（聊天），正好是 Qt 网络编程最典型的两块。



### Qt网络模块有哪些类

| 类 | 作用 | 本项目 |
| --- | --- | --- |
| `QNetworkAccessManager` | HTTP 客户端总管：发请求、收响应、管连接池 | HttpMgr 内部用了一个 |
| `QNetworkRequest` | 描述一次请求（URL、请求头） | PostHttpReq 里用 |
| `QNetworkReply` | 一次请求的响应对象（异步返回） | finished 信号里读 |
| `QTcpSocket` | TCP 客户端 socket（异步） | TcpMgr 内部用了一个 |
| `QTcpServer` | TCP 服务端（监听、接收连接） | 未用（服务端是 asio 写的） |
| `QUdpSocket` | UDP socket | 未用 |
| `QHostAddress` | IP 地址封装 | 连接服务器时隐式使用 |
| `QDataStream` | 数据的二进制序列化（配合字节序） | 封包/解包使用 |

CMake 引入方式（本项目 CMakeLists.txt）：

```
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network)
target_link_libraries(chat_app PRIVATE Qt::Core Qt::Widgets Qt::Network)
```

> 网络模块对应 Qt 的 Network 组件，不引入的话 `QTcpSocket`、`QNetworkAccessManager` 这些类都用不了



### 为什么GUI程序里的网络必须异步

先看一个反面例子，如果我们在 Qt 里用最原始的同步写法：

```
// 伪代码，同步阻塞
socket.connect();      // 等握手完成，网络慢就卡住
socket.send(request);  // 等数据发完
QString resp = socket.recv();  // 等服务器响应，可能等好几秒
```

这几行代码在执行期间，**主线程（GUI线程）被占死了**：窗口拖不动、按钮点不了、画面不刷新，因为事件循环根本轮不到处理 UI 事件。用户体验就是"程序卡死"。

Qt 的解决方案：

* 所有网络操作**立即返回**，底层在后台干活（操作系统事件通知，类似 epoll 的机制）；
* 干完活之后，Qt 往事件循环里投递一个事件，触发对应的信号；
* 我们的槽函数在**事件循环空闲时**被执行，UI 全程不阻塞。

```
调用 connectToHost / post   ← 立即返回
        │
        ▼
   后台异步干活（握手、传输、等待响应）
        │
        ▼
   事件循环收到通知 → 发出 connected / finished / readyRead 信号
        │
        ▼
   我们 connect 的槽函数被调用
```

所以 Qt 网络编程的基本套路永远是：

```
发起（立即返回） → connect 信号 → 在槽里处理结果
```



## HTTP编程：QNetworkAccessManager

### 一次HTTP请求的完整流程

Qt 发一个 HTTP POST 请求只需要 4 个类配合：

```
QNetworkAccessManager  ← 总管（整个程序一个就够）
QNetworkRequest        ← 描述"发给谁、带什么头"
QNetworkReply          ← 描述"服务器回什么"
finished 信号          ← 通知"响应到了"
```

官方文档原话值得记住：**"One QNetworkAccessManager instance should be enough for the whole Qt application."**——一个应用一个 QNAM 实例就足够了。这正是本项目把 HttpMgr 做成单例的官方依据：

* 连接池、Cookie、代理、缓存等配置都在 manager 里，全局共享；
* 每个请求不需要自己建 manager，用完也不用销毁 manager。

### 本项目 PostHttpReq 逐行拆解

```cpp
void HttpMgr::PostHttpReq(QUrl url, QJsonObject json, ReqId req_id, Modules mod)
{
    // 1. 序列化：QJsonObject → 字节流（HTTP 传输的只是字节）
    QByteArray data = QJsonDocument(json).toJson();

    // 2. 构造请求对象，设置请求头
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setHeader(QNetworkRequest::ContentLengthHeader, QByteArray::number(data.length()));

    // 3. 发起 POST，立即返回 reply
    auto self = shared_from_this();   // 保持 HttpMgr 在异步回调期间存活
    QNetworkReply* reply = _manager.post(request, data);

    // 4. connect finished，回包到达时执行
    connect(reply, &QNetworkReply::finished, [self, reply, req_id, mod]{
        if (reply->error() != QNetworkReply::NoError) {
            // 网络层错误：超时、连接拒绝、DNS 失败等
            emit self->sig_http_finish(req_id, "", ErrorCodes::ERR_NETWORK, mod);
        } else {
            // 成功：读出响应体（就是服务器返回的 json 字符串）
            QString res = reply->readAll();
            emit self->sig_http_finish(req_id, res, ErrorCodes::SUCCESS, mod);
        }
        reply->deleteLater();   // 回收 reply，但不能直接 delete
    });
}
```

拆开看每个点：

**① 请求头**

```cpp
request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
request.setHeader(QNetworkRequest::ContentLengthHeader, QByteArray::number(data.length()));
```

服务器就是靠 `Content-Type` 知道请求体是 json，靠 `Content-Length` 知道要读多少字节。相当于：

```
POST /user_login HTTP/1.1
Content-Type: application/json
Content-Length: 27

{"user":"aaa","passwd":"xxx"}
```

**② 为什么 `_manager.post` 返回的 reply 是 new 出来的**

`post()` 内部创建 `QNetworkReply` 对象并返回指针，**它不归我们 new，也不归我们直接 delete**。官方文档明确说：

> After the request has finished, it is the responsibility of the user to delete the QNetworkReply object... Do not directly delete it inside the slot connected to finished(). You can use the deleteLater() function.

原因：finished 槽函数正在执行时，这个 reply 还在被事件系统引用，直接 `delete` 会造成悬垂指针（可能崩溃）。`deleteLater()` 会把删除请求**投递到事件循环**，等当前事件处理完再安全删除。这是 Qt 异步资源的通用回收方式。

另外 reply 的 parent 是 manager，所以就算忘了 deleteLater，manager 析构时也会把它一起回收（对象树机制兜底）。

**③ 为什么错误处理放在 finished 里而不是单独连 errorOccurred**

两种写法都行。本项目只连了 `finished`，因为 `finished` 是"请求结束"的统一出口——不管成功失败都会触发，在槽里用 `reply->error()` 判断是成功还是失败，一个分支全搞定，逻辑更集中。

**④ 为什么网络错误统一转成 `ErrorCodes::ERR_NETWORK`**

上层（LoginDialog/RegisterDialog）只认业务错误码（密码错、用户不存在……），它不关心底层是 DNS 失败还是超时。HttpMgr 把"网络层失败"翻译成业务错误码，UI 层处理逻辑就只剩一种。**这就是封装的意义：把复杂留给内部，把简单留给外部。**

**⑤ `shared_from_this()` 是干嘛的**

`HttpMgr` 是单例，但它析构时机不受我们控制（由 shared_ptr 引用计数管理）。lambda 里捕获了 `self`（一份 shared_ptr 副本），只要请求还没完成，这份引用就保证 HttpMgr 不会被销毁；请求完成后 lambda 释放，引用计数归零才析构。**避免"回包到达时管理对象已经死了"的悬垂问题。**

### 关于QNetworkAccessManager的线程亲和性

官方文档：QNAM 基于 QObject，**只能从它所属的线程使用**（所有函数是可重入的，但对象本身有线程亲和性）。

本项目所有网络调用都在主线程（GUI 线程），所以没问题。如果要跨线程发请求，规范做法是：

* 把 QNAM 放到工作线程，或者
* 把"发起请求"也通过信号槽投递到 QNAM 所在线程执行。

这是个面试常考点，记住一句话：**QNAM 只能在它所属线程用，但它的所有函数是可重入的**。

### 本项目HTTP常见坑

| 坑 | 说明 |
| --- | --- |
| 直接 delete reply | 崩溃风险，必须 `deleteLater()` |
| 每次请求都 new 一个 manager | 浪费，连接池不共享；一个应用一个就够 |
| 在 finished 槽里用 `readAll()` 之外的指针 | finished 之后 reply 随时可能被回收 |
| 请求未完成就销毁 manager | reply 的 parent 是 manager，manager 没了请求也断 |
| 阻塞等待响应（while(!finished)） | 死锁/卡界面，必须用信号槽 |



## TCP编程：QTcpSocket

HTTP 是无状态短连接：发一次请求，服务器回一次，连接就完事。

聊天需要**长连接**：连上一次，之后随时双向收发消息。Qt 里就是 `QTcpSocket`。

### 客户端连接流程（全部异步）

本项目 TcpMgr 的构造函数把 socket 的 4 个关键信号全连了：

```cpp
// 连接成功
connect(&_socket, &QTcpSocket::connected, [this](){
    qDebug() << "connect to server";
    emit sig_con_success(true);   // 通知 UI："连上了"
});

// 连接断开
connect(&_socket, &QTcpSocket::disconnected, [this]{
    qDebug() << "disconnect to server";
});

// 有数据可读
connect(&_socket, &QTcpSocket::readyRead, [this](){
    // 粘包处理，见下文
});

// 出错（重载信号要 QOverload 指定）
connect(&_socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::errorOccurred),
        [this](QAbstractSocket::SocketError socketError){
            Q_UNUSED(socketError);
            qDebug() << "Error : " << _socket.errorString();
        });
```

发起连接：

```cpp
void TcpMgr::slot_tcp_connect(ServerInfo si)
{
    _host = si.Host;
    _port = static_cast<uint16_t>(si.Port.toUInt());
    _socket.connectToHost(_host, _port);  // 异步！立即返回
}
```

`connectToHost` 和阻塞 socket 编程里的 `connect` 完全不同：

* 它**立即返回**，不等待握手完成；
* 握手成功后 Qt 自动发 `connected` 信号；
* 失败则发 `errorOccurred`。

所以"连接结果"不是一个返回值，而是一个信号。这也是整个 Qt 网络的核心心智模型：**发起异步，结果靠信号**。

### 消息格式设计（封包）

聊天协议要解决一个核心问题：**TCP 是字节流，没有消息边界**。

想象用 `write()` 发了三条消息：

```
发送端：write("你好")  write("在吗")  write("？")
接收端可能一次收到："你好在吗？"      ← 粘包
也可能分两次收到："你好在"  "吗？"    ← 半包
```

这就是著名的**粘包/半包问题**。TCP 只保证字节顺序，不保证"一次 write 对应一次 read"。

解决方案有很多，本项目用的是最常用的**定长消息头 + 长度字段**：

```
┌────────────┬────────────┬────────────────┐
│ 消息ID(2B) │ 消息长度(2B) │  消息体(长度B)  │
└────────────┴────────────┴────────────────┘
```

接收方流程：

```
1. 先凑够 4 字节，解析出 消息ID 和 消息长度
2. 再凑够 长度 字节，这才是完整的消息体
3. 不够就继续等（半包），多了就截取（粘包）
```

### 发送端封包代码

```cpp
void TcpMgr::slot_send_data(ReqId reqId, QString data)
{
    uint16_t id = reqId;
    QByteArray dataByte = data.toUtf8();          // 字符串 → utf-8 字节
    quint16 len = static_cast<quint16>(data.size());

    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);     // 网络字节序
    out << id << len;                             // 写头部：id + len
    block.append(dataByte);                       // 拼消息体
    _socket.write(block);                         // 一次性写出去
}
```

知识点：

* `QDataStream` 是把数据"按格式写进字节流"的工具，`<<` 运算符自动处理多字节类型的存储；
* **字节序必须统一**：发送端显式 `BigEndian`（大端，也就是网络字节序），这样不管客户端跑在 x86（小端）还是其他架构上，字节在网络上都是一样的排法。跨语言通信时（本项目是 C++ 客户端对 C++ 服务端）尤其要注意；
* 消息体用 UTF-8 编码，中文也能传输。

### 接收端解包代码（重点）

```cpp
connect(&_socket, &QTcpSocket::readyRead, [this](){
    // 1. 把新到的数据全部追加进缓冲区
    _buffer.append(_socket.readAll());

    QDataStream stream(&_buffer, QIODevice::ReadOnly);
    stream.setVersion(QDataStream::Qt_6_0);   // 收发双方约定序列化版本

    // 2. 头部解析：消息ID + 消息长度，各2字节
    if (!_b_recy_pending) {
        if (_buffer.size() < static_cast<int>(sizeof(quint16) * 2)) {
            return;   // 连头部都不够，等下一次 readyRead
        }
        stream >> _message_id >> _message_len;
        _buffer = _buffer.mid(sizeof(quint16) * 2);   // 消费掉头部
    }

    // 3. 消息体是否凑齐
    if (_buffer.size() < _message_len) {
        return;   // 半包，继续等
    }

    // 4. 取出一条完整消息，消费掉
    QByteArray messageBody = _buffer.mid(0, _message_len);
    _buffer = _buffer.mid(_message_len);

    // 5. 按消息ID查 handler 表，分发处理
    auto iter = _handler.find(ReqId(_message_id));
    if (iter == _handler.end()) {
        qDebug() << "id error";
        return;
    }
    iter.value()(ReqId(_message_id), _message_len, messageBody);
});
```

这段代码值得反复看，它是 TCP 客户端最重要的部分：

**为什么缓冲区是 `QByteArray` 而不是直接用 readAll 的结果？**

因为一次 `readyRead` 可能只到了一个包的一半。如果这次没凑齐就返回，**下一次 readyRead 来的数据必须和上一次剩下的拼在一起**，所以要把所有没消费的数据暂存在成员变量 `_buffer` 里，等凑齐为止。

**`stream.setVersion(QDataStream::Qt_6_0)` 是干嘛的？**

`QDataStream` 的二进制格式随 Qt 版本会变，收发双方必须用同一个版本，否则同一个 `<<` 写出来的东西 `>>` 可能读不对。这里固定 Qt_6_0，保证客户端和服务器（同样用 QDataStream 封包的话）兼容。

**为什么叫"状态机"式处理？**

因为接收逻辑不是"来一次处理一次"，而是"缓冲区里够不够 → 不够等 → 够了解析"。缓冲区就是状态，`_b_recy_pending` 这个标志标记"当前正在等一个完整包"，保证上一包的残留在下一次解析时不会被当成新包头。

> 小坑（也是学习点）：本项目 `_b_recy_pending` 声明了、注释也写了，但**代码里从头到尾没有给它赋值**，永远是 false。也就是说"头部已解析、正在等消息体"这个状态没有真正记录。正确做法是：解析完头部后置 `_b_recy_pending = true`，凑齐消息体并消费后置回 false。平时数据包都完整到达时看不出来，一旦出现半包就会解析错位。

**一次 readyRead 只处理一个包，够吗？**

严格来说不够：如果缓冲区里同时有 3 个完整包，这段代码只处理 1 个，剩下 2 个要等下次 `readyRead` 才处理（可能等很久）。规范写法是在第 4 步外面套一个 `while` 循环，直到缓冲区不够一个完整包为止。这也是一个可以改进的点。

### 拓展：服务端怎么写（QTcpServer）

本项目服务端是 asio，但 Qt 客户端如果临时要开本地服务，套路如下（了解即可）：

```
QTcpServer server;
server.listen(QHostAddress::Any, 9999);   // 监听端口
connect(&server, &QTcpServer::newConnection, [&]{
    QTcpSocket* sock = server.nextPendingConnection();  // 拿新连接
    // 每个连接单独管理，readyRead 处理数据
});
```

核心点：**服务端一个监听 socket + 每个客户端一个通信 socket**，和 asio 的 acceptor/socket 模型一一对应。

### 拓展：UDP（QUdpSocket）

UDP 是数据报协议，**天然有消息边界**，不需要粘包处理，代价是不保证送达：

```
QUdpSocket socket;
socket.bind(9999);          // 绑定本地端口
socket.writeDatagram(data, QHostAddress("127.0.0.1"), 8888);  // 发
// readyRead 信号 + readDatagram 收
```

聊天室用 TCP 是因为消息不能丢、顺序不能乱；UDP 适合音视频这类"丢一点无所谓、延迟必须低"的场景。



## 序列化：协议设计

### HTTP 层为什么用 JSON

* **跨语言**：服务器是 C++/Node.js，JSON 谁都能解析；
* **可读**：抓包直接能看内容，方便调试；
* 缺点：体积大、解析慢，但对登录/注册这种低频请求完全无所谓。

```cpp
QJsonObject json_obj;
json_obj["user"] = ui->user_edit->text();
json_obj["passwd"] = xorString(ui->pwd_edit->text());
QByteArray data = QJsonDocument(json_obj).toJson();   // 序列化
// 收到响应：QJsonDocument::fromJson(res.toUtf8())    // 反序列化
```

### TCP 层为什么用"二进制头 + JSON体"

聊天消息既要传输效率，又要处理粘包，所以：

* **头**：定长的 id + len，二进制，4 字节，负责"定位消息边界"；
* **体**：仍然是 JSON（本项目把 json 字符串塞进消息体），方便扩展字段、跨语言解析。

这是很多商业协议的做法：**定长二进制头 + 灵活体**。

### 字节序（大端/小端）

```
大端（BigEndian）：高字节在前   0x1234 → 12 34
小端（LittleEndian）：低字节在前  0x1234 → 34 12   （x86 默认）
```

网络字节序规定用大端。本项目发送端显式 `BigEndian`，保证任何平台收发一致。这也是"为什么 `quint16` 而不是 `unsigned short`"的原因：Qt 的 quint 类型保证跨平台固定宽度，unsigned short 在不同平台可能宽度不同。



## 本项目网络分层设计

### HTTP 短连接 vs TCP 长连接

| | HTTP（HttpMgr） | TCP（TcpMgr） |
| --- | --- | --- |
| 连接方式 | 短连接，用完即断 | 长连接，登录后常驻 |
| 通信对象 | GateServer（网关） | ChatServer（聊天服务器） |
| 数据格式 | JSON 字符串 | 二进制头 + JSON 体 |
| 职责 | 验身份（密码/验证码） | 验凭证（token）、收发消息 |
| 生命周期 | 全局单例，可反复发 | 全局单例，一次连一个服务器 |

对应登录流程（详见 [login.md](login.md)）：

```
用户+密码 → HTTP 登录 → 拿到 {host, port, token}
                              ↓
                 用 token 连 TCP 聊天服务器 → 正式进入聊天
```

**为什么分两层、两个管理器？**

* 职责不同：HTTP 是"临时办一件事"，TCP 是"长期住下来"；
* 生命周期不同：HTTP 请求随时可能发起，TCP 连接要等登录成功才有；
* 拆开之后，两个单例互不干扰，各自的信号槽也清晰——HttpMgr 负责回包分发，TcpMgr 负责连接状态和消息分发。

### 两层之间怎么衔接（信号槽链）

登录成功后，LoginDialog 把服务器给的地址信息通过信号交给 TcpMgr：

```cpp
// LoginDialog 里
ServerInfo si;
si.Uid = json_obj["uid"].toInt();
si.Host = json_obj["host"].toString();
si.Port = json_obj["port"].toString();
si.Token = json_obj["token"].toString();
emit sig_connect_tcp(si);      // 只发信号，不关心谁处理

// MainWindow/构造函数里装配
connect(this, &LoginDialog::sig_connect_tcp,
        TcpMgr::GetInstance().get(), &TcpMgr::slot_tcp_connect);
```

这就是 Qt 网络编程和 UI 结合的标准姿势：**UI 只发"我要连服务器"的意图，网络层自己干活，干完再发信号告诉 UI 结果**。全程没有一行代码是互相 new 对方、互相调对方私有方法的。



## 常见坑汇总

| 坑 | 后果 | 正确做法 |
| --- | --- | --- |
| reply 直接 delete | 崩溃/悬垂 | `deleteLater()` |
| 中文长度用 `data.size()` | 头部长度 < 实际字节数，粘包解析错位 | 用 `dataByte.size()`（UTF-8 字节数） |
| 半包时把残留数据丢了 | 消息永久缺失 | 用成员缓冲区暂存，凑齐再消费 |
| 一次 readyRead 只处理一个包 | 多余包滞留，可能延迟处理 | while 循环消费到不够为止 |
| 收发字节序/版本不一致 | 解析出乱码或错位 | 统一 BigEndian + 统一 QDataStream 版本 |
| 在非所属线程用 QNAM | 未定义行为 | 保持线程亲和性，或通过信号槽投递 |
| 异步回调里用裸 this | 对象销毁后回调崩溃 | `shared_from_this()` 或 connect 时指定接收者 |
| 阻塞等待网络结果 | GUI 卡死 | 全程信号槽，绝不阻塞事件循环 |

> 本项目就踩了"中文长度"这个坑：`slot_send_data` 里 `quint16 len = static_cast<quint16>(data.size())`，`data.size()` 是 QString 的字符数（UTF-16 长度），而消息体实际是 UTF-8 字节。纯英文时两者相等，一旦发中文，len 就会比真实字节数小，服务器按 len 截取会截出半个汉字。正确写法是 `dataByte.size()`。



## 与asio服务端的对比（学习延伸）

| | Qt 客户端 | asio 服务端 |
| --- | --- | --- |
| 事件驱动核心 | 应用事件循环（QApplication::exec） | `io_context` |
| 异步结果通知 | 信号槽 | 回调函数 |
| 连接管理 | 一个 QTcpSocket 成员变量 | 每个连接一个 session 对象 |
| 粘包处理 | 成员缓冲区 + 状态标志 | 读缓冲 + 异步读满指定长度 |
| 适用场景 | GUI 程序（要跟界面交互） | 服务器（高并发、无界面） |

理解一句话就够了：**Qt 的事件循环和 asio 的 io_context 本质是同一件事**——都是"等待 I/O 就绪，然后执行对应的处理函数"。只是 Qt 把处理函数包装成了信号槽，asio 保持裸回调。本项目客户端用 Qt 是因为要画界面，服务端用 asio 是因为要扛高并发，各取所长。



### 面试/复习自测

* 为什么 GUI 程序网络必须异步？事件循环被阻塞会怎样？
* 粘包是什么？半包是什么？为什么 TCP 会有这个问题？
* 定长头方案怎么设计？解析时为什么要缓冲区 + 状态标志？
* 为什么 reply 不能用 delete，要用 deleteLater？
* QNetworkAccessManager 一个应用几个？能跨线程用吗？
* 为什么发送端要显式设置大端字节序？QDataStream 版本号为什么必须统一？
* HTTP 和 TCP 在这个项目里各负责什么？为什么拆成两个管理器？

