#pragma once
#ifndef LOGICSYSTEM_H
#define LOGICSYSTEM_H

#include "singleton.h"
#include "const.h"
#include "HttpConnection.h"
#include <map>
#include <memory>
#include <functional>
#include <string>

class HttpConnection;
typedef std::function<void(std::shared_ptr<HttpConnection>)> HttpHandler;

/*
LogicSystem 逻辑层，完成路由，业务相关，获取验证码的操作
路由就是根据请求的 URL 路径和 HTTP 方法（GET、POST 等），将请求分发到对应的处理函数（Handler）上。
*/
class LogicSystem : public Singleton<LogicSystem>, public std::enable_shared_from_this<LogicSystem> {
    friend class Singleton<LogicSystem>; // 单例模式，设置基类有元，从而访问私有构造函数
public:
    bool HandleGet(std::string path, std::shared_ptr<HttpConnection> con);
    void RegGet(std::string url, HttpHandler handler);

    bool HandlePost(std::string path, std::shared_ptr<HttpConnection> con);
    void RegPost(std::string url, HttpHandler handler);

private:
    LogicSystem();
    std::map<std::string, HttpHandler> _post_handlers;
    std::map<std::string, HttpHandler> _get_handlers;
};

#endif // LOGICSYSTEM_H