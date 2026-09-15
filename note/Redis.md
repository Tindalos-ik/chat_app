# Redis 在项目中的作用

Redis 在这个聊天室中不是用来保存最终业务数据的数据库，而是一个所有服务都能访问的**共享快速状态层**。
它解决的是“短时间有效的数据”“跨 ChatServer 都要看见的在线状态”以及“多个实例不能同时修改同一资源”这三类问题。

```text
VarifyServer ── 验证码 ──────────────────────────┐
GateServer   ── 读取验证码 ──────────────────────┤
StatusServer ── token、ChatServer 负载 ──────────┤
ChatServer1  ── 在线路由、session、缓存、分布式锁 ├─ Redis
ChatServer2  ── 在线路由、session、缓存、分布式锁 ┘
                         │
                         └─ MySQL：用户、好友、好友申请等最终数据
```

> Redis 丢失或 key 过期后，能从 MySQL 重建的内容应回源重建；不能重建的最终业务事实不能只放 Redis。

## 1. 验证码：短期、自动过期的数据

VarifyServer 生成邮箱验证码后写入：

```text
key   = code_<email>
value = <六位验证码>
TTL   = 180 秒
```

它使用 Node.js 的 `SETEX` 一次完成写值和过期时间设置。GateServer 在注册、重置密码时读取同一个
`code_<email>` 并比较验证码；读不到通常表示验证码过期或不存在。

验证码很适合 Redis：它不需要长期保存，而且 TTL 到期后自动清除，无需定时任务扫表。

## 2. 登录 token：StatusServer 与 ChatServer 之间的凭证

StatusServer 为用户挑选 ChatServer 时生成 token，并写入：

```text
key   = utoken_<uid>
value = <登录 token>
```

之后客户端携带 `uid + token` 连接 ChatServer；ChatServer 再通过 gRPC 调用 StatusServer 校验。
StatusServer 和 ChatServer 都读取同一个 key，所以 token 不需要在两个服务间额外同步。

当前实现的 `RedisMgr::Set()` 没有给 `utoken_<uid>` 设置 TTL。这对教学联调足够，但生产环境应让 token
带过期时间，并在校验成功后按业务策略轮换或删除，避免 Redis 长期积累旧 token。

## 3. 服务负载：StatusServer 选择较空闲的 ChatServer

所有 ChatServer 共用一个 Hash：

```text
key   = login_count
field = ChatServer1 / ChatServer2
value = 当前在线人数
```

| 时机 | ChatServer 操作 | StatusServer 的用途 |
| --- | --- | --- |
| 服务启动 | `HSET login_count <server_name> 0` | 让实例进入可选列表 |
| 用户登录成功 | `HINCRBY ... +1` | 读取各 field，选择人数较少的实例 |
| 会话关闭 | `HINCRBY ... -1` | 使后续调度反映真实负载 |
| 服务退出 | `HDEL login_count <server_name>` | 不再把已下线实例分配给新用户 |

这里用 `HINCRBY`，而不是“先 GET、再 SET”。前者是 Redis 的单条原子命令，多个线程同时登录时不会丢失
增量。

## 4. 在线路由：知道用户在哪台 ChatServer

用户 TCP 登录成功后，ChatServer 写入：

```text
key   = uip_<uid>
value = ChatServer1 或 ChatServer2
```

这不是客户端 IP，而是用户当前所在的**服务实例名称**。发送好友申请、好友认证、文本消息时，当前
ChatServer 先读取 `uip_<uid>`：

- value 是本机名称：从本机 `UserMgr` 的内存 map 找到 `CSession`，直接推送；
- value 是另一台服务名：通过 gRPC 转发给对方 ChatServer；
- key 不存在：用户不在线，当前项目不支持离线消息补投。

`UserMgr` 中的 `uid → CSession` 只存在于本机内存，不能替代 `uip_<uid>`；另一台 ChatServer 看不到
这个 map。

## 5. 当前 session 版本：避免旧连接误删新连接状态

同一个 uid 可以很快重新登录。此时旧 TCP 连接可能在新连接已经建立后才触发断开回调。仅凭
`uip_<uid>` 无法区分“同一台服务器上的旧 session”和“新 session”，因此需要额外保存当前 session UUID：

```text
key   = usession_<uid>
value = 当前最新 CSession 的 session_id（UUID）
```

断开时先比较：

