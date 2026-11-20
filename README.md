# chat_app

Windows 从零安装、Docker Compose 启动 MySQL 和 Redis、编译和运行步骤见 [环境部署](环境部署.md)。自动化恢复见 [环境部署（Agent）](环境部署-Agent.md)。

## 服务与通信

| 组件 | 默认端口 | 职责 |
| --- | --- | --- |
| Qt 6 桌面客户端 | — | 账号界面、聊天与群管理、SQLite 缓存、资源传输、好友音视频 |
| GateServer | HTTP 8080 | 注册、登录、重置密码，调用验证码与选服服务 |
| VarifyServer | gRPC 50051 | 邮箱验证码、Redis 有效期、SMTP 邮件 |
| StatusServer | gRPC 50052 | 根据在线计数选择聊天实例，签发和校验 token |
| ChatServer1 | TCP 8090 / gRPC 50055 | 长连接、好友、私聊、群聊、通话信令 |
| ChatServer2 | TCP 8091 / gRPC 50056 | 独立聊天实例、跨服通知 |
| ResourceServer | TCP 9090 / gRPC 50057 | 分片上传下载、资源核验、下载鉴权 |
| MySQL 8 | X Protocol 33060 | 用户、社交关系、会话、消息、群管理事务 |
| Redis | 6379 | 验证码、token、在线路由、在线计数、资料缓存和锁 |

```mermaid
flowchart LR
    Qt[Qt 客户端] -->|HTTP| Gate[GateServer]
    Gate -->|gRPC| Verify[VarifyServer]
    Verify --> SMTP
    Gate -->|gRPC| Status[StatusServer]
    Qt -->|TCP| Chat[ChatServer1 / ChatServer2]
    Chat <-->|gRPC 跨服通知| Peer[其他聊天实例]
    Qt -->|资源 TCP| Resource[ResourceServer]
    Chat -->|gRPC 核验| Resource
    Resource -->|gRPC 认证| Status
    Chat --> MySQL
    Resource --> MySQL
    Gate --> MySQL
    Chat --> Redis
    Status --> Redis
    Verify --> Redis
```

客户端先通过网关验证账号密码，再使用返回的 `host/port/token` 直接建立聊天 TCP 连接。资源连接和媒体连接独立于聊天连接。各端点都必须对客户端可达。

## 功能与数据语义

- 私聊支持文本、图片和普通文件。消息先持久化，再进行同服推送或跨服 gRPC 转发；离线记录通过历史接口恢复。
- 消息区分保存、实时投递、展示 ACK 和已读状态。服务端 `message_id` 用于正式历史及缓存去重，客户端 `msgid` 关联待确认消息；重复发送文本请求仍可能新增正式记录。
- 群聊使用 `thread_id` 作为群号，支持纯文本消息、邀请与申请、审批、管理员、群主转让、公告和群头像。离群、踢出和解散保留历史，客户端展示只读状态。
- 左侧搜索框提供创建群、按完整群号查找及加入入口；群通知处理邀请、申请和结果；群信息窗口处理成员、角色、公告和头像。
- 好友一对一音视频采用 Qt WebEngine、QWebChannel 和 ZLMediaKit；固定 WebM 文件的 **test** 入口独立保留。设备采集在接听后启动。

## 目录与代码入口

| 目录或文件 | 内容 |
| --- | --- |
| `chat_app desktop/` | Qt Widgets 客户端与媒体页面 |
| `GateServer/`、`StatusServer/`、`ChatServer1/`、`ChatServer2/`、`ResourceServer/` | C++ 服务 |
| `VarifyServer/` | Node.js 验证码服务 |
| `proto/message.proto` | 共用 protobuf/gRPC 契约 |
| `sql/` | MySQL 建表、增量迁移与 SQLite 结构参考 |
| `tests/` | 协议、数据库事务、SQLite、资源和容量测试 |
| `deploy/` | [MySQL 和 Redis 的 Compose 配置](deploy/compose.yaml)、ZLMediaKit 启动脚本和配置样例 |
| `DistributeLock/` | 可独立构建的 hiredis 分布式锁组件 |
| `note/` | 按主题组织的知识文档 |

