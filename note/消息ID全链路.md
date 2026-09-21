# 消息 ID 全链路

本文按当前 `chat_app`、`GateServer`、`StatusServer`、`ChatServer1/2`、`ResourceServer` 和 `VarifyServer` 的代码整理。ChatServer 与 ResourceServer 是两条独立 TCP 连接，各自拥有消息 ID 空间；HTTP 接口同样使用 `ReqId` 作为客户端回调索引，但不会把这个 ID 放进 HTTP body。

> 判断一条链路是否真正完成，要同时检查客户端发送、服务端 callback 注册、服务端业务落库/转发、客户端回包 handler 四个环节。

## 1. 通信分层

```text
Qt 客户端
  ├─ HTTP + JSON ──> GateServer:8080
  │                    ├─ MySQL：用户、密码、验证码结果
  │                    ├─ gRPC ──> VarifyServer:50051（验证码）
  │                    └─ gRPC ──> StatusServer:50052（选择 ChatServer、签发 token）
  │
  └─ TCP 长连接 + [ID(2B) + 长度(2B) + JSON] ──> ChatServer:8090/8091
                                                   ├─ Redis：token、在线位置、用户缓存
                                                   ├─ MySQL：用户、好友申请、好友关系
                                                   └─ gRPC：ChatServer1 <-> ChatServer2
```

HTTP 负责注册、登录、验证码和重置密码等短请求。登录拿到 `uid/host/port/token` 后，客户端连接 StatusServer 分配的 ChatServer，后续搜索、好友和聊天业务都走 TCP。

## 2. ID 总表

| ID | 名称 | 方向/载荷 | 处理情况 |
| ---: | --- | --- | --- |
| 1001 | `ID_GET_VARIFY_CODE` | HTTP `/get_varifycode`，`{email}` | GateServer 调用 VarifyServer 发送验证码 |
| 1002 | `ID_REG_USER` | HTTP `/user_register`，用户资料和验证码 | GateServer 校验后写入用户数据 |
| 1003 | `ID_RESET_PWD` | HTTP `/user_resetpassword`，用户、邮箱、验证码、新密码 | GateServer 校验后更新密码 |
| 1004 | `ID_LOGIN_USER` | 预留的 HTTP 登录 ID | 当前登录代码使用 1005，未单独使用 |
| 1005 | `ID_CHAT_LOGIN` | HTTP 登录回调索引；TCP `{uid,token}` 登录 ChatServer | 两层复用，语义由 `Modules`/传输层区分 |
| 1006 | `ID_CHAT_LOGIN_RSP` | ChatServer 返回 `{error,uid,token,user,...}` | 客户端解析资料并进入聊天页 |
| 1007 | `ID_SEARCH_USER_REQ` | TCP `{uid}`，可传 UID 或用户名 | ChatServer 按 UID 或用户名查询 |
| 1008 | `ID_SEARCH_USER_RSP` | TCP 返回搜索到的用户资料 | 客户端展示查询结果 |
| 1009 | `ID_ADD_FRIEND_REQ` | TCP `{uid,applyname,bakname,touid}` | ChatServer 写申请并通知目标用户 |
| 1010 | `ID_ADD_FRIEND_RSP` | ChatServer 返回申请写入结果 | 服务端已回包，客户端尚未注册专用 handler |
| 1011 | `ID_NOTIFY_ADD_FRIEND_REQ` | ChatServer 推送 `{applyuid,name,desc,nick,sex,icon}` 给被申请方 | 客户端加入新朋友列表 |
| 1013 | `ID_AUTH_FRIEND_REQ` | 被申请方提交 `{fromuid,bakname,touid}` | ChatServer1 在同一事务内确认好友关系、创建/复用私聊并写入初始消息 |
| 1014 | `ID_AUTH_FRIEND_RSP` | 认证请求处理结果，含 `textmsgs` | 客户端更新好友列表、绑定 `thread_id` 并把初始消息写入 SQLite |
| 1015 | `ID_NOTIFY_AUTH_FRIEND_REQ` | 通知申请方认证结果，含同一份 `textmsgs` | 同服和跨服均透传初始消息；客户端以 `msg_id` 幂等缓存 |
| 1017 | `ID_TEXT_CHAT_MSG_REQ` | TCP `{fromuid,touid,textArray}` 文本请求 | ChatServer1/2 已校验会话 UID 后同服或跨服转发 |
| 1018 | `ID_TEXT_CHAT_MSG_RSP` | 文本消息请求回包 | 客户端已按 `msgid` 输出发送成功/失败日志 |
| 1019 | `ID_NOTIFY_TEXT_CHAT_MSG_REQ` | 推送对方收到的文本消息 | 客户端已解析；当前打开对应聊天窗口时显示气泡 |
| 1021 | `ID_NOTIFY_OFF_LINE_REQ` | 重复登录时通知旧客户端下线 | 服务端发送后关闭旧 socket；客户端解析通知或检测已登录连接断开后返回登录页 |
| 1023/1024 | `ID_HEART_BEAT_REQ/RSP` | 客户端定期 Ping、服务端立即 Pong | 客户端每 20 秒发送，60 秒无回包时弹出心跳超时提示并返回登录页；ChatServer 60 秒无有效收包清理会话 |
| 1025/1026 | `ID_LOAD_CHAT_THREAD_REQ/RSP` | 加载聊天会话列表 | ID 已定义，当前未完整实现 |
| 1027/1028 | `ID_CREATE_PRIVATE_CHAT_REQ/RSP` | 创建私聊会话 | 客户端好友资料页发送 1027；ChatServer 创建或获取唯一私聊；客户端处理 1028 并写入本地 SQLite 会话缓存 |
| 1029/1030 | `ID_LOAD_CHAT_MSG_REQ/RSP` | 加载会话历史消息 | ID 已定义，当前未完整实现 |
| 1031/1032 | `ID_UPDATE_USER_PROFILE_REQ/RSP` | TCP `{uid,nick,desc,icon}` 更新当前用户资料 | ChatServer 以登录会话 UID 鉴权，更新 MySQL 并失效 Redis 用户缓存 |

