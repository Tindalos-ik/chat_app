#pragma once
#ifndef MYSQL_MGR_H
#define MYSQL_MGR_H

#include <mysqlx/xdevapi.h>  // X DevAPI 头文件
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include "singleton.h"

/**
 * @class MySqlPool
 * @brief MySQL 连接池类，继承自 Singleton 单例模板
 * 
 * 管理一个预先创建的 Session 连接池，提供线程安全的连接获取和归还功能。
 * 使用 X DevAPI（端口 33060）而非传统 JDBC API（端口 3306）。
 */
class MySqlPool{

public:
    /**
     * @brief 构造函数，初始化连接池
     * 
     * 创建指定数量的 Session 连接并放入池中。
     * X DevAPI 的连接字符串格式：mysqlx://user:password@host:33060/schema
     * 
     */
    MySqlPool();

    ~MySqlPool();

    /**
     * @brief 从连接池中获取一个连接
     * 
     * 如果池中有空闲连接，直接返回；否则阻塞等待直到有空闲连接或连接池关闭。
     * 使用 RAII 模式，返回的 unique_ptr 会在析构时自动调用 returnConnection。
     * 
     * @return std::unique_ptr<mysqlx::Session> 连接对象的智能指针，池关闭时返回 nullptr
     */
    std::unique_ptr<mysqlx::Session> GetConnection();

    /**
     * @brief 将连接归还到连接池
     * 
     * 调用此函数将使用完毕的连接放回池中，供其他线程使用。
     * 注意：归还前应确保连接状态正常，可以通过 ping() 检查。
     * 
     * @param con 要归还的连接对象
     */
    void ReturnConnection(std::unique_ptr<mysqlx::Session> con);

    /**
     * @brief 关闭整个连接池
     * 
     * 设置停止标志，唤醒所有等待的线程，并清空连接池。
     * 调用后 GetConnection 将返回 nullptr。
     */
    void Close();


private:

    /**
     * @brief 创建一个新的 MySQL X DevAPI 会话连接
     * 
     * 根据当前配置创建 Session 对象，并设置默认 schema。
     * mysql的连接长时间不用会自动断开，redis不会
     * 
     * @return std::unique_ptr<mysqlx::Session> 新创建的会话指针
     */
    std::unique_ptr<mysqlx::Session> CreateSession();

    // 连接配置信息
    std::string url_;       // MySQL 服务器连接地址（X Protocol 端口 33060）
    std::string host_;      // 主机名或 IP 地址
    std::string port_;       // 端口号（默认 33060）
    std::string user_;      
    std::string pass_; 
    std::string schema_;    // 默认使用的数据库名称
    int poolSize_;       

    std::queue<std::unique_ptr<mysqlx::Session>> pool_;  //空闲连接队列
    mutable std::mutex mutex_;                           
    std::condition_variable cond_;                   
    std::atomic<bool> b_stop_;                      
};

struct UserInfo{
    std::string user;
    std::string email;
    std::string passwd;
    int uid; // 用户ID
};

//数据库操作类
class MysqlMgr : public Singleton<MysqlMgr>{
    friend class Singleton<MysqlMgr>;
public:
    ~MysqlMgr(); //析构的时候先调用这个析构再去调用pool的析构，所以手动关闭连接池

    //注册用户
    bool RegUser(const std::string& name, const std::string& email, const std::string& password);

    //查找用户是否存在
    bool Checkuser(const::std::string& name);

    //检查用户名和邮箱是否匹配
    bool isMatch(const::std::string& name,std::string& email);

    //检查邮箱是否已经注册
    bool Checkemail(const std::string& email);

    //更新密码
    bool UpdatePwd(const std::string& name, const std::string& newpwd);

    //检查用户名和密码是否匹配
    bool CheckPwd(const std::string& name, const std::string& pwd, UserInfo& userInfo);

private:
    MysqlMgr();

    std::unique_ptr<MySqlPool> pool_; 
};

#endif // MYSQL_MGR_H