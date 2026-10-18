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

好友资料页的语音入口发起一对一实时麦克风通话，原有 **test** 固定文件入口保留。主叫等待时可以取消；被叫明确接听或拒绝，关闭来电窗口按拒绝处理，关闭已接听的通话窗口按挂断处理。双方均收到服务端 `accepted` 后，才建立媒体页面和申请麦克风；邀请阶段不采集设备。

### 协议和端到端流程

语音控制复用已登录聊天 TCP 长连接、Redis 在线路由、同服 `TrySend` 与跨服专用 gRPC 通知。请求使用 `ID_VOICE_CALL_REQ`，响应使用 `ID_VOICE_CALL_RSP`，通知使用 `ID_NOTIFY_VOICE_CALL`，无需新增业务监听端口或数据库表。

| 操作 | 请求字段 | 服务端语义 |
| --- | --- | --- |
| `invite` | `request_id`、`touid` | 从登录 session 取得主叫身份，检查双向好友关系、目标在线 session 和忙线状态，分配 `call_id`；目标 TCP 通知入队成功才返回 `invited`。 |
| `accept` | `request_id`、`call_id` | 仅被叫可接听；原子核验参与者、session 和邀请状态，返回并通知 `accepted`。 |
| `reject` | `request_id`、`call_id` | 仅被叫可拒绝有效邀请，终结双方占用并通知 `rejected`。 |
| `ready` | `request_id`、`call_id` | 每端在自身发布连接成功且实际发送音频字节后报告；通知携带 `actoruid`，对端据此发起一次订阅。 |
| `end` | `request_id`、`call_id`、`reason` | 取消、挂断或媒体失败时终结状态，释放占用并通知 `ended`。 |

`request_id` 为 UUID，关联单次请求；`call_id` 标识整场通话。所有公开状态都携带 `invite_request_id`，固定对应最初 `invite` 的 UUID，后续操作不会改写。邀请通知包含 `fromuid`、`touid` 和 `call_id`；接听结果额外包含 `app=voice_call`、`media_url`、`caller_stream`、`callee_stream`。两端各自发布属于自己的随机流，并订阅对方流，客户端不生成或猜测服务端流标识。

失败状态为 `offline`、`unreachable`、`rpc_failed`、`forbidden`、`invalid`、`busy`、`media_unavailable`。`rpc_failed` 表达业务依赖或跨服通知失败；`invited` 只证明通知已排入目标 TCP 队列，不证明对方已经看见、接听或建立媒体连接。

Redis 邀请元数据和双方 UID 占位存活 120 秒；Qt 邀请界面等待 60 秒即结束当前呼叫。接听后移除 Redis TTL，避免把正常长通话按固定时长截断。`end`、拒绝和精确 session 断线清理撤销状态；新邀请创建时会回收双方登录已经换代的旧通话。进程异常退出且 Redis 仍保留原 session 时，旧通话会表现为忙线，相关账号重新登录换代后可在创建时回收；当前没有额外的后台租约调度。

活跃通知与成功响应入队前再次核对 Redis 阶段，本服投递锁保证终态通知入队后不再排入迟到 `accepted/ready`。跨服 RPC 与连接池等待共用三秒 deadline，网络转发和失败撤销前释放本服投递锁；投递不确定时先撤销并向双方尽力通知结束，不无限重试。

```text
主叫 invite → ChatServer 登录/好友/在线校验 → 被叫来电
被叫 accept → Redis 原子状态迁移 → 双方 accepted
双方申请麦克风 → 各自 push 到 ZLMediaKit
本端 connected + 实际音频 bytes → ready → 服务端通知对端
收到对端 ready → 单次 play 对端流 → 实际收包、解码和播放进展
取消/拒绝/挂断/失败 → 终结信令 + 两端媒体资源清理
```

### 页面契约和资源生命周期

