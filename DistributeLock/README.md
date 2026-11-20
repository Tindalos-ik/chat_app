# Redis 分布式锁组件

## 组成与接口

| 类型 | 职责 |
| --- | --- |
| `RedisConnectionConfig` | Redis 地址、密码、池容量、连接超时 |
| `RedisConnectionPool` | 建连认证、有限等待借用、关闭 |
| `ConnectionLease` | 独占借用 hiredis 连接，析构归还或销毁 |
| `RedisDistributedLock` | 获取、有限等待、续期、比较 token 后释放 |
| `RedisLockResult` | 区分成功、竞争、失去所有权、依赖错误和参数错误 |

实现为 C++17，使用 `redis-plus-plus` 依赖提供的 hiredis。ChatServer 登录使用其自身 `RedisMgr + DistLock` 封装；两者共享 `SET NX PX` 与 Lua 所有者比较原理。

## 获取、续期与释放

`TryLock()` 执行 `SET key token NX PX ttl`。key 已存在时返回 `Busy`，依赖异常返回 `RedisError`。`TryLockFor(wait_time, retry_interval)` 在给定等待窗口中只重试竞争。

`Unlock()` 通过 Lua 比较 token 后删除 key；`Renew()` 比较后刷新 TTL。它们成功时也返回 `Acquired`。`NotOwnerOrExpired` 表示 Redis 中的 key 已失效或属于其他对象；`NotLocked` 表示本地没有持锁状态。

| 状态 | 操作要求 |
| --- | --- |
| 已取得锁 | 在租约内完成业务，或到期前主动续期 |
| `Busy` | 按业务等待或结束本次请求 |
| `RedisError` | 停止受锁保护的写入，检查依赖 |
| 续期失败 | 放弃持锁假设，停止后续临界区操作 |
| 解锁结果未知 | 本地放弃所有权，由 TTL 限制残留时间 |

析构会尽力释放锁，不能替代显式检查续期与解锁结果。`OwnsLock()` 只读本地标记，不查询 Redis。重复调用同对象的 `TryLock()` 不构成续租。

## 配置与对象生命周期

1. 从私有运行配置读取 Redis 主机、端口和密码，构造 `RedisConnectionConfig`。
2. 用 `shared_ptr<RedisConnectionPool>` 共享连接池，池容量按同时发送 Redis 命令的数量确定。
3. 为每次业务创建独立锁对象，传入完整资源 key 和毫秒租约；独立组件不会额外添加前缀。
4. 检查加锁结果后才访问共享资源；显式释放并检查结果。
5. 停止接收业务后关闭池。已借出连接归还时销毁，不重新进入关闭的池。

`ConnectionLease` 不可复制；移动转移借用权。空借用、坏连接与关闭状态均不应继续发命令。组件会丢弃损坏连接，未实现运行时自动补建。

## 独立构建

按 [环境部署](../环境部署.md#2-安装-c-依赖) 设置 `VCPKG_ROOT` 并安装 `redis-plus-plus`。在 `DistributeLock/` 目录中运行：

```powershell
cmake --preset windows-vcpkg-debug
cmake --build --preset windows-vcpkg-debug
```

预设选择 Visual Studio 2022、x64 和 `VCPKG_ROOT` 中的 vcpkg。换了工具链时，从新的构建目录配置，例如 `cmake --preset windows-vcpkg-debug -B build_recovered`，再用 `cmake --build build_recovered --config Debug` 构建。目标为静态库 `distribute_lock` 和程序 `distribute_lock_test`。

VS Code 可直接打开本目录，选择 `Windows x64 Debug (vcpkg)` 预设，构建后运行 `distribute_lock_test`。Windows 构建会把 hiredis DLL 复制到测试程序旁。

## 运行组件测试

按照 [Docker Compose 配置步骤](../环境部署.md#3-用-docker-compose-启动-mysql-和-redis)准备仓库外的 `.env`。在 `DistributeLock/` 目录执行以下命令；`start_redis.bat` 只启动 Compose 的 Redis 服务。测试使用回环地址，从环境变量读取端口和密码。

```powershell
$envFile = Join-Path $env:LOCALAPPDATA 'chat_app\docker\.env'
$settings = ConvertFrom-StringData (Get-Content -LiteralPath $envFile -Raw)
$env:REDIS_PORT = $settings.REDIS_PORT
$env:REDIS_PASSWORD = $settings.REDIS_PASSWORD
.\start_redis.bat
if ($LASTEXITCODE -ne 0) { throw 'Redis 未就绪' }
.\build\Debug\distribute_lock_test.exe
```

测试程序检查同 key 竞争、当前持有者续期和释放、释放后再次获取，以及多线程竞争下临界区的排他性。它依赖真实 Redis，不属于根目录 GoogleTest/CTest 的自动注册目标。

## 业务一致性边界

TTL 到期不会停止旧线程。owner token 可以防止错误释放另一把锁，数据库写入还需使用事务、唯一约束或版本条件。该组件使用单 Redis 节点，不提供数据库 fencing token 或 Redis 高可用协议。

等待窗口用于控制循环重试；同步网络命令的实际耗时还取决于连接与底层超时。长任务由调用方安排 `Renew()`，组件没有后台续期线程。

原理与 ChatServer 接入见 [分布式锁设计](../note/分布式锁设计.md)，连接借用与等待见 [连接池](../note/连接池.md)。