ResourceServer 的独立协议使用 1003/1004 上传分片、1005/1006 同步断点。
完成回包包含 `resource_url`；客户端只有拿到该字段后，才向 ChatServer 发送 1031。

## 3. 注册、验证码和重置密码

### 3.1 获取验证码

```text
客户端 RegisterDialog/ResetDialog
  -> HTTP POST /get_varifycode {email}
  -> GateServer
  -> gRPC VarifyService.GetVarifyCode
  -> VarifyServer
  -> Redis 保存验证码（带过期时间）并发送邮件
  <- GateServer 返回 {error,email}
```

客户端使用 `ID_GET_VARIFY_CODE` 区分注册模块和重置密码模块。验证码本身由 VarifyServer 生成，GateServer 只负责转发结果。

### 3.2 注册

```text
客户端 -> POST /user_register
       {user,email,passwd,confirm,varifycode}
       -> GateServer 校验参数、验证码、用户名/邮箱唯一性
       -> MySQL 写入 user
客户端 <- {error,email}
```

### 3.3 重置密码

```text
客户端 -> POST /user_resetpassword
       {user,email,passwd,varifycode}
       -> GateServer 校验用户、邮箱和验证码
       -> MySQL 更新密码
客户端 <- {error}
```

## 4. 登录和 ChatServer 会话

```text
1. 客户端 -> POST /user_login {user,passwd}
2. GateServer -> MySQL 校验用户名和密码
3. GateServer -> StatusServer.GetChatServer(uid)
4. StatusServer 选择负载较低的 ChatServer，生成 token，并写 Redis `utoken_<uid>`
5. GateServer <- {uid,host,port,token}
6. 客户端 TCP 连接 host:port
7. 客户端 -> ID_CHAT_LOGIN {uid,token}
8. ChatServer 校验 Redis token，读取用户资料，绑定 uid/session
9. 客户端 <- ID_CHAT_LOGIN_RSP {error,uid,token,user,nick,desc,sex,icon}
```

