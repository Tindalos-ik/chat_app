#include "MySqlMgr.h"
#include <algorithm>
#include <iostream>
#include <vector>
#include <sstream>
#include <limits>
#include <json.h>
#include "ConfigMgr.h"
#include "const.h"
#include "RedisMgr.h"

// 把 UserInfo 序列化成 JSON 对象，字段与登录缓存的 ubaseinfo_<uid> 保持一致
static void FillUserJson(const UserInfo &info, Json::Value &obj)
{
    obj["uid"]    = info.uid;
    obj["user"]   = info.user;
    obj["pwd"]    = info.passwd;
    obj["email"]  = info.email;
    obj["nick"]   = info.nick;
    obj["desc"]   = info.desc;
    obj["sex"]    = info.sex;
    obj["icon"]   = info.icon;
}

// 从 JSON 对象还原 UserInfo；格式不对返回 false，调用方忽略这份缓存
static bool ParseUserJson(const Json::Value &obj, UserInfo &info)
{
    if (!obj.isObject() || !obj.isMember("uid")) {
        return false;
    }
    info.uid    = obj["uid"].asInt();
    info.user   = obj["user"].asString();
    info.passwd = obj["pwd"].asString();
    info.email  = obj["email"].asString();
    info.nick   = obj["nick"].asString();
    info.desc   = obj["desc"].asString();
    info.sex    = obj["sex"].asInt();
    info.icon   = obj["icon"].asString();
    return true;
}


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
 * @brief 根据用户名查询用户，把用户信息返回给调用方
 * @param name 要查询的用户名（网关保证唯一，最多返回一条）
 * @param userInfo 输出参数：查到的用户信息
 * @return true 查到该用户；false 未查到或查询出错
 *
 * 缓存策略：先查 Redis（unameinfo_<name>），没命中再查 MySQL，查到后写回 Redis。
 * 和单参数版 Checkuser 的区别：单参数版只回答"存不存在"，这个版本把完整用户资料带出来。
 */
