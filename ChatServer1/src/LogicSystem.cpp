#include "LogicSystem.h"
#include <json.h>
#include <sstream>
#include <iostream>
#include "MysqlMgr.h"
#include "RedisMgr.h"
#include "UserMgr.h"

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
// 新增分布式处理
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

    Json::Value rtvalue;
    Defer defer([this, &rtvalue, session]{
        std::string return_str = rtvalue.toStyledString();
        session->Send(return_str, MSG_CHAT_LOGIN_RSP); // 发送登录回包，在出作用域的时候会自动调用，防御式编程处理
    });

    // 直接从redis中校验token
    std::string token_key = USERTOKENFREFIX + std::to_string(uid);
    std::string token_in_redis = "";
    bool success = RedisMgr::GetInstance()->Get(token_key, token_in_redis);

    if (!success) {
        rtvalue["error"] = ErrorCode::UidInvalid;
        return;
    }

    if (token != token_in_redis) {
        rtvalue["error"] = ErrorCode::TokenInvalid;
        return;
    }

    rtvalue["error"] = ErrorCode::Success;

    // 查询用户信息，返回客户端，用于渲染界面
    std::string base_key = USER_BASE_INFO + std::to_string(uid);
    auto user_info = std::make_shared<UserInfo>();
    bool b_base = GetBaseInfo(base_key, uid, user_info);
    if (!b_base) {
        rtvalue["error"] = ErrorCode::UidInvalid;
        return;
    }

    rtvalue["uid"] = uid;
    rtvalue["token"] = token;
    rtvalue["user"] = user_info->user;
    rtvalue["email"] = user_info->email;
    rtvalue["nick"] = user_info->nick;
    rtvalue["desc"] = user_info->desc;
    rtvalue["sex"] = user_info->sex;
    rtvalue["icon"] = user_info->icon;

    // 从数据库获取申请列表，存在本地

    // 从数据库获取好友列表，存在本地

    auto server_name = ConfigMgr::Inst()["SelfServer"]["name"];
    // 将登录数量增加
    auto rd_res = RedisMgr::GetInstance()->HGet(LOGIN_COUNT, server_name);
    int count = 0;
    if(! rd_res.empty()){
        count = std::stoi(rd_res); // 不为空
    }
    count++;
    // 更新登录数量
    auto count_str = std::to_string(count);
    RedisMgr::GetInstance()->HSet(LOGIN_COUNT, server_name, count_str);

    // session 绑定用户uid
    session->SetUserId(uid);

    // 为用户设置登录ip server的名字
    std::string ipkey = USERIPPREFIX + std::to_string(uid);
    RedisMgr::GetInstance()->Set(ipkey, server_name); // 设置用户登录的server名字

    // uid和session绑定管理，方便踢人操作
    UserMgr::GetInstance()->SetUserSession(uid, session);

    return;
}

bool LogicSystem::GetBaseInfo(std::string base_key, int uid, std::shared_ptr<UserInfo>& user_info){
    if (user_info == nullptr) {
        return false;
    }

    // 先在redis中查询，如果查询不到，则从mysql中查询，并缓存到redis中

    std::string info_str;
    if (RedisMgr::GetInstance()->Get(base_key, info_str)) {
        Json::CharReaderBuilder reader;
        Json::Value root;
        std::string errs;
        std::istringstream info_stream(info_str);
        if (!Json::parseFromStream(reader, info_stream, &root, &errs) ||
            !root.isObject() || root["uid"].asInt() != uid) {
            std::cout << "Failed to parse cached user info for uid " << uid << std::endl;
        } else {
            user_info->uid = root["uid"].asInt();
            user_info->user = root["user"].asString();
            user_info->passwd = root["pwd"].asString();
            user_info->email = root["email"].asString();
            user_info->nick = root["nick"].asString();
            user_info->desc = root["desc"].asString();
            user_info->sex = root["sex"].asInt();
            user_info->icon = root["icon"].asString();
            return true;
        }
    }

    UserInfo db_user_info = MysqlMgr::GetInstance()->GetUserInfo(uid);
    if (db_user_info.uid != uid) {
        return false;
    }

    *user_info = db_user_info;

    Json::Value cache_root;
    cache_root["uid"] = user_info->uid;
    cache_root["user"] = user_info->user;
    cache_root["pwd"] = user_info->passwd;
    cache_root["email"] = user_info->email;
    cache_root["nick"] = user_info->nick;
    cache_root["desc"] = user_info->desc;
    cache_root["sex"] = user_info->sex;
    cache_root["icon"] = user_info->icon;
    RedisMgr::GetInstance()->Set(base_key, cache_root.toStyledString()); // 缓存到redis中

    return true;
}
