# WebRTC视频通信

首先是webrtc的coturn服务搭建



## coturn服务

coturn 是实现了 STUN 和 TURN 两种协议的开源服务器软件



### STUN

STUN 是一种工具型协议，用于帮助位于 NAT 后面的设备，发现自己在公网上的 IP 地址和端口

在设备在局域网内的时候，它只知道自己的私有地址，比如`192.168.17.1`就是一个私有网段地址，STUN 客户端会向公网上的 STUN 服务器发送请求，服务器收到后，会把观察到的请求来源(即设备经过 NAT 转换后的公网地址)返回给你



### TURN

TURN 是 STUN 的扩展协议，用于无法建立直接连接时，通过服务器中继数据来保证通信

TURN 客户端会向 TURN 服务器申请一个中继地址，此后，所有发送对方的数据都先发给 TURN 服务器，再由服务器转发



coturn是一个开源的 TURN 和 STUN 服务器软件，部署在公网服务器上，可以用来提供上述的 STUN 和 TURN 服务

```
			WebRTC 通信
                 │
     ┌───────────┴───────────┐
     │                       │
  先尝试 STUN           如果失败 → TURN
     │                       │
  直接P2P通信         通过服务器转发
                 │
             coturn
     （实现STUN和TURN的程序）
```

部署详情后面再说



## 架构设计

![Qt WebEngine 与 ZLMediaKit 音视频架构](assets/webrtc-zlmediakit-architecture.png)

### 架构说明

| 组成 | 职责 |
| --- | --- |
| Windows Qt 客户端 | 使用 Qt WebEngine 采集、发布、订阅和播放音视频，通过 QWebChannel 与 Qt 界面交互 |
| Windows ChatServer | 管理通话邀请、接听、拒绝、挂断和跨服通知 |
| Linux / WSL ZLMediaKit | 提供 HTTPS SDP 协商，接收双方发布流并向对端转发媒体 |

每位客户端发布自己的音视频，同时订阅对方的流。媒体连接的对端是 ZLMediaKit。



## ZLMediaKit接入

以下补充 Qt WebEngine 客户端接入 ZLMediaKit 的技术约定。

> 本章的短时媒体凭证、GateServer 鉴权回调和公网 TURN 配置属于生产接入设计。下文“连通测试”和“好友语音通话”的当前媒体部署复用可信本机回环 ZLMediaKit；聊天信令具有登录与好友鉴权，媒体接口没有接入上述短时授权，不能直接作为公网媒体鉴权方案。




### 技术组合

| 层次 | 技术 | 职责 |
| --- | --- | --- |
| 桌面界面 | Qt 6.8 Widgets，MSVC 2022 64 位 kit | 聊天、来电通知、通话窗口和用户操作 |
| 媒体运行时 | Qt WebEngine 内置 Chromium | 提供浏览器 WebRTC API、设备采集、编解码、传输和播放 |
| C++ 与页面交互 | QWebChannel | 传递通话参数、控制命令、状态和错误 |
| 通话业务 | ChatServer1/2、Redis | 登录与私聊授权、共享通话状态、忙线和跨服通知 |
| SDP 协商 | 页面与 ZLMediaKit 的 HTTPS 请求 | 建立发布和订阅连接 |
| 媒体接入与转发 | ZLMediaKit | 接收每位参与者的发布流，向获授权的对端转发 |
| 媒体鉴权 | ZLMediaKit 回调、GateServer、聊天业务 | 核验短时发布与订阅权限，按通话生命周期回收媒体会话 |
| 公网网络适配 | ICE、按网络验证结果配置的 coturn | 为客户端到媒体节点选择可达路径 |

一对一通话由双方各自发布、订阅对方组成。Qt C++ 负责业务与窗口，页面持有媒体对象，ZLMediaKit 承担媒体服务。



### Qt 构建环境

通过 Qt Maintenance Tool 安装同版本的 MSVC 2022 64 位、Qt WebEngine 和 Qt WebChannel 组件。Qt Creator 选择对应 kit，并使用独立的 MSVC 构建目录。

kit 将编译器、Qt 库、架构和调试器组成一套匹配环境。Qt 提供的预编译 WebEngine 组件包含 Chromium 与 WebRTC 运行时，应用链接相应 Qt 模块。

桌面项目的依赖配置示例：

```cmake
find_package(Qt6 6.8 REQUIRED COMPONENTS
    Core Widgets Network Sql WebEngineWidgets WebChannel)

target_link_libraries(chat_app PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Network Qt6::Sql
    Qt6::WebEngineWidgets Qt6::WebChannel)
```

采用 Qt 6.8 的权限 API 时，页面通过 `QWebEnginePage::permissionRequested` 和 `QWebEnginePermission` 处理授权。

WebEngine 用来在 Qt 的页面中嵌入浏览器，可以调用 WebRTC API



### 客户端模块

| 模块 | 管理内容 |
| --- | --- |
| 通话控制器 | 当前 `call_id`、私聊对象、服务端状态版本、窗口和桥接生命周期 |
| 通话窗口 | 来电与建连提示、静音、摄像头开关、挂断，以及 QWebEngineView |
| 页面桥接 | 开始通话参数、设备命令、结束命令、媒体就绪和失败回报 |
| 页面媒体模块 | 本地 MediaStream、发布 PeerConnection、订阅 PeerConnection、播放器和统计 |

来电从聊天长连接到达 Qt，用户操作由 Qt 交给受信任页面。页面状态回报携带 `call_id` 与本地生命周期代次，Qt 据此关联当前通话。

`thread_id` 等 uint64 标识在 JSON、QWebChannel 和 JavaScript 中使用十进制字符串，保持完整精度。



### 发布与订阅

