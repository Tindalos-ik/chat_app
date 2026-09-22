#include "chatimagetransfer.h"

#include <QCoreApplication>
#include <QDebug>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QMimeType>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QRegularExpression>

#include "global.h"
#include "resourceclient.h"
#include "usermgr.h"

namespace {
constexpr qint64 kUploadChunkSize = 1024;
constexpr qint64 kMaxImageBytes = 100LL * 1024 * 1024;
constexpr qint32 kDownloadChunkSize = 2048;
}

ChatImageTransferTask::ChatImageTransferTask(QObject *parent)
    : QObject(parent)
{
    auto resourceClient = ResourceClient::GetInstance();
    connect(resourceClient.get(), &ResourceClient::sig_con_success, this, [this](bool success) {
        if (!_hasActive || _state != State::WaitingConnection) {
            return;
        }
        if (!success) {
            failActive(tr("连接资源服务器失败。"));
            return;
        }
        if (_active.kind == JobKind::Upload) {
            beginUpload();
        } else {
            beginDownload();
        }
    });
    connect(resourceClient.get(), &ResourceClient::sig_net_error, this, [this](const QString &message) {
        if (_hasActive && _state != State::Idle) {
            failActive(message.isEmpty() ? tr("资源服务器网络异常。") : message);
        }
    });
    connect(resourceClient.get(), &ResourceClient::sig_upload_error, this,
            [this](const QString &message, const QString &uploadId) {
        if (_hasActive && _active.kind == JobKind::Upload
            && (uploadId.isEmpty() || uploadId == _active.metadata.resourceId)) {
            failActive(message);
        }
    });
    connect(resourceClient.get(), &ResourceClient::sig_file_sync, this,
            [this](qint64 offset, qint64 total, bool completed, const QString &uploadId,
                   const QString &resourceUrl) {
        Q_UNUSED(resourceUrl);
        if (!_hasActive || _active.kind != JobKind::Upload || _state != State::UploadSync
            || uploadId != _active.metadata.resourceId || total != _active.metadata.fileSize
            || offset < 0 || offset > total) {
            return;
        }
        if (completed) {
            finishActiveUpload();
            return;
        }
        _state = State::Uploading;
        sendNextUploadChunk(offset);
    });
    connect(resourceClient.get(), &ResourceClient::sig_upload_progress, this,
            [this](const QString &uploadId, qint64 offset, qint64 total, bool completed,
                   const QString &resourceUrl) {
        Q_UNUSED(resourceUrl);
        if (!_hasActive || _active.kind != JobKind::Upload || _state != State::Uploading
            || uploadId != _active.metadata.resourceId || total != _active.metadata.fileSize
            || offset < 0 || offset > total) {
            return;
        }
        if (completed) {
            finishActiveUpload();
        } else {
            sendNextUploadChunk(offset);
        }
    });
    connect(resourceClient.get(), &ResourceClient::sig_download_chunk, this,
            [this](const QString &resourceId, qint64 total, qint64 offset, const QByteArray &data,
                   bool isLast, const QString &fileName) {
        Q_UNUSED(fileName);
        if (!_hasActive || _active.kind != JobKind::Download || _state != State::Downloading
            || resourceId != _active.metadata.resourceId || !_downloadWriter
            || offset != _downloadOffset || total <= 0 || total != _active.metadata.fileSize
            || total != _downloadTotal || data.isEmpty() || data.size() > total - offset) {
            failActive(tr("资源服务器返回的图片分片不连续或与消息元数据不一致。"));
            return;
        }
        if (_downloadWriter->write(data) != data.size()) {
            failActive(tr("写入图片临时文件失败。"));
            return;
        }
        _downloadOffset += data.size();
        if (isLast) {
            if (_downloadOffset != _downloadTotal) {
                failActive(tr("图片下载长度不完整。"));
                return;
            }
            finishActiveDownload();
            return;
        }
        ResourceClient::GetInstance()->requestDownload(resourceId, _active.metadata.threadId,
                                                        _downloadOffset, kDownloadChunkSize);
    });
    connect(resourceClient.get(), &ResourceClient::sig_download_error, this,
            [this](const QString &message) {
        if (_hasActive && _active.kind == JobKind::Download) {
            failActive(message);
        }
    });
}

