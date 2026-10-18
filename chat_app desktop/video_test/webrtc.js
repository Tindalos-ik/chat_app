// 本脚本统一视频测试与好友语音的编解码筛选、完整ICE收集和ZLM单次SDP交换；不持有页面媒体资源。
'use strict'; // 禁止隐式全局变量，避免两类页面污染协商状态。
window.ChatWebRTC = (() => { // 对外仅暴露无会话状态的能力查询与异步协商接口。
    // kind为audio/video，sender选择发送能力；返回Opus/VP8能力数组，缺失能力直接抛出真实错误。
    function codecs(kind, sender) { // 两类页面复用相同的浏览器能力选择规则。
        const capabilities = (sender ? RTCRtpSender : RTCRtpReceiver).getCapabilities(kind); // 不用接收能力冒充发送编码能力。
        const mime = kind === 'video' ? 'video/vp8' : 'audio/opus'; // ZLM现有测试约定为VP8与Opus。
        const selected = capabilities?.codecs.filter(codec => codec.mimeType.toLowerCase() === mime) || []; // 保留匹配能力的原始参数。
        if (!selected.length) throw new Error('当前Qt WebEngine缺少WebRTC ' + mime + '能力'); // 能力不足时禁止发送不可用Offer。
        return selected; // setCodecPreferences直接消费能力对象。
    }
    // signal为页面生命周期；operation返回异步结果，ms为期限，label描述失败阶段；取消时拒绝并移除监听。
    function bounded(operation, ms, label, signal) { // 所有等待都能被页面结束及时作废。
        return new Promise((resolve, reject) => { // 将期限和生命周期接入同一个结果。
            let timer; // 保存本次等待的期限句柄。
            const detach = () => { clearTimeout(timer); signal.removeEventListener('abort', abort); }; // 无论完成方式均解除临时资源。
            const abort = () => { detach(); reject(new DOMException('媒体会话已结束', 'AbortError')); }; // 取消不会继续应用SDP。
            if (signal.aborted) { abort(); return; } // 结束后的新等待直接拒绝。
            signal.addEventListener('abort', abort, {once:true}); // 页面cleanup能打断等待。
            timer = setTimeout(() => { detach(); reject(new Error(label + '超时')); }, ms); // 到期提供明确阶段而非静默挂起。
            Promise.resolve().then(() => { if (signal.aborted) throw new DOMException('媒体会话已结束', 'AbortError'); return operation(); }).then(value => { detach(); resolve(value); }, error => { detach(); reject(error); }); // 微任务执行前复查取消，防止已结束页面发起新的SDP操作。
        });
    }
    // options包含pc/mediaUrl/app/stream/sender/kinds/signal/stage；返回Answer已应用的Promise，不重试HTTP或订阅。
    async function negotiate(options) { // 发布与订阅严格复用同一条非trickle时序。
        const {pc, mediaUrl, app, stream, sender, kinds, signal, stage} = options; // 所有会话状态归调用页面所有。
        const alive = () => { if (signal.aborted) throw new DOMException('媒体会话已结束', 'AbortError'); }; // 每次异步边界阻止关闭后复活。
        alive(); // 不为已结束会话创建Offer。
        stage(sender ? '正在收集ICE并向ZLM发布(push)…' : '正在收集ICE并向ZLM请求播放(play)…'); // 页面负责阶段呈现。
        const offer = await bounded(() => pc.createOffer(), 8000, '生成SDP', signal); // 先创建Offer再触发候选收集。
        alive(); // Offer晚到时不再设置本地描述。
        await bounded(() => pc.setLocalDescription(offer), 8000, '设置本地SDP', signal); // ICE候选会补入localDescription。
        alive(); // 本地描述完成后再次确认生命周期。
        await new Promise((resolve, reject) => { // 单独管理ICE监听，超时及取消都能完整移除。
            let timer; // ICE等待独立限定八秒。
            const detach = () => { clearTimeout(timer); pc.removeEventListener('icegatheringstatechange', changed); signal.removeEventListener('abort', abort); }; // 防止结束后残留PC回调。
            const changed = () => { if (pc.iceGatheringState === 'complete') { detach(); resolve(); } }; // 只在完整候选集形成后提交SDP。
            const abort = () => { detach(); reject(new DOMException('媒体会话已结束', 'AbortError')); }; // 取消解除候选监听。
            if (signal.aborted) { abort(); return; } // 处理注册监听前已结束的竞态。
            pc.addEventListener('icegatheringstatechange', changed); // 观察本次PC的候选状态。
            signal.addEventListener('abort', abort, {once:true}); // 会话退出无需等满八秒。
            timer = setTimeout(() => { detach(); reject(new Error('ICE候选收集超时')); }, 8000); // 禁止不完整候选集提前提交。
            changed(); // 兼容注册前已收集完成的PC。
        });
        alive(); // 收集完成后不允许终结页面发起HTTP。
        const url = new URL('/index/api/webrtc', mediaUrl); // 保持已跑通的ZLM接口和现有媒体端口。
        url.searchParams.set('app', app); // 视频测试和语音使用各自应用命名空间。
        url.searchParams.set('stream', stream); // 调用方明确选择自身发布流或对方订阅流。
        url.searchParams.set('type', sender ? 'push' : 'play'); // 不做订阅重试或隐式降级。
        const request = new AbortController(); // HTTP期限与整个页面生命周期均可取消请求。
        const abort = () => request.abort(); // 将页面取消转发至fetch。
        signal.addEventListener('abort', abort, {once:true}); // fetch结束后会移除此临时监听。
        let expired = false; // 区分HTTP期限与正常结束取消。
        const timer = setTimeout(() => { expired = true; request.abort(); }, 20000); // 覆盖ZLM最长十五秒等流期限。
        let answer; // Answer仅在HTTP及JSON成功后可用。
        try { // 无论网络结果如何都释放期限与取消监听。
            const response = await fetch(url, {method:'POST', headers:{'Content-Type':'text/plain'}, body:pc.localDescription.sdp, signal:request.signal, credentials:'omit', redirect:'error'}); // 一次发送包含完整ICE的SDP。
            if (!response.ok) throw new Error('ZLM HTTP ' + response.status); // 保留服务器HTTP错误码。
            answer = await response.json(); // ZLM响应含业务code、sdp及可能的msg。
        } catch (error) { // 网络与取消错误传给页面统一结束。
            if (expired) throw new Error('ZLM SDP请求超时（20秒）'); // 明确真正的请求期限原因。
            throw error; // 保留权限、网络与JSON解析的真实异常。
        } finally { clearTimeout(timer); signal.removeEventListener('abort', abort); } // 完成后不再持有页面取消回调。
        alive(); // 迟到HTTP结果不能继续操作已关闭PC。
        if (answer.code !== 0 || typeof answer.sdp !== 'string' || !answer.sdp.startsWith('v=0')) throw new Error('ZLM协商失败：' + (answer.msg || answer.code || '无有效answer')); // 先检查业务结果及SDP基本格式。
        for (const kind of kinds) { // 视频同时要求VP8/Opus，语音只要求Opus。
            const accepted = kind === 'video' ? /a=rtpmap:\d+ VP8\/90000/i : /a=rtpmap:\d+ opus\/48000/i; // 根据实际请求的媒体类型核查编码。
            if (!accepted.test(answer.sdp)) throw new Error('ZLM answer未接受' + (kind === 'video' ? 'VP8' : 'Opus')); // 不把缺少音轨的Answer视作成功。
        }
        await bounded(() => pc.setRemoteDescription({type:'answer', sdp:answer.sdp}), 8000, '设置ZLM answer', signal); // 只有有效Answer才能触发DTLS和媒体建立。
        alive(); // 结束后不再报告协商成功阶段。
        stage('SDP已接受，等待WebRTC连接和实际媒体数据…'); // 实际就绪仍由各页面统计决定。
    }
    return {codecs, negotiate}; // 页面只依赖此稳定接口，不共享PC或媒体轨道。
})();
