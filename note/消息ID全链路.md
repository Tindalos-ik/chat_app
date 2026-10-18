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
                                                   ├─ MySQL：账号、社交、私聊/群聊、消息与回执
                                                   └─ gRPC：ChatServer1 <-> ChatServer2
```

HTTP 负责注册、登录、验证码和重置密码等短请求。登录拿到 `uid/host/port/token` 后，客户端连接 StatusServer 分配的 ChatServer，后续搜索、好友和聊天业务都走 TCP。

## 2. ID 总表

| 消息常量 | 方向/载荷 | 处理情况 |
| --- | --- | --- |
| `ID_GET_VARIFY_CODE` | HTTP `/get_varifycode`，`{email}` | GateServer 调用 VarifyServer 发送验证码 |
| `ID_REG_USER` | HTTP `/user_register`，用户资料和验证码 | GateServer 校验后写入用户数据 |
| `ID_RESET_PWD` | HTTP `/user_resetpassword`，用户、邮箱、验证码、新密码 | GateServer 校验后更新密码 |
| `ID_LOGIN_USER` | 预留的 HTTP 登录消息 | 当前登录代码使用 `ID_CHAT_LOGIN`，未单独使用 |
| `ID_CHAT_LOGIN` | HTTP 登录回调索引；TCP `{uid,token}` 登录 ChatServer | 两层复用，语义由 `Modules`/传输层区分 |
| `ID_CHAT_LOGIN_RSP` | ChatServer 返回 `{error,uid,token,user,...}` | 客户端解析资料并进入聊天页 |
| `ID_SEARCH_USER_REQ` | TCP `{uid}`，可传 UID 或用户名 | ChatServer 按 UID 或用户名查询 |
| `ID_SEARCH_USER_RSP` | TCP 返回搜索到的用户资料 | 客户端展示查询结果 |
| `ID_ADD_FRIEND_REQ` | TCP `{uid,applyname,bakname,touid}` | ChatServer 写申请并通知目标用户 |
| `ID_ADD_FRIEND_RSP` | ChatServer 返回申请写入结果 | 服务端已回包，客户端尚未注册专用 handler |
| `ID_NOTIFY_ADD_FRIEND_REQ` | ChatServer 推送 `{applyuid,name,desc,nick,sex,icon}` 给被申请方 | 客户端加入新朋友列表 |
| `ID_AUTH_FRIEND_REQ` | 被申请方提交 `{fromuid,bakname,touid}` | ChatServer1 在同一事务内确认好友关系、创建/复用私聊并写入初始消息 |
| `ID_AUTH_FRIEND_RSP` | 认证请求处理结果，含 `textmsgs` | 客户端更新好友列表、绑定 `thread_id` 并把初始消息写入 SQLite |
| `ID_NOTIFY_AUTH_FRIEND_REQ` | 通知申请方认证结果，含同一份 `textmsgs` | 同服和跨服均透传初始消息；客户端以 `msg_id` 幂等缓存 |
| `ID_TEXT_CHAT_MSG_REQ` | TCP `{fromuid,touid,textArray}` 文本请求 | ChatServer1 先持久化后同服或跨服转发 |
| `ID_TEXT_CHAT_MSG_RSP` | 文本消息请求回包 | 成功回包带服务端 `message_id/thread_id`，发送方据此写 SQLite |
| `ID_NOTIFY_TEXT_CHAT_MSG_REQ` | 推送对方收到的文本消息 | 每条消息必须带完整服务端元数据；客户端实时写 SQLite，缺字段即丢弃 |
| `ID_NOTIFY_OFF_LINE_REQ` | 重复登录时通知旧客户端下线 | 服务端发送后关闭旧 socket；客户端解析通知或检测已登录连接断开后返回登录页 |
| `ID_HEART_BEAT_REQ` / `ID_HEARTBEAT_RSP` | 客户端定期 Ping、服务端立即 Pong | 客户端每 20 秒发送，60 秒无回包时弹出心跳超时提示并返回登录页；ChatServer 60 秒无有效收包清理会话 |
| `ID_LOAD_CHAT_THREAD_REQ` / `ID_LOAD_CHAT_THREAD_RSP` | 加载聊天会话列表 | 客户端登录后按游标发现新增会话并写 SQLite |
| `ID_CREATE_PRIVATE_CHAT_REQ` / `ID_CREATE_PRIVATE_CHAT_RSP` | 创建私聊会话 | 客户端好友资料页请求唯一私聊；回包写入本地 SQLite 会话缓存 |
| `ID_LOAD_CHAT_MSG_REQ` / `ID_LOAD_CHAT_MSG_RSP` | 加载会话历史消息 | 客户端按会话游标正序分页同步到 SQLite |
| `ID_UPDATE_USER_PROFILE_REQ` / `ID_UPDATE_USER_PROFILE_RSP` | TCP `{uid,nick,desc,icon}` 更新当前用户资料 | ChatServer 以登录会话 UID 鉴权，更新 MySQL 并失效 Redis 用户缓存 |
| `ID_IMAGE_CHAT_MSG_REQ` (`1033`) | TCP `{fromuid,touid,imageArray}` 图片请求 | ChatServer 先调用 ResourceServer gRPC 核验已完成图片，再写 `chat_message` |
| `ID_IMAGE_CHAT_MSG_RSP` (`1034`) | 图片发送确认 | 成功表示图片已核验且已持久化；`delivered:false` 时接收方走历史增量同步 |
| `ID_NOTIFY_IMAGE_CHAT_MSG_REQ` (`1035`) | 收到图片通知 | 服务端下发可信 `resource_id/name/mime_type/file_size/width/height` 与正式消息元数据 |
| `ID_MESSAGE_DISPLAY_ACK_REQ` (`1036`) | 接收端 `{thread_id,message_ids}` | 消息已实际绘制后确认；服务端幂等写入 `displayed_at`，不改已读状态 |
| `ID_NOTIFY_MESSAGE_DISPLAYED` (`1037`) | 服务端推送发送端 | 对方已显示指定消息，客户端将投递状态提升为“已显示” |
| `ID_MARK_THREAD_READ_REQ` (`1038`) | 接收端 `{thread_id,read_through_message_id}` | 当前会话已读游标；服务端只更新该用户实际接收的未读消息 |
| `ID_NOTIFY_THREAD_READ` (`1039`) | 服务端推送发送端 | 对方已读到指定游标，客户端将对应发送消息显示为“已读” |
| `ID_FILE_CHAT_MSG_REQ` (`1040`) | TCP `{fromuid,touid,fileArray}` 私聊文件请求 | ChatServer 以 session UID 鉴权，并调用 ResourceServer 核验已完成的普通文件 |
| `ID_FILE_CHAT_MSG_RSP` (`1041`) | 文件发送确认 | 成功表示可信元数据已持久化；实时推送失败时仍由 1030 历史补齐 |
| `ID_NOTIFY_FILE_CHAT_MSG_REQ` (`1042`) | 收到私聊文件通知 | 只传资源元数据；Qt 保存到 SQLite 并显示可点击卡片，不自动下载 |
| `ID_CREATE_GROUP_REQ/RSP` | `{request_id,operation_id,name}` | 事务创建群/群主，持久业务重放 |
| `ID_SEARCH_GROUP_REQ/RSP` | `{request_id,thread_id}` | 精确搜索有效群号，返回group对象 |
| `ID_JOIN_GROUP_REQ/RSP` | `{request_id,operation_id,thread_id,explanation?}` | joined或pending；现有邀请转明确同意，已有申请仍待审批 |
| `ID_LOAD_GROUP_LIST_REQ/RSP` | `{request_id,after_thread_id,page_size}` | 从0建立session UID快照，groups/next_thread_id/load_more，4KiB分页；整轮成功才客户端失效对账 |
| `ID_LOAD_GROUP_MEMBERS_REQ/RSP` | `{request_id,thread_id,after_uid,page_size}` | 当前有效成员，最新完整group/self proof及members[{uid,nick,role,membership_version}] |
| `ID_GROUP_TEXT_MSG_REQ/RSP` | `{thread_id,textArray:[{msgid,content}]}` | 当前成员单份消息事务，正式元数据及sender_nick，persisted与实时结果分离 |
| `ID_NOTIFY_GROUP_TEXT_MSG` | 群文本正式元数据+target_member_uid | 提交后按成员扇出，同服/跨服目标锁内授权并排队，不再落库 |
| `ID_KICK_GROUP_MEMBER_REQ/RSP` | `{request_id,operation_id,thread_id,target_member_uid,expected_membership_version}` | 群主踢非owner，管理员仅踢role0，版本指目标 |
| `ID_DISSOLVE_GROUP_REQ/RSP` | `{request_id,operation_id,thread_id}` | owner逻辑解散/成员失效版本，保留历史 |
| `ID_NOTIFY_GROUP_STATE` | `{thread_id,type:'group',active:false,reason,target_member_uid}` | reason kicked/dissolved，只失效本群，不触发账号下线 |
| `ID_INVITE_GROUP_REQ/RSP` | `{request_id,operation_id,thread_id,target_member_uid}` | 好友邀请准入契约 |
| `ID_DECIDE_GROUP_INVITE_REQ/RSP` | `{request_id,operation_id,thread_id,invitation_id,accept}` | 被邀请者明确同意/拒绝 |
| `ID_LOAD_GROUP_REQUESTS_REQ/RSP` | `{request_id,scope,after_id,page_size,thread_id?}` | inbox/applications/results/approvals；每个scope独立使用全局准入id分页 |
| `ID_DECIDE_GROUP_APPLICATION_REQ/RSP` | `{request_id,operation_id,thread_id,application_id,approve}` | 管理员/群主审批申请 |
| `ID_SET_GROUP_ADMIN_REQ/RSP` | `{request_id,operation_id,thread_id,target_member_uid,role,expected_membership_version}` | 群主管理角色0/1，版本指目标 |
| `ID_TRANSFER_GROUP_OWNER_REQ/RSP` | `{request_id,operation_id,thread_id,target_member_uid,expected_membership_version}` | 群主原子转让，版本指目标 |
| `ID_LEAVE_GROUP_REQ/RSP` | `{request_id,operation_id,thread_id,expected_membership_version}` | 普通/管理员自愿离群，版本指自己 |
| `ID_SET_GROUP_APPROVAL_REQ/RSP` | `{request_id,operation_id,thread_id,approval_required}` | 群主审批开关，不自动处理已有申请 |
| `ID_UPDATE_GROUP_ANNOUNCEMENT_REQ/RSP` | `{request_id,operation_id,thread_id,announcement,expected_announcement_version}` | 纯 UTF8 最大1024字节；完整输入仍受2048限制 |
| `ID_UPDATE_GROUP_AVATAR_REQ/RSP` | `{request_id,operation_id,thread_id,resource_id}` | 可信群头像核验/发布契约 |
| `ID_NOTIFY_GROUP_EVENT` | event_type inbox/result/metadata/members/invalidated | 非成员信箱/结果不携带有效成员证明 |
| `ID_VIDEO_TEST_REQ/RSP` | `{op,request_id,touid?}` 或 `{op,request_id,test_id,reason?}` | 单向固定文件测试控制；`start/ready/stop`，身份取登录 session，Redis 保存短期状态 |
| `ID_NOTIFY_VIDEO_TEST` | `{op,status,test_id,stream_id,app,media_url,fromuid,touid,...}` | 通知好友测试开始、发布就绪或停止；跨服使用专用 `NotifyVideoTest` RPC |
| `ID_VOICE_CALL_REQ/RSP` | `{op,request_id,touid?}` 或 `{op,request_id,call_id,reason?}` | 好友双向语音 `invite/accept/reject/ready/end`；身份取登录 session，被叫接听/拒绝与状态变更原子校验 |
| `ID_NOTIFY_VOICE_CALL` | `{op,status,call_id,invite_request_id,fromuid,touid,actoruid?,...}` | 所有状态保留最初邀请UUID，隔离同好友连续呼叫；接听后 `accepted` 携带 `app=voice_call/media_url/caller_stream/callee_stream`；跨服使用 `NotifyVoiceCall` |

视频测试发送端在 transport connected 且 outbound 视频 bytes 增长时发送 `ready`；接收端随后单次 play，ZLM 以 `maxStreamWaitMS` 等待流注册。`rpc_failed` 统一表示 Redis/MySQL 依赖或跨服 RPC 失败，客户端提示“业务依赖或跨服通知失败”。同服与跨服文件音视频、非好友、离线、素材缺失和清理结果见 [连通测试](WebRTC视频通信.md#连通测试)。

语音在服务端确认接听后才申请麦克风，两端分别 push；每端在发布连接成功且实际音频 bytes 增长后上报 `ready`，对端经服务端通知后单次 play。`request_id` 关联操作，`call_id` 关联整场通话，页面另用 `generation` 淘汰迟到结果。拒绝为 `rejected`，取消/挂断为 `ended`；失败状态与视频测试相同。控制信令的好友鉴权不代表已部署媒体短时授权，当前复用可信回环 ZLM 配置；完整生命周期与验证边界见 [好友语音通话](WebRTC视频通信.md#好友语音通话)。


群管理 mutation 均要求 operation_id（现有 create/join/kick/dissolve 同样适用）。准入响应 entries 使用 `{kind,id,thread_id,group_name,inviter_uid,applicant_uid,target_uid,status,explanation,created_at_ms,updated_at_ms}`，next_id 为全局准入游标。group包含最新version、审批/公告/头像独立版本与可信元数据，以及接收者专属member/role/membership_version/membership_proof/source；source为mutation/snapshot/event。成员项携带membership_version，成员回包包含完整最新group。

公告仅有效成员查看。group.member=false的精确搜索、pending准入、旧JOIN重放和本人失效事件将announcement置空、announcement_version/announcement_author_uid/announcement_updated_at_ms置0，announcement_visible=false；当前有效成员visible=true且字段完整。公开name/thread_id/owner_uid/active/approval_required仍可读；公告占位0不是“清空公告”或新公告版本。客户端不得把非成员group资料合并到缓存，失效事件只推进成员负向版本/active，不用这些占位字段覆盖缓存公告。广播可见性按实际接收者，而不是操作人的权限。

`inbox`返回本人收到的全部邀请（pending及终态）；`applications`返回本人的全部申请（pending及终态）；`results`返回本人是被邀请者/申请者/邀请者的终态记录；`approvals`仅返回该群待审申请并锁内校验当前管理权限。每轮刷新各scope都从after_id=0开始，不能把其他scope的游标混用，也不能只按新id刷新旧记录状态。通知紧凑JSON显式带接收者`uid`和`type="group"`；uid不是操作人。只有新入群者的members事件带membership_granted=true，非成员结果不能授予权限。

membership_proof不等于激活许可：普通metadata/members只能刷新已有有效身份，不能激活未知群或tombstone。仅实际新增/重入收件人得到event_type='members',source='event',membership_granted=true，其他成员无grant。self显式CREATE/JOIN在当前锁定证明member=true时确认grant，包括已经入群与相同键重放；旧JOIN被踢后重放没有grant。接受邀请仅实际新入群响应有grant，pending/离群重放/接受邀请重放无grant；审批者响应不能代替申请者grant。离线激活只能完整成功快照轮，旧版本不能覆盖新身份。

邀请先确认目标账号存在（否则SearchUserNoExist）和互为好友的两条friend边，再产生pending；一条单向残留关系不成立。终态邀请/申请的新operation_id响应error=0、already_processed=true，status_conflict表示请求决定与entry.status不一致（invalid总是冲突）；不得覆盖终态、重入或将相反审批显示为成功执行。

所有mutation错误响应也回传operation_id、request_id、真实session uid及可验证数值格式的thread_id；这些只关联请求，不授予身份证明。列表/成员/准入分页预算先包含最终UID/相关字段，以较大的load_more=false计整帧，失败页不推进快照游标。
管理/列表/成员响应原样回传字符串request_id；身份只用登录session UID。群不可用GroupUnavailable，非成员GroupNotMember，权限或版本冲突GroupPermissionDenied；群号uint64 thread_id。群历史复用ID_LOAD_CHAT_MSG_REQ/RSP，有效成员可读加入前历史，踢出/解散后拒绝。


ResourceServer 的独立协议使用其自身的上传分片、断点同步消息常量。完成回包包含
`resource_url`；客户端只有拿到该字段后，才向 ChatServer 发送 `ID_UPDATE_USER_PROFILE_REQ`。

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
5. A 同服时直接收到 `ID_NOTIFY_AUTH_FRIEND_REQ`；跨 ChatServer 时以 gRPC `NotifyAuthFriend(AuthFriendReq)`
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

TCP `ID_AUTH_FRIEND_RSP` / `ID_NOTIFY_AUTH_FRIEND_REQ` 将其映射为同名 `textmsgs` 数组。单项 JSON 使用
`sender_id`、`unique_id`、`msg_id`、`thread_id`、`msgcontent`；客户端仅接受这些正式字段，
不再解析历史字段名。

## 8. 聊天通信

文本消息目前仍按用户 UID 路由，并不依赖私聊会话。`ID_CREATE_PRIVATE_CHAT_REQ` 的服务端处理会创建或获取私聊：

```text
客户端好友资料页 -> ID_CREATE_PRIVATE_CHAT_REQ {uid, other_id}
                  -> ChatServer.CreatePrivateChat
                  -> MySQL chat_thread/private_chat
