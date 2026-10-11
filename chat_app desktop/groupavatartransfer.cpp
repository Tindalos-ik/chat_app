// 文件职责：执行群头像的 UID/thread/resource/task 绑定分片传输、原子缓存和发布回执校验。
#include "groupavatartransfer.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>
#include <QTimer>

#include "global.h"
#include "localchatstoragemgr.h"
#include "tcpmgr.h"
#include "usermgr.h"

namespace {
constexpr qint64 kMaxAvatarBytes = 2 * 1024 * 1024;
constexpr qint32 kAvatarChunkBytes = 1024;

QString FreshTaskId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool SameRoute(const GroupAvatarRoute &left, const GroupAvatarRoute &right)
{
    return left.purpose == QStringLiteral("group_avatar")
        && right.purpose == QStringLiteral("group_avatar")
        && left.taskId == right.taskId && left.threadId == right.threadId
        && left.uid == right.uid && left.resourceId == right.resourceId;
}

bool IsSupportedImageMime(const QString &mime)
{
    return mime == QStringLiteral("image/png") || mime == QStringLiteral("image/jpeg");
}

QString MimeForImageFormat(const QByteArray &format)
{
    const QByteArray normalized = format.toLower();
    if (normalized == "png") return QStringLiteral("image/png");
    if (normalized == "jpg" || normalized == "jpeg") return QStringLiteral("image/jpeg");
    return {};
}

bool IsVerifiedCacheImage(const QString &path, const GroupInfo &group)
{
    const QFileInfo info(path);
    if (!info.isFile() || info.size() != group.avatarSize
        || info.size() <= 0 || info.size() > kMaxAvatarBytes
        || group.avatarWidth <= 0 || group.avatarHeight <= 0
        || group.avatarWidth > 1024 || group.avatarHeight > 1024)
        return false;
    QImageReader reader(path);
    reader.setAutoTransform(false);
    const QString mime = MimeForImageFormat(reader.format());
    const QSize dimensions = reader.size();
    if (!IsSupportedImageMime(mime) || mime != group.avatarMimeType
        || dimensions.width() != group.avatarWidth || dimensions.height() != group.avatarHeight)
        return false;
    const QImage decoded = reader.read();
    return !decoded.isNull() && decoded.size() == dimensions;
}

QString ResourceCacheComponent(const QString &resourceId)
{
    if (resourceId.isEmpty() || resourceId.size() > 512) return {};
    QString safe;
    safe.reserve(qMin(resourceId.size(), 96));
    for (const QChar ch : resourceId) {
        const ushort c = ch.unicode();
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (safe.size() < 96) safe.append(allowed ? ch : QLatin1Char('_'));
    }
    const QByteArray digest = QCryptographicHash::hash(resourceId.toUtf8(),
                                                        QCryptographicHash::Sha256).toHex().left(20);
    return safe + QLatin1Char('_') + QString::fromLatin1(digest);
}

LocalChatThread FindCachedGroup(qint64 threadId)
{
    for (const LocalChatThread &thread : LocalChatStorageMgr::GetInstance()->LoadAllThreads()) {
        if (thread.threadType == QStringLiteral("group") && thread.threadId == threadId)
            return thread;
    }
    return {};
}
}

GroupAvatarTransfer::GroupAvatarTransfer(QObject *parent)
    : QObject(parent)
{
    auto *resource = ResourceClient::GetInstance().get();
    connect(resource, &ResourceClient::sig_group_avatar_sync,
            this, &GroupAvatarTransfer::HandleUploadSync);
    connect(resource, &ResourceClient::sig_group_avatar_upload_progress,
            this, &GroupAvatarTransfer::HandleUploadProgress);
    connect(resource, &ResourceClient::sig_group_avatar_download_chunk,
            this, &GroupAvatarTransfer::HandleDownloadChunk);
    connect(resource, &ResourceClient::sig_group_avatar_error,
            this, &GroupAvatarTransfer::HandleResourceError);
    connect(resource, &ResourceClient::sig_con_success, this, [this](bool success) {
        _connectionRequested = false;
        if (success) {
            StartWaitingTasks();
            return;
        }
        const auto downloads = _downloads.keys();
        for (const QString &id : downloads) FailDownload(id, tr("资源服务器连接失败"));
        const auto uploads = _uploads.keys();
        for (const QString &id : uploads) FinishUpload(id, false, tr("资源服务器连接失败"));
    });
    connect(resource, &ResourceClient::sig_net_error, this, [this](const QString &message) {
        _connectionRequested = false;
        const auto downloads = _downloads.keys();
        for (const QString &id : downloads) FailDownload(id, message);
        const auto uploads = _uploads.keys();
        for (const QString &id : uploads) FinishUpload(id, false, message);
    });
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_group_mutation_result,
            this, &GroupAvatarTransfer::HandleMutationResult);
}