`ID_CHAT_LOGIN_RSP` 成功后客户端才切换到 `ChatDialog`。`ID_CHAT_LOGIN` 在当前代码中既作为 HTTP 登录回调索引，又作为 TCP 登录 ID；HTTP 请求由 `Modules::LOGINMOD` 区分，TCP 则通过包头 ID 区分。

## 5. 搜索用户

```text
客户端 -> ID_SEARCH_USER_REQ {"uid":"10001"}
       -> ChatServer.UserSearchHandler
          纯数字先按 uid 查，查不到再按用户名查
客户端 <- ID_SEARCH_USER_RSP {error,uid,user,nick,desc,sex,icon}
```

搜索成功后客户端显示用户资料，点击“添加好友”才进入下一条好友申请链路。

## 6. 好友申请

### 6.1 客户端 A 发起申请

```text
A -> ID_ADD_FRIEND_REQ
   {
     "uid": A 的 uid,
     "applyname": A 的展示名,
     "bakname": 备注名,
     "touid": B 的 uid
   }
```

ChatServer 收到后向 `friend_apply(from_uid,to_uid,applicant_remark)` 写入申请记录；
`bakname` 保存为 `applicant_remark`，表示 A 给 B 设置的好友备注。B 同意后，服务端把它
写入 A 的 `friend(self_id=A, friend_id=B).back`，并把 B 在认证页填写的备注写入反向记录。
随后向 A 返回：

```text
A <- ID_ADD_FRIEND_RSP {"error":0}
```

这个回包只表示“申请已被服务器接收/写入”，不表示 B 已同意。

### 6.2 通知 B 有新申请

如果 B 与 A 在同一个 ChatServer，服务器直接向 B 推送；如果跨 ChatServer，则先通过 gRPC `NotifyAddFriend(AddFriendReq)` 转发到 B 所在服务，再由目标服务推送 TCP 消息：

```text
B <- ID_NOTIFY_ADD_FRIEND_REQ
   {
     "applyuid": A 的 uid,
     "name": A 的用户名/昵称,
     "desc": 申请附言,
     "nick": A 的昵称,
     "sex": A 的性别,
     "icon": A 的头像
   }
```

B 的 `ChatDialog::slot_apply_friend` 将申请缓存到 `UserMgr`，点开“新的朋友”页面后渲染 `ApplyInfo`。同一 `applyuid` 会去重。

## 7. 好友认证和最终结果

当前同意好友后的完整链路如下：

```text
1. B 在申请列表点击“同意”或“拒绝”
2. B -> ID_AUTH_FRIEND_REQ
   {"fromuid":A 的 uid,"bakname":"B 给 A 的备注","touid":B 的 uid}
3. B 所在 ChatServer 在一个 MySQL 事务中更新 `friend_apply`、双向 `friend`、
   创建或复用 `private_chat`，并向 `chat_message` 写入一条“已成为好友”初始消息
4. B <- ID_AUTH_FRIEND_RSP，消息在 `textmsgs` 中返回
5. A 同服时直接收到 `1015`；跨 ChatServer 时以 gRPC `NotifyAuthFriend(AuthFriendReq)`
   转发，`AuthFriendReq.fromuid` 是 B，`touid` 是 A，`textmsgs` 原样透传
6. A <- ID_NOTIFY_AUTH_FRIEND_REQ，带同一条初始消息
7. 两端客户端先更新好友资料，再按 `thread_id` 建立/更新本地正式会话，并以 `msg_id`
   主键幂等写入 SQLite；重复通知不会重复增加未读数
```

跨服认证的 protobuf 附加消息为：

```proto
message AddFriendMsg {
  int32 sender_id = 1;
  string unique_id = 2;
  int32 msg_id = 3;
  int32 thread_id = 4;
  string msgcontent = 5;
}

message AuthFriendReq {
  int32 fromuid = 1;              // 认证方
  int32 touid = 2;                // 接收通知的原申请方
  repeated AddFriendMsg textmsgs = 3;
}
```