1. 页面取得 ChatServer 分配的流标识、操作权限、短时凭证和媒体服务地址。
2. `getUserMedia()` 取得本地轨道，推流 PeerConnection 添加轨道并创建 Offer。
3. 页面通过 ZLMediaKit 的 `/index/api/webrtc` 接口执行 `type=push`，设置成功响应中的 Answer，观察实际连接状态。
4. 拉流 PeerConnection 按需接收音频与视频，通过同一接口执行 `type=play`，设置 Answer 后由 `ontrack` 绑定远端播放器。
5. 对端发布流就绪与订阅建立分开处理，建连期限由通话控制约定统一管理。

Offer 提交时机、ICE 收集和会话清理遵循所部署 ZLMediaKit 版本的接入示例。协商回调按通话和连接代次串行应用；通话结束后到达的异步结果进入资源清理流程。

媒体就绪依据连接状态与实际收发、播放状态判断。页面通过 `getStats()` 记录候选对、码率、丢包、帧率等指标，业务端据双方就绪报告推进通话状态。



### 设备与播放约定

| 场景 | 行为 |
| --- | --- |
| 语音通话 | 请求音频设备，建立音频发布与订阅 |
| 视频通话 | 用户确认后请求麦克风和摄像头，提供本地预览与远端播放 |
| 本地预览 | 设置 `muted`，避免本地声音回放 |
| 远端音频 | 处理 `play()` 的结果，播放策略限制时提供用户点击入口 |
| 静音 | 切换音频轨道 `enabled`，将实际结果同步给界面 |
| 停止摄像头采集 | 调整发送轨道并停止原视频 track；恢复时重新采集并更新发送轨道 |
| 设备切换 | 先取得新轨道、成功替换，再释放旧轨道；失败时展示明确错误 |
| 设备拒绝或占用 | 展示权限、设备或采集失败原因，允许用户结束当前呼叫 |

初始媒体参数采用设备可接受的 `ideal` 约束。音频请求回声消除、降噪和自动增益，并在目标设备上测量效果。编解码器以 WebEngine 与 ZLMediaKit 的实际协商和双向收发结果为准。



### 身份与媒体安全



#### 通话控制

ChatServer 从当前登录连接取得身份，校验私聊关系并分配 `call_id`。共享状态记录参与者、忙线占用、状态版本和期限；原子迁移裁决同时互拨、取消与接听竞争、重复结束等事件。

状态分为邀请中、建连中、通话中、已结束。来电、接听、拒绝、取消、挂断及媒体就绪均作为通话控制语义，按项目协议分配流程接入客户端和两个 ChatServer。



#### 发布与订阅鉴权

业务端生成绑定用户、登录会话、通话、流标识、操作与有效期的短时凭证。ZLMediaKit 的发布和播放鉴权回调经 GateServer 进入聊天业务，依据当前通话校验授权。

管理凭据存放在后端，发布权限和订阅权限分别校验。结束通话和登录会话失效时，授权撤销与媒体会话关闭共同执行；短时有效期约束新建连接的授权窗口。

公网接入使用登录 HTTPS、受 TLS 保护的聊天连接、可信通话页面和 HTTPS SDP 接口。媒体传输使用 DTLS-SRTP，ZLMediaKit 作为媒体对端终止各条连接并转发媒体；服务器具备媒体访问能力，部署和隐私策略据此制定。



#### 页面权限

通话页面使用受信任的稳定 origin，QWebChannel 仅向该页面提供必要的通话接口。设备授权同时校验 origin、权限类型和当前用户动作；导航、新窗口与 CSP 按允许的页面和网络端点设置策略。

Windows 摄像头、麦克风隐私设置与页面授权共同影响设备可用性。安全上下文和设备访问行为在实际发布方式下验证。



### 生命周期与故障收敛

每次通话创建自己的媒体对象。关闭通话窗口按挂断处理，最小化保留通话；用户退出和重复登录互踢触发当前通话结束。

清理顺序为：更新本地生命周期代次、停止全部本地 track、关闭推拉流 PeerConnection、清空播放器 `srcObject` 和计时任务、释放页面。晚到的设备采集结果立即停止其轨道。

| 故障 | 收敛方式 |
| --- | --- |
| 聊天会话失效 | ChatServer 确认终态、释放忙线并撤销媒体授权；客户端停止采集 |
| 对端发布流延迟 | 保持建连提示，在通话建连期限内等待或进行有界订阅重试 |
| 媒体路径持续失败 | 页面上报失败，控制器按期限结束或执行一次明确的重建流程 |
| ZLMediaKit 重启 | 当前媒体连接失败，双方获得服务中断提示，结束后由用户重新发起 |
| 鉴权回调失败 | 拒绝对应发布或订阅，向客户端展示建连失败并释放资源 |
| 页面崩溃或窗口销毁 | Qt 触发结束流程，后端清理当前通话和媒体会话 |

日志记录关联标识、状态迁移、错误分类与聚合媒体指标，对账号凭证、媒体凭证、SDP 和网络地址采取脱敏及访问控制。



### 网络与容量

ZLMediaKit 的候选地址、WebRTC 监听端口及防火墙映射组成客户端的媒体入口。HTTPS 代理处理页面和 SDP 请求，媒体与 TURN 流量按对应协议进入服务节点。

STUN 提供 NAT 映射候选，TURN 提供中继候选，ICE 探测实际可用的候选对。配置 coturn 时使用短时凭证、限定中继端口与配额，凭证由后端签发。网络验收覆盖同网、异网和受限出口，并通过实际选路及双向收发证明结果。