void GroupAvatarTransfer::SetSession(int accountUid, quint64 loginGeneration)
{
    if (_accountUid == accountUid && _loginGeneration == loginGeneration) return;
    CancelAllTasks();
    _accountUid = accountUid;
    _loginGeneration = loginGeneration;
    _verifiedCachePaths.clear();
}

void GroupAvatarTransfer::CancelAllTasks()
{
    for (DownloadTask &task : _downloads) {
        if (!task.writer) continue;
        task.writer->cancelWriting();
        delete task.writer.data();
    }
    _downloads.clear();
    _downloadByCachePath.clear();
    _uploads.clear();
    _publishRequestToTask.clear();
}

bool GroupAvatarTransfer::IsCurrentGroup(const GroupInfo &group, bool requireManager) const
{
    const auto user = UserMgr::GetInstance()->GetUserInfo();
    const auto storage = LocalChatStorageMgr::GetInstance();
    if (!user || user->_uid <= 0 || user->_uid != _accountUid
        || TcpMgr::GetInstance()->LoginGeneration() != _loginGeneration
        || !storage->IsReady() || storage->CurrentUid() != _accountUid
        || group.threadId <= 0 || !group.active || !group.member
        || group.avatarVersion < 0 || group.membershipVersion <= 0) return false;
    const LocalChatThread cached = FindCachedGroup(group.threadId);
    if (cached.threadId <= 0 || !cached.active || cached.role < 0 || cached.role > 2
        || cached.avatarVersion != group.avatarVersion
        || cached.avatarResourceId != group.avatarResourceId) return false;
    if (requireManager && cached.role < 1) return false;
    return true;
}

bool GroupAvatarTransfer::IsCurrentTask(const GroupAvatarRoute &route, const GroupInfo &group,
                                       quint64 loginGeneration) const
{
    return route.purpose == QStringLiteral("group_avatar") && !route.taskId.isEmpty()
        && route.uid == _accountUid && route.threadId == group.threadId
        && loginGeneration == _loginGeneration
        && TcpMgr::GetInstance()->LoginGeneration() == loginGeneration
        && IsCurrentGroup(group, false);
}

QString GroupAvatarTransfer::CachePath(int accountUid, const GroupInfo &group)
{
    const QString resource = ResourceCacheComponent(group.avatarResourceId);
    if (accountUid <= 0 || group.threadId <= 0 || group.avatarVersion <= 0
        || resource.isEmpty() || !IsSupportedImageMime(group.avatarMimeType)) return {};
    const QString ext = group.avatarMimeType == QStringLiteral("image/png")
        ? QStringLiteral("png") : QStringLiteral("jpg");
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty()) return {};
    return QDir::cleanPath(QDir(root).filePath(
        QStringLiteral("group-avatar-cache/%1/%2/%3/%4.%5")
            .arg(accountUid).arg(group.threadId).arg(group.avatarVersion).arg(resource, ext)));
}
QString GroupAvatarTransfer::VerifiedCachePath(const GroupInfo &group)
{
    const auto user = UserMgr::GetInstance()->GetUserInfo();
    const auto storage = LocalChatStorageMgr::GetInstance();
    if (!user || user->_uid != _accountUid || _accountUid <= 0
        || TcpMgr::GetInstance()->LoginGeneration() != _loginGeneration
        || !storage->IsReady() || storage->CurrentUid() != _accountUid)
        return {};
    const LocalChatThread cached = FindCachedGroup(group.threadId);
    if (cached.threadId <= 0 || cached.avatarVersion != group.avatarVersion
        || cached.avatarResourceId != group.avatarResourceId)
        return {};
    const QString path = CachePath(_accountUid, group);
    if (path.isEmpty()) return {};
    if (_verifiedCachePaths.contains(path) && QFileInfo::exists(path)) return path;
    _verifiedCachePaths.remove(path);
    if (!QFileInfo::exists(path)) return {};
    if (!IsVerifiedCacheImage(path, group)) {
        QFile::remove(path);
        return {};
    }
    _verifiedCachePaths.insert(path);
    return path;
}


