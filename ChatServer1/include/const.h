#pragma once //防止重复包含
#ifndef _CONST_H_
#define _CONST_H_

#include <memory>
#include <functional>
#include <chrono>
//不能一个头文件包含一大堆头文件，很容易出错

enum ErrorCode{ 
    Success = 0,
    Error_Json = 1001,
    RPCFaild = 1002,
    DuplicateRequest = 1003,
    Error_VarifyCode = 1004,
    Error_VarifyCodeExpired = 1005,
    Error_EmailRegistered = 1006,
    Error_UserExist = 1007, //用户名存在
    Error_Password = 1008, // 密码错误
    Error_UserNoExist = 1009, //用户名不存在
    Error_UserNotMatchEamil = 1010, //用户名和邮箱不匹配
    UidInvalid = 1011, //用户id无效
    TokenInvalid = 1012, //token无效
    SearchUserNoExist = 1013, //搜索的用户不存在
    ServerBusy = 1014, //服务器繁忙
    Create_Chat_Failed = 1015, //创建聊天失败
    // 单条历史消息连同 1030 响应字段也无法放入一个 TCP JSON 包时返回；
    // 不推进游标，避免客户端静默跳过这条消息或陷入重复分页。
    MessageTooLarge = 1016,
};

//手动定义一个Defer类，里面有一个函数，用于在函数结束时自动执行，比如释放资源，实现类似RAII的功能   这个是go语言中的defer关键字
class Defer{
public:
    Defer(std::function<void()> func):func_(func){}

    ~Defer(){
        func_(); //在出去作用域的时候会自动执行这个函数
    }

private:
    std::function<void()> func_;
};

// ==================== 网络协议常量 ====================

// 单个消息包的最大长度（字节）。包头(4字节) + 包体最大2044字节。
// 收到的长度超过该值会被认为是非法包，直接断开连接，防止恶意数据撑爆内存。
#define MAX_LENGTH 1024 * 2

// 消息头总长度 = 消息id(2字节) + 消息体长度(2字节)
#define HEAD_TOTAL_LEN 4
// 消息id 在头部中的字节数
#define HEAD_ID_LEN 2
// 消息体长度 在头部中的字节数
#define HEAD_DATA_LEN 2

// 接收队列最大长度
#define MAX_RECVQUE 10000
// 发送队列最大长度。防止对端消费过慢时，服务器内存中堆积过多待发送消息。
#define MAX_SENDQUE 1000

// ==================== 消息id定义（与客户端约定一致） ====================
enum MSG_IDS {
    MSG_CHAT_LOGIN = 1005,          // 客户端登录聊天服务器请求
    MSG_CHAT_LOGIN_RSP = 1006,      // 服务器登录结果回包
    ID_SEARCH_USER_REQ = 1007,      // 搜索用户请求
    ID_SEARCH_USER_RSP = 1008,      // 搜索用户回包
    ID_ADD_FRIEND_REQ = 1009,       // 添加好友请求
    ID_ADD_FRIEND_RSP = 1010,       // 添加好友回复
    ID_NOTIFY_ADD_FRIEND_REQ = 1011,// 通知用户有新的好友申请
    ID_AUTH_FRIEND_REQ = 1013,      // 认证好友请求
    ID_AUTH_FRIEND_RSP = 1014,      // 认证好友回复
    ID_NOTIFY_AUTH_FRIEND_REQ = 1015, // 通知用户好友认证结果
    ID_TEXT_CHAT_MSG_REQ = 1017,    // 文本聊天消息请求
    ID_TEXT_CHAT_MSG_RSP = 1018,    // 文本聊天消息回复
    ID_NOTIFY_TEXT_CHAT_MSG_REQ = 1019, // 通知用户收到文本聊天消息
    ID_NOTIFY_OFF_LINE_REQ = 1021,  // 通知用户下线
    ID_HEART_BEAT_REQ = 1023,       // 心跳请求
    ID_HEARTBEAT_RSP = 1024,        // 心跳回复
    ID_LOAD_CHAT_THREAD_REQ = 1025, // 加载聊天线程列表请求
    ID_LOAD_CHAT_THREAD_RSP = 1026, // 加载聊天线程列表回复
    ID_CREATE_PRIVATE_CHAT_REQ = 1027, // 创建私聊请求
    ID_CREATE_PRIVATE_CHAT_RSP = 1028, // 创建私聊回复
    ID_LOAD_CHAT_MSG_REQ = 1029,    // 加载聊天消息请求
    ID_LOAD_CHAT_MSG_RSP = 1030,    // 加载聊天消息回复
    ID_UPDATE_USER_PROFILE_REQ = 1031, // 更新当前登录用户资料
    ID_UPDATE_USER_PROFILE_RSP = 1032, // 更新用户资料回包
    ID_IMAGE_CHAT_MSG_REQ = 1033, // 图片聊天请求；仅传资源标识和候选元数据
    ID_IMAGE_CHAT_MSG_RSP = 1034, // 图片已核验、已持久化后的发送确认
    ID_NOTIFY_IMAGE_CHAT_MSG_REQ = 1035, // 通知接收方下载已核验的图片资源
    // 接收端只在消息确实绘制到当前聊天窗口后发送 1036；1037 是服务端回推给发送端的
    // “对方已显示”状态，不能与“已落库”或“已排入实时 socket”混为一谈。
    ID_MESSAGE_DISPLAY_ACK_REQ = 1036,
    ID_NOTIFY_MESSAGE_DISPLAYED = 1037,
    // 1038 表示用户已进入会话并读到指定游标，1039 将已读游标通知消息发送端。
    ID_MARK_THREAD_READ_REQ = 1038,
    ID_NOTIFY_THREAD_READ = 1039,
};

#define LOGIN_COUNT "login_count"
#define USERIPPREFIX "uip_"  // 用户登录的服务器
#define USERTOKENFREFIX "utoken_"
#define UIPCOUNTPREFIX "ipcount_"
#define USER_BASE_INFO "ubaseinfo_"
#define USER_NAME_INFO "unameinfo_"
#define LOGIN_LOCK_PREFIX "login_lock_"
#define USER_SESSION_PREFIX "usession_"
#define LOCK_COUNT "lockcount"

// 会话在该时间内没有收到任何完整 TCP 包，就被认为已失联。
#define HEARTBEAT_TIMEOUT 60
// CServer 定时扫描间隔；正常调度下额外等待约不超过该值，线程阻塞时可能更久。
#define HEARTBEAT_CHECK_INTERVAL 10

// 持有锁最大时间，防止死锁
const auto LOCK_TIME_OUT = std::chrono::seconds(10);
// 分布式锁最大等待时间
const auto ACQUIRE_TIME_OUT = std::chrono::seconds(5);


#endif // !_CONST_H_
