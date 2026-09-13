#include "RedisDistributedLock.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr int kRedisPort = 6379;
constexpr char kRedisPassword[] = "123456";
constexpr int kThreadCount = 8;

const char* ToString(RedisLockResult result) {
    switch (result) {
        case RedisLockResult::Acquired:
            return "Acquired";
        case RedisLockResult::Busy:
            return "Busy";
        case RedisLockResult::NotOwnerOrExpired:
            return "NotOwnerOrExpired";
        case RedisLockResult::RedisError:
            return "RedisError";
        case RedisLockResult::InvalidArgument:
            return "InvalidArgument";
        case RedisLockResult::NotLocked:
            return "NotLocked";
    }
    return "Unknown";
}

// 每次运行使用独立 key。即使上次测试在解锁前中断，遗留 key 也不会影响本次测试。
std::string CreateTestKey(const char* suffix) {
    const auto timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::ostringstream stream;
    stream << "test:distributed-lock:" << suffix << ':' << timestamp;
    return stream.str();
}

bool ExpectResult(const char* step, RedisLockResult actual, RedisLockResult expected) {
    if (actual == expected) {
        std::cout << "[PASS] " << step << " -> " << ToString(actual) << std::endl;
        return true;
    }

    std::cerr << "[FAIL] " << step << ": expected " << ToString(expected)
              << ", actual " << ToString(actual) << std::endl;
    return false;
}

bool TestBasicLockLifecycle(const std::shared_ptr<RedisConnectionPool>& pool) {
    std::cout << "\n=== Test 1: 抢锁、续期、解锁 ===" << std::endl;
    const std::string key = CreateTestKey("lifecycle");
    RedisDistributedLock first_lock(pool, key, 3s);
    RedisDistributedLock competing_lock(pool, key, 3s);

    // 第一把锁获得成功后，使用相同 key 的第二把锁必须被 Redis 拒绝。
    if (!ExpectResult("第一个对象抢锁", first_lock.TryLock(), RedisLockResult::Acquired)) {
        return false;
    }
    if (!ExpectResult("第二个对象不能同时持锁", competing_lock.TryLock(), RedisLockResult::Busy)) {
        return false;
    }

    // Renew() 需要 token 匹配；成功说明当前 owner 可以安全刷新 TTL。
    if (!ExpectResult("当前 owner 续期", first_lock.Renew(), RedisLockResult::Acquired)) {
        return false;
    }
    if (!ExpectResult("当前 owner 解锁", first_lock.Unlock(), RedisLockResult::Acquired)) {
        return false;
    }

    // 第一把锁释放后，第二个对象应能获得同一个 key。
    if (!ExpectResult("解锁后第二个对象可抢锁", competing_lock.TryLock(), RedisLockResult::Acquired)) {
        return false;
    }
    return ExpectResult("第二个对象解锁", competing_lock.Unlock(), RedisLockResult::Acquired);
}

bool TestConcurrentMutualExclusion(const std::shared_ptr<RedisConnectionPool>& pool) {
    std::cout << "\n=== Test 2: " << kThreadCount << " 个线程竞争同一把锁 ===" << std::endl;
    const std::string key = CreateTestKey("concurrent");

    // start_condition 确保所有线程尽可能同时发起抢锁，才能真正覆盖并发竞争场景。
    std::mutex start_mutex;
    // 两类等待者不能共用一个条件变量：主线程等待“全部就绪”，工作线程等待“开始信号”。
    // 若共用 notify_one，通知可能唤醒另一个工作线程，主线程会错过最后一次 ready 通知而卡住。
    std::condition_variable ready_condition;
    std::condition_variable start_condition;
    int ready_threads = 0;
    bool start = false;

    std::atomic<int> successful_threads{0};
    std::atomic<int> active_critical_sections{0};
    std::atomic<int> max_active_critical_sections{0};
    std::mutex failure_mutex;
    std::vector<std::string> failures;
    std::vector<std::thread> workers;
    workers.reserve(kThreadCount);

    for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
        workers.emplace_back([&, thread_index] {
            {
                std::unique_lock<std::mutex> lock(start_mutex);
                ++ready_threads;
                ready_condition.notify_one();
                start_condition.wait(lock, [&start] { return start; });
            }

            RedisDistributedLock lock(pool, key, 2s);
            std::cout << "线程 " << thread_index << " 开始竞争锁" << std::endl;
            const RedisLockResult lock_result = lock.TryLockFor(3s, 5ms);
            if (lock_result != RedisLockResult::Acquired) {
                std::lock_guard<std::mutex> failure_lock(failure_mutex);
                failures.emplace_back("线程 " + std::to_string(thread_index)
                                      + " 未取得锁：" + ToString(lock_result));
                return;
            }

            // 若实现正确，任何时刻 active 都只能是 1。这个计数模拟真正的临界区。
            const int active_now = active_critical_sections.fetch_add(1) + 1;
            std::cout << "线程 " << thread_index << " 进入临界区，当前并发数："
                      << active_now << std::endl;
            int observed_max = max_active_critical_sections.load();
            while (observed_max < active_now
                   && !max_active_critical_sections.compare_exchange_weak(observed_max, active_now)) {
                // compare_exchange_weak 失败时会自动把最新 max 写入 observed_max，再继续比较。
            }

            // 故意持锁一小段时间，扩大其他线程与当前线程发生竞争的窗口。
            std::this_thread::sleep_for(30ms);
            active_critical_sections.fetch_sub(1);
            ++successful_threads;

            const RedisLockResult unlock_result = lock.Unlock();
            if (unlock_result != RedisLockResult::Acquired) {
                std::lock_guard<std::mutex> failure_lock(failure_mutex);
                failures.emplace_back("线程 " + std::to_string(thread_index)
                                      + " 解锁失败：" + ToString(unlock_result));
            }
        });
    }

    {
        std::unique_lock<std::mutex> lock(start_mutex);
        ready_condition.wait(lock, [&ready_threads] { return ready_threads == kThreadCount; });
        start = true;
    }
    start_condition.notify_all();

    for (std::thread& worker : workers) {
        worker.join();
    }

    std::cout << "成功进入临界区的线程数：" << successful_threads.load() << '/' << kThreadCount
              << "，临界区最大并发数：" << max_active_critical_sections.load() << std::endl;
    for (const std::string& failure : failures) {
        std::cerr << "[FAIL] " << failure << std::endl;
    }

    // 所有线程最终都应获得锁，并且最大临界区并发数必须始终为 1。
    return failures.empty() && successful_threads == kThreadCount
        && max_active_critical_sections == 1;
}

} // namespace

int main() {
    // 与项目 config.ini 一致：Redis 位于本机 6379，requirepass 为 123456。
    RedisConnectionConfig config;
    config.host = "127.0.0.1";
    config.port = kRedisPort;
    config.password = kRedisPassword;
    config.pool_size = kThreadCount;
    config.connect_timeout = 1000ms;

    auto pool = std::make_shared<RedisConnectionPool>(config);
    const bool lifecycle_ok = TestBasicLockLifecycle(pool);
    const bool concurrency_ok = lifecycle_ok && TestConcurrentMutualExclusion(pool);

    if (!lifecycle_ok || !concurrency_ok) {
        std::cerr << "\n分布式锁测试失败。请先运行 start_redis.bat，并确认 Redis 密码为 123456。"
                  << std::endl;
        return 1;
    }

    std::cout << "\n所有分布式锁测试通过。" << std::endl;
    return 0;
}