bool GroupAvatarTransfer::InspectImage(const QString &sourcePath, QImage *image,
                                       QString *mimeType, QString *error)
{
    auto reject = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (!image || !mimeType || sourcePath.isEmpty())
        return reject(QObject::tr("请选择有效的 PNG 或 JPEG 图片"));
    QFileInfo info(sourcePath);
    if (!info.exists() || !info.isFile() || info.size() <= 0 || info.size() > kMaxAvatarBytes)
        return reject(QObject::tr("头像文件必须为 1 字节至 2 MiB"));
    QImageReader reader(sourcePath);
    reader.setAutoTransform(false);
    const QString mime = MimeForImageFormat(reader.format());
    const QSize dimensions = reader.size();
    if (!IsSupportedImageMime(mime) || dimensions.width() <= 0 || dimensions.height() <= 0
        || dimensions.width() > 1024 || dimensions.height() > 1024)
        return reject(QObject::tr("头像必须是尺寸不超过 1024×1024 的 PNG 或 JPEG"));
    const QImage decoded = reader.read();
    if (decoded.isNull() || decoded.size() != dimensions)
        return reject(QObject::tr("头像图片内容损坏或无法解码"));
    if (image) *image = decoded;
    if (mimeType) *mimeType = mime;
    if (error) error->clear();
    return true;
}

void GroupAvatarTransfer::RequestAvatar(const GroupInfo &group)
{
    if (!IsCurrentGroup(group, false) || group.avatarResourceId.isEmpty()
        || group.avatarVersion <= 0 || group.avatarSize <= 0
        || group.avatarSize > kMaxAvatarBytes || !IsSupportedImageMime(group.avatarMimeType)
        || group.avatarWidth <= 0 || group.avatarHeight <= 0
        || group.avatarWidth > 1024 || group.avatarHeight > 1024) return;

    const QString path = CachePath(_accountUid, group);
    if (path.isEmpty()) return;
    const QString cachedPath = VerifiedCachePath(group);
    if (!cachedPath.isEmpty()) {
        emit sig_avatar_ready(_accountUid, _loginGeneration, group.threadId,
                              group.version, group.avatarVersion,
                              group.avatarResourceId, cachedPath);
        return;
    }
    if (_downloadByCachePath.contains(path)) return;

    const QString taskId = FreshTaskId();
    DownloadTask task;
    task.route = {QStringLiteral("group_avatar"), taskId, group.threadId,
                  _accountUid, group.avatarResourceId};
    task.group = group;
    task.loginGeneration = _loginGeneration;
    task.cachePath = path;
    const QFileInfo target(path);
    if (!QDir().mkpath(target.absolutePath())) {
        emit sig_avatar_download_failed(taskId, _accountUid, _loginGeneration,
            group.threadId, group.avatarVersion, group.avatarResourceId,
            tr("无法创建群头像缓存目录"));
        return;
    }
    auto *writer = new QSaveFile(path, this);
    if (!writer->open(QIODevice::WriteOnly)) {
        delete writer;
        emit sig_avatar_download_failed(taskId, _accountUid, _loginGeneration,
            group.threadId, group.avatarVersion, group.avatarResourceId,
            tr("无法创建群头像临时文件"));
        return;
    }
    task.writer = writer;
    _downloads.insert(taskId, task);
    _downloadByCachePath.insert(path, taskId);
    if (EnsureConnection()) SendDownloadRequest(taskId, 0);
}

