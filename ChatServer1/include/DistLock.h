#pragma once

#include <chrono>
#include <string>
#include <hiredis/hiredis.h>
#include "singleton.h"

// 区分几种状态
enum class RedisLockResult {
    Acquired, // 成功获取锁
    Busy, // 锁已被占用
    NotOwnerOrExpired, // 不是锁的持有者或已过期
    RedisError, // Redis 错误
    InvalidArgument, // 锁名或 token 为空
};

struct RedisLockAcquireResult {
    RedisLockResult result = RedisLockResult::RedisError;
    std::string identifier;
};

class DistLock : public Singleton<DistLock> {
    friend Singleton<DistLock>;
public:
    ~DistLock();
    // 只执行一次 SET NX PX。等待与重试由 RedisMgr 在归还连接后处理，避免长时间占用连接池。
    RedisLockAcquireResult tryAcquire(redisContext* context,
                                      const std::string& lockName,
                                      std::chrono::milliseconds leaseTime);
    // 仅当 token 匹配时才删除锁，避免过期 owner 删除新 owner 的锁。
    RedisLockResult releaseLock(redisContext* context,
                                const std::string& lockName,
                                const std::string& identifier);
    // 长任务在 TTL 到期前调用。token 不匹配或锁已过期时返回 NotOwnerOrExpired。
    RedisLockResult renewLock(redisContext* context,
                              const std::string& lockName,
                              const std::string& identifier,
                              std::chrono::milliseconds leaseTime);
private:
    DistLock() = default;
};
