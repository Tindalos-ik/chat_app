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
    ID_GET_VERIFY_CODE = 1001, //获取验证码
    ID_REG_USER = 1002, //注册用户
    ID_LOGIN = 1003, //登录
    ID_RESETPASSWORD = 1004, //重置密码
    ID_CHAT_LOGIN, //登录聊天服务器
    ID_CHAT_LOGIN_RSP, //登录聊天服务器回包
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
