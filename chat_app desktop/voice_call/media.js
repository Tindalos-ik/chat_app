// 历史voice_call页面由语音与视频共用：Qt拥有邀请，页面拥有真实设备、双PC和全部异步资源。
'use strict'; // 禁止隐式全局状态跨越通话生命周期。
(() => { // 每个页面代次单独持有媒体，不建立第二套呼叫系统。
    const query = new URLSearchParams(location.search); // Qt在受信URL中绑定呼叫身份。
    const id = query.get('call_id'); // 回报与命令始终关联服务端呼叫。
    const generation = Number(query.get('generation')); // 淘汰同一呼叫的旧页面结果。
    const remote = document.getElementById('remote'); // 唯一远端播放器同时输出音频和视频，避免重复音频。
    const local = document.getElementById('local'); // 本地预览静音但不禁用任何发送轨道。
    const lifetime = new AbortController(); // 清理时中断ICE、SDP及HTTP等待。
    const timers = new Set(); // 权限与统计期限由页面统一取消。
    let bridge, handler, config, captured, push, play, remoteStream; // 两种通话共用设备流、发布PC和订阅PC。
    let dead = false, playStarted = false, playSucceeded = false, readySent = false, playingSent = false, statsBusy = false; // 防止重复异步启动及重复上报。
    let peerReady = false, playbackStart = 0, frameCallback, presentedFrames = 0; // 订阅可早于本地授权，帧呈现独立于音频时钟。
    const progress = {push:{}, play:{}}; // 各方向各媒体独立保存字节、解码和最后增长时刻，音频不能掩盖视频断流。
    function report(event, detail = '') { // 页面只回报媒体事实，不裁决业务状态。
        if (!dead && bridge) bridge.mediaEvent(id, generation, event, String(detail)); // 代次随每个异步结果回传。
    }
    function cleanup() { // 挂断、离页及失败共用幂等资源回收。
        if (dead) return; // 取消异常不能触发第二次清理。
        dead = true; // 先作废，再取消等待，避免微任务恢复采集。
        lifetime.abort(); // 中止共享协商帮助器所有等待。
        for (const timer of timers) { clearTimeout(timer); clearInterval(timer); } // 删除权限期限和统计轮询。
        timers.clear(); // 不保留已取消任务引用。
        if (frameCallback !== undefined && remote.cancelVideoFrameCallback) remote.cancelVideoFrameCallback(frameCallback); // 停止递归视频呈现观察。
        if (bridge && handler) bridge.command.disconnect(handler); // 解除桥接对闭包的引用。
        bridge = null; handler = null; config = null; // 迟到结果不能再访问通话配置。
        for (const pc of [push, play]) { // 两个PC都归当前页面所有。
            if (!pc) continue; // 授权阶段可能尚无连接。
            pc.ontrack = null; pc.onconnectionstatechange = null; // 关闭前移除属性监听。
            for (const receiver of pc.getReceivers()) { if (receiver.track) { receiver.track.onended = null; receiver.track.stop(); } } // 回收尚未绑定播放器的轨道。
            pc.close(); // 关闭ICE、DTLS及媒体传输。
        }
        push = null; play = null; // 阻止统计继续读取旧连接。
        if (captured) captured.getTracks().forEach(track => { track.onended = null; track.stop(); }); // 同时关闭麦克风和摄像头。
        captured = null; // 释放本地设备流。
        if (remoteStream) remoteStream.getTracks().forEach(track => { track.onended = null; track.stop(); }); // 回收按事件聚合的远端轨道。
        remoteStream = null; // 移除远端流引用。
        for (const element of [remote, local]) { // 本地预览和远端播放器同时重置。
            element.onended = null; element.onerror = null; // 清除播放错误回调。
            element.pause(); element.srcObject = null; element.removeAttribute('src'); element.load(); // 停止解码并清除媒体源。
        }
        removeEventListener('pagehide', cleanup); // 页面结束不保留退出监听。
    }
    function fail(error) { // 保留权限、设备和网络的真实失败原因，禁止降级或重试。
        if (dead) return; // cleanup导致的取消无需再次报告。
        report('error', error instanceof Error || error instanceof DOMException ? error.name + '：' + error.message : String(error)); // Qt展示原始错误分类。
        cleanup(); // 任一媒体方向失败均释放整场通话。
    }
    function later(callback, ms) { // 一次性期限同样受页面生命周期约束。
        const timer = setTimeout(() => { timers.delete(timer); if (!dead) callback(); }, ms); // 到期前再次检查通话是否存活。
        timers.add(timer); // 统一登记以便取消。
        return timer; // 阶段完成时可提前撤销期限。
    }
    function kinds() { // 无参数；configure之后返回本次权威类型的必需媒体数组，供采集、SDP与统计共同使用，不改变阶段。
        return config.call_type === 'video' ? ['audio', 'video'] : ['audio']; // 语音不请求或订阅摄像头。
    }
    function makePeer(sender) { // 发布和订阅使用独立传输但共享既有ZLM入口。
        const pc = new RTCPeerConnection({iceServers:[]}); // 保留现有直连候选部署，不引入端口或设施。
        pc.onconnectionstatechange = () => { // 实际连接失败立即收敛。
            if (!dead && ['failed', 'closed', 'disconnected'].includes(pc.connectionState)) fail(new Error((sender ? '发布' : '订阅') + 'WebRTC连接状态：' + pc.connectionState)); // 不把断开伪装成继续通话。
        };
        return pc; // 调用者立即登记以确保异常可清理。
    }
    function negotiate(pc, sender, stream) { // 两种通话复用完整ICE与一次性SDP交换。
        return ChatWebRTC.negotiate({pc, mediaUrl:config.media_url, app:config.app, stream, sender, kinds:kinds(), signal:lifetime.signal, stage:detail => report('stage', (sender ? '发布：' : '订阅：') + detail)}); // 视频Answer须同时接受Opus和VP8。
    }
    function watchFrames() { // 无参数且无返回值；视频订阅开始时递归登记呈现回调，异步累加真实显示帧，句柄归cleanup取消。
        if (dead) return; // 当前Qt 6.8运行时提供呈现帧API，结束后不再递归登记。
        frameCallback = remote.requestVideoFrameCallback(() => { if (dead) return; ++presentedFrames; watchFrames(); }); // 递归句柄由cleanup统一取消。
    }
    async function bindRemote(event) { // ontrack只绑定媒体，成功由统计与播放共同判断。
        if (dead) { event.track.stop(); return; } // 结束后的迟到轨道立即回收。
        if (!kinds().includes(event.track.kind)) { event.track.stop(); throw new Error('收到与通话类型不符的远端轨道'); } // 语音不会悄悄接收视频。
        if (!remoteStream) remoteStream = event.streams[0] || new MediaStream(); // 优先使用浏览器关联流，无stream时创建唯一聚合流。
        for (const track of [...(event.streams[0]?.getTracks() || []), event.track]) { // 兼容同流分批到达和后续无stream事件。
            if (!kinds().includes(track.kind)) { track.stop(); throw new Error('远端流包含未授权媒体类型'); } // 流内其他轨道也受类型限制。
            if (!remoteStream.getTracks().some(existing => existing.id === track.id)) remoteStream.addTrack(track); // 后到轨道合并而不覆盖已播放媒体。
            track.onended = () => fail(new Error('远端' + track.kind + '轨道已结束')); // 任一必要媒体结束均通知Qt。
        }
        if (remote.srcObject) return; // 不重复设置srcObject或调用play打断现有播放。
        remote.srcObject = remoteStream; // 单个播放器持有完整远端音画流。
        remote.muted = false; remote.volume = 1; // 不能通过静音远端绕过自动播放限制。
        await remote.play(); // 实际播放拒绝交由统一fail，不宣称接通。
        if (dead) return; // 迟到播放Promise不能恢复通话。
        playbackStart = remote.currentTime; // 后续必须观察播放时钟实际推进。
        playSucceeded = true; // play兑现只是必要条件，不等于媒体已解码。
    }
    async function publish() { // configure只在服务端accepted后到达，邀请期不采集。
        report('stage', config.call_type === 'video' ? '正在请求麦克风和摄像头…' : '正在请求麦克风…'); // 状态与实际设备请求一致。
        const timer = later(() => fail(new Error('等待设备权限或启动超时')), 30000); // 不无限等待用户授权。
        const stream = await navigator.mediaDevices.getUserMedia({audio:{echoCancellation:true, noiseSuppression:true, autoGainControl:true}, video:config.call_type === 'video' ? {width:{ideal:640}, height:{ideal:480}, frameRate:{ideal:24}} : false}); // 视频使用可接受ideal约束，保留原音频处理，不降级。
        clearTimeout(timer); timers.delete(timer); // 设备返回即解除授权期限。
        if (dead) { stream.getTracks().forEach(track => track.stop()); return; } // 挂断后晚授权的所有设备必须立即关闭。
        captured = stream; // 异常也能由cleanup停止全部轨道。
        for (const kind of kinds()) { // 每种必需媒体都必须有真实live轨道。
            const tracks = stream.getTracks().filter(track => track.kind === kind); // 不用另一种轨道冒充成功。
            if (!tracks.length || tracks.some(track => !track.enabled || track.readyState !== 'live')) throw new Error(kind + '设备未产生有效轨道'); // 缺少摄像头不能自动转语音。
        }
        if (config.call_type === 'video') { // 本地预览只存在于视频通话。
            local.hidden = false; local.muted = true; local.srcObject = stream; // 静音元素避免回声，不修改发送轨道enabled。
            await local.play(); // 本地预览失败同样呈现真实错误。
            if (dead) return; // 预览晚完成不能创建PC。
        }
        push = makePeer(true); // 在添加轨道前登记发布连接。
        for (const track of stream.getTracks()) { // 音画来自同一真实设备流并加入同一发布PC。
            track.onended = () => fail(new Error('本地' + track.kind + '设备轨道已结束')); // 设备撤销或拔出直接结束通话。
            const sender = push.addTrack(track, stream); // 明确携带相同stream使接收端可关联音画。
            const transceiver = push.getTransceivers().find(item => item.sender === sender); // 使用addTrack创建的收发器设置单向发送与编码。
            transceiver.direction = 'sendonly'; // 不让发布PC意外承担接收或本地回放。
            transceiver.setCodecPreferences(ChatWebRTC.codecs(track.kind, true)); // 音频Opus与视频VP8均使用真实发送能力。
        }
        await negotiate(push, true, Number(config.self_uid) === Number(config.fromuid) ? config.caller_stream : config.callee_stream); // 每端只发布自己的服务端流。
        if (dead) return; // 迟到Answer不创建后续观察。
        startProgress('push'); // 从SDP成功开始分别限定每种媒体的进展。
        later(() => { if (!readySent) fail(new Error('本端必要媒体未全部连接并实际发送')); }, 25000); // 单音轨或单视频轨不能报告ready。
    }
    async function subscribe() { // 对端ready只触发一次订阅，不等待本端授权。
        if (dead || !config || !peerReady || playStarted) return; // 尚无配置和重复ready都不创建PC。
        playStarted = true; // 在异步边界前锁定，禁止重复HTTP。
        play = makePeer(false); // 接收方向使用同一个PC承载所需音画。
        play.ontrack = event => { bindRemote(event).catch(fail); }; // 播放Promise异常必须收敛整个呼叫。
        for (const kind of kinds()) play.addTransceiver(kind, {direction:'recvonly'}).setCodecPreferences(ChatWebRTC.codecs(kind, false)); // audio仅音频，video同时音画，禁止隐式降级。
        if (config.call_type === 'video') { // 视频必须使用目标Qt支持的严格呈现证据，不设置备用成功路径。
            if (typeof remote.requestVideoFrameCallback !== 'function') throw new Error('当前Qt WebEngine缺少视频呈现帧观察能力'); // 能力缺失明确失败，不能降级为只看尺寸或解码。
            watchFrames(); // 在远端轨道到达前开始观察实际呈现帧。
        }
        await negotiate(play, false, Number(config.self_uid) === Number(config.fromuid) ? config.callee_stream : config.caller_stream); // 订阅对方而非本地设备流。
        if (dead) return; // 结束后的协商结果不能恢复计时器。
        startProgress('play'); // 音画独立检测连续断流。
        later(() => { if (!playingSent) fail(new Error('远端必要媒体未全部解码并实际播放')); }, 25000); // ontrack或connected均不足以成功。
    }
    function startProgress(direction) { // direction为push/play，无返回值；对应SDP确认后初始化各必需媒体期限，保留协商期间已有增长证据。
        for (const kind of kinds()) { // 为每种必需媒体建立独立时限。
            const item = progress[direction][kind] ||= {bytes:0, decoded:0, grew:false, decodedGrew:false}; // 统计可早于HTTP协商返回。
            item.at ||= performance.now(); // 未发送媒体也会在有限期限内失败。
        }
    }
    async function inspect(pc, direction) { // pc为本页push/play连接，direction为push/play；媒体观察阶段返回统计完成的Promise，异常拒绝交给sample统一终结。
        const stats = await pc.getStats(); // 读取真实RTP而非模拟计数。
        if (dead) return; // 清理后不访问配置或播放器。
        for (const kind of kinds()) { // 每种媒体分别统计，不能合并字节掩盖缺失。
            let bytes = 0, decoded = 0; // 聚合本轮同类RTP数据。
            stats.forEach(stat => { // 忽略远端汇总以及其他方向。
                if (stat.type !== (direction === 'push' ? 'outbound-rtp' : 'inbound-rtp') || stat.isRemote || (stat.kind || stat.mediaType) !== kind) return; // 只采纳本端真实RTP记录。
                bytes += (direction === 'push' ? stat.bytesSent : stat.bytesReceived) || 0; // 不以非RTP连接状态冒充传输。
                if (direction === 'play') decoded += kind === 'video' ? (stat.framesDecoded || 0) : Math.max(0, (stat.totalSamplesReceived ?? stat.jitterBufferEmittedCount ?? 0) - (stat.concealedSamples || 0)); // 视频须解码帧；音频排除丢包隐藏样本。
            });
            const item = progress[direction][kind] ||= {bytes:0, decoded:0, grew:false, decodedGrew:false}; // 每方向媒体保留独立基线。
            if (bytes > item.bytes) { item.grew = true; item.at = performance.now(); } // 只有本媒体字节增长才刷新本媒体期限。
            if (decoded > item.decoded) { item.decodedGrew = true; item.decodedAt = performance.now(); } // 实际解码增长与收包独立记录。
            item.bytes = bytes; item.decoded = decoded; // 下轮比较增量，不把累计非零永远算作进展。
            if (item.at && performance.now() - item.at > 25000) throw new Error((direction === 'push' ? '本端发送' : '远端接收') + kind + '持续中断'); // 视频停止时音频仍增长也不能延长视频期限。
            if (direction === 'play' && item.decodedAt && performance.now() - item.decodedAt > 25000) throw new Error('远端' + kind + '解码持续中断'); // 仅持续收包但不解码同样失败。
        }
    }
    async function sample() { // 一套低频轮询覆盖双方各媒体证据。
        if (dead || statsBusy) return; // 上轮未结束时不重叠getStats。
        statsBusy = true; // 等待期间保持互斥。
        try { // 统计错误沿用统一终结路径。
            if (push) await inspect(push, 'push'); // 本端音频和视频各自必须发送。
            if (dead) return; // 发布统计期间可能已挂断。
            if (play) await inspect(play, 'play'); // 对端音画各自必须收包与解码。
            if (dead) return; // 接收统计期间可能已挂断。
            if (!readySent && push?.connectionState === 'connected' && kinds().every(kind => progress.push[kind]?.grew)) { readySent = true; report('ready'); } // 视频全部发送成立才通知对方拉流。
            const videoShown = config.call_type !== 'video' || (remote.videoWidth > 0 && remote.videoHeight > 0 && presentedFrames > 0); // 视频仅认真实呈现帧与有效画面尺寸，不提供弱化的兼容成功条件。
            if (!playingSent && kinds().every(kind => progress.play[kind]?.grew && progress.play[kind]?.decodedGrew) && playSucceeded && !remote.paused && remote.readyState >= 2 && remote.currentTime > playbackStart + 0.05 && videoShown) { playingSent = true; report('playing'); } // 实际音频播放与视频解码显示共同成立才报告对端播放。
        } catch (error) { fail(error); } // RTP或持续断流失败不静默。
        finally { statsBusy = false; } // 任一返回路径均解除互斥。
    }
    new QWebChannel(qt.webChannelTransport, channel => { // 订阅桥接后再握手，避免配置丢失。
        if (dead) return; // 通道晚初始化不得复活页面。
        bridge = channel.objects.voiceCall; // 继续使用历史共享对象而非新系统。
        handler = command => { // 保存引用以便cleanup断开信号。
            if (dead || command.call_id !== id || command.generation !== generation) return; // 包括stop在内均校验页面身份。
            if (command.action === 'stop') { cleanup(); return; } // 立即停止所有设备和异步任务。
            if (command.action === 'play') { peerReady = true; subscribe().catch(fail); return; } // 提前ready可锁存到configure后执行。
            if (command.action !== 'configure' || config) return; // 每页仅配置一次。
            if (!['audio', 'video'].includes(command.call_type)) { fail(new Error('通话媒体类型无效')); return; } // 不推断缺失类型或自动降级。
            config = command; // Qt已验证服务端类型与原邀请一致。
            peerReady = peerReady || Boolean(config.peer_ready); // 合并页面握手前的ready。
            document.body.classList.toggle('video-call', config.call_type === 'video'); // 同一页面按类型呈现画面，不新增通话系统。
            remote.onerror = () => fail(new Error('远端播放器失败：' + (remote.error?.message || remote.error?.code || '未知错误'))); // 播放错误明确反馈。
            remote.onended = () => fail(new Error('远端播放已结束')); // 远端结束触发整场清理。
            const timer = setInterval(sample, 500); timers.add(timer); // 一套串行采样覆盖两个方向。
            publish().catch(fail); // 双方accepted后才采集真实麦克风和可选摄像头。
            subscribe().catch(fail); // 本端权限等待不阻塞对方ready后的订阅。
        };
        bridge.command.connect(handler); // 必须先订阅以接收Qt立即下发的configure。
        bridge.pageReady(id, generation); // 向Qt确认本代次已经可接收命令。
    });
    addEventListener('pagehide', cleanup); // 页面关闭也必须回收全部资源。
})();
