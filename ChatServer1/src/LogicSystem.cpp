#include "LogicSystem.h"
#include <json.h>
#include <sstream>
#include <iostream>
#include <cctype>
#include "MysqlMgr.h"
#include "RedisMgr.h"
#include "UserMgr.h"
#include "ChatGrpcClient.h"
#include "CServer.h"

using namespace std;

LogicSystem::LogicSystem() : _b_stop(false), _p_server(nullptr) {
    RegisterCallBacks(); // 注册消息处理函数
    // 启动工作线程，专门消费消息队列，业务逻辑与IO线程分离
    _worker_thread = std::thread(&LogicSystem::DealMsg, this);
}

LogicSystem::~LogicSystem() {
    Stop();
}

void LogicSystem::Stop() {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_b_stop) {
            return;
        }
        _b_stop = true; // 和队列消费线程使用同一把锁保护停止标志
    }
    _consume.notify_all(); // 唤醒工作线程，让它处理完剩余消息后退出
    if (_worker_thread.joinable()) {
        _worker_thread.join();
    }
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

        // 停止时仍按正常路径排空已入队消息，空队列才退出。
        if (_b_stop && _msg_que.empty()) {
            break;
        }

        // 锁只保护取消息；数据库/RPC 耗时期间不能阻塞 I/O 线程的 PostMsgToQue，
        // 否则同一个 I/O 线程上的其他会话也无法继续读心跳。
        auto msg_node = _msg_que.front();
        _msg_que.pop();
        unique_lk.unlock();
        auto call_back_iter = _fun_callbacks.find(msg_node->_recvnode->_msg_id);
        if (call_back_iter == _fun_callbacks.end()) {
            std::cout << "msg id [" << msg_node->_recvnode->_msg_id << "] handler not found" << std::endl;
            continue;
        }

        // 单个 worker 仍按顺序处理业务；1023 心跳由 CSession 直接处理。
        call_back_iter->second(msg_node->_session, msg_node->_recvnode->_msg_id,
            std::string(msg_node->_recvnode->_data, msg_node->_recvnode->_cur_len));
    }
}

// 注册登录、搜索好友、聊天等业务映射；1023 心跳在 CSession 中处理。
void LogicSystem::RegisterCallBacks() {
    _fun_callbacks[MSG_CHAT_LOGIN] = [this](std::shared_ptr<CSession> session,
                                            const short &msg_id,
                                            const std::string &msg_data) {
        LoginHandler(session, msg_id, msg_data);
    };
    _fun_callbacks[ID_SEARCH_USER_REQ] = [this](std::shared_ptr<CSession> session,
                                            const short &msg_id,
                                            const std::string &msg_data) {  
        UserSearchHandler(session, msg_id, msg_data);
    };
    _fun_callbacks[ID_ADD_FRIEND_REQ] = [this](std::shared_ptr<CSession> session,
                                            const short &msg_id,    
                                            const std::string &msg_data) {
        AddFriendApply(session, msg_id, msg_data);
    };
    _fun_callbacks[ID_AUTH_FRIEND_REQ] = [this](std::shared_ptr<CSession> session,
                                            const short &msg_id,
                                            const std::string &msg_data) {  
        AuthFriend(session, msg_id, msg_data);
    };
    _fun_callbacks[ID_TEXT_CHAT_MSG_REQ] = [this](std::shared_ptr<CSession> session,
                                            const short &msg_id,
                                            const std::string &msg_data) {
        HandleTextMsg(session, msg_id, msg_data);
    };
    _fun_callbacks[ID_LOAD_CHAT_MSG_REQ] = [this](std::shared_ptr<CSession> session,
                                            const short &msg_id,
                                            const std::string &msg_data) {
        LoadChatMessages(session, msg_id, msg_data);
    };
    _fun_callbacks[ID_LOAD_CHAT_THREAD_REQ] = [this](std::shared_ptr<CSession> session,
                                               const short &msg_id,
                                               const std::string &msg_data) {
        LoadChatThreads(session, msg_id, msg_data);
    };
    _fun_callbacks[ID_UPDATE_USER_PROFILE_REQ] = [this](std::shared_ptr<CSession> session,
                                            const short &msg_id,
                                            const std::string &msg_data) {
        UpdateUserProfile(session, msg_id, msg_data);
    };
    _fun_callbacks[ID_CREATE_PRIVATE_CHAT_REQ] = [this](std::shared_ptr<CSession> session,
                                            const short &msg_id,
                                            const std::string &msg_data) {  
        CreatePrivateChat(session, msg_id, msg_data);
    };
}

