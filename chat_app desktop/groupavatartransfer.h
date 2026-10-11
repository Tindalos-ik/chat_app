// 文件职责：隔离群头像的认证分片上传/下载、精确群资源缓存与登录代次校验；不承接个人或私聊头像。
#ifndef GROUPAVATARTRANSFER_H
#define GROUPAVATARTRANSFER_H

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QtGlobal>

#include "resourceclient.h"
#include "userdata.h"

class QImage;
class QSaveFile;

class GroupAvatarTransfer final : public QObject
{
    Q_OBJECT
public:
    /** 创建群头像传输器并绑定 ResourceClient 的专属群路由回包；parent 为生命周期所有者。 */
    explicit GroupAvatarTransfer(QObject *parent = nullptr);

    /** 设置当前认证账号和 TCP 登录代次；变化时取消旧会话任务与未提交文件。 */
    void SetSession(int accountUid, quint64 loginGeneration);

    /** 请求 group 当前已发布头像；仅 active member 可下载，资源身份绑定 thread/version/resource。 */
    void RequestAvatar(const GroupInfo &group);

    /** 上传并发布 PNG/JPEG 群头像；group 是当前群缓存快照，sourcePath 是本机原图。返回任务 UUID，失败返回空串。 */
    QString StartUpload(const GroupInfo &group, const QString &sourcePath);

    /** 为账号、群、头像版本和净化资源 ID 生成稳定缓存路径；登录代次只约束回调，不参与文件名。 */
    static QString CachePath(int accountUid, const GroupInfo &group);
    /** 返回本账号/群当前 resource/version 的已解码缓存路径；未验证/缺失时返回空串，不要求当前成员在线。 */
    QString VerifiedCachePath(const GroupInfo &group);

    /** 解码并验证头像原图；成功时填入 image/mimeType，失败时填入 error 并返回 false。 */
    static bool InspectImage(const QString &sourcePath, QImage *image,
                             QString *mimeType, QString *error);

signals:
    /** 头像落盘且真实解码成功；UID/登录代次/thread/active/avatarVersion/resourceId 是身份，groupVersion 仅供观察不可作相等门槛。 */
    void sig_avatar_ready(int accountUid, quint64 loginGeneration, qint64 threadId,
                          qint64 groupVersion, qint64 avatarVersion,
                          const QString &resourceId, const QString &cachePath);
    /** 群头像下载失败；taskId/thread/version/resource 标识失败任务，不广播到私聊图片页。 */
    void sig_avatar_download_failed(const QString &taskId, int accountUid,
                                    quint64 loginGeneration, qint64 threadId,
                                    qint64 avatarVersion, const QString &resourceId,
                                    const QString &message);
    /** 上传或 ChatServer 发布失败/成功；expectedAvatarVersion 用于拒绝迟到的上传结果。 */
    void sig_avatar_upload_finished(const QString &taskId, int accountUid,
                                    quint64 loginGeneration, qint64 threadId,
                                    qint64 expectedAvatarVersion, const QString &resourceId,
                                    bool success, const QString &message);

private:
    struct DownloadTask {
        GroupAvatarRoute route;
        GroupInfo group;
        QString cachePath;
        qint64 offset = 0;
        QPointer<QSaveFile> writer;
        bool requestSent = false;
        quint64 loginGeneration = 0;
    };
    // 上传阶段单向推进：等待连接 → 同步 → 分片 → 发布，避免共享连接唤醒重发 SYNC。
    enum class UploadPhase { WaitingConnection, AwaitSync, Uploading, Publishing };
    struct UploadTask {
        GroupAvatarRoute route;
        GroupInfo group;
        QString sourcePath;
        QString md5;
        QString name;
        qint64 totalSize = 0;
        qint64 expectedOffset = 0;
        QString publishRequestId;
        UploadPhase phase = UploadPhase::WaitingConnection;
        bool chunkInFlight = false;
        quint64 loginGeneration = 0;
    };

    /** 检查服务端群身份仍对应当前登录账号缓存；requireManager 为发布时的本地预检。 */
    bool IsCurrentGroup(const GroupInfo &group, bool requireManager) const;
    /** 检查 task 捕获的登录代次以及 UID/thread/avatar resource-version 均仍有效。 */
    bool IsCurrentTask(const GroupAvatarRoute &route, const GroupInfo &group,
                       quint64 loginGeneration) const;
    /** 在缓存失效/登录切换时关闭所有 QSaveFile 并丢弃待处理身份。 */
    void CancelAllTasks();
    /** 建立共享 ResourceServer TCP 连接；连接中时返回 false 并由 connected 回调启动任务。 */
    bool EnsureConnection();
    /** 对所有等待连接的 task 发送第一条精确绑定的资源请求。 */
    void StartWaitingTasks();
    /** 启动下载 task 的下一分片；offset 是服务端已确认的连续字节位置。 */
    void SendDownloadRequest(const QString &taskId, qint64 offset);
    /** 处理与 task UUID/thread/uid/resource 完全相同的下载响应分片。 */
    void HandleDownloadChunk(const GroupAvatarRoute &route, qint64 totalSize,
                              qint64 offset, const QByteArray &data, bool isLast);
    /** 处理首次/续传同步回包；已启动的 chunk 或发布阶段不会被再次同步覆盖。 */
    void HandleUploadSync(const GroupAvatarRoute &route, qint64 confirmedOffset,
                          qint64 totalSize, bool completed);
    /** 处理单个已发送 chunk 的确认；仅确认精确 expectedOffset 后才调度下一片。 */
    void HandleUploadProgress(const GroupAvatarRoute &route, qint64 confirmedOffset,
                              qint64 totalSize, bool completed);
    /** 读取 sourcePath 的 offset 后 1024 字节并发出绑定当前 task 的上传分片。 */
    void SendUploadChunk(const QString &taskId, qint64 offset);
    /** 上传完整后向 ChatServer 发布 task UUID；发布不成功前不改变本地 GroupInfo。 */
    void PublishUpload(const QString &taskId);
    /** 按资源任务 UUID 路由上传/下载业务错误，避免触发通用私聊错误信号。 */
    void HandleResourceError(const GroupAvatarRoute &route, const QString &message);
    /** 关联 UPDATE_GROUP_AVATAR 响应并核实返回的资源/版本确实已发布。 */
    void HandleMutationResult(const QString &requestId, int requestType, int error,
                              const QJsonObject &response);
    /** 原子完成下载并验证 PNG/JPEG 实际解码/尺寸；成功后发出缓存路径。 */
    void FinishDownload(const QString &taskId);
    /** 丢弃下载临时文件并按准确身份通知管理界面。 */
    void FailDownload(const QString &taskId, const QString &message);
    /** 移除上传 task 并按捕获的账号/登录代次/版本发出结果。 */
    void FinishUpload(const QString &taskId, bool success, const QString &message);

    int _accountUid = 0;
    quint64 _loginGeneration = 0;
    bool _connectionRequested = false;
    QHash<QString, DownloadTask> _downloads;
    QHash<QString, QString> _downloadByCachePath;
    QHash<QString, UploadTask> _uploads;
    QHash<QString, QString> _publishRequestToTask;
    QSet<QString> _verifiedCachePaths;
};

#endif // GROUPAVATARTRANSFER_H
