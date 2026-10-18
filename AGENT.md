# AGENT.md — 本项目 AI 助手行为规范

> 本文件只写行为规范：在本仓库工作时该怎么做、不该怎么做。

> 项目知识、常见疑惑解答在 `note/` 目录，不要往这里堆知识点。

---

## 工作原则

1. 动手改代码前，先读 README.md 与本文件；涉及具体模块时，再读 note/ 里对应的笔记。
2. 用户问过的小疑惑、新学到的知识点，写入 note/（新建或补充现有 md），并在文末索引登记。
3. 不执行破坏性命令（rm、reset、清库等）；确需执行必须先征得同意。
4. 后续实现或调整任何业务时，必须同步更新相关文档：至少检查对应专题笔记、README/架构总览、协议或消息 ID 状态表及 AGENT.md 文档索引；删除或改写与当前代码冲突的“未实现、占位、后续再做”等旧描述，保证文档记录的是当前真实行为、失败语义与已知边界，不能只在旧文档末尾追加新说明。
5. 功能完成后，应将它从“待办、问题、缺口、后续计划”等列表中直接删除，不保留“已实现、已完成、已经接入”等完成打卡记录；需要长期保留的协议和设计内容，直接描述当前行为。
6.gitcommit使用中文描述
7.测试不需要隔离，不要随意新建测试数据库，直接在原有的基础上动，如果是临时测试，修改完复原即可

## 代码规范（必须遵守）

新增功能，头文件要清楚注释，文件功能，函数功能，参数等，源文件也要写好注释和逻辑

减少防御性编程，和非必要的测试

## 文档规范

* 文档风格：中文、叙述式、带"为什么"，表格 + 代码块 + `>` 提示，参考 note/ 现有笔记
* 笔记风格参考桌面计算机笔记（`C:\Users\马煜坤\Desktop\计算机笔记`）：小标题分层、列表与代码块为主、口语化讲解，笔记简约，不需要太多粗笔
* 本次新增文档中的协议消息编号用语义名称表达，不写数字编号
* 新文档放 note/，文件名用中文；写完后更新 AGENT.md 文末索引。
* 文档中不要出现“未实现、占位、后续再做”等描述，只写当前行为；需要长期保留的协议和设计内容，直接描述当前行为。
* 不要使用mermaid画图，如果需要画图，请在别的地方画好，然后直接截图
* 文档中不要出现“未实现、占位、后续再做”等描述，只写当前行为；需要长期保留的协议和设计内容，直接描述当前行为。
* 文档不要出现 不是A而是B 的表述，直接写 A

## 文档索引