void LogicSystem::UpdateUserProfile(std::shared_ptr<CSession> session, const short &msg_id,
                                    const std::string &msg_data) {
    (void)msg_id;
    Json::Value response;
    Defer defer([session, &response] {
        session->Send(response.toStyledString(), ID_UPDATE_USER_PROFILE_RSP);
    });

    const int session_uid = session->GetUserId();
    if (session_uid <= 0) {
        response["error"] = ErrorCode::TokenInvalid;
        return;
    }

    Json::CharReaderBuilder reader;
    Json::Value root;
    std::istringstream input(msg_data);
    std::string errors;
    if (!Json::parseFromStream(reader, input, &root, &errors) || !root.isObject() ||
        !root["nick"].isString() || !root["desc"].isString() ||
        !root["icon"].isString()) {
        response["error"] = ErrorCode::Error_Json;
        return;
    }

    // uid 只用于客户端关联回包，真正的写入身份始终取已认证会话，禁止越权修改。
    if (root.isMember("uid") && (!root["uid"].isInt() || root["uid"].asInt() != session_uid)) {
        response["error"] = ErrorCode::UidInvalid;
        return;
    }

    const std::string nick = root["nick"].asString();
    const std::string desc = root["desc"].asString();
    const std::string icon = root["icon"].asString();
    // user 表三个字段都是 varchar(255)。这里按 UTF-8 字节做更保守的上限，
    // 防止超长 ASCII URL/签名直到 MySQL 才报错；昵称 UI 只允许 24 个字符。
    if (nick.empty() || nick.size() > 96 || desc.size() > 255 || icon.size() > 255) {
        response["error"] = ErrorCode::Error_Json;
        return;
    }

    if (!MysqlMgr::GetInstance()->UpdateUserProfile(session_uid, nick, desc, icon)) {
        response["error"] = ErrorCode::RPCFaild;
        return;
    }

    response["error"] = ErrorCode::Success;
    response["uid"] = session_uid;
    response["nick"] = nick;
    response["desc"] = desc;
    response["icon"] = icon;
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

    // 从数据库获取用户的好友申请列表，返回客户端
    std::vector<std::shared_ptr<ApplyInfo>> apply_list;
    bool b_apply = MysqlMgr::GetInstance()->GetFriendApplyInfo(uid, apply_list);
    
    if (b_apply) {
        for(auto apply : apply_list) {
            Json::Value apply_json;
            apply_json["uid"] = apply->_uid;
            apply_json["name"] = apply->_user;
            apply_json["nick"] = apply->_nick;
            apply_json["desc"] = apply->_desc;
            apply_json["icon"] = apply->_icon;
            rtvalue["apply_list"].append(apply_json);
        }
    }else{
        rtvalue["apply_list"] = Json::Value::null;
    }

    // 从数据库获取好友列表，返回客户端
    std::vector<std::shared_ptr<UserInfo>> friend_list;
    bool b_friend = MysqlMgr::GetInstance()->GetFriendInfo(uid, friend_list);

    if(b_friend) {
        for(auto friend_info : friend_list) {
            Json::Value friend_json;
            friend_json["uid"] = friend_info->uid;
            friend_json["name"] = friend_info->user;
            friend_json["nick"] = friend_info->nick;
            friend_json["desc"] = friend_info->desc;
            friend_json["sex"] = friend_info->sex;
            friend_json["icon"] = friend_info->icon;
            rtvalue["friend_list"].append(friend_json);
        }
    }

    // 添加分布式锁，防止并发读改写
    // 这样，两个服务器同时处理 uid = 1001 的登录请求，只有一个服务器能获取到锁，另一个服务器会等待锁释放
    auto lock_key  = LOGIN_LOCK_PREFIX + std::to_string(uid);
    auto lock_result = RedisMgr::GetInstance()->acquireLock(lock_key, LOCK_TIME_OUT, ACQUIRE_TIME_OUT);

    if(lock_result.result != RedisLockResult::Acquired){
        std::cout << "acquire lock failed" << std::endl;
        rtvalue["error"] = ErrorCode::ServerBusy;
        return;
    }

    std::string identifier = lock_result.identifier;
    // 利用defer解锁
    Defer lock_defer([this, identifier, lock_key](){
        RedisMgr::GetInstance()->releaseLock(lock_key, identifier);
    });

    // 判断用户是否在别处登录或在本服务器登录
    std::string uid_ip_value;
    auto uid_ip_key = USERIPPREFIX  + std::to_string(uid);
    bool b_ip = RedisMgr::GetInstance()->Get(uid_ip_key, uid_ip_value);
    if(b_ip){ // 用户已经登录
        std::string old_session_id;
        RedisMgr::GetInstance()->Get(USER_SESSION_PREFIX + std::to_string(uid), old_session_id);

        // 获取当前服务器ip信息
        auto self_name = ConfigMgr::Inst()["SelfChatServer"]["name"];
        if(uid_ip_value == self_name){ // 用户在本服务器登录，则直接在本服务器踢掉
            // 查找旧连接
            auto old_session = UserMgr::GetInstance()->GetSession(uid);
            if(old_session){
                // Redis 会话与本机会话不一致时不能按 uid 盲踢，交给客户端稍后重试。
                if(old_session_id.empty() || old_session->GetSessionId() != old_session_id){
                    rtvalue["error"] = ErrorCode::ServerBusy;
                    return;
                }
                if(!_p_server){
                    rtvalue["error"] = ErrorCode::ServerBusy;
                    return;
                }
                // 通知客户端下线
                old_session->NotifyOffline();
                // 此处仅立即注销旧连接的本地映射和在线数，不能直接调用 HandleDisconnect：
                // HandleDisconnect 会关闭 socket，可能导致刚入队的 1021 下线通知尚未写完就丢失。
                // 发送队列排空后，HandleWrite 会调用 HandleDisconnect 完成 socket 和 Redis 清理。
                _p_server->ClearSession(old_session->GetSessionId());
            }
        }else{
            // 用户在其他服务器：同步等待旧服完成踢人，成功后才能覆盖 Redis 路由。
            KickUserReq request;
            request.set_uid(uid);
            request.set_session_id(old_session_id);
            std::cout << "cross-server kick request, uid = " << uid
                      << ", old server = " << uid_ip_value
                      << ", session = " << old_session_id << std::endl;
            auto response = ChatGrpcClient::GetInstance()->NotifyKickUser(uid_ip_value, request);
            if(response.error() != ErrorCode::Success){
                std::cout << "cross-server kick failed, uid = " << uid
                          << ", error = " << response.error() << std::endl;
                rtvalue["error"] = response.error();
                return;
            }
            std::cout << "cross-server kick succeeded, uid = " << uid << std::endl;
        }
    }

    auto server_name = ConfigMgr::Inst()["SelfChatServer"]["name"];
    // 原子地给本服务器在线人数 +1（HINCRBY 避免并发读改写丢更新）
    RedisMgr::GetInstance()->HIncrBy(LOGIN_COUNT, server_name, 1);

    // session 绑定用户uid
    session->SetUserId(uid);

    // 为用户设置登录ip server的名字
    std::string ipkey = USERIPPREFIX + std::to_string(uid);
    RedisMgr::GetInstance()->Set(ipkey, server_name); // 设置用户登录的server名字

    // 写入用户session信息，将信息跨服传递
    std::string session_key = USER_SESSION_PREFIX + std::to_string(uid);
    RedisMgr::GetInstance()->Set(session_key, session->GetSessionId());

    // uid和session绑定管理，方便踢人操作
    UserMgr::GetInstance()->SetUserSession(uid, session);

    return;
}

