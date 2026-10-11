# chat_app

基于 C++ 的分布式即时聊天项目，包含 Qt 6 桌面客户端、HTTP 网关、状态服务、两个 TCP 聊天服务实例、资源服务和 Node.js 邮箱验证码服务。

项目提供注册、登录、好友、重复登录互踢、心跳，以及私聊文本/图片/文件和纯文本群聊。群号即uint64 thread_id，群管理支持好友邀请明确同意、审批、角色0/1/2、原子转让、离群和版本公告（正文仅有效成员可读）；成员失效只影响该群，不注销账号。群头像支持专用认证上传、用户token与配置服务凭据核验、可信ID/独立avatar_version发布及当前成员每片下载。群正文不扩展附件。管理、真实资源安全与Qt UI证据分别记录，数据层通过不能代替网络或视觉验收。

Qt 群菜单提供创建、搜索加入和邀请/申请信箱；群信息窗口提供成员分页、好友邀请、审批、管理员任免、转让、离群/解散、完整公告编辑和头像选择。按钮按当前角色与成员版本开放，最终仍由服务端事务鉴权；退出/踢出/解散保留本地会话历史，列表和标题显示对应只读状态。

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

Qt 桌面客户端 ---- TCP 9090（分片上传/下载；下载带 uid/token/thread_id）----> ResourceServer ----> uploads
ChatServer1/2 -------- gRPC 50057 ----^（图片与普通文件资源核验）
ResourceServer -------- gRPC 50052 / MySQL ----> StatusServer / private_chat 鉴权
```

| 组件 | 技术 | 端口 | 职责 |
| --- | --- | ---: | --- |
| GateServer | C++、Boost.Beast、gRPC | 8080 | HTTP API、账号校验、验证码和登录网关 |
| VarifyServer | Node.js、gRPC | 50051 | 生成验证码并通过 SMTP 发送邮件 |
| StatusServer | C++、gRPC | 50052 | ChatServer 负载选择、token 签发和校验 |
| ChatServer1 | C++、Boost.Asio、gRPC | TCP 8090 / gRPC 50055 | 长连接、会话和消息路由 |
| ChatServer2 | C++、Boost.Asio、gRPC | TCP 8091 / gRPC 50056 | 第二个聊天服务实例、跨服转发 |
| ResourceServer | C++、Boost.Asio、gRPC | TCP 9090 / gRPC 50057 | 通用资源分片上传、断点续传和下载；1007 校验登录 token、私聊成员关系与资源归属；以及 ChatServer 的图片/文件资源核验 |
| MySQL | MySQL 8 | X Protocol 33060 | 用户、好友、会话和消息数据 |
| Redis | Redis | 6379 | 验证码、token、在线路由和服务负载 |

## 目录

```text
chat_app/
├── ChatServer1/          # 第一台 TCP/gRPC 聊天服务
├── ChatServer2/          # 第二台 TCP/gRPC 聊天服务
├── GateServer/           # HTTP 网关
├── StatusServer/         # 登录状态与负载均衡服务
├── ResourceServer/       # 图片和文件分片上传、断点续传与下载服务
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
.\vcpkg install grpc protobuf redis-plus-plus mysqlcppconnx libpng libjpeg-turbo --triplet x64-windows
```

> `CMakePresets.json` 和各服务的 `CMakeLists.txt` 使用了上述 vcpkg 与 Boost 路径。安装在其他位置时，请先统一更新这些路径再配置工程。

## 初次配置

### 1. 初始化数据库

启动 MySQL，并通过 MySQL 传统协议端口（默认 `3306`）执行全量建库脚本：

```powershell
mysql --protocol=tcp -h 127.0.0.1 -P 3306 -uroot -p < sql/create_tables.sql
```

`sql/create_tables.sql` 是全新环境唯一需要执行的 MySQL 建表入口。除用户、好友、会话、成员和消息表，还定义持久化准入记录 `group_admission` 与业务重放结果 `group_operation`。群元数据含版本、审批开关、公告和头像资源字段；成员保留 `active/membership_version`，角色 CHECK 允许0普通成员、1管理员、2群主。脚本只会在 `user_id` 为空时初始化分配器。

项目服务通过 MySQL X DevAPI 使用 `33060` 端口；上面的 `mysql` 命令行客户端使用传统协议端口 `3306`。`sql/chat_message_storage.sql` 保留给旧版分步初始化流程，只创建缺失的聊天表，不会升级已有表结构；新库不需要再执行它。

已有数据库若尚无聊天表，可执行 `sql/chat_message_storage.sql` 补建当前结构；已有旧消息表按需执行图片/文件与回执迁移。已有旧群表必须停写备份后执行一次 `sql/chat_group_migration.sql`，增加 owner_uid/active、统一角色0/2及 RESTRICT 外键，再部署新版 ChatServer。重跑 CREATE TABLE IF NOT EXISTS 不会升级已有表。解散仅active=0，历史不删除；群消息每条只写一行recv_id=0，提交后按成员UID扇出，实时失败不等于保存失败。

群管理结构升级使用新增的 `sql/chat_group_management_migration.sql`：对已具有 owner_uid/active 的旧群库停写、备份后执行一次，保留全部角色和历史。不重跑 `chat_group_migration.sql`，该旧脚本会把管理员重置为普通成员。此增量结构与旧 ChatServer 的删除成员行为不兼容，必须与新版服务同步部署；协议定义不等于七项业务已经运行验收。

### 2. 配置服务端连接信息

按本机 MySQL 和 Redis 的实际账号修改下列文件中的 `[Mysql]`、`[Redis]`：

- `GateServer/config.ini`
- `StatusServer/config.ini`
- `ChatServer1/config.ini`
- `ChatServer2/config.ini`

这四份配置应使用同一套数据库、Redis 地址和密码。默认 MySQL 端口是 X DevAPI 的 `33060`，不是传统 MySQL 协议端口 `3306`。

资源服务监听 `ResourceServer/config.ini` 中的 9090 端口，桌面客户端
`chat_app desktop/config.ini` 的 `[ResourceServer]` 必须指向同一个可达地址。
ResourceServer 的 `[StatusServer]` 与 `[Mysql]` 也必须与 ChatServer 指向同一套登录服务和
聊天库；图片下载会校验当前 token、私聊成员关系及 `resource_id` 对该 `thread_id` 的归属。
上传完成后，客户端把 ResourceServer 返回的 `resource_url` 作为头像字段，
通过当前已登录的 ChatServer 更新用户资料。

聊天服务的配置还必须保持互相匹配：

| 实例 | TCP 端口 | gRPC 端口 | 服务名 |
| --- | ---: | ---: | --- |
| ChatServer1 | 8090 | 50055 | `ChatServer1` |
| ChatServer2 | 8091 | 50056 | `ChatServer2` |

`StatusServer/config.ini` 的 `[ChatServers]`、每台 ChatServer 的 `[SelfChatServer]` 和 `[PeerServer]` 共同定义这套拓扑。若修改端口、主机或名称，需要同步更新三份相关配置。

### 3. 配置验证码服务

```powershell
Set-Location VarifyServer
Copy-Item config.example.json config.json
npm install
```

编辑 `VarifyServer/config.json`，填写可用的 SMTP 邮箱和授权码，并确认其中 Redis 配置可用。该文件含敏感信息，已被 `.gitignore` 排除。

> `config.example.json` 中 MySQL 端口为 `3306`。若验证码服务后续需要连接本项目的 MySQL X DevAPI，请按实际 MySQL 配置调整；当前验证码发送流程依赖 SMTP 与 Redis。

### 4. 配置客户端网关地址

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

## 自动化测试

根目录 `CMakeLists.txt` 已通过 `add_subdirectory(tests)` 接入所有测试，`include(CTest)` 默认打开 `BUILD_TESTING`。先在根目录配置并执行一次默认构建，测试目标就会随项目一起编译：

```powershell
cmake --preset windows-vcpkg
cmake --build --preset debug
```

之后运行 `run_tests.ps1` 逐项选择测试。脚本只调用 CTest，不会重新配置 CMake 或编译目标：

```powershell
.\run_tests.ps1
```

提示时输入 `y` 运行、回车跳过。也可以用 `-Tests tcp,sqlite` 直接指定测试；可选项为 `tcp`、`friend`、`message`、`sqlite` 和 `capacity`。连接容量测试需要 ChatServer 正在运行。好友和消息用例只在显式配置独立测试库时执行；未配置时会跳过，避免修改开发数据。SQLite 用例使用 Qt 6.5+ Core、Sql、Widgets 和 QSQLITE 插件；Windows 上根构建会从桌面构建缓存读取 Qt kit 与编译器，并在默认构建过程中完成独立子构建，避免混用 MSVC 和 MinGW。CI 使用独立 MySQL schema 运行数据库用例。具体覆盖范围、测试库配置及容量测试方式见 [测试笔记](note/测试.md)。

群事务测试为 `group_transaction_test`，随CMake注册编译；从显式隔离config.ini工作目录运行，并设置 `CHAT_GROUP_TEST_SCHEMA=chat_app_test_*` 与 `CHAT_GROUP_TEST_CONFIRM_ISOLATED=YES`，不使用开发库。隔离双实例实际验证分三层记录：数据库好友/私聊/群事务用例1+3+2；真实HTTP/TCP/gRPC群链路；Qt生产Widgets实际登录、菜单/确认窗与群页面操作。Qt已观察创建/精确搜索/幂等加入、非好友跨服文本、成员分页、离线历史、踢人/解散只读、旧群号发现、超过100身份完整对账、未读摘要保留及同进程切账号；外部可复现脚本、stdout、实际窗口PNG和包序/SQLite读出见交付报告。SQLite匹配Qt kit的独立子构建实际11/11通过；不据此声称根CTest全套或客户端半包测试通过。详见测试笔记。


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
| 离线图片不显示、文件已读状态不刷新 | 检查旧库 `chat_message` 是否有 `displayed_at` 列；缺列时历史消息查询会失败。执行一次 `sql/chat_message_receipt_migration.sql` 后重新登录以触发增量同步 |
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
- [总结群聊](note/总结群聊.md)：从创建、加入到跨服文本、离线恢复和群失效，结合后端与Qt/SQLite真实源码讲解权限、身份及游标边界。
- [资源服务](note/资源服务.md)：资源上传、资源 ID、gRPC 图片核验与 TCP 分片下载协议。
- [文件传输](note/文件传输.md)：私聊文件消息、手动下载和资源归属鉴权。
- [视频通话技术方案与学习路径](note/视频通话技术.md)：拟接入的一对一音视频方案、Qt kit 约束、信令鉴权、coturn 与分阶段验收；不代表当前已支持通话。
