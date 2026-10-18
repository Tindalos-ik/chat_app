// 好友固定文件测试的Qt协调层：TcpMgr持有控制器，控制器协调业务会话和测试窗口。
// TCP负责start/ready/stop；窗口内WebEngine执行文件捕获、ZLM SDP协商和媒体播放。
// QWebChannel连接两层；页面、脚本与唯一配置素材统一映射到chatvideo://test。
#ifndef VIDEOTESTCONTROLLER_H
#define VIDEOTESTCONTROLLER_H

#include <QObject>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QHash>

class TcpMgr;
class QDialog;
class QLabel;
class QWebEngineView;
class QWebEnginePage; // 共享安全页面工厂的返回类型。
class QWebEngineProfile; // 每次媒体会话独立的浏览器网络环境。
class QUrl; // 共享工厂只接收已校验的媒体源地址。

// 每个TcpMgr一个实例；登录代次隔离业务回包，页面代次隔离浏览器异步回调。
class VideoTestController final : public QObject
{
    Q_OBJECT
public:
    // QApplication构造前调用，让Chromium启动时取得chatvideo的安全上下文与跨域规则。
    static void RegisterScheme();
    // profile拥有资源处理器；file为视频唯一素材（语音传空），origin限定SDP服务，入口仅允许内置页面。
    static QWebEnginePage *CreateMediaPage(QWebEngineProfile *profile, QObject *parent,
                                          const QString &entry, const QString &file, const QUrl &origin); // 返回由parent拥有的受限页面。
    // tcp为QObject父对象并提供信令；窗口持有view/page，profile在page之后回收。
    explicit VideoTestController(TcpMgr *tcp);
    // friendUid为接收方UID；异步结果由Handle/mediaEvent更新，活动期间点击只激活窗口。
    void Start(int friendUid);

public slots:
    // 页面异步上报：testId标识业务会话，generation标识页面代次，event/detail描述媒体阶段。
    void mediaEvent(const QString &testId, int generation,
                    const QString &event, const QString &detail);
    // 页面订阅command后回传URL中的会话/代次；校验通过后以command交付configure配置。
    void pageReady(const QString &testId, int generation);

signals:
    // 向已连接的JS订阅者异步投递configure/play/stop；执行结果由页面槽调用回报。
    void command(const QJsonObject &configuration);

private:
    // message为TCP JSON；notification区分服务端通知与请求响应，决定按test_id或request_id关联。
    void Handle(const QJsonObject &message, bool notification);
    // 创建独立窗口并显示status；窗口关闭即结束会话，控制器持弱引用。
    void CreateWindow(const QString &status);
    // 读取固定素材配置，为当前会话创建profile/page/channel；加载完成后等待pageReady握手。
    void ConfigurePage();
    // reason发给对端，status显示在本地；notifyPeer控制stop投递，closeWindow控制窗口关闭。
    void Finish(const QString &reason, const QString &status, bool notifyPeer, bool closeWindow = false);
    // 更新保留窗口的状态文本，不改变业务会话。
    void SetStatus(const QString &text);
    // 将后端状态映射成用户文本，业务故障与离线、媒体错误分别呈现。
    QString StatusText(const QString &status) const;
    // message为服务端会话描述，sender为本端角色；返回校验结果，成功时保存test_id及完整描述。
    bool AcceptSession(const QJsonObject &message, bool sender);

    TcpMgr *_tcp; // QObject父对象；控制器存活期间提供业务信令。
    // QPointer随QObject销毁清空；窗口拥有状态控件和view，view拥有page，page拥有channel。
    QPointer<QDialog> _window;
    QPointer<QLabel> _status;
    QPointer<QWebEngineView> _view;
    QTimer _deadline; // 同一单次定时器按业务应答、媒体建连、ready应答阶段重新计时。
    QJsonObject _session; // 当前服务端媒体描述，交付页面前补充唯一素材URL。
    QString _startRequest; // 关联本次start响应，防止旧请求结果覆盖当前测试。
    QString _readyRequest; // 关联发布就绪确认，防止旧ready响应改变当前状态。
    QString _testId; // 服务端测试ID，关联双方通知和页面事件。
    QSet<QString> _retired; // 记住终结ID，吸收stop早于start到达的通知。
    QHash<QString, quint64> _cancelledStarts; // 已取消start请求及其登录代次，供迟到成功响应补发stop。
    int _generation = 0; // 每次开始/结束递增，淘汰旧页面仍在途的channel回调。
    quint64 _loginGeneration = 0; // 捕获TCP登录代次，区分同一账号断线重登前后的业务结果。
    int _accountUid = 0; // 当前测试所属账号，跨账号登录后过滤旧业务消息。
    bool _active = false;
    bool _sender = false;
    bool _ready = false; // 接收端记住业务ready，页面尚未握手时也可保留播放意图。
    bool _pageReady = false; // 当前页面已订阅command并完成握手。
    bool _readySent = false; // 发布端实际媒体就绪后只提交一次业务ready。
};
#endif
