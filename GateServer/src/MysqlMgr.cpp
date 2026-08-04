#include "MySqlMgr.h"
#include <iostream>
#include "ConfigMgr.h"
#include "const.h"


// 检查连接是否有效
bool IsConnectionValid(mysqlx::Session* session) {
    if (!session) return false;
    try {
        session->sql("SELECT 1").execute();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Connection invalid: " << e.what() << std::endl;
        return false;
    }
}


MySqlPool::MySqlPool()
{
    auto& config = ConfigMgr::Inst();
    this->host_ = config["Mysql"]["host"];
    this->port_ = config["Mysql"]["port"];
    this->user_ = config["Mysql"]["user"];
    this->url_ = host_ + ":" + port_;
    this->pass_ = config["Mysql"]["passwd"];
    this->schema_ = config["Mysql"]["schema"];
    this->poolSize_ = std::stoi(config["Mysql"]["poolsize"]);
    this->b_stop_ = false;
    // 循环创建指定数量的连接
    for (int i = 0; i < poolSize_; ++i) {
        // 使用构造函数直接创造
        auto session = std::make_unique<mysqlx::Session>(host_, std::stoi(port_), user_, pass_, schema_);
        pool_.push(std::move(session)); //unique_ptr使用移动语义
    }
     
}

/**
 * @brief 析构函数
 * 
 */
MySqlPool::~MySqlPool() {
    //在MysqlMgr中调用Close()函数
}

/**
 * @brief 创建新的 Session 连接
 * 
 * 核心流程：
 * 1. 解析 URL 获取 host 和 port
 * 2. 使用 mysqlx::Session 构造函数创建连接
 * 3. 返回 unique_ptr 管理连接对象
 * 
 * @return std::unique_ptr<mysqlx::Session> 新创建的会话
 */
std::unique_ptr<mysqlx::Session> MySqlPool::CreateSession() {
    
    // 直接构造 Session 对象，自动建立连接
    // 构造函数参数：host, port, user, password, database
    return std::make_unique<mysqlx::Session>(host_, std::stoi(port_), user_, pass_, schema_);
}

/**
 * @brief 从连接池获取连接
 * 
 * 核心流程：
 * 1. 加锁保护队列操作
 * 2. 如果池为空且未停止，阻塞等待条件变量
 * 3. 唤醒后检查停止标志
 * 4. 从队列头部取出连接
 * 5. 可选：检查连接是否有效，无效则创建新连接
 * 
 * @return std::unique_ptr<mysqlx::Session> 连接对象，停止时返回 nullptr
 */
std::unique_ptr<mysqlx::Session> MySqlPool::GetConnection() {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // 等待条件：池非空 或 连接池已停止
    cond_.wait(lock, [this] {
        return !pool_.empty() || b_stop_;
    });
    
    // 如果已停止，返回空指针
    if (b_stop_) {
        return nullptr;
    }
    
    // 从队列中取出连接
    std::unique_ptr<mysqlx::Session> con = std::move(pool_.front());
    pool_.pop();
    
    // 检查连接是否有效
    if (!con || !IsConnectionValid(con.get())) {
        // 连接无效，创建新连接替代
        con = CreateSession();
    }
    
    return con;
}

/**
 * @brief 归还连接到连接池
 * 
 * 核心流程：
 * 1. 加锁保护队列
 * 2. 如果池已停止，直接丢弃连接（unique_ptr 自动释放）
 * 3. 否则将连接放回队列
 * 4. 唤醒一个等待的线程
 * 
 * @param con 要归还的连接
 */
void MySqlPool::ReturnConnection(std::unique_ptr<mysqlx::Session> con) {
    if (!con) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 如果连接池已停止，直接丢弃连接
    if (b_stop_) {
        // con 会在函数结束时自动释放
        return;
    }
    
    // 检查连接状态，无效则丢弃
    if (!con || !IsConnectionValid(con.get())) {
        // 连接已断开，创建新的连接归还
        con = CreateSession();
    }
    
    // 归还连接到队列
    pool_.push(std::move(con));
    cond_.notify_one(); //唤醒一个等待的线程
}

/**
 * @brief 关闭连接池
 */
void MySqlPool::Close() {
    // 设置停止标志
    b_stop_ = true;
    
    // 唤醒所有等待的线程
    cond_.notify_all();
    
    // 清空队列
    std::lock_guard<std::mutex> lock(mutex_);
    while (!pool_.empty()) {
        pool_.pop();
    }
}

MysqlMgr::~MysqlMgr()
{
    pool_->Close();
}


