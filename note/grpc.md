# gRPC 在聊天室项目中的应用

> 本文基于本项目（chat_app）中的实际代码整理，说明 gRPC 在这个分布式聊天室里是怎么落地使用的。

---

## 1. 为什么这个项目要用 gRPC

聊天室由多个独立进程（服务）组成，它们之间需要互相调用：

- GateServer（C++，HTTP 网关）需要向验证服务器要验证码；
- GateServer 需要向状态服务器查询"该给用户分配哪台聊天服务器"；
- ChatServer 需要向状态服务器校验用户的 token。

这些"服务调用服务"的场景可以用 gRPC 解决，原因是：

| 优势 | 说明 |
| --- | --- |
| 跨语言 | 验证服务器用 Node.js 写，C++ 客户端照样能调用（`.proto` 是通用契约） |
| 性能好 | 基于 HTTP/2 + Protobuf 二进制序列化，比 HTTP+JSON 更轻量 |
| 接口即契约 | `.proto` 文件同时生成服务端基类和客户端 Stub，两端代码不会不一致 |
| 类型安全 | 请求/响应都是强类型 message，不再靠手拼字符串 |

一句话：**gRPC = 像调用本地函数一样调用另一台机器上的函数**。

---

## 2. 项目中的 gRPC 服务全景

### 2.1 服务清单

| 服务 | 进程（语言） | 端口 | RPC 方法 | 职责 |
| --- | --- | --- | --- | --- |
| `VarifyService` | VarifyServer（Node.js） | 50051 | `GetVarifyCode` | 生成验证码、存 Redis、发邮件 |
| `StatusService` | StatusServer（C++） | 50052 | `GetChatServer` | 负载均衡选聊天服务器 + 发 token |
| `StatusService` | StatusServer（C++） | 50052 | `Login` | 校验 uid/token 是否匹配 |

### 2.2 谁调用谁

```text
客户端(Qt)
   │  HTTP
   ▼
GateServer (8080, C++)
   │  gRPC  ──────────────►  VarifyServer (50051, Node.js) ──► Redis + 163邮箱
   │  gRPC  ──────────────►  StatusServer (50052, C++)  ──► 选择 ChatServer + 生成token
   ▼  TCP 长连接
ChatServer (8090/8091, C++)
   │  gRPC  ──────────────►  StatusServer (50052) ──► Login(uid, token) 校验
```

可以看到 **GateServer 和 ChatServer 是 gRPC 客户端，VarifyServer 和 StatusServer 是 gRPC 服务端**。

---

## 3. `.proto` 契约文件

所有服务的接口统一定义在项目根目录的 `proto/message.proto` 中。各服务不再维护自己的协议副本，避免多个文件逐渐产生差异：

```protobuf
syntax = "proto3";          // 使用 proto3 语法
package message;            // 包名，生成代码时变成命名空间 message

// 定义数据结构
message GetVarifyReq {
    string email = 1;       // 字段编号从 1 开始，序列化时用编号代替名字
}

message GetVarifyRsp {
    int32 error = 1;
    string email = 2;
    string code = 3;
}

// 定义服务接口（RPC 方法）
service VarifyService {
    rpc GetVarifyCode (GetVarifyReq) returns (GetVarifyRsp) {}
}
```

同一个文件里还定义了 `StatusService`：

```protobuf
service StatusService {
    rpc GetChatServer (GetChatServerReq) returns (GetChatServerRsp) {}
    rpc Login (LoginReq) returns (LoginRsp) {}
}
```

用 `protoc` 编译后自动生成：

- `message.pb.h / message.pb.cc`：消息类的序列化代码；
- `message.grpc.pb.h / message.grpc.pb.cc`：**服务端 Service 基类** + **客户端 Stub 代理类**。

> 根目录的 `.proto` 是唯一源文件。修改协议后只需要重新生成这一套代码，所有 C++ 服务和 Node.js 服务都会使用同一份定义。

### 3.1 C++ 服务共用一套生成代码

顶层 `CmakeLists.txt` 负责查找 `protoc` 和 gRPC 插件，并为根目录协议定义一次生成规则：

