#pragma once //防止重复包含
#ifndef _CONST_H_
#define _CONST_H_

#include "ConfigMgr.h"
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


// redis key
#define LOGIN_COUNT "login_count"
#define USERIPPREFIX "uip_"
#define USERTOKENFREFIX "utoken_"
#define UIPCOUNTPREFIX "ipcount_"
#define USER_BASE_INFO "ubaseinfo_"


#endif // !_CONST_H_

