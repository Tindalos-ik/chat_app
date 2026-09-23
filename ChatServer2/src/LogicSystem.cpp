#include "LogicSystem.h"
#include <json.h>
#include <charconv>
#include <cstdint>
#include <system_error>
#include <sstream>
#include <iostream>
#include <cctype>
#include "MysqlMgr.h"
#include "RedisMgr.h"
#include "UserMgr.h"
#include "ChatGrpcClient.h"
#include "ResourceGrpcClient.h"
#include "CServer.h"

using namespace std;

namespace {

// Qt 为避免 JSON double 损失 64 位整数精度，将 thread_id/message_id 游标以字符串发送。
// 这里仅接受十进制无符号整数字符串；格式不正确的请求由调用方返回 Error_Json。
bool ParseUint64StringField(const Json::Value& object, const char* fieldName,
                            std::uint64_t& value)
{
    if (!object.isObject() || !object.isMember(fieldName) || !object[fieldName].isString()) {
        return false;
    }

    const std::string text = object[fieldName].asString();
    if (text.empty()) {
        return false;
    }

    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value, 10);
    return result.ec == std::errc() && result.ptr == end;
}

// 图片发送确认、同服通知和跨服通知都使用这一个序列化入口，避免其中某条链路遗漏
// ResourceServer 已核验的字段。客户端提交的同名字段永远不会进入这里。
Json::Value BuildVerifiedImageJson(const StoredTextMessage& message) {
    Json::Value item;
    item["msgid"] = message.uniqueId;
    item["resource_id"] = message.resourceId;
    item["name"] = message.name;
    item["mime_type"] = message.mimeType;
    item["file_size"] = static_cast<Json::UInt64>(message.fileSize);
    item["width"] = message.width;
    item["height"] = message.height;
    item["message_id"] = static_cast<Json::UInt64>(message.messageId);
    item["thread_id"] = static_cast<Json::UInt64>(message.threadId);
    item["sender_id"] = message.senderId;
    item["recv_id"] = message.recvId;
    item["created_at_ms"] = static_cast<Json::UInt64>(message.createdAtMs);
    item["status"] = message.status;
    item["peer_displayed"] = message.peerDisplayed;
    return item;
}

Json::Value BuildVerifiedFileJson(const StoredTextMessage& message) {
    Json::Value item;
    item["msgid"] = message.uniqueId;
    item["resource_id"] = message.resourceId;
    item["name"] = message.name;
    item["mime_type"] = message.mimeType;
    item["file_size"] = static_cast<Json::UInt64>(message.fileSize);
    item["message_id"] = static_cast<Json::UInt64>(message.messageId);
    item["thread_id"] = static_cast<Json::UInt64>(message.threadId);
    item["sender_id"] = message.senderId;
    item["recv_id"] = message.recvId;
    item["created_at_ms"] = static_cast<Json::UInt64>(message.createdAtMs);
    item["status"] = message.status;
    return item;
}

// 图片请求的候选大小和尺寸不会被信任，但固定协议仍要求它们是非负整数，避免不同版本
// 的客户端把不完整对象误当作图片发送请求。JsonCpp 会将负数 asUInt64 转成大数，故需先判型。
bool IsNonNegativeJsonInteger(const Json::Value& value) {
    return value.isUInt() || value.isUInt64() ||
        (value.isInt() && value.asInt() >= 0) ||
        (value.isInt64() && value.asInt64() >= 0);
}

// 1030 的消息体由 2 字节长度字段承载。历史分页将单个 JSON 包限定为 4 KiB，
// 给图片元数据等可变字段留出确定边界；使用与实际发送完全相同的紧凑序列化结果计数，
// 不能根据消息数量或字段长度估算。
constexpr std::size_t kMaxHistoryResponseJsonBytes = 4 * 1024;

