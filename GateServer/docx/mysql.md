# GateServer 的 MySQL 账号访问

## 连接配置与依赖

[config.ini](../config.ini) 的 `[Mysql]` 提供 `host`、`port`、`user`、`passwd`、`schema`、`poolsize`。[MysqlMgr.cpp](../src/MysqlMgr.cpp) 启动时据此建立 X DevAPI Session 池；端口应填写 MySQL X Protocol 监听端口，常见值为 `33060`。命令行导入数据库使用的经典端口通常为 `3306`，需分别确认。

[CMakeLists.txt](../CMakeLists.txt) 链接 `mysqlcppconnx`，头文件为 `<mysqlx/xdevapi.h>`。配置密码不应复制到命令示例、截图或日志中。修改 ini 后重启服务使初始化参数生效。

## 账号数据与调用关系

| 入口 | MysqlMgr 调用 | 数据作用 |
| --- | --- | --- |
| `/user_register` | `Checkemail`、`Checkuser`、`RegUser` | 检查账号并分配 uid、插入用户 |
| `/user_login` | `Checkuser`、`CheckPwd` | 读取用户并比较密码串 |
| `/user_resetpassword` | `Checkuser`、`isMatch`、`UpdatePwd` | 确认用户名与邮箱关系并更新密码 |

HTTP 路由见 [LogicSystem.cpp](../src/LogicSystem.cpp)，表结构见 [create_tables.sql](../../sql/create_tables.sql)。`user.id` 为物理自增主键，业务 uid 来自 `user_id` 分配器。`uid` 与邮箱存在唯一索引，用户名目前是普通索引，入口先查询用户名不能消除并发注册窗口。

## 注册事务

`RegUser()` 借用同一 Session，执行 `startTransaction()`，更新 `user_id.id`、读取 uid、参数绑定插入 `user`，然后 `commit()`；异常时尝试回滚。分配器依赖单行约定，初始化值需覆盖已有 uid 范围。业务数据值使用 `?` 与 `bind()`，动态 SQL 结构仍需由程序控制。

```cpp
// 有效会话上的参数绑定片段；事务和归还责任由外层调用管理。
con->sql("INSERT INTO user (uid, name, email, pwd) VALUES (?, ?, ?, ?)")
   .bind(uid).bind(name).bind(email).bind(password).execute();
```

## 使用与边界

部署前先建立 schema 与用户表，再配置具备必要权限的服务账号。`poolsize` 应为正数，连接数量需要计入所有服务实例的数据库总连接上限。借出和归还都会 `SELECT 1` 校验，失效时重建；这无法保证业务 SQL 永不失败，也没有事务写入自动重试。

网关注册路由当前先构造成功 JSON，再调用 `RegUser()`，未检查其 bool 结果；重置密码同样未把更新失败转换为响应错误。操作界面显示成功不能直接作为写入提交依据。`Checkuser()` 等 bool 还合并了未找到和部分数据库异常，故障排查应检查数据库连接与服务日志。

当前密码为客户端异或后的可逆字符串，数据库层未使用密码哈希。运维应限制数据库访问、保护日志与凭据，并单独评估账号安全需求。

Session、结果集、异常与池关闭的完整规则见 [MySQL连接库](../../note/MySQL连接库.md) 和 [连接池](../../note/连接池.md)。