void LogicSystem::UserSearchHandler(std::shared_ptr<CSession> session, const short &msg_id, const std::string &msg_data){
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

    // 客户端搜索框传的 uid 字段永远是字符串：可能输入 uid，也可能输入用户名
    // 先判断是不是纯数字：是纯数字才按 uid 查，否则直接按用户名查。
    // 注意：不能对字符串直接调 asInt()，jsoncpp 会触发断言导致进程退出；
    // 而且解析成 0 还会误命中历史遗留的 uid=0 用户。
    std::string uid_str = root["uid"].asString();
    bool is_number = !uid_str.empty();
    for (char ch : uid_str) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            is_number = false;
            break;
        }
    }

    Json::Value rtvalue;
    Defer defer([this, &rtvalue, session]{
        std::string return_str = rtvalue.toStyledString();
        session->Send(return_str, ID_SEARCH_USER_RSP); // 发送用户搜索回包，在出作用域的时候会自动调用，防御式编程处理
    });

    // 从数据库中寻找这个用户是否存在：输入是纯数字时优先按 uid 查，否则按用户名查
    // （网关保证用户名唯一，所以这里只需要单个 UserInfo，不用 vector）
    UserInfo user_info;
    bool found = false;
    if (is_number) {
        try {
            found = MysqlMgr::GetInstance()->Checkuid(std::stoi(uid_str), user_info);
        } catch (const std::exception &e) {
            // uid 字符串超出 int 范围等情况：按查不到处理，不能让异常逃出工作线程
            std::cout << "Exception: " << e.what() << std::endl;
        }
    }

    if (!found) {
        // 输入的是用户名，或按 uid 没查到：再按用户名查一次
        found = MysqlMgr::GetInstance()->Checkuser(uid_str, user_info);
    }

    if(found){
        // 说明客户端传来的是uid，且查到了用户
        rtvalue["error"] = ErrorCode::Success;
        rtvalue["uid"] = user_info.uid;
        rtvalue["user"] = user_info.user;
        rtvalue["email"] = user_info.email;
        rtvalue["nick"] = user_info.nick;
        rtvalue["desc"] = user_info.desc;
        rtvalue["sex"] = user_info.sex;
        rtvalue["icon"] = user_info.icon;
    } else {
        // 用户不存在
        rtvalue["error"] = ErrorCode::SearchUserNoExist;
    }
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