bool MysqlMgr::Checkuser(const std::string &name, UserInfo &userInfo)
{
    // 1) Redis 缓存优先：unameinfo_<name> 存的是单个 JSON 对象
    std::string cache_key = USER_NAME_INFO + name;
    std::string cache_str;
    if (RedisMgr::GetInstance()->Get(cache_key, cache_str)) {
        Json::CharReaderBuilder reader;
        Json::Value root;
        std::istringstream ss(cache_str);
        std::string errs;
        if (Json::parseFromStream(reader, ss, &root, &errs) && ParseUserJson(root, userInfo)) {
            return true;
        }
        // 缓存内容损坏：忽略，落到 MySQL 重新查并回写
    }

    // 2) 缓存未命中 -> 查 MySQL（用户名唯一，fetchOne 取一行即可）
    auto con = pool_->GetConnection(); //获取连接
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); }); //自动归还连接
    try{
        if(con == nullptr){
            return false;
        }

        // desc 是 MySQL 保留字，SQL 里必须用反引号括起来
        std::string sql = "SELECT uid, name, email, pwd, nick, `desc`, sex, icon FROM user WHERE name = ?";
        auto result = con->sql(sql).bind(name).execute();
        auto rows = result.fetchOne();
        if (!rows) {
            return false; // 用户不存在，不写缓存
        }

        // 填充输出参数，列下标从 0 开始，和 SELECT 的列顺序一一对应
        userInfo.uid    = rows[0].get<int>();
        userInfo.user   = rows[1].get<std::string>();
        userInfo.email  = rows[2].get<std::string>();
        userInfo.passwd = rows[3].get<std::string>();
        userInfo.nick   = rows[4].get<std::string>();
        userInfo.desc   = rows[5].get<std::string>();
        userInfo.sex    = rows[6].get<int>();
        userInfo.icon   = rows[7].get<std::string>();

        // 3) 把用户信息写回 Redis，下次直接命中，不用再查库
        Json::Value obj;
        FillUserJson(userInfo, obj);
        RedisMgr::GetInstance()->Set(cache_key, obj.toStyledString());

        return true;
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
 * @brief 根据 uid 查询用户，把用户信息返回给调用方
 * @param uid 用户ID（user 表里 uid 是唯一索引，最多返回一条）
 * @param userInfo 输出参数：查到的用户信息
 * @return true 查到该用户；false 未查到或查询出错
 *
 * 缓存策略：先查 Redis（ubaseinfo_<uid>，与登录缓存共用），没命中再查 MySQL，查到后写回 Redis。
 */
bool MysqlMgr::Checkuid(int uid, UserInfo &userInfo)
{
    // 1) Redis 缓存优先：ubaseinfo_<uid> 与登录缓存共用同一份，存的是单个 JSON 对象
    std::string cache_key = USER_BASE_INFO + std::to_string(uid);
    std::string cache_str;
    if (RedisMgr::GetInstance()->Get(cache_key, cache_str)) {
        Json::CharReaderBuilder reader;
        Json::Value root;
        std::istringstream ss(cache_str);
        std::string errs;
        if (Json::parseFromStream(reader, ss, &root, &errs) && root.isObject() &&
            root.isMember("uid") && root["uid"].asInt() == uid) {
            if (ParseUserJson(root, userInfo)) {
                return true;
            }
        }
        // 缓存不存在 / 内容损坏 / uid 不匹配：忽略，落到 MySQL 重新查并回写
    }

    // 2) 缓存未命中 -> 查 MySQL
    auto con = pool_->GetConnection(); //获取连接
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); }); //自动归还连接
    try{
        if(con == nullptr){
            return false;
        }

        // uid 是唯一索引，实际最多返回一行；和 GetUserInfo 保持同一套列顺序
        std::string sql = "SELECT uid, name, email, pwd, nick, `desc`, sex, icon FROM user WHERE uid = ?";
        auto result = con->sql(sql).bind(uid).execute();
        auto rows = result.fetchOne();
        if (!rows) {
            return false; // 用户不存在，不写缓存
        }

        userInfo.uid    = rows[0].get<int>();
        userInfo.user   = rows[1].get<std::string>();
        userInfo.email  = rows[2].get<std::string>();
        userInfo.passwd = rows[3].get<std::string>();
        userInfo.nick   = rows[4].get<std::string>();
        userInfo.desc   = rows[5].get<std::string>();
        userInfo.sex    = rows[6].get<int>();
        userInfo.icon   = rows[7].get<std::string>();

        // 3) 写回 Redis（对象格式与登录缓存 ubaseinfo_<uid> 一致）
        Json::Value obj;
        FillUserJson(userInfo, obj);
        RedisMgr::GetInstance()->Set(cache_key, obj.toStyledString());

        return true;
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

bool MysqlMgr::UpdateUserProfile(int uid, const std::string& nick,
                                 const std::string& desc, const std::string& icon)
{
    auto con = pool_->GetConnection();
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); });
    try {
        if (con == nullptr) {
            return false;
        }

        // fetchOne 会推进结果集游标，因此 SqlResult 不能声明为 const。
        auto name_result = con->sql("SELECT name FROM user WHERE uid = ?")
                               .bind(uid).execute();
        const auto name_row = name_result.fetchOne();
        if (!name_row) {
            return false;
        }
        const std::string user_name = name_row[0].get<std::string>();

        // desc 是 MySQL 保留字，必须使用反引号；bind 防止资料内容进入 SQL 结构。
        const std::string sql =
            "UPDATE user SET nick = ?, `desc` = ?, icon = ? WHERE uid = ?";
        con->sql(sql).bind(nick).bind(desc).bind(icon).bind(uid).execute();

        // 两种查询入口各有一份缓存，更新后都失效，后续读取统一回源 MySQL。
        RedisMgr::GetInstance()->Del(USER_BASE_INFO + std::to_string(uid));
        RedisMgr::GetInstance()->Del(USER_NAME_INFO + user_name);
        return true;
    } catch (const std::exception &e) {
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


bool MysqlMgr::AddFriendApply(
    int applicantUid,
    int recipientUid,
    const std::string& applicantRemark){
    auto con = pool_->GetConnection(); //获取连接
    if(con == nullptr) return false;
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); }); //自动归还连接

    try{
        if (applicantUid <= 0 || recipientUid <= 0 || applicantUid == recipientUid) {
            return false;
        }
        // friend_apply 通过 (from_uid, to_uid) 唯一索引避免重复申请。
        // 待处理申请允许更新申请方备注；终态申请不允许被新请求覆盖。
        const std::string sql =
            "INSERT INTO friend_apply (from_uid, to_uid, applicant_remark, status) VALUES (?, ?, ?, 0) "
            "ON DUPLICATE KEY UPDATE applicant_remark = "
            "IF(status = 0, VALUES(applicant_remark), applicant_remark)";
        con->sql(sql).bind(applicantUid).bind(recipientUid).bind(applicantRemark).execute();
        return true;
    }catch(const std::exception &e){
        std::cout << "Exception: " << e.what() << std::endl;
        return false;
    }
}