内置页面地址为 `chatvideo://test/voice.html?call_id=...&generation=...`。Qt 通过 QWebChannel 暴露 `voiceCall`，页面先订阅 `command(QJsonObject)`，再调用 `pageReady(id, generation)`。`configure` 携带服务端接听字段和 `action`、`generation`、`self_uid`、`peer_ready`；后续命令为 `play`、`stop`，页面通过 `mediaEvent(id, generation, event, detail)` 回报 `ready`、`playing`、`stage`、`error`。

Qt 同时核对当前账号、登录代次、`call_id` 和页面 `generation`；尚未收到邀请响应的主叫，必须先用 `invite_request_id` 精确匹配本次邀请才能绑定提前 `accepted/ended/rejected` 或锁存 `ready`，不能只凭同一好友 UID 推断会话。已取消邀请的活动通知只补偿结束其原 `call_id`，终态不再发送结束，均不影响立即重拨的新通话。对端 `ready` 可以早于页面握手，控制器锁存播放意图，握手后仍只订阅一次；已终结通话不能重新打开采集页面。

每页拥有本地麦克风流、推流 PeerConnection、拉流 PeerConnection、远端 audio 元素、协商请求和计时器。仅受信任的内置语音页允许音频采集，摄像头权限拒绝。远端音频保持非静音，通过 `ontrack` 绑定 `srcObject` 后调用 `audio.play()`；`playing` 还要求实际接收和解码、播放进展，不能只凭 SDP 成功或 `ontrack` 宣告接通。

接收可以早于本端麦克风授权：此时界面仅显示已播放对端音频、等待本端发布，并保留建连期限。只有本端真实发布 `ready` 与对端实际播放 `playing` 两项证据都齐全，才显示双向通话并停止建连等待；任一方向失败仍结束整场通话。

结束时先作废会话代次，再停止本地与远端轨道、关闭双 PeerConnection、清空 `srcObject`、取消计时器和 fetch、断开桥接并释放页面。`getUserMedia()` 可能在窗口关闭后才返回，这类迟到流立即停止全部轨道。媒体错误和阶段超时结束当前通话，不进行无限重试。

### 源码职责

| 文件 | 类/函数 | 职责 |
| --- | --- | --- |
| `chat_app desktop/friendinfopage.cpp` | `FriendInfoPage` 构造函数 | 既有语音按钮调用全局控制器，保留独立 test 按钮。 |
| `chat_app desktop/tcpmgr.cpp` | `TcpMgr::SendVoiceCall`、`TcpMgr::initHandlers` | 发送 UUID 关联请求，分发语音响应/通知，连接重置触发媒体清理。 |
| `chat_app desktop/voicecallcontroller.h`、`chat_app desktop/voicecallcontroller.cpp` | `VoiceCallController::Start`、`Handle`、`ConfigurePage`、`pageReady`、`mediaEvent`、`Finish` | 邀请与接听窗口、状态乱序隔离、受限麦克风授权、桥接命令和页面生命周期；去重记录在登录重置时清除。 |
| `chat_app desktop/videotestcontroller.cpp` | `VideoTestController::CreateMediaPage`、`TestFiles::requestStarted`、`TestRequests::interceptRequest` | 视频与语音共用的固定页面工厂、嵌入资源映射和媒体源白名单。 |
| `chat_app desktop/voice_call/index.html`、`chat_app desktop/voice_call/media.js` | `publish`、`subscribe`、`bindRemote`、`sample`、`cleanup` | 音频页、真实麦克风轨道、双 PC、远端播放、媒体证据与完整清理。 |
| `chat_app desktop/video_test/webrtc.js` | `ChatWebRTC.codecs`、`ChatWebRTC.negotiate` | 无会话状态的共享编解码选择与一次 SDP 交换。 |
| `ChatServer1/src/VoiceCall.cpp` | `LogicSystem::HandleVoiceCall`、`DisconnectVoiceCall`、`State`、`Deliver`、`Forward`、`Abort`、`ChatGrpcClient::NotifyVoiceCall`、`ChatServiceImpl::NotifyVoiceCall` | 登录与好友授权、Redis 原子角色/状态判断、精确投递、跨服通知和终结清理；ChatServer2 保持共享实现同步。 |
| `ChatServer1/include/MediaSignal.h` | `MediaSignal::Pack`、`Parse`、`IsUuid`、`Lookup`、`DeliverCurrent` | 视频与语音共用的紧凑 JSON、UUID 校验、Redis 路由和 session 精确投递，不引入另一套业务状态机。 |
| `proto/message.proto` | `VoiceCallNotifyReq/Rsp`、`ChatService.NotifyVoiceCall` | 目标 UID、精确 session 与公开 JSON 的内网跨服信封。 |


