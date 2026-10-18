// 好友双向语音协调器：ChatServer经TcpMgr交付业务状态，本类管理Qt邀请/通话窗口并经QWebChannel驱动受限WebEngine页面。
// ChatServer裁决邀请与接听；Qt仅在accepted后创建页面；JS拥有麦克风轨道、推拉流双PC、SDP请求与实际播放统计。
#ifndef VOICECALLCONTROLLER_H // 防止重复声明语音控制器。
#define VOICECALLCONTROLLER_H // 本文件的唯一头文件保护。
#include <QObject> // 提供信号槽和父子生命周期。
#include <QJsonObject> // 承载服务器会话与页面命令。
#include <QPointer> // 避免窗口销毁后的悬空访问。
#include <QTimer> // 限定邀请和媒体建连等待时间。
#include <QSet> // 终结会话不可被迟到消息复活。
#include <QHash> // 保存已取消邀请的登录代次。
class TcpMgr; // 提供已登录的信令传输。
class QDialog; // 承载来电或通话窗口。
class QLabel; // 显示真实业务与媒体状态。
class QPushButton; // 提供接听、拒绝和挂断操作。
class QWebEngineView; // 仅accepted阶段拥有媒体页面。
// 每个TcpMgr拥有一个控制器；会话ID和页面代次双重隔离异步结果。
class VoiceCallController final : public QObject {
    Q_OBJECT // 向WebChannel公开槽和命令信号。
public:
    // 下列void接口无同步业务返回值：信令结果由Handle异步消费，媒体结果由mediaEvent和窗口状态反馈。
    explicit VoiceCallController(TcpMgr *tcp); // tcp为父对象和唯一信令来源。
    void Start(int friendUid); // friendUid为好友UID；异步结果显示在通话窗口。
public slots:
    void pageReady(const QString &id, int generation); // id为call_id，generation为页面代次；页面订阅后握手，校验成功异步发送configure。
    void mediaEvent(const QString &id, int generation, const QString &event, const QString &detail); // id/generation关联当前页，event为ready/playing/stage/error，detail为阶段或错误说明；异步更新信令与界面。
signals:
    void command(const QJsonObject &configuration); // configuration含action、call_id、generation及动作参数；异步发送configure/play/stop，结果由mediaEvent回报。
private:
    void Handle(const QJsonObject &message, bool notification); // message为TCP JSON，notification区分推送与请求回包；按UUID/会话关联后推进阶段，无同步返回。
    void CreateWindow(bool incoming); // incoming决定显示接听/拒绝还是取消按钮。
    void ConfigurePage(); // 仅accepted后创建WebEngine并安装受限麦克风权限。
    void Finish(const QString &reason, const QString &status, bool notify, bool close = false); // reason为对端结束原因，status为本地提示，notify决定发送reject/end，close决定是否关闭提示窗口；同步停页，信令异步完成。
    void SetStatus(const QString &text); // 仅更新仍存活窗口的提示。
    QString PeerName(int uid) const; // uid为对端账号；返回好友昵称/名称和UID，资料缺失仍返回UID。
    QString StatusText(const QString &status) const; // status为服务端业务状态；返回对应中文错误原因，不修改通话阶段。
    TcpMgr *_tcp; // 父对象持有，贯穿控制器生命周期。
    QPointer<QDialog> _window; // 当前或已结束通话的可关闭提示窗口。
    QPointer<QLabel> _status; // 窗口拥有的状态标签。
    QPointer<QPushButton> _accept; // 仅来电等待阶段可接听。
    QPointer<QPushButton> _hangup; // 根据阶段执行拒绝、取消或挂断。
    QPointer<QWebEngineView> _view; // accepted阶段拥有，结束立即删除。
    QTimer _deadline; // 单次邀请/媒体阶段的超时器。
    QJsonObject _session; // 已校验的accepted配置，供页面握手后发送。
    QString _callId; // 当前服务器分配的会话ID。
    QString _inviteRequest; // 主叫邀请响应的UUID。
    QHash<QString, QString> _requests; // 当前操作UUID到操作名的关联。
    QHash<QString, quint64> _cancelled; // 已取消邀请UUID到原登录代次的关联；登录reset时清除，供迟到成功补end。
    QSet<QString> _retired; // 本登录已终结ID隔离迟到accepted/ready；登录reset时清除。
    QSet<QString> _earlyReady; // accepted之前到达的对端ready会话ID。
    quint64 _login = 0; // 当前会话所属TCP登录代次。
    int _self = 0; // 当前会话所属账号UID。
    int _peer = 0; // 邀请界面显示的好友UID。
    int _generation = 0; // 每次结束递增，淘汰旧页面回调。
    bool _active = false; // 有尚未终结的业务会话。
    bool _incoming = false; // 本端是否为被叫。
    bool _accepted = false; // 服务端已确认接听，才允许采集。
    bool _acceptPending = false; // 防止重复点击接听和错误拒绝已提交的接听。
    bool _peerReady = false; // 对端发布就绪，跨页面握手锁存。
    bool _pageReady = false; // 当前页面已订阅命令。
    bool _readySent = false; // 实际发出音频后仅发送一次ready。
    bool _playing = false; // 已收到实际音频，不被后续accepted/stage覆盖。
};
#endif // VOICECALLCONTROLLER_H