bool MysqlMgr::GetFriendApplyInfo(
    int recipientUid,
    std::vector<std::shared_ptr<ApplyInfo>>& applications){
    if (recipientUid <= 0) {
        return false;
    }

    auto con = pool_->GetConnection();
    if (con == nullptr) {
        return false;
    }
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); });

    try {
        // friend_apply 记录申请关系和处理状态，user 提供申请方的展示资料。
        // 使用自增 id 倒序表示最新申请优先。
        const std::string sql =
            "SELECT u.uid, u.name, u.`desc`, u.icon, u.nick, fa.status "
            "FROM friend_apply AS fa "
            "INNER JOIN user AS u ON fa.from_uid = u.uid "
            "WHERE fa.to_uid = ? "
            "ORDER BY fa.id DESC";
        auto result = con->sql(sql).bind(recipientUid).execute();

        // 仅在整个查询成功后更新输出参数，调用方不会看到部分结果。
        std::vector<std::shared_ptr<ApplyInfo>> queriedApplications;
        for (const auto& row : result.fetchAll()) {
            auto application = std::make_shared<ApplyInfo>();
            application->_uid = row[0].get<int>();
            application->_user = row[1].get<std::string>();
            application->_desc = row[2].get<std::string>();
            application->_icon = row[3].get<std::string>();
            application->_nick = row[4].get<std::string>();
            application->_status = row[5].get<int>();
            queriedApplications.push_back(std::move(application));
        }

        applications = std::move(queriedApplications);
        return true;
    } catch (const std::exception& e) {
        std::cout << "Exception: " << e.what() << std::endl;
        return false;
    }
}

bool MysqlMgr::UpdateFriendApplyStatus(
    int applicantUid,
    int recipientUid,
    int newStatus){
    constexpr int kAcceptedStatus = 1;
    constexpr int kRejectedStatus = 2;
    if (applicantUid <= 0 || recipientUid <= 0 || applicantUid == recipientUid ||
        (newStatus != kAcceptedStatus && newStatus != kRejectedStatus)) {
        return false;
    }

    auto con = pool_->GetConnection();
    if (con == nullptr) {
        return false;
    }
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); });

    try {
        // 终态不能互相覆盖：只允许待处理(0)转为本次终态，或重复提交相同终态。
        const std::string updateSql =
            "UPDATE friend_apply SET status = ? "
            "WHERE from_uid = ? AND to_uid = ? AND (status = 0 OR status = ?)";
        auto result = con->sql(updateSql)
                          .bind(newStatus)
                          .bind(applicantUid)
                          .bind(recipientUid)
                          .bind(newStatus)
                          .execute();
        if (result.getAffectedItemsCount() > 0) {
            return true;
        }

        // MySQL 对“赋值为原值”的 UPDATE 可返回 0；再次确认保证重试语义正确。
        const std::string querySql =
            "SELECT status FROM friend_apply WHERE from_uid = ? AND to_uid = ?";
        auto row = con->sql(querySql).bind(applicantUid).bind(recipientUid).execute().fetchOne();
        return row && row[0].get<int>() == newStatus;
    } catch (const std::exception& e) {
        std::cout << "Exception: " << e.what() << std::endl;
        return false;
    }
}

