

# AI 规范

本文件中的每一条规则都是强制性的。违反任何一条规则都会遭受毁灭性打击。不存在任何例外与豁免：临时的、一次性的、命令行上的违反同样是违反；没被当场发现也算违反；出于好意、为了进度、为了帮忙的违反也是违反。

## 语言

不允许使用"不是...而是..."句式；如果不需要对比的话，就不要对比；不要在任何话说完之后都提一句"不是其他的xxx"。如果没有叫你进行对比，就不允许使用"不是...而是..."、"要...而不是..."等类似的句式，你根本就没有需要说"不是"的对象，不要虚空打靶。所有类似的句式都不允许使用。

如果我让你搜索A相关内容，你搜索到B、C、D发现不满足要求，就不允许再把B、C、D列举出来了。我根本就不关心，看到这些只会污染我的眼睛

任何回答都不允许总结和总起，包括：
"上述内容是<某种概述>，下面详细拆开"
"一句话总结：xxx"

不允许使用"落地"、"钉死"、"对齐"等非技术名词、显然有其他可以代替的词语的黑话。使用正常的，不在互联网公司或者金融公司工作的任何人可以看懂的，在简单中文里常用的词汇

## 行为

不允许在实现或者测试的时候使用任何 mock、假的、欺骗的、只为了通过测试而 workaround 的方式来欺骗我

不要求最小化依赖，不允许用各种乱七八糟的方式（包括造轮子）绕过依赖。

编写的代码应当寻求 fast-fail，在出错位置就地崩溃，而不是捕获错误，也不是 fallback

我经常会在你更改后撤回/修改你的更改，所以如果你发现无法从你上一次更改之后继续更改，你应该重新读取文件内容。比如：你添加了A、B、C内容，我把B删掉了，这意味着接下来的改动应该在B被删掉的状态下（A、C）开始改动，不允许把B加回去

你在发现任何文档或者代码有错误的时候，你的更新不要保留任何错误痕迹。我们不需要任何错误的记录

如果你在执行一件事的过程中，用户问了一个别的事，如果回应用户能马上回应，那么就直接回应。暂时处理完用户请求以后马上继续你之前正在执行的事情，不要干一半不干了

不允许使用 ASCII Art 画示意图、表格等。如果你需要画图（实际上许多时候你并不需要），必须使用 mermaid。ASCII art 是一种人类不可读、AI也不可读的极其恶心的格式，不允许使用

在写任何代码的时候，都需要在文件的最前面添加 docstring，也不允许添加 shebang。注释使用中文，术语保留英文；不要过度注释

每个新增的函数都需要写好函数作用，解释参数

你可以在做事过程中新建中间文件，但是完成之后需要删除，我不需要我不看的文件来稀释我的注意力

在我没有声明需要备份或者新建数据库之前，所有的修改你可以直接原地修改，不需要新建数据库

在我没有声明备份代码之前，你不能进行任何代码的打包

文档是写给人看的，要简洁易懂，格式符合人的阅读需求

不要过度自我否定，不要主动削弱研究贡献，不要把论文写成检讨书，而是准确地，有力地呈现研究结果

新加入功能的时候，不要将你自己的验证记录补充到文档中，我只需要文档中有如何操作的指南即可
## 文档索引

### 阅读入口与系统设计

| 文件 | 内容 |
| --- | --- |
| [README.md](README.md) | 环境、配置、构建、启动与知识阅读路径 |
| [全栈聊天室](note/全栈聊天室.md) | 通信通道、数据职责及源码阅读顺序 |
| [分布式聊天服务设计](note/分布式聊天服务设计.md) | 选服、在线路由、跨服投递与身份一致性 |
| [项目总结](note/项目总结.md) | 即时通信中的状态、持久化、并发与容量问题 |
| [项目优化](note/项目优化.md) | 当前业务调度模型、瓶颈分析与多 Worker 设计 |
| [疑惑](note/疑惑.md) | 身份、Qt 并发、存储与消息机制问答 |

### 登录、传输与生命周期

| 文件 | 内容 |
| --- | --- |
| [login](note/login.md) | HTTP 账号验证、选服、TCP 身份绑定 |
| [TCP服务器](note/TCP服务器.md) | Asio、增量帧解析、发送队列与连接管理 |
| [消息ID全链路](note/消息ID全链路.md) | HTTP/TCP 协议、请求与通知、业务标识 |
| [聊天信息收发](note/聊天信息收发.md) | 私聊和群文本的鉴权、持久化、路由与通知 |
| [聊天服务器服务](note/聊天服务器服务.md) | ChatService gRPC 契约与跨服业务语义 |
| [服务器踢人](note/服务器踢人.md) | 重复登录、会话身份比较与注销 |
| [心跳检测](note/心跳检测.md) | 双端心跳、单调时钟、超时及清理 |
| [异常处理](note/异常处理.md) | 异步对象生存期、网络异常与停止顺序 |
| [锁的精度控制](note/锁的精度控制.md) | 临界区、会话快照、锁顺序与成本 |
| [分布式锁设计](note/分布式锁设计.md) | Redis 租约、所有者比较、业务接入及一致性边界 |
| [独立分布式锁](DistributeLock/README.md) | 独立组件的配置、构建、接口与运行指南 |

