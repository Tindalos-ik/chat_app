#pragma once
#ifndef REDIS_MGR_H
#define REDIS_MGR_H

#include "singleton.h"
#include <memory>
#include <hiredis/hiredis.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

 /* 需要引入redis连接池，为什么呢？
    最开始的单例模式在多线程环境下不是线程安全的，我们引入池的概念，用多个连接去访问redis
    池都是生产者-消费者模型
    */

//和之前的AsIOServicePool类似，重要的是这种池的思想以及如何封装
class RedisConPool {
public:
    RedisConPool(size_t poolSize, const char* host, int port, const char* pwd)
        : poolSize_(poolSize), host_(host), port_(port), b_stop_(false){
        //初始化连接池
        for (size_t i = 0; i < poolSize_; ++i) {
            auto* context = redisConnect(host, port);
            if (context == nullptr || context->err != 0) {
                if (context != nullptr) {
                    redisFree(context);
                }
                continue;
            }

            auto reply = (redisReply*)redisCommand(context, "AUTH %s", pwd);
            if (reply->type == REDIS_REPLY_ERROR) {
                std::cout << "auth failed" << std::endl;
                freeReplyObject(reply);
                redisFree(context);
                continue;
            }

            freeReplyObject(reply);
            std::cout << "auth success" << std::endl;
            connections_.push(context);
        }

    }

    ~RedisConPool() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!connections_.empty()) {
            auto* con = connections_.front();
            redisFree(con);
            connections_.pop();
        }
    }

    redisContext* getConnection() {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this] { 
            if (b_stop_) {
                return true;
            }
            return !connections_.empty(); 
            });
        //如果停止则直接返回空指针
        if (b_stop_) {
            return  nullptr;
        }
        auto* context = connections_.front();
        connections_.pop();
        return context;
    }

    void returnConnection(redisContext* context) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (b_stop_) {
            return;
        }
        connections_.push(context);
        cond_.notify_one(); //唤醒一个等待的线程
    }

    void Close() { 
        b_stop_ = true;
        cond_.notify_all();
    }

private:
    std::atomic<bool> b_stop_;
    size_t poolSize_;
    const char* host_;
    int port_;
    std::queue<redisContext*> connections_; //队列存储连接池
    std::mutex mutex_;
    std::condition_variable cond_;
};



/**
 * @brief Redis 管理器（单例模式）
 * 基于 hiredis C 库封装的 Redis 客户端管理类
 * 提供基础的 Redis 操作：连接、认证、键值对、队列、哈希等
 * hiredis 是 C 库，需要手动管理 redisContext 和 redisReply 的内存
 * 
 */
class RedisMgr : public Singleton<RedisMgr>, 
                 public std::enable_shared_from_this<RedisMgr>
{
    friend class Singleton<RedisMgr>;

public:
    ~RedisMgr();

    /**
     * @brief 连接到 Redis 服务器
     * @param host Redis 服务器地址（如 "127.0.0.1"）
     * @param port Redis 服务器端口（默认 6379）
     * @return true 连接成功，false 连接失败
     */
    //bool Connect(const std::string& host, int port); 用上连接池就不要了

    /**
     * @brief 获取指定 key 的值
     * @param key 键名
     * @param value 输出参数，存储获取到的值
     * @return true 获取成功，false 获取失败或 key 不存在
     */
    bool Get(const std::string& key, std::string& value);

    /**
     * @brief 设置指定 key 的值
     * @param key 键名
     * @param value 键值
     * @return true 设置成功，false 设置失败
     */
    bool Set(const std::string& key, const std::string& value);

    /**
     * @brief Redis 密码认证（如果 Redis 设置了 requirepass）
     * @param password 密码
     * @return true 认证成功，false 认证失败
     */
    bool Auth(const std::string& password);

    /**
     * @brief 左侧推入（队列头部插入）
     * @param key 键名
     * @param value 要推入的值
     * @return true 成功，false 失败
     */
    bool LPush(const std::string& key, const std::string& value);

    /**
     * @brief 左侧弹出（从队列头部取出）
     * @param key 键名
     * @param value 输出参数，存储取出的值
     * @return true 成功，false 失败或队列为空
     */
    bool LPop(const std::string& key, std::string& value);

    /**
     * @brief 右侧推入（队列尾部插入）
     * @param key 键名
     * @param value 要推入的值
     * @return true 成功，false 失败
     */
    bool RPush(const std::string& key, const std::string& value);

    /**
     * @brief 右侧弹出（从队列尾部取出）
     * @param key 键名
     * @param value 输出参数，存储取出的值
     * @return true 成功，false 失败或队列为空
     */
    bool RPop(const std::string& key, std::string& value);

    /**
     * @brief 设置哈希表中的字段值（C++ string 版本）
     * @param key 哈希表键名
     * @param hkey 字段名
     * @param value 字段值
     * @return true 成功，false 失败
     */
    bool HSet(const std::string& key, const std::string& hkey, const std::string& value);

    /**
     * @brief 设置哈希表中的字段值（C 字符串版本，带长度）
     * @param key 哈希表键名
     * @param hkey 字段名
     * @param hvalue 字段值
     * @param hvaluelen 字段值长度
     * @return true 成功，false 失败
     */
    bool HSet(const char* key, const char* hkey, const char* hvalue, size_t hvaluelen);

    /**
     * @brief 获取哈希表中指定字段的值
     * @param key 哈希表键名
     * @param hkey 字段名
     * @return std::string 字段值，如果不存在返回空字符串
     */
    std::string HGet(const std::string& key, const std::string& hkey);

    /**
     * @brief 删除指定的 key
     * @param key 要删除的键名
     * @return true 删除成功，false 删除失败或 key 不存在
     */
    bool Del(const std::string& key);

    bool HDel(const std::string& key, const std::string& field);

    /**
     * @brief 原子增加哈希表中字段的值（Redis HINCRBY，避免读改写丢更新）
     * @param key 哈希表键名
     * @param field 字段名
     * @param incr 增量，可为负数
     * @return true 成功，false 失败
     */
    bool HIncrBy(const std::string& key, const std::string& field, int incr);

    /**
     * @brief 检查 key 是否存在
     * @param key 键名
     * @return true 存在，false 不存在
     */
    bool ExistsKey(const std::string& key);

    /**
     * @brief 关闭 Redis 连接，释放资源
     */
    void Close(); 

private:
    RedisMgr();

    //redisContext* _connect;   // Redis 连接上下文，管理底层 socket 连接
    //redisReply* _reply;       // Redis 命令执行后的回复对象，使用后需要 freeReplyObject()

    std::unique_ptr<RedisConPool> _connectionPool; //连接池
};

#endif // !REDIS_MGR_H