本架构的双方媒体均由 ZLMediaKit 转发。按每端发送码率 `b`、并发通话数 `C` 估算，一对一通话的媒体节点入站和出站各约 `2 × C × b`；预算另计协议开销、TURN 节点流量、带宽余量及实际编解码处理成本。



## 连通测试

本节记录好友资料页单向固定文件测试的实现、部署和实测结果。



### 当前功能

入口是好友资料页的播放图标 **test**。点击者发送固定视频文件，好友端自动订阅。每个账号维护一个测试窗口，重复点击激活原窗口；关窗、文件播放结束或媒体失败时结束测试。

媒体路径：本地文件解码 → WebRTC → ZLMediaKit → 好友端播放。视频取自 `captureStream()`；音频通过同一媒体元素的 `MediaElementSource → MediaStreamDestination` 提取，destination 仅连接发送轨道。Qt 6.8.3 中 `muted=true` 或 `volume=0` 会将直接捕获的音轨样本清零，因此源元素保持 `muted=false`、`volume=1`。



### 信令语义与路由

控制消息复用聊天 TCP 长连接的 `[消息 ID + JSON 长度 + JSON]` 帧；页面直接向 ZLMediaKit POST SDP。ChatServer 从认证 session 取得 UID，核对 Redis 当前 session，并通过 MysqlMgr 只读检查双向好友关系。

| 操作 | 请求关键字段 | 行为及成功含义 |
| --- | --- | --- |
| `start` | `request_id`、`touid` | 分配随机 `test_id`、`stream_id`，占用双方 session 并通知接收端；`notified` 表示目标 TCP `TrySend` 入队成功。 |
| `ready` | `request_id`、`test_id` | 仅发布者在实际 WebRTC 已连接且 outbound 视频字节增加后发送；服务端通知接收端开始 `play`。 |
| `stop` | `request_id`、`test_id`、可选 `reason` | 撤销状态后通知对端；`status=stopped` 为状态结果，`delivery_status` 为通知投递结果。 |
| start/通知媒体字段 | `test_id`、`stream_id`、`app`、`media_url`、`fromuid`、`touid`、`status` | `app=video_test`，成功通知为 `status=notified`。 |

请求携带标准 UUID `request_id`，后续操作按 `test_id` 关联。失败状态包括 `offline`、`unreachable`、`rpc_failed`、`forbidden`、`invalid`、`media_unavailable`、`busy`；客户端将 `offline`/`unreachable` 显示为“对方不在线”。`rpc_failed` 统一表示 Redis/MySQL 依赖或跨服 gRPC 失败。

接收端收到 `start` 后等待，收到 `ready` 后发起一次 `play`；ZLM 以 `maxStreamWaitMS=15000` 等待流注册。Redis 状态与双方 session 占位固定存活 120 秒。停止时先撤销状态，再尽力通知对端；客户端用本地代次和 `test_id` 过滤迟到响应。



### 主要源码职责

| 文件 | 类/函数 | 职责 |
| --- | --- | --- |
| `chat_app desktop/friendinfopage.cpp` | `FriendInfoPage` 构造函数 | 建立 test 图标并调用当前 `TcpMgr` 控制器。 |
| `chat_app desktop/tcpmgr.cpp` | `TcpMgr::SendVideoTest`、`TcpMgr::initHandlers`、连接重置路径 | 发控制 TCP 帧、分发响应/通知，连接关闭时通知媒体控制器清理。 |
| `chat_app desktop/main.cpp` | `main` | 在 QApplication 创建前注册 `chatvideo` scheme。 |
| `chat_app desktop/videotestcontroller.cpp` | `VideoTestController::RegisterScheme`、`TestFiles::requestStarted`、`VideoTestController::Start`、`Handle`、`ConfigurePage`、`mediaEvent`、`Finish` | 注册并限制自定义 scheme、本地素材映射、单窗口信令状态和生命周期清理。 |
| `chat_app desktop/video_test/media.js` | `start`、`negotiate`、`bindRemote`、`sample`、`cleanup` | 固定文件捕获、SDP 发布/订阅、远端播放、实际媒体数据判定和 JS 资源清理。 |
| `chat_app desktop/video_test/webrtc.js` | `ChatWebRTC.codecs`、`ChatWebRTC.negotiate` | 视频与语音共用的能力选择、完整 ICE 收集、一次 HTTP SDP 交换与 Answer 校验；不持有页面轨道或 PC。 |
| `ChatServer1/src/VideoTest.cpp` | `LogicSystem::HandleVideoTest`、`Forward`、`DeliverLocal`、`VideoTestStop`、`DisconnectVideoTest`、`ChatGrpcClient::NotifyVideoTest`、`ChatServiceImpl::NotifyVideoTest` | TCP 信令、Redis 状态、同服 session 校验投递、跨服 gRPC 和断线清理。ChatServer2 使用同步后的共享实现。 |
| `ChatServer1/src/MysqlMgr.cpp` | `MysqlMgr::AuthorizeVideoTest` | 只读核验双向好友关系。 |
| `ChatServer1/src/RedisMgr.cpp` | `RedisMgr::VideoTestScript` | 执行视频状态 Lua 脚本。 |
| `proto/message.proto` | `VideoTestNotifyReq/Rsp`、`ChatService.NotifyVideoTest` | 跨服信令信封及专用 RPC。 |



### 核心实现

好友资料页 `FriendInfoPage` 按钮入口：

```cpp
    connect(testButton, &QPushButton::clicked, this, [this] {
        TcpMgr::GetInstance()->VideoTest()->Start(_uid);
    });
```

`Forward` 按 Redis 路由选择同服 `DeliverLocal` 或跨服 `NotifyVideoTest`。跨服连接池等待与 RPC 共用三秒 deadline；投递失败时撤销状态并发送 stop。`DeliverLocal` 校验目标当前 session：

