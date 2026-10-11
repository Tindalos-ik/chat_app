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
#include <functional>
#include <json.h>

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

// chat_message与TCP JSON之间的服务端确认消息。messageId是正式主键/客户端去重标识，
// threadId是会话身份，uniqueId用于匹配本账号pending UUID；最新实时ID不是完整同步游标。
// 只有完整授权history页才能推进连续游标；文本/图片共用结构，历史可混合读取两类消息。
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
    std::string senderNick; // 群消息显示名，资料缺失时使用 UID
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

// 群号就是 chat_thread.id；active=false 的群和所有历史永久保留。
struct GroupChatThread {
    std::uint64_t threadId = 0;
    std::string name;
    int ownerUid = 0;
    bool active = true;
    std::uint64_t version = 1;
    bool approvalRequired = false;
    std::string announcement;
    std::uint64_t announcementVersion = 0;
    int announcementAuthorUid = 0;
    std::uint64_t announcementUpdatedAtMs = 0;
    std::string avatarResourceId, avatarMimeType;
    std::uint64_t avatarSize = 0, avatarVersion = 0;
    std::uint32_t avatarWidth = 0, avatarHeight = 0;
    bool member = false;
    int role = 0;
    std::uint64_t membershipVersion = 0;
};
// 当前有效成员页：昵称缺资料回退UID，role为0成员/1管理员/2群主，携带身份版本。
struct GroupChatMember {
    int uid = 0;
    std::string nick;
    int role = 0;
    std::uint64_t membershipVersion = 0;
};
// 所有群权限读取与修改都在事务内锁同一 group_chat 行，防止撤权后的旧权限成功。
enum class GroupAction { Search, Members, Send, History, Authorize };
// 接收者专属事件；提交前建立正式proof与版本，提交后只能尽力投递。
struct GroupEvent {
    int targetUid = 0;
    std::string type;
    Json::Value payload;
};
// 统一群 JSON，source 显式区分 mutation/snapshot/event，避免搜索元数据授予身份。
Json::Value BuildGroupJson(const GroupChatThread& group, const char* source);
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

    // 只读授权/成员/历史或纯文本写入；uid必须来自登录session，threadId为正式群号。
    // after为成员UID或历史message_id游标，limit控制分页。
    // texts是(msgid,content)纯文本批次；Send提交后才允许使用messages/recipients。
    // authorizedDelivery在持群锁期间执行；Authorize用于TCP入队，Send用于帧预算，
    // 不允许回调再次获取数据库连接，拒绝分别返回ServerBusy/MessageTooLarge。
    int GroupOperation(GroupAction action, int uid, std::uint64_t& threadId,
        std::uint64_t after, int limit,
        const std::vector<std::pair<std::string, std::string>>& texts,
        GroupChatThread& group, std::vector<GroupChatMember>& members,
        std::vector<StoredTextMessage>& messages, std::vector<int>& recipients,
        const std::function<bool()>& authorizedDelivery = {});
    // uid来自登录session；groups输出单次一致性查询的全量有效群身份。
    // LogicSystem将结果绑定session UID并按字节分页；快照元数据不授予发送/历史访问权限。
    int LoadGroupSnapshot(int uid, std::vector<GroupChatThread>& groups);
    // threadId为正式会话ID；返回是否group，仅用于可靠历史分派，不授予成员权限。
    bool IsGroupThread(std::uint64_t threadId);
    // MutateGroup参数uid来自认证session，action为管理请求ID，request含持久operation_id。
    // expected_membership_version用于目标kick/admin/transfer，leave用于uid自身；旧世代不能作用于重入。
    // verifiedAvatar只接受ResourceServer可信元数据；response输出当前自身份，events只在Success提交后投递。
    // 返回业务/依赖错误码；群→成员→准入当前读，失败回滚，重放也先校验当前管理权限。
    int MutateGroup(int uid, short action, const Json::Value& request,
        const Json::Value& verifiedAvatar, Json::Value& response, std::vector<GroupEvent>& events);
    // LoadGroupRequests参数uid为认证用户，request含scope/after_id/page_size，审批还需thread_id。
    // inbox=本人全部邀请；applications=本人全部申请；results=相关终态；approvals=当前经理待审批。
    // response输出全局ID页/next_id/load_more与真实UID；返回错误码，不给非成员信箱身份权限。
    int LoadGroupRequests(int uid, const Json::Value& request, Json::Value& response);
private:
    MysqlMgr();

    std::unique_ptr<MySqlPool> pool_; 
};

#endif // MYSQL_MGR_H