QString GroupAvatarTransfer::StartUpload(const GroupInfo &group, const QString &sourcePath)
{
    if (!IsCurrentGroup(group, true)) return {};
    QImage decoded;
    QString mime, error;
    if (!InspectImage(sourcePath, &decoded, &mime, &error)) return {};
    QFile file(sourcePath);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash md5(QCryptographicHash::Md5);
    while (!file.atEnd()) {
        const QByteArray bytes = file.read(64 * 1024);
        if (bytes.isEmpty() && file.error() != QFile::NoError) return {};
        md5.addData(bytes);
    }
    const qint64 size = file.size();
    if (size <= 0 || size > kMaxAvatarBytes) return {};

    const QString taskId = FreshTaskId();
    UploadTask task;
    task.route = {QStringLiteral("group_avatar"), taskId, group.threadId,
                  _accountUid, taskId};
    task.group = group;
    task.sourcePath = sourcePath;
    task.md5 = QString::fromLatin1(md5.result().toHex());
    task.name = QFileInfo(sourcePath).fileName();
    task.totalSize = size;
    task.loginGeneration = _loginGeneration;
    _uploads.insert(taskId, task);
    // 返回 task UUID 后再触发首个请求，保证 UI 已记录身份后不会漏掉同步错误回包。
    QTimer::singleShot(0, this, [this, taskId] {
        if (!_uploads.contains(taskId)) return;
        if (EnsureConnection()) StartWaitingTasks();
    });
    return taskId;
}

bool GroupAvatarTransfer::EnsureConnection()
{
    auto *resource = ResourceClient::GetInstance().get();
    if (resource->IsConnected()) return true;
    if (_connectionRequested) return false;
    if (resource->IsConnecting()) {
        _connectionRequested = true;
        return false;
    }
    const QString configPath = QCoreApplication::applicationDirPath() + QStringLiteral("/config.ini");
    QSettings settings(configPath, QSettings::IniFormat);
    const QString host = settings.value(QStringLiteral("ResourceServer/host")).toString();
    const QString port = settings.value(QStringLiteral("ResourceServer/port")).toString();
    if (host.isEmpty() || port.toUShort() == 0) {
        const auto downloads = _downloads.keys();
        for (const QString &id : downloads) FailDownload(id, tr("ResourceServer 地址未配置"));
        const auto uploads = _uploads.keys();
        for (const QString &id : uploads) FinishUpload(id, false, tr("ResourceServer 地址未配置"));
        return false;
    }
    _connectionRequested = true;
    resource->slot_tcp_connect(host, port);
    return false;
}

void GroupAvatarTransfer::StartWaitingTasks()
{
    if (!ResourceClient::GetInstance()->IsConnected()) return;
    const auto downloadIds = _downloads.keys();
    for (const QString &id : downloadIds) {
        auto it = _downloads.find(id);
        if (it == _downloads.end() || it->requestSent) continue;
        if (!IsCurrentTask(it->route, it->group, it->loginGeneration)) {
            FailDownload(id, tr("群头像所属账号或成员状态已变化"));
            continue;
        }
        SendDownloadRequest(id, it->offset);
    }
    const auto uploadIds = _uploads.keys();
    for (const QString &id : uploadIds) {
        auto it = _uploads.find(id);
        if (it == _uploads.end() || it->phase != UploadPhase::WaitingConnection) continue;
        if (!IsCurrentTask(it->route, it->group, it->loginGeneration) || !IsCurrentGroup(it->group, true)) {
            FinishUpload(id, false, tr("群管理权限或头像版本已变化"));
            continue;
        }
        it->phase = UploadPhase::AwaitSync;
        ResourceClient::GetInstance()->syncGroupAvatarUpload(
            it->route.taskId, it->route.threadId, it->route.uid, it->md5,
            it->name, it->totalSize, kAvatarChunkBytes);
    }
}

void GroupAvatarTransfer::SendDownloadRequest(const QString &taskId, qint64 offset)
{
    auto it = _downloads.find(taskId);
    if (it == _downloads.end() || !IsCurrentTask(it->route, it->group, it->loginGeneration)) return;
    it->requestSent = true;
    it->offset = offset;
    ResourceClient::GetInstance()->requestGroupAvatarDownload(
        it->route.taskId, it->route.resourceId, it->route.threadId,
        it->route.uid, offset, kAvatarChunkBytes);
}

