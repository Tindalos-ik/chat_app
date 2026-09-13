# Redis 分布式锁

这个目录提供独立的 C++17 Redis 分布式锁组件，使用项目已安装的 `hiredis`（由
`redis-plus-plus` vcpkg 包提供）。它没有直接接入 ChatServer 的登录流程，便于先单独
编译、测试，再按业务需要接入。

## 安全设计

1. `SET key token NX PX ttl` 是一条原子命令：只有 key 不存在才写入 token，并同时设置
   过期时间。TTL 是进程宕机时避免死锁的兜底。
2. 每个锁对象生成一个随机 `token`。`Unlock()` 和 `Renew()` 用 Lua 在 Redis 内原子检查
   `GET key == token` 后再 `DEL` / `PEXPIRE`，旧 owner 不会误操作新 owner 的锁。
3. 连接池中的一条 hiredis 连接一次只借给一个调用者；`ConnectionLease` 离开作用域后自动
   归还，命令网络失败时会销毁坏连接。
4. 长于 TTL 的业务必须在锁过期前调用 `Renew()`；最终数据约束仍应由 MySQL 事务、唯一索引
   或条件更新兜底，分布式锁不能替代数据库约束。

## 使用示例

```cpp
#include "RedisDistributedLock.h"

auto pool = std::make_shared<RedisConnectionPool>(RedisConnectionConfig{
    "127.0.0.1", 6379, "123456", 5
});

// 只锁同一条好友申请，而不是锁住整个聊天系统。
RedisDistributedLock lock(pool, "lock:friend-apply:42", std::chrono::seconds(10));
if (lock.TryLockFor(std::chrono::milliseconds(200)) != RedisLockResult::Acquired) {
    // Busy：请求正在被别的实例处理；RedisError：Redis 不可用，应按业务降级或报错。
    return;
}

// 在这里执行：检查申请状态 -> 数据库事务写好友关系 -> 更新申请状态。
// 若工作时间可能超过 10 秒，定期检查 lock.Renew() 的结果。

lock.Unlock(); // token 匹配才会真的删除 Redis key；析构时也会尽力调用一次。
```

## VS Code CMake Tools：点击运行

用 VS Code **打开 `DistributeLock` 文件夹本身**（不是仓库根目录）。CMake Tools 会自动识别
`CMakePresets.json`，其中已固定 Visual Studio 2022、x64、vcpkg 和 Debug 配置。

1. 点击状态栏的 **Configure**，选择 `Windows x64 Debug (vcpkg)`；
2. 点击 **Build**，会构建 `distribute_lock_test`；
3. 先运行 `start_redis.bat`，确认 Redis 就绪；
4. 在状态栏选择运行目标 `distribute_lock_test`，点击三角形的 **Run**（或命令面板运行
   `CMake: Run Without Debugging`）。

构建后会自动把 `hiredisd.dll` 复制到测试程序旁边，因此不需要为 VS Code 手工配置 DLL 的
`PATH`。

也可在终端单独配置和编译：

```powershell
cmake --preset windows-vcpkg-debug
cmake --build --preset windows-vcpkg-debug
```

构建后先双击或在终端运行 `start_redis.bat`。脚本与根目录 `start_all.bat` 使用同一个 Redis
安装路径 `D:\cppsoft\Redis-x64-5.0.14.1`，以及相同密码 `123456`。脚本只在 `PING` 返回
`PONG` 时才视为 Redis 就绪；成功和失败都会暂停，双击运行时可以看到结果。然后执行：

```powershell
.\build\Debug\distribute_lock_test.exe
```

`distribute_lock_test` 会验证：

1. 第一把锁获取成功后，第二个相同 key 的锁返回 `Busy`；
2. 当前持锁者能够 `Renew()` 和 `Unlock()`；
3. 解锁后第二个对象能取得同一 key；
4. 8 个线程竞争同一个 key 时，所有线程最终都能进入临界区，且临界区最大并发数始终为 `1`。

产物 target 包含 `distribute_lock`（静态库）和 `distribute_lock_test`（测试程序）。需要让某个服务使用它时，再在该服务的
`CMakeLists.txt` 中通过 `add_subdirectory(../DistributeLock ...)` 或直接纳入源文件。