```cpp
std::string DeliverLocal(int uid, const std::string& sid, const std::string& payload) {
    const auto target = UserMgr::GetInstance()->GetSession(uid);
    if (!target || target->GetSessionId() != sid) return "unreachable";
    std::string current;
    const auto lookup = Lookup(USER_SESSION_PREFIX + std::to_string(uid), current);
    if (lookup == "rpc_failed") return lookup;
    if (lookup != "ok" || current != sid) return "unreachable";
    return target->TrySend(payload, ID_NOTIFY_VIDEO_TEST) ? "notified" : "unreachable";
}
```

跨服分支：

```cpp
    if (server == ConfigMgr::Inst()["SelfChatServer"]["name"])
        return DeliverLocal(uid, sid, Pack(notice));
    message::VideoTestNotifyReq request;
    request.set_target_uid(uid); request.set_target_session(sid); request.set_payload_json(Pack(notice));
    return ChatGrpcClient::GetInstance()->NotifyVideoTest(server, request).status();
```

`main()` 在 QApplication 创建前注册 `chatvideo` scheme；`ConfigurePage()` 将配置素材映射到 `chatvideo://test/fixed.webm`，`TestFiles::requestStarted` 按固定路径提供页面、脚本与素材。

`media.js::start` 从文件元素提取视频与 WebAudio 音频轨道：

```js
            audioContext = new AudioContext();
            audioSource = audioContext.createMediaElementSource(source);
            audioDestination = audioContext.createMediaStreamDestination();
            audioSource.connect(audioDestination);
            await bounded(audioContext.resume(), 5000, '固定文件音频启动');
            if (dead) return;
            captured = source.captureStream();
            captured.getAudioTracks().forEach(track => { captured.removeTrack(track); track.stop(); });
            const audio = audioDestination.stream.getAudioTracks()[0];
            if (!audio) throw new Error('固定文件音频提取未产生轨道');
            captured.addTrack(audio);
            await bounded(source.play(), 8000, '本地文件播放');
            if (dead) return;
            const video = captured.getVideoTracks()[0];
```

`media.js::negotiate` 将当前 PC、流和生命周期交给 `video_test/webrtc.js::ChatWebRTC.negotiate`，保留原 `video_test` 应用、VP8/Opus、push/play 和实际数据判定：

```js
return ChatWebRTC.negotiate({
    pc, mediaUrl:config.media_url, app:'video_test', stream:config.stream_id,
    sender, kinds:['video', 'audio'], signal:negotiationLifetime.signal,
    stage:detail => report('stage', detail)
});
```

共用帮助器依次创建 Offer、设置本地描述、等待完整 ICE、向 `/index/api/webrtc` POST SDP，检查 Answer 的业务 code、SDP 格式和所需编码后设置远端描述。HTTP 期限为 20 秒；页面终结信号取消 ICE 等待和 fetch，不增加自动重试。

`sample` 每半秒检查连接、音视频字节和解码播放状态，分别报告 ready、playing。

`pc.ontrack = bindRemote` 绑定接收轨道，自动播放：

```js
    function bindRemote(event) {
        if (dead) return;
        const stream = remote.srcObject || new MediaStream();
        if (!stream.getTracks().some(track => track.id === event.track.id)) stream.addTrack(event.track);
        // 第二条音/视频轨道只加入同一stream，不重新赋srcObject中断首次play。
        if (!remote.srcObject) {
            remote.srcObject = stream;
            remote.muted = false; remote.volume = 1;
            remote.play().catch(error => fail(new Error('远端自动播放失败：' + error.message)));
        }
    }
```

文件结束、错误、超时、关窗、账号退出或 TCP 断开时，`Finish` 更新本地代次并释放页面；`cleanup` 取消请求、计时器和媒体资源：

```js
        for (const timer of timers) { clearTimeout(timer); clearInterval(timer); }
        timers.clear();
        negotiationLifetime.abort();
        if (frameCallback !== undefined && remote.cancelVideoFrameCallback)
            remote.cancelVideoFrameCallback(frameCallback);
        if (pc) { pc.ontrack = pc.onconnectionstatechange = null; pc.close(); pc = null; }
        if (captured) captured.getTracks().forEach(track => track.stop());
        if (remote.srcObject) remote.srcObject.getTracks().forEach(track => track.stop());
```

```js
        for (const video of [source, remote]) {
            video.pause(); video.srcObject = null; video.removeAttribute('src'); video.load();
        }
        if (audioSource) audioSource.disconnect();
        if (audioDestination) audioDestination.stream.getTracks().forEach(track => track.stop());
        if (audioContext) audioContext.close().catch(() => {});
```



### 固定素材和客户端配置

仓库素材为 `chat_app desktop/media/video-test.webm`：实际探测为 30.008 秒、2,396,494 字节，WebM 容器、VP8 640×360 / 24 fps 视频和 Opus 48 kHz 单声道测试音。CMake post-build 将 `media` 目录复制到可执行文件旁边；客户端按 exe 相对路径读取配置：

```ini
[VideoTest]
file=media/video-test.webm
```

`[VideoTest] file` 支持相对 exe 目录的路径或绝对路径，页面通过 `chatvideo://test/fixed.webm` 读取。



### 当前 ZLMediaKit 测试部署

测试配置为 `deploy/zlmediakit-video-test.ini`。运行环境：Windows Qt 6.8.3 MSVC 2022 64-bit + WSL Ubuntu 22.04 mirrored 网络；ZLMediaKit `git08329b3`，程序路径 `/home/tindalos/ZLMediaKit/release/linux/Release/MediaServer`，`api.version` 返回 `code=0`、hash `08329b3`。

测试实例使用显式配置启动，参数为：