void GroupAvatarTransfer::HandleDownloadChunk(const GroupAvatarRoute &route, qint64 totalSize,
                                               qint64 offset, const QByteArray &data, bool isLast)
{
    auto it = _downloads.find(route.taskId);
    if (it == _downloads.end() || !SameRoute(it->route, route)) return;
    if (!IsCurrentTask(it->route, it->group, it->loginGeneration)) {
        FailDownload(route.taskId, tr("群头像请求已过期"));
        return;
    }
    if (totalSize != it->group.avatarSize || totalSize <= 0 || totalSize > kMaxAvatarBytes
        || offset != it->offset || data.isEmpty() || data.size() > kAvatarChunkBytes
        || offset + data.size() > totalSize || !it->writer) {
        FailDownload(route.taskId, tr("群头像分片身份或范围无效"));
        return;
    }
    const qint64 written = it->writer->write(data);
    if (written != data.size()) {
        FailDownload(route.taskId, tr("写入群头像缓存失败"));
        return;
    }
    it->offset += written;
    it->requestSent = false;
    if (isLast) {
        if (it->offset != totalSize) FailDownload(route.taskId, tr("群头像分片提前结束"));
        else FinishDownload(route.taskId);
        return;
    }
    if (it->offset >= totalSize) {
        FailDownload(route.taskId, tr("群头像未收到最终分片标记"));
        return;
    }
    SendDownloadRequest(route.taskId, it->offset);
}

void GroupAvatarTransfer::HandleUploadSync(const GroupAvatarRoute &route, qint64 confirmedOffset,
                                            qint64 totalSize, bool completed)
{
    auto it = _uploads.find(route.taskId);
    if (it == _uploads.end() || !SameRoute(it->route, route)
        || it->phase != UploadPhase::AwaitSync) return;
    if (!IsCurrentTask(it->route, it->group, it->loginGeneration)
        || !IsCurrentGroup(it->group, true)) {
        FinishUpload(route.taskId, false, tr("群管理权限或头像版本已变化"));
        return;
    }
    if (totalSize != it->totalSize || confirmedOffset < 0 || confirmedOffset > totalSize) {
        FinishUpload(route.taskId, false, tr("资源服务器返回了无效上传位置"));
        return;
    }
    if (completed) {
        if (confirmedOffset != totalSize) {
            FinishUpload(route.taskId, false, tr("已完成上传的长度不匹配"));
            return;
        }
        PublishUpload(route.taskId);
        return;
    }
    if (confirmedOffset == totalSize) {
        FinishUpload(route.taskId, false, tr("资源服务器未确认完整头像上传"));
        return;
    }
    it->phase = UploadPhase::Uploading;
    SendUploadChunk(route.taskId, confirmedOffset);
}

void GroupAvatarTransfer::HandleUploadProgress(const GroupAvatarRoute &route,
                                                qint64 confirmedOffset, qint64 totalSize,
                                                bool completed)
{
    auto it = _uploads.find(route.taskId);
    if (it == _uploads.end() || !SameRoute(it->route, route)
        || it->phase != UploadPhase::Uploading || !it->chunkInFlight) return;
    if (!IsCurrentTask(it->route, it->group, it->loginGeneration)
        || !IsCurrentGroup(it->group, true)) {
        FinishUpload(route.taskId, false, tr("群管理权限或头像版本已变化"));
        return;
    }
    if (totalSize != it->totalSize || confirmedOffset < 0 || confirmedOffset > totalSize) {
        FinishUpload(route.taskId, false, tr("资源服务器上传分片长度无效"));
        return;
    }
    if (confirmedOffset != it->expectedOffset) {
        FinishUpload(route.taskId, false, tr("资源服务器确认位置与当前上传分片不符"));
        return;
    }
    it->chunkInFlight = false;
    if (completed) {
        if (confirmedOffset != totalSize) {
            FinishUpload(route.taskId, false, tr("已完成上传的长度不匹配"));
            return;
        }
        PublishUpload(route.taskId);
        return;
    }
    if (confirmedOffset == totalSize) {
        FinishUpload(route.taskId, false, tr("资源服务器未确认完整头像上传"));
        return;
    }
    SendUploadChunk(route.taskId, confirmedOffset);
}