void LogicSystem::AddFriendApply(std::shared_ptr<CSession> session, const short &msg_id, const std::string &msg_data){
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

    auto uid = root["uid"].asInt();
    auto applyname = root["applyname"].asString();
    auto bakname = root["bakname"].asString();
    auto touid = root["touid"].asInt();

    Json::Value rtvalue;
    Defer defer([this, &rtvalue, session]{
        std::string return_str = rtvalue.toStyledString();
        session->Send(return_str, ID_ADD_FRIEND_RSP); // 发送回包，在出作用域的时候会自动调用，防御式编程处理
    });

    // 先更新数据库中关于好友申请的数据表；写入失败时不再继续通知在线用户
    if (!MysqlMgr::GetInstance()->AddFriendApply(uid, touid, bakname)) {
        rtvalue["error"] = ErrorCode::RPCFaild;
        return;
    }
    rtvalue["error"] = ErrorCode::Success;

    // 查询redis 查询对端的服务器
    auto touid_str = std::to_string(touid);
    auto to_ip_key = USERIPPREFIX + touid_str;
    std::string to_ip_value; 
    bool b_ip = RedisMgr::GetInstance()->Get(to_ip_key, to_ip_value);
    if (!b_ip) {
        return;
    }

    auto self_name = ConfigMgr::Inst()["SelfChatServer"]["name"];

    // 如果对端服务器就是自己，则直接发送好友申请
    if(self_name == to_ip_value){
        auto session = UserMgr::GetInstance()->GetSession(touid);
        if(session){
            // 在内存中直接发送通知对方
            Json::Value notify;
            notify["error"] = ErrorCode::Success;
            notify["applyuid"] = uid;
            notify["name"] = applyname;
            notify["desc"] = "";
            std::string return_str = notify.toStyledString();
            session->Send(return_str, ID_NOTIFY_ADD_FRIEND_REQ);
        }
        return;
    }

    // 如果对端服务器不是自己，则转发给对端服务器
    std::string base_key = USER_BASE_INFO + std::to_string(touid);
    auto apply_info = std::make_shared<UserInfo>();
    bool b_info = GetBaseInfo(base_key, touid, apply_info);

    AddFriendReq add_req;
    add_req.set_applyuid(uid);
    add_req.set_name(applyname);
    add_req.set_desc("");
    add_req.set_touid(touid);
    
    if(b_info){
        add_req.set_icon(apply_info->icon);
        add_req.set_sex(apply_info->sex);
        add_req.set_nick(apply_info->nick);
    }

    // 转发，通知对端服务器有新的好友申请
    ChatGrpcClient::GetInstance()->NotifyAddFriend(to_ip_value, add_req);
}

