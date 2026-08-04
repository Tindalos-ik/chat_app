# 登录功能全链路解析



---

## 1. 登录流程总览

```text
┌────────┐ ① /user_login (HTTP+JSON)   ┌────────────┐
│ 客户端  │ ──────────────────────────► │ GateServer │
│ (Qt)   │                             └─────┬──────┘
└────────┘                                   │ ② 查 MySQL：用户存在？密码正确？
    ▲                                       │ ③ gRPC GetChatServer(uid)
    │                                       ▼
    │                                ┌────────────┐
    │                                │StatusServer│ ④ 负载均衡选 ChatServer
    │                                └─────┬──────┘    生成 token，存 Redis(utoken_uid)
    │                                      │ ⑤ 返回 {host, port, token}
    │                                      ▼
    │  ⑥ 客户端收到 host/port/token
    │    建立 TCP 长连接
    │                                      ┌────────────┐
    │ ⑦ MSG_CHAT_LOGIN {uid, token} ─────► │ ChatServer │
    └────────────────────────────────────── │  ⑧ 校验 token（StatusServer / Redis）
         ⑨ MSG_CHAT_LOGIN_RSP ◄──────────── │  ⑨ 加载用户资料、好友列表
                                           │  ⑩ 踢掉旧登录、绑定 uid/session
                                           └────────────┘
```

核心思想：**HTTP 登录负责"验身份"（用户+密码），TCP 登录负责"验凭证"（token）**，两层各司其职。

---

## 2. 第一阶段：HTTP 登录（客户端 → GateServer）

### 2.1 客户端发起

`LoginDialog::on_login_btn_clicked` 校验输入后，发 HTTP POST 到 GateServer：

```cpp
QJsonObject json_obj;
json_obj["email"] = email;
json_obj["passwd"] = xorString(pwd);   // 密码做简单异或混淆再传输
HttpMgr::GetInstance()->PostHttpReq(
    QUrl(gate_url_prefix + "/user_login"),
    json_obj, ReqId::ID_LOGIN_USER, Modules::LOGINMOD);
```

### 2.2 GateServer 处理 /user_login

```cpp
// 1. 查 MySQL：用户名/邮箱 + 密码是否匹配（UserInfo 里会拿到 uid）
bool pwd_valid = MysqlMgr::GetInstance()->CheckPwd(email, pwd, userInfo);
if (!pwd_valid) {
    root["error"] = ErrorCodes::PasswdInvalid;   // 密码不对，直接返回
    ...
}

// 2. 用户身份验证通过后，问 StatusServer 要一台聊天服务器和 token
auto reply = StatusGrpcClient::GetInstance()->GetChatServer(userInfo.uid);
if (reply.error()) {
    root["error"] = ErrorCodes::RPCFailed;       // gRPC 调用失败
    ...
}

// 3. 组装回包：uid + token + 聊天服务器地址
root["error"] = 0;
root["email"] = email;
root["uid"]   = userInfo.uid;
root["token"] = reply.token();
root["host"]  = reply.host();
root["port"]  = reply.port();
```

**这里完成了"用户是否存在 + 密码是否正确"的校验**。

---

## 3. 第二阶段：领取聊天服务器和 token（StatusServer）

### 3.1 GetChatServer（负载均衡 + 签发 token）

```cpp
Status StatusServiceImpl::GetChatServer(ServerContext* context,
                                        const GetChatServerReq* request,
                                        GetChatServerRsp* reply) {
    const auto& server = getChatServer();        // 选一台负载最低的 ChatServer
    reply->set_host(server.host);
    reply->set_port(server.port);
    reply->set_error(ErrorCodes::Success);
    reply->set_token(generate_unique_string());  // UUID 作为 token
    insertToken(request->uid(), reply->token()); // 存 Redis：utoken_<uid> = token
    return Status::OK;
}
```

负载均衡策略（不同版本）：

- day14 版本：`_server_index` 轮询；
- 最终版：遍历 `_servers`（config.ini 里配置的 ChatServer1/2），返回 `con_count` 最小的那台；
- 还预留了基于 Redis `logincount` 哈希表统计各服务器在线人数的方案（代码里已注释）。