| 项目 | 测试值/事实 |
| --- | --- |
| HTTP API | `127.0.0.1:18080`；HTTPS 关闭；只允许本机回环来源 |
| WebRTC 媒体 | UDP `127.0.0.1:18000`，TCP `127.0.0.1:18000`；候选地址 `127.0.0.1` |
| 编解码 | VP8 `640×360`、Opus `48 kHz / 2 声道`，实测 track loss 为 0 |
| 流等待 | `maxStreamWaitMS=15000` |
| 实际选路 | UDP 回环候选对，DTLS connected |
| 隔离测试服务 | ChatServer TCP `38090/38091`、gRPC `35055/35056`；MySQL `33316/33360`；Redis `36379` |

运行配置为 `D:/temp/chat-video-probe/zlm.ini`，管理 secret 保存在该后端配置中。ChatServer 的 `[VideoTest] enabled` 默认 `false`，测试时启用并设置双方可访问的 `media_url`。专用 ZLM 以前台方式运行，启动前确认 `18080/18000` 端口空闲。



### 最少运行步骤与验证结果

最少可复现实测步骤：

1. 在已用 Qt 6.8.3 MSVC 2022 64-bit kit 配置好的构建目录执行：

```powershell
cmake --build build/qt-msvc-release --config Release --target chat_app
```

确认 exe 旁有 `config.ini` 和 `media/video-test.webm`；Qt Creator 使用此 MSVC kit 启动两个客户端实例。

2. 在实际运行的 ChatServer 配置文件中（同服测试改目标实例，跨服测试改两端实例）设：

```ini
[VideoTest]
enabled = true
media_url = http://127.0.0.1:18080
```

3. WSL Ubuntu 22.04 中复制仓库配置，生成只供后端 API 使用的随机 secret：

```bash
mkdir -p /mnt/d/temp/chat-video-probe/zlm-logs
cp /mnt/d/myproject/chat_app/deploy/zlmediakit-video-test.ini /mnt/d/temp/chat-video-probe/zlm.ini
printf '\n[api]\nsecret=%s\n' "$(openssl rand -hex 32)" >> /mnt/d/temp/chat-video-probe/zlm.ini
```

在 PowerShell 启动该回环测试实例：

```powershell
wsl.exe --exec /home/tindalos/ZLMediaKit/release/linux/Release/MediaServer -c /mnt/d/temp/chat-video-probe/zlm.ini -t 2 -l 2 --log-dir /mnt/d/temp/chat-video-probe/zlm-logs
```

4. 启动所需 Redis/MySQL、StatusServer、ChatServer 与两个 Qt 客户端，测试账号须互为好友。验证同服时两端 TCP session 连到同一 ChatServer；验证跨服时分别连接 ChatServer1/2。发送端在好友资料页点击 **test**。
5. 检查接收端视频帧、播放时间和音频输出；文件结束或关窗后，检查 stop、Redis 占位与页面清理。




## 好友语音通话

语音通话可以分成两件事：先确定“谁同意和谁通话”，再让麦克风的声音到达对方。前者由聊天服务处理，后者由 WebRTC 和 ZLMediaKit（下文简称 ZLM）处理。理解这两个层次，也就容易理解为什么接听成功以后还要等待设备、连接和播放。

### 控制面和媒体面各管什么

控制面沿用登录后的聊天 TCP 连接，处理邀请、接听、拒绝、就绪和挂断。ChatServer 从登录 session 确认身份，检查好友关系、在线状态和忙线；Redis 保存双方占用及通话元数据，跨服通知通过 gRPC 转发。这里传递的是通话意图和关联信息。

媒体面负责采集、编码、传输、解码和播放。Qt WebEngine 内的页面持有设备流与 `RTCPeerConnection`，音频经过 ZLM 转发给另一端。每个客户端有两个 PC：`push` 发布本端媒体，`play` 订阅对端媒体；两个 PC 的 WebRTC 对端都是 ZLM。双方客户端之间不直接交换 Offer/Answer。

Qt 与页面的分工是：

- Qt 控制器管理登录身份、邀请窗口、业务状态和页面寿命。
- WebEngine 提供浏览器的设备、WebRTC 和播放器能力。
- QWebChannel 连接两者：Qt 下发 `configure`、`play`、`stop`，页面回报 `ready`、`playing` 或错误。

在 `chat_app desktop/voicecallcontroller.cpp` 的 `ConfigurePage`、`pageReady` 中，内置页面地址固定为 `chatvideo://test/voice.html`，查询参数绑定 `call_id` 和页面 `generation`。页面先连接命令信号，再调用 `pageReady`，Qt 才下发配置；这样页面初始化期间的配置不会丢失。

### 为什么接听确认后才申请麦克风

点击接听只是向服务端发送 `accept`。服务端核对被叫身份与邀请阶段，并把状态原子地改成 `accepted` 后，才向双方公开媒体参数。`ChatServer1/src/VoiceCall.cpp` 的 `LogicSystem::HandleVoiceCall`、`Notice` 和 `kVoiceScript` 定义了这个边界。

因此，设备采集从双方各自收到有效 `accepted` 后开始：用户等待来电、主叫等待接听时，都不需要占用麦克风；接听请求失败或邀请已失效，也不会先启动设备。`invited` 只表示邀请通知已进入目标连接的发送队列，`accepted` 表示业务接听成立，它们都还没有说明声音能够播放。

`call_type` 在 `invite` 时选择一次，取值为 `audio` 或 `video`，服务端保存后在活动消息中回传。后续 `accept/ready/end` 等操作只引用通话，不再提交类型，避免同一次邀请在两端变成不同的设备权限与媒体要求。`VoiceCallController`、`voiceCall` 和 `voice_call` 是两种通话共用的历史命名。

