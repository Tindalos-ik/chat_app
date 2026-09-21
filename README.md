# chat_app

基于 C++ 的分布式即时聊天项目，包含 Qt 6 桌面客户端、HTTP 网关、状态服务、两个 TCP 聊天服务实例、资源服务和 Node.js 邮箱验证码服务。

项目提供注册、邮箱验证码、登录、找回密码、好友申请/认证、好友列表加载、同账号重复登录互踢、客户端与 ChatServer 的应用层心跳检测，以及在线文本消息的同服/跨服转发。好友认证和 ChatServer1 的普通文本消息都会创建或复用私聊并持久化到 MySQL；桌面端以 SQLite 缓存会话、实时通知和登录后的增量历史。跨 ChatServer 的文本转发字段同步由部署方维护，详见 `note/聊天信息存储方案.md`。

## 架构

```text
Qt 桌面客户端
  | HTTP 8080
  v
GateServer ---------------------- gRPC 50051 ---> VarifyServer ---> SMTP / Redis
  | gRPC 50052
  v
StatusServer -------------------- MySQL / Redis
  | 选择负载较低的 ChatServer，签发登录 token
  +-------------------------------+
  |                               |
  v TCP 8090 / gRPC 50055         v TCP 8091 / gRPC 50056
ChatServer1 <----- gRPC -------> ChatServer2
  |                               |
  +----------- MySQL / Redis -----+

Qt 桌面客户端 ---- TCP 9090 ----> ResourceServer ----> uploads
```

| 组件 | 技术 | 端口 | 职责 |
| --- | --- | ---: | --- |
| GateServer | C++、Boost.Beast、gRPC | 8080 | HTTP API、账号校验、验证码和登录网关 |
| VarifyServer | Node.js、gRPC | 50051 | 生成验证码并通过 SMTP 发送邮件 |
| StatusServer | C++、gRPC | 50052 | ChatServer 负载选择、token 签发和校验 |
| ChatServer1 | C++、Boost.Asio、gRPC | TCP 8090 / gRPC 50055 | 长连接、会话和消息路由 |
| ChatServer2 | C++、Boost.Asio、gRPC | TCP 8091 / gRPC 50056 | 第二个聊天服务实例、跨服转发 |
| ResourceServer | C++、Boost.Asio | TCP 9090 | 图片分片上传、断点续传和资源地址发布 |
| MySQL | MySQL 8 | X Protocol 33060 | 用户、好友和好友申请数据 |
| Redis | Redis | 6379 | 验证码、token、在线路由和服务负载 |

## 目录

```text
chat_app/
├── ChatServer1/          # 第一台 TCP/gRPC 聊天服务
├── ChatServer2/          # 第二台 TCP/gRPC 聊天服务
├── GateServer/           # HTTP 网关
├── StatusServer/         # 登录状态与负载均衡服务
├── ResourceServer/       # 图片分片上传与断点续传服务
├── VarifyServer/         # Node.js 邮箱验证码服务
├── chat_app desktop/     # Qt 6 桌面客户端
├── proto/message.proto   # 所有服务共用的 protobuf/gRPC 协议
├── sql/create_tables.sql # MySQL 建库建表脚本
├── start_all.bat         # Windows 后端一键启动脚本
└── note/                 # 设计和实现笔记
```

## 环境要求

项目当前的开发配置面向 Windows 10/11。

| 工具 | 要求 |
| --- | --- |
| Visual Studio 2022 | 安装“使用 C++ 的桌面开发”工作负载 |
| CMake | 3.21 或更高版本 |
| vcpkg | 安装 C++ 依赖，默认路径为 `D:/cppsoft/vcpkg` |
| Boost | 1.91，默认路径为 `C:/local/boost_1_91_0` |
| MySQL | 8.x，启用 X Plugin（默认端口 33060） |
| Redis | 任意较新版本 |
| Node.js | 18 或更高版本 |
| Qt | 6.8 或兼容 Qt 6 版本，用于桌面客户端 |

安装 vcpkg 依赖：

```powershell
git clone https://github.com/microsoft/vcpkg.git D:/cppsoft/vcpkg
Set-Location D:/cppsoft/vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg install grpc protobuf redis-plus-plus mysqlcppconnx --triplet x64-windows
```

