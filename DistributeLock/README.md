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

可单独配置和编译：

```powershell
cmake -S DistributeLock -B build-distribute-lock `
  -DCMAKE_TOOLCHAIN_FILE=D:/cppsoft/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build-distribute-lock --config Debug
```

产物 target 名为 `distribute_lock`。需要让某个服务使用它时，再在该服务的
`CMakeLists.txt` 中通过 `add_subdirectory(../DistributeLock ...)` 或直接纳入源文件。
