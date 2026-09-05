#include "MySqlMgr.h"
#include <iostream>
#include <vector>
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
        // 注册 = 分配 uid + 插入用户，包在一个事务里保证原子性
        // 1. user_id 表自增分配 uid（单行 UPDATE 天然串行，并发安全）
        con->startTransaction();
        con->sql("UPDATE user_id SET id = id + 1").execute();
        auto res = con->sql("SELECT id FROM user_id").execute();
        auto row = res.fetchOne();
        if(!row){
            con->rollback();
            return false;
        }
        int uid = row[0].get<int>();

        // 2. 插入用户（带 uid），参数化查询防止 SQL 注入
        std::string sql = "INSERT INTO user (uid, name, email, pwd) VALUES (?, ?, ?, ?)";
        con->sql(sql).bind(uid).bind(name).bind(email).bind(password).execute();
        con->commit();
        std::cout << "user:" << name << " email:" << email << " password:" << password 
                                                    << " register success" << std::endl;
        return true;
    }catch(const std::exception &e){
        // 任一步失败都回滚，避免 user_id 自增了但用户没插进去
        try { con->rollback(); } catch (...) {}
        std::cout << "Exception: " << e.what() << std::endl;
        return false;
    }
}

bool MysqlMgr::Checkuser(const std::string & name)
{
    auto con = pool_->GetConnection(); //获取连接
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); }); //自动归还连接
    try{
        if(con == nullptr){
            return false;
        }
        // 查询用户是否存在，统计满足条件的行数
        std::string sql = "SELECT COUNT(*) FROM user WHERE name = ?";
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

/**
 * @brief 根据用户名查询用户，把匹配到的用户信息返回给调用方
 * @param name 要查询的用户名
 * @param userVec 输出参数：匹配到的用户信息列表（调用前内容会被清空）
 * @return true 查到至少一个用户；false 未查到或查询出错
 *
 * 和单参数版 Checkuser 的区别：单参数版只回答"存不存在"，
 * 这个版本把完整的用户资料（uid/昵称/头像/签名等）一并带出来，
 * 避免调用方查到人之后还要再查一次数据库。
 */
bool MysqlMgr::Checkuser(const std::string &name, std::vector<UserInfo> &userVec)
{
    auto con = pool_->GetConnection(); //获取连接
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); }); //自动归还连接
    try{
        if(con == nullptr){
            return false;
        }

        // 先清空输出容器，保证调用方拿到的是本次查询的完整结果，而不是新旧数据混在一起
        userVec.clear();

        // name 列只建了普通索引、不是唯一索引，理论上可能查到多条，所以用 fetchAll 取全部行
        // desc 是 MySQL 保留字，SQL 里必须用反引号括起来
        std::string sql = "SELECT uid, name, email, pwd, nick, `desc`, sex, icon FROM user WHERE name = ?";
        auto result = con->sql(sql).bind(name).execute();
        auto rows = result.fetchAll();

        // 逐行填充 UserInfo，列下标从 0 开始，和 SELECT 的列顺序一一对应
        for (const auto &row : rows) {
            UserInfo info;
            info.uid    = row[0].get<int>();
            info.user   = row[1].get<std::string>();
            info.email  = row[2].get<std::string>();
            info.passwd = row[3].get<std::string>();
            info.nick   = row[4].get<std::string>();
            info.desc   = row[5].get<std::string>();
            info.sex    = row[6].get<int>();
            info.icon   = row[7].get<std::string>();
            userVec.push_back(std::move(info));
        }

        return !userVec.empty();
    }catch(const std::exception &e){
        std::cout << "Exception: " << e.what() << std::endl;
        return false;
    }
}

/**
 * @brief 根据 uid 判断用户是否存在
 * @param uid 用户ID
 * @return true 存在；false 不存在或查询出错
 *
 * 和 Checkuser(name) 对称：注册/登录按用户名查，其他业务（如加好友、发消息）
 * 拿到的是 uid，判断身份时直接按 uid 查更顺手。
 */
bool MysqlMgr::Checkuid(int uid)
{
    auto con = pool_->GetConnection(); //获取连接
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); }); //自动归还连接
    try{
        if(con == nullptr){
            return false;
        }
        // 统计满足条件的行数，> 0 就说明该 uid 已注册
        std::string sql = "SELECT COUNT(*) FROM user WHERE uid = ?";
        auto result = con->sql(sql).bind(uid).execute();
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
 * @brief 根据 uid 查询用户，把匹配到的用户信息返回给调用方
 * @param uid 用户ID（user 表里 uid 是唯一索引，最多返回一条）
 * @param userVec 输出参数：匹配到的用户信息列表（调用前内容会被清空）
 * @return true 查到该用户；false 未查到或查询出错
 */
bool MysqlMgr::Checkuid(int uid, std::vector<UserInfo> &userVec)
{
    auto con = pool_->GetConnection(); //获取连接
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); }); //自动归还连接
    try{
        if(con == nullptr){
            return false;
        }

        userVec.clear();

        // uid 是唯一索引，实际最多返回一行；和 GetUserInfo 保持同一套列顺序
        std::string sql = "SELECT uid, name, email, pwd, nick, `desc`, sex, icon FROM user WHERE uid = ?";
        auto result = con->sql(sql).bind(uid).execute();
        auto rows = result.fetchAll();

        for (const auto &row : rows) {
            UserInfo info;
            info.uid    = row[0].get<int>();
            info.user   = row[1].get<std::string>();
            info.email  = row[2].get<std::string>();
            info.passwd = row[3].get<std::string>();
            info.nick   = row[4].get<std::string>();
            info.desc   = row[5].get<std::string>();
            info.sex    = row[6].get<int>();
            info.icon   = row[7].get<std::string>();
            userVec.push_back(std::move(info));
        }

        return !userVec.empty();
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
        std::string sql = "SELECT COUNT(*) FROM user WHERE name = ? AND email = ?";
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
        std::string sql = "SELECT COUNT(*) FROM user WHERE email = ?";
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
        std::string sql = "UPDATE user SET pwd = ? WHERE name = ?";
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
        std::string sql = "SELECT uid, name, email, pwd FROM user WHERE name = ?";
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


// 按uid查询用户信息，登录成功后返回客户端用于渲染界面
// 查询不到或出错时返回默认 UserInfo（uid == 0，调用方据此判断）
UserInfo MysqlMgr::GetUserInfo(int uid){
    UserInfo user_info;

    auto con = pool_->GetConnection(); //获取连接
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); }); //自动归还连接
    try{
        if(con == nullptr){
            return user_info;
        }
        // desc 是 MySQL 保留字，必须用反引号括起来
        std::string sql = "SELECT uid, name, email, pwd, nick, `desc`, sex, icon FROM user WHERE uid = ?";
        auto result = con->sql(sql).bind(uid).execute();
        auto rows = result.fetchOne();

        if(!rows){
            // 用户不存在
            return user_info;
        }

        // 填充用户信息，列下标从0开始，与SELECT顺序一一对应
        user_info.uid = rows[0].get<int>();
        user_info.user = rows[1].get<std::string>();
        user_info.email = rows[2].get<std::string>();
        user_info.passwd = rows[3].get<std::string>();
        user_info.nick = rows[4].get<std::string>();
        user_info.desc = rows[5].get<std::string>();
        user_info.sex = rows[6].get<int>();
        user_info.icon = rows[7].get<std::string>();

        return user_info;
    }catch(const std::exception &e){
        std::cout << "Exception: " << e.what() << std::endl;
        return user_info;
    }
}
