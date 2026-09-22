#pragma once
#ifndef GLOBAL_H
#define GLOBAL_H
//放一些要用到的头文件
#include <QWidget>
#include <functional>
#include <QDebug>
#include "QStyle" //Qt样式的头文件
#include <QRegularExpressionValidator>
#include <QRegularExpression>
#include <QFile>
#include <QDir>
#include <QSettings>
#include <memory>
#include <iostream>
#include <mutex>
#include <thread>

//使用extern声明变量，避免重定义
extern std::function<void(QWidget*)> repolish; //用于实现注册界面错误信息的刷新，也就是刷新qss

/*简单加密函数：异或字符串
 *复杂加密用一些库可以用*/
extern std::function<QString(QString)> xorString;


enum ReqId{
    ID_GET_VARIFY_CODE = 1001, //获取验证码
    ID_REG_USER = 1002, //注册用户
    ID_RESET_PWD = 1003, //重置密码
    ID_LOGIN_USER = 1004, //用户登录
    ID_CHAT_LOGIN = 1005, //登陆聊天服务器
    ID_CHAT_LOGIN_RSP= 1006, //登陆聊天服务器回包
    ID_SEARCH_USER_REQ = 1007, //用户搜索请求
    ID_SEARCH_USER_RSP = 1008, //搜索用户回包
    ID_ADD_FRIEND_REQ = 1009,  //添加好友申请
    ID_ADD_FRIEND_RSP = 1010, //申请添加好友回复
    ID_NOTIFY_ADD_FRIEND_REQ = 1011,  //通知用户添加好友申请
    ID_AUTH_FRIEND_REQ = 1013,  //认证好友请求
    ID_AUTH_FRIEND_RSP = 1014,  //认证好友回复
    ID_NOTIFY_AUTH_FRIEND_REQ = 1015, //通知用户认证好友申请
    ID_TEXT_CHAT_MSG_REQ  = 1017,  //文本聊天信息请求
    ID_TEXT_CHAT_MSG_RSP  = 1018,  //文本聊天信息回复
    ID_NOTIFY_TEXT_CHAT_MSG_REQ = 1019, //通知用户文本聊天信息
    ID_NOTIFY_OFF_LINE_REQ = 1021, //通知用户下线
    ID_HEART_BEAT_REQ = 1023,      //心跳请求
    ID_HEARTBEAT_RSP = 1024,       //心跳回复
    ID_LOAD_CHAT_THREAD_REQ = 1025,      //加载聊天线程
    ID_LOAD_CHAT_THREAD_RSP = 1026,      //加载聊天线程回复
    ID_CREATE_PRIVATE_CHAT_REQ = 1027, //创建私聊请求
    ID_CREATE_PRIVATE_CHAT_RSP = 1028, //创建私聊回复
    ID_LOAD_CHAT_MSG_REQ = 1029,      //加载聊天消息
    ID_LOAD_CHAT_MSG_RSP = 1030,      //加载聊天消息
    // 资料更新走已登录的 ChatServer；服务端以 session UID 鉴权，不能信任请求中的 uid。
    ID_UPDATE_USER_PROFILE_REQ = 1031, //更新当前用户资料请求：uid/nick/desc/icon
    ID_UPDATE_USER_PROFILE_RSP = 1032, //更新当前用户资料回包：error
    // 图片正文只在 ResourceServer 的独立 TCP 连接中传输；ChatServer 仅路由以下
    // JSON 元数据，避免 2+2 字节聊天包承载图片二进制。
    ID_IMAGE_CHAT_MSG_REQ = 1033,       // 图片消息请求：fromuid/touid/imageArray
    ID_IMAGE_CHAT_MSG_RSP = 1034,       // 图片消息发送确认
    ID_NOTIFY_IMAGE_CHAT_MSG_REQ = 1035, // 对端图片消息通知
    ID_MESSAGE_DISPLAY_ACK_REQ = 1036,  // 接收端已将消息实际绘制到当前会话
    ID_NOTIFY_MESSAGE_DISPLAYED = 1037, // 服务端通知发送端：对端已显示
    ID_MARK_THREAD_READ_REQ = 1038,     // 当前会话已读到 message_id 游标
    ID_NOTIFY_THREAD_READ = 1039        // 服务端通知发送端：对端已读游标
};

// ResourceServer 使用独立 TCP 连接和 2+4 字节包头；这里的 ID 属于另一套协议，
// 即使数值与 HTTP/ChatServer ID 重叠也不能混用。
enum ResourceReqId : quint16 {
    ID_TEST_REQ = 1001,
    ID_TEST_RSP = 1002,
    ID_UPLOAD_FILE_REQ = 1003,
    ID_UPLOAD_FILE_RSP = 1004,
    ID_SYNC_FILE_REQ = 1005,
    ID_SYNC_FILE_RSP = 1006,
    // 资源下载以 resource_id（上传任务 ID）定位资源；绝不能把服务端本机路径发回去下载。
    ID_DOWNLOAD_FILE_REQ = 1007,
    ID_DOWNLOAD_FILE_RSP = 1008
};

enum Modules{
    REGISTERMOD = 0, //注册模块
    LOGINMOD = 1, //注册模块
    RESETMOD = 2, //重置密码模块
};

enum ErrorCodes{
    SUCCESS = 0,
    ERR_JSON = 1001, //json解析失败
    ERR_NETWORK = 1002, //网络错误
    DuplicateRequest = 1003, //请勿重复请求
    Error_VarifyCode = 1004, //验证码不匹配
    Error_VarifyCodeExpired = 1005, //验证码过期
    Error_EmailRegistered = 1006, //邮箱已经使用
    Error_UserExist = 1007, //用户名存在
    Error_Password = 1008, // 密码错误
    Error_UserNoExist = 1009, //用户名不存在
    Error_UserNotMatchEamil = 1010, //用户名和邮箱不匹配
    UidInvalid = 1011, //用户id无效
    TokenInvalid = 1012, //token无效
};

extern QString gate_url_prefix;

enum ClickLbState{
    Normal = 0, //Normal，闭眼
    Selected = 1 //睁眼
};

// 自定义QListWidgetItem的几种类型
enum ListItemType{
    CHAT_USER_ITEM, // 聊天用户
    CONTACT_USER_ITEM, // 联系人用户
    SEARCH_USER_ITEM, // 搜索到的用户
    ADD_USER_TIP_ITEM, // 显示添加用户
    APPLY_FRIEND_ITEM, // 好友申请入口（联系人页的"新的朋友"）
    INVALID_ITEM, // 不可点击条目
    GRUOP_TIP_ITEM, // 分组显示条目
};

struct ServerInfo{
    QString Host;
    QString Port;
    QString Token;
    int Uid;
};

enum ChatRole{
    Self,
    Other,
};

struct MsgInfo{
    QString msgFlag; // 消息类型 "text,image,file"
    QString content; // 表示文件和图像的url，文本信息
    QPixmap pixmap; // 文件和图片的缩略图
};


#endif // GLOBAL_H