几个标识各有用途：`request_id` 关联单次操作，`call_id` 关联整场通话，`invite_request_id` 始终关联最初的邀请。`chat_app desktop/voicecallcontroller.cpp` 的 `Handle` 还核对登录代次和页面代次；同一好友立即重拨时，旧邀请的迟到消息便不能打开新页面或结束新通话。

邀请在 Redis 中有过期时间，接听后则移除 TTL，使长通话不会按邀请期限被截断。拒绝、挂断和精确 session 断线清理释放占用。若进程异常退出而旧 session 仍保留，占用可能持续到登录换代后由新邀请回收；这里没有额外的后台租约调度。

### 从麦克风到发布流

`getUserMedia()` 返回一个 Promise，兑现后得到 `MediaStream`。流是轨道的容器，真正连续产生声音的是其中的 `MediaStreamTrack`；音频轨道的 `kind` 为 `audio`，`readyState` 为 `live` 才表示设备轨道仍在工作。

`chat_app desktop/voice_call/media.js` 的 `publish` 按通话类型构造约束。仅看语音分支，采集参数可以写成：

```js
const stream = await navigator.mediaDevices.getUserMedia({
    audio: {echoCancellation: true, noiseSuppression: true, autoGainControl: true},
    video: false
});
```

回声消除、降噪和自动增益交给浏览器的音频处理能力；`video:false` 明确不申请摄像头。采集返回后还需确认音轨存在、处于 live 且 enabled。权限拒绝、设备不可用或轨道结束会终结通话，不用空轨道继续显示通话状态。

每端都发布自己的麦克风，也订阅对方的流。服务端在 `accepted` 中给出 `app:"voice_call"`、`media_url`、`caller_stream` 和 `callee_stream`：主叫发布 caller_stream、订阅 callee_stream，被叫反过来。这两个随机流名用于 ZLM 的媒体路由，与浏览器里的 `MediaStream.id` 分属不同层次。

发布 PC 用 `addTrack(track, stream)` 接入采集轨道；订阅 PC 用 `addTransceiver` 声明接收需求。语音订阅只声明音频，下面是 `chat_app desktop/voice_call/media.js` 的 `subscribe` 中语音分支对应的核心操作：

```js
play.addTransceiver('audio', {direction: 'recvonly'})
    .setCodecPreferences(ChatWebRTC.codecs('audio', false));
```

`recvonly` 表示这个收发器只接收；`setCodecPreferences` 使用浏览器接收能力中的 Opus 配置。发送端则查询发送能力，两种能力分别由 `chat_app desktop/video_test/webrtc.js` 的 `ChatWebRTC.codecs` 获取。

### Offer、ICE 和 ZLM Answer 的顺序

两个 PC 都通过 `chat_app desktop/video_test/webrtc.js` 的 `ChatWebRTC.negotiate` 完成协商。SDP 描述媒体类型、编码、方向和传输参数；ICE 候选描述可能使用的网络地址。创建 Offer 后，设置本地描述才会推动候选收集：

```js
const offer = await bounded(() => pc.createOffer(), 8000, '生成SDP', signal);
await bounded(() => pc.setLocalDescription(offer), 8000, '设置本地SDP', signal);
```

这里的 `bounded` 是同文件的等待辅助函数，把期限和生命周期取消信号接到异步操作上。随后帮助器监听 `icegatheringstatechange`，等 `pc.iceGatheringState === 'complete'`，再读取 `pc.localDescription.sdp`。候选会补入本地描述，所以提交使用这个最终描述。当前采用一次完整 SDP 交换的非 trickle 方式，没有另建逐条发送候选的信令通道。

候选收集结束后，向 ZLM 发一次 POST：

```js
const url = new URL('/index/api/webrtc', mediaUrl);
url.searchParams.set('app', app);
url.searchParams.set('stream', stream);
url.searchParams.set('type', sender ? 'push' : 'play');
const response = await fetch(url, {
    method: 'POST', headers: {'Content-Type': 'text/plain'},
    body: pc.localDescription.sdp, signal: request.signal,
    credentials: 'omit', redirect: 'error'
});
```

`sender` 决定发布还是订阅，`stream` 决定这次操作对应哪一端的 ZLM 流；`request` 是帮助器为 HTTP 创建的 `AbortController`，与页面取消和请求期限相连。HTTP 成功后还需解析 JSON，检查业务 `code`、SDP 格式和所需编码，再把 ZLM 返回的 Answer 应用到当前 PC：

```js
await bounded(
    () => pc.setRemoteDescription({type: 'answer', sdp: answer.sdp}),
    8000, '设置ZLM answer', signal
);
```

Answer 应用后仍要等待连接与实际媒体传输。ICE 收集 complete 只说明候选收集结束；它不保证地址可达。当前 `makePeer` 使用 `iceServers:[]`，能否连通仍取决于浏览器与 ZLM 的实际网络可达性。

### ready 与 playing 分别说明什么

发布协商成功后，`chat_app desktop/voice_call/media.js` 的 `inspect`、`sample` 通过 `getStats()` 观察媒体。语音的 `ready` 要求发布 PC 为 connected，并且音频 `outbound-rtp.bytesSent` 有增长。Qt 再把这个事实通过控制面报告服务端，服务端转发带 `actoruid` 的 ready 通知，对端才启动一次订阅。

这个顺序让订阅发生在对方已经发布媒体之后。对端 ready 可能早于页面握手，也可能早于本端设备授权：Qt 和页面会锁存它，`subscribe` 在任何异步等待前设置 `playStarted`，防止重复通知创建多个 PC 或重复请求。发布和订阅可以独立推进，所以本端等待麦克风时也可能先听到对方。

