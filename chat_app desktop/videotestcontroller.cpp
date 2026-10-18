#include "videotestcontroller.h"
#include "tcpmgr.h"
#include "usermgr.h"
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QSettings>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>
#include <QFile>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlRequestInfo>

namespace {
// profile提供本次测试的网络环境，parent负责page生命周期；导航入口固定为内置主页。
class TestPage final : public QWebEnginePage {
public:
    TestPage(QWebEngineProfile *profile, QObject *parent, const QString &entry) // entry由Qt控制器选择，不能由页面导航修改。
        : QWebEnginePage(profile, parent), _entry(entry) {} // 每个页面固定自己的导航入口。
protected:
    // Chromium发起导航时返回是否放行；查询参数携带会话标识，路径仍固定为主页。
    bool acceptNavigationRequest(const QUrl &url, NavigationType, bool) override
    { return url.scheme() == "chatvideo" && url.host() == "test" && url.path() == _entry; } // 查询参数不改变受信入口。
    // 弹窗请求返回空page，保持所有媒体操作处于当前窗口的生命周期内。
    QWebEnginePage *createWindow(WebWindowType) override { return nullptr; }
private:
    QString _entry; // 页面生命周期内固定的视频或语音入口。
};

// 将固定URL映射为qrc内容或配置素材；浏览器始终以chatvideo://test作为页面与文件的同源地址。
class TestFiles final : public QWebEngineUrlSchemeHandler {
public:
    // file为从配置解析出的唯一素材路径；parent为安装此处理器的profile。
    TestFiles(const QString &file, QObject *parent) : QWebEngineUrlSchemeHandler(parent), _file(file) {}
    // job代表一次Chromium资源请求；通过reply异步读取设备，失败通过fail结束该请求。
    void requestStarted(QWebEngineUrlRequestJob *job) override
    {
        const QUrl url = job->requestUrl();
        QString path;
        QByteArray mime;
        if (url.host() != "test" || job->requestMethod() != "GET") {
            job->fail(QWebEngineUrlRequestJob::RequestDenied); return;
        }
        if (url.path() == "/index.html") { path = ":/video_test/index.html"; mime = "text/html"; }
        else if (url.path() == "/media.js") { path = ":/video_test/media.js"; mime = "application/javascript"; }
        else if (url.path() == "/voice.html") { path = ":/voice_call/index.html"; mime = "text/html"; } // 语音仅保留控制器实际使用的唯一入口，qrc路径不作为备用URL。
        else if (url.path() == "/voice_call/media.js") { path = ":/voice_call/media.js"; mime = "application/javascript"; } // 仅放行内置语音脚本。
        else if (url.path() == "/webrtc.js" || url.path() == "/voice_call/webrtc.js") { path = ":/video_test/webrtc.js"; mime = "application/javascript"; } // 两种媒体复用协商实现。
        else if (url.path() == "/qwebchannel.js") { path = ":/qtwebchannel/qwebchannel.js"; mime = "application/javascript"; }
        else if (url.path() == "/fixed.webm") { path = _file; mime = "video/webm"; }
        else { job->fail(QWebEngineUrlRequestJob::UrlNotFound); return; }
        // QFile挂到job下，在Chromium读完或取消请求并销毁job时回收；reply后保留设备供异步读取。
        auto *file = new QFile(path, job);
        if (!file->open(QIODevice::ReadOnly)) { job->fail(QWebEngineUrlRequestJob::UrlNotFound); return; }
        job->reply(mime, file);
    }
private:
    QString _file;
};

// 请求拦截先限制资源边界：内置页面/脚本/素材，以及本次媒体服务的SDP入口。
class TestRequests final : public QWebEngineUrlRequestInterceptor {
public:
    // origin取自已校验的media_url，限定SDP请求的协议、主机和端口；parent为profile。
    TestRequests(const QUrl &origin, QObject *parent)
        : QWebEngineUrlRequestInterceptor(parent), _origin(origin) {}
    // info为当前请求，写入block结果；WebRTC媒体走协商出的传输路径，SDP走此URL白名单。
    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        const QUrl url = info.requestUrl();
        const bool local = url.scheme() == "chatvideo" && url.host() == "test"
            && (url.path() == "/index.html" || url.path() == "/media.js"
                || url.path() == "/qwebchannel.js" || url.path() == "/fixed.webm" // 视频原有本地资源。
                || url.path() == "/voice.html" // 语音唯一固定入口。
                || url.path() == "/voice_call/media.js" || url.path() == "/voice_call/webrtc.js" // 语音内置脚本。
                || url.path() == "/webrtc.js"); // 视频共用协商脚本。
        const bool api = url.scheme() == _origin.scheme() && url.host() == _origin.host()
            && url.port() == _origin.port() && url.path() == "/index/api/webrtc";
        info.block(!(local || api));
    }
private:
    QUrl _origin;
};
}

