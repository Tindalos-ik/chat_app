#ifndef RESOURCECLIENT_H
#define RESOURCECLIENT_H
#include <QObject>
#include <QByteArray>
#include <QTcpSocket>
#include "singleton.h"
#include "global.h"
#include <functional>
#include <QMap>
#include <QDataStream>

// 资源发送客户端，负责发送和解析数据
// 因为文件资源的包头和前面的不一样，所以要重新定义

// 文件作用：ResourceServer 的 TCP 分帧与资源协议解析；群头像回复独立路由，不复用私聊信号。
struct GroupAvatarRoute
{
    QString purpose;    // 固定为 group_avatar，用于与旧私聊/个人头像资源协议隔离。
    QString taskId;     // 本次上传/下载 UUID，错误回包也必须原样返回。
    qint64 threadId = 0; // 上传/下载绑定的正式群 thread_id。
    int uid = 0;        // 当前认证账号 UID。
    QString resourceId; // 上传任务 UUID 或当前群已发布资源 ID。
};
class ResourceClient : public QObject, public Singleton<ResourceClient>
{
    Q_OBJECT
    friend class Singleton<ResourceClient>;
public:
    ~ResourceClient();
    bool IsConnected() const;
    /** 与共享私聊资源队列并存时判断 socket 正在查找/连接，避免重复 connectToHost。 */
    bool IsConnecting() const;
    void sendMsg(quint16 id,QByteArray data);
    /** 按私聊原协议请求指定 thread 的下载分片；保留个人/私聊资源兼容路径。 */
    void requestDownload(const QString& resourceId, qint64 threadId, qint64 offset,
                         qint32 chunkSize = 2048);
    /** 请求已发布群头像的分片；taskId、resourceId、threadId、uid 绑定每次下载。 */
    void requestGroupAvatarDownload(const QString &taskId, const QString &resourceId,
                                    qint64 threadId, int uid, qint64 offset,
                                    qint32 chunkSize = 2048);
    /** 发起群头像续传同步；taskId 同时作为 upload/resource ID 并随 thread/uid 鉴权。 */
    void syncGroupAvatarUpload(const QString &taskId, qint64 threadId, int uid,
                               const QString &md5, const QString &name,
                               qint64 totalSize, qint32 chunkSize);
    /** 发送一个认证群头像分片；offset、chunk 和 isLast 描述当前 taskId 的续传位置。 */
    void uploadGroupAvatarChunk(const QString &taskId, qint64 threadId, int uid,
                                const QString &md5, const QString &name,
                                qint64 totalSize, qint64 offset,
                                const QByteArray &chunk, bool isLast);
private:
    ResourceClient();

    void initHandlers();
    QMap<quint16, std::function<void(quint16 id, int len, QByteArray data)>> _handler;

    QTcpSocket* _socket;
    QByteArray _buffer;
    bool _b_recy_pending;
    quint16 _message_id;
    quint32 _message_len;

    QString _host;
    uint16_t _port;

public slots:
    void slot_send_msg(quint16 id, QByteArray body);
    void slot_tcp_connect(QString, QString);

signals:
    void sig_net_error(QString);
    void sig_send_msg(quint16, QByteArray);
    void sig_show_test(QString);
    void sig_con_success(bool);
    // 服务端确认写入后的真实进度；qint64 避免 2GB 以上文件溢出。
    // resource_url 是完成上传后供业务服务保存的资源地址，未完成时允许为空。
    // upload_id 用于把确认回包精确归属到单个上传任务，避免头像上传与聊天图片
    // 队列并发时误把别人的进度当成本任务进度。
    void sig_upload_progress(const QString& upload_id, qint64 confirmed_offset,
                             qint64 total_size, bool completed, const QString& resource_url);
    // 同步上传任务后返回的续传位置；已完成的任务也必须带回 resource_url。
    void sig_file_sync(qint64 confirmed_offset, qint64 total_size, bool completed,
                       const QString& upload_id, const QString& resource_url);
    void sig_upload_error(const QString& message, const QString& upload_id);
    // 一个下载回包对应一个请求分片。data 已从 Base64 还原为原始二进制；offset 是
    // 该分片在服务端文件中的起始字节位置，is_last 为 true 时文件传输完成。
    void sig_download_chunk(const QString& resource_id, qint64 total_size, qint64 offset,
                            const QByteArray& data, bool is_last, const QString& file_name);
    /** 群头像续传响应；route 必须与请求身份一致，剩余字段为服务端确认的上传进度。 */
    void sig_group_avatar_sync(const GroupAvatarRoute &route, qint64 confirmedOffset,
                               qint64 totalSize, bool completed);
    /** 群头像分片上传确认；route 绑定任务，偏移/总长/完成标志来自服务端回包。 */
    void sig_group_avatar_upload_progress(const GroupAvatarRoute &route, qint64 confirmedOffset,
                                          qint64 totalSize, bool completed);
    /** 群头像下载分片；route、offset 与服务端 thread/resource 身份一并交给任务校验。 */
    void sig_group_avatar_download_chunk(const GroupAvatarRoute &route, qint64 totalSize,
                                         qint64 offset, const QByteArray &data, bool isLast);
    /** 群头像业务错误；只按 route 发往对应 task，不转发到通用私聊下载/上传错误信号。 */
    void sig_group_avatar_error(const GroupAvatarRoute &route, const QString &message);
    void sig_download_error(const QString& message);
};

#endif // RESOURCECLIENT_H
