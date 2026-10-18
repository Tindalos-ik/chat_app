// 本页负责一次已接受好友通话的麦克风发布及远端订阅；Qt拥有呼叫信令，页面拥有轨道、双PC和异步生命周期。
'use strict'; // 避免桥接回调意外写入隐式全局状态。
(() => { // 每个页面代次隔离所有媒体资源。
    const query = new URLSearchParams(location.search); // 解析Qt提供的呼叫身份与页面代次。
    const id = query.get('call_id'); // 所有桥接命令与结果必须属于本次呼叫。
    const generation = Number(query.get('generation')); // 同一呼叫的旧页面不能接收新页面命令。
    const remote = document.getElementById('remote'); // 仅播放对方音轨，麦克风永不接入播放器。
    const lifetime = new AbortController(); // cleanup取消共用协商的ICE、SDP等待和HTTP。
    const timers = new Set(); // 保存权限、阶段及统计计时器供终结统一取消。
    let bridge, commandHandler, config, microphone, push, play; // 本页拥有桥接引用、配置、麦克风和两个独立PC。
    let dead = false, playStarted = false, playSucceeded = false, readySent = false, playingSent = false, statsBusy = false; // 单次状态锁阻止重复订阅和重复阶段通知。
    let peerReady = false, outgoingBytes = 0, incomingBytes = 0, decodedSamples = 0; // 保留对方ready和上轮实际音频计数。
    let receivedGrowth = false, decodedGrowth = false, playbackStart = 0; // playing需要数据增长、解码增长及播放器进度共同成立。
    let pushProgress = 0, playProgress = 0; // SDP完成后记录最近媒体进展，识别真实断流。

    // event/detail为Qt阶段或真实错误；仅活跃会话可调用槽，异步结果由Qt处理。
    function report(event, detail = '') { // 页面不自行推导呼叫信令状态。
        if (!dead && bridge) bridge.mediaEvent(id, generation, event, String(detail)); // 代次与呼叫ID随每次事件传回Qt。
    }
    // 会话结束同步作废所有后续回调并释放资源；不等待尚未兑现的设备授权Promise。
    function cleanup() { // stop、pagehide与任何真实失败共用唯一清理入口。
        if (dead) return; // 清理可被多个终结来源安全调用。
        dead = true; // 必须先作废，再中止异步任务以免取消异常触发新工作。
        lifetime.abort(); // 中止两路共享协商及在途HTTP请求。
        for (const timer of timers) { clearTimeout(timer); clearInterval(timer); } // 清除权限期限、媒体期限和统计轮询。
        timers.clear(); // 不再持有已取消计时任务。
        if (bridge && commandHandler) bridge.command.disconnect(commandHandler); // 解除QWebChannel信号对本页闭包的引用。
        bridge = null; commandHandler = null; config = null; // 禁止迟到回调再次访问桥接和配置。
        for (const pc of [push, play]) { // 发布和订阅连接均属于本页。
            if (!pc) continue; // 允许授权或配置阶段尚未创建PC时清理。
            pc.ontrack = null; pc.onconnectionstatechange = null; // 关闭前解除所有PC属性回调。
            for (const receiver of pc.getReceivers()) { if (receiver.track) { receiver.track.onended = null; receiver.track.stop(); } } // 停止尚未绑定播放器的接收轨道。
            pc.close(); // 结束ICE、DTLS和媒体传输。
        }
        push = null; play = null; // 防止统计回调复用已关闭PC。
        if (microphone) microphone.getTracks().forEach(track => { track.onended = null; track.stop(); }); // 关闭设备采集并解除麦克风回调。
        microphone = null; // 释放本页保存的采集流。
        if (remote.srcObject) remote.srcObject.getTracks().forEach(track => { track.onended = null; track.stop(); }); // 释放所有合并后的远端音轨。
        remote.onended = null; remote.onerror = null; // 取消播放器事件回调。
        remote.pause(); remote.srcObject = null; remote.removeAttribute('src'); remote.load(); // 停止解码、移除音源并重置播放器。
        removeEventListener('pagehide', cleanup); // 页面终结后不保留退出监听。
    }
    // error为浏览器或协商异常；先报告原始名称和消息，再统一释放媒体。
    function fail(error) { // 避免将权限、设备、播放或SDP失败冒充媒体就绪。
        if (dead) return; // cleanup产生的取消不再报告新错误。
        report('error', error instanceof Error || error instanceof DOMException ? error.name + '：' + error.message : String(error)); // 保留NotAllowedError等关键权限原因。
        cleanup(); // 任何一向失败都结束本页双向媒体。
    }
    // callback在ms毫秒后执行；返回句柄且归本页生命周期所有。
    function later(callback, ms) { // 一次性期限不遗留到下一次通话。
        const timer = setTimeout(() => { timers.delete(timer); if (!dead) callback(); }, ms); // 触发前再次确认页面存活。
        timers.add(timer); // cleanup可以取消未触发的期限。
        return timer; // 调用方可提前取消已完成阶段的期限。
    }
    // sender指定发布或订阅；返回只承载音频的PC，连接失败立即终结双向媒体。
    function makePeer(sender) { // 发布与订阅不共享传输状态或SDP。
        const pc = new RTCPeerConnection({iceServers:[]}); // 复用现有ZLM直连ICE部署，不新增服务。
        pc.onconnectionstatechange = () => { // 捕获连接失败而非等待统计期限掩盖原因。
            if (!dead && ['failed', 'closed', 'disconnected'].includes(pc.connectionState)) fail(new Error((sender ? '发布' : '订阅') + 'WebRTC连接状态：' + pc.connectionState)); // 回报具体方向和浏览器状态。
        };
        return pc; // 调用方立即登记为push或play以供cleanup关闭。
    }
    // pc与sender选择协商方向，stream是唯一目标流；返回有效Answer已设置的Promise。
    function negotiate(pc, sender, stream) { // 两路严格复用视频已跑通的完整ICE帮助器。
        return ChatWebRTC.negotiate({pc, mediaUrl:config.media_url, app:config.app, stream, sender, kinds:['audio'], signal:lifetime.signal, stage:detail => report('stage', (sender ? '发布：' : '订阅：') + detail)}); // 语音Answer只要求Opus。
    }
    // event携带远端音轨；合并到唯一MediaStream，等待play兑现并将失败交给统一终结。
    async function bindRemote(event) { // ontrack不意味着已经播放，playing由统计单独确认。
        if (dead) { event.track.stop(); return; } // 迟到轨道不能重新绑定播放器。
        if (event.track.kind !== 'audio') { event.track.stop(); throw new Error('语音订阅收到非音频轨道'); } // 不接收意外的视频媒体。
        const stream = remote.srcObject || new MediaStream(); // 分批到达的轨道合并而不重置已有音源。
        if (!stream.getTracks().some(track => track.id === event.track.id)) stream.addTrack(event.track); // 不重复添加同一接收轨道。
        event.track.onended = () => fail(new Error('远端音频轨道已结束')); // 设备或媒体终止需真正结束通话。
        if (remote.srcObject) return; // 已启动的播放器不因后续轨道重启。
        remote.srcObject = stream; // 只绑定远端轨道，没有任何麦克风本地回放路径。
        remote.muted = false; remote.volume = 1; // 远端音频必须可听，不能用静音规避自动播放失败。
        await remote.play(); // 真正等待自动播放结果，拒绝直接进入fail。
        if (dead) return; // 晚到的播放Promise不得恢复会话。
        playbackStart = remote.currentTime; // 后续观察播放时钟增长，而非只看paused。
        playSucceeded = true; // 只表示play已兑现，尚不报告playing。
    }
    // 已接受后的configure调用；异步获取麦克风并发布自身流，返回协商结果，不影响对方ready触发订阅。
    async function publish() { // 只有这里调用getUserMedia，页面握手不会触发采集。
        report('stage', '正在请求麦克风权限并准备Opus发布…'); // 授权等待是实际阶段。
        const permissionTimer = later(() => fail(new Error('等待麦克风权限或设备启动超时')), 30000); // 不让未回应授权无限占用呼叫。
        const stream = await navigator.mediaDevices.getUserMedia({audio:{echoCancellation:true, noiseSuppression:true, autoGainControl:true}, video:false}); // 真实麦克风启用回声消除、降噪及自动增益，不请求摄像头。
        clearTimeout(permissionTimer); timers.delete(permissionTimer); // 权限完成后取消授权期限。
        if (dead) { stream.getTracks().forEach(track => track.stop()); return; } // 关窗后晚授权的设备必须立即停止。
        microphone = stream; // 所有采集轨道交给cleanup统一管理。
        const tracks = stream.getAudioTracks(); // 只从真实授权结果读取音轨。
        if (!tracks.length || tracks.some(track => !track.enabled || track.readyState !== 'live')) throw new Error('麦克风未产生有效音频轨道'); // 设备不可用不能报告就绪。
        push = makePeer(true); // 保存发布PC后再增加轨道以保证异常也可清理。
        const codecs = ChatWebRTC.codecs('audio', true); // 发布端必须具备实际Opus编码能力。
        for (const track of tracks) { // 将保存的真实麦克风轨道仅送入发布PC。
            track.onended = () => fail(new Error('麦克风设备音轨已结束')); // 拔出设备或停止授权时报告明确原因。
            push.addTransceiver(track, {direction:'sendonly', streams:[stream]}).setCodecPreferences(codecs); // 明确sendonly及Opus，避免本地或双向SDP回放。
        }
        await negotiate(push, true, Number(config.self_uid) === Number(config.fromuid) ? config.caller_stream : config.callee_stream); // caller发布caller_stream，callee发布callee_stream。
        if (dead) return; // SDP完成后页面可能已结束。
        pushProgress = performance.now(); // 从协商成功后开始计算媒体传输期限。
        later(() => { if (!readySent) fail(new Error('麦克风发布未连接或未发送实际音频')); }, 25000); // 连接成功本身不足以变为ready。
    }
    // 对方ready触发一次订阅；允许早于麦克风授权或自身发布完成，返回本次协商Promise。
    async function subscribe() { // Qt锁存的peer_ready和后续play走同一单次入口。
        if (dead || !config || !peerReady || playStarted) return; // 未接受或未ready不拉流，也不做重试。
        playStarted = true; // 在任何异步等待前锁定，避免重复play命令发起第二次HTTP。
        play = makePeer(false); // 接收使用独立PC，不受本端发布协商阻塞。
        play.ontrack = event => { bindRemote(event).catch(fail); }; // await play的真实异常统一上报并清理。
        play.addTransceiver('audio', {direction:'recvonly'}).setCodecPreferences(ChatWebRTC.codecs('audio', false)); // 仅接收对方Opus音频。
        await negotiate(play, false, Number(config.self_uid) === Number(config.fromuid) ? config.callee_stream : config.caller_stream); // 只订阅对方独立流，不订阅自身麦克风。
        if (dead) return; // 迟到Answer不启动后续媒体观察期限。
        playProgress = performance.now(); // 等流HTTP结束后再计算实际播放期限。
        later(() => { if (!playingSent) fail(new Error('远端音频未收到、解码或实际播放')); }, 25000); // 连接状态不能代替可听媒体证明。
    }
    // 每500ms串行采样两个PC；只用实际音频RTP增长和播放器进度推动ready/playing。
    async function sample() { // 不增加高频日志或额外统计传输。
        if (dead || statsBusy) return; // getStats未完成时不叠加下一轮。
        statsBusy = true; // 本轮两个PC的采样都归此互斥标志管理。
        try { // 统计异常与媒体异常采用同一个终结路径。
            if (push) { // 麦克风授权期间没有发布PC可采样。
                const stats = await push.getStats(); // 获取浏览器真实发送字节而非模拟计数。
                if (dead) return; // 关闭后不访问已清空push。
                let bytes = 0; // 聚合本次所有实际音频发送流。
                stats.forEach(stat => { if (stat.type === 'outbound-rtp' && !stat.isRemote && (stat.kind || stat.mediaType) === 'audio') bytes += stat.bytesSent || 0; }); // 排除视频及远端汇总数据。
                if (bytes > outgoingBytes) { // 累计音频字节确实比上轮增长。
                    pushProgress = performance.now(); // 记录最近一次真实发送进展。
                    if (!readySent && push.connectionState === 'connected') { readySent = true; report('ready'); } // ready必须同时满足connected和本轮音频字节增长。
                }
                outgoingBytes = bytes; // 下一轮继续检查增量而非只看非零值。
            }
            if (play) { // 对方ready后才存在接收PC。
                const stats = await play.getStats(); // 检查真实接收及解码采样。
                if (dead) return; // 清理后不再读取播放器状态。
                let bytes = 0, samples = 0; // 保存本轮音频接收字节与非隐藏的解码采样数。
                stats.forEach(stat => { // 只分析本端音频inbound-rtp。
                    if (stat.type !== 'inbound-rtp' || stat.isRemote || (stat.kind || stat.mediaType) !== 'audio') return; // 排除其他统计和回送报告。
                    bytes += stat.bytesReceived || 0; // 音频RTP必须实际进入浏览器。
                    samples += Math.max(0, (stat.totalSamplesReceived ?? stat.jitterBufferEmittedCount ?? 0) - (stat.concealedSamples || 0)); // 排除丢包隐藏样本，兼容Qt浏览器采样统计字段。
                });
                if (bytes > incomingBytes) { receivedGrowth = true; playProgress = performance.now(); } // 接收进展用于真实断流期限。
                if (samples > decodedSamples) decodedGrowth = true; // 仅连接或收到空包不能证明音频已解码。
                incomingBytes = bytes; decodedSamples = samples; // 保存下一轮对比基线。
                if (!playingSent && receivedGrowth && decodedGrowth && playSucceeded && !remote.paused && remote.readyState >= 2 && remote.currentTime > playbackStart + 0.05) { // 播放承诺、实际解码和播放时钟共同确认可听路径。
                    playingSent = true; report('playing'); // 每个页面最多发送一次实际播放阶段。
                }
            }
            const now = performance.now(); // 使用单调时钟检测持续媒体停滞。
            if (pushProgress && now - pushProgress > 25000) throw new Error('麦克风音频发送持续中断'); // 不能无限保留已失去媒体的通话。
            if (playProgress && now - playProgress > 25000) throw new Error('远端音频接收持续中断'); // 没有音频字节增长需报告真实失败。
        } catch (error) { fail(error); } // 将getStats或断流异常交给Qt并清理。
        finally { statsBusy = false; } // 即使已结束也释放本轮互斥状态。
    }
    // channel握手回调只建立桥接；configure由Qt在双方accepted后交付，握手本身不访问设备。
    new QWebChannel(qt.webChannelTransport, channel => { // 使用与视频测试相同的内置QWebChannel方式。
        if (dead) return; // 页面在通道初始化期间结束时不再持有桥接对象。
        bridge = channel.objects.voiceCall; // 专用对象与视频测试控制器隔离。
        commandHandler = command => { // 保存同一函数引用，便于cleanup断开信号。
            if (dead || command.call_id !== id || command.generation !== generation) return; // 所有动作包括stop均限定本页代次。
            if (command.action === 'stop') { cleanup(); return; } // Qt挂断立即停止双向媒体。
            if (command.action === 'play') { peerReady = true; subscribe().catch(fail); return; } // 锁存提前ready，尚未configure不会拉流。
            if (command.action !== 'configure' || config) return; // 每页仅接受一次已接受配置。
            config = command; // 保存Qt确认的双方身份、流名及媒体地址。
            peerReady = peerReady || Boolean(config.peer_ready); // 合并Qt此前收到的对方ready。
            remote.onerror = () => fail(new Error('远端音频播放器失败：' + (remote.error?.message || remote.error?.code || '未知媒体错误'))); // 播放器自身错误不得静默。
            remote.onended = () => fail(new Error('远端音频播放已结束')); // 远端媒体终止时统一结束会话。
            const timer = setInterval(sample, 500); timers.add(timer); // 一套低频统计同时观察两个方向。
            publish().catch(fail); // 双方只在accepted配置后采集并推送真实麦克风。
            subscribe().catch(fail); // 订阅不等待本端授权或发布完成。
        };
        bridge.command.connect(commandHandler); // 必须先订阅再握手，防止同步配置丢失。
        bridge.pageReady(id, generation); // Qt据此交付当前代次accepted配置与锁存ready。
    });
    addEventListener('pagehide', cleanup); // 窗口关闭与Qt stop共享完整清理语义。
})();