void GroupAvatarTransfer::SendUploadChunk(const QString &taskId, qint64 offset)
{
    auto it = _uploads.find(taskId);
    if (it == _uploads.end() || it->chunkInFlight || !it->publishRequestId.isEmpty()) return;
    if (!IsCurrentTask(it->route, it->group, it->loginGeneration)
        || !IsCurrentGroup(it->group, true)) {
        FinishUpload(taskId, false, tr("群管理权限或头像版本已变化"));
        return;
    }
    if (offset < 0 || offset >= it->totalSize) {
        FinishUpload(taskId, false, tr("群头像续传位置已到文件末尾"));
        return;
    }
    QFile file(it->sourcePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() != it->totalSize || !file.seek(offset)) {
        FinishUpload(taskId, false, tr("无法读取待上传群头像"));
        return;
    }
    const QByteArray bytes = file.read(qMin<qint64>(kAvatarChunkBytes, it->totalSize - offset));
    if (bytes.isEmpty() || offset + bytes.size() > it->totalSize) {
        FinishUpload(taskId, false, tr("群头像上传分片为空或范围错误"));
        return;
    }
    const bool isLast = offset + bytes.size() == it->totalSize;
    it->expectedOffset = offset + bytes.size();
    it->chunkInFlight = true;
    // 每个 chunk 都重新携带当前登录 token，并由 ResourceClient 使用专属群路由回包。
    ResourceClient::GetInstance()->uploadGroupAvatarChunk(
        it->route.taskId, it->route.threadId, it->route.uid, it->md5,
        it->name, it->totalSize, offset, bytes, isLast);
}

void GroupAvatarTransfer::PublishUpload(const QString &taskId)
{
    auto it = _uploads.find(taskId);
    if (it == _uploads.end() || it->chunkInFlight || !it->publishRequestId.isEmpty()) return;
    if (!IsCurrentTask(it->route, it->group, it->loginGeneration)
        || !IsCurrentGroup(it->group, true)) {
        FinishUpload(taskId, false, tr("群管理权限或头像版本已变化"));
        return;
    }
    const QString requestId = TcpMgr::GetInstance()->UpdateGroupAvatar(
        it->route.threadId, it->route.resourceId);
    if (requestId.isEmpty()) {
        FinishUpload(taskId, false, tr("无法提交群头像发布请求"));
        return;
    }
    it->phase = UploadPhase::Publishing;
    it->publishRequestId = requestId;
    _publishRequestToTask.insert(requestId, taskId);
}


void GroupAvatarTransfer::HandleResourceError(const GroupAvatarRoute &route, const QString &message)
{
    auto download = _downloads.find(route.taskId);
    if (download != _downloads.end() && SameRoute(download->route, route)) {
        FailDownload(route.taskId, message);
        return;
    }
    auto upload = _uploads.find(route.taskId);
    if (upload != _uploads.end() && SameRoute(upload->route, route))
        FinishUpload(route.taskId, false, message);
}

void GroupAvatarTransfer::HandleMutationResult(const QString &requestId, int requestType, int error,
                                                const QJsonObject &response)
{
    const QString taskId = _publishRequestToTask.take(requestId);
    if (taskId.isEmpty()) return;
    auto it = _uploads.find(taskId);
    if (it == _uploads.end() || it->publishRequestId != requestId) return;
    if (requestType != ID_UPDATE_GROUP_AVATAR_REQ || error != ErrorCodes::SUCCESS) {
        FinishUpload(taskId, false, response.value(QStringLiteral("message")).toString(
                         tr("群头像发布失败（错误码 %1）").arg(error)));
        return;
    }
    const QJsonObject group = response.value(QStringLiteral("group")).toObject();
    const qint64 threadId = group.value(QStringLiteral("thread_id")).toVariant().toLongLong();
    const qint64 avatarVersion = group.value(QStringLiteral("avatar_version")).toVariant().toLongLong();
    const QString resourceId = group.value(QStringLiteral("avatar_resource_id")).toString();
    const bool grantedMember = group.value(QStringLiteral("member")).toBool()
        && group.value(QStringLiteral("membership_proof")).toBool();
    const qint64 groupVersion = group.value(QStringLiteral("version")).toVariant().toLongLong();
    const LocalChatThread current = FindCachedGroup(threadId);
    const auto user = UserMgr::GetInstance()->GetUserInfo();
    const bool currentSession = user && user->_uid == it->route.uid
        && it->loginGeneration == _loginGeneration
        && TcpMgr::GetInstance()->LoginGeneration() == it->loginGeneration;
    const bool persistedPublishedGroup = currentSession && current.active
        && current.membershipVersion >= it->group.membershipVersion
        && current.version >= groupVersion && current.avatarVersion == avatarVersion
        && current.avatarResourceId == resourceId;
    if (threadId != it->group.threadId || resourceId != it->route.resourceId
        || avatarVersion <= it->group.avatarVersion || !grantedMember || groupVersion <= it->group.version
        || !persistedPublishedGroup) {
        FinishUpload(taskId, false, tr("群头像发布回包身份/版本不匹配，未采用该结果"));
        return;
    }
    FinishUpload(taskId, true, tr("群头像已发布"));
}