// 在应用初始化前声明scheme；固定host与默认端口形成稳定origin，供媒体页与跨域SDP请求使用。
void VideoTestController::RegisterScheme()
{
    QWebEngineUrlScheme scheme("chatvideo");
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::HostAndPort);
    scheme.setDefaultPort(443);
    // SecureScheme使内置页成为安全上下文，CorsEnabled让Chromium按CORS规则处理跨域访问。
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::CorsEnabled);
    QWebEngineUrlScheme::registerScheme(scheme);
}

// 所有媒体窗口复用同一导航与请求白名单；profile负责资源设备和拦截器的销毁。
QWebEnginePage *VideoTestController::CreateMediaPage(QWebEngineProfile *profile, QObject *parent,
                                                    const QString &entry, const QString &file, const QUrl &origin) // 参数均由已校验的Qt会话提供。
{
    profile->installUrlSchemeHandler("chatvideo", new TestFiles(file, profile)); // 共用固定资源映射，不能浏览任意本地文件。
    profile->setUrlRequestInterceptor(new TestRequests(origin, profile)); // 只允许内置资源及当前媒体服务SDP接口。
    return new TestPage(profile, parent, entry); // 页面所有权交给调用方的view。
}

// tcp拥有控制器；构造时订阅全局测试通知和登录结束事件，好友窗口尚未打开也可接收start。
VideoTestController::VideoTestController(TcpMgr *tcp) : QObject(tcp), _tcp(tcp)
{
    _deadline.setSingleShot(true);
    // 阶段超时、连接重置和账号下线统一进入Finish，保持业务状态与浏览器资源同时收敛。
    connect(&_deadline, &QTimer::timeout, this, [this] {
        Finish("media_error", tr("测试超时：未完成当前业务/媒体阶段"), true);
    });
    connect(tcp, &TcpMgr::sig_video_test, this, &VideoTestController::Handle);
    connect(tcp, &TcpMgr::sig_video_test_reset, this, [this] {
        Finish("logout", tr("连接已关闭"), true, true);
    });
    connect(tcp, &TcpMgr::sig_off_line, this, [this] {
        Finish("logout", tr("账号已下线"), true, true);
    });
}

// friendUid指定接收方；发送start后由Handle推进会话，服务端notified应答到达后才启动媒体页。
void VideoTestController::Start(int friendUid)
{
    if (_active) { if (_window) { _window->show(); _window->raise(); _window->activateWindow(); } return; }
    const auto account = UserMgr::GetInstance()->GetUserInfo();
    if (!account || friendUid <= 0 || friendUid == account->_uid) return;
    Finish("closed", {}, false, true);
    _accountUid = account->_uid;
    // 同时记录账号与TCP登录代次，后续回包须属于发起测试时的登录连接。
    _loginGeneration = _tcp->LoginGeneration();
    _sender = true;
    _active = true;
    ++_generation;
    CreateWindow(tr("正在通知好友…"));
    _startRequest = _tcp->SendVideoTest("start", friendUid);
    if (_startRequest.isEmpty()) { Finish("media_error", tr("聊天服务器未连接"), false); return; }
    _deadline.start(12000);
}

// status为初始提示；dialog顶层显示并在关闭时自销毁，控件/view随窗口释放。
void VideoTestController::CreateWindow(const QString &status)
{
    auto *dialog = new QDialog;
    _window = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("test · 好友单向视频"));
    dialog->resize(760, 540);
    auto *layout = new QVBoxLayout(dialog);
    _status = new QLabel(status, dialog);
    _status->setWordWrap(true);
    layout->addWidget(_status);
    // 用户关窗触发统一结束流程；主动关闭时Finish先解除此连接，避免重复结束。
    connect(dialog, &QDialog::finished, this, [this] {
        Finish("closed", tr("测试已关闭"), true, true);
    });
    dialog->show();
}

