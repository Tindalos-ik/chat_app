#include "DistLock.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace {

constexpr char kUnlockScript[] =
    "if redis.call('GET', KEYS[1]) == ARGV[1] then return redis.call('DEL', KEYS[1]) "
    "else return 0 end";
constexpr char kRenewScript[] =
    "if redis.call('GET', KEYS[1]) == ARGV[1] then return redis.call('PEXPIRE', KEYS[1], ARGV[2]) "
    "else return 0 end";

std::string GenerateUuid() {
    boost::uuids::uuid uuid = boost::uuids::random_generator()();
    return boost::uuids::to_string(uuid);
}

std::string MakeLockKey(const std::string& lockName) {
    return "lock:" + lockName;
}

// GET 与 DEL/PEXPIRE 必须在 Redis 内原子执行，不能在客户端拆成两条命令。
RedisLockResult EvaluateOwnershipScript(redisContext* context,
                                        const char* script,
                                        const std::string& lockKey,
                                        const std::string& identifier,
                                        const std::string* ttl) {
    if (context == nullptr || lockKey.empty() || identifier.empty()) {
        return RedisLockResult::InvalidArgument;
    }
    if (context->err != 0) {
        return RedisLockResult::RedisError;
    }

    // redisCommandArgv 按长度传参，锁名、token 和 Lua 脚本中即使含空格也不会被重新分词。
    std::array<const char*, 6> args{"EVAL", script, "1", lockKey.data(), identifier.data(), nullptr};
    std::array<size_t, 6> lengths{
        4, std::char_traits<char>::length(script), 1, lockKey.size(), identifier.size(), 0};
    int argumentCount = 5;
    if (ttl != nullptr) {
        args[5] = ttl->data();
        lengths[5] = ttl->size();
        argumentCount = 6;
    }

    redisReply* reply = static_cast<redisReply*>(
        redisCommandArgv(context, argumentCount, args.data(), lengths.data()));
    if (reply == nullptr) {
        return RedisLockResult::RedisError;
    }

    const bool succeeded = reply->type == REDIS_REPLY_INTEGER && reply->integer == 1;
    const bool notOwner = reply->type == REDIS_REPLY_INTEGER && reply->integer == 0;
    freeReplyObject(reply);
    if (succeeded) {
        return RedisLockResult::Acquired;
    }
    return notOwner ? RedisLockResult::NotOwnerOrExpired : RedisLockResult::RedisError;
}

} // namespace

DistLock::~DistLock() {
}

RedisLockAcquireResult DistLock::tryAcquire(redisContext* context,
                                            const std::string& lockName,
                                            std::chrono::milliseconds leaseTime) {
    if (context == nullptr || lockName.empty() || leaseTime.count() <= 0) {
        return {RedisLockResult::InvalidArgument, {}};
    }
    if (context->err != 0) {
        return {RedisLockResult::RedisError, {}};
    }

    // token 不带业务语义，仅用来证明当前请求是这把锁的 owner。
    const std::string identifier = GenerateUuid();
    const std::string lockKey = MakeLockKey(lockName);
    const std::string ttl = std::to_string(leaseTime.count());
    // SET NX PX 在 Redis 中一次完成“仅不存在时设置”和“设置租约”。
    std::array<const char*, 6> args{"SET", lockKey.data(), identifier.data(), "NX", "PX", ttl.data()};
    const std::array<size_t, 6> lengths{3, lockKey.size(), identifier.size(), 2, 2, ttl.size()};
    redisReply* reply = static_cast<redisReply*>(
        redisCommandArgv(context, static_cast<int>(args.size()), args.data(), lengths.data()));
    if (reply == nullptr) {
        return {RedisLockResult::RedisError, {}};
    }

    const bool acquired = reply->type == REDIS_REPLY_STATUS && reply->str != nullptr
        && std::string(reply->str, static_cast<size_t>(reply->len)) == "OK";
    const bool busy = reply->type == REDIS_REPLY_NIL;
    freeReplyObject(reply);
    if (acquired) {
        return {RedisLockResult::Acquired, identifier};
    }
    return {busy ? RedisLockResult::Busy : RedisLockResult::RedisError, {}};
}

RedisLockResult DistLock::releaseLock(redisContext* context,
                                      const std::string& lockName,
                                      const std::string& identifier) {
    if (lockName.empty() || identifier.empty()) {
        return RedisLockResult::InvalidArgument;
    }
    return EvaluateOwnershipScript(context, kUnlockScript, MakeLockKey(lockName), identifier, nullptr);
}

RedisLockResult DistLock::renewLock(redisContext* context,
                                    const std::string& lockName,
                                    const std::string& identifier,
                                    std::chrono::milliseconds leaseTime) {
    if (lockName.empty() || identifier.empty() || leaseTime.count() <= 0) {
        return RedisLockResult::InvalidArgument;
    }
    const std::string ttl = std::to_string(leaseTime.count());
    return EvaluateOwnershipScript(context, kRenewScript, MakeLockKey(lockName), identifier, &ttl);
}