bool MysqlMgr::RegUser(const std::string &name, const std::string &email, const std::string &password)
{
    auto con = pool_->GetConnection(); //获取连接
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); }); //自动归还连接
    try{
        if(con == nullptr){
            return false;
        }
        // 准备调用存储过程或执行 SQL 插入
        // 使用参数化查询防止 SQL 注入
        std::string sql = "INSERT INTO users (name, email, password) VALUES (?, ?, ?)";
        auto result = con->sql(sql).bind(name).bind(email).bind(password).execute();
        std::cout << "user:" << name << " email:" << email << " password:" << password 
                                                    << " register success" << std::endl;
        return true;
    }catch(const std::exception &e){
        std::cout << "Exception: " << e.what() << std::endl;
        return false;
    }
}

bool MysqlMgr::Checkuser(const::std::string & name)
{
    auto con = pool_->GetConnection(); //获取连接
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); }); //自动归还连接
    try{
        if(con == nullptr){
            return false;
        }
        // 查询用户是否存在，统计满足条件的行数
        std::string sql = "SELECT COUNT(*) FROM users WHERE name = ?";
        auto result = con->sql(sql).bind(name).execute();
        auto rows = result.fetchOne();
        if (rows) {
            int count = rows[0].get<int>();
            return count > 0;
        }
        return false;
    }catch(const std::exception &e){
        std::cout << "Exception: " << e.what() << std::endl;
        return false;
    }
}

bool MysqlMgr::isMatch(const ::std::string &name, std::string &email)
{
    auto con = pool_->GetConnection(); //获取连接
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); }); //自动归还连接
    try{
        if(con == nullptr){
            return false;
        }
        std::string sql = "SELECT COUNT(*) FROM users WHERE name = ? AND email = ?";
        auto result = con->sql(sql).bind(name).bind(email).execute();
        auto rows = result.fetchOne();
        if (rows) {
            int count = rows[0].get<int>();
            return count > 0;
        }
        return false;
    }catch(const std::exception &e){
        std::cout << "Exception: " << e.what() << std::endl;
        return false;
    }
}

/**
 * @brief 检查邮箱是否已注册
 * @param name 用户名（可扩展检查用户名）
 * @param email 邮箱
 * @return true 邮箱已存在，false 邮箱不存在
 */
bool MysqlMgr::Checkemail(const std::string &email)
{
    auto con = pool_->GetConnection(); //获取连接
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); }); //自动归还连接
    try{
        if(con == nullptr){
            return false;
        }
        // 查询邮箱是否存在，统计满足条件的行数
        std::string sql = "SELECT COUNT(*) FROM users WHERE email = ?";
        auto result = con->sql(sql).bind(email).execute();
        auto rows = result.fetchOne();
        if (rows) {
            int count = rows[0].get<int>();
            return count > 0;
        }
        return false;
    }catch(const std::exception &e){
        std::cout << "Exception: " << e.what() << std::endl;
        return false;
    }
}

/**
 * @brief 更新用户密码
 * @param name 用户名
 * @param newpwd 新密码
 * @return true 更新成功，false 更新失败
 */
bool MysqlMgr::UpdatePwd(const std::string &name, const std::string &newpwd)
{
    auto con = pool_->GetConnection(); //获取连接
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); }); //自动归还连接
    try{
        if(con == nullptr){
            return false;
        }
        // 更新密码，update不返回结果集
        std::string sql = "UPDATE users SET password = ? WHERE name = ?";
        auto result = con->sql(sql).bind(newpwd).bind(name).execute();
        return true; //没有异常就是成功
    }catch(const std::exception &e){
        std::cout << "Exception: " << e.what() << std::endl;
        return false;
    }
}

/**
 * @brief 验证用户密码
 * @param name 用户名
 * @param pwd 输入的密码
 * @param userInfo 输出参数，存储用户信息
 * @return true 密码正确，false 密码错误或用户不存在
 * 
 * 为什么要返回用户信息？因为这些数据在后续的操作要用到的，防止再查一次
 */

bool MysqlMgr::CheckPwd(const std::string &name, const std::string &pwd, UserInfo &userInfo)
{
    auto con = pool_->GetConnection(); //获取连接
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); }); //自动归还连接
    try{
        if(con == nullptr){
            return false;
        }
        // 查询用户信息
        std::string sql = "SELECT uid, name, email, password FROM users WHERE name = ?";
        auto result = con->sql(sql).bind(name).execute();
        auto rows = result.fetchOne(); //获取一行返回
        
        if (!rows) {
            return false;
        }
        
        std::string storedPassword = rows[3].get<std::string>();
        
        // 验证密码
        if (storedPassword != pwd) {
            return false;
        }
        
        // 填充用户信息
        userInfo.uid = rows[0].get<int>();
        userInfo.user = rows[1].get<std::string>();
        userInfo.email = rows[2].get<std::string>();
        userInfo.passwd = storedPassword;
        
        return true;
    }catch(const std::exception &e){
        std::cout << "Exception: " << e.what() << std::endl;
        return false;
    }
}

MysqlMgr::MysqlMgr()
{   
    //连接池构造函数里面实现了读取配置
    pool_.reset(new MySqlPool); 
}