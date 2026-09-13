#include "RedisDistributedLock.h"

// redisConnectWithTimeout 使用 timeval。Windows 在 winsock2.h 中定义它；
// 必须放在 hiredis（以及可能间接包含 Windows 头）的前面，避免 IntelliSense 和 MSVC
// 把 timeval 视为不完整类型。
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/time.h>
#endif

#include <hiredis/hiredis.h>

#include <array>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>
#include <utility>

namespace {

constexpr char kSetCommand[] = "SET";
constexpr char kNxOption[] = "NX";
constexpr char kPxOption[] = "PX";

// Lua 脚本整体在 Redis 内原子执行，GET 和 DEL 之间不会被其他客户端插入命令。
constexpr char kUnlockScript[] =
    "if redis.call('GET', KEYS[1]) == ARGV[1] then "
    "return redis.call('DEL', KEYS[1]) "
    "else return 0 end";

// 同样先核对 token，再延长 TTL，避免旧 owner 给新 owner 的锁续期。
constexpr char kRenewScript[] =
    "if redis.call('GET', KEYS[1]) == ARGV[1] then "
    "return redis.call('PEXPIRE', KEYS[1], ARGV[2]) "
    "else return 0 end";

bool IsOkStatus(const redisReply* reply) {
    // hiredis 的 STATUS 回复不是以 \0 结尾的 C 字符串，因此按 reply->len 构造 string。
    return reply != nullptr && reply->type == REDIS_REPLY_STATUS && reply->str != nullptr
        && std::string(reply->str, static_cast<std::size_t>(reply->len)) == "OK";
}

std::string GenerateOwnerToken() {
    // token 不携带业务含义，只用于“证明自己是这把锁的 owner”。
    // 随机数加进程内递增序号，避免同一进程高频创建对象时碰撞。
    static std::atomic<std::uint64_t> sequence{0};
    std::random_device random_device;
    std::mt19937_64 generator(random_device());
    const auto random_part = generator();
    const auto sequence_part = sequence.fetch_add(1, std::memory_order_relaxed);
    const auto time_part = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());

    std::ostringstream stream;
    stream << std::hex << random_part << '-' << time_part << '-' << sequence_part;
    return stream.str();
}

redisContext* CreateAuthenticatedConnection(const RedisConnectionConfig& config) {
    if (config.host.empty() || config.port <= 0 || config.port > 65535) {
        return nullptr;
    }

    // hiredis 需要 timeval；把 C++ milliseconds 拆成秒和微秒。
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(config.connect_timeout.count() / 1000);
    timeout.tv_usec = static_cast<long>((config.connect_timeout.count() % 1000) * 1000);
    // 连接失败时 hiredis 有时仍会返回 context，只是 context->err 非 0，所以两个条件都检查。
    redisContext* context = redisConnectWithTimeout(config.host.c_str(), config.port, timeout);
    if (context == nullptr || context->err != 0) {
        if (context != nullptr) {
            redisFree(context);
        }
        return nullptr;
    }

    // 未设置 requirepass 时不能发送空 AUTH；Redis 会返回错误。
    if (config.password.empty()) {
        return context;
    }

    // 统一使用 redisCommandArgv：参数按长度传递，不需要拼接命令字符串，也避免空格等字符
    // 被 Redis 命令解析器误当作分隔符。
    std::array<const char*, 2> args{"AUTH", config.password.data()};
    const std::array<std::size_t, 2> arg_lengths{4, config.password.size()};
    redisReply* reply = static_cast<redisReply*>(
        redisCommandArgv(context, static_cast<int>(args.size()), args.data(), arg_lengths.data()));
    const bool authenticated = IsOkStatus(reply);
    if (reply != nullptr) {
        freeReplyObject(reply);
    }
    if (!authenticated) {
        redisFree(context);
        return nullptr;
    }
    return context;
}

} // namespace

struct RedisConnectionPool::State {
    explicit State(const RedisConnectionConfig& connection_config) : config(connection_config) {}

    ~State() {
        std::lock_guard<std::mutex> lock(mutex);
        while (!connections.empty()) {
            redisFree(connections.front());
            connections.pop();
        }
    }

    void Return(redisContext* context, bool broken) noexcept {
        if (context == nullptr) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex);
        // 关闭后的池不再保留连接；命令失败的连接也不能复用，否则下一个请求会继续失败。
        if (stopped || broken) {
            redisFree(context);
            return;
        }
        connections.push(context);
        condition.notify_one();
    }

    RedisConnectionConfig config;
    std::queue<redisContext*> connections;
    std::mutex mutex;
    std::condition_variable condition;
    bool stopped = false;
};

RedisConnectionPool::RedisConnectionPool(const RedisConnectionConfig& config)
    : state_(std::make_shared<State>(config)) {
    // 连接建立放在构造期完成；失败的连接不入池，业务方法会返回 RedisError 而非无限阻塞。
    for (std::size_t index = 0; index < config.pool_size; ++index) {
        if (redisContext* context = CreateAuthenticatedConnection(config); context != nullptr) {
            state_->connections.push(context);
        }
    }
}

