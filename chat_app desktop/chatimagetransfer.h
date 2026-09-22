#ifndef CHATIMAGETRANSFER_H
#define CHATIMAGETRANSFER_H

#include <QObject>
#include <QQueue>

#include "userdata.h"

class QSaveFile;

// 聊天图片的资源传输任务。
//
// ResourceClient 的上传确认历史上没有携带可供多个调用方路由的完整上下文，因此不能
// 让多个界面同时各自直接发送分片。本任务把“同步 -> 单片 -> 服务端确认 -> 下一片”
// 这一套 SettingDialog 已验证的流程收口为 FIFO：界面可并发投递多张图片，而网络层
// 始终只存在一个确定的资源任务。下载同样串行，保证 sig_download_error 不会误伤别的图。
class ChatImageTransferTask : public QObject
{
    Q_OBJECT
public:
    explicit ChatImageTransferTask(QObject *parent = nullptr);

    // sourcePath 是本机图片。成功时 emitted 的 metadata.resourceId 可直接放入 1033。
    void enqueueUpload(const QString &sourcePath, const ImageChatData &metadata);
    // 下载完成后才发出 imageReady；落盘过程由 QSaveFile 完成，外部看不到半成品。
    void enqueueDownload(const ImageChatData &metadata);

signals:
    void uploadFinished(const ImageChatData &metadata);
    void uploadFailed(const QString &msgId, const QString &message);
    void imageReady(const ImageChatData &metadata, const QString &localPath);
    void imageDownloadFailed(const ImageChatData &metadata, const QString &message);

private:
    enum class JobKind { Upload, Download };
    enum class State { Idle, WaitingConnection, UploadSync, Uploading, Downloading };
    struct Job {
        JobKind kind = JobKind::Upload;
        ImageChatData metadata;
        QString sourcePath;
        QString md5;
    };

    bool prepareUpload(Job *job, QString *error) const;
    bool ensureResourceConnection();
    void startNext();
    void beginUpload();
    void sendNextUploadChunk(qint64 confirmedOffset);
    void beginDownload();
    void finishActiveUpload();
    void finishActiveDownload();
    void failActive(const QString &message);
    void cleanupDownloadWriter();
    QString cachePathFor(const ImageChatData &metadata) const;
    bool isUsableImageFile(const QString &path, const ImageChatData &metadata) const;

    QQueue<Job> _queue;
    Job _active;
    bool _hasActive = false;
    State _state = State::Idle;
    qint64 _downloadOffset = 0;
    qint64 _downloadTotal = 0;
    QSaveFile *_downloadWriter = nullptr;
};

#endif // CHATIMAGETRANSFER_H