> `CMakePresets.json` 和各服务的 `CMakeLists.txt` 使用了上述 vcpkg 与 Boost 路径。安装在其他位置时，请先统一更新这些路径再配置工程。

## 初次配置

### 1. 初始化数据库

启动 MySQL，并执行仓库内的建表脚本：

```powershell
mysql -uroot -p < sql/create_tables.sql
```

脚本会创建 `chat_app_db`，以及 `user`、`user_id`、`friend`、`friend_apply` 四张表。

> 重复执行脚本前请留意 `user_id` 的初始插入语句。已有数据时不应重复插入该分配器记录。

### 2. 初始化聊天会话表

私聊会话创建（TCP `1027/1028` 的服务端入口）依赖额外的会话表。需要启用该能力时，在完成基础建表后执行：

```powershell
mysql -uroot -p < sql/chat_message_storage.sql
```

脚本创建 `chat_thread`、`private_chat`、群聊相关表和 `chat_message`。ChatServer1 已使用私聊表创建或复用会话，并将好友认证和普通文本消息写入 `chat_message`；桌面端通过 `1025/1026` 发现会话、通过 `1029/1030` 分页增量加载历史。

### 3. 配置服务端连接信息

按本机 MySQL 和 Redis 的实际账号修改下列文件中的 `[Mysql]`、`[Redis]`：

- `GateServer/config.ini`
- `StatusServer/config.ini`
- `ChatServer1/config.ini`
- `ChatServer2/config.ini`

这四份配置应使用同一套数据库、Redis 地址和密码。默认 MySQL 端口是 X DevAPI 的 `33060`，不是传统 MySQL 协议端口 `3306`。

资源服务监听 `ResourceServer/config.ini` 中的 9090 端口，桌面客户端
`chat_app desktop/config.ini` 的 `[ResourceServer]` 必须指向同一个可达地址。
上传完成后，客户端把 ResourceServer 返回的 `resource_url` 作为头像字段，
通过当前已登录的 ChatServer 更新用户资料。

聊天服务的配置还必须保持互相匹配：

| 实例 | TCP 端口 | gRPC 端口 | 服务名 |
| --- | ---: | ---: | --- |
| ChatServer1 | 8090 | 50055 | `ChatServer1` |
| ChatServer2 | 8091 | 50056 | `ChatServer2` |

`StatusServer/config.ini` 的 `[ChatServers]`、每台 ChatServer 的 `[SelfChatServer]` 和 `[PeerServer]` 共同定义这套拓扑。若修改端口、主机或名称，需要同步更新三份相关配置。

### 4. 配置验证码服务

```powershell
Set-Location VarifyServer
Copy-Item config.example.json config.json
npm install
```

编辑 `VarifyServer/config.json`，填写可用的 SMTP 邮箱和授权码，并确认其中 Redis 配置可用。该文件含敏感信息，已被 `.gitignore` 排除。

> `config.example.json` 中 MySQL 端口为 `3306`。若验证码服务后续需要连接本项目的 MySQL X DevAPI，请按实际 MySQL 配置调整；当前验证码发送流程依赖 SMTP 与 Redis。

### 5. 配置客户端网关地址

桌面客户端从 `chat_app desktop/config.ini` 的 `[GateServer]` 读取 HTTP 网关地址。默认值为 `localhost:8080`；客户端与服务端不在同一台机器时，改为可访问的网关主机名或 IP。

## 构建

在仓库根目录配置并构建五个 C++ 服务：

```powershell
Set-Location D:\myproject\chat_app
cmake --preset windows-vcpkg
cmake --build --preset debug
```

Debug 可执行文件会生成在：

```text
build\GateServer\Debug\GateServer.exe
build\StatusServer\Debug\StatusServer.exe
build\ChatServer1\Debug\ChatServer1.exe
build\ChatServer2\Debug\ChatServer2.exe
build\ResourceServer\Debug\ResourceServer.exe
```

根目录 CMake 会从 `proto/message.proto` 自动生成 protobuf/gRPC 代码。各服务的 `config.ini` 也会被复制到对应 Debug 目录；服务必须从可执行文件所在目录启动，才能读取这份运行时配置。