RedisConnectionPool::~RedisConnectionPool() {
    Close();
}

RedisConnectionPool::ConnectionLease RedisConnectionPool::BorrowFor(
    std::chrono::milliseconds wait_time) {
    const std::shared_ptr<State> state = state_;
    if (state == nullptr) {
        return {};
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    // 条件变量可能“虚假唤醒”，所以 wait_for 接受谓词，并在每次唤醒后重新判断条件。
    const auto ready = [&state] { return state->stopped || !state->connections.empty(); };
    if (wait_time.count() <= 0) {
        if (!ready()) {
            return {};
        }
    } else if (!state->condition.wait_for(lock, wait_time, ready)) {
        return {};
    }

    if (state->stopped || state->connections.empty()) {
        return {};
    }

    // 取出后不再放回队列；直到 ConnectionLease 析构，其他线程都无法同时使用这条连接。
    redisContext* context = state->connections.front();
    state->connections.pop();
    return ConnectionLease(state, context);
}

void RedisConnectionPool::Close() {
    const std::shared_ptr<State> state = state_;
    if (state == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    state->stopped = true;
    state->condition.notify_all();
}

RedisConnectionPool::ConnectionLease::ConnectionLease(
    std::shared_ptr<RedisConnectionPool::State> state, redisContext* context)
    : state_(std::move(state)), context_(context) {}

RedisConnectionPool::ConnectionLease::~ConnectionLease() {
    Return();
}

RedisConnectionPool::ConnectionLease::ConnectionLease(ConnectionLease&& other) noexcept
    : state_(std::move(other.state_)), context_(other.context_), broken_(other.broken_) {
    other.context_ = nullptr;
    other.broken_ = false;
}

RedisConnectionPool::ConnectionLease& RedisConnectionPool::ConnectionLease::operator=(
    ConnectionLease&& other) noexcept {
    if (this != &other) {
        Return();
        state_ = std::move(other.state_);
        context_ = other.context_;
        broken_ = other.broken_;
        other.context_ = nullptr;
        other.broken_ = false;
    }
    return *this;
}

redisContext* RedisConnectionPool::ConnectionLease::Get() const noexcept {
    return context_;
}

RedisConnectionPool::ConnectionLease::operator bool() const noexcept {
    return context_ != nullptr;
}

void RedisConnectionPool::ConnectionLease::MarkBroken() noexcept {
    broken_ = true;
}

void RedisConnectionPool::ConnectionLease::Return() noexcept {
    if (context_ != nullptr && state_ != nullptr) {
        // RAII 的关键：任何 return、异常或提前退出都能走到这里，避免连接泄漏。
        state_->Return(context_, broken_);
    }
    context_ = nullptr;
    broken_ = false;
    state_.reset();
}

RedisDistributedLock::RedisDistributedLock(std::shared_ptr<RedisConnectionPool> connection_pool,
                                           std::string key,
                                           std::chrono::milliseconds lease_time)
    : connection_pool_(std::move(connection_pool)),
      key_(std::move(key)),
      owner_token_(GenerateOwnerToken()),
      lease_time_(lease_time) {}

RedisDistributedLock::~RedisDistributedLock() {
    // 析构函数不能抛异常。若 Redis 已不可达，TTL 仍会作为最后的防死锁保障。
    (void)Unlock();
}

RedisLockResult RedisDistributedLock::TryLock() {
    std::lock_guard<std::mutex> lock(mutex_);
    return TryLockOnceLocked();
}

RedisLockResult RedisDistributedLock::TryLockFor(std::chrono::milliseconds wait_time,
                                                 std::chrono::milliseconds retry_interval) {
    if (wait_time.count() < 0 || retry_interval.count() <= 0) {
        return RedisLockResult::InvalidArgument;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    // steady_clock 不受系统时间校准影响，适合计算“最多等待多久”。
    const auto deadline = std::chrono::steady_clock::now() + wait_time;
    while (true) {
        // 每轮只发送一次 SET NX PX；Redis 返回 NIL 说明锁仍被其他 owner 持有。
        const RedisLockResult result = TryLockOnceLocked();
        if (result != RedisLockResult::Busy) {
            return result;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return RedisLockResult::Busy;
        }
        // 先把剩余时间显式转换为 milliseconds。这样既不会睡过 deadline，也避免
        // MSVC IntelliSense 对 std::min/chrono 模板实参推导出现“错误类型”。
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        // 最后一轮最多睡到 deadline，不能因 retry_interval 较大而超过用户指定的等待时间。
        const auto sleep_duration = retry_interval < remaining ? retry_interval : remaining;
        std::this_thread::sleep_for(sleep_duration);
    }
}

RedisLockResult RedisDistributedLock::TryLockOnceLocked() {
    if (owns_lock_) {
        // 同一个锁对象已经成功加锁时不重复 SET；它仍持有同一个 owner token。
        return RedisLockResult::Acquired;
    }
    if (connection_pool_ == nullptr || key_.empty() || lease_time_.count() <= 0) {
        return RedisLockResult::InvalidArgument;
    }

    // 锁竞争应在 Redis 层表现为 Busy，而不是在本地连接池长时间排队；因此这里立即借用。
    auto connection = connection_pool_->BorrowFor(std::chrono::milliseconds(0));
    if (!connection) {
        return RedisLockResult::RedisError;
    }

    // 等价命令：SET <key> <token> NX PX <ttl-ms>
    // NX 保证“key 不存在才写入”；PX 保证进程崩溃后 key 会自动过期，避免永久死锁。
    const std::string ttl = std::to_string(lease_time_.count());
    std::array<const char*, 6> args{
        kSetCommand, key_.data(), owner_token_.data(), kNxOption, kPxOption, ttl.data()};
    const std::array<std::size_t, 6> arg_lengths{
        3, key_.size(), owner_token_.size(), 2, 2, ttl.size()};
    redisReply* reply = static_cast<redisReply*>(redisCommandArgv(
        connection.Get(), static_cast<int>(args.size()), args.data(), arg_lengths.data()));
    if (reply == nullptr) {
        // nullptr 通常代表网络连接已断；不要把这条 context 放回池中。
        connection.MarkBroken();
        return RedisLockResult::RedisError;
    }

    const bool acquired = IsOkStatus(reply);
    // SET ... NX 在 key 已存在时返回 NIL；这属于正常竞争而不是 Redis 故障。
    const bool busy = reply->type == REDIS_REPLY_NIL;
    freeReplyObject(reply);
    if (acquired) {
        owns_lock_ = true;
        return RedisLockResult::Acquired;
    }
    return busy ? RedisLockResult::Busy : RedisLockResult::RedisError;
}

RedisLockResult RedisDistributedLock::Unlock() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!owns_lock_) {
        return RedisLockResult::NotLocked;
    }

    // 不能先 GET 再 DEL：两条命令之间锁可能过期并被其他实例抢走。
    // Lua 把“比较 token”和“删除 key”组合为 Redis 内的一个原子操作。
    const RedisLockResult result = EvaluateOwnershipScriptLocked(kUnlockScript);
    // 解锁命令遇到网络错误时，客户端无法判断 Redis 是否已经执行脚本。为避免业务继续
    // 使用“不确定是否仍存在”的锁，本对象从此一律放弃 owner 身份；最坏情况由 TTL 释放 key。
    owns_lock_ = false;
    return result;
}

RedisLockResult RedisDistributedLock::Renew() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!owns_lock_) {
        return RedisLockResult::NotLocked;
    }

    // 续期同样必须核对 token；否则暂停后恢复的旧请求会延长新 owner 的租约。
    const RedisLockResult result = EvaluateOwnershipScriptLocked(kRenewScript);
    // 续期失败（包括网络错误）后不能继续执行临界区：锁可能已经到期并被其他实例取得。
    if (result != RedisLockResult::Acquired) {
        owns_lock_ = false;
    }
    return result;
}