### 3.2 token 存到 Redis

```cpp
void StatusServiceImpl::insertToken(int uid, std::string token) {
    std::string uid_str = std::to_string(uid);
    std::string token_key = USERTOKENPREFIX + uid_str;   // "utoken_" + uid
    RedisMgr::GetInstance()->Set(token_key, token);
}
```

token 的意义：它是"**该 uid 刚刚通过密码验证**"的凭证。之后 ChatServer 只认 token，不认密码。

> 你项目（chat_app）的 StatusServer 用的是**内存 map**（`_tokens[uid] = token`、`_token_to_server[token] = 服务器名`），原理相同，只是把 Redis 换成了进程内存储，并且额外记录 token 与服务器的对应关系，方便后面的分布式踢人。

---

## 4. 第三阶段：TCP 长连接登录（客户端 → ChatServer）

### 4.1 客户端连接并发送登录消息

客户端拿到 host/port/token 后：

```cpp
// LoginDialog 收到 HTTP 登录回包
ServerInfo si;
si.Uid   = jsonObj["uid"].toInt();
si.Host  = jsonObj["host"].toString();
si.Port  = jsonObj["port"].toString();
si.Token = jsonObj["token"].toString();
emit sig_connect_tcp(si);                 // 1. 触发 TCP 连接

// TcpMgr 连接成功回调
QJsonObject jsonObj;
jsonObj["uid"] = _uid;
jsonObj["token"] = _token;
emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_CHAT_LOGIN, jsonData); // 2. 发送登录消息
```

### 4.2 ChatServer 的 LoginHandler

这是登录的核心，参考实现分五步：

**第 1 步：校验 token**

```cpp
std::string token_key = USERTOKENPREFIX + uid_str;        // utoken_<uid>
bool success = RedisMgr::GetInstance()->Get(token_key, token_value);
if (!success)          { rtvalue["error"] = UidInvalid;  return; }  // 没签发过 token
if (token_value != token) { rtvalue["error"] = TokenInvalid; return; } // token 不匹配
```

> 你项目当前版本是调 `StatusGrpcClient::Login(uid, token)` 让 StatusServer 校验，等价，只是把 Redis 读换成了 gRPC 调用。

**第 2 步：加载用户资料（先缓存后数据库）**

```cpp
std::string base_key = USER_BASE_INFO + uid_str;          // ubaseinfo_<uid>
bool b_base = GetBaseInfo(base_key, uid, user_info);      // Redis 有就用缓存
if (!b_base) { rtvalue["error"] = UidInvalid; return; }   // 没有则查 MySQL 并写回缓存

rtvalue["uid"] = uid;
rtvalue["name"] = user_info->name;
rtvalue["nick"] = user_info->nick;
rtvalue["email"] = user_info->email;
rtvalue["icon"] = user_info->icon;
... // pwd/desc/sex 等
```

`GetBaseInfo` 的缓存策略：**先查 Redis `ubaseinfo_<uid>`，查不到就用 `MysqlMgr::GetUser(uid)` 查库，再把结果写回 Redis**，下次登录直接命中缓存。

**第 3 步：加载好友申请列表和好友列表**

```cpp
GetFriendApplyInfo(uid, apply_list);   // 数据库查好友申请
GetFriendList(uid, friend_list);       // 数据库查好友列表
rtvalue["apply_list"] = ...;           // 一起打包进登录回包
rtvalue["friend_list"] = ...;
```

这一步是为了让客户端登录后立刻能渲染出好友和申请列表，不用再单独发请求。

**第 4 步：分布式踢人（防止同一账号多处登录）**