### 媒体核心源码

`chat_app desktop/voice_call/media.js::publish` 在 `configure` 后申请真实设备，并把音轨交给只发送的 PC；结束期间晚到的设备结果先停止，不进入发布：

```js
const stream = await navigator.mediaDevices.getUserMedia({
    audio:{echoCancellation:true, noiseSuppression:true, autoGainControl:true},
    video:false
});
if (dead) { stream.getTracks().forEach(track => track.stop()); return; }
microphone = stream;
push = makePeer(true);
for (const track of stream.getAudioTracks()) {
    push.addTransceiver(track, {direction:'sendonly', streams:[stream]})
        .setCodecPreferences(ChatWebRTC.codecs('audio', true));
}
```

`subscribe` 独立创建 `play` PC，添加 `recvonly` 音频收发器，通过 `ChatWebRTC.negotiate` 订阅对方流。`self_uid === fromuid` 时发布 `caller_stream`、订阅 `callee_stream`，另一端反向选择。两路协商都使用 `app=voice_call`，并按完整 ICE、单次 POST SDP、Opus Answer 校验的相同时序工作；SDP 对端均为 ZLM。

`bindRemote` 只把远端音轨接到 audio 元素，设备流没有本地回放路径：

```js
remote.srcObject = stream;
remote.muted = false; remote.volume = 1;
await remote.play();
if (dead) return;
playbackStart = remote.currentTime;
playSucceeded = true;
```

`sample` 每半秒串行检查双 PC。发布 `ready` 要求 `connectionState === 'connected'` 且 `outbound-rtp.bytesSent` 增长；播放 `playing` 要求 `inbound-rtp.bytesReceived` 增长、非隐藏音频样本增长、`play()` 已兑现、播放器非暂停且播放时钟前进。非隐藏样本从 `totalSamplesReceived`（浏览器未提供时取 `jitterBufferEmittedCount`）扣除 `concealedSamples`；如果运行时不能提供真实解码证据，不会降级为只看 connected 的成功。

`cleanup` 首先设置 `dead`、取消生命周期信号并断开 `voiceCall.command`，随后关闭双 PC 和全部轨道、释放播放器、计时器及桥接引用。`video_test/webrtc.js::ChatWebRTC.negotiate` 只管理单次协商等待，不共享通话资源，因此旧文件测试仍拥有原来的单 PC、文件音视频轨道和播放判定。


### 运行步骤与安全边界

1. 使用现有 Windows Qt 6.8 MSVC kit、WebEngine 和 WebChannel 构建客户端与同版两个 ChatServer；保留原 TCP/gRPC 端口和数据库配置。
2. 按上节 ZLMediaKit 本机部署启动媒体服务；在实际运行的 ChatServer 配置中启用既有 `[VideoTest] enabled`，将 `media_url` 设为两端可达的同一媒体地址。语音复用这两项配置，固定文件的 `file` 仅供原 **test** 使用。
3. 启动 Redis、MySQL、登录相关服务与 ChatServer，登录两个互为好友的账号。Windows 隐私设置须允许桌面应用访问麦克风，并为两端选择可用的音频输入输出设备；同机双实例建议使用耳机避免回声。
4. 在好友资料页发起语音，确认主叫等待和被叫来电，再接听。分别讲话，观察双端发布与播放状态；同服与跨服分别检查实际 TCP 所连实例。
5. 检查取消、拒绝、来电关窗、接听后关窗、设备拒绝和聊天连接断开：通话应结束，采集指示与媒体连接停止。原 **test** 仍按前节固定文件流程使用。

