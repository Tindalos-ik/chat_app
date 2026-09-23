#pragma once
#ifndef MYSQL_MGR_H
#define MYSQL_MGR_H

// 文件作用：声明 ChatServer1 的 MySQL 连接池、聊天持久化记录结构及数据访问接口。

#include <mysqlx/xdevapi.h>  // X DevAPI 头文件
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include "singleton.h"
#include "data.h"

// 好友认证成功时随通知下发的初始会话消息。
// 这里使用独立的数据结构，避免数据访问层依赖 protobuf 生成头文件；
// LogicSystem 再将它转换为 AuthFriendReq::textmsgs。
struct FriendAuthMessage {
    std::uint64_t senderId = 0;
    std::uint64_t messageId = 0;
    std::uint64_t threadId = 0;
    std::string uniqueId;
    std::string content;
};

// chat_message 与 TCP JSON 之间的服务端确认消息。messageId/threadId 是客户端
// SQLite 去重和增量同步使用的游标，uniqueId 用于对应客户端本次发送的 UUID。
// 文本和图片共用这一结构，保证 ID_LOAD_CHAT_MSG 的同一个游标可以混合读取两类消息。
struct StoredTextMessage {
    std::uint64_t messageId = 0;
    std::uint64_t threadId = 0;
    int senderId = 0;
    int recvId = 0;
    std::string uniqueId;
    std::string content;
    std::uint64_t createdAtMs = 0;
    int status = 0;
    bool peerDisplayed = false;
    std::string messageType = "text";
    std::string resourceId;
    std::string name;
    std::string mimeType;
    std::uint64_t fileSize = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct DisplayReceipt {
    int senderId = 0;
    int readerId = 0;
    std::uint64_t threadId = 0;
    std::vector<std::uint64_t> messageIds;
};

struct ReadReceipt {
    int senderId = 0;
    int readerId = 0;
    std::uint64_t threadId = 0;
    std::uint64_t readThroughMessageId = 0;
};

// 图片字段只由 ResourceServer gRPC 核验回包构造，不能从 TCP JSON 直接填入。
struct VerifiedImageMessage {
    std::string uniqueId;
    std::string resourceId;
    std::string name;
    std::string mimeType;
    std::uint64_t fileSize = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// 文件元数据只允许从 ResourceServer VerifyFiles 响应构造。
// 数据结构作用：待写入私聊的已核验文件描述。只能用 VerifyFiles 响应构造。
struct VerifiedFileMessage {
    std::string uniqueId;
    std::string resourceId;
    std::string name;
    std::string mimeType;
    std::uint64_t fileSize = 0;
};

struct PrivateChatThread {
    std::uint64_t threadId = 0;
    int user1Id = 0;
    int user2Id = 0;
};

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

//数据库操作类
class MysqlMgr : public Singleton<MysqlMgr>{
    friend class Singleton<MysqlMgr>;
public:
    ~MysqlMgr(); //析构的时候先调用这个析构再去调用pool的析构，所以手动关闭连接池

    // 注册用户
    bool RegUser(const std::string& name, const std::string& email, const std::string& password);

    // 根据用户名查找用户是否存在
    bool Checkuser(const std::string& name);
    // 存在该用户名则把用户信息返回（网关保证用户名唯一，最多一条）
    bool Checkuser(const std::string& name, UserInfo& userInfo);

    // 根据uid查找用户是否存在
    bool Checkuid(int uid);
    // 存在该uid则把用户信息返回（uid 是唯一索引，最多一条）
    bool Checkuid(int uid, UserInfo& userInfo);

    //检查用户名和邮箱是否匹配
    bool isMatch(const::std::string& name,std::string& email);

    //检查邮箱是否已经注册
    bool Checkemail(const std::string& email);

    //更新密码
    bool UpdatePwd(const std::string& name, const std::string& newpwd);

    // 更新当前用户可编辑资料；uid 来自已认证会话，不接受客户端指定其他用户。
    bool UpdateUserProfile(int uid, const std::string& nick,
                           const std::string& desc, const std::string& icon);

    //检查用户名和密码是否匹配
    bool CheckPwd(const std::string& name, const std::string& pwd, UserInfo& userInfo);

    UserInfo GetUserInfo(int uid);