## 数据库初始化与迁移

全新环境按 [环境部署：用 Docker Compose 启动 MySQL 和 Redis](环境部署.md#3-用-docker-compose-启动-mysql-和-redis) 启动，[sql/create_tables.sql](sql/create_tables.sql) 会在空 MySQL 数据卷上自动执行。脚本包含用户、好友、会话、消息、群资料、成员、准入和业务操作记录。`CREATE TABLE IF NOT EXISTS` 只补建缺表，不能升级已有表。

| 已有结构 | 对应脚本与前提 |
| --- | --- |
| 缺少聊天表 | `sql/chat_message_storage.sql` |
| 消息缺少图片或文件字段 | `sql/chat_image_message_migration.sql` |
| 消息缺少展示确认字段 | `sql/chat_message_receipt_migration.sql` |
| 早期群表缺少 `owner_uid/active` | `sql/chat_group_migration.sql`；执行前停写并检查角色转换 |
| 已有 `owner_uid/active` 的群管理结构升级 | `sql/chat_group_management_migration.sql`；保留角色与历史，随新版服务部署 |
| 隔离测试库 | `sql/chat_app_test_local.sql`；仅用于测试 schema |

迁移前确认源结构、备份与停写窗口。旧群迁移会重置管理员角色，已完成该阶段的数据库直接使用对应管理增量脚本。详细约束见 [数据库设计](note/数据库设计.md)。

## 网关接口

| HTTP 方法 | 路径 | 作用 |
| --- | --- | --- |
| GET | `/get_test` | 网关连通性 |
| POST | `/get_varifycode` | 发送邮箱验证码 |
| POST | `/user_register` | 注册 |
| POST | `/user_login` | 验证账号并取得聊天地址与 token |
| POST | `/user_resetpassword` | 凭邮箱验证码重置密码 |

启动命令及客户端连通性检查见 [环境部署：启动和检查](环境部署.md#6-启动和检查)。

## 测试入口

```powershell
.\run_tests.ps1
.\run_tests.ps1 -Tests tcp,sqlite
```

脚本调用 CTest，不负责重新构建。数据库用例需要显式隔离 schema 与环境配置；缺少前提可能跳过。群事务、资源 smoke 等入口及其权限要求见 [tests/README.md](tests/README.md)，测试分层与断言含义见 [测试](note/测试.md)。

## 知识阅读路径

| 主题 | 文档 |
| --- | --- |
| 服务架构与一致性 | [全栈聊天室](note/全栈聊天室.md)、[分布式聊天服务设计](note/分布式聊天服务设计.md)、[系统设计问题](note/项目总结.md) |
| 登录与传输 | [登录](note/login.md)、[TCP 服务器](note/TCP服务器.md)、[消息 ID](note/消息ID全链路.md)、[聊天收发](note/聊天信息收发.md) |
| 数据与群管理 | [数据库设计](note/数据库设计.md)、[消息存储](note/聊天信息存储方案.md)、[群聊](note/群聊.md)、[群聊事务](note/总结群聊.md) |
| 资源与可靠性 | [资源服务](note/资源服务.md)、[图片](note/图片传输.md)、[文件](note/文件传输.md)、[可靠性与资源安全](note/聊天可靠性与资源安全提升.md) |
| 并发与生命周期 | [连接池](note/连接池.md)、[分布式锁](note/分布式锁设计.md)、[锁粒度](note/锁的精度控制.md)、[异常处理](note/异常处理.md)、[心跳](note/心跳检测.md) |
| Qt 与音视频 | [Qt 知识](note/Qt知识点.md)、[Qt 网络](note/Qt网络编程.md)、[样式表](note/样式表.md)、[头像](note/Qt头像统一显示.md)、[WebRTC](note/WebRTC视频通信.md) |
| 依赖与常见问题 | [MySQL](note/MySQL连接库.md)、[Redis](note/Redis.md)、[gRPC](note/grpc.md)、[JSON](note/json.md)、[常见问题](note/疑惑.md) |