本次隔离验证复用既有数据目录和 `chat_app_test_video_20261017` schema（只读确认 11 张表），专用 MySQL 使用经典协议 `33316`、X Protocol `33360`，专用 Redis 使用 `36379`；用户原有 `33060` MySQL 实例不作改动。既有 WSL 回环 ZLM 保留使用。真实 Qt driver 放在仓库外，通过 `TcpMgr` 登录、点击 `FriendInfoPage` 语音按钮并操作 `VoiceCallController` 窗口；媒体仍由实际 WebEngine 麦克风与远端 audio 元素处理。环境启动和设备枚举只证明前置条件，语音结果须由双向媒体与清理证据判定。

当前语音继承视频测试的可信回环媒体部署边界：ZLM 管理 secret 仅保存在服务端配置，客户端不携带管理密钥；聊天登录和好友校验不替代 ZLM 发布/订阅鉴权。当前实现没有签发短时媒体凭证，也没有接入 GateServer 媒体鉴权回调。回环候选地址只能用于相应本机/WSL 网络，异机或公网需要正确媒体候选、防火墙和受保护的媒体鉴权部署，现有回环结果不能外推为公网可用。


### 实际验证结果与证据范围

2026-10-18 本机验证使用 Windows Qt 6.8.3 MSVC 2022 64-bit、WSL mirrored 回环 ZLM 和上述隔离数据环境。ChatServer 测试实例为 TCP `38090/38091`、gRPC `35055/35056`，产品默认监听配置不变。证据根目录为 `D:/temp/chat-voice-probe/evidence`，首轮汇总为 `voice-run-summary.json`，补充运行日志与截图位于 `audio-observation/`；客户端时间字段采用 UTC，ZLM 日志采用本机时区。