void LogicSystem::AuthFriend(std::shared_ptr<CSession> session, const short &msg_id, const std::string &msg_data){
    // 客户端发过来就默认同意了
    // 服务器回包需要把申请人的信息传回去
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

    auto fromuid = root["fromuid"].asInt();
    auto bakname = root["bakname"].asString();
    auto touid = root["touid"].asInt();

    Json::Value rtvalue;
    Defer defer([this, &rtvalue, session]{
        std::string return_str = rtvalue.toStyledString();
        session->Send(return_str, ID_AUTH_FRIEND_RSP); // 发送回包，在出作用域的时候会自动调用，防御式编程处理
    });

    // 获取申请人信息
    std::string base_key = USER_BASE_INFO + std::to_string(fromuid);
    auto user_info = std::make_shared<UserInfo>();
    bool b_info = GetBaseInfo(base_key, fromuid, user_info);

    if (!b_info) {
        rtvalue["error"] = ErrorCode::UidInvalid;
        return;
    }

    rtvalue["uid"] = fromuid;
    // 1014 的接收者就是认证方；明确回传 touid，供客户端填写本地消息 recv_id。
    rtvalue["touid"] = touid;
    rtvalue["name"] = user_info->user;
    rtvalue["email"] = user_info->email;
    rtvalue["nick"] = user_info->nick;
    rtvalue["desc"] = user_info->desc;
    rtvalue["sex"] = user_info->sex;
    rtvalue["icon"] = user_info->icon;
    rtvalue["bakname"] = bakname;

    // AddFriend 在一个事务内确认申请、建立双向 friend/private_chat，并写入初始消息。
    // touid 是处理申请的用户（初始消息发送者），fromuid 是原申请用户。
    std::vector<FriendAuthMessage> authMessages;
    if (!MysqlMgr::GetInstance()->AddFriend(touid, fromuid, bakname, authMessages)) {
        rtvalue["error"] = ErrorCode::RPCFaild;
        return;
    }

    rtvalue["error"] = ErrorCode::Success;
    // 1014 与 1015 使用相同的附加消息结构。即使申请方当前不在线，认证方也能
    // 通过 1014 建立自己的正式本地会话；申请方上线后的完整增量同步后续再补齐。
    Json::Value textMessages(Json::arrayValue);
    for (const FriendAuthMessage &message : authMessages) {
        Json::Value textMessage;
        textMessage["sender_id"] = static_cast<Json::Int64>(message.senderId);
        textMessage["unique_id"] = message.uniqueId;
        textMessage["msg_id"] = static_cast<Json::Int64>(message.messageId);
        textMessage["thread_id"] = static_cast<Json::Int64>(message.threadId);
        textMessage["msgcontent"] = message.content;
        textMessages.append(std::move(textMessage));
    }
    rtvalue["textmsgs"] = textMessages;

    // 查询redis 查询申请者的服务器
    auto from_ip_key = USERIPPREFIX + std::to_string(fromuid);
    std::string from_ip_value; 
    bool b_ip = RedisMgr::GetInstance()->Get(from_ip_key, from_ip_value);
    if (!b_ip) {
        return;
    }

    auto self_name = ConfigMgr::Inst()["SelfChatServer"]["name"];

    // 如果申请方在本服务器，直接通知其认证结果。
    if(self_name == from_ip_value){
        auto applicantSession = UserMgr::GetInstance()->GetSession(fromuid);
        if(applicantSession){
            UserInfo approverInfo = MysqlMgr::GetInstance()->GetUserInfo(touid);
            if (approverInfo.uid != touid) {
                return;
            }
            Json::Value notify;
            notify["error"] = ErrorCode::Success;
            notify["uid"] = touid;
            notify["touid"] = fromuid;
            notify["name"] = approverInfo.user;
            notify["nick"] = approverInfo.nick;
            notify["desc"] = approverInfo.desc;
            notify["sex"] = approverInfo.sex;
            notify["icon"] = approverInfo.icon;
            notify["bakname"] = approverInfo.user;
            notify["textmsgs"] = textMessages;
            std::string return_str = notify.toStyledString();
            applicantSession->Send(return_str, ID_NOTIFY_AUTH_FRIEND_REQ);
        }
        return;
    }

    // 申请方在其他服务器时，转发认证结果，由对端服务器通知对应 TCP 会话。
    AuthFriendReq authReq;
    // AuthFriendReq 的 fromuid 是认证方，touid 是需要接收通知的原申请方。
    authReq.set_fromuid(touid);
    authReq.set_touid(fromuid);
    for (const FriendAuthMessage &message : authMessages) {
        auto *textMessage = authReq.add_textmsgs();
        textMessage->set_sender_id(static_cast<std::int32_t>(message.senderId));
        textMessage->set_unique_id(message.uniqueId);
        textMessage->set_msg_id(static_cast<std::int32_t>(message.messageId));
        textMessage->set_thread_id(static_cast<std::int32_t>(message.threadId));
        textMessage->set_msgcontent(message.content);
    }
    ChatGrpcClient::GetInstance()->NotifyAuthFriend(from_ip_value, authReq);
} 