收到 `ontrack` 表示接收轨道已经交给页面，播放器 `play()` 兑现表示播放请求获准；还需观察音频是否真的流动。语音的 `playing` 同时要求：

- 音频 RTP 接收字节增长，并有扣除丢包隐藏样本后的音频样本计数增长。
- 远端元素的 `play()` 兑现、未暂停、`readyState >= 2`，播放时钟继续推进。

这些观察逐步覆盖收包、音频样本和播放器运行，但不能判断扬声器硬件音量或使用者是否听见。Qt 只有在本端 ready 与远端 playing 都成立时才停止建连等待并显示双向通话。持续观察也按发送、接收分别记录进展，累计字节曾经非零不能代表当前仍在传输。

共享页面用一个 `<video>` 元素播放远端流，语音时也可以只播放其中的音轨，本地预览隐藏。`chat_app desktop/voicecallcontroller.cpp` 的 `ConfigurePage` 对当前受信页面关闭 `PlaybackRequiresUserGesture`，让 ready 可以驱动播放；页面仍须处理 `play()` Promise 的拒绝。自动播放设置、设备授权和真正播放是三个独立环节。

### 挂断为什么要先作废异步结果

`getUserMedia()`、SDP、HTTP、`play()` 和 `getStats()` 都可能在挂断后才返回。若只关闭窗口而仍接受这些结果，迟到的采集可能继续占用设备，旧页面也可能上报新状态。

`chat_app desktop/voicecallcontroller.cpp` 的 `Finish` 先把通话设为非活动并递增 generation，再发 `stop`、销毁媒体页面。`chat_app desktop/voice_call/media.js` 的 `cleanup` 先设置 `dead`，再调用 `lifetime.abort()`，取消等待、监听和计时器。取消用于及时收敛等待，await 后的身份与存活检查用于丢弃迟到结果。

设备采集本身没有接入这里的 AbortSignal，所以 `publish` 还保留了这个处理：

```js
if (dead) {
    stream.getTracks().forEach(track => track.stop());
    return;
}
```

页面清理同时停止设备与接收轨道、关闭 push/play 两个 PC、暂停并解绑播放器、断开桥接。关闭 PC 回收传输，`track.stop()` 结束轨道，清空 `srcObject` 移除播放器对流的引用，三者负责不同资源。未接听的来电关闭走 reject，接听后或主叫取消走 end；设备、网络或播放失败同样结束整场通话，不自动重试。

## 视频通话

视频沿用上一节的控制面、双 PC 和 ZLM 协商流程，`call_type:"video"` 把每个方向的必要媒体扩展为音频与视频。理解视频时，重点是同一端的音画如何关联，以及如何分别确认两类媒体都在工作。

### 同一次采集中的音轨与视频轨

`chat_app desktop/voice_call/media.js` 的 `publish` 使用真实麦克风和摄像头，采集约束如下：

```js
const stream = await navigator.mediaDevices.getUserMedia({
    audio: {echoCancellation: true, noiseSuppression: true, autoGainControl: true},
    video: config.call_type === 'video'
        ? {width: {ideal: 640}, height: {ideal: 480}, frameRate: {ideal: 24}}
        : false
});
```

视频通话得到的同一个 `MediaStream` 中包含音轨和视频轨。`ideal` 表达期望，浏览器可以根据设备能力选择接近的尺寸和帧率，640×480、24 fps 并非强制结果；它也不提供缺摄像头时的替代轨道。页面仍会检查两类轨道都存在、live 且 enabled，任一必要设备失败就结束通话，不自动改为语音。

这里容易混淆三个对象：

- `MediaStreamTrack` 是单一媒体源的轨道，分别产生音频样本或视频帧。
- `MediaStream` 将相关轨道组成一组，供发送时标记关联、接收时绑定播放器。
- `RTCPeerConnection` 承担协商和网络传输；它可以承载多条轨道，同一发布 PC 能同时发送音画。

ZLM 的 caller_stream/callee_stream 则标识双方各自发布的媒体。它们与浏览器流对象的 id 无需相等；视频也仍然是每端发布一个 ZLM 流、订阅另一个流。

### addTrack 如何连接到收发器

`chat_app desktop/voice_call/media.js` 的 `publish` 将采集流中的每条轨道加入同一 push PC：

```js
const sender = push.addTrack(track, stream);
const transceiver = push.getTransceivers()
    .find(item => item.sender === sender);
transceiver.direction = 'sendonly';
transceiver.setCodecPreferences(ChatWebRTC.codecs(track.kind, true));
```

这段位于轨道循环中。`addTrack` 返回负责该轨道发送的 `RTCRtpSender`；第二个参数 `stream` 传达轨道的流关联，音轨与视频轨使用同一个流对象，接收侧便有机会从 `event.streams` 得到共同的流。关联信息随协商传递，轨道的数据仍由各自的 RTP 发送器发送。

`RTCRtpTransceiver` 把 sender、receiver 和媒体方向联系起来。代码找到 addTrack 对应的真实收发器后设为 `sendonly`，让发布连接专注于发送。编码偏好来自浏览器实际发送能力：音频选择 Opus，视频选择 VP8。

订阅端没有本地轨道可添加，因此 `chat_app desktop/voice_call/media.js` 的 `subscribe` 直接声明两种接收需求：

```js
for (const kind of kinds())
    play.addTransceiver(kind, {direction: 'recvonly'})
        .setCodecPreferences(ChatWebRTC.codecs(kind, false));
```

视频时 `kinds()` 返回 `['audio', 'video']`。两个收发器属于同一个 play PC，共用这一连接的协商流程。共享 `ChatWebRTC.negotiate` 仍按“Offer → 本地描述 → 完整 ICE → push/play POST → ZLM Answer”推进，并核查 Answer 包含 Opus 与 VP8；只有一种必要编码不能满足视频通话的要求。