void ChatImageTransferTask::enqueueUpload(const QString &sourcePath, const ImageChatData &metadata)
{
    Job job;
    job.kind = JobKind::Upload;
    job.sourcePath = sourcePath;
    job.metadata = metadata;
    QString error;
    if (!prepareUpload(&job, &error)) {
        emit uploadFailed(metadata.msgId, error);
        return;
    }
    _queue.enqueue(job);
    startNext();
}

void ChatImageTransferTask::enqueueDownload(const ImageChatData &metadata)
{
    if (metadata.resourceId.isEmpty() || metadata.threadId <= 0 || metadata.fileSize <= 0 || metadata.fileSize > kMaxImageBytes
        || metadata.width <= 0 || metadata.height <= 0 || !metadata.mimeType.startsWith("image/")) {
        emit imageDownloadFailed(metadata, tr("图片消息元数据无效。"));
        return;
    }
    const QString cachedPath = cachePathFor(metadata);
    if (isUsableImageFile(cachedPath, metadata)) {
        // 保持异步语义：调用方可以先完成 1034/1035 的状态更新，再收到图片就绪回调。
        QTimer::singleShot(0, this, [this, metadata, cachedPath] { emit imageReady(metadata, cachedPath); });
        return;
    }
    qInfo() << "queue image download, resource_id=" << metadata.resourceId
            << "bytes=" << metadata.fileSize << "cache_path=" << cachedPath;
    Job job;
    job.kind = JobKind::Download;
    job.metadata = metadata;
    _queue.enqueue(job);
    startNext();
}

bool ChatImageTransferTask::prepareUpload(Job *job, QString *error) const
{
    const QFileInfo info(job->sourcePath);
    if (!info.exists() || !info.isFile() || info.size() <= 0 || info.size() > kMaxImageBytes) {
        *error = tr("图片文件不存在、为空或超过 100 MB。");
        return false;
    }
    QImageReader reader(info.absoluteFilePath());
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    const QMimeType mime = QMimeDatabase().mimeTypeForFile(info);
    if (!size.isValid() || !mime.name().startsWith("image/")) {
        *error = tr("所选文件不是可读取的图片。");
        return false;
    }
    // ResourceServer 当前按文件魔数核验 PNG/JPEG/GIF/WebP。若允许 BMP、TIFF、
    // SVG 等格式先上传，ChatServer 最终仍会因 gRPC 核验拒绝，用户只能白白等待。
    // 因此在上传前给出明确提示；未来服务端扩展格式时这里也要同步增加白名单。
    const QString mimeName = mime.name();
    if (mimeName != QStringLiteral("image/png") && mimeName != QStringLiteral("image/jpeg")
        && mimeName != QStringLiteral("image/gif") && mimeName != QStringLiteral("image/webp")) {
        *error = tr("当前仅支持 PNG、JPEG、GIF 和 WebP 图片。");
        return false;
    }
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        *error = tr("无法读取待发送的图片。");
        return false;
    }
    QCryptographicHash md5(QCryptographicHash::Md5);
    while (!file.atEnd()) {
        const QByteArray data = file.read(1024 * 1024);
        if (data.isEmpty() && file.error() != QFileDevice::NoError) {
            *error = tr("计算图片校验信息时读取失败。");
            return false;
        }
        md5.addData(data);
    }
    job->metadata.name = info.fileName();
    job->metadata.mimeType = mimeName;
    job->metadata.fileSize = info.size();
    job->metadata.width = size.width();
    job->metadata.height = size.height();
    job->metadata.resourceId = QStringLiteral("%1_%2_%3")
                                   .arg(QString::fromLatin1(md5.result().toHex()))
                                   .arg(job->metadata.fileSize)
                                   .arg(kUploadChunkSize);
    job->md5 = job->metadata.resourceId.section(QLatin1Char('_'), 0, 0);
    return true;
}