```cpp
// 加分布式锁（lock_<uid>），防止并发登录竞争
auto lock_key = LOCK_PREFIX + uid_str;
auto identifier = RedisMgr::GetInstance()->acquireLock(lock_key, LOCK_TIME_OUT, ACQUIRE_TIME_OUT);

// 查用户之前登录在哪台服务器
bool b_ip = RedisMgr::GetInstance()->Get(USERIPPREFIX + uid_str, uid_ip_value); // uip_<uid>
if (b_ip) {
    if (uid_ip_value == self_name) {
        // 同服：直接找旧 session 通知下线并清除
        auto old_session = UserMgr::GetInstance()->GetSession(uid);
        if (old_session) {
            old_session->NotifyOffline(uid);
            _p_server->ClearSession(old_session->GetSessionId());
        }
    } else {
        // 跨服：通过 gRPC 通知那台 ChatServer 踢人
        KickUserReq kick_req;
        kick_req.set_uid(uid);
        ChatGrpcClient::GetInstance()->NotifyKickUser(uid_ip_value, kick_req);
    }
}
```

**第 5 步：绑定在线状态并回包**

```cpp
session->SetUserId(uid);                                     // session 绑定 uid
RedisMgr::GetInstance()->Set(USERIPPREFIX + uid_str, server_name);        // uip_<uid> = 本服务器名
UserMgr::GetInstance()->SetUserSession(uid, session);        // 内存 uid -> session
RedisMgr::GetInstance()->Set(USER_SESSION_PREFIX + uid_str, session->GetSessionId()); // usession_<uid>
```

最后通过 `Defer` 把 `rtvalue` 以 `MSG_CHAT_LOGIN_RSP` 发回客户端。

---

## 5. 客户端处理登录回包

```cpp
_handlers.insert(ID_CHAT_LOGIN_RSP, [this](ReqId id, int len, QByteArray data){
    QJsonObject jsonObj = QJsonDocument::fromJson(data).object();
    int err = jsonObj["error"].toInt();
    if (err != ErrorCodes::SUCCESS) {
        emit sig_login_failed(err);        // 登录失败，提示用户
        return;
    }
    UserMgr::GetInstance()->SetUid(jsonObj["uid"].toInt());
    UserMgr::GetInstance()->SetName(jsonObj["name"].toString());
    UserMgr::GetInstance()->SetToken(jsonObj["token"].toString());
    emit sig_swich_chatdlg();              // 跳转到聊天主界面
});
```

至此登录全流程结束：客户端进入聊天界面，同时 ChatServer 端该 uid 的会话已就绪，可以收发消息。

---

## 6. 登录涉及的 Redis key 一览

| key | 内容 | 写入方 | 用途 |
| --- | --- | --- | --- |
| `utoken_<uid>` | 登录 token | StatusServer | ChatServer 登录时校验 token |
| `ubaseinfo_<uid>` | 用户资料 JSON（name/nick/icon...） | ChatServer | 登录时加载用户信息（缓存） |
| `uip_<uid>` | 用户所在 ChatServer 名 | ChatServer | 在线状态、跨服转发、踢人 |
| `usession_<uid>` | 用户当前 session_id | ChatServer | 判断是否同一会话、异常清理 |
| `lock_<uid>` | 分布式锁标识 | ChatServer | 登录/踢人时防止并发竞争 |
| `logincount` | 各服务器在线人数 | ChatServer（预留） | 负载均衡参考 |

---

## 7. 关键设计点总结

1. **两层验证**：HTTP 层验"用户名+密码"（MySQL），TCP 层验"token"（Redis/StatusServer）。token 不存在或对不上都拒绝，杜绝绕过 HTTP 网关直接连 TCP 的情况。
2. **token 生命周期**：token 由 StatusServer 在 `GetChatServer` 时签发并存储，ChatServer 登录校验通过后会话建立。踢人/下线时清理相关 Redis key。
3. **缓存优先，数据库兜底**：用户资料先查 Redis 再查 MySQL 并回填缓存，避免每次登录都查库。
4. **登录即初始化**：好友申请列表、好友列表随登录回包一次性下发，减少客户端交互次数。
5. **防重复登录**：通过分布式锁 + `uip_<uid>` 判断旧登录位置，同服直接踢，跨服走 gRPC 踢，保证一个账号同一时刻只在一个连接上。
6. **会话绑定**：`session->SetUserId(uid)` + `UserMgr` + `usession_<uid>`，让后续消息能"按 uid 找到会话"、按会话找到 uid。