void LogicSystem::HandleTextMsg(std::shared_ptr<CSession> session, const short &msg_id, const std::string &msg_data){
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

    const auto fromuid = root["fromuid"].asInt();
    auto touid = root["touid"].asInt();
    const Json::Value &textarray = root["textArray"];

    Json::Value rtvalue;
    rtvalue["error"] = ErrorCode::Success;
    rtvalue["fromuid"] = fromuid;
    rtvalue["touid"] = touid;

    Defer defer([this, &rtvalue, session]{
        std::string return_str = rtvalue.toStyledString();
        session->Send(return_str, ID_TEXT_CHAT_MSG_RSP); // 发送回包，在出作用域的时候会自动调用，防御式编程处理
    });

    if (session->GetUserId() != fromuid || touid <= 0 || !textarray.isArray() || textarray.empty()) {
        rtvalue["error"] = ErrorCode::Error_Json;
        return;
    }

    std::vector<std::pair<std::string, std::string>> clientMessages;
    clientMessages.reserve(textarray.size());
    for (const auto& textObj : textarray) {
        const std::string uniqueId = textObj["msgid"].asString();
        const std::string content = textObj["content"].asString();
        if (uniqueId.empty() || content.empty()) {
            rtvalue["error"] = ErrorCode::Error_Json;
            return;
        }
        clientMessages.emplace_back(uniqueId, content);
    }

    // 先持久化再投递：接收方离线时消息仍可在下次按 message_id 游标增量同步。
    std::uint64_t threadId = 0;
    std::vector<StoredTextMessage> storedMessages;
    if (!MysqlMgr::GetInstance()->SavePrivateTextMessages(
            fromuid, touid, clientMessages, threadId, storedMessages)) {
        rtvalue["error"] = ErrorCode::RPCFaild;
        return;
    }
    rtvalue["thread_id"] = static_cast<Json::UInt64>(threadId);
    Json::Value confirmedMessages(Json::arrayValue);
    for (const StoredTextMessage& message : storedMessages) {
        Json::Value confirmed;
        confirmed["msgid"] = message.uniqueId;
        confirmed["content"] = message.content;
        confirmed["message_id"] = static_cast<Json::UInt64>(message.messageId);
        confirmed["thread_id"] = static_cast<Json::UInt64>(message.threadId);
        confirmed["sender_id"] = message.senderId;
        confirmed["recv_id"] = message.recvId;
        confirmed["created_at_ms"] = static_cast<Json::UInt64>(message.createdAtMs);
        confirmed["status"] = message.status;
        confirmedMessages.append(std::move(confirmed));
    }
    rtvalue["textArray"] = confirmedMessages;

    // 查询 Redis 决定是否实时投递；查不到并不是保存失败，1018 仍确认消息已落库。
    auto to_ip_key = USERIPPREFIX + std::to_string(touid);
    std::string to_ip_value;
    bool b_ip = RedisMgr::GetInstance()->Get(to_ip_key, to_ip_value);
    if(!b_ip){
        std::cout << "text chat target is offline, uid = " << touid << std::endl;
        rtvalue["delivered"] = false;
        return;
    }

    if(to_ip_value == ConfigMgr::Inst()["SelfChatServer"]["name"]){
        // 直接在内存中转发消息
        auto toSession = UserMgr::GetInstance()->GetSession(touid);
        if(toSession){
            // 在内存中直接通知对方
            std::cout << "text chat local push, from = " << fromuid
                      << ", to = " << touid << std::endl;
            std::string return_str = rtvalue.toStyledString();
            toSession->Send(return_str, ID_NOTIFY_TEXT_CHAT_MSG_REQ);
            rtvalue["delivered"] = true;
        } else {
            std::cout << "text chat target session missing, uid = " << touid << std::endl;
            rtvalue["delivered"] = false;
        }
        return;
    }

    // 对端在其他服务器上，转发消息
    TextChatMsgReq textChatMsgReq;
    textChatMsgReq.set_fromuid(fromuid);
    textChatMsgReq.set_touid(touid);
    // 组装已持久化后的消息，跨服目标可直接复用服务端 message_id/thread_id。
    for(const auto &message : storedMessages){
        // 向 protobuf 的 repeated textmsgs 列表追加一条消息，并返回该元素的可写指针。
        auto text_msg = textChatMsgReq.add_textmsgs();
        text_msg->set_msgcontent(message.content);
        text_msg->set_msgid(message.uniqueId);
    }

    std::cout << "text chat cross-server push, from = " << fromuid
              << ", to = " << touid << ", server = " << to_ip_value << std::endl;
    const auto rpcRsp = ChatGrpcClient::GetInstance()->NotifyTextChatMsg(to_ip_value, textChatMsgReq);
    rtvalue["error"] = rpcRsp.error();
    rtvalue["delivered"] = rpcRsp.error() == ErrorCode::Success;
}