// message为服务端媒体描述，sender决定核对fromuid还是touid；返回true时保存会话供页面配置。
bool VideoTestController::AcceptSession(const QJsonObject &message, bool sender)
{
    const int from = message.value("fromuid").toInt(), to = message.value("touid").toInt();
    const QUrl origin(message.value("media_url").toString());
    // media_url只接受HTTP(S)源地址，页面再固定拼接SDP路径；会话参与者必须包含当前角色账号。
    const QString id = message.value("test_id").toString();
    const QString stream = message.value("stream_id").toString();
    if ((sender ? from : to) != _accountUid || from <= 0 || to <= 0 || from == to
        || id.isEmpty() || id.size() > 128 || stream.isEmpty() || stream.size() > 128
        || message.value("app").toString() != "video_test"
        || (origin.scheme() != "http" && origin.scheme() != "https") || origin.host().isEmpty()
        || !origin.userInfo().isEmpty() || !origin.query().isEmpty() || !origin.fragment().isEmpty()
        || (!origin.path().isEmpty() && origin.path() != "/")) return false;
    _testId = id;
    _session = message;
    return true;
}

// message来自TCP分发，notification区分主动通知和请求应答；业务结果在这里转成媒体启动/结束动作。
void VideoTestController::Handle(const QJsonObject &message, bool notification)
{
    const auto account = UserMgr::GetInstance()->GetUserInfo();
    if (!account) return;
    const QString op = message.value("op").toString();
    const QString id = message.value("test_id").toString();
    const QString status = message.value("status").toString();
    // 即使stop先于异步start到达，也记住已结束测试，不能让迟到start复活媒体窗口。
    if (notification && op == "stop" && !id.isEmpty() && (!_active || id != _testId)) {
        _retired.insert(id);
        if (_retired.size() > 128) _retired.erase(_retired.begin());
        return;
    }
    // start尚未回包时也允许关闭：迟到的成功响应必须补发stop，不留下服务端占位。
    const QString requestId = message.value("request_id").toString();
    if (!notification && op == "start" && _cancelledStarts.contains(requestId)) {
        const quint64 login = _cancelledStarts.take(requestId);
        if (login == _tcp->LoginGeneration() && status == "notified" && !id.isEmpty())
            _tcp->SendVideoTest("stop", 0, id, "closed");
        return;
    }
    if (notification && op == "start") {
        if (_retired.contains(id)) return;
        if (_active) { if (id != _testId) _tcp->SendVideoTest("stop", 0, id, "busy"); return; }
        Finish("closed", {}, false, true);
        _accountUid = account->_uid;
        _loginGeneration = _tcp->LoginGeneration();
        _sender = false;
        if (!AcceptSession(message, false)) {
            if (!id.isEmpty()) _tcp->SendVideoTest("stop", 0, id, "media_error");
            CreateWindow(tr("服务端媒体描述无效，测试已拒绝"));
            return;
        }
        _active = true;
        ++_generation;
        CreateWindow(tr("好友已发起test，等待发布媒体就绪…"));
        _deadline.start(45000);
        ConfigurePage();
        return;
    }
    // 活动会话还须匹配账号和登录代次；request_id关联操作应答，test_id关联双方会话通知。
    if (!_active || _accountUid != account->_uid || _loginGeneration != _tcp->LoginGeneration()) return;
    if (!notification && message.value("request_id").toString() == _startRequest && op == "start") {
        _startRequest.clear();
        if (message.value("error").toInt(-1) != 0 || status != "notified") {
            Finish("media_error", StatusText(status), false); return;
        }
        if (!AcceptSession(message, true)) {
            if (!id.isEmpty()) _tcp->SendVideoTest("stop", 0, id, "media_error");
            Finish("media_error", tr("服务端媒体描述无效"), true); return;
        }
        // notified表示好友TCP通知已入队，此时才创建发布页；实际媒体就绪由页面另行回报。
        SetStatus(tr("好友通知已排队；正在打开固定文件并发布媒体…"));
        _deadline.start(45000);
        ConfigurePage();
        return;
    }
    if (_testId.isEmpty() || id != _testId) return;
    if (notification && op == "stop") {
        Finish("closed", tr("对端已停止测试：%1").arg(message.value("reason").toString().left(64)), false);
    } else if (notification && op == "ready" && !_sender && !_ready) {
        // ready可早于页面channel握手到达：先锁存，已握手则立即投递play，否则随configure交付。
        _ready = true;
        SetStatus(tr("发布端已有实际视频输出；正在向ZLM拉流…"));
        if (_pageReady) emit command(QJsonObject{{"action", "play"}, {"test_id", _testId}, {"generation", _generation}});
    } else if (!notification && op == "ready" && message.value("request_id").toString() == _readyRequest) {
        _readyRequest.clear();
        if (message.value("error").toInt(-1) != 0 || status != "ready")
            Finish("media_error", StatusText(status), true);
        else {
            // ready应答确认业务通知阶段；接收方实际播放由其页面playing事件独立确认。
            _deadline.stop();
            SetStatus(tr("媒体已发布；已通知好友自动播放（不代表对端已播放）"));
        }
    }
}