```cmake
set(PROTO_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/proto/message.proto)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto_gen)

add_custom_command(
    OUTPUT ${PROTO_GENERATED_FILES}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
        --cpp_out=${PROTO_GEN_DIR}
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
        ${PROTO_SOURCE}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
        --grpc_out=${PROTO_GEN_DIR}
        --plugin=protoc-gen-grpc=${gRPC_CPP_PLUGIN}
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
        ${PROTO_SOURCE}
    DEPENDS ${PROTO_SOURCE}
)
```

生成的四个文件放在构建目录的 `proto_gen/`：

```text
message.pb.h
message.pb.cc
message.grpc.pb.h
message.grpc.pb.cc
```

为了避免四个服务分别编译同一组 `.pb.cc`，顶层 CMake 还把它们编译成公共静态库：

```cmake
add_library(chat_proto STATIC ${PROTO_GENERATED_FILES})
target_include_directories(chat_proto PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(chat_proto PUBLIC
    gRPC::grpc++
    protobuf::libprotobuf
)
```

`GateServer`、`StatusServer`、`ChatServer1` 和 `ChatServer2` 只需要链接 `chat_proto`，不再自行调用 `protoc`，也不再把生成源码重复加入自己的目标。

---

## 4. 服务端实现

### 4.1 Node.js 版（VarifyServer，跨语言）

`proto.js` 负责加载根目录的 `.proto` 并解析出服务对象：

```js
const PROTO_PATH = path.join(__dirname, '..', 'proto', 'message.proto');
const packageDefinition = protoLoader.loadSync(PROTO_PATH, {...});
const protoDescriptor = grpc.loadPackageDefinition(packageDefinition);
const message_proto = protoDescriptor.message;   // 包名 message
module.exports = message_proto;
```

`server.js` 实现接口并启动服务：

```js
// 实现 RPC：call 是客户端请求，callback 是回包
async function GetVarifyCode(call, callback) {
    let query_res = await redis_module.GetRedis("code_" + call.request.email);
    if (query_res != null) {
        callback(null, { email: call.request.email, error: Errors.DuplicateRequest });
        return;                                    // 验证码已存在，不重复发
    }
    let uniqueId = uuidv4().substring(0, 4);       // 生成 4 位验证码
    await redis_module.SetRedisExpire("code_" + call.request.email, uniqueId, 180);
    await email_module.SendMail({ to: call.request.email, text: '您的验证码为' + uniqueId });
    callback(null, { email: call.request.email, error: Errors.Success });
}

function main() {
    var server = new grpc.Server();
    server.addService(message_proto.VarifyService.service, { GetVarifyCode });
    server.bindAsync('0.0.0.0:50051', grpc.ServerCredentials.createInsecure(), () => {
        server.start();
    });
}
```

业务逻辑很清楚：**查 Redis 有没有旧验证码 → 生成新码存 Redis（180 秒过期）→ 发邮件 → 回包**。C++ 的 GateServer 完全不用关心这些细节，只调用接口。

### 4.2 C++ 版（StatusServer）

实现类继承 proto 生成的 `StatusService::Service`，重写两个虚函数：

```cpp
class StatusServiceImpl final : public StatusService::Service {
    Status GetChatServer(ServerContext* context, const GetChatServerReq* request,
                         GetChatServerRsp* reply) override;
    Status Login(ServerContext* context, const LoginReq* request, LoginRsp* reply) override;
};
```

`GetChatServer` 的核心逻辑（**负载均衡 + 发 token**）：

```cpp
Status StatusServiceImpl::GetChatServer(ServerContext* context,
                                        const GetChatServerReq* request,
                                        GetChatServerRsp* reply) {
    ChatServer server = getChatServer();            // 选当前连接数最少的服务器
    reply->set_host(server.host);
    reply->set_port(server.port);
    reply->set_token(generate_unique_string());     // 生成一次性 token
    reply->set_error(ErrorCode::Success);
    _tokens[request->uid()] = reply->token();       // 记住 uid -> token
    _token_to_server[reply->token()] = server.name; // 记住 token -> 服务器
    return Status::OK;                              // 返回 OK，reply 自动序列化发回
}
```

`getChatServer()` 就是遍历 `_servers`（从 config.ini 读入 ChatServer1/2），返回 `con_count` 最小的那台并自增计数。

`Login` 则是纯校验：

