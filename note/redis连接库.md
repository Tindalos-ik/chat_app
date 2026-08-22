# Redis 连接库知识点（hiredis / redis-plus-plus）

[hiredis 官方文档](https://github.com/redis/hiredis) ｜ [redis-plus-plus](https://github.com/sewenew/redis-plus-plus)

> 本项目实际使用的库：vcpkg 安装的 **redis-plus-plus（依赖 hiredis）**，CMake 链接目标 `redis++::redis++`，但代码里直接用的是 **hiredis 的 C API**，头文件 `<hiredis/hiredis.h>`。
> 对应代码：GateServer / StatusServer / ChatServer 三份完全相同的 `RedisMgr.h/cpp`；验证码服务 VarifyServer 用的是 Node.js 的 **ioredis**（`redis.js`）。
> 连接池的完整设计见 [连接池.md](连接池.md)

这个项目有点特殊：虽然 CMake 里链接的是 `redis++::redis++`（redis-plus-plus 这个 C++ 库），但封装类里写的全是 hiredis 的 C 接口。redis-plus-plus 本身就依赖 hiredis，所以两个库是一起装上的，直接 include `<hiredis/hiredis.h>` 就能用。


## 项目里的 Redis 客户端

| 客户端 | 语言 | 用在哪儿 | 特点 |
| --- | --- | --- | --- |
| hiredis | C | RedisMgr（三个 C++ 服务） | 最底层的 C 客户端，手动管理内存 |
| redis-plus-plus | C++ | 只出现在 CMake 链接里 | hiredis 的 C++ 封装，本项目的代码没直接用 |
| ioredis | Node.js | VarifyServer/redis.js | 异步 + Promise，天然适合 Node |



redis-plus-plus 的底层就是 hiredis，vcpkg 安装 redis-plus-plus 会把 hiredis 一起装进来，两个头文件和库都在。项目里 RedisMgr 直接用 C API：

* 连接：`redisConnect(host, port)` 返回 `redisContext*`
* 命令：`redisCommand(context, "SET %s %s", ...)` 返回 `redisReply*`
* 释放：`freeReplyObject(reply)`、`redisFree(context)`

> 用 C API 的好处是零依赖、直观；代价是所有内存都要手动管理，reply 用完必须 free，连接用完必须还回池子。


## hiredis 核心对象模型

和 MySQL 的 X DevAPI 那种链式 API 完全不一样，hiredis 就三个东西：

```text
redisContext*  一次 TCP 连接（含 socket、err 状态），相当于 MySQL 的 Session
    └── redisCommand(ctx, "GET %s", key)   发送命令
            └── redisReply*   服务器的回复
                    ├── type     回复类型（见下表）
                    ├── str      字符串值（type 为 STRING/STATUS/ERROR 时用）
                    └── integer  整数值（type 为 INTEGER 时用）
```

| 类型常量 | 含义 | 对应命令示例 |
| --- | --- | --- |
| `REDIS_REPLY_STRING` | 字符串 | GET、HGET |
| `REDIS_REPLY_STATUS` | 状态回复，内容是 "OK" | SET、AUTH |
| `REDIS_REPLY_INTEGER` | 整数 | LPUSH、EXISTS、DEL、HSET |
| `REDIS_REPLY_NIL` | 空/nil（key 不存在） | GET 不存在的 key、LPOP 空队列 |
| `REDIS_REPLY_ERROR` | 服务器报错 | AUTH 密码错误 |
| `REDIS_REPLY_ARRAY` | 数组（多条结果） | LRANGE、MGET |

知识点：

* `redisCommand` 的返回值本质是 `void*`，代码里强制转成 `redisReply*`；
* 返回 `NULL` 表示命令根本没执行成功（比如网络断了），**不是** "key 不存在"——key 不存在返回的是 `REDIS_REPLY_NIL`；
* 判断结果先看 `type`，再看对应字段：字符串看 `str`，整数看 `integer`。


## 项目用法逐段拆解

### 1. 创建连接：redisConnect + AUTH

```cpp
redisContext* context = redisConnect(host, port);
if (context == nullptr || context->err != 0) {
    // context->errstr 里有失败原因
    if (context != nullptr) redisFree(context);
    continue;
}
redisReply* reply = (redisReply*)redisCommand(context, "AUTH %s", pwd);
if (reply->type == REDIS_REPLY_ERROR) {   // 密码不对
    freeReplyObject(reply);
    redisFree(context);
    continue;
}
freeReplyObject(reply);
```

知识点：

* `redisConnect` 只负责建立 TCP 连接，**不认证**；Redis 设了 `requirepass` 就得手动发 `AUTH`；
* 判断连接失败要看 `context->err != 0`，同时 `context->errstr` 是失败描述；
* 认证失败的 reply 类型是 `REDIS_REPLY_ERROR`，释放完直接丢掉这条连接。

### 2. 执行命令：printf 风格的 redisCommand

```cpp
redisReply* reply = (redisReply*)redisCommand(connect, "SET %s %s", key.c_str(), value.c_str());
```

`redisCommand` 是 **printf 风格的格式化命令**，`%s` 会自动做参数转义，比手动拼字符串安全。项目里每个方法都是固定套路——**借连接 → 执行命令 → 判断 reply → 释放 reply → 还连接**：

```cpp
bool RedisMgr::Set(const std::string &key, const std::string &value) {
    auto connect = _connectionPool->getConnection();   // ① 借连接
    if (connect == nullptr) return false;              // 池已关闭拿不到

    redisReply* reply = (redisReply*)redisCommand(connect, "SET %s %s", key.c_str(), value.c_str());
    if (reply == NULL) {                               // ② 命令执行失败
        freeReplyObject(reply);                        // ③ 释放 reply
        _connectionPool->returnConnection(connect);    // ④ 还连接
        return false;
    }
    // SET 成功返回 STATUS 类型，内容 "OK"
    if (reply->type != REDIS_REPLY_STATUS || (strcmp(reply->str, "OK") != 0 && strcmp(reply->str, "ok") != 0)) {
        freeReplyObject(reply);
        _connectionPool->returnConnection(connect);
        return false;
    }

    freeReplyObject(reply);
    _connectionPool->returnConnection(connect);
    return true;
}
```

知识点：

* **每个失败分支都要释放 reply 并且归还连接**，漏一个连接就少一个，池子迟早被借空；
* SET 的返回值是 `REDIS_REPLY_STATUS`，内容 "OK"，代码里大小写都判断了；
* 项目里没给 Redis 用 Defer/RAII 兜底（MySQL 那边用了），全靠手写保证借还对。

### 3. 读取结果：先看 type 再取字段

```cpp
// GET：key 存在 → STRING；不存在 → NIL
redisReply* reply = (redisReply*)redisCommand(connect, "GET %s", key.c_str());
if (reply == NULL) { ... }
if (reply->type != REDIS_REPLY_STRING) { ... return false; }   // NIL 也会进这里
value = reply->str;     // 字符串结果在 str 里
```

```cpp
// LPUSH / EXISTS / DEL：返回整数
if (reply->type != REDIS_REPLY_INTEGER || reply->integer <= 0) { ... }
```

```cpp
// LPOP 空队列：返回 NIL
if (reply == NULL || reply->type == REDIS_REPLY_NIL) { ... }
```

知识点：

* 每个命令的返回类型不一样，封装函数做的事就是"**把 C 的 reply 翻译成 bool / string**"；
* `reply->str` 是 `char*`，赋值给 `std::string` 时直接拷贝，之后 free 掉 reply 也没关系；
* HGet 不存在时返回 NIL，函数返回空字符串 `""`，调用方用空串判断。

### 4. 二进制安全：redisCommandArgv

普通 `%s` 的命令遇到 `\0` 会截断（C 字符串以 `\0` 结尾），所以存二进制数据（图片、带 `\0` 的序列化内容）要用 **argv + argvlen** 的方式显式传长度：

```cpp
const char* argv[4];
size_t argvlen[4];
argv[0] = "HSET";      argvlen[0] = 4;
argv[1] = key;         argvlen[1] = strlen(key);
argv[2] = hkey;        argvlen[2] = strlen(hkey);
argv[3] = hvalue;      argvlen[3] = hvaluelen;   // 显式给长度，可以包含 '\0'
redisReply* reply = (redisReply*)redisCommandArgv(connect, 4, argv, argvlen);
```

知识点：

* `redisCommandArgv(context, argc, argv, argvlen)`：命令单词个数 + 参数数组 + 每个参数的长度；
* 这就是为什么 HSet 有两个重载：`std::string` 版本图方便，`const char* + 长度` 版本能存任意二进制。


## RedisConPool：连接池封装

### 为什么要有池

注释里写得很直白：**单例 + 单连接在多线程下不是线程安全的**。GateServer 是 Boost.Asio 多线程处理 HTTP 请求，多个线程同时拿同一个 `redisContext*` 发命令会乱，hiredis 的 context 不是线程安全的，所以要用多个连接，每次借一个用。

Redis 连接成本：一次 TCP 连接 + AUTH 认证。每次请求都新建连接太慢，池化后启动时一次建好 N 条，用"借-还"复用。

### 池的骨架（和连接池.md 完全一致）

```cpp
class RedisConPool {
    std::queue<redisContext*> connections_;   // ① 空闲连接队列（裸指针）
    std::mutex mutex_;                        // ② 互斥锁
    std::condition_variable cond_;            // ③ 池空时挂起等待
    std::atomic<bool> b_stop_;                // ④ 停止标志

    redisContext* getConnection() {           // 借
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this] {
            return b_stop_ || !connections_.empty();
        });
        if (b_stop_) return nullptr;          // 关闭后拿到 nullptr
        auto* context = connections_.front();
        connections_.pop();
        return context;
    }

    void returnConnection(redisContext* context) {  // 还
        std::lock_guard<std::mutex> lock(mutex_);
        if (b_stop_) return;                  // 池已关，连接就地释放
        connections_.push(context);
        cond_.notify_one();                   // 唤醒一个等待者
    }

    void Close() {
        b_stop_ = true;
        cond_.notify_all();                   // 唤醒所有等待者
    }
};
```

构造函数里把 5 条连接全部准备好：`redisConnect` → 查 `err` → `AUTH` → 查 reply 类型 → 入队，任何一步失败就 `redisFree` 丢弃这条。析构时把队列里剩下的连接全部 `redisFree`。

知识点：

* **借必须用 `unique_lock`**：`cond_.wait` 等待时要释放锁让出 CPU；
* 等待条件必须包含 `b_stop_`，否则池空时关闭，线程永远挂死；
* `Close()` 只置标志 + `notify_all`，唤醒的线程拿到 `nullptr` 自己退出；
* 归还前检查 `b_stop_`：关闭后不再把连接塞回队列；
* 和 MySqlPool 不同，**Redis 连接不会被服务端空闲断开**（MySQL 有 wait_timeout），所以这里没有 `SELECT 1` 式的心跳检测。


## RedisMgr：单例管理器

RedisMgr 继承 `Singleton<RedisMgr>`，进程内唯一，三个 C++ 服务各有一份相同的实现。

### 成员与构造

```cpp
class RedisMgr : public Singleton<RedisMgr>, public std::enable_shared_from_this<RedisMgr> {
    std::unique_ptr<RedisConPool> _connectionPool;
};

RedisMgr::RedisMgr() {
    auto& config = ConfigMgr::Inst();
    _connectionPool = std::move(std::unique_ptr<RedisConPool>(
        new RedisConPool(5, config["Redis"]["host"].c_str(),
                            std::stoi(config["Redis"]["port"]),
                            config["Redis"]["passwd"].c_str())));
}
```

知识点：

* 连接参数从 `config.ini` 的 `[Redis]` 段读，host/port/passwd 都是字符串，port 要 `std::stoi`；
* **池大小写死为 5**，不像 MySQL 有 `poolsize` 配置项——想调并发只能改代码重编译；
* 原来注释掉的单连接版 `Connect(host, port)` 因为上了池就不需要了。

### 方法清单

| 方法 | Redis 命令 | 返回类型 | 说明 |
| --- | --- | --- | --- |
| `Get(key, value)` | GET | bool | key 不存在返回 false |
| `Set(key, value)` | SET | bool | 要求 STATUS 且 "OK" |
| `Auth(password)` | AUTH | bool | 单独认证 |
| `LPush / RPush` | LPUSH / RPUSH | bool | 要求返回整数 > 0 |
| `LPop / RPop` | LPOP / RPOP | bool | 空队列返回 NIL → false |
| `HSet(key, hkey, value)` | HSET | bool | string 版本 |
| `HSet(key, hkey, value, len)` | HSET | bool | 二进制安全版本 |
| `HGet(key, hkey)` | HGET | std::string | 不存在返回 "" |
| `Del(key)` | DEL | bool | 删掉返回 true |
| `ExistsKey(key)` | EXISTS | bool | 存在才 true |
| `Close()` | - | void | 关池，唤醒所有等待线程 |

### 析构顺序的坑

```cpp
RedisMgr::~RedisMgr() {
    Close();   // RedisMgr 析构先于 Pool 析构，所以要手动 Close
}
```

`_connectionPool` 是 RedisMgr 的成员，RedisMgr 析构完才轮到 pool 析构；而 pool 析构只释放队列里的连接，**正在被借出去的连接没人管**。先 `Close()` 置停止标志、唤醒等待者，业务线程拿到 nullptr 自然退出，之后 pool 析构才安全。


## CMake / vcpkg 集成

安装（README 里的命令）：

```
vcpkg install redis-plus-plus --triplet x64-windows
```

redis-plus-plus 会**自动带上 hiredis**，两个头文件都在 vcpkg 的 include 目录下，不用单独装。三个服务的 CMakeLists.txt 里都是：

```cmake
find_package(redis++ CONFIG REQUIRED)

target_link_libraries(${PROJECT_NAME}
    ...
    redis++::redis++    # 链接 redis C++ 库（内部依赖 hiredis）
)
```

头文件路径不用手动配，vcpkg 工具链（`D:/cppsoft/vcpkg/scripts/buildsystems/vcpkg.cmake`）会把 `<hiredis/hiredis.h>` 所在的目录加进编译搜索路径。

> 注意：链接目标虽然叫 `redis++::redis++`，但代码里用的是 hiredis 的 C API，所以本质上是"装了 redis-plus-plus，用了它的底层 hiredis"。


## 配置

```
[Redis]
host = 127.0.0.1
port = 6379
passwd = 123456
```

* 默认端口 6379；
* 和 MySQL 一样用 ConfigMgr 解析，改配置不用重新编译；
* `passwd` 对应 Redis 配置里的 `requirepass`。


## VarifyServer：Node.js 的 ioredis

验证码服务是 Node.js 写的，用的 ioredis（另一个 redis 客户端），和 C++ 那边完全不同的风格：

```js
const Redis = require("ioredis");
const RedisCli = new Redis({
  host: config_module.redis_host,
  port: config_module.redis_port,
  password: config_module.redis_passwd,
});

RedisCli.on("error", function (err) {
  console.log("RedisCli connect error");
  RedisCli.quit();
});

async function GetRedis(key) {
  try {
    const result = await RedisCli.get(key);
    if (result === null) return null;      // key 不存在 → null
    return result;
  } catch (error) { return null; }
}

async function SetRedisExpire(key, value, exptime) {
  try {
    await RedisCli.setex(key, exptime, value);   // set + expire 一步到位
    return true;
  } catch (error) { return false; }
}
```

知识点：

* ioredis 默认端口就是 6379，和 hiredis 的 `redisConnect` 等价；
* **ioredis 是异步 API**：`get/setex` 返回 Promise，要用 `async/await`；
* `setex key 秒数 value`：设置值的同时带过期时间，一步完成，正好用来存验证码；
* Node.js 单线程 + 事件循环，不需要连接池和锁，一个全局 client 就够。


## 业务串联：验证码流程

Redis 在项目里目前最核心的作用就是**邮箱验证码的过期缓存**：

```text
VarifyServer（存）                            GateServer（取）
code_<email> → setex(验证码, expire_time) ──► Get("code_" + email) → 比对
```

VarifyServer `server.js`：

```js
await redis_module.SetRedisExpire(const_module.code_prefix + email, uniqueId, expire_time);
```

GateServer `LogicSystem.cpp`：

```cpp
std::string email_prefix = "code_";
bool b_get_varify = RedisMgr::GetInstance()->Get(email_prefix + email, redis_varifycode);
if (!b_get_varify) { /* 验证码过期或不存在 */ }
```

为什么用 Redis 存验证码：

1. **天然过期**：`expire` 到期自动删除，不用自己写定时器清理；
2. **读快**：验证码就是 key-value，O(1) 读写；
3. **跨语言共享**：VarifyServer 存、GateServer 取，C++ 和 Node.js 连的是同一份数据。

> 项目规划里 Redis 还承担 token、在线状态路由的缓存（README 写的"token、验证码、在线状态缓存"），StatusServer 已经 include 了 RedisMgr.h，后续用户状态映射（uid → 聊天服务器 + token）也会落到 Redis。


## 面试/复习自测

* hiredis 和 redis-plus-plus 什么关系？项目里实际用的是哪个？
* `redisConnect` 后为什么不直接能发命令？AUTH 失败怎么判断？
* `redisReply` 有哪些 type？GET 不存在的 key 返回什么？命令执行失败返回什么？
* 为什么 `redisCommand` 要传 `%s` 格式化而不是拼字符串？
* 什么情况下要用 `redisCommandArgv`？它和 `redisCommand` 的区别？
* `redisContext` 为什么不能多线程共用？连接池怎么解决？
* 借连接为什么必须 `unique_lock`？`cond_.wait` 的条件为什么要带 `b_stop_`？
* 每个 Redis 方法漏掉 `returnConnection` 会怎样？
* RedisMgr 为什么析构要先 `Close()`？
* ioredis 的 `setex` 和"先 set 再 expire"有什么区别？
