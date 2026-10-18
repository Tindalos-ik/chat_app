#include "voicecallcontroller.h" // 历史VoiceCall声明同一好友音视频会话状态。
#include "videotestcontroller.h" // 复用安全页面工厂与scheme。
#include "tcpmgr.h" // 发送已登录业务请求。
#include "usermgr.h" // 查询当前账号和好友名称。
#include <QDialog> // 提供独立通话窗口。
#include <QLabel> // 展示业务及媒体错误。
#include <QPushButton> // 接听与挂断操作。
#include <QVBoxLayout> // 窗口统一纵向布局。
#include <QSizePolicy> // 明确状态文字与媒体画布的纵向空间分配。
#include <QUrlQuery> // 携带页面会话和代次。
#include <QWebChannel> // 连接受信脚本与控制器。
#include <QWebEngineView> // 承载共享音画页面及本地预览。
#include <QWebEnginePage> // 限制页面权限。
#include <QWebEngineProfile> // 每通电话隔离浏览器状态。
#include <QWebEngineSettings> // 允许业务驱动自动播放。

// tcp拥有本对象；结束或断线统一销毁采集页面。
VoiceCallController::VoiceCallController(TcpMgr *tcp) : QObject(tcp), _tcp(tcp) {
    _deadline.setSingleShot(true); // 每次阶段重新计时，不重复触发。
    connect(&_deadline, &QTimer::timeout, this, [this] { Finish("timeout", tr("通话等待超时，通话已结束"), true); }); // 邀请与两种媒体建连均不能无限占用。
    connect(tcp, &TcpMgr::sig_voice_call, this, &VoiceCallController::Handle); // 所有窗口共用唯一信令入口。
    connect(tcp, &TcpMgr::sig_video_test_reset, this, [this] { // 登录代次结束时回收跨通话去重记录，避免客户端常驻积累。
        Finish("logout", tr("连接已关闭"), true, true); // 先终结当前页面并尝试在原连接发送结束。
        _retired.clear(); _cancelled.clear(); // 原连接后续帧不再分发，下一登录不需要旧会话去重或补偿。
    });
    connect(tcp, &TcpMgr::sig_off_line, this, [this] { Finish("logout", tr("账号已下线"), true, true); }); // 踢下线立即关闭麦克风、摄像头与媒体页。
}
// uid只用于显示，不作为服务端身份依据。
QString VoiceCallController::PeerName(int uid) const {
    for (const auto &friendInfo : UserMgr::GetInstance()->GetFriendList()) { // 采用登录时缓存的好友资料。
        if (friendInfo && friendInfo->_uid == uid) return tr("%1（UID %2）").arg(friendInfo->_nick.isEmpty() ? friendInfo->_name : friendInfo->_nick).arg(uid); // 始终展示不可混淆的UID。
    }
    return tr("好友 UID %1").arg(uid); // 尚未加载资料也不隐藏来电身份。
}
// 点击已有通话只激活窗口，不能创建第二次采集。
void VoiceCallController::Start(int friendUid, const QString &callType) { // 历史语音协调器承载audio/video，共用服务端状态与终结隔离。
    if (_active) { if (_window) { _window->show(); _window->raise(); _window->activateWindow(); } return; } // 保留当前唯一通话。
    if (callType != "audio" && callType != "video") return; // 不推断缺失模式，避免视频入口意外请求仅音频。
    const auto account = UserMgr::GetInstance()->GetUserInfo(); // 捕获本次发起账号。
    if (!account || friendUid <= 0 || friendUid == account->_uid) return; // 不发起无效或自呼叫。
    Finish("closed", {}, false, true); // 关闭上次结束后的提示窗口。
    _self = account->_uid; _peer = friendUid; _login = _tcp->LoginGeneration(); // 绑定账号、对端和连接代次。
    _callType = callType; // 在创建邀请窗口前固定本次请求的类型。
    _active = true; _incoming = false; // 进入主叫等待状态，尚不创建WebEngine。
    CreateWindow(false); // 提供等待和取消操作。
    _inviteRequest = _tcp->SendVoiceCall("invite", friendUid, {}, {}, _callType); // 类型只随invite发送，后续由权威会话确定。
    if (_inviteRequest.isEmpty()) { Finish("connection", tr("聊天服务器未连接"), false); return; } // 无信令时直接收敛。
    _deadline.start(60000); // 邀请无人响应时结束占位。
}
// incoming只决定等待阶段按钮；真正接听仍须等待服务器accepted。
void VoiceCallController::CreateWindow(bool incoming) {
    auto *dialog = new QDialog; // 顶层窗口由关闭动作回收。
    _window = dialog; // 控制器只保存弱引用。
    dialog->setAttribute(Qt::WA_DeleteOnClose); // 避免结束窗口遗留。
    dialog->setWindowTitle(tr("%1通话 · %2").arg(_callType == "video" ? tr("视频") : tr("语音"), PeerName(_peer))); // 接听前明确展示会访问的设备类型。
    dialog->resize(_callType == "video" ? 900 : 520, _callType == "video" ? 700 : 320); // 视频模式容纳双画面，语音保持轻量窗口。
    auto *layout = new QVBoxLayout(dialog); // 窗口拥有控件布局。
    _status = new QLabel(incoming ? tr("%1 发来%2通话，是否接听？").arg(PeerName(_peer), _callType == "video" ? tr("视频") : tr("语音")) : tr("正在呼叫 %1…").arg(PeerName(_peer)), dialog); // 告知被叫接听视频将启用摄像头。
    _status->setWordWrap(true); layout->addWidget(_status); // 允许完整显示媒体错误。
    _status->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum); // 状态只占实际文字高度，避免长空白挤压真实视频画布。
    _accept = new QPushButton(tr("接听"), dialog); // 接听不直接打开麦克风。
    _accept->setVisible(incoming); layout->addWidget(_accept); // 主叫无接听操作。
    _hangup = new QPushButton(incoming ? tr("拒绝") : tr("取消呼叫"), dialog); // 各等待角色提供相应操作。
    layout->addWidget(_hangup); // 操作始终位于媒体页之前。
    connect(_accept, &QPushButton::clicked, this, [this] { // 请求成功前保持无媒体状态。
        if (!_active || _accepted || _acceptPending) return; // 防止重复提交接听。
        _acceptPending = true; _accept->setEnabled(false); _hangup->setText(tr("挂断")); // 已提交接听后关闭应走end。
        const QString request = _tcp->SendVoiceCall("accept", 0, _callId); // 仅使用服务端会话ID。
        if (request.isEmpty()) { Finish("connection", tr("接听失败：连接已关闭"), true); return; } // 失败不能遗留等待状态。
        _requests.insert(request, "accept"); SetStatus(tr("正在确认接听…")); _deadline.start(15000); // 等待服务器accepted。
    });
    connect(_hangup, &QPushButton::clicked, dialog, &QDialog::close); // 拒绝、取消、挂断均共用结束入口。
    connect(dialog, &QDialog::finished, this, [this] { Finish("closed", tr("通话已关闭"), true, true); }); // 原生关闭按钮同样拒绝或挂断。
    dialog->show(); // 来电即使好友资料页未打开也可处理。
}
// message为服务端消息；所有页面启动前先检查账号、会话和业务状态。
void VoiceCallController::Handle(const QJsonObject &message, bool notification) {
    const auto account = UserMgr::GetInstance()->GetUserInfo(); // 未登录不接收来电。
    if (!account) return; // 退出后不恢复窗口。
    const QString op = message.value("op").toString(); // 业务操作名。
    const QString id = message.value("call_id").toString(); // 服务端唯一会话ID。
    const QString status = message.value("status").toString(); // 业务成功阶段或具体错误。
    const QString request = message.value("request_id").toString(); // 响应UUID关联。
    const QString inviteRequest = message.value("invite_request_id").toString(); // 原邀请UUID跨accept/ready/end保持稳定，不能用相同好友UID代替呼叫关联。
    const QString callType = message.value("call_type").toString(); // 类型来自服务端元数据，不能由accepted或ready临时切换。
    if (notification && _cancelled.contains(inviteRequest) && _cancelled.value(inviteRequest) == _tcp->LoginGeneration()
        && message.value("fromuid").toInt() == account->_uid && !id.isEmpty()) { // 取消后立即重拨时，旧呼叫通知仍只属于原邀请。
        const bool retired = _retired.contains(id); // 同一旧call_id仅补偿一次活动通知。
        _retired.insert(id); _earlyReady.remove(id); // 旧通话不得进入当前等待、提前ready或媒体页面。
        if (!retired && (status == "invited" || status == "accepted" || status == "ready")) _tcp->SendVoiceCall("end", 0, id, "cancelled"); // 原邀请回包前也可精确撤销旧活动通话，终态不再发送end。
        return; // 无论当前是否已重拨，都不改变新呼叫。
    }
    if (!notification && op == "invite" && _cancelled.contains(request)) { // 用户可能在invite响应前已取消。
        const quint64 login = _cancelled.take(request); // 只补偿同一登录连接。
        if (login == _tcp->LoginGeneration() && !id.isEmpty() && status == "invited" && !_retired.contains(id)) { _retired.insert(id); _tcp->SendVoiceCall("end", 0, id, "cancelled"); } // 已由旧活动通知补偿或已见终态则不重复end，否则精确结束迟到分配的ID。
        return; // 迟到成功不能创建窗口。
    }
    if (notification && (status == "ended" || status == "rejected") && !id.isEmpty()) { // 结束可能早于invite响应或accepted。
        _retired.insert(id); _earlyReady.remove(id); // 终结ID不能复活。
        if (_active && !_incoming && _callId.isEmpty() && !_inviteRequest.isEmpty() && _self == account->_uid && _login == _tcp->LoginGeneration()
            && inviteRequest == _inviteRequest && message.value("fromuid").toInt() == _self && message.value("touid").toInt() == _peer) _callId = id; // 只有原邀请UUID及双方身份均匹配的提前终态才能结束当前pending主叫。
        if (_active && id == _callId) Finish("remote", status == "rejected" ? tr("对方已拒绝来电") : tr("对方已挂断：%1").arg(message.value("reason").toString().left(160)), false); // 呈现对方真实结束原因。
        return; // 不继续处理终结消息。
    }
    if (_retired.contains(id)) { // 丢弃终结会话的迟到通知。
        if (!notification && request == _inviteRequest) Finish("remote", tr("通话已结束"), false); // 尚未取得ID的主叫也退出等待。
        return; // 禁止晚到accepted恢复采集。
    }
    if (notification && status == "invited" && op == "invite") { // 新来电不启动媒体。
        const int from = message.value("fromuid").toInt(); // 来电发起方UID由服务端提供。
        if (id.isEmpty() || id.size() > 128 || from <= 0 || from == account->_uid || message.value("touid").toInt() != account->_uid || (callType != "audio" && callType != "video")) return; // 无权威类型的来电不能展示接听或打开设备。
        if (_active) { if (id != _callId) _tcp->SendVoiceCall("reject", 0, id, "busy"); return; } // 当前通话不被另一来电替换。
        Finish("closed", {}, false, true); // 关闭旧提示窗口。
        _self = account->_uid; _peer = from; _login = _tcp->LoginGeneration(); _callId = id; // 绑定本次被叫身份。
        _callType = callType; // 先锁定来电设备范围，再显示用户确认窗口。
        _active = true; _incoming = true; CreateWindow(true); _deadline.start(60000); // 等待明确接听或拒绝。
        return; // invited阶段绝不访问麦克风。
    }
    if (!_active || _self != account->_uid || _login != _tcp->LoginGeneration()) return; // 账号或连接更换后忽略旧结果。
    if ((notification || message.value("error").toInt(-1) == 0) && ((!notification && op == "invite" && request == _inviteRequest && status == "invited") || (!_callId.isEmpty() && id == _callId && (notification || _requests.value(request) == op)) || (notification && !_inviteRequest.isEmpty() && inviteRequest == _inviteRequest)) && callType != _callType) { // 只校验已关联成功结果/活动通知；失败响应可能无类型，须保留后续UUID关联后的真实错误。
        Finish("invalid", tr("服务器通话类型与当前邀请不一致"), true); return; // 类型异常必须停止整个呼叫，不能自动降级为语音。
    }
    if (!notification && op == "invite" && request == _inviteRequest) { // 主叫收到唯一邀请结果。
        _inviteRequest.clear(); // 不再把已应答邀请计为待取消请求。
        if (status != "invited" || message.value("error").toInt(-1) != 0) { Finish("failed", StatusText(status), false); return; } // 离线和业务失败单独显示。
        if (id.isEmpty() || id.size() > 128 || inviteRequest != request || message.value("fromuid").toInt() != _self || message.value("touid").toInt() != _peer) { Finish("invalid", tr("服务器来电描述无效"), false); return; } // 成功回包必须属于当前原邀请及相同双方。
        if (!_callId.isEmpty() && _callId != id) { _tcp->SendVoiceCall("end", 0, id, "invalid"); Finish("invalid", tr("服务器返回了不一致的通话会话"), true); return; } // 提前accepted绑定的身份不能被晚到邀请结果替换。
        _callId = id; if (!_accepted) SetStatus(tr("已通知 %1，等待接听…").arg(PeerName(_peer))); // 晚到invite不能覆盖实际通话状态。
        return; // accepted才允许下一阶段。
    }
    if (notification && status == "ready" && op == "ready" && !id.isEmpty() && message.value("actoruid").toInt() == _peer) { // 允许ready早于accepted甚至invite响应。
        if ((!_callId.isEmpty() && id == _callId) || (_callId.isEmpty() && !_incoming && !_inviteRequest.isEmpty() && inviteRequest == _inviteRequest)) _earlyReady.insert(id); // 未取得call_id时必须先精确匹配原邀请，旧同好友ready不能污染新呼叫。
    }
    if (notification && op == "accept" && status == "accepted" && !_incoming && _callId.isEmpty() && !_inviteRequest.isEmpty()
        && inviteRequest == _inviteRequest && !id.isEmpty() && id.size() <= 128 && message.value("fromuid").toInt() == _self && message.value("touid").toInt() == _peer) _callId = id; // 提前accepted只绑定当前原邀请，不以同一好友身份猜测call_id。
    if (_callId.isEmpty() || id != _callId) return; // 其余消息只作用于当前会话。
    if (!notification) { // 响应必须属于本端尚未完成的操作。
        if (_requests.value(request) != op) return; // 不能以伪造/旧UUID推进状态。
        _requests.remove(request); // 每次操作只消费一次。
        if (message.value("error").toInt(-1) != 0) { Finish("failed", StatusText(status), true); return; } // ready/accept业务失败也立即停麦。
    }
    if (status == "accepted" && op == "accept") { // 双方均由服务器accepted启动发布。
        if (_accepted) return; // 重复accepted不能覆盖playing或重复采集。
        const QUrl origin(message.value("media_url").toString()); // 后续SDP限定此源。
        if (message.value("fromuid").toInt() != (_incoming ? _peer : _self) || message.value("touid").toInt() != (_incoming ? _self : _peer)
            || message.value("app").toString() != "voice_call" || message.value("caller_stream").toString().isEmpty() || message.value("callee_stream").toString().isEmpty()
            || message.value("caller_stream").toString().size() > 128 || message.value("callee_stream").toString().size() > 128
            || (origin.scheme() != "http" && origin.scheme() != "https") || origin.host().isEmpty() || !origin.userInfo().isEmpty()
            || !origin.query().isEmpty() || !origin.fragment().isEmpty() || (!origin.path().isEmpty() && origin.path() != "/")) { // 校验参与者、双流和媒体源，不让页面获得任意网络入口。
            Finish("invalid", tr("服务器通话媒体描述无效"), true); return; // 两类通话均拒绝无效accepted配置。
        }
        _session = message; _accepted = true; _peerReady = _earlyReady.remove(id) || _peerReady; // accepted保留之前到达的ready。
        if (_accept) _accept->hide(); if (_hangup) _hangup->setText(tr("挂断")); // 进入通话阶段操作。
        SetStatus(_callType == "video" ? tr("对方已接听，正在连接麦克风、摄像头和媒体服务…") : tr("对方已接听，正在连接麦克风和媒体服务…")); _deadline.start(45000); ConfigurePage(); // accepted才允许采集，视频沿用同一建连期限。
    } else if (notification && status == "ready" && message.value("actoruid").toInt() == _peer && !_peerReady) { // 只有对端实际发布可触发拉流。
        _peerReady = true; // 单次锁存，重复通知不重复play。
        if (_accepted && _pageReady) emit command(QJsonObject{{"action", "play"}, {"call_id", _callId}, {"generation", _generation}}); // 页面未握手则留给configure。
    } // ready成功响应不覆盖真实playing提示，也不停止尚未收音的建连超时。
}
// profile和page按一次通话隔离，结束销毁页面释放本次类型的全部设备。
void VoiceCallController::ConfigurePage() {
    if (!_active || !_accepted || !_window || _view) return; // 禁止邀请阶段或重复创建。
    _view = new QWebEngineView(_window); // 窗口拥有媒体view。
    auto *profile = new QWebEngineProfile(_window); // 使用off-the-record隔离缓存与权限。
    auto *page = VideoTestController::CreateMediaPage(profile, _view, "/voice.html", {}, QUrl(_session.value("media_url").toString())); // 复用视频的安全边界。
    connect(page, &QObject::destroyed, profile, &QObject::deleteLater); // profile必须晚于page回收。
    _view->setPage(page); // view持有page。
    page->settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false); // 对端ready驱动自动播放。
    page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true); // SDP跨域仍受请求白名单限制。
    const int generation = _generation; const QString id = _callId; // 权限回调捕获不可变页面身份。
    connect(page, &QWebEnginePage::featurePermissionRequested, page, [this, page, generation, id](const QUrl &url, QWebEnginePage::Feature feature) { // 权限严格绑定当前accepted的类型、受信页面和生命周期。
        const bool allowed = _active && _accepted && generation == _generation && id == _callId && _view && _view->page() == page
            && url.scheme() == "chatvideo" && url.host() == "test" && (url.path().isEmpty() || url.path() == "/" || url.path() == "/voice.html")
            && page->url().path() == "/voice.html" && (feature == QWebEnginePage::MediaAudioCapture || (_callType == "video" && (feature == QWebEnginePage::MediaVideoCapture || feature == QWebEnginePage::MediaAudioVideoCapture))); // audio只授麦克风；video可授摄像头或组合请求，仍受accepted和页面代次约束。
        page->setFeaturePermission(url, feature, allowed ? QWebEnginePage::PermissionGrantedByUser : QWebEnginePage::PermissionDeniedByUser); // 当前用户确认的类型决定最小设备权限。
    });
    connect(page, &QWebEnginePage::loadFinished, this, [this, generation, id](bool ok) { if (!ok && _active && generation == _generation && id == _callId) Finish("media_error", tr("内置通话页面加载失败"), true); }); // 两种通话共用页面，加载失败立即终结。
    auto *channel = new QWebChannel(page); // page负责channel生命周期。
    channel->registerObject("voiceCall", this); page->setWebChannel(channel); // 历史桥接名称由两类通话共用，保持单一生命周期。
    static_cast<QVBoxLayout*>(_window->layout())->addWidget(_view, 1); // 已知共享纵向布局将剩余空间交给媒体页，让远端画面与本地预览可见。
    QUrl url("chatvideo://test/voice.html"); QUrlQuery query; // 固定内置入口不可远程导航。
    query.addQueryItem("call_id", _callId); query.addQueryItem("generation", QString::number(_generation)); // JS握手必须回传会话和页面代次。
    url.setQuery(query); _view->load(url); // 等待页面订阅完成再下发配置。
}
// 页面订阅后才发送configure，避免丢失accepted和提前ready。
void VoiceCallController::pageReady(const QString &id, int generation) {
    if (!_active || !_accepted || id != _callId || generation != _generation || _pageReady) return; // 丢弃旧页面或重复握手。
    _pageReady = true; QJsonObject config = _session; // 配置来自已校验的服务器accepted。
    config.insert("action", "configure"); config.insert("generation", _generation); // 页面据此绑定当前代次。
    config.insert("self_uid", _self); config.insert("peer_ready", _peerReady); // 让双方选择各自发布流，并锁存提前ready。
    emit command(config); // JS异步采集本次类型需要的设备并发布同一流。
}
// JS只能回报当前accepted页面；ready须证明全部必需媒体已实际发送。
void VoiceCallController::mediaEvent(const QString &id, int generation, const QString &event, const QString &detail) {
    if (!_active || !_accepted || id != _callId || generation != _generation) return; // 旧JS回调不能复活会话。
    if (event == "error") Finish("media_error", tr("通话媒体失败：%1").arg(detail.left(1000)), true); // 两类通话均呈现真实权限、设备、SDP和播放错误。
    else if (event == "ready" && !_readySent) { // 每端只发布一次业务ready。
        _readySent = true; const QString request = _tcp->SendVoiceCall("ready", 0, _callId); // 脚本已证明音频及视频模式下的视频均真实发送。
        if (request.isEmpty()) Finish("connection", tr("本端媒体已发布，但聊天连接不可用"), true); // 无业务连接时不能继续占用设备。
        else { // 仅在ready实际发出后合并已经收到的对端播放证据。
            _requests.insert(request, "ready"); // 后续失败响应仍会终结通话。
            if (_playing) { _deadline.stop(); SetStatus(_callType == "video" ? tr("双向视频通话中：本端音画已发送，对端音画已播放") : tr("双向语音通话中：本端音频已发出并播放对端音频")); } // 必须同时拥有本端全部媒体发送及远端全部媒体播放证据。
            else SetStatus(_callType == "video" ? tr("本端音画已实际发出，等待对端音画…") : tr("本端音频已实际发出，等待对端音频…")); // 视频只单轨成功时页面不会上报ready。
        }
    } else if (event == "playing") { // 页面已确认收包、解码采样和播放器运行。
        _playing = true; // 接收可早于麦克风授权，因此先记录单向接收证据。
        if (_readySent) { _deadline.stop(); SetStatus(_callType == "video" ? tr("双向视频通话中：本端音画已发送，对端音画已播放") : tr("双向语音通话中：本端音频已发出并播放对端音频")); } // 不将只收不发或只有音轨的会话宣称双向成功。
        else SetStatus(_callType == "video" ? tr("对端音画已播放，等待本端麦克风与摄像头发布…") : tr("已收到并播放对端音频，等待本端麦克风发布…")); // 保留完整建连期限。
    } else if (event == "stage" && !_playing) SetStatus(detail.left(1000)); // 不让晚到协商阶段覆盖通话状态。
}
// 只改提示，不推进媒体业务阶段。
void VoiceCallController::SetStatus(const QString &text) { if (_status) _status->setText(text); } // 结束窗口仍可显示错误。
// 状态来自共享业务协议，保留未知错误便于定位。
QString VoiceCallController::StatusText(const QString &status) const {
    if (status == "offline" || status == "unreachable") return tr("对方不在线"); // 对用户统一呈现离线，协议仍区分不存在路由与无法入队。
    if (status == "rpc_failed") return tr("业务依赖或跨服通知失败"); // 与原视频测试一致，涵盖Redis/MySQL和gRPC故障。
    if (status == "forbidden") return tr("无权与该用户通话"); // 好友权限由服务器判定。
    if (status == "busy") return tr("你或对方正在通话"); // 服务端占用冲突。
    if (status == "media_unavailable") return tr("通话媒体服务未启用或不可用"); // 同一媒体配置服务两种通话。
    if (status == "invalid") return tr("通话请求无效或通话已失效"); // 类型和状态非法均保留协议失败。
    return tr("通话请求失败：%1").arg(status); // 未知真实状态直接呈现，不变成媒体类型错误。
}
// 先作废异步身份，再结束信令和页面；被叫未提交接听时关闭按reject处理。
void VoiceCallController::Finish(const QString &reason, const QString &status, bool notify, bool close) {
    const QString id = _callId; const bool active = _active; // 保存清理前的业务身份。
    const int generation = _generation; // stop必须携带终结前页面代次，脚本才能接受当前页面的清理命令。
    const bool reject = _incoming && !_accepted && !_acceptPending; // 来电等待关闭应明确拒绝。
    if (!_inviteRequest.isEmpty()) _cancelled.insert(_inviteRequest, _login); // 邀请回包迟到也会精确end。
    _active = false; ++_generation; _deadline.stop(); // 立即作废页面与定时器。
    if (!id.isEmpty()) { _retired.insert(id); _earlyReady.remove(id); } // 结束ID不能被晚到accepted/ready重用。
    if (notify && active && !id.isEmpty()) _tcp->SendVoiceCall(reject ? "reject" : "end", 0, id, reason); // 已提交接听的竞态用end收敛双方。
    emit command(QJsonObject{{"action", "stop"}, {"call_id", id}, {"generation", generation}}); // JS校验原页面代次后主动停止轨道，随后销毁页面兜底释放。
    if (_view) { delete _view.data(); _view = nullptr; } // 删除page兜底释放麦克风、摄像头和全部WebRTC连接。
    _session = {}; _callId.clear(); _inviteRequest.clear(); _requests.clear(); _earlyReady.clear(); // 清空仅属于本次通话的关联和提前ready。
    _callType.clear(); // 结束后不向下次邀请泄漏设备范围。
    _accepted = _acceptPending = _peerReady = _pageReady = _readySent = _playing = false; // 下次通话不继承阶段。
    if (_accept) _accept->hide(); if (_hangup) _hangup->setText(tr("关闭")); // 结束窗口只允许关闭。
    if (!status.isEmpty()) SetStatus(status); // 保留具体业务/媒体错误。
    if (close && _window) { // 主动关闭时先断开finished，避免重入Finish。
        auto *window = _window.data(); _window = nullptr; _status = nullptr; _accept = nullptr; _hangup = nullptr; // 释放全部窗口弱引用。
        disconnect(window, nullptr, this, nullptr); window->close(); // WA_DeleteOnClose负责最终删除窗口。
    }
}