bool MysqlMgr::AddFriend(
    int recipientUid,
    int applicantUid,
    const std::string& recipientRemark,
    std::vector<FriendAuthMessage>& authMessages){
    // 只有整个事务成功后才把消息交给调用方，避免认证失败仍向客户端显示“已是好友”。
    authMessages.clear();
    if (recipientUid <= 0 || applicantUid <= 0 || recipientUid == applicantUid) {
        return false;
    }

    auto con = pool_->GetConnection();
    if (con == nullptr) {
        return false;
    }
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); });

    try {
        // 锁住申请记录后确认状态，再写入双向关系，避免“已同意但好友只加了一边”。
        con->startTransaction(); // 事务开始
        const std::string applicationSql =
            "SELECT status, applicant_remark FROM friend_apply "
            "WHERE from_uid = ? AND to_uid = ? FOR UPDATE";
        auto applicationRow = con->sql(applicationSql)
                                  .bind(applicantUid)
                                  .bind(recipientUid)
                                  .execute()
                                  .fetchOne();
        if (!applicationRow) {
            con->rollback();
            return false;
        }

        constexpr int kPendingStatus = 0;
        constexpr int kAcceptedStatus = 1;
        bool acceptedNow = false;
        const int applicationStatus = applicationRow[0].get<int>();
        if (applicationStatus == kPendingStatus) {
            const std::string acceptSql =
                "UPDATE friend_apply SET status = ? WHERE from_uid = ? AND to_uid = ?";
            con->sql(acceptSql)
                .bind(kAcceptedStatus)
                .bind(applicantUid)
                .bind(recipientUid)
                .execute();
            acceptedNow = true;
        } else if (applicationStatus != kAcceptedStatus) {
            con->rollback();
            return false;
        }
        const std::string applicantRemark = applicationRow[1].get<std::string>();

        const std::string insertSql =
            "INSERT INTO friend (self_id, friend_id, back) VALUES (?, ?, ?) "
            "ON DUPLICATE KEY UPDATE back = back";

        // 接收者确认时填写的备注，仅保存到接收者自己的好友记录。
        con->sql(insertSql)
            .bind(recipientUid)
            .bind(applicantUid)
            .bind(recipientRemark)
            .execute();

        // 申请方在发起申请时填写的备注，保存到申请方自己的好友记录。
        con->sql(insertSql)
            .bind(applicantUid)
            .bind(recipientUid)
            .bind(applicantRemark)
            .execute();

        // 好友关系和私聊必须在同一个事务内完成。双方 uid 固定按小到大保存，
        // 才能和 private_chat 的唯一索引 (user1_id, user2_id) 对应起来。
        const int uid1 = std::min(recipientUid, applicantUid);
        const int uid2 = std::max(recipientUid, applicantUid);
        std::uint64_t threadId = 0;
        const auto existingThread = con->sql(
            "SELECT thread_id FROM private_chat "
            "WHERE user1_id = ? AND user2_id = ? FOR UPDATE")
            .bind(uid1)
            .bind(uid2)
            .execute()
            .fetchOne();
        if (existingThread) {
            threadId = existingThread[0].get<std::uint64_t>();
        } else {
            auto createThreadResult = con->sql(
                "INSERT INTO chat_thread (type, created_at) VALUES ('private', NOW())").execute();
            const std::uint64_t newThreadId = createThreadResult.getAutoIncrementValue();
            if (newThreadId == 0) {
                con->rollback();
                return false;
            }

            // 唯一索引是最终并发保障。重复键时 LAST_INSERT_ID(thread_id) 会返回赢家，
            // 本次临时创建的 chat_thread 随后删除，避免留下无法访问的孤儿会话。
            con->sql(
                "INSERT INTO private_chat (thread_id, user1_id, user2_id, created_at) "
                "VALUES (LAST_INSERT_ID(?), ?, ?, NOW()) "
                "ON DUPLICATE KEY UPDATE thread_id = LAST_INSERT_ID(thread_id)")
                .bind(newThreadId)
                .bind(uid1)
                .bind(uid2)
                .execute();
            const auto threadIdRow = con->sql("SELECT LAST_INSERT_ID()").execute().fetchOne();
            if (!threadIdRow) {
                con->rollback();
                return false;
            }
            threadId = threadIdRow[0].get<std::uint64_t>();
            if (threadId != newThreadId) {
                con->sql("DELETE FROM chat_thread WHERE id = ?").bind(newThreadId).execute();
            }
        }

        // textmsgs 的 msg_id/thread_id 当前是 proto int32。数据库使用 BIGINT，超过
        // int32 范围时拒绝本次事务，不能静默截断成错误的本地会话或消息 ID。
        if (threadId == 0 || threadId > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            con->rollback();
            return false;
        }

        constexpr const char* kFriendAuthContent = "我们已经是好友了，开始聊天吧。";
        std::uint64_t messageId = 0;
        // 重复点击“同意”应保持幂等：第一次认证写入初始消息；后续请求复用同一条，
        // 让网络重试仍可把正确的 message_id 推给客户端。
        if (!acceptedNow) {
            const auto existingMessage = con->sql(
                "SELECT message_id FROM chat_message "
                "WHERE thread_id = ? AND sender_id = ? AND recv_id = ? AND content = ? "
                "ORDER BY message_id ASC LIMIT 1")
                .bind(threadId)
                .bind(recipientUid)
                .bind(applicantUid)
                .bind(kFriendAuthContent)
                .execute()
                .fetchOne();
            if (existingMessage) {
                messageId = existingMessage[0].get<std::uint64_t>();
            }
        }
        if (messageId == 0) {
            auto insertMessageResult = con->sql(
                "INSERT INTO chat_message "
                "(thread_id, sender_id, recv_id, content, created_at, updated_at, status) "
                "VALUES (?, ?, ?, ?, NOW(), NOW(), 0)")
                .bind(threadId)
                .bind(recipientUid)
                .bind(applicantUid)
                .bind(kFriendAuthContent)
                .execute();
            messageId = insertMessageResult.getAutoIncrementValue();
        }
        if (messageId == 0 || messageId > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            con->rollback();
            return false;
        }

        con->commit();

        FriendAuthMessage message;
        message.senderId = static_cast<std::uint64_t>(recipientUid);
        message.messageId = messageId;
        message.threadId = threadId;
        // 系统生成的初始消息没有客户端 UUID；以服务端 message_id 构造稳定标识，
        // 同一条消息重试时 unique_id 不会变化。
        message.uniqueId = "friend-auth-" + std::to_string(messageId);
        message.content = kFriendAuthContent;
        authMessages.push_back(std::move(message));
        return true;
    } catch (const std::exception& e) {
        try {
            con->rollback();
        } catch (...) {
        }
        std::cout << "Exception: " << e.what() << std::endl;
        return false;
    }
}


