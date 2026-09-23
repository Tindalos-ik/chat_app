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
constexpr qint64 kMaxTransferBytes = 100LL * 1024 * 1024;
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
            && (uploadId.isEmpty() || uploadId == _active.resource.resourceId)) {
            failActive(message);
        }
    });
    connect(resourceClient.get(), &ResourceClient::sig_file_sync, this,
            [this](qint64 offset, qint64 total, bool completed, const QString &uploadId,
                   const QString &resourceUrl) {
        Q_UNUSED(resourceUrl);
        if (!_hasActive || _active.kind != JobKind::Upload || _state != State::UploadSync
            || uploadId != _active.resource.resourceId || total != _active.resource.fileSize
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
            || uploadId != _active.resource.resourceId || total != _active.resource.fileSize
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
            || resourceId != _active.resource.resourceId || !_downloadWriter
            || offset != _downloadOffset || total <= 0 || total != _active.resource.fileSize
            || total != _downloadTotal || data.isEmpty() || data.size() > total - offset) {
            failActive(_active.isFile ? tr("资源服务器返回的文件分片不连续或与消息元数据不一致。")
                                      : tr("资源服务器返回的图片分片不连续或与消息元数据不一致。"));
            return;
        }
        if (_downloadWriter->write(data) != data.size()) {
            failActive(_active.isFile ? tr("写入文件临时文件失败。") : tr("写入图片临时文件失败。"));
            return;
        }
        _downloadOffset += data.size();
        if (isLast) {
            if (_downloadOffset != _downloadTotal) {
                failActive(_active.isFile ? tr("文件下载长度不完整。") : tr("图片下载长度不完整。"));
                return;
            }
            finishActiveDownload();
            return;
        }
        ResourceClient::GetInstance()->requestDownload(resourceId, _active.resource.threadId,
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
    job.imageMetadata = metadata;
    job.resource.msgId = metadata.msgId;
    job.resource.resourceId = metadata.resourceId;
    job.resource.name = metadata.name;
    job.resource.threadId = metadata.threadId;
    job.resource.fileSize = metadata.fileSize;
    QString error;
    if (!prepareUpload(&job, &error)) {
        emit uploadFailed(metadata.msgId, error);
        return;
    }
    _queue.enqueue(job);
    startNext();
}

void ChatImageTransferTask::enqueueFileUpload(const QString &sourcePath, const FileChatData &metadata)
{
    // 文件上传的公开边界始终使用 FileChatData。Job::resource 只保存分片协议共同需要的
    // upload_id、文件名和长度，不把文件消息伪装成 ImageChatData，也不执行图片解码校验。
    Job job;
    job.kind = JobKind::Upload;
    job.isFile = true;
    job.sourcePath = sourcePath;
    job.fileMetadata = metadata;
    job.resource.msgId = metadata.msgId;
    job.resource.resourceId = metadata.resourceId;
    job.resource.name = metadata.name;
    job.resource.threadId = metadata.threadId;
    job.resource.fileSize = metadata.fileSize;
    const QFileInfo info(sourcePath);
    if (!info.isFile() || info.size() <= 0 || info.size() > kMaxTransferBytes) {
        emit uploadFailed(metadata.msgId, tr("文件不存在、为空或超过 100 MB。"));
        return;
    }
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        emit uploadFailed(metadata.msgId, tr("无法读取待发送文件。"));
        return;
    }
    QCryptographicHash md5(QCryptographicHash::Md5);
    // ResourceServer 以“MD5_文件长度_分片长度”作为可断点续传的 upload_id。
    // 必须在进入队列前计算完成，使同步请求和之后每个上传分片使用同一个稳定 ID。
    while (!file.atEnd()) {
        const QByteArray data = file.read(1024 * 1024);
        if (data.isEmpty() && file.error() != QFileDevice::NoError) {
            emit uploadFailed(metadata.msgId, tr("读取文件失败。")); return;
        }
        md5.addData(data);
    }
    job.resource.name = info.fileName();
    job.fileMetadata.name = job.resource.name;
    job.fileMetadata.mimeType = QMimeDatabase().mimeTypeForFile(info).name();
    job.resource.fileSize = info.size();
    job.fileMetadata.fileSize = job.resource.fileSize;
    job.resource.resourceId = QStringLiteral("%1_%2_%3")
        .arg(QString::fromLatin1(md5.result().toHex())).arg(info.size()).arg(kUploadChunkSize);
    job.fileMetadata.resourceId = job.resource.resourceId;
    job.md5 = job.resource.resourceId.section(QLatin1Char('_'), 0, 0);
    _queue.enqueue(job);
    startNext();
}

void ChatImageTransferTask::enqueueFileDownload(const FileChatData &metadata,
                                                 const QString &destinationPath)
{
    // destinationPath 来自用户主动确认的保存对话框。接收 1042 或加载 1030 历史时
    // 不会调用此函数，因此显示文件卡片不会产生任何自动下载或磁盘写入。
    const QFileInfo destination(destinationPath);
    if (metadata.resourceId.isEmpty() || metadata.threadId <= 0 || metadata.fileSize <= 0
        || metadata.fileSize > kMaxTransferBytes || destinationPath.trimmed().isEmpty()
        || !destination.isAbsolute() || destination.fileName().isEmpty()) {
        emit fileDownloadFailed(metadata, tr("文件消息或保存路径无效。"));
        return;
    }
    Job job;
    job.kind = JobKind::Download;
    job.isFile = true;
    job.fileMetadata = metadata;
    job.resource.msgId = metadata.msgId;
    job.resource.resourceId = metadata.resourceId;
    job.resource.name = metadata.name;
    job.resource.threadId = metadata.threadId;
    job.resource.fileSize = metadata.fileSize;
    job.destinationPath = destination.absoluteFilePath();
    _queue.enqueue(job);
    startNext();
}

void ChatImageTransferTask::enqueueDownload(const ImageChatData &metadata)
{
    if (metadata.resourceId.isEmpty() || metadata.threadId <= 0 || metadata.fileSize <= 0 || metadata.fileSize > kMaxTransferBytes
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
    job.imageMetadata = metadata;
    job.resource.msgId = metadata.msgId;
    job.resource.resourceId = metadata.resourceId;
    job.resource.name = metadata.name;
    job.resource.threadId = metadata.threadId;
    job.resource.fileSize = metadata.fileSize;
    _queue.enqueue(job);
    startNext();
}

bool ChatImageTransferTask::prepareUpload(Job *job, QString *error) const
{
    const QFileInfo info(job->sourcePath);
    if (!info.exists() || !info.isFile() || info.size() <= 0 || info.size() > kMaxTransferBytes) {
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
    job->resource.name = info.fileName();
    job->resource.fileSize = info.size();
    job->imageMetadata.name = info.fileName();
    job->imageMetadata.mimeType = mimeName;
    job->imageMetadata.fileSize = info.size();
    job->resource.resourceId = QStringLiteral("%1_%2_%3")
                                   .arg(QString::fromLatin1(md5.result().toHex()))
                                   .arg(job->resource.fileSize)
                                   .arg(kUploadChunkSize);
    job->imageMetadata.resourceId = job->resource.resourceId;
    job->imageMetadata.width = size.width();
    job->imageMetadata.height = size.height();
    job->md5 = job->resource.resourceId.section(QLatin1Char('_'), 0, 0);
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
    // 图片和普通文件共用 ResourceServer 的同步/上传协议；这里仅序列化资源层字段，
    // 聊天层的 fromuid/touid/fileArray 要等资源上传完成后由 ChatDialog 发送给 ChatServer。
    request["upload_id"] = _active.resource.resourceId;
    request["md5"] = _active.md5;
    request["name"] = _active.resource.name;
    request["total_size"] = static_cast<double>(_active.resource.fileSize);
    request["chunk_size"] = static_cast<double>(kUploadChunkSize);
    _state = State::UploadSync;
    ResourceClient::GetInstance()->sendMsg(ResourceReqId::ID_SYNC_FILE_REQ,
        QJsonDocument(request).toJson(QJsonDocument::Compact));
}

void ChatImageTransferTask::sendNextUploadChunk(qint64 confirmedOffset)
{
    QFile file(_active.sourcePath);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(confirmedOffset)) {
        failActive(_active.isFile ? tr("读取待发送文件失败。") : tr("读取待发送图片失败。"));
        return;
    }
    const QByteArray chunk = file.read(kUploadChunkSize);
    if (chunk.isEmpty() && confirmedOffset < _active.resource.fileSize) {
        failActive(_active.isFile ? tr("读取文件分片失败。") : tr("读取图片分片失败。"));
        return;
    }
    QJsonObject request;
    request["upload_id"] = _active.resource.resourceId;
    request["md5"] = _active.md5;
    request["name"] = _active.resource.name;
    request["total_size"] = static_cast<double>(_active.resource.fileSize);
    request["offset"] = static_cast<double>(confirmedOffset);
    request["is_last"] = confirmedOffset + chunk.size() == _active.resource.fileSize;
    request["data"] = QString::fromLatin1(chunk.toBase64());
    ResourceClient::GetInstance()->sendMsg(ResourceReqId::ID_UPLOAD_FILE_REQ,
        QJsonDocument(request).toJson(QJsonDocument::Compact));
}

void ChatImageTransferTask::beginDownload()
{
    if (!_hasActive || _active.kind != JobKind::Download) {
        return;
    }
    // 图片沿用应用缓存路径；普通文件严格写入用户选中的目标路径。二者都使用
    // QSaveFile，写入期间只存在临时文件，成功 commit 后目标文件才原子可见。
    const QString cachePath = _active.isFile ? _active.destinationPath : cachePathFor(_active.imageMetadata);
    if (!QDir().mkpath(QFileInfo(cachePath).absolutePath())) {
        failActive(_active.isFile ? tr("无法创建文件保存目录。") : tr("无法创建图片缓存目录。"));
        return;
    }
    _downloadWriter = new QSaveFile(cachePath, this);
    _downloadWriter->setDirectWriteFallback(false);
    if (!_downloadWriter->open(QIODevice::WriteOnly)) {
        failActive(_active.isFile ? tr("无法创建文件临时文件。") : tr("无法创建图片临时文件。"));
        return;
    }
    _downloadOffset = 0;
    _downloadTotal = _active.resource.fileSize;
    _state = State::Downloading;
    qInfo() << (_active.isFile ? "request first file chunk, resource_id=" : "request first image chunk, resource_id=") << _active.resource.resourceId
            << "total=" << _downloadTotal;
    ResourceClient::GetInstance()->requestDownload(_active.resource.resourceId,
                                                    _active.resource.threadId, 0,
                                                    kDownloadChunkSize);
}

void ChatImageTransferTask::finishActiveUpload()
{
    const bool isFile = _active.isFile;
    const FileChatData fileMetadata = _active.fileMetadata;
    const ImageChatData imageMetadata = _active.imageMetadata;
    _hasActive = false;
    _state = State::Idle;
    if (isFile) emit fileUploadFinished(fileMetadata);
    else emit uploadFinished(imageMetadata);
    QTimer::singleShot(0, this, &ChatImageTransferTask::startNext);
}

void ChatImageTransferTask::finishActiveDownload()
{
    const bool isFile = _active.isFile;
    const ImageChatData imageMetadata = _active.imageMetadata;
    const FileChatData fileMetadata = _active.fileMetadata;
    const QString path = isFile ? _active.destinationPath : cachePathFor(imageMetadata);
    // 先检查临时文件长度再提交；长度错误时 cancelWriting 会丢弃临时文件，避免覆盖
    // 用户原来已存在的目标文件。commit 后再核对最终路径，确认原子替换结果完整。
    if (!_downloadWriter || _downloadWriter->size() != (isFile ? fileMetadata.fileSize : imageMetadata.fileSize)) {
        failActive(isFile ? tr("下载临时文件长度与文件元数据不一致。")
                          : tr("图片临时文件长度与元数据不一致。"));
        return;
    }
    if (!_downloadWriter->commit()) {
        failActive(isFile ? tr("文件原子写入失败。") : tr("图片缓存原子提交失败。"));
        return;
    }
    delete _downloadWriter;
    _downloadWriter = nullptr;
    if (isFile && QFileInfo(path).size() != fileMetadata.fileSize) {
        failActive(tr("保存后的文件长度与消息元数据不一致。"));
        return;
    }
    if (!isFile && !isUsableImageFile(path, imageMetadata)) {
        // 目标文件只位于本任务创建的专用缓存目录，校验失败时清除它，不能显示伪造图片。
        QFile::remove(path);
        failActive(tr("下载的数据无法解码为与消息元数据一致的图片。"));
        return;
    }
    _hasActive = false;
    _state = State::Idle;
    qInfo() << (isFile ? "file download completed, resource_id=" : "image download completed, resource_id=") << _active.resource.resourceId
            << "cache_path=" << path;
    if (isFile) emit fileDownloadFinished(fileMetadata, path);
    else emit imageReady(imageMetadata, path);
    QTimer::singleShot(0, this, &ChatImageTransferTask::startNext);
}

void ChatImageTransferTask::failActive(const QString &message)
{
    if (!_hasActive) {
        return;
    }
    const Job failedJob = _active;
    qWarning() << "chat resource transfer failed, kind="
               << (failedJob.kind == JobKind::Upload ? "upload" : "download")
               << "resource_id=" << failedJob.resource.resourceId << "reason=" << message;
    cleanupDownloadWriter();
    _hasActive = false;
    _state = State::Idle;
    if (failedJob.kind == JobKind::Upload) {
        emit uploadFailed(failedJob.resource.msgId, message);
    } else if (failedJob.isFile) {
        emit fileDownloadFailed(failedJob.fileMetadata, message);
    } else {
        emit imageDownloadFailed(failedJob.imageMetadata, message);
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