void GroupAvatarTransfer::FinishDownload(const QString &taskId)
{
    auto it = _downloads.find(taskId);
    if (it == _downloads.end()) return;
    DownloadTask task = it.value();
    if (!IsCurrentTask(task.route, task.group, task.loginGeneration) || !task.writer
        || task.offset != task.group.avatarSize || !task.writer->commit()) {
        FailDownload(taskId, tr("群头像缓存提交失败或身份已过期"));
        return;
    }
    const QString path = task.cachePath;
    delete task.writer.data();
    task.writer.clear();
    _downloads.erase(it);
    _downloadByCachePath.remove(path);

    QImage decoded;
    QString mime, error;
    QFileInfo info(path);
    bool valid = info.size() == task.group.avatarSize;
    if (!valid) error = tr("群头像文件长度与已发布元数据不符");
    if (valid) valid = InspectImage(path, &decoded, &mime, &error);
    if (valid && (mime != task.group.avatarMimeType
        || decoded.width() != task.group.avatarWidth
        || decoded.height() != task.group.avatarHeight)) {
        valid = false;
        error = tr("群头像内容与已发布 MIME/尺寸不符");
    }
    if (!valid || !IsCurrentTask(task.route, task.group, task.loginGeneration)) {
        QFile::remove(path);
        if (!error.isEmpty() && IsCurrentTask(task.route, task.group, task.loginGeneration))
            emit sig_avatar_download_failed(task.route.taskId, task.route.uid,
                task.loginGeneration, task.group.threadId, task.group.avatarVersion,
                task.group.avatarResourceId, error);
        return;
    }
    _verifiedCachePaths.insert(path);
    emit sig_avatar_ready(task.route.uid, task.loginGeneration, task.group.threadId,
                          task.group.version, task.group.avatarVersion,
                          task.group.avatarResourceId, path);
}

void GroupAvatarTransfer::FailDownload(const QString &taskId, const QString &message)
{
    auto it = _downloads.find(taskId);
    if (it == _downloads.end()) return;
    const DownloadTask task = it.value();
    if (task.writer) {
        task.writer->cancelWriting();
        delete task.writer.data();
    }
    _downloads.erase(it);
    _downloadByCachePath.remove(task.cachePath);
    if (IsCurrentTask(task.route, task.group, task.loginGeneration))
        emit sig_avatar_download_failed(taskId, task.route.uid, task.loginGeneration,
            task.group.threadId, task.group.avatarVersion, task.group.avatarResourceId, message);
}

void GroupAvatarTransfer::FinishUpload(const QString &taskId, bool success, const QString &message)
{
    auto it = _uploads.find(taskId);
    if (it == _uploads.end()) return;
    const UploadTask task = it.value();
    if (!task.publishRequestId.isEmpty()) _publishRequestToTask.remove(task.publishRequestId);
    _uploads.erase(it);
    const auto user = UserMgr::GetInstance()->GetUserInfo();
    if (!user || user->_uid != task.route.uid || task.route.uid != _accountUid
        || task.loginGeneration != _loginGeneration
        || TcpMgr::GetInstance()->LoginGeneration() != task.loginGeneration) return;
    emit sig_avatar_upload_finished(task.route.taskId, task.route.uid, task.loginGeneration,
        task.route.threadId, task.group.avatarVersion, task.route.resourceId,
        success, message);
}