bool MysqlMgr::GetFriendInfo(
    int uid,
    std::vector<std::shared_ptr<UserInfo>>& friendinfo){
    if (uid <= 0) {
        return false;
    }

    auto con = pool_->GetConnection();
    if (con == nullptr) {
        return false;
    }
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); });

    try {
        // friend 保存关系，user 提供好友展示资料；按关系创建时间倒序返回。
        const std::string sql =
            "SELECT u.uid, u.name, u.nick, u.`desc`, u.sex, u.icon "
            "FROM friend AS f "
            "INNER JOIN user AS u ON f.friend_id = u.uid "
            "WHERE f.self_id = ? "
            "ORDER BY f.id DESC";
        auto result = con->sql(sql).bind(uid).execute();

        std::vector<std::shared_ptr<UserInfo>> queriedFriends;
        for (const auto& row : result.fetchAll()) {
            auto friendInfo = std::make_shared<UserInfo>();
            friendInfo->uid = row[0].get<int>();
            friendInfo->user = row[1].get<std::string>();
            friendInfo->nick = row[2].get<std::string>();
            friendInfo->desc = row[3].get<std::string>();
            friendInfo->sex = row[4].get<int>();
            friendInfo->icon = row[5].get<std::string>();
            queriedFriends.push_back(std::move(friendInfo));
        }

        friendinfo = std::move(queriedFriends);
        return true;
    } catch (const std::exception& e) {
        std::cout << "Exception: " << e.what() << std::endl;
        return false;
    }
}