void LogicSystem::LoadChatThreads(std::shared_ptr<CSession> session, const short &msg_id,
                                  const std::string &msg_data)
{
    Json::Value response;
    response["error"] = ErrorCode::Success;
    Defer defer([&response, session] { session->Send(response.toStyledString(), ID_LOAD_CHAT_THREAD_RSP); });

    Json::CharReaderBuilder reader;
    Json::Value request;
    std::istringstream stream(msg_data);
    std::string errors;
    if (!session || !Json::parseFromStream(reader, stream, &request, &errors)) {
        response["error"] = ErrorCode::Error_Json;
        return;
    }
    const int uid = session->GetUserId();
    const std::uint64_t afterThreadId = request.get("after_thread_id", 0).asUInt64();
    const int pageSize = request.get("page_size", 100).asInt();
    if (uid <= 0 || pageSize <= 0 || pageSize > 1000) {
        response["error"] = ErrorCode::Error_Json;
        return;
    }
    std::vector<PrivateChatThread> threads;
    if (!MysqlMgr::GetInstance()->LoadPrivateChatThreads(uid, afterThreadId, pageSize + 1, threads)) {
        response["error"] = ErrorCode::RPCFaild;
        return;
    }
    const bool loadMore = threads.size() > static_cast<size_t>(pageSize);
    if (loadMore) {
        threads.pop_back();
    }
    Json::Value threadArray(Json::arrayValue);
    std::uint64_t nextThreadId = afterThreadId;
    for (const PrivateChatThread& thread : threads) {
        Json::Value item;
        item["thread_id"] = static_cast<Json::UInt64>(thread.threadId);
        item["type"] = "private";
        item["user1_id"] = thread.user1Id;
        item["user2_id"] = thread.user2Id;
        threadArray.append(std::move(item));
        nextThreadId = thread.threadId;
    }
    response["uid"] = uid;
    response["threads"] = threadArray;
    response["next_thread_id"] = static_cast<Json::UInt64>(nextThreadId);
    response["load_more"] = loadMore;
}