bool RedisDistributedLock::OwnsLock() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return owns_lock_;
}

const std::string& RedisDistributedLock::Key() const noexcept {
    return key_;
}

RedisLockResult RedisDistributedLock::EvaluateOwnershipScriptLocked(const char* script) {
    if (connection_pool_ == nullptr || key_.empty() || lease_time_.count() <= 0) {
        return RedisLockResult::InvalidArgument;
    }

    auto connection = connection_pool_->BorrowFor(std::chrono::milliseconds(0));
    if (!connection) {
        return RedisLockResult::RedisError;
    }

    const std::string ttl = std::to_string(lease_time_.count());
    // EVAL script 1 key token [ttl]。解锁脚本不读取 ttl，传入额外参数不影响其结果。
    std::array<const char*, 6> args{
        "EVAL", script, "1", key_.data(), owner_token_.data(), ttl.data()};
    const std::array<std::size_t, 6> arg_lengths{
        4, std::char_traits<char>::length(script), 1, key_.size(), owner_token_.size(), ttl.size()};
    redisReply* reply = static_cast<redisReply*>(redisCommandArgv(
        connection.Get(), static_cast<int>(args.size()), args.data(), arg_lengths.data()));
    if (reply == nullptr) {
        // Redis 是否已执行脚本无法从客户端确定；保持 owns_lock_ 不变，调用方应按错误处理，
        // 且不能假设自己仍拥有锁。TTL 会在最坏情况下释放该锁。
        connection.MarkBroken();
        return RedisLockResult::RedisError;
    }

    // 两段 Lua 脚本在成功时都返回 1；token 不匹配或 key 已过期时返回 0。
    const bool succeeded = reply->type == REDIS_REPLY_INTEGER && reply->integer == 1;
    const bool not_owner = reply->type == REDIS_REPLY_INTEGER && reply->integer == 0;
    freeReplyObject(reply);
    if (succeeded) {
        return RedisLockResult::Acquired;
    }
    return not_owner ? RedisLockResult::NotOwnerOrExpired : RedisLockResult::RedisError;
}