bool MysqlMgr::CreatePrivateChat(int user1Id, int user2Id, std::uint64_t& threadId)
{
    // 输出参数先清零：调用方只要看到 false，就不会误用上次调用残留的会话 ID。
    threadId = 0;
    if (user1Id <= 0 || user2Id <= 0 || user1Id == user2Id) {
        return false;
    }

    auto con = pool_->GetConnection();
    if (con == nullptr) {
        return false;
    }
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); });

    try {
        // 私聊双方统一按 uid 小到大保存，(A, B) 与 (B, A) 才能命中同一个唯一索引。
        const int uid1 = std::min(user1Id, user2Id);
        const int uid2 = std::max(user1Id, user2Id);

        // 创建 chat_thread、private_chat 和清理并发竞争产生的临时会话必须处于同一事务。
        con->startTransaction();

        // 已存在时直接返回。FOR UPDATE 同时让常见并发场景在这一行上串行化。
        const std::string findSql =
            "SELECT thread_id FROM private_chat "
            "WHERE user1_id = ? AND user2_id = ? FOR UPDATE";
        const auto existingRow = con->sql(findSql).bind(uid1).bind(uid2).execute().fetchOne();
        if (existingRow) {
            threadId = existingRow[0].get<std::uint64_t>();
            con->commit();
            return true;
        }

        // 尚未找到时先创建统一会话入口。getAutoIncrementValue 只属于当前连接，
        // 连接被本次事务独占，不会混入连接池中其他请求的自增 ID。
        // SqlResult::getAutoIncrementValue() 会读取结果状态，Connector/C++ 将它定义为非 const。
        auto createThreadResult = con->sql(
            "INSERT INTO chat_thread (type, created_at) VALUES ('private', NOW())").execute();
        const std::uint64_t newThreadId = createThreadResult.getAutoIncrementValue();
        if (newThreadId == 0) {
            con->rollback();
            return false;
        }

        // uniq_private_thread 是最后一道并发保障：即使两个事务都先查不到，最终也只允许
        // 一条 private_chat 记录。LAST_INSERT_ID(?) 保证“本次插入成功”的分支也会设置
        // 当前连接的会话变量；发生重复时 LAST_INSERT_ID(thread_id) 则写入“赢家”的 ID，
        // 因而下面无论哪条分支都可用同一种方式读取最终 thread_id。
        const std::string upsertPrivateChatSql =
            "INSERT INTO private_chat (thread_id, user1_id, user2_id, created_at) "
            "VALUES (LAST_INSERT_ID(?), ?, ?, NOW()) "
            "ON DUPLICATE KEY UPDATE thread_id = LAST_INSERT_ID(thread_id)";
        con->sql(upsertPrivateChatSql)
            .bind(newThreadId)
            .bind(uid1)
            .bind(uid2)
            .execute();

        const auto threadIdRow = con->sql("SELECT LAST_INSERT_ID()").execute().fetchOne();
        if (!threadIdRow) {
            con->rollback();
            return false;
        }
        threadId = threadIdRow[0].get<std::uint64_t>();

        // 若另一并发请求先创建了私聊，刚插入的 chat_thread 没有任何 private_chat 引用。
        // 在提交前删除这条临时记录，避免聊天列表出现无法进入的孤儿会话。
        if (threadId != newThreadId) {
            con->sql("DELETE FROM chat_thread WHERE id = ?")
                .bind(newThreadId)
                .execute();
        }

        con->commit();
        return true;
    } catch (const std::exception& e) {
        // 事务内任一步失败都回滚；rollback 自身失败不应掩盖原始异常。
        try {
            con->rollback();
        } catch (...) {
        }
        threadId = 0;
        std::cerr << "Exception: " << e.what() << std::endl;
        return false;
    }
}