### 数据、群聊与资源

| 文件 | 内容 |
| --- | --- |
| [数据库设计](note/数据库设计.md) | MySQL 与 SQLite 模型、索引、约束和迁移 |
| [聊天信息存储方案](note/聊天信息存储方案.md) | 服务端持久化、本地缓存、消息状态与增量同步 |
| [群聊](note/群聊.md) | 群模型、成员、准入、管理、公告头像与客户端同步 |
| [总结群聊](note/总结群聊.md) | 群事务、权限、版本、业务重放与并发推导 |
| [聊天可靠性与资源安全提升](note/聊天可靠性与资源安全提升.md) | ACK、已读、历史补偿、资源授权与恢复 |
| [资源服务](note/资源服务.md) | 上传任务、资源 ID、分片协议、核验与鉴权 |
| [图片传输](note/图片传输.md) | 图片上传、消息提交、下载、展示和缓存 |
| [文件传输](note/文件传输.md) | 文件元数据、手动下载、资源权限和附件界面 |

### Qt 与音视频

| 文件 | 内容 |
| --- | --- |
| [Qt知识点](note/Qt知识点.md) | 对象、信号槽、事件、布局及绘制 |
| [Qt网络编程](note/Qt网络编程.md) | HTTP/TCP、分帧、连接和请求生命周期 |
| [Qt头像统一显示](note/Qt头像统一显示.md) | 头像路径、默认图、裁切与缓存 |
| [样式表](note/样式表.md) | QSS、状态、图标、绘制与布局规则 |
| [WebRTC视频通信](note/WebRTC视频通信.md) | 好友通话、固定文件 test、媒体生命周期、部署和安全设计 |

### 依赖与测试

| 文件 | 内容 |
| --- | --- |
| [MySQL连接库](note/MySQL连接库.md) | X DevAPI、参数绑定、结果读取与事务 |
| [Redis](note/Redis.md) | 验证码、token、路由、计数、缓存与锁 |
| [Redis连接库](note/Redis连接库.md) | hiredis、redis-plus-plus、回复类型与连接资源 |
| [连接池](note/连接池.md) | 池化资源、借还、等待及关闭 |
| [grpc](note/grpc.md) | protobuf、服务、stub、调用生命周期与错误 |
| [json](note/json.md) | Qt、JsonCpp 与 Node.js 的序列化和精度 |
| [测试](note/测试.md) | 测试分层、隔离、断言与故障边界 |
| [测试运行指南](tests/README.md) | CMake/CTest、环境变量及各测试入口 |
| [网关 MySQL](GateServer/docx/mysql.md) | 网关账号数据访问 |
| [网关 Redis](GateServer/docx/redis.md) | 网关验证码与缓存访问 |
| [网关 gRPC](GateServer/docx/grpc.md) | 验证码和状态服务调用 |
| [网关 JsonCpp](GateServer/jsoncpp/jsoncpp.md) | HTTP JSON 解析和响应 |
| [状态服务 JsonCpp](StatusServer/jsoncpp/jsoncpp.md) | 状态服务中的 JSON 使用范围 |
| [ChatServer1 JsonCpp](ChatServer1/jsoncpp/jsoncpp.md) | 聊天请求、通知与序列化 |
| [ChatServer2 JsonCpp](ChatServer2/jsoncpp/jsoncpp.md) | 第二实例的协议一致性 |
| [资源服务 JsonCpp](ResourceServer/jsoncpp/jsoncpp.md) | 任务元数据与资源协议 |
| [邮箱认证服务](VarifyServer/nodejs搭建邮箱认证服务.md) | Node.js、SMTP、Redis 和验证码 gRPC |

### 数据库脚本

| 文件 | 内容 |
| --- | --- |
| [create_tables.sql](sql/create_tables.sql) | 全新环境完整 MySQL 建表 |
| [chat_app_test_local.sql](sql/chat_app_test_local.sql) | 隔离测试 schema 初始化 |
| [chat_message_storage.sql](sql/chat_message_storage.sql) | 补建缺失聊天表，不升级已有表 |
| [chat_image_message_migration.sql](sql/chat_image_message_migration.sql) | 图片与文件资源字段迁移 |
| [chat_message_receipt_migration.sql](sql/chat_message_receipt_migration.sql) | 展示确认字段迁移 |
| [chat_group_migration.sql](sql/chat_group_migration.sql) | 早期群表 owner、active、角色与外键迁移 |
| [chat_group_management_migration.sql](sql/chat_group_management_migration.sql) | 群版本、准入与业务操作结果迁移 |
| [client_chat_cache.sql](sql/client_chat_cache.sql) | 客户端 SQLite 表结构参考 |
