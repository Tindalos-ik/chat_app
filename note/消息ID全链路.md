# 消息 ID 全链路

本文按当前 `chat_app`、`GateServer`、`StatusServer`、`ChatServer1/2` 和 `VarifyServer` 的代码整理。消息 ID 只描述 TCP 长连接上的业务消息；HTTP 接口同样使用 `ReqId` 作为客户端回调索引，但不会把这个 ID 放进 HTTP body。

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

| ID | 名称 | 方向/载荷 | 当前状态 |
| ---: | --- | --- | --- |
| 1001 | `ID_GET_VARIFY_CODE` | HTTP `/get_varifycode`，`{email}` | 已实现 |
| 1002 | `ID_REG_USER` | HTTP `/user_register`，用户资料和验证码 | 已实现 |
| 1003 | `ID_RESET_PWD` | HTTP `/user_resetpassword`，用户、邮箱、验证码、新密码 | 已实现 |
| 1004 | `ID_LOGIN_USER` | 预留的 HTTP 登录 ID | 当前登录代码使用 1005，未单独使用 |
| 1005 | `ID_CHAT_LOGIN` | HTTP 登录回调索引；TCP `{uid,token}` 登录 ChatServer | 两层复用，语义由 `Modules`/传输层区分 |
| 1006 | `ID_CHAT_LOGIN_RSP` | ChatServer 返回 `{error,uid,token,user,...}` | 已实现 |
| 1007 | `ID_SEARCH_USER_REQ` | TCP `{uid}`，可传 UID 或用户名 | 已实现 |
| 1008 | `ID_SEARCH_USER_RSP` | TCP 返回搜索到的用户资料 | 已实现 |
| 1009 | `ID_ADD_FRIEND_REQ` | TCP `{uid,applyname,bakname,touid}` | 已实现 |
| 1010 | `ID_ADD_FRIEND_RSP` | ChatServer 返回申请写入结果 | 服务端已回包，客户端尚未注册专用 handler |
| 1011 | `ID_NOTIFY_ADD_FRIEND_REQ` | ChatServer 推送 `{applyuid,name,desc,nick,sex,icon}` 给被申请方 | 已实现 |
| 1013 | `ID_AUTH_FRIEND_REQ` | 被申请方提交 `{fromuid,bakname,touid}` | ChatServer1/2 已注册并建立双向好友关系 |
| 1014 | `ID_AUTH_FRIEND_RSP` | 认证请求处理结果 | 客户端已解析好友资料并更新好友列表 |
| 1015 | `ID_NOTIFY_AUTH_FRIEND_REQ` | 通知申请方同意/拒绝结果 | 同服和跨服通知均已接通 |
| 1017 | `ID_TEXT_CHAT_MSG_REQ` | TCP `{fromuid,touid,textArray}` 文本请求 | ChatServer1/2 已校验会话 UID 后同服或跨服转发 |
| 1018 | `ID_TEXT_CHAT_MSG_RSP` | 文本消息请求回包 | 客户端已按 `msgid` 输出发送成功/失败日志 |
| 1019 | `ID_NOTIFY_TEXT_CHAT_MSG_REQ` | 推送对方收到的文本消息 | 客户端已解析；当前打开对应聊天窗口时显示气泡 |
| 1021 | `ID_NOTIFY_OFF_LINE_REQ` | 重复登录时通知旧客户端下线 | 服务端发送后关闭旧 socket；客户端解析通知或检测已登录连接断开后返回登录页 |
| 1023/1024 | `ID_HEART_BEAT_REQ/RSP` | 心跳请求/回包 | ID 已定义，当前未完整实现 |
| 1025/1026 | `ID_LOAD_CHAT_THREAD_REQ/RSP` | 加载聊天会话列表 | ID 已定义，当前未完整实现 |
| 1027/1028 | `ID_CREATE_PRIVATE_CHAT_REQ/RSP` | 创建私聊会话 | ID 已定义，当前未完整实现 |
| 1029/1030 | `ID_LOAD_CHAT_MSG_REQ/RSP` | 加载会话历史消息 | ID 已定义，当前未完整实现 |

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

设计上的完整链路如下：

```text
1. B 在申请列表点击“同意”或“拒绝”
2. B -> ID_AUTH_FRIEND_REQ
   {"uid":B 的 uid,"touid":A 的 uid,"agree":true/false}
3. B 所在 ChatServer 更新 friend_apply 状态
4. B <- ID_AUTH_FRIEND_RSP
5. 跨 ChatServer 时，服务端之间通过 gRPC `NotifyAuthFriend` 转发认证结果
6. A <- ID_NOTIFY_AUTH_FRIEND_REQ
7. A 更新“等待对方同意”为“已添加”或“已拒绝”
```

## 8. 聊天通信

设计上的单聊链路为：

```text
A -> ID_CREATE_PRIVATE_CHAT_REQ {fromuid,touid}
A <- ID_CREATE_PRIVATE_CHAT_RSP {thread_id,...}

A -> ID_TEXT_CHAT_MSG_REQ {fromuid,touid,textArray:[{content,msgid}]}
A <- ID_TEXT_CHAT_MSG_RSP {error,fromuid,touid,textArray}
B <- ID_NOTIFY_TEXT_CHAT_MSG_REQ {error,fromuid,touid,textArray}
```

服务端用当前 TCP 会话保存的 UID 校验 `fromuid`，避免客户端伪造发送者。跨 ChatServer 时，发送方服务根据 Redis 的 `uip_<uid>` 找到目标服务，经 gRPC `NotifyTextChatMsg` 转发，再推送给目标用户。当前没有离线消息落库；目标会话不存在会在 `1018` 中返回错误。会话列表和历史消息分别使用 1025/1026、1029/1030 加载。

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
| ChatServer | TCP 登录、搜索、好友申请/认证、在线文本转发 | 离线消息、会话/历史消息 |
| ChatServer 间 gRPC | 好友申请、认证结果、在线文本转发 | 离线消息补偿和重试 |
| Qt 客户端 | HTTP 流程、TCP 登录/搜索/好友、在线文本收发 | 发送状态 UI、重试、离线消息展示 |
| VarifyServer | Redis 验证码、邮件发送 | 邮件失败后的补偿和监控 |