| 文件 | 内容 |
| --- | --- |
| [sql/create_tables.sql](sql/create_tables.sql) | 全新环境完整数据库建表脚本（含用户、好友、会话、消息和回执表） |
| [sql/chat_app_test_local.sql](sql/chat_app_test_local.sql) | 本机集成测试 schema 的完整建库建表脚本 |
| [sql/chat_message_storage.sql](sql/chat_message_storage.sql) | 旧版分步初始化的聊天表兼容脚本；新环境执行统一建表脚本即可 |
| [sql/chat_image_message_migration.sql](sql/chat_image_message_migration.sql) | 已部署 chat_message 的图片/文件资源字段和消息类型迁移脚本 |
| [sql/chat_message_receipt_migration.sql](sql/chat_message_receipt_migration.sql) | 已部署 chat_message 的显示确认字段迁移脚本 |
| [sql/chat_group_migration.sql](sql/chat_group_migration.sql) | 已部署群表 owner_uid/active、角色0/2与 RESTRICT 外键迁移 |
| [sql/chat_group_management_migration.sql](sql/chat_group_management_migration.sql) | 已部署群管理结构增量升级：保留管理员/历史，增加版本、准入与业务重放结果 |
| [疑惑.md](note/疑惑.md) | 常见小知识点解答（uid/token、分布式、Qt 并发安全、MySQL/Redis 边界等） |
| [全栈聊天室.md](note/全栈聊天室.md) | 架构总览 |
| [分布式聊天服务设计.md](note/分布式聊天服务设计.md) | 分布式设计总结：服务拆分、双通道登录、负载均衡/在线路由/跨服转发 |
| [login.md](note/login.md) | 登录全链路 |
| [TCP服务器.md](note/TCP服务器.md) | Asio TCP 服务器、粘包处理 |
| [连接池.md](note/连接池.md) | 项目中 4 类连接池 |
| [grpc.md](note/grpc.md) | gRPC 应用 |
| [测试.md](note/测试.md) | CTest好友/私聊/群事务、SQLite直接子构建11项、真实QtWidgets群/私聊资源与完整快照race隔离验证及CI边界 |
| [数据库设计.md](note/数据库设计.md) | MySQL 用户、社交、会话与消息表，以及客户端 SQLite 缓存设计 |
| [聊天信息存储方案.md](note/聊天信息存储方案.md) | 服务端会话/消息表、客户端 SQLite 缓存管理器与增量同步设计 |
| [聊天可靠性与资源安全提升.md](note/聊天可靠性与资源安全提升.md) | 消息 ACK、已读与未读、历史分包、图片下载鉴权和故障恢复边界 |
| [聊天服务器服务.md](note/聊天服务器服务.md) | proto 中定义的 ChatServer gRPC 服务、消息字段、群消息与好友音视频跨服转发约定及服务端权威通话类型 |
| [聊天信息收发.md](note/聊天信息收发.md) | 私聊与纯文本群聊先持久化后投递、群行锁授权、独立群失效通知与帧边界 |
| [群聊.md](note/群聊.md) | 群聊知识：数据模型与索引、入群和角色管理、事务与业务重放、消息投递、公告头像及 SQLite 同步 |
| [服务器踢人.md](note/服务器踢人.md) | 同服/跨服重复登录互踢、1021 投递、会话清理与当前边界 |
| [心跳检测.md](note/心跳检测.md) | 1023/1024 双端心跳、单调时钟、超时参数、幂等清理与复测步骤 |
| [异常处理.md](note/异常处理.md) | TCP/心跳异常、Qt/Asio 异步回调对象生命周期与服务端、客户端停止顺序 |
| [消息ID全链路.md](note/消息ID全链路.md) | 客户端消息 ID、HTTP/TCP 流程、好友申请认证、聊天及 invite 必填 call_type 的音视频信令 |
| [MySQL连接库.md](note/MySQL连接库.md) | X DevAPI / mysqlcppconnx |
| [Redis.md](note/Redis.md) | Redis 在验证码、token、在线路由、负载、缓存和分布式锁中的作用 |
| [Qt知识点.md](note/Qt知识点.md) | Qt 类设计、信号槽、事件 |
| [Qt网络编程.md](note/Qt网络编程.md) | HTTP/TCP 编程、粘包 |
| [Qt头像统一显示.md](note/Qt头像统一显示.md) | Qt 客户端头像路径、稳定兜底和圆形裁切约定 |
| [样式表.md](note/样式表.md) | 桌面统一视觉、QPainter 登录标识、线性图标、QSS 状态与布局边界 |
| [json.md](note/json.md) | JSON 在客户端、C++ 服务端和 Node.js 配置中的使用 |
| [资源服务.md](note/资源服务.md) | ResourceServer 上传任务、资源 ID 与分片下载协议 |
| [总结图片传输.md](note/总结图片传输.md) | 图片上传、核验、路由、下载、持久化与联调边界总结 |
| [文件传输.md](note/文件传输.md) | 私聊文件上传、可信元数据、手动下载鉴权与历史同步 |
| [视频通话技术.md](note/视频通话技术.md) | 音视频生产接入设计与学习路径：Qt WebEngine/MSVC、独立WSS方案、coturn、鉴权、状态机与验收；当前好友音视频实现见 WebRTC视频通信 |
| [WebRTC视频通信.md](note/WebRTC视频通信.md) | Qt WebEngine + ZLMediaKit 架构设计、好友音视频通话、原单向固定文件 test、信令与媒体生命周期及可信回环部署边界；短时媒体鉴权和公网 ICE 属生产设计 |
