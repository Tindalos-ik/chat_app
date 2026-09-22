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

class ResourceClient : public QObject, public Singleton<ResourceClient>
{
    Q_OBJECT
    friend class Singleton<ResourceClient>;
public:
    ~ResourceClient();
    bool IsConnected() const;
    void sendMsg(quint16 id,QByteArray data);
    // 请求资源的一个下载分片。每个 1007 都携带当前 uid/token 和正式私聊 thread_id，
    // ResourceServer 据此校验身份、私聊成员关系及资源归属。
    void requestDownload(const QString& resourceId, qint64 threadId, qint64 offset,
                         qint32 chunkSize = 2048);
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
    void sig_download_error(const QString& message);
};

#endif // RESOURCECLIENT_H
