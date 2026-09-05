#pragma once //防止重复包含
#ifndef _CONST_H_
#define _CONST_H_

#include <memory>
#include <functional>
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
};

#define LOGIN_COUNT "login_count"
#define USERIPPREFIX "uip_"
#define USERTOKENFREFIX "utoken_"
#define UIPCOUNTPREFIX "ipcount_"
#define USER_BASE_INFO "ubaseinfo_"


#endif // !_CONST_H_