bool ChatImageTransferTask::ensureResourceConnection()
{
    auto resourceClient = ResourceClient::GetInstance();
    if (resourceClient->IsConnected()) {
        return true;
    }
    const QString configPath = QDir::toNativeSeparators(
        QCoreApplication::applicationDirPath() + QDir::separator() + QStringLiteral("config.ini"));
    QSettings settings(configPath, QSettings::IniFormat);
    const QString host = settings.value(QStringLiteral("ResourceServer/host")).toString();
    const QString port = settings.value(QStringLiteral("ResourceServer/port")).toString();
    if (host.isEmpty() || port.toUShort() == 0) {
        failActive(tr("ResourceServer 地址未配置或端口无效。"));
        return false;
    }
    _state = State::WaitingConnection;
    resourceClient->slot_tcp_connect(host, port);
    return false;
}

void ChatImageTransferTask::startNext()
{
    if (_hasActive || _queue.isEmpty()) {
        return;
    }
    _active = _queue.dequeue();
    _hasActive = true;
    if (!ensureResourceConnection()) {
        return;
    }
    if (_active.kind == JobKind::Upload) {
        beginUpload();
    } else {
        beginDownload();
    }
}

void ChatImageTransferTask::beginUpload()
{
    if (!_hasActive || _active.kind != JobKind::Upload) {
        return;
    }
    QJsonObject request;
    request["upload_id"] = _active.metadata.resourceId;
    request["md5"] = _active.md5;
    request["name"] = _active.metadata.name;
    request["total_size"] = static_cast<double>(_active.metadata.fileSize);
    request["chunk_size"] = static_cast<double>(kUploadChunkSize);
    _state = State::UploadSync;
    ResourceClient::GetInstance()->sendMsg(ResourceReqId::ID_SYNC_FILE_REQ,
        QJsonDocument(request).toJson(QJsonDocument::Compact));
}

void ChatImageTransferTask::sendNextUploadChunk(qint64 confirmedOffset)
{
    QFile file(_active.sourcePath);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(confirmedOffset)) {
        failActive(tr("读取待发送图片失败。"));
        return;
    }
    const QByteArray chunk = file.read(kUploadChunkSize);
    if (chunk.isEmpty() && confirmedOffset < _active.metadata.fileSize) {
        failActive(tr("读取图片分片失败。"));
        return;
    }
    QJsonObject request;
    request["upload_id"] = _active.metadata.resourceId;
    request["md5"] = _active.md5;
    request["name"] = _active.metadata.name;
    request["total_size"] = static_cast<double>(_active.metadata.fileSize);
    request["offset"] = static_cast<double>(confirmedOffset);
    request["is_last"] = confirmedOffset + chunk.size() == _active.metadata.fileSize;
    request["data"] = QString::fromLatin1(chunk.toBase64());
    ResourceClient::GetInstance()->sendMsg(ResourceReqId::ID_UPLOAD_FILE_REQ,
        QJsonDocument(request).toJson(QJsonDocument::Compact));
}

void ChatImageTransferTask::beginDownload()
{
    if (!_hasActive || _active.kind != JobKind::Download) {
        return;
    }
    const QString cachePath = cachePathFor(_active.metadata);
    if (!QDir().mkpath(QFileInfo(cachePath).absolutePath())) {
        failActive(tr("无法创建图片缓存目录。"));
        return;
    }
    _downloadWriter = new QSaveFile(cachePath, this);
    _downloadWriter->setDirectWriteFallback(false);
    if (!_downloadWriter->open(QIODevice::WriteOnly)) {
        failActive(tr("无法创建图片临时文件。"));
        return;
    }
    _downloadOffset = 0;
    _downloadTotal = _active.metadata.fileSize;
    _state = State::Downloading;
    qInfo() << "request first image chunk, resource_id=" << _active.metadata.resourceId
            << "total=" << _downloadTotal;
    ResourceClient::GetInstance()->requestDownload(_active.metadata.resourceId,
                                                    _active.metadata.threadId, 0,
                                                    kDownloadChunkSize);
}

