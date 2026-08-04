#include "LogicSystem.h"
#include <json.h>
#include <sstream>
#include <iostream>

using namespace std;

LogicSystem::LogicSystem() : _b_stop(false), _p_server(nullptr) {
    RegisterCallBacks(); // 注册消息处理函数
    // 启动工作线程，专门消费消息队列，业务逻辑与IO线程分离
    _worker_thread = std::thread(&LogicSystem::DealMsg, this);
}

LogicSystem::~LogicSystem() {
    _b_stop = true;        // 置停止标志
    _consume.notify_one(); // 唤醒工作线程，让它处理完剩余消息后退出
    _worker_thread.join();
}

// 会话层把解析好的消息投递到队列，并唤醒工作线程
void LogicSystem::PostMsgToQue(std::shared_ptr<LogicNode> msg) {
    std::unique_lock<std::mutex> unique_lk(_mutex);
    _msg_que.push(msg);
    if (_msg_que.size() == 1) {
        // 队列从空变成非空，需要唤醒正在等待的工作线程
        unique_lk.unlock();
        _consume.notify_one();
    }
}

void LogicSystem::SetServer(std::shared_ptr<CServer> pserver) {
    _p_server = pserver;
}

// 工作线程主循环：等待消息 -> 按消息id分发到处理函数
void LogicSystem::DealMsg() {
    for (;;) {
        std::unique_lock<std::mutex> unique_lk(_mutex);
        // 队列为空且没有停止请求时，挂起等待
        while (_msg_que.empty() && !_b_stop) {
            _consume.wait(unique_lk);
        }

        // 收到停止请求：把队列剩余消息处理完再退出
        if (_b_stop) {
            while (!_msg_que.empty()) {
                auto msg_node = _msg_que.front();
                auto call_back_iter = _fun_callbacks.find(msg_node->_recvnode->_msg_id);
                if (call_back_iter != _fun_callbacks.end()) {
                    call_back_iter->second(msg_node->_session, msg_node->_recvnode->_msg_id,
                        std::string(msg_node->_recvnode->_data, msg_node->_recvnode->_cur_len));
                }
                _msg_que.pop();
            }
            break;
        }

        // 取出队首消息
        auto msg_node = _msg_que.front();
        auto call_back_iter = _fun_callbacks.find(msg_node->_recvnode->_msg_id);
        if (call_back_iter == _fun_callbacks.end()) {
            _msg_que.pop();
            std::cout << "msg id [" << msg_node->_recvnode->_msg_id << "] handler not found" << std::endl;
            continue;
        }

        // 调用对应的处理函数（登录、搜索好友、聊天、心跳等）
        call_back_iter->second(msg_node->_session, msg_node->_recvnode->_msg_id,
            std::string(msg_node->_recvnode->_data, msg_node->_recvnode->_cur_len));
        _msg_que.pop();
    }
}

// 注册 消息id -> 处理函数 的映射
void LogicSystem::RegisterCallBacks() {
    _fun_callbacks[MSG_CHAT_LOGIN] = std::bind(&LogicSystem::LoginHandler, this,
        std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
    // 后续消息（搜索好友、聊天、心跳等）在对应开发阶段继续注册
}

// 登录处理：解析uid/token -> 请求StatusServer校验 -> 把结果回包给客户端
void LogicSystem::LoginHandler(std::shared_ptr<CSession> session, const short &msg_id,
                               const std::string &msg_data) {
    Json::CharReaderBuilder reader;
    Json::Value root;
    std::istringstream ss(msg_data);
    std::string errs;
    bool parse_success = Json::parseFromStream(reader, ss, &root, &errs);
    if (!parse_success) {
        std::cout << "Failed to parse JSON data" << std::endl;
        std::cout << errs << std::endl;
        return;
    }

    int uid = root["uid"].asInt();
    std::string token = root["token"].asString();
    std::cout << "user login uid is " << uid << " user token is " << token << std::endl;

    // 调用StatusServer校验token是否合法（gRPC）
    auto rsp = StatusGrpcClient::GetInstance()->Login(uid, token);

    // 构造回包：error == 0 表示登录成功
    Json::Value rtvalue;
    rtvalue["error"] = rsp.error();
    rtvalue["uid"] = rsp.uid();
    rtvalue["token"] = rsp.token();
    if (rsp.error() == ErrorCode::Success) {
        session->SetUserId(uid); // 登录成功，记录用户id，后续消息路由使用
    }

    std::string return_str = rtvalue.toStyledString();
    // 把登录结果回包发送给客户端
    session->Send(return_str, MSG_CHAT_LOGIN_RSP);
}