// 按当前_session创建媒体运行环境；页面加载与channel初始化异步完成，由pageReady交付配置。
void VideoTestController::ConfigurePage()
{
    if (!_window || _view) return;
    QSettings settings(QDir(QCoreApplication::applicationDirPath()).filePath("config.ini"), QSettings::IniFormat);
    const QString configured = settings.value("VideoTest/file").toString();
    const QString file = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(configured);
    if (_sender && (configured.isEmpty() || !QFileInfo(file).isFile())) {
        Finish("media_error", tr("固定测试文件不存在，请检查config.ini的VideoTest/file"), true); return;
    }
    // 每次测试独立使用无存储名称的off-the-record profile，隔离缓存与网络策略；窗口拥有view/profile。
    _view = new QWebEngineView(_window);
    auto *profile = new QWebEngineProfile(_window);
    // 视频与语音走同一个页面工厂，保留原视频素材与独立profile生命周期。
    auto *page = CreateMediaPage(profile, _view, "/index.html", file, QUrl(_session.value("media_url").toString()));
    // 页面显式删除时也安排profile回收；窗口作为父对象兜底管理profile生命周期。
    connect(page, &QObject::destroyed, profile, &QObject::deleteLater);
    _view->setPage(page);
    auto *settingsPage = page->settings();
    // 自动订阅由业务ready驱动，放开播放手势要求；远端访问供SDP使用，实际URL再由拦截器收紧。
    settingsPage->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
    settingsPage->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    // 固定文件通过媒体元素捕获轨道，页面的摄像头/麦克风等权限请求统一拒绝。
    connect(page, &QWebEnginePage::featurePermissionRequested, page,
            [page](const QUrl &url, QWebEnginePage::Feature feature) {
        page->setFeaturePermission(url, feature, QWebEnginePage::PermissionDeniedByUser);
    });
    // channel由page拥有；注册的是既有控制器，页面只通过其槽回报握手和媒体事件。
    auto *channel = new QWebChannel(page);
    channel->registerObject("videoTest", this);
    page->setWebChannel(channel);
    _session.insert("file_url", "chatvideo://test/fixed.webm");
    _window->layout()->addWidget(_view);
    // 首屏URL先携带会话与页面代次，让JS能在收到完整配置前发送可校验的pageReady。
    QUrl url("chatvideo://test/index.html");
    QUrlQuery query;
    query.addQueryItem("test_id", _testId);
    query.addQueryItem("generation", QString::number(_generation));
    url.setQuery(query);
    _view->load(url);
}

// id/generation来自页面URL；握手匹配当前会话且仅处理一次，异步发送完整configure命令。
void VideoTestController::pageReady(const QString &id, int generation)
{
    if (!_active || id != _testId || generation != _generation || _pageReady) return;
    _pageReady = true;
    QJsonObject config = _session;
    config.insert("action", "configure");
    config.insert("generation", _generation);
    config.insert("sender", _sender);
    // 带上先前锁存的业务ready；接收页据此决定配置后等待还是直接拉流，覆盖ready先到的时序。
    config.insert("ready", _ready);
    emit command(config);
}