```cpp
auto iter = _tokens.find(uid);
if (iter == _tokens.end())            { reply->set_error(ErrorCode::UidInvalid); }
else if (iter->second != token)       { reply->set_error(ErrorCode::TokenInvalid); }
else                                  { reply->set_error(ErrorCode::Success); }
return Status::OK;
```

服务启动（main 里）：

```cpp
grpc::ServerBuilder builder;
builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
builder.RegisterService(&service);
std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
server->Wait();                 // 阻塞等待，收到信号后 Shutdown()
```

---

## 5. 客户端实现（C++）

### 5.1 一次 RPC 调用的三个固定步骤

以 GateServer 调 VarifyServer 为例（`VarifyGrpcClient::GetVarifyCode`）：

```cpp
// 第 1 步：创建 Channel（连接远程服务器）
std::shared_ptr<Channel> channel =
    grpc::CreateChannel("127.0.0.1:50051", grpc::InsecureChannelCredentials());

// 第 2 步：用 Channel 创建 Stub（远程服务的本地代理）
auto stub = VarifyService::NewStub(channel);

// 第 3 步：准备上下文 + 请求 + 响应，同步调用
ClientContext context;
GetVarifyReq request;
GetVarifyRsp reply;
request.set_email(email);
Status status = stub->GetVarifyCode(&context, request, &reply);

if (status.ok()) {
    return reply;                   // 调用成功
} else {
    reply.set_error(ErrorCode::RPCFaild);   // 网络失败，填错误码返回
}
```

> 这是**同步**（阻塞）一元调用：发起后等服务端回包才返回。项目中业务逻辑层（LogicSystem）本来就是独立线程，阻塞调用不会卡住网络收发。

### 5.2 连接池（为什么需要）

`CreateChannel` 每次调用都会新建连接，而 Stub 借出/归还频繁。所以项目里用**连接池**管理：

```cpp
class RPConPool {
    std::queue<std::unique_ptr<VarifyService::Stub>> _connections; // 池
    std::mutex _mutex;
    std::condition_variable _cond;

    // 借用：池空则等待（unique_lock 可以临时解锁）
    std::unique_ptr<VarifyService::Stub> GetConnection() {
        std::unique_lock<std::mutex> lock(_mutex);
        _cond.wait(lock, [this]() { return _b_stop || !_connections.empty(); });
        auto con = std::move(_connections.front());
        _connections.pop();
        return con;
    }

    // 归还：放回队列并唤醒一个等待线程
    void ReleaseConnection(std::unique_ptr<VarifyService::Stub> connection) {
        std::lock_guard<std::mutex> lock(_mutex);
        _connections.push(std::move(connection));
        _cond.notify_one();
    }
};
```

要点：

- `NewStub` 返回 `unique_ptr`，队列元素也用 `unique_ptr`，全程**移动语义**，零拷贝；
- 每次调用 `GetConnection()` 借用、用完 `ReleaseConnection()` 归还，**必须成对出现**（项目里有的用 `Defer` 保证归还）；
- 池大小在构造函数里配置：GateServer 调 VarifyServer 用 5 个，调 StatusServer 用 10 个。

### 5.3 项目里的两个实际调用点

**场景 1：GateServer 获取验证码**（`/get_varifycode` 路由）

```cpp
auto email = src_root["email"].asString();
GetVarifyRsp rsp = VarifyGrpcClient::GetInstance()->GetVarifyCode(email);
root["error"] = rsp.error();           // 把验证服务器返回的错误码透传给客户端
root["email"] = src_root["email"];
```

**场景 2：GateServer 登录时分配聊天服务器**（`/user_login` 路由）

```cpp
// 密码校验通过后，问 StatusServer 要一台聊天服务器 + token
auto reply = StatusGrpcClient::GetInstance()->GetChatServer(user_info.uid);
root["uid"]   = user_info.uid;
root["token"] = reply.token();   // token 是后续登录聊天服务器的凭证
root["host"]  = reply.host();    // 例如 127.0.0.1
root["port"]  = reply.port();    // 例如 8090（负载均衡选出来的）
```

**场景 3：ChatServer 校验登录 token**（`LoginHandler`，即 TCP 服务器 day16 的逻辑）