| 项目 | 已观察结果 | 证据 |
| --- | --- | --- |
| 产品构建 | proto 生成及双 ChatServer MSVC 链接完成；产品 `chat_app` 和仓库外真实 Qt driver 均链接成功。 | `backend-build.log`、`backend-build-incremental.log`、`product-client-build.log`、`client-build.log` |
| 离线呼叫 | 服务端返回 `offline`，界面显示“对方不在线”；没有创建 WebEngine view，也没有麦克风权限请求。 | `voice-1900810001.log` 与对应状态截图 |
| 跨服取消与拒绝 | 实际点击取消/拒绝，双方获得 `ended/rejected`，主叫显示“对方已拒绝来电”；两端 view 和权限请求均为零。取消场景发生在已收到 `call_id` 后。 | 两个 UID 的运行日志及来电、终态 PNG |
| 跨服双向音频媒体 | `1bde383e-ced8-4c07-b6da-e02644f6081f`：双方接听后请求 `MediaAudioCapture`，分别报告真实 `ready`，界面达到双向媒体状态；两端远端 audio 为 `readyState=4`、`paused=false`，轨道 live/enabled 且 `track.muted=false`，播放时钟分别从 0 增至 10.295 / 10.991 秒。 | `voice-1900810001.log`、`voice-1900810002.log`、通话中 PNG |
| 同服双向音频媒体 | `aac97f2f-6514-46c5-931d-8d5aec92b44c`：双方连接 TCP `38090`，完成真实接听、双端 `ready` 与实际播放，播放时钟持续增长约 5 秒后挂断。 | 两个 UID 日志的同服运行记录及状态 PNG |
| 同服取消与拒绝 | 补充运行通过真实取消/拒绝按钮结束邀请，双方最终 WebEngine view 和麦克风权限请求均为零。 | `audio-observation/voice-1900810001.log`、`audio-observation/voice-1900810002.log` 与终态 PNG |
| 真实输入、元素音量与解码 PCM | 短同服通话 `9f0b70db-615f-45dc-85d5-219f4920b296`：两端实际采集 Intel 智音麦克风阵列，轨道 live/enabled，采样率 48000 Hz、单声道；远端 audio 元素 `muted=false`、`volume=1`。主叫 inbound bytes 从 3307 增至 14276、`totalSamplesReceived` 从 56640 增至 224640，`concealedSamples` 保持 8160；被叫对应样本为 bytes 3686→15014、samples 47040→215040、concealed 0。临时远端 PCM 观察的 AudioContext 为 running，双端出现约 `2.5e-4`、`4.2e-4` 的非零 RMS 与非零样本。 | `audio-observation/` 下两个 UID 日志中的 `microphoneTracks/settings`、`real_remote_element`、`rtpStats` 和 PCM 采样 |
| 页面状态提示 | 实际通话 PNG 显示页面中性提示“音频设备与连接状态请查看上方通话提示。”，与 Qt 上方真实阶段状态共同展示。 | `audio-observation/voice-*-state-*.png` |
| 原固定文件 test 冒烟 | 真实点击原好友页 test 按钮，固定文件发布进入 `ready`，接收窗口显示实际色条视频帧并进入音频播放状态；随后关闭接收窗口，双方收到停止结果并释放 view。 | `audio-observation/` 下两个 UID 日志中的 `video_test_signal`、`ui_action`，以及 `video-test-*-state-*.png`、`video-test-*-final-*.png` |
| 挂断清理 | 同服和跨服均通过实际挂断按钮触发 `ended`，两端最终 WebEngine view 为零；跨服 ZLM 记录两路 Opus 发布流注册，以及挂断后的 DTLS close 和媒体注销。 | 两端日志、`voice-zlm-media.log`、ZLM 原日志 |
| Redis 与 ZLM 媒体释放 | 专用 Redis `36379` 的 `SCAN MATCH voice:*` 返回零 key；本轮 ZLM 证据中 11 路注册流均有对应注销，缺失注销为零。 | `redis-voice-cleanup.json`、`zlm-cleanup-summary.json`、`voice-zlm-media.log` |

设备清单 `audio-endpoints.json` 记录 Intel 智音麦克风阵列、Realtek 耳机与扬声器；补充日志进一步记录实际 `getUserMedia()` 返回的物理输入轨道及设置。以上媒体运行使用真实 `getUserMedia()` 与 WebRTC 路径，没有 fake-media、虚拟设备开关或注入音源；双端 `ready` 与播放状态由产品媒体代码的实际数据判据推进，driver 同时读取真实远端元素时钟、元素音量、轨道和 RTP 统计。临时 Analyser 仅观察远端流的解码 PCM，没有连接音频 destination，也没有修改产品媒体逻辑。

这些结果证明本机同服/跨服的实时麦克风发布、接收播放链路与挂断清理。双实例运行于同一台机器并使用同一麦克风阵列，低幅非零 RMS 仅作为环境底噪经媒体链路到达远端解码 PCM 的观察证据；没有进行两位独立真人的人工听辨、语音内容核验或 WASAPI 物理输出录音，不能宣称“双方真人已互相听清”，也不能据此判断人声可懂度。
 
本轮没有实际覆盖尚未获得 `call_id` 时取消后立即重拨的乱序场景、原生 X 关窗、被叫主动挂断、单独同服离线、权限拒绝、设备故障、SDP 失败、聊天断线或跨服 RPC 失败；上述协议隔离和失败处理的实现说明不等于这些分支已实测。现有证据均来自本机可信回环部署，不代表异机、公网或 TURN 路径验收。

本次启动的专用 MySQL、Redis 和 ChatServer 已停止，测试 PID 与监听端口无残留；仓库外临时 driver、脚本、构建目录与二进制已清理，`D:/temp/chat-voice-probe/evidence` 保留运行证据。用户原有 MySQL 与既有 WSL ZLM 保持运行，清理记录见 `owned-services-cleanup.json` 和最终 `voice-run-summary.json`。