客户端 <- ID_CREATE_PRIVATE_CHAT_RSP {error, uid, other_id, thread_id}
```

服务端会将两个 UID 排序，并通过 `private_chat(user1_id, user2_id)` 的唯一索引确保同一对用户只对应一个 `thread_id`。并发请求命中已有记录时返回已有会话；竞争创建产生的临时 `chat_thread` 会在同一事务中删除。Qt 客户端发送 `ID_CREATE_PRIVATE_CHAT_REQ` 后会注册并处理 `ID_CREATE_PRIVATE_CHAT_RSP`：校验当前登录 uid、写入 `LocalChatStorageMgr`，再将正式 `thread_id` 绑定到对应 `ChatUserWid`。

ChatServer1 和 ChatServer2 使用相同的完整单聊链路：

```text
A -> ID_TEXT_CHAT_MSG_REQ {fromuid,touid,textArray:[{content,msgid}]}
A <- ID_TEXT_CHAT_MSG_RSP {error,textArray:[{msgid,content,message_id,thread_id,sender_id,recv_id,created_at_ms,status}]}
B <- ID_NOTIFY_TEXT_CHAT_MSG_REQ {error,fromuid,touid,textArray:[{msgid,content,message_id,thread_id,sender_id,recv_id,created_at_ms,status}]}
```

服务端用当前 TCP 会话保存的 UID 校验文本请求的 `fromuid`，避免客户端伪造发送者。ChatServer 先写入 MySQL，因而目标离线时 `ID_TEXT_CHAT_MSG_RSP` 返回 `delivered:false`，但不是发送失败；客户端下次登录以 `ID_LOAD_CHAT_THREAD_REQ/RSP`、`ID_LOAD_CHAT_MSG_REQ/RSP` 增量补齐。1018/1034 明确返回 `persisted` 与 `realtime_delivered`：前者表示事务已提交，后者仅表示通知已排入对端 TCP 发送队列。对端只能在气泡实际进入聊天窗口后发送 1036，服务端才写 `displayed_at` 并用 1037 通知发送端；进入会话时客户端再发送 1038，服务端更新 `status=1` 并用 1039 推送已读游标。客户端 SQLite 的 `delivery_state` 依次保存“已保存、已送达、已显示、已读”，本地 `unread_count` 在增量同步、实时通知和进入会话清零之间保持一致。跨 ChatServer 的显示/已读回执使用新的 `NotifyMessageDisplayed`、`NotifyThreadRead` RPC；发送端离线时状态仍由下次 1030 历史同步读取。两台服务和客户端必须同时部署这版协议，不支持旧通知。

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
| ChatServer1 | 登录、社交、私聊创建/消息/历史/回执；群创建/精确搜索/加入/分页/文本/群主踢人/逻辑解散；好友单向固定文件测试信令与短期状态，同服/跨服真实好友通知、发布及停止已实测 | 撤回、发送幂等重试 |
| ChatServer 间 gRPC | 社交、私聊文本/资源/回执；群已提交消息与独立群状态转发；视频测试 `NotifyVideoTest` 按目标 session 跨服投递且发布/停止流程已实测 | 生产级重试与传输身份边界 |
| Qt 客户端 | HTTP/TCP登录与社交、SQLite历史/增量同步、私聊文本资源；共享搜索框中的群搜索卡片/创建/加入、侧栏嵌入式群通知、独立thread身份/纯文本/昵称、数字未读、真实成员分页与群管理审批、失效只读和完整快照对账；好友资料页 test 单向固定文件发布/接收，同服及跨服音视频已实测 | 文本发送幂等重试（本次不改） |
| VarifyServer | Redis 验证码、邮件发送 | 邮件失败后的补偿和监控 |
