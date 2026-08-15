# AGENT.md — 本项目 AI 助手行为规范

> 本文件只写行为规范：在本仓库工作时该怎么做、不该怎么做。
> 项目知识、常见疑惑解答在 `note/` 目录，不要往这里堆知识点。

---

## 工作原则

1. 动手改代码前，先读 README.md 与本文件；涉及具体模块时，再读 note/ 里对应的笔记。
2. 小改动（修 bug、补接口、整理文档）直接做；涉及数据库行为、协议字段、跨服务接口或破坏性操作的改动，先向用户说明影响范围再动手。
3. 修改后尽量验证：SQL/表名改动用 rg 检查三份一致性；文档改动确认内容完整、编码正常。
4. 用户问过的小疑惑、新学到的知识点，写入 note/（新建或补充现有 md），并在文末索引登记。
5. 不执行破坏性命令（rm、reset、清库等）；确需执行必须先征得同意。
6. 不要重复报告已经写进"已知坑"清单的问题；遇到它们直接按既定方向修或跳过，并在交付时说明。

## 代码规范（必须遵守）

| 规范 | 内容 |
| --- | --- |
| 表名 / 字段 | 统一 `user` / `pwd`（llfc1），**不要改回 users/password** |
| `desc` 列 | MySQL 保留字，SQL 里必须写反引号：`` `desc` `` |
| MySQL API | mysqlcppconnx（X DevAPI）：端口 33060、列下标从 0 开始、参数用 `?` + `bind()` 防注入 |
| 连接池 | 拿到的连接必须用 `Defer` 自动归还；使用前判空（池关闭时返回 nullptr） |
| 三份 MysqlMgr | GateServer / StatusServer / ChatServer 三份默认保持同步；只改一份时必须说明原因 |
| 密码 | 存的是客户端 xorString 后的串（可逆），不要当安全方案宣传 |
| 协议 | 消息 ID / 错误码见 `ChatServer/include/const.h` 与 `chat_app desktop/global.h`；改协议前后端要同步 |
| 单例 | HttpMgr / TcpMgr / MysqlMgr 等用 Singleton 模板基类：构造私有 + friend，不要破坏 |
| 密码学相关 | token 生成、密码哈希若要"认真做"，先跟用户确认方案（当前是教学级实现） |

## 已知坑（遇到直接修，不要当新发现重复报告）

1. ~~`RegUser` 没分配 uid~~：已修复——改为事务里 `UPDATE user_id SET id=id+1` 分配 uid 再插入（三份 MysqlMgr 同步）；依赖 `user_id` 表存在并初始化，建表脚本见 `sql/create_tables.sql`。
2. ChatServer `LoginHandler` 的 `GetUserInfo(uid)` 已实现但没接进登录回包（TODO，注意回包别带 pwd）。
3. token 存 StatusServer 内存 map：重启全失效、无 TTL、多实例不一致。
4. TcpMgr `_b_recy_pending` 声明未赋值：半包状态没真正记录，遇到半包会解析错位。
5. Qt 客户端 `slot_send_data` 用 `data.size()` 算包长，中文会错，应改用 `dataByte.size()`。
6. TimerBtn 手动 `emit clicked()` 且基类还会再发一次，可能重复触发。
7. RegisterDialog 用 `QEventLoop::exec` 阻塞 3 秒，期间 UI 卡死。
8. GateServer 注册是"先查重再插入"，无事务，并发下可能重复注册。
9. 跨 ChatServer 消息路由缺失，A 服务器的用户发不了 B 服务器的用户。

## 文档规范

* 文档风格：中文、叙述式、带"为什么"、表格 + 代码块 + `>` 提示，参考 note/ 现有笔记。
* 新文档放 note/，文件名用中文；写完后更新 AGENT.md 文末索引。

## 文档索引

| 文件 | 内容 |
| --- | --- |
| [sql/create_tables.sql](sql/create_tables.sql) | 数据库建表脚本（user/user_id/friend/friend_apply） |
| [疑惑.md](note/疑惑.md) | 常见小知识点解答（uid/token、分布式、Qt 并发安全、MySQL/Redis 边界等） |
| [全栈聊天室.md](note/全栈聊天室.md) | 架构总览 |
| [login.md](note/login.md) | 登录全链路 |
| [TCP服务器.md](note/TCP服务器.md) | Asio TCP 服务器、粘包处理 |
| [连接池.md](note/连接池.md) | 项目中 4 类连接池 |
| [grpc.md](note/grpc.md) | gRPC 应用 |
| [服务部署.md](note/服务部署.md) | Windows/Linux 部署 |
| [数据库设计.md](note/数据库设计.md) | MySQL 用户数据设计（llfc1） |
| [MySQL连接库.md](note/MySQL连接库.md) | X DevAPI / mysqlcppconnx |
| [Qt知识点.md](note/Qt知识点.md) | Qt 类设计、信号槽、事件 |
| [Qt网络编程.md](note/Qt网络编程.md) | HTTP/TCP 编程、粘包 |
