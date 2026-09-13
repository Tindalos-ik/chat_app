#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

struct redisContext;

/**
 * Redis 连接池配置。
 *
 * 分布式锁的每次加锁、续期和解锁都是一次 Redis 命令。连接池避免每次命令都重新
 * 建立 TCP 连接并 AUTH；同一条 hiredis 连接只会同时借给一个调用方使用。
 */
struct RedisConnectionConfig {
    std::string host = "127.0.0.1";                  // Redis 地址
    int port = 6379;                                 // Redis TCP 端口
    std::string password;                            // requirepass；无密码 Redis 留空
    std::size_t pool_size = 5;                       // 最多同时被借出的 Redis 连接数
    std::chrono::milliseconds connect_timeout{1000}; // 建连和认证允许的最长时间
};

/** Redis 锁操作的结果。Busy 和 RedisError 必须由调用方区别处理。 */
enum class RedisLockResult {
    Acquired,              // 成功取得锁，或成功续期/释放（视调用方法而定）
    Busy,                  // 锁仍被其他 owner 持有
    NotOwnerOrExpired,     // key 不存在，或 value 已不再是当前实例的 token
    RedisError,            // 网络、认证或 Redis 命令错误
    InvalidArgument,       // 空 key、非正 TTL 等调用错误
    NotLocked              // 当前对象并未成功持有锁
};

/**
 * @brief hiredis 连接池。
 *
 * 这是锁组件内部使用的池。BorrowFor() 返回的 ConnectionLease 采用 RAII：作用域结束
 * 自动归还连接。命令异常导致连接不健康时调用 MarkBroken()，该连接会被销毁而不会
 * 再交给下一个请求使用。
 */
class RedisConnectionPool {
public:
    class ConnectionLease;

    explicit RedisConnectionPool(const RedisConnectionConfig& config);
    ~RedisConnectionPool();

    RedisConnectionPool(const RedisConnectionPool&) = delete;
    RedisConnectionPool& operator=(const RedisConnectionPool&) = delete;

    // 在 wait_time 内等待一条空闲连接；超时或连接池关闭时返回空 lease。
    [[nodiscard]] ConnectionLease BorrowFor(std::chrono::milliseconds wait_time);
    // 不再接受新的借用者；已经借出的连接归还时会被释放。
    void Close();

private:
    struct State;
    std::shared_ptr<State> state_;
};

class RedisConnectionPool::ConnectionLease {
public:
    ConnectionLease() = default;
    ~ConnectionLease();

    ConnectionLease(const ConnectionLease&) = delete;
    ConnectionLease& operator=(const ConnectionLease&) = delete;
    ConnectionLease(ConnectionLease&& other) noexcept;
    ConnectionLease& operator=(ConnectionLease&& other) noexcept;

    // 仅供本组件向 hiredis 发命令使用；lease 存活期间该连接不会被其他线程拿到。
    [[nodiscard]] redisContext* Get() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;

    // redisCommand()/redisCommandArgv() 返回 nullptr 时调用，避免归还坏连接。
    void MarkBroken() noexcept;

private:
    friend class RedisConnectionPool;
    ConnectionLease(std::shared_ptr<RedisConnectionPool::State> state, redisContext* context);
    void Return() noexcept;

    std::shared_ptr<RedisConnectionPool::State> state_;
    redisContext* context_ = nullptr;
    bool broken_ = false;
};

/**
 * @brief 基于 Redis 的单实例分布式锁。
 *
 * 加锁命令为 SET key token NX PX ttl：Redis 在一个原子命令内完成“key 不存在才写入”
 * 和“设置过期时间”。token 是本对象独有的随机值；解锁、续期均通过 Lua 脚本先核对
 * token，避免旧持锁者在锁过期后误删或续期新持锁者的锁。
 *
 * 一个 RedisDistributedLock 对象只应由一个业务线程使用。它不可复制，析构时会尽力
 * 解锁；即使进程崩溃或 Redis 临时不可用，PX 设置的 TTL 仍会最终释放锁。
 */
class RedisDistributedLock {
public:
    RedisDistributedLock(std::shared_ptr<RedisConnectionPool> connection_pool,
                         std::string key,
                         std::chrono::milliseconds lease_time);
    ~RedisDistributedLock();

    RedisDistributedLock(const RedisDistributedLock&) = delete;
    RedisDistributedLock& operator=(const RedisDistributedLock&) = delete;

    // 立即尝试一次，不等待其他持锁者释放。
    // Acquired 表示当前对象从此刻起持锁；Busy 表示其他 token 正在持有该 key。
    [[nodiscard]] RedisLockResult TryLock();

    // 在 wait_time 内按 retry_interval 重试；不会突破构造时指定的 lease_time。
    // RedisError 不重试：它代表依赖不可用，而不是其他业务实例正在竞争锁。
    [[nodiscard]] RedisLockResult TryLockFor(
        std::chrono::milliseconds wait_time,
        std::chrono::milliseconds retry_interval = std::chrono::milliseconds(20));

    // 仅 owner token 匹配时才删除 key，防止误删已经过期并被其他实例抢到的锁。
    // 返回 NotOwnerOrExpired 时，说明锁已过期或当前实例已失去所有权。
    [[nodiscard]] RedisLockResult Unlock();

    // 仅 owner token 匹配时刷新 TTL。长任务必须在锁过期前主动调用它。
    // 续期失败后不能继续把自己当作持锁者，业务应停止写入临界资源。
    [[nodiscard]] RedisLockResult Renew();

    [[nodiscard]] bool OwnsLock() const;
    [[nodiscard]] const std::string& Key() const noexcept;

private:
    // 调用方已持有 mutex_，执行一次 SET NX PX，不做等待或重试。
    RedisLockResult TryLockOnceLocked();
    // 调用方已持有 mutex_，执行“token 校验 + DEL/PEXPIRE”的 Lua 脚本。
    RedisLockResult EvaluateOwnershipScriptLocked(const char* script);

    std::shared_ptr<RedisConnectionPool> connection_pool_;
    const std::string key_;                         // Redis 中的锁名，例如 lock:friend-apply:42
    const std::string owner_token_;                 // 只属于本对象的“锁所有权凭证”
    const std::chrono::milliseconds lease_time_;    // Redis PX 的租约；过期会自动删除 key
    mutable std::mutex mutex_;
    bool owns_lock_ = false;
};