void LogicSystem::LoadChatMessages(std::shared_ptr<CSession> session, const short &msg_id,
                                   const std::string &msg_data)
{
    Json::Value response;
    response["error"] = ErrorCode::Success;
    Defer defer([&response, session] {
        session->Send(response.toStyledString(), ID_LOAD_CHAT_MSG_RSP);
    });

    Json::CharReaderBuilder reader;
    Json::Value request;
    std::istringstream stream(msg_data);
    std::string errors;
    if (!Json::parseFromStream(reader, stream, &request, &errors) || !session) {
        response["error"] = ErrorCode::Error_Json;
        return;
    }

    const int uid = session->GetUserId();
    const std::uint64_t threadId = request["thread_id"].asUInt64();
    const std::uint64_t afterMessageId = request.get("after_message_id", 0).asUInt64();
    const int pageSize = request.get("page_size", 50).asInt();
    if (uid <= 0 || threadId == 0 || pageSize <= 0 || pageSize > 1000) {
        response["error"] = ErrorCode::Error_Json;
        return;
    }

    // 多取一条只用于判断 load_more；真正返回给客户端的永远不超过 pageSize 条。
    std::vector<StoredTextMessage> messages;
    if (!MysqlMgr::GetInstance()->LoadPrivateTextMessages(
            uid, threadId, afterMessageId, pageSize + 1, messages)) {
        response["error"] = ErrorCode::RPCFaild;
        return;
    }
    const bool loadMore = messages.size() > static_cast<size_t>(pageSize);
    if (loadMore) {
        messages.pop_back();
    }

    Json::Value messageArray(Json::arrayValue);
    std::uint64_t nextMessageId = afterMessageId;
    for (const StoredTextMessage& message : messages) {
        Json::Value item;
        item["message_id"] = static_cast<Json::UInt64>(message.messageId);
        item["thread_id"] = static_cast<Json::UInt64>(message.threadId);
        item["sender_id"] = message.senderId;
        item["recv_id"] = message.recvId;
        item["content"] = message.content;
        item["created_at_ms"] = static_cast<Json::UInt64>(message.createdAtMs);
        item["status"] = message.status;
        messageArray.append(std::move(item));
        nextMessageId = message.messageId;
    }
    response["thread_id"] = static_cast<Json::UInt64>(threadId);
    response["messages"] = messageArray;
    response["next_message_id"] = static_cast<Json::UInt64>(nextMessageId);
    response["load_more"] = loadMore;
}

void LogicSystem::CreatePrivateChat(std::shared_ptr<CSession> session, const short &msg_id, const std::string &msg_data){
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

    // uid 必须来自已经完成 ChatServer 登录的 session；请求 JSON 中的 uid 仅为协议兼容字段，
    // 不可作为身份凭证，否则客户端可以伪造 uid 为任意用户创建会话。
    const int uid = session ? session->GetUserId() : 0;
    auto other_id = root["other_id"].asInt();

    Json::Value rtvalue;
    rtvalue["error"] = ErrorCode::Success;
    rtvalue["uid"] = uid;
    rtvalue["other_id"] = other_id;

    Defer defer([this, &rtvalue, session]{
        std::string return_str = rtvalue.toStyledString();
        session->Send(return_str, ID_CREATE_PRIVATE_CHAT_RSP); // 发送回包，在出作用域的时候会自动调用，防御式编程处理
    });

    if (uid <= 0) {
        rtvalue["error"] = ErrorCode::TokenInvalid;
        return;
    }

    // chat_thread.id 是 BIGINT UNSIGNED，调用方使用同样的 64 位类型接收输出参数。
    std::uint64_t thread_id = 0;
    bool res = MysqlMgr::GetInstance()->CreatePrivateChat(uid, other_id, thread_id);
    if(!res){
        rtvalue["error"] = ErrorCode::Create_Chat_Failed;
        return;
    }

    rtvalue["thread_id"] = thread_id;
}