// id/generation过滤旧页回报；event/detail为JS判定的阶段和说明，驱动业务ready、提示或终止。
void VideoTestController::mediaEvent(const QString &id, int generation, const QString &event, const QString &detail)
{
    if (!_active || id != _testId || generation != _generation) return;
    if (event == "error") Finish("media_error", tr("媒体失败：%1").arg(detail.left(600)), true);
    else if (event == "ended") Finish("ended", tr("固定文件播放完毕"), true);
    else if (event == "ready" && _sender && !_readySent) {
        // JS在发布连接成功且outbound视频字节增长后报告ready，再由TCP通知好友可开始订阅。
        _readySent = true;
        _deadline.stop();
        _readyRequest = _tcp->SendVideoTest("ready", 0, _testId);
        if (_readyRequest.isEmpty()) Finish("media_error", tr("媒体已连接，但业务连接不可用"), true);
        else { SetStatus(tr("实际视频字节已发出；正在通知好友播放…")); _deadline.start(10000); }
    } else if (event == "playing" && !_sender) {
        // playing由接收页按实际媒体数据判定，至此停止接收端建连期限。
        _deadline.stop();
        SetStatus(tr("正在播放：已收到实际视频帧和音频媒体数据"));
    } else if (event == "stage") SetStatus(detail.left(600));
}

// text写入仍存活窗口的状态控件，结束后保留的窗口也可展示失败原因。
void VideoTestController::SetStatus(const QString &text)
{
    if (_status) _status->setText(text);
}

// status为业务状态，返回对应用户提示；offline/unreachable与业务依赖、媒体故障分开呈现。
QString VideoTestController::StatusText(const QString &status) const
{
    if (status == "offline" || status == "unreachable") return tr("对方不在线");
    if (status == "rpc_failed") return tr("业务依赖或跨服通知失败");
    if (status == "forbidden") return tr("无权向该用户发起测试");
    if (status == "invalid") return tr("测试请求无效");
    if (status == "busy") return tr("当前用户或好友已有活动测试");
    if (status == "media_unavailable") return tr("媒体服务未启用或不可用");
    return tr("测试失败：%1").arg(status);
}

// reason用于对端stop，status用于本地提示；notifyPeer控制通知，closeWindow决定关闭还是保留提示。
void VideoTestController::Finish(const QString &reason, const QString &status, bool notifyPeer, bool closeWindow)
{
    const QString id = _testId;
    if (!_startRequest.isEmpty()) {
        _cancelledStarts.insert(_startRequest, _loginGeneration);
        if (_cancelledStarts.size() > 128) _cancelledStarts.erase(_cancelledStarts.begin());
    }
    const bool wasActive = _active;
    // 先作废活动状态和页面代次，再清理资源；在途JS回报进入槽时会被会话/代次校验淘汰。
    _active = false;
    ++_generation;
    _deadline.stop();
    if (!id.isEmpty()) _retired.insert(id);
    if (_retired.size() > 128) _retired.erase(_retired.begin());
    if (notifyPeer && wasActive && !id.isEmpty()) _tcp->SendVideoTest("stop", 0, id, reason);
    // stop经channel异步投递，随后立即销毁view/page；页面销毁会释放浏览器上下文及其媒体资源。
    // JS若收到命令则执行自身cleanup，资源回收同时依托页面销毁的生命周期。
    emit command(QJsonObject{{"action", "stop"}, {"test_id", id}});
    if (_view) { delete _view.data(); _view = nullptr; }
    _session = {};
    _testId.clear(); _startRequest.clear(); _readyRequest.clear();
    _ready = _pageReady = _readySent = false;
    if (!status.isEmpty()) SetStatus(status);
    if (closeWindow && _window) {
        // 先清弱引用并断开finished回调，再关闭窗口，避免关窗信号再次进入Finish。
        auto *window = _window.data();
        _window = nullptr; _status = nullptr;
        disconnect(window, nullptr, this, nullptr);
        window->close();
    }
}