### ontrack 为什么要聚合到同一个远端流

`ontrack` 按轨道触发，音轨和视频轨可以分批到达；事件也可能没有关联的 `streams`。如果每来一条轨道就新建流并覆盖播放器，后来的画面可能替换先前的声音。

`chat_app desktop/voice_call/media.js` 的 `bindRemote` 优先使用浏览器提供的流，无流时只创建一个聚合对象：

```js
if (!remoteStream)
    remoteStream = event.streams[0] || new MediaStream();
for (const track of [...(event.streams[0]?.getTracks() || []), event.track]) {
    if (!remoteStream.getTracks().some(existing => existing.id === track.id))
        remoteStream.addTrack(track);
}
```

同时遍历关联流与当前事件轨道，可以补上后到或无 stream 的轨道；按 track id 去重，则不会把同一轨道重复加入。后续一直保留这个 `remoteStream` 对象，播放器能够使用它新增的轨道。

绑定播放器的核心部分是：

```js
if (remote.srcObject) return;
remote.srcObject = remoteStream;
remote.muted = false;
remote.volume = 1;
await remote.play();
```

`srcObject` 直接绑定流对象，无需生成文件 URL。只在首次绑定时调用 `play()`，避免每次 ontrack 都重置播放。完整函数还检查轨道类型和结束事件，并将播放拒绝送入统一失败处理。

### 两个 video 元素为什么只有一处声音

`chat_app desktop/voice_call/index.html` 使用两个播放器：

```html
<video id="remote" autoplay playsinline></video>
<video id="local" muted autoplay playsinline hidden></video>
```

远端元素持有对方的完整流，同时输出声音和画面，不再叠加一个 audio 元素播放同一音轨。视频配置到达后，本地预览解除隐藏并绑定采集流：

```js
local.hidden = false;
local.muted = true;
local.srcObject = stream;
await local.play();
```

`muted` 只控制本地元素的声音输出，不改变音轨的 `enabled`，所以预览不会把麦克风声音再放出来，发送给对方的声音仍然保留。若直接禁用音轨，则会影响发送内容。

页面对远端视频和本地预览都使用 `object-fit:contain`，在给定区域内保持画面比例，必要时留边。`playsinline` 表达内联播放意图，`autoplay` 表达自动播放意图；实际是否启动仍要由 `play()` 结果和播放进展判断。

摄像头权限也随通话类型扩大。`chat_app desktop/voicecallcontroller.cpp` 的 `ConfigurePage` 只给当前 accepted、call_id 与 generation 匹配的固定内置页面授权：audio 仅音频采集，video 可申请音频、视频或组合采集。浏览器页面授权通过以后，设备仍可能受系统权限或摄像头独占限制；同一摄像头能否被多个进程同时使用取决于设备和系统。

### 音画分别观察，才能理解连接状态

视频的 ready 在语音基础上增加视频发送要求。`chat_app desktop/voice_call/media.js` 的 `sample` 中：

```js
if (!readySent && push?.connectionState === 'connected'
    && kinds().every(kind => progress.push[kind]?.grew)) {
    readySent = true;
    report('ready');
}
```

`grew` 来自各媒体 outbound RTP 字节的增长。连接建立但只发出音频时，视频的条件仍不成立；ready 表达本端音画已经发送，不代表对方已经听到、看到。

远端 playing 则同时检查音频接收与样本增长、视频接收与 `framesDecoded` 增长、播放器时钟推进。视频还增加实际画面要求：

```js
const videoShown = config.call_type !== 'video'
    || (remote.videoWidth > 0 && remote.videoHeight > 0 && presentedFrames > 0);
```

`presentedFrames` 在 `requestVideoFrameCallback` 中递增，用于观察播放器提交呈现的视频帧；非零尺寸只说明已有有效画面尺寸，单看尺寸无法说明画面仍在呈现。当前页面要求此 API 可用，缺失时明确失败。上述观察同样不判断人的感知或画面内容。

`inspect` 为 push/play 的 audio/video 分别保存字节与解码进展，持续音频不能刷新视频的停滞期限。Qt 把本端 ready 与远端 playing 合并后才显示双向视频状态；只接收到对方音画时，本端设备仍可能在等待授权。

### 视频结束时要释放哪些对象

视频复用语音节的“先作废、再取消、最后释放”顺序，但每处都要覆盖音轨与视频轨。`chat_app desktop/voice_call/media.js` 的 `cleanup` 不只停止第一个轨道：

```js
if (captured)
    captured.getTracks().forEach(track => {
        track.onended = null;
        track.stop();
    });
```

本地流、聚合后的远端流，以及 PC receiver 中尚未绑定播放器的轨道都需要回收；push 和 play 都要关闭。播放器也要分别清空，本地预览隐藏或窗口关闭本身不能代替停采：

```js
for (const element of [remote, local]) {
    element.onended = null;
    element.onerror = null;
    element.pause();
    element.srcObject = null;
    element.removeAttribute('src');
    element.load();
}
```

暂停播放、解绑流并重置元素可以释放旧声音和画面的引用。清理还取消呈现帧回调、统计计时器、ICE 监听与协商请求，断开 QWebChannel；迟到的 getUserMedia 结果仍需停止其全部轨道。这样设备、传输、播放器和异步任务都落在同一个页面生命周期内。

> 媒体协商请求只使用通话给出的 app、流名和媒体地址；ZLM 管理密钥应留在后端，不放进页面。视频与语音共用业务状态与清理路径，设备权限和必要媒体条件则始终按当前 call_type 区分。