桌面客户端使用 Qt Creator 打开 `chat_app desktop/CMakeLists.txt`，选择带 Qt SQL 模块的 Qt 6 kit 后构建运行。SQLite 使用 Qt 自带的 `QSQLITE` 驱动，不需要安装或启动独立数据库服务。

## 启动

启动前确认 MySQL 与 Redis 均已运行。推荐顺序是：

```text
Redis -> VarifyServer -> StatusServer -> ChatServer1 -> ChatServer2 -> ResourceServer -> GateServer
```

### 一键启动（Windows）

`start_all.bat` 会分别打开 Redis 和六个服务窗口。脚本中的 `ROOT`、`REDIS` 与 `CFG` 是本机路径和构建配置，首次使用前请检查它们：

```powershell
Set-Location D:\myproject\chat_app
.\start_all.bat
```

### 手动启动

```powershell
Set-Location D:\myproject\chat_app\VarifyServer
npm run serve
```

在另外四个终端中执行：

```powershell
Set-Location D:\myproject\chat_app\build\StatusServer\Debug
.\StatusServer.exe
```

```powershell
Set-Location D:\myproject\chat_app\build\ChatServer1\Debug
.\ChatServer1.exe
```

```powershell
Set-Location D:\myproject\chat_app\build\ChatServer2\Debug
.\ChatServer2.exe
```

```powershell
Set-Location D:\myproject\chat_app\build\GateServer\Debug
.\GateServer.exe
```

## 验证

所有服务启动后，可以先检查网关：

```powershell
curl http://127.0.0.1:8080/get_test
```

网关目前提供以下 HTTP 接口：

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| GET | `/get_test` | 网关连通性测试 |
| POST | `/get_varifycode` | 发送邮箱验证码 |
| POST | `/user_register` | 注册账号 |
| POST | `/user_login` | 登录并获取 ChatServer 地址与 token |
| POST | `/user_resetpassword` | 验证邮箱验证码后重置密码 |

之后启动 Qt 客户端，完成注册和登录。StatusServer 将根据 Redis 中的在线计数选择 ChatServer，客户端再建立到该服务的 TCP 长连接。

## 常见问题

| 现象 | 排查方向 |
| --- | --- |
| CMake 找不到 gRPC、Protobuf、Redis 或 MySQL Connector | 检查 vcpkg 安装、`CMAKE_TOOLCHAIN_FILE` 和各 CMake 文件中的 vcpkg 路径 |
| 找不到 Boost 头文件 | 检查 `C:/local/boost_1_91_0`，或同步更新各服务 CMake 的 include 路径 |
| 服务启动提示 `Config file not found` | 从对应的 `build/<Service>/Debug` 目录启动，确认 `config.ini` 已复制过去 |
| 收不到验证码 | 检查 `VarifyServer/config.json` 的邮箱授权码、SMTP 配置和 Redis 连通性 |
| 登录后无法连接聊天服务 | 检查 StatusServer 与两个 ChatServer 的主机/端口/服务名配置是否一致；跨机器部署时不要把返回给客户端的地址设为 `127.0.0.1` |
| 跨服消息未送达 | 检查 50055、50056 是否可互通，以及每台 ChatServer 的 `[PeerServer]` 配置 |
| 异地登录未踢掉旧端 | 检查 Redis 的 `uip_<uid>` 是否指向真实旧服、对端 `rpcport` 是否可达，以及登录回包是否为 `RPCFaild` |

## 相关文档

- [心跳检测](note/心跳检测.md)：1023/1024 协议、超时参数、统一清理及复测步骤。
- [异常处理](note/异常处理.md)：网络异常、异步回调生命周期与 ChatServer 停止顺序。

- [聊天信息收发](note/聊天信息收发.md)：当前文本聊天的完整 TCP、Redis 路由和 gRPC 跨服转发链路。
- [登录全链路](note/login.md)：注册、登录与 token 校验流程。
- [分布式聊天服务设计](note/分布式聊天服务设计.md)：服务拆分和负载均衡设计。
- [服务部署](note/服务部署.md)：Windows/Linux 部署笔记。
- [数据库设计](note/数据库设计.md)：MySQL 表结构说明。
- [聊天信息存储方案](note/聊天信息存储方案.md)：服务端会话/消息模型、客户端 SQLite 缓存和增量同步边界。