TCP `1014/1015` 将其映射为同名 `textmsgs` 数组。单项 JSON 使用
`sender_id`、`unique_id`、`msg_id`、`thread_id`、`msgcontent`；客户端仍可读取旧版本的
`chat_datas` / `sender` / `msg_content`，因此升级客户端后可兼容旧服务端的认证通知。

## 8. 聊天通信

文本消息目前仍按用户 UID 路由，并不依赖私聊会话。`1027` 的服务端处理会创建或获取私聊：

```text
客户端好友资料页 -> ID_CREATE_PRIVATE_CHAT_REQ {uid, other_id}
                  -> ChatServer.CreatePrivateChat
                  -> MySQL chat_thread/private_chat
客户端 <- ID_CREATE_PRIVATE_CHAT_RSP {error, uid, other_id, thread_id}
```

服务端会将两个 UID 排序，并通过 `private_chat(user1_id, user2_id)` 的唯一索引确保同一对用户只对应一个 `thread_id`。并发请求命中已有记录时返回已有会话；竞争创建产生的临时 `chat_thread` 会在同一事务中删除。Qt 客户端发送 1027 后会注册并处理 1028：校验当前登录 uid、写入 `LocalChatStorageMgr`，再将正式 `thread_id` 绑定到对应 `ChatUserWid`。

当前 ChatServer1 的完整单聊链路为：

```text
A -> ID_TEXT_CHAT_MSG_REQ {fromuid,touid,textArray:[{content,msgid}]}
A <- ID_TEXT_CHAT_MSG_RSP {error,fromuid,touid,thread_id,textArray:[{...,message_id,thread_id,...}]}
B <- ID_NOTIFY_TEXT_CHAT_MSG_REQ {error,fromuid,touid,thread_id,textArray:[{...,message_id,thread_id,...}]}
```

服务端用当前 TCP 会话保存的 UID 校验文本请求的 `fromuid`，避免客户端伪造发送者。ChatServer1 先写入 MySQL，因而目标离线时 `1018` 返回 `delivered:false`，但不是发送失败；客户端下次登录以 1025/1026、1029/1030 增量补齐。带服务器 ID 的同服 `1019` 由 SQLite 去重后立即更新本地历史、摘要和未读数。跨 ChatServer 时，发送方服务根据 Redis 的 `uip_<uid>` 找到目标服务，经 gRPC `NotifyTextChatMsg` 转发；当前 protobuf 尚只含 UUID 和正文，因此另一台服务需同步扩展字段后才能得到同样的实时持久化效果。

## 9. TCP 包格式

ChatServer TCP 包固定为：

```text
| message id: 2 bytes, BigEndian | body length: 2 bytes, BigEndian | JSON body: length bytes |
```

客户端 `TcpMgr::slot_send_data` 写入包头和 JSON；`readyRead` 将数据追加到缓冲区，按 ID 找到 handler 后解析 JSON。TCP 是字节流，服务端和客户端必须同时处理半包、粘包，并且长度必须使用 UTF-8 字节数而不是 QString 字符数。

## 10. 当前实现边界

| 模块 | 已接通 | 仍需补齐 |
| --- | --- | --- |
| GateServer | 验证码、注册、登录、重置密码 | 登录回包字段可继续补充用户资料 |
| StatusServer | ChatServer 负载选择、token 签发/校验 | 更完整的在线状态管理 |
| ChatServer1 | TCP 登录、搜索、好友申请/认证、私聊创建/查询、文本持久化、会话/历史消息加载 | 已读回执、撤回、发送幂等重试 |
| ChatServer 间 gRPC | 好友申请、认证结果、在线文本转发 | 文本的 `message_id/thread_id` 同步、离线补偿和重试 |
| Qt 客户端 | HTTP 流程、TCP 登录/搜索/好友、SQLite 历史与增量同步、在线文本收发 | 发送状态 UI、重试、上拉加载更早本地历史 |
| VarifyServer | Redis 验证码、邮件发送 | 邮件失败后的补偿和监控 |