void ChatImageTransferTask::finishActiveUpload()
{
    const ImageChatData metadata = _active.metadata;
    _hasActive = false;
    _state = State::Idle;
    emit uploadFinished(metadata);
    QTimer::singleShot(0, this, &ChatImageTransferTask::startNext);
}

void ChatImageTransferTask::finishActiveDownload()
{
    const ImageChatData metadata = _active.metadata;
    const QString path = cachePathFor(metadata);
    if (!_downloadWriter || !_downloadWriter->commit()) {
        failActive(tr("图片缓存原子提交失败。"));
        return;
    }
    delete _downloadWriter;
    _downloadWriter = nullptr;
    if (!isUsableImageFile(path, metadata)) {
        // 目标文件只位于本任务创建的专用缓存目录，校验失败时清除它，不能显示伪造图片。
        QFile::remove(path);
        failActive(tr("下载的数据无法解码为与消息元数据一致的图片。"));
        return;
    }
    _hasActive = false;
    _state = State::Idle;
    qInfo() << "image download completed, resource_id=" << metadata.resourceId
            << "cache_path=" << path;
    emit imageReady(metadata, path);
    QTimer::singleShot(0, this, &ChatImageTransferTask::startNext);
}

void ChatImageTransferTask::failActive(const QString &message)
{
    if (!_hasActive) {
        return;
    }
    const Job failedJob = _active;
    qWarning() << "image transfer failed, kind="
               << (failedJob.kind == JobKind::Upload ? "upload" : "download")
               << "resource_id=" << failedJob.metadata.resourceId << "reason=" << message;
    cleanupDownloadWriter();
    _hasActive = false;
    _state = State::Idle;
    if (failedJob.kind == JobKind::Upload) {
        emit uploadFailed(failedJob.metadata.msgId, message);
    } else {
        emit imageDownloadFailed(failedJob.metadata, message);
    }
    QTimer::singleShot(0, this, &ChatImageTransferTask::startNext);
}

void ChatImageTransferTask::cleanupDownloadWriter()
{
    if (_downloadWriter) {
        _downloadWriter->cancelWriting();
        delete _downloadWriter;
        _downloadWriter = nullptr;
    }
    _downloadOffset = 0;
    _downloadTotal = 0;
}

QString ChatImageTransferTask::cachePathFor(const ImageChatData &metadata) const
{
    const QString uid = QString::number(UserMgr::GetInstance()->GetUid());
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(metadata.resourceId.toUtf8(), QCryptographicHash::Sha256).toHex());
    QString suffix = QFileInfo(metadata.name).suffix().toLower();
    if (suffix.isEmpty() || suffix.size() > 10 || !suffix.contains(QRegularExpression("^[a-z0-9]+$"))) {
        suffix = QStringLiteral("img");
    }
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(root).filePath(QStringLiteral("chat_images/%1/%2.%3").arg(uid, digest, suffix));
}

bool ChatImageTransferTask::isUsableImageFile(const QString &path, const ImageChatData &metadata) const
{
    const QFileInfo info(path);
    if (!info.isFile() || info.size() != metadata.fileSize) {
        return false;
    }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!size.isValid() || size.width() != metadata.width || size.height() != metadata.height) {
        return false;
    }
    // size() 只能验证文件头；实际解码一次才能拒绝“尺寸字段正确、像素数据损坏”的缓存。
    return !reader.read().isNull();
}
