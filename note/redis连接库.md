# hiredis、redis-plus-plus 与 ioredis

## 项目依赖与调用层次

C++ 服务的 CMake 通过 `find_package(redis++ CONFIG REQUIRED)` 链接 `redis++::redis++`，业务封装直接包含 `<hiredis/hiredis.h>` 并调用 hiredis C API。redis-plus-plus 依赖 hiredis；项目当前没有使用 `sw::redis::Redis` 这类 C++ 高层接口。构建依据见 [GateServer CMake](../GateServer/CMakeLists.txt)、[ChatServer1 CMake](../ChatServer1/CMakeLists.txt)。vcpkg 安装端口名为 `redis-plus-plus`，编译、链接和部署还需确保其 hiredis 依赖可用。

| 组件 | 对象/API | 当前用途 |
| --- | --- | --- |
| hiredis | `redisContext`、`redisReply`、`redisCommand` | C++ RedisMgr 的实际命令执行 |
| redis-plus-plus | `redis++::redis++` | CMake 依赖与链接入口 |
| ioredis | `new Redis()`、`get()`、`setex()` | VarifyServer 的异步 Redis 客户端 |

业务 key 和生命周期见 [Redis](Redis.md)，借还、等待与关闭见 [连接池](连接池.md)。

## 连接与认证

`redisConnect(host, port)` 返回 `redisContext*`。失败既可能返回空指针，也可能返回带 `err` 的 context；后一种仍需 `redisFree()`。TCP 连接建立后，项目执行 `AUTH`，密码来自 `[Redis] passwd`。当前构造流程使用单参数密码认证，未配置 ACL 用户名或 TLS。

认证需要同时检查命令返回是否为空、reply 类型与成功状态。现有池构造在读取 `reply->type` 前未判断 AUTH 的空 reply，认证阶段网络故障可能触发空指针访问；不能把这段实现作为完整错误处理范例。

连接初始化会失败，实际入队数量可能小于请求的池大小。该池没有后台补充线程，全部初始化失败时普通借用会一直等待到 `Close()`。

## 命令格式与响应类型

`redisCommand()` 为同步调用，返回 `void*`，通常转换为 `redisReply*`。格式串应由程序固定，参数通过 `%s`、`%b` 或 argv 传入，不能让用户内容成为格式串。

```cpp
// 已借到有效 ctx；此片段只说明命令与响应的关系。
auto* reply = static_cast<redisReply*>(redisCommand(ctx, "GET %s", key.c_str()));
if (reply && reply->type == REDIS_REPLY_STRING) {
    value.assign(reply->str, reply->len);
}
if (reply) freeReplyObject(reply);
```

`%s` 将一个以 NUL 结尾的字符串编码为命令参数，参数内空格不会被拆成新参数。它无法传递内嵌 `\0` 后的内容。需要二进制安全时使用 `%b` 并传长度，或使用 `redisCommandArgv()`。

| reply 类型 | 可读取字段 | 命令语义示例 |
| --- | --- | --- |
| `REDIS_REPLY_STRING` | `str` 与 `len` | `GET`、`HGET` 的值 |
| `REDIS_REPLY_STATUS` | `str` | `SET`、`AUTH` 返回 `OK` |
| `REDIS_REPLY_INTEGER` | `integer` | `DEL` 数量、`EXISTS` 数量、`HINCRBY` 新值 |
| `REDIS_REPLY_NIL` | 不读取字符串 | key 不存在、空列表弹出、`SET NX` 未取得锁 |
| `REDIS_REPLY_ERROR` | `str` | 认证、类型或脚本错误 |
| `REDIS_REPLY_ARRAY` | `elements`、`element[]` | 多元素查询结果 |

命令返回空指针表示 I/O 或客户端执行失败，缺失 key 返回有效的 NIL reply。两者具有不同运维含义。`HSET` 返回新增 field 数量，更新已有 field 时返回 `0` 仍是成功；不能把所有整数响应都按“大于零”判断成功。