std::string SerializeCompactJson(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

Json::Value BuildHistoryMessageJson(const StoredTextMessage& message) {
    Json::Value item;
    item["message_id"] = static_cast<Json::UInt64>(message.messageId);
    item["thread_id"] = static_cast<Json::UInt64>(message.threadId);
    item["sender_id"] = message.senderId;
    item["recv_id"] = message.recvId;
    item["content"] = message.content;
    item["message_type"] = message.messageType;
    if (message.messageType == "image" || message.messageType == "file") {
        // 离线图片下载依赖这些已由 ResourceServer 核验过的元数据。
        item["msgid"] = message.uniqueId;
        item["resource_id"] = message.resourceId;
        item["name"] = message.name;
        item["mime_type"] = message.mimeType;
        item["file_size"] = static_cast<Json::UInt64>(message.fileSize);
        if (message.messageType == "image") {
            item["width"] = message.width;
            item["height"] = message.height;
        }
    }
    item["created_at_ms"] = static_cast<Json::UInt64>(message.createdAtMs);
    item["status"] = message.status;
    item["peer_displayed"] = message.peerDisplayed;
    return item;
}

Json::Value BuildHistoryResponse(std::uint64_t threadId, std::uint64_t nextMessageId,
                                 const Json::Value& messages, bool loadMore) {
    Json::Value response;
    response["error"] = ErrorCode::Success;
    response["thread_id"] = static_cast<Json::UInt64>(threadId);
    response["messages"] = messages;
    response["next_message_id"] = static_cast<Json::UInt64>(nextMessageId);
    response["load_more"] = loadMore;
    return response;
}

} // namespace

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
        // 业务处理器运行在线程入口之外，必须兜底捕获异常；否则任意一次 JSON、
        // MySQL 或 Redis 异常都会触发 std::terminate 并终止整个 ChatServer 进程。
        try {
            call_back_iter->second(msg_node->_session, msg_node->_recvnode->_msg_id,
                std::string(msg_node->_recvnode->_data, msg_node->_recvnode->_cur_len));
        } catch (const std::exception& e) {
            std::cerr << "logic handler exception, msg id ["
                      << msg_node->_recvnode->_msg_id << "]: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "logic handler unknown exception, msg id ["
                      << msg_node->_recvnode->_msg_id << "]" << std::endl;
        }
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
    _fun_callbacks[ID_IMAGE_CHAT_MSG_REQ] = [this](std::shared_ptr<CSession> session,
                                             const short &msg_id,
                                             const std::string &msg_data) {
        HandleImageMsg(session, msg_id, msg_data);
    };
    _fun_callbacks[ID_FILE_CHAT_MSG_REQ] = [this](std::shared_ptr<CSession> session,
                                            const short &msg_id, const std::string &msg_data) {
        HandleFileMsg(session, msg_id, msg_data);
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
    _fun_callbacks[ID_MESSAGE_DISPLAY_ACK_REQ] = [this](std::shared_ptr<CSession> session,
                                                   const short &msg_id, const std::string &msg_data) {
        HandleMessageDisplayed(session, msg_id, msg_data);
    };
    _fun_callbacks[ID_MARK_THREAD_READ_REQ] = [this](std::shared_ptr<CSession> session,
                                               const short &msg_id, const std::string &msg_data) {
        HandleThreadRead(session, msg_id, msg_data);
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
    // 即使后续校验、落库或投递失败，也回传客户端原始 msgid。Qt 端依靠这个 UUID
    // 找到乐观展示的那一个气泡并显示失败图标；成功落库后会被 confirmedMessages 覆盖。
    if (textarray.isArray()) {
        rtvalue["textArray"] = textarray;
    }

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
    // “落库成功”“已排入实时 TCP 发送队列”“对方实际显示”是三个不同阶段。
    rtvalue["persisted"] = true;
    rtvalue["realtime_delivered"] = false;
    rtvalue["peer_displayed"] = false;
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
            rtvalue["realtime_delivered"] = true;
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
        // 只在 MySQL 已成功写入后才跨服转发这组元数据；目标 ChatServer 可以原样
        // 映射为新版 ID_NOTIFY_TEXT_CHAT_MSG_REQ，让接收方客户端立即 SQLite 去重落库。
        auto text_msg = textChatMsgReq.add_textmsgs();
        text_msg->set_msgcontent(message.content);
        text_msg->set_msgid(message.uniqueId);
        text_msg->set_message_id(message.messageId);
        text_msg->set_thread_id(message.threadId);
        text_msg->set_sender_id(message.senderId);
        text_msg->set_recv_id(message.recvId);
        text_msg->set_created_at_ms(message.createdAtMs);
        text_msg->set_status(message.status);
    }

    std::cout << "text chat cross-server push, from = " << fromuid
              << ", to = " << touid << ", server = " << to_ip_value << std::endl;
    const auto rpcRsp = ChatGrpcClient::GetInstance()->NotifyTextChatMsg(to_ip_value, textChatMsgReq);
    // MySQL 已提交后，跨服实时投递失败不是发送失败。保留 error=Success 让发送端
    // 确认“已保存”，再单独报告 realtime_delivered/delivery_error；否则用户会重发并
    // 产生重复持久化消息。
    rtvalue["delivered"] = rpcRsp.error() == ErrorCode::Success;
    rtvalue["realtime_delivered"] = rpcRsp.error() == ErrorCode::Success;
    if (rpcRsp.error() != ErrorCode::Success) {
        rtvalue["delivery_error"] = rpcRsp.error();
    }
}

void LogicSystem::HandleImageMsg(std::shared_ptr<CSession> session, const short &msg_id,
                                 const std::string &msg_data) {
    (void)msg_id;
    Json::Value response;
    response["error"] = ErrorCode::Success;
    Defer defer([&response, session] {
        session->Send(response.toStyledString(), ID_IMAGE_CHAT_MSG_RSP);
    });

    Json::CharReaderBuilder reader;
    Json::Value request;
    std::istringstream stream(msg_data);
    std::string errors;
    if (!session || !Json::parseFromStream(reader, stream, &request, &errors) || !request.isObject()) {
        response["error"] = ErrorCode::Error_Json;
        return;
    }
    const int fromuid = request["fromuid"].asInt();
    const int touid = request["touid"].asInt();
    const Json::Value& imageArray = request["imageArray"];
    response["fromuid"] = fromuid;
    response["touid"] = touid;
    // 即使后续资源核验或落库失败，也必须回传客户端原始 msgid。桌面端依靠
    // imageArray 中的 UUID 标记对应的乐观图片气泡为失败，不能只收到一个 error。
    if (imageArray.isArray()) {
        response["imageArray"] = imageArray;
    }

    // 发送者身份只能取已登录 TCP 会话；名称/MIME/尺寸/大小即使存在也仅作兼容读取，
    // 完全不参与信任决策。小批次上限也避免长期占用单工作线程和资源文件锁。
    if (session->GetUserId() != fromuid || touid <= 0 || fromuid == touid ||
        !imageArray.isArray() || imageArray.empty() || imageArray.size() > 50) {
        response["error"] = ErrorCode::Error_Json;
        return;
    }

    message::ResourceVerifyReq verifyRequest;
    std::vector<std::string> messageIds;
    messageIds.reserve(imageArray.size());
    for (const Json::Value& image : imageArray) {
        if (!image.isObject() || !image["msgid"].isString() || !image["resource_id"].isString() ||
            !image["name"].isString() || !image["mime_type"].isString() ||
            !IsNonNegativeJsonInteger(image["file_size"]) ||
            !IsNonNegativeJsonInteger(image["width"]) || !IsNonNegativeJsonInteger(image["height"])) {
            response["error"] = ErrorCode::Error_Json;
            return;
        }
        const std::string msgid = image["msgid"].asString();
        const std::string resourceId = image["resource_id"].asString();
        if (msgid.empty() || msgid.size() > 128 || resourceId.empty()) {
            response["error"] = ErrorCode::Error_Json;
            return;
        }
        messageIds.push_back(msgid);
        verifyRequest.add_resource_ids(resourceId);
    }

    // 先向资源服务核验整批资源；RPC 失败、未完成资源和伪装图片都不会触碰 chat_message。
    const message::ResourceVerifyRsp verifyResponse = ResourceGrpcClient::GetInstance()->VerifyImages(verifyRequest);
    if (verifyResponse.error() != ErrorCode::Success ||
        verifyResponse.resources_size() != verifyRequest.resource_ids_size()) {
        response["error"] = verifyResponse.error() == ErrorCode::Success
            ? ErrorCode::RPCFaild : verifyResponse.error();
        std::cout << "image resource verification failed, from=" << fromuid
                  << ", to=" << touid << ", error=" << response["error"].asInt()
                  << ", resource_count=" << verifyRequest.resource_ids_size() << std::endl;
        return;
    }
    std::vector<VerifiedImageMessage> verifiedImages;
    verifiedImages.reserve(messageIds.size());
    for (int index = 0; index < verifyResponse.resources_size(); ++index) {
        const auto& resource = verifyResponse.resources(index);
        // 防御错误部署或协议错配：回包必须严格保持输入顺序和 ID。
        if (resource.resource_id() != verifyRequest.resource_ids(index) || resource.name().empty() ||
            resource.mime_type().empty() || resource.file_size() == 0 || resource.width() == 0 || resource.height() == 0) {
            response["error"] = ErrorCode::RPCFaild;
            return;
        }
        VerifiedImageMessage image;
        image.uniqueId = messageIds[static_cast<std::size_t>(index)];
        image.resourceId = resource.resource_id();
        image.name = resource.name();
        image.mimeType = resource.mime_type();
        image.fileSize = resource.file_size();
        image.width = resource.width();
        image.height = resource.height();
        verifiedImages.push_back(std::move(image));
    }

    std::uint64_t threadId = 0;
    std::vector<StoredTextMessage> storedImages;
    if (!MysqlMgr::GetInstance()->SavePrivateImageMessages(
            fromuid, touid, verifiedImages, threadId, storedImages)) {
        response["error"] = ErrorCode::RPCFaild;
        return;
    }

    Json::Value canonicalImages(Json::arrayValue);
    for (const StoredTextMessage& image : storedImages) {
        canonicalImages.append(BuildVerifiedImageJson(image));
    }
    response["thread_id"] = static_cast<Json::UInt64>(threadId);
    response["imageArray"] = canonicalImages;
    response["persisted"] = true;
    response["realtime_delivered"] = false;
    response["peer_displayed"] = false;

    // 落库成功即为 1034 成功。实时推送只是加速路径；对端离线、路由失效或跨服 RPC
    // 失败时仍可经 1029/1030 增量取回，不能诱导发送方重复上传或重复落库。
    response["delivered"] = false;
    std::string targetServer;
    if (!RedisMgr::GetInstance()->Get(USERIPPREFIX + std::to_string(touid), targetServer)) {
        // Redis 没有目标用户路由时，消息已经安全落库；仅跳过实时推送，客户端可通过
        // 历史增量同步取到该图片。该日志用于区分“用户离线”和 RPC 投递故障。
        std::cout << "image chat target offline, from=" << fromuid
                  << ", to=" << touid << std::endl;
        return;
    }
    const std::string selfServer = ConfigMgr::Inst()["SelfChatServer"]["name"];
    std::cout << "image chat route resolved, from=" << fromuid
              << ", to=" << touid << ", source_server=" << selfServer
              << ", target_server=" << targetServer << std::endl;
    if (targetServer == selfServer) {
        auto targetSession = UserMgr::GetInstance()->GetSession(touid);
        if (targetSession) {
            Json::Value notification = response;
            notification["delivered"] = true;
            targetSession->Send(notification.toStyledString(), ID_NOTIFY_IMAGE_CHAT_MSG_REQ);
            response["delivered"] = true;
            response["realtime_delivered"] = true;
            std::cout << "image chat local push succeeded, from=" << fromuid
                      << ", to=" << touid << std::endl;
        } else {
            // Redis 路由仍指向本机但内存会话已断开，不能伪装成实时投递成功。
            std::cout << "image chat local session missing, from=" << fromuid
                      << ", to=" << touid << ", server=" << selfServer << std::endl;
        }
        return;
    }

    message::ImageChatMsgReq pushRequest;
    pushRequest.set_fromuid(fromuid);
    pushRequest.set_touid(touid);
    for (const StoredTextMessage& image : storedImages) {
        auto* item = pushRequest.add_imagemsgs();
        item->set_msgid(image.uniqueId);
        item->set_resource_id(image.resourceId);
        item->set_name(image.name);
        item->set_mime_type(image.mimeType);
        item->set_file_size(image.fileSize);
        item->set_width(image.width);
        item->set_height(image.height);
        item->set_message_id(image.messageId);
        item->set_thread_id(image.threadId);
        item->set_sender_id(image.senderId);
        item->set_recv_id(image.recvId);
        item->set_created_at_ms(image.createdAtMs);
        item->set_status(image.status);
    }
    const auto pushResponse = ChatGrpcClient::GetInstance()->NotifyImageChatMsg(targetServer, pushRequest);
    response["delivered"] = pushResponse.error() == ErrorCode::Success;
    response["realtime_delivered"] = pushResponse.error() == ErrorCode::Success;
    if (pushResponse.error() != ErrorCode::Success) {
        response["delivery_error"] = pushResponse.error();
        std::cout << "image chat cross-server push failed, from=" << fromuid
                  << ", to=" << touid << ", target_server=" << targetServer
                  << ", business_error=" << pushResponse.error() << std::endl;
    } else {
        std::cout << "image chat cross-server push succeeded, from=" << fromuid
                  << ", to=" << touid << ", target_server=" << targetServer << std::endl;
    }
}

// 私聊文件入口：先用 ResourceServer 的已发布资源记录替换所有客户端候选元数据，
// 再事务落库；成功后才确认发送方并尝试实时通知接收方。
void LogicSystem::HandleFileMsg(std::shared_ptr<CSession> session, const short& msg_id,
                                const std::string& msg_data) {
    (void)msg_id;
    Json::Value response;
    response["error"] = ErrorCode::Success;
    Defer defer([&response, session] {
        session->Send(response.toStyledString(), ID_FILE_CHAT_MSG_RSP);
    });
    Json::CharReaderBuilder reader;
    Json::Value request;
    std::istringstream stream(msg_data);
    std::string errors;
    if (!session || !Json::parseFromStream(reader, stream, &request, &errors) || !request.isObject()) {
        response["error"] = ErrorCode::Error_Json;
        return;
    }
    const int fromuid = request["fromuid"].asInt();
    const int touid = request["touid"].asInt();
    const Json::Value& fileArray = request["fileArray"];
    response["fromuid"] = fromuid;
    response["touid"] = touid;
    if (fileArray.isArray()) response["fileArray"] = fileArray; // 失败时保留 msgid，客户端可定位气泡。
    if (session->GetUserId() != fromuid || touid <= 0 || fromuid == touid ||
        !fileArray.isArray() || fileArray.empty() || fileArray.size() > 50) {
        response["error"] = ErrorCode::Error_Json;
        return;
    }

    message::ResourceFileVerifyReq verifyRequest;
    std::vector<std::string> msgids;
    msgids.reserve(fileArray.size());
    for (const auto& item : fileArray) {
        // 客户端只提供资源句柄与关联 UUID；其他任何声明都不能成为数据库元数据来源。
        if (!item.isObject() || !item["msgid"].isString() || !item["resource_id"].isString()) {
            response["error"] = ErrorCode::Error_Json;
            return;
        }
        const std::string msgid = item["msgid"].asString();
        const std::string resourceId = item["resource_id"].asString();
        if (msgid.empty() || msgid.size() > 128 || resourceId.empty()) {
            response["error"] = ErrorCode::Error_Json;
            return;
        }
        msgids.push_back(msgid);
        verifyRequest.add_resource_ids(resourceId);
    }

    // VerifyFiles 原子核验资源是否已发布并返回权威文件属性；任何错误都在碰数据库前退出。
    const auto verified = ResourceGrpcClient::GetInstance()->VerifyFiles(verifyRequest);
    if (verified.error() != ErrorCode::Success ||
        verified.resources_size() != verifyRequest.resource_ids_size()) {
        response["error"] = verified.error() == ErrorCode::Success ? ErrorCode::RPCFaild : verified.error();
        return;
    }
    std::vector<VerifiedFileMessage> files;
    files.reserve(msgids.size());
    for (int i = 0; i < verified.resources_size(); ++i) {
        const auto& resource = verified.resources(i);
        // 按索引绑定 msgid，同时核对资源 ID，避免错误或版本不匹配的响应串换资源。
        if (resource.resource_id() != verifyRequest.resource_ids(i) || resource.name().empty() ||
            resource.mime_type().empty()) {
            response["error"] = ErrorCode::RPCFaild;
            return;
        }
        VerifiedFileMessage file;
        file.uniqueId = msgids[static_cast<std::size_t>(i)];
        file.resourceId = resource.resource_id();
        file.name = resource.name();
        file.mimeType = resource.mime_type();
        file.fileSize = resource.file_size();
        files.push_back(std::move(file));
    }

    std::uint64_t threadId = 0;
    std::vector<StoredTextMessage> stored;
    if (!MysqlMgr::GetInstance()->SavePrivateFileMessages(fromuid, touid, files, threadId, stored)) {
        response["error"] = ErrorCode::RPCFaild;
        return;
    }
    Json::Value confirmed(Json::arrayValue);
    for (const auto& file : stored) confirmed.append(BuildVerifiedFileJson(file));
    response["thread_id"] = static_cast<Json::UInt64>(threadId);
    response["fileArray"] = confirmed;
    response["persisted"] = true;
    response["realtime_delivered"] = false;
    response["delivered"] = false;

    // 落库结果已成功。在线推送只是加速路径，其失败必须保留 1041 成功语义以免客户端重发。
    std::string targetServer;
    if (!RedisMgr::GetInstance()->Get(USERIPPREFIX + std::to_string(touid), targetServer)) return;
    const std::string selfServer = ConfigMgr::Inst()["SelfChatServer"]["name"];
    if (targetServer == selfServer) {
        const auto target = UserMgr::GetInstance()->GetSession(touid);
        if (target) {
            Json::Value notice;
            notice["error"] = ErrorCode::Success;
            notice["fromuid"] = fromuid;
            notice["touid"] = touid;
            notice["delivered"] = true;
            notice["fileArray"] = confirmed;
            target->Send(notice.toStyledString(), ID_NOTIFY_FILE_CHAT_MSG_REQ);
            response["delivered"] = true;
            response["realtime_delivered"] = true;
        }
        return;
    }
    message::FileChatMsgReq push;
    push.set_fromuid(fromuid);
    push.set_touid(touid);
    for (const auto& file : stored) {
        auto* data = push.add_filemsgs();
        data->set_msgid(file.uniqueId); data->set_resource_id(file.resourceId);
        data->set_name(file.name); data->set_mime_type(file.mimeType); data->set_file_size(file.fileSize);
        data->set_message_id(file.messageId); data->set_thread_id(file.threadId);
        data->set_sender_id(file.senderId); data->set_recv_id(file.recvId);
        data->set_created_at_ms(file.createdAtMs); data->set_status(file.status);
    }
    const auto pushResult = ChatGrpcClient::GetInstance()->NotifyFileChatMsg(targetServer, push);
    response["delivered"] = pushResult.error() == ErrorCode::Success;
    response["realtime_delivered"] = pushResult.error() == ErrorCode::Success;
    if (pushResult.error() != ErrorCode::Success) response["delivery_error"] = pushResult.error();
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
    if (!session || !Json::parseFromStream(reader, stream, &request, &errors)
        || !request.isObject()) {
        response["error"] = ErrorCode::Error_Json;
        return;
    }
    const int uid = session->GetUserId();
    std::uint64_t afterThreadId = 0;
    if (!ParseUint64StringField(request, "after_thread_id", afterThreadId)) {
        response["error"] = ErrorCode::Error_Json;
        return;
    }
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
    (void)msg_id;
    if (!session) {
        return;
    }
    auto sendResponse = [session](const Json::Value& response) {
        std::string payload = SerializeCompactJson(response);
        if (payload.size() > kMaxHistoryResponseJsonBytes) {
            // 这里是兜底，正常成功路径在逐条加入时已测量。任何将来新增的固定字段
            // 也不能绕过帧上限而被截断写入 2 字节长度协议。
            Json::Value errorResponse;
            errorResponse["error"] = ErrorCode::MessageTooLarge;
            payload = SerializeCompactJson(errorResponse);
        }
        session->Send(payload, ID_LOAD_CHAT_MSG_RSP);
    };
    auto sendError = [&sendResponse](int error) {
        Json::Value response;
        response["error"] = error;
        sendResponse(response);
    };

    Json::CharReaderBuilder reader;
    Json::Value request;
    std::istringstream stream(msg_data);
    std::string errors;
    if (!Json::parseFromStream(reader, stream, &request, &errors) || !request.isObject()) {
        sendError(ErrorCode::Error_Json);
        return;
    }

    const int uid = session->GetUserId();
    std::uint64_t threadId = 0;
    std::uint64_t afterMessageId = 0;
    if (!ParseUint64StringField(request, "thread_id", threadId)
        || !ParseUint64StringField(request, "after_message_id", afterMessageId)) {
        sendError(ErrorCode::Error_Json);
        return;
    }
    const int pageSize = request.get("page_size", 50).asInt();
    if (uid <= 0 || threadId == 0 || pageSize <= 0 || pageSize > 1000) {
        sendError(ErrorCode::Error_Json);
        return;
    }

    // 多取一条只用于判断 load_more；真正返回给客户端的永远不超过 pageSize 条。
    std::vector<StoredTextMessage> messages;
    if (!MysqlMgr::GetInstance()->LoadPrivateTextMessages(
            uid, threadId, afterMessageId, pageSize + 1, messages)) {
        sendError(ErrorCode::RPCFaild);
        return;
    }

    // 除了请求页大小外，按实际 JSON 序列化后的字节数再切一层。客户端已按
    // next_message_id/load_more 拉取下一页，因此较小的实际页仍是同一份 1030 协议。
    Json::Value messageArray(Json::arrayValue);
    std::uint64_t nextMessageId = afterMessageId;
    size_t returnedCount = 0;
    for (const StoredTextMessage& message : messages) {
        if (returnedCount >= static_cast<size_t>(pageSize)) {
            break;
        }

        Json::Value candidateArray = messageArray;
        candidateArray.append(BuildHistoryMessageJson(message));
        // 先按 load_more=true 测量：这比最终没有后续页时的 false 至少不小，
        // 因而最终实际发送的 JSON 也一定不超过预算。
        const Json::Value candidateResponse = BuildHistoryResponse(
            threadId, message.messageId, candidateArray, true);
        if (SerializeCompactJson(candidateResponse).size() > kMaxHistoryResponseJsonBytes) {
            if (messageArray.empty()) {
                // 不能拆分一条消息而让客户端把片段当作完整内容，也不能不推进游标后让
                // 客户端无限请求同一条。因此明确失败并保留原游标，供上层处理。
                sendError(ErrorCode::MessageTooLarge);
                return;
            }
            break;
        }

        messageArray = std::move(candidateArray);
        nextMessageId = message.messageId;
        ++returnedCount;
    }

    // messages 最多有 pageSize + 1 条。未放入的多取一条既可能来自请求页边界，
    // 也可能来自 4 KiB 边界；两种情况都必须让客户端从本帧游标继续读取。
    const bool loadMore = returnedCount < messages.size();
    sendResponse(BuildHistoryResponse(threadId, nextMessageId, messageArray, loadMore));
}

void LogicSystem::HandleMessageDisplayed(std::shared_ptr<CSession> session, const short &msg_id,
                                         const std::string &msg_data)
{
    // 1036 是客户端的展示确认，而非可信的“消息已读”声明。服务端只接受当前会话
    // 对属于自己的私聊消息做状态提升，数据库层还会再次校验成员关系和接收者身份。
    (void)msg_id;
    if (!session || session->GetUserId() <= 0) {
        return;
    }
    Json::CharReaderBuilder reader;
    Json::Value request;
    std::istringstream stream(msg_data);
    std::string errors;
    std::uint64_t threadId = 0;
    if (!Json::parseFromStream(reader, stream, &request, &errors) || !request.isObject()
        || !ParseUint64StringField(request, "thread_id", threadId)
        || !request["message_ids"].isArray() || request["message_ids"].empty()
        || request["message_ids"].size() > 100) {
        return;
    }
    std::vector<std::uint64_t> messageIds;
    messageIds.reserve(request["message_ids"].size());
    for (const Json::Value& item : request["message_ids"]) {
        if (!item.isString()) {
            return;
        }
        std::uint64_t messageId = 0;
        const std::string value = item.asString();
        const auto result = std::from_chars(value.data(), value.data() + value.size(), messageId, 10);
        if (value.empty() || result.ec != std::errc() || result.ptr != value.data() + value.size() || messageId == 0) {
            return;
        }
        messageIds.push_back(messageId);
    }
    std::vector<DisplayReceipt> receipts;
    if (!MysqlMgr::GetInstance()->MarkMessagesDisplayed(session->GetUserId(), threadId, messageIds, receipts)) {
        std::cerr << "message display ack persistence failed, reader=" << session->GetUserId()
                  << ", thread=" << threadId << std::endl;
        return;
    }
    for (const DisplayReceipt& receipt : receipts) {
        ForwardDisplayReceipt(receipt);
    }
}

void LogicSystem::HandleThreadRead(std::shared_ptr<CSession> session, const short &msg_id,
                                   const std::string &msg_data)
{
    // 已读游标来自客户端，因此不能直接据此通知对端；必须先持久化并由数据库筛出
    // 从未读变为已读的实际记录，避免伪造游标和重复请求产生错误回执。
    (void)msg_id;
    if (!session || session->GetUserId() <= 0) {
        return;
    }
    Json::CharReaderBuilder reader;
    Json::Value request;
    std::istringstream stream(msg_data);
    std::string errors;
    std::uint64_t threadId = 0;
    std::uint64_t readThroughMessageId = 0;
    if (!Json::parseFromStream(reader, stream, &request, &errors) || !request.isObject()
        || !ParseUint64StringField(request, "thread_id", threadId)
        || !ParseUint64StringField(request, "read_through_message_id", readThroughMessageId)) {
        return;
    }
    std::vector<ReadReceipt> receipts;
    if (!MysqlMgr::GetInstance()->MarkPrivateThreadRead(session->GetUserId(), threadId,
                                                        readThroughMessageId, receipts)) {
        std::cerr << "thread read receipt persistence failed, reader=" << session->GetUserId()
                  << ", thread=" << threadId << std::endl;
        return;
    }
    for (const ReadReceipt& receipt : receipts) {
        ForwardReadReceipt(receipt);
    }
}

void LogicSystem::ForwardDisplayReceipt(const DisplayReceipt& receipt)
{
    // 展示状态已先写入 MySQL。实时通知只是在线优化，Redis 路由缺失、会话离线或
    // 跨服 RPC 失败都不能影响已写入的事实；发送端下次 1030 同步仍会得到状态。
    if (receipt.senderId <= 0 || receipt.readerId <= 0 || receipt.threadId == 0 || receipt.messageIds.empty()) {
        return;
    }
    Json::Value notification;
    notification["error"] = ErrorCode::Success;
    notification["thread_id"] = static_cast<Json::UInt64>(receipt.threadId);
    notification["reader_id"] = receipt.readerId;
    notification["peer_displayed"] = true;
    Json::Value ids(Json::arrayValue);
    for (std::uint64_t id : receipt.messageIds) {
        ids.append(static_cast<Json::UInt64>(id));
    }
    notification["message_ids"] = ids;

    std::string targetServer;
    if (!RedisMgr::GetInstance()->Get(USERIPPREFIX + std::to_string(receipt.senderId), targetServer)) {
        return; // 离线发送端下次历史同步时会读取 displayed_at。
    }
    if (targetServer == ConfigMgr::Inst()["SelfChatServer"]["name"]) {
        if (const auto senderSession = UserMgr::GetInstance()->GetSession(receipt.senderId)) {
            senderSession->Send(notification.toStyledString(), ID_NOTIFY_MESSAGE_DISPLAYED);
        }
        return;
    }
    MessageDisplayedReq request;
    request.set_sender_id(receipt.senderId);
    request.set_reader_id(receipt.readerId);
    request.set_thread_id(receipt.threadId);
    for (std::uint64_t id : receipt.messageIds) {
        request.add_message_ids(id);
    }
    ChatGrpcClient::GetInstance()->NotifyMessageDisplayed(targetServer, request);
}

void LogicSystem::ForwardReadReceipt(const ReadReceipt& receipt)
{
    // 与展示确认相同，已读通知允许丢失实时路径：状态以数据库为准，离线发送端通过
    // 后续历史同步恢复，而不应让读取端为通知失败重复修改消息状态。
    if (receipt.senderId <= 0 || receipt.readerId <= 0 || receipt.threadId == 0
        || receipt.readThroughMessageId == 0) {
        return;
    }
    Json::Value notification;
    notification["error"] = ErrorCode::Success;
    notification["thread_id"] = static_cast<Json::UInt64>(receipt.threadId);
    notification["reader_id"] = receipt.readerId;
    notification["read_through_message_id"] = static_cast<Json::UInt64>(receipt.readThroughMessageId);

    std::string targetServer;
    if (!RedisMgr::GetInstance()->Get(USERIPPREFIX + std::to_string(receipt.senderId), targetServer)) {
        return;
    }
    if (targetServer == ConfigMgr::Inst()["SelfChatServer"]["name"]) {
        if (const auto senderSession = UserMgr::GetInstance()->GetSession(receipt.senderId)) {
            senderSession->Send(notification.toStyledString(), ID_NOTIFY_THREAD_READ);
        }
        return;
    }
    ThreadReadReq request;
    request.set_sender_id(receipt.senderId);
    request.set_reader_id(receipt.readerId);
    request.set_thread_id(receipt.threadId);
    request.set_read_through_message_id(receipt.readThroughMessageId);
    ChatGrpcClient::GetInstance()->NotifyThreadRead(targetServer, request);
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

