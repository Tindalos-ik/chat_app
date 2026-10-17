'use strict';
// 固定文件经浏览器解码后发布到ZLM；Qt通过QWebChannel协调当前会话的配置、启动和结束。
(() => {
    // URL携带当前测试ID与Qt页面代次，用于握手和过滤迟到命令。
    const query = new URLSearchParams(location.search);
    const id = query.get('test_id');
    const generation = Number(query.get('generation'));
    // source负责固定文件解码与捕获，remote负责接收流的呈现和实际帧观察。
    const source = document.getElementById('source');
    const remote = document.getElementById('remote');
    // bridge引用Qt控制器；其余配置、PC、捕获流及WebAudio节点由本页会话持有。
    let bridge, config, pc, captured, audioContext, audioSource, audioDestination;
    // dead标记终结，started锁定单次启动；readySent/playingSent分别锁定发布就绪与实际播放报告。
    let dead = false, started = false, readySent = false, playingSent = false;
    // statsBusy避免异步getStats重叠；frameSeen记录已呈现过视频帧，frameCallback供清理时取消观察。
    let statsBusy = false, frameSeen = false, frameCallback;
    // lastVideoBytes保存累计视频字节；progressAt记录最近增长时间，检测持续断流。
    let progressAt = performance.now(), lastVideoBytes = 0;
    // 登记本页计时任务和在途fetch的AbortController，结束时集中取消。
    const timers = new Set(), controllers = new Set();

    // 关窗/pagehide、stop或失败时调用；同步作废会话并发起资源释放，返回值为空，AudioContext关闭异步完成。
    function cleanup() {
        if (dead) return;
        dead = true;
        // 停止采样和阶段期限并中止fetch；已取消期限的待完成Promise随页面上下文退出。
        for (const timer of timers) { clearTimeout(timer); clearInterval(timer); }
        timers.clear();
        for (const controller of controllers) controller.abort();
        controllers.clear();
        // 取消呈现帧观察并解除PC回调，close结束连接；stop分别结束捕获轨道和接收轨道。
        if (frameCallback !== undefined && remote.cancelVideoFrameCallback)
            remote.cancelVideoFrameCallback(frameCallback);
        if (pc) { pc.ontrack = pc.onconnectionstatechange = null; pc.close(); pc = null; }
        if (captured) captured.getTracks().forEach(track => track.stop());
        if (remote.srcObject) remote.srcObject.getTracks().forEach(track => track.stop());
        // pause停止播放，解除srcObject/src后load重置元素，释放当前媒体加载与解码状态。
        for (const video of [source, remote]) {
            video.pause(); video.srcObject = null; video.removeAttribute('src'); video.load();
        }
        // 断开音频处理图、停止destination输出轨道，再关闭Context释放音频运行资源。
        if (audioSource) audioSource.disconnect();
        if (audioDestination) audioDestination.stream.getTracks().forEach(track => track.stop());
        if (audioContext) audioContext.close().catch(() => {});
    }
    // event/detail描述阶段或错误；会话存活时异步投递Qt槽，返回值为空。
    function report(event, detail = '') {
        if (!dead && bridge) bridge.mediaEvent(id, generation, event, String(detail));
    }
    // error接收异常或文本；先上报错误，再触发本页清理，返回值为空。
    function fail(error) {
        if (dead) return;
        report('error', error && error.message ? error.message : error);
        cleanup();
    }
    // ms毫秒后在会话存活时执行callback，返回计时器句柄并登记到timers。
    function later(callback, ms) {
        const timer = setTimeout(() => { timers.delete(timer); if (!dead) callback(); }, ms);
        timers.add(timer); return timer;
    }
    // 包装promise，返回保留其结果的Promise；活跃期间超出ms则以stage说明拒绝。
    function bounded(promise, ms, stage) {
        return new Promise((resolve, reject) => {
            const timer = later(() => reject(new Error(stage + '超时')), ms);
            promise.then(value => { clearTimeout(timer); timers.delete(timer); resolve(value); },
                error => { clearTimeout(timer); timers.delete(timer); reject(error); });
        });
    }
    // 使用config.file_url加载唯一素材；返回Promise，loadeddata时兑现，解码错误时拒绝。
    function waitMedia() {
        return new Promise((resolve, reject) => {
            const loaded = () => { detach(); resolve(); };
            const error = () => { detach(); reject(new Error('固定文件解码失败：' + (source.error?.message || '不支持的媒体'))); };
            const detach = () => { source.removeEventListener('loadeddata', loaded); source.removeEventListener('error', error); };
            source.addEventListener('loadeddata', loaded, {once:true});
            source.addEventListener('error', error, {once:true});
            source.src = config.file_url;
            source.load();
        });
    }
    // kind取video/audio，sender选择发送/接收能力；返回VP8/Opus能力列表，缺失时抛错。
    function codecs(kind, sender) {
        const capabilities = (sender ? RTCRtpSender : RTCRtpReceiver).getCapabilities(kind);
        const mime = kind === 'video' ? 'video/vp8' : 'audio/opus';
        const selected = capabilities?.codecs.filter(codec => codec.mimeType.toLowerCase() === mime) || [];
        if (!selected.length) throw new Error('当前Qt WebEngine缺少WebRTC ' + mime + '能力');
        return selected;
    }
    // 返回候选收集完成的Promise，期限8秒；收齐候选后将完整SDP一次提交，连通性由后续ICE检查决定。
    async function gather() {
        if (pc.iceGatheringState === 'complete') return;
        await bounded(new Promise(resolve => {
            const changed = () => {
                if (!dead && pc.iceGatheringState === 'complete') {
                    pc.removeEventListener('icegatheringstatechange', changed); resolve();
                }
            };
            pc.addEventListener('icegatheringstatechange', changed); changed();
        }), 8000, 'ICE候选收集');
    }
    // sender选择push发布或play订阅；返回覆盖SDP交换的Promise，失败向调用方抛出。
    async function negotiate(sender) {
        // 创建Offer后设置本地描述以触发ICE收集；候选持续补入pc.localDescription，收齐后再取SDP。
        report('stage', sender ? '正在收集ICE并向ZLM发布(push)…' : '正在收集ICE并向ZLM请求播放(play)…');
        const offer = await bounded(pc.createOffer(), 8000, '生成SDP');
        if (dead) return;
        await bounded(pc.setLocalDescription(offer), 8000, '设置本地SDP');
        await gather();
        // 此处与Offer、fetch完成处检查dead，阻止结束后的协商结果继续应用到会话。
        if (dead) return;
        const url = new URL('/index/api/webrtc', config.media_url);
        url.searchParams.set('app', 'video_test');
        url.searchParams.set('stream', config.stream_id);
        url.searchParams.set('type', sender ? 'push' : 'play');
        const controller = new AbortController(); controllers.add(controller);
        // 业务ready后单次请求play；ZLM按maxStreamWaitMS=15000等待流注册，客户端HTTP期限20秒。
        const timeout = later(() => controller.abort(), 20000);
        let response, answer;
        try {
            response = await fetch(url, {method:'POST', headers:{'Content-Type':'text/plain'},
                body:pc.localDescription.sdp, signal:controller.signal, credentials:'omit', redirect:'error'});
            if (!response.ok) throw new Error('ZLM HTTP ' + response.status);
            answer = await response.json();
        } finally { clearTimeout(timeout); timers.delete(timeout); controllers.delete(controller); }
        if (dead) return;
        // 先检查HTTP，再检查ZLM业务结果和Answer编码能力，最后将Answer应用于PC。
        if (answer.code !== 0 || typeof answer.sdp !== 'string' || !answer.sdp.startsWith('v=0'))
            throw new Error('ZLM协商失败：' + (answer.msg || answer.code || '无有效answer'));
        if (!/a=rtpmap:\d+ VP8\/90000/i.test(answer.sdp) || !/a=rtpmap:\d+ opus\/48000/i.test(answer.sdp))
            throw new Error('ZLM answer未接受VP8/Opus');
        // Answer应用完成后进入ICE选路、DTLS建连和媒体传输阶段，由连接状态与sample确认实际进展。
        await bounded(pc.setRemoteDescription({type:'answer', sdp:answer.sdp}), 8000, '设置ZLM answer');
        report('stage', 'SDP已接受，等待WebRTC连接和实际媒体数据…');
    }
    // event为PC的track事件；同步合并接收轨道，首次play的异步拒绝交给fail处理，返回值为空。
    function bindRemote(event) {
        if (dead) return;
        const stream = remote.srcObject || new MediaStream();
        if (!stream.getTracks().some(track => track.id === event.track.id)) stream.addTrack(event.track);
        // 音视频轨道可能分别到达；复用同一stream且仅首次赋srcObject/play，避免重置打断播放。
        if (!remote.srcObject) {
            remote.srcObject = stream;
            remote.muted = false; remote.volume = 1;
            remote.play().catch(error => fail(new Error('远端自动播放失败：' + error.message)));
        }
    }
    // 为remote递归登记呈现帧回调并更新frameSeen，返回值为空；句柄保留给cleanup取消。
    function watchFrames() {
        if (dead) return;
        frameCallback = remote.requestVideoFrameCallback(() => { frameSeen = true; watchFrames(); });
    }
    // 返回本轮getStats采样的Promise；每500ms调用，内部捕获失败并清理，statsBusy串行化采样。
    async function sample() {
        if (dead || statsBusy || !pc) return;
        statsBusy = true;
        try {
            const stats = await pc.getStats();
            if (dead) return;
            let videoBytes = 0, audioBytes = 0, videoFrames = 0;
            stats.forEach(stat => {
                const kind = stat.kind || stat.mediaType;
                if (stat.type === (config.sender ? 'outbound-rtp' : 'inbound-rtp') && !stat.isRemote) {
                    if (kind === 'video') {
                        videoBytes += config.sender ? (stat.bytesSent || 0) : (stat.bytesReceived || 0);
                        videoFrames += stat.framesDecoded || 0;
                    }
                    if (kind === 'audio') audioBytes += config.sender ? (stat.bytesSent || 0) : (stat.bytesReceived || 0);
                }
            });
            if (videoBytes > lastVideoBytes) { lastVideoBytes = videoBytes; progressAt = performance.now(); }
            // 发布端按connected与累计视频字节报告ready；Qt再经TCP通知好友可订阅。
            if (pc.connectionState === 'connected' && videoBytes > 0) {
                if (config.sender && !readySent) { readySent = true; report('ready'); }
                // 接收端同时核对音频字节、呈现/解码帧和播放器状态，再报告playing。
                if (!config.sender && !playingSent && audioBytes > 0 && (frameSeen || videoFrames > 0)
                    && !remote.paused && remote.readyState >= 2) {
                    playingSent = true; report('playing');
                }
            }
            if (performance.now() - progressAt > 25000) throw new Error('WebRTC没有持续的实际视频数据');
        } catch (error) { fail(error); }
        finally { statsBusy = false; }
    }
    // sender选择发布/订阅；返回轨道准备与SDP协商的Promise，调用方catch(fail)，后续媒体由sample观察。
    async function start(sender) {
        if (dead || started) return;
        started = true;
        const videoCodecs = codecs('video', sender), audioCodecs = codecs('audio', sender);
        pc = new RTCPeerConnection({iceServers:[]});
        pc.onconnectionstatechange = () => {
            if (!dead && ['failed', 'closed', 'disconnected'].includes(pc.connectionState))
                fail(new Error('WebRTC连接状态：' + pc.connectionState));
        };
        if (sender) {
            if (typeof source.captureStream !== 'function')
                throw new Error('当前Qt WebEngine不支持HTMLMediaElement.captureStream，无法发布文件');
            report('stage', '正在解码固定文件并捕获音视频轨道…');
            // Qt6.8.3实测静音/零音量会清零capture音轨；保持元素音量，同一元素经WebAudio抽取发送音频。
            // 音频图只连接MediaStreamDestination，本地预览静音，音视频共用文件播放时钟。
            source.muted = false; source.volume = 1;
            source.style.display = 'block'; remote.style.display = 'none';
            // loadeddata、AudioContext.resume和source.play分别等待解码数据、音频图和播放启动。
            await bounded(waitMedia(), 10000, '读取固定文件');
            // 这些等待完成后检查dead，避免关窗后继续创建音频资源或向PC添加轨道。
            if (dead) return;
            audioContext = new AudioContext();
            audioSource = audioContext.createMediaElementSource(source);
            audioDestination = audioContext.createMediaStreamDestination();
            audioSource.connect(audioDestination);
            await bounded(audioContext.resume(), 5000, '固定文件音频启动');
            if (dead) return;
            captured = source.captureStream();
            // 停止并移除原捕获音轨，释放被替换的轨道资源；同一元素的WebAudio输出轨道接入发送流。
            captured.getAudioTracks().forEach(track => { captured.removeTrack(track); track.stop(); });
            const audio = audioDestination.stream.getAudioTracks()[0];
            if (!audio) throw new Error('固定文件音频提取未产生轨道');
            captured.addTrack(audio);
            await bounded(source.play(), 8000, '本地文件播放');
            if (dead) return;
            const video = captured.getVideoTracks()[0];
            if (!video) throw new Error('captureStream未产生文件视频轨道');
            if (!video.enabled || !audio.enabled || video.readyState !== 'live' || audio.readyState !== 'live')
                throw new Error('固定文件捕获轨道未激活');
            // WebM由HTML媒体元素解码；WebRTC按VP8/Opus能力重新编码，两条sendonly轨道共用发布PC。
            pc.addTransceiver(video, {direction:'sendonly', streams:[captured]}).setCodecPreferences(videoCodecs);
            pc.addTransceiver(audio, {direction:'sendonly', streams:[captured]}).setCodecPreferences(audioCodecs);
            source.onended = () => { report('ended'); cleanup(); };
            source.onerror = () => fail(new Error('固定文件播放/解码失败'));
        } else {
            // 好友端独立PC配置recvonly；发布/订阅双方的SDP和媒体对端均为ZLM。
            pc.ontrack = bindRemote;
            pc.addTransceiver('video', {direction:'recvonly'}).setCodecPreferences(videoCodecs);
            pc.addTransceiver('audio', {direction:'recvonly'}).setCodecPreferences(audioCodecs);
            if (remote.requestVideoFrameCallback) watchFrames();
        }
        await negotiate(sender);
        if (dead) return;
        progressAt = performance.now();
        const timer = setInterval(sample, 500); timers.add(timer);
        later(() => {
            if (!(sender ? readySent : playingSent)) fail(new Error('WebRTC连接或实际音视频播放超时'));
        }, 25000);
    }
    // 先订阅command再回报pageReady，让Qt安全交付配置；stop按test_id处理，其他命令再核对页面代次。
    new QWebChannel(qt.webChannelTransport, channel => {
        bridge = channel.objects.videoTest;
        bridge.command.connect(command => {
            if (command.test_id !== id || dead) return;
            if (command.action === 'stop') { cleanup(); return; }
            if (command.generation !== generation) return;
            if (command.action === 'configure') {
                if (config) return;
                config = command;
                // 发布端立即启动；接收端使用Qt锁存的ready决定启动或等待play，started统一锁定单次启动。
                if (config.sender || config.ready) start(config.sender).catch(fail);
            } else if (command.action === 'play' && config && !config.sender) start(false).catch(fail);
        });
        bridge.pageReady(id, generation);
    });
    // 页面退出与Qt结束命令共用资源清理入口。
    addEventListener('pagehide', cleanup);
})();