```cpp
auto rsp = StatusGrpcClient::GetInstance()->Login(uid, token);
rtvalue["error"] = rsp.error();            // 0 表示 token 合法
if (rsp.error() == ErrorCode::Success) {
    session->SetUserId(uid);
}
session->Send(return_str, MSG_CHAT_LOGIN_RSP);
```

---

## 6. 一次"注册 + 登录"的完整业务闭环

```text
1. 用户点"获取验证码"
   客户端 ──HTTP──► GateServer(/get_varifycode)
                       │ gRPC GetVarifyCode(email)
                       ▼
                  VarifyServer ──► Redis 存 code_<email>=验证码(180s)
                       └────────► 163邮箱发送验证码

2. 用户注册
   客户端 ──HTTP──► GateServer(/user_register)
                       │ 查 MySQL(邮箱/用户名是否重复)
                       │ 比对 Redis 里的验证码
                       └── 写入 MySQL

3. 用户登录
   客户端 ──HTTP──► GateServer(/user_login)
                       │ 查 MySQL(密码正确?)
                       │ gRPC GetChatServer(uid)
                       ▼
                  StatusServer ──► 选负载最低的 ChatServer、生成 token
                       └────────► 返回 {host, port, token}
   客户端拿到 host/port 后发起 TCP 长连接

4. 客户端登录聊天服务器
   客户端 ──TCP──► ChatServer
                       │ 解析 MSG_CHAT_LOGIN(uid, token)
                       │ gRPC Login(uid, token)
                       ▼
                  StatusServer ──► token 匹配则返回 Success
                       └────────► ChatServer 回包 MSG_CHAT_LOGIN_RSP
```

token 在这里的作用：GateServer 登录时由 StatusServer 签发，ChatServer 再向 StatusServer 校验，**保证只有经过 HTTP 网关登录的用户才能建立 TCP 长连接**。

---

## 7. CMake 构建配置

```cmake
# 顶层只生成一次根目录 proto/message.proto
add_custom_target(proto_codegen DEPENDS ${PROTO_GENERATED_FILES})
add_library(chat_proto STATIC ${PROTO_GENERATED_FILES})
add_dependencies(chat_proto proto_codegen)

# 子服务只链接公共协议库
target_link_libraries(${PROJECT_NAME} chat_proto ...)
```

构建依赖关系如下：

```text
proto/message.proto
        |
        v
proto_codegen -> chat_proto（只编译一次）
        |
        +--> GateServer
        +--> StatusServer
        +--> ChatServer1
        +--> ChatServer2
```

这样只有协议源文件发生变化时才会重新运行 `protoc`；普通增量编译不会重复生成或重复编译协议代码。

---

## 8. 要点与踩坑总结

1. **.proto 是"合同"**：服务端和客户端必须用同一份 proto 生成代码，字段编号（`= 1`、`= 2`）不能随意改，否则序列化错位。
2. **包名决定命名空间**：`package message;` 让生成代码都在 `message::` 命名空间下，客户端 `using message::GetVarifyRsp` 等。
3. **判断调用结果看 `status.ok()`**：网络层失败（连接不上、超时）返回非 OK；业务失败（验证码重复、token 无效）是 `Status::OK` + 业务 error 字段，两者要分清。
4. **Stub 用完必须归还连接池**：项目里用 `Defer` 或显式 `ReleaseConnection(std::move(stub))`，漏归还会导致连接池慢慢耗尽。
5. **跨语言版本**：C++ 用 gRPC 1.x，Node.js 用 `@grpc/grpc-js`（纯 JS 实现，不需要原生编译），只要 proto 一致就能互通。
6. **同步调用的位置**：本项目客户端都是同步一元调用，放在 LogicSystem 的业务线程里，不阻塞 IO 线程。
7. **统一协议源**：协议文件放在根目录的 `proto/message.proto`，C++ 端通过公共 `chat_proto` 静态库复用生成代码；Node.js 端通过相对路径加载同一份文件，避免各服务协议副本不一致。
8. **后续扩展**：多台 ChatServer 之间还需要互相通信（跨服转发好友申请、聊天消息、踢人下线），参考项目在后续开发中加了 `ChatService`（`ChatGrpcClient` + `ChatServiceImpl`），让 ChatServer 也同时扮演 gRPC 服务端——模式与 StatusServer 完全一致。