## 二进制安全与资源所有权

[RedisMgr HSet](../GateServer/src/RedisMgr.cpp) 的带长度重载使用 argv：

```cpp
// key、field 和 value 均是 std::string，可包含内嵌 NUL。
const char* argv[] = {"HSET", key.data(), field.data(), value.data()};
const size_t lengths[] = {4, key.size(), field.size(), value.size()};
auto* reply = static_cast<redisReply*>(redisCommandArgv(ctx, 4, argv, lengths));
```

调用者负责两类独立资源：

- 每个命令产生的 reply 用 `freeReplyObject()` 释放，数组回复由该函数递归释放子项。
- context 用毕归还池；池决定是否保留，最终由 `redisFree()` 销毁。

reply 中的字符串归 reply 所有，释放前必须复制需要保留的数据。复制时显式使用 `len` 才能保留 NUL。发送端使用 argv 只解决发送的二进制安全，若读取封装使用 `value = reply->str`，接收端仍会截断内嵌 NUL。

同一同步 `redisContext` 不允许多个线程同时执行命令，否则请求与响应可能交错。Redis 服务端命令串行执行并不能保护客户端 socket 的并发读写。项目通过池独占借用 context。

## RedisMgr 的服务差异

[GateServer 头文件](../GateServer/include/RedisMgr.h) 和 [StatusServer 头文件](../StatusServer/include/RedisMgr.h) 提供普通 String、List、Hash 操作。`Get()` 返回 bool，`HGet()` 返回字符串，现有封装会将不存在与部分错误合并；空字符串也可能是合法业务值，调用者不能据此推导 Redis 一定正常。

[ChatServer1 头文件](../ChatServer1/include/RedisMgr.h) 与 [ChatServer2 头文件](../ChatServer2/include/RedisMgr.h) 还提供在线数增减、删除 Hash field、分布式锁和 Lua 脚本接口。锁通过有界借用等待，网络失败与正常竞争分别返回 `RedisError` 和 `Busy`。标记为 broken 的连接归还时释放，当前没有补建，因此持续故障会减少可用数量。

各 C++ RedisMgr 构造时请求 5 条连接，配置读取 `host/port/passwd`，池大小未从 ini 读取。普通命令借用无超时；锁和部分脚本使用 `getConnectionFor()`。当前未统一配置连接和命令超时，也没有普通连接的周期探活或完整自动重建。Redis 自身 timeout、网络设备和进程重启均可导致断连。

关闭时也存在实现差异：ChatServer 池会释放停止后归还的 context；GateServer/StatusServer 的停止分支直接返回，没有释放该裸指针。`Close()` 唤醒等待线程，但不等待正在借用的业务结束，池对象必须活到所有借用者退出之后。

## Node.js 的 ioredis

[VarifyServer redis.js](../VarifyServer/redis.js) 建立一个共享 ioredis 实例，`get()` 和 `setex()` 返回 Promise，业务通过 `await` 等待结果。等待期间事件循环仍可处理其他回调，无需为每封邮件创建一个 Redis 客户端。

`GetRedis()` 将 key 缺失和异常都返回为 `null`；`SetRedisExpire()` 将异常返回为 `false`。error 事件处理器调用 `quit()`，因此本服务不能依赖 ioredis 默认重连行为保证长期自愈。异步回调可以交错执行，多个请求的读后写仍有竞争窗口。

包清单同时包含 `redis` 与 `ioredis`，实际 `require()` 使用 `ioredis`。服务运行配置与发信操作见 [Node.js 邮箱认证服务](../VarifyServer/nodejs搭建邮箱认证服务.md)。

API 参考：[hiredis](https://github.com/redis/hiredis)、[redis-plus-plus](https://github.com/sewenew/redis-plus-plus)、[ioredis](https://github.com/redis/ioredis)。
