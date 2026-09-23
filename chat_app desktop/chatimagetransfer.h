// 文件作用：统一管理聊天图片和文件的 ResourceServer 分片传输。
// 所有任务共用 FIFO，确保 ResourceClient 的分片回包只会交给当前活动任务处理。
#ifndef CHATIMAGETRANSFER_H
#define CHATIMAGETRANSFER_H

#include <QObject>
#include <QQueue>

#include "userdata.h"

class QSaveFile;

// 作用：协调聊天图片和文件的 ResourceServer 传输。所有资源任务共用 FIFO，避免
// ResourceClient 的分片回包在并行任务之间串线；文件下载只由显式保存操作触发。
//
// ResourceClient 的上传确认历史上没有携带可供多个调用方路由的完整上下文，因此不能
// 让多个界面同时各自直接发送分片。本任务把“同步 -> 单片 -> 服务端确认 -> 下一片”
// 这一套 SettingDialog 已验证的流程收口为 FIFO：界面可并发投递多张图片，而网络层
// 始终只存在一个确定的资源任务。下载同样串行，保证 sig_download_error 不会误伤别的图。
class ChatImageTransferTask : public QObject
{
    Q_OBJECT
public:
    // 创建传输协调器，并连接 ResourceClient 的连接、上传、下载回包信号。
    explicit ChatImageTransferTask(QObject *parent = nullptr);

    // sourcePath 是本机图片。成功时 emitted 的 metadata.resourceId 可直接放入 1033。
    // 参数：sourcePath 为本地图片路径；metadata 携带客户端 msgid 和私聊双方 UID。
    // 作用：校验图片格式后排队上传；uploadFinished 返回可发送给 ChatServer 的图片元数据。
    void enqueueUpload(const QString &sourcePath, const ImageChatData &metadata);
    // 参数：sourcePath 是本地文件；metadata 是独立文件类型元数据，至少含 msgId/fromUid/toUid。
    // 作用：以 100 MiB 上限排队上传任意文件，不读取图片尺寸；成功通过 fileUploadFinished 返回。
    void enqueueFileUpload(const QString &sourcePath, const FileChatData &metadata);
    // 参数：metadata 是图片消息元数据。作用：下载到应用图片缓存并校验解码结果。
    void enqueueDownload(const ImageChatData &metadata);
    // 参数：metadata 是文件消息元数据；destinationPath 是用户通过保存对话框选定的完整路径。
    // 作用：按资源分片下载，使用 QSaveFile 原子写入目标路径并验证最终字节数；不会自动调用。
    void enqueueFileDownload(const FileChatData &metadata, const QString &destinationPath);

signals:
    void uploadFinished(const ImageChatData &metadata);
    // 文件上传成功后发出，可直接序列化为 1040 fileArray 项。
    void fileUploadFinished(const FileChatData &metadata);
    void uploadFailed(const QString &msgId, const QString &message);
    void imageReady(const ImageChatData &metadata, const QString &localPath);
    void imageDownloadFailed(const ImageChatData &metadata, const QString &message);
    // 文件保存成功/失败。成功参数为最终保存路径。
    void fileDownloadFinished(const FileChatData &metadata, const QString &destinationPath);
    void fileDownloadFailed(const FileChatData &metadata, const QString &message);

private:
    enum class JobKind { Upload, Download };
    enum class State { Idle, WaitingConnection, UploadSync, Uploading, Downloading };
    struct ResourceJobMetadata {
        QString msgId;
        QString resourceId;
        QString name;
        qint64 threadId = 0;
        qint64 fileSize = 0;
    };
    struct Job {
        JobKind kind = JobKind::Upload;
        ResourceJobMetadata resource;
        ImageChatData imageMetadata;
        FileChatData fileMetadata;
        QString sourcePath;
        QString destinationPath;
        QString md5;
        bool isFile = false; // true 表示文件任务，选择文件元数据和完成信号
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