    // 保存好友申请及申请方为接收者设置的备注；重复的待处理申请会更新备注。
    bool AddFriendApply(
        int applicantUid,
        int recipientUid,
        const std::string& applicantRemark);

    // 获取指定接收者收到的好友申请，并以查询结果完整替换 applications。
    bool GetFriendApplyInfo(
        int recipientUid,
        std::vector<std::shared_ptr<ApplyInfo>>& applications);

    bool GetFriendInfo(
        int uid,
        std::vector<std::shared_ptr<UserInfo>>& friendinfo
    );

    // 将待处理申请更新为最终状态：1=同意，2=拒绝。
    bool UpdateFriendApplyStatus(
        int applicantUid,
        int recipientUid,
        int newStatus);

    // 原子地确认申请、建立双向好友关系和唯一私聊，并返回需要通知双方的初始消息。
    // 事务提交成功前 authMessages 不会被修改，调用方不会把半成品消息推给客户端。
    bool AddFriend(
        int recipientUid,
        int applicantUid,
        const std::string& recipientRemark,
        std::vector<FriendAuthMessage>& authMessages);

    // 创建或获取两个用户唯一的私聊；threadId 是输出参数，对应 chat_thread.id（BIGINT UNSIGNED）。
    // 无论本次创建还是已存在，成功时都会返回同一个会话 ID。
    bool CreatePrivateChat(int user1Id, int user2Id, std::uint64_t& threadId);

    // 先创建或取得双方唯一私聊，再在一个事务内批量写入文本消息。
    // 成功时 storedMessages 的顺序与 clientMessages 相同，便于客户端按 UUID 确认发送结果。
    bool SavePrivateTextMessages(int senderUid, int recvUid,
                                 const std::vector<std::pair<std::string, std::string>>& clientMessages,
                                 std::uint64_t& threadId,
                                 std::vector<StoredTextMessage>& storedMessages);

    // 写入已由 ResourceServer 证明完成且为图片的资源。整个批次只有全部成功才提交，
    // 因此发送确认不会出现“部分图片有 message_id”的不可恢复状态。
    bool SavePrivateImageMessages(int senderUid, int recvUid,
                                  const std::vector<VerifiedImageMessage>& imageMessages,
                                  std::uint64_t& threadId,
                                  std::vector<StoredTextMessage>& storedMessages);
    // 原子保存一批已核验私聊文件；senderUid/recvUid 为双方 UID，fileMessages 为可信资源描述；
    // threadId 输出正式会话 ID，storedMessages 输出数据库生成的确认及同步字段。
    bool SavePrivateFileMessages(int senderUid, int recvUid,
                                 const std::vector<VerifiedFileMessage>& fileMessages,
                                 std::uint64_t& threadId,
                                 std::vector<StoredTextMessage>& storedMessages);

    // 按 (thread_id, message_id) 游标正序读取。调用方传入 limit + 1 即可判断是否还有下一页。
    // 同时校验 uid 必须属于该私聊，避免客户端借 thread_id 读取他人聊天记录。
    bool LoadPrivateTextMessages(int uid, std::uint64_t threadId, std::uint64_t afterMessageId,
                                 int limit, std::vector<StoredTextMessage>& messages);

    // 返回 thread_id 大于 afterThreadId 的私聊，用于登录时发现本地尚不存在的新会话。
    bool LoadPrivateChatThreads(int uid, std::uint64_t afterThreadId, int limit,
                                std::vector<PrivateChatThread>& threads);

    // 将“已显示”和“已读”分开保存：显示仅代表消息进入了对方界面，不能把它写成已读。
    // 输出只含本次首次发生状态变化的消息，调用方据此避免对发送端重复通知。
    bool MarkMessagesDisplayed(int readerUid, std::uint64_t threadId,
                               const std::vector<std::uint64_t>& messageIds,
                               std::vector<DisplayReceipt>& receipts);
    bool MarkPrivateThreadRead(int readerUid, std::uint64_t threadId,
                               std::uint64_t readThroughMessageId,
                               std::vector<ReadReceipt>& receipts);

private:
    MysqlMgr();

    std::unique_ptr<MySqlPool> pool_; 
};

#endif // MYSQL_MGR_H