```text
Redis 中 usession_<uid> == 正在断开的 session_id？
  是 → 当前连接仍有效，可删除 usession_<uid> 和 uip_<uid>
  否 → 已有更新的连接；只能清理旧 TCP 会话，不能删 Redis 的新在线状态
```

这份状态必须在获取 `login_lock_<uid>` 后、释放锁前写入；当前登录流程在
`session->SetUserId(uid)` 后写入它，并同时维护 `uip_<uid>` 和本机 `UserMgr`。更稳妥的写入顺序是
先写 `usession_<uid>`，再写 `uip_<uid>`，这样后续断开回调始终有可比较的 session 版本。

> 当前 `USER_SESSION_PREFIX`、写入和断开校验逻辑已在 ChatServer1 出现；ChatServer2 也必须同步这套
> key 定义、写入与校验，否则跨服登录的会话一致性不能成立。

## 6. 用户资料缓存：减少 MySQL 查询

ChatServer 把用户资料序列化为 JSON 缓存到 Redis：

| key | value | 用途 |
| --- | --- | --- |
| `ubaseinfo_<uid>` | 某个用户的完整资料 JSON | 按 uid 查询、登录时加载资料 |
| `unameinfo_<name>` | 某个用户的完整资料 JSON | 按用户名搜索用户 |

读取方式是典型的 cache-aside：先查 Redis，命中且 JSON 合法就直接返回；未命中或内容损坏再查 MySQL，
然后回写 Redis。

用户资料发生修改时必须删除或更新两种缓存 key，否则客户端会读到旧昵称、头像、简介等信息。缓存只是
加速层，MySQL 仍是用户资料的最终来源。

## 7. 分布式锁：让同一业务资源串行执行

Redis 还保存短租约锁。以同一个用户并发登录为例，业务传入：

```text
业务锁名 = login_lock_<uid>
Redis key = lock:login_lock_<uid>
value     = 本次抢锁生成的 UUID token
TTL       = 10 秒（当前配置）
```

`SET key token NX PX ttl` 保证只有一个请求能获得同一 uid 的锁。获得锁的调用拿到 token，后续续租和
解锁必须带回同一个 token；Lua 脚本会先比较 token，再 `DEL` 或 `PEXPIRE`，旧请求不会误删新请求的锁。

锁的粒度应按**资源**而不是按整个功能划分：

```text
login_lock_1001 与 login_lock_1002：可以并发
login_lock_1001 与另一台服务器上的 login_lock_1001：必须互斥
```

详细实现见 [分布式锁设计.md](分布式锁设计.md)。分布式锁只能让业务流程串行，好友关系、申请状态等
最终仍要由 MySQL 事务、唯一索引或条件更新兜底。

## 8. key 一览与生命周期

| key 模式 | 类型 | 写入者 | 读取者 | 生命周期 |
| --- | --- | --- | --- | --- |
| `code_<email>` | String | VarifyServer | GateServer | 180 秒 TTL |
| `utoken_<uid>` | String | StatusServer | StatusServer、ChatServer | 当前无 TTL；应补 TTL |
| `login_count` | Hash | ChatServer1/2 | StatusServer | 随服务启动、退出维护 |
| `uip_<uid>` | String | 用户所在 ChatServer | 两台 ChatServer | 登录写入，当前 session 断开时删除 |
| `usession_<uid>` | String | 用户所在 ChatServer | 该 session 的断开回调 | 登录覆盖，当前 session 断开时删除 |
| `ubaseinfo_<uid>` | String(JSON) | ChatServer | ChatServer | 缓存；资料修改时失效 |
| `unameinfo_<name>` | String(JSON) | ChatServer | ChatServer | 缓存；资料修改时失效 |
| `lock:login_lock_<uid>` | String | DistLock | DistLock | 10 秒租约或显式解锁 |

## 9. 连接方式与边界

C++ 服务使用 hiredis，并在各自 `RedisMgr` 中维护 Redis TCP 连接池；一条 `redisContext` 在同一时刻只应
由一个线程借用。VarifyServer 使用 Node.js 的 ioredis。

Redis 的单线程执行模型保证每一条命令或 Lua 脚本自身原子，但不保证多条命令组成的业务流程原子。例如
“读 `uip_` → 踢旧连接 → 写新 `uip_`”必须由 `login_lock_<uid>` 串行化，或重新设计成单个 Lua 脚本。

最后要区分两类失败：

- `Busy`：Redis 正常，只是其他请求持有同一把锁；可返回“处理中”。
- `RedisError`：Redis 或连接池不可用；不能误当成普通竞争，更不能继续写临界资源。
