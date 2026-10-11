#include "resourceclient.h"
#include "usermgr.h"
#include <QJsonDocument>
#include <QJsonObject>
namespace {
GroupAvatarRoute GroupAvatarRouteFromJson(const QJsonObject &object)
{
    GroupAvatarRoute route;
    route.purpose = object.value(QStringLiteral("purpose")).toString();
    route.taskId = object.value(QStringLiteral("task_id")).toString();
    if (route.taskId.isEmpty())
        route.taskId = object.value(QStringLiteral("upload_id")).toString();
    if (route.taskId.isEmpty())
        route.taskId = object.value(QStringLiteral("resource_id")).toString();
    route.threadId = object.value(QStringLiteral("thread_id")).toVariant().toLongLong();
    route.uid = object.value(QStringLiteral("uid")).toVariant().toInt();
    route.resourceId = object.value(QStringLiteral("resource_id")).toString();
    return route;
}

QString GroupAvatarErrorText(int error, const QString &operation)
{
    if (error == 1020) return QObject::tr("资源服务器拒绝群头像%1：身份或群/资源绑定无效。").arg(operation);
    if (error == 1021) return QObject::tr("暂时无法验证群头像%1权限，请稍后重试。").arg(operation);
    return QObject::tr("群头像%1失败，错误码：%2").arg(operation).arg(error);
}

bool IsGroupAvatarReply(const QJsonObject &object)
{
    return object.value(QStringLiteral("purpose")).toString()
        == QStringLiteral("group_avatar");
}
}

ResourceClient::~ResourceClient()
{

}

// 对外提供连接状态，业务界面在发送数据前可据此做提示。
bool ResourceClient::IsConnected() const
{
    return _socket != nullptr &&
           _socket->state() == QAbstractSocket::ConnectedState;
}
bool ResourceClient::IsConnecting() const
{
    if (!_socket) return false;
    const auto state = _socket->state();
    return state == QAbstractSocket::HostLookupState
        || state == QAbstractSocket::ConnectingState;
}

ResourceClient::ResourceClient(): _b_recy_pending(false), _message_id(0), _message_len(0){

    _socket = new QTcpSocket();
    //_socket连接服务器成功之后，发送信号通知一下
    connect(_socket, &QTcpSocket::connected, [this](){
        qDebug() << "connect to server" << Qt::endl;
        emit sig_con_success(true);
    });

    // 记录断开原因，便于区分客户端主动退出、服务端关闭和网络错误。
    connect(_socket, &QTcpSocket::disconnected, [this]{
        qWarning() << "disconnected from server:" << _socket->errorString();
        _buffer.clear();
        _b_recy_pending = false;
    });

    //在有数据可读时候进行处理
    connect(_socket,&QTcpSocket::readyRead,[this](){
        //读取所有数据到缓冲区
        _buffer.append(_socket->readAll());

        // 一次 readyRead 可能包含多个完整包；半包则保留已读包头，等下次数据补齐包体。
        while (true) {
            //解析头部，消息头是消息id + 消息长度 2 + 4
            if(!_b_recy_pending){
                //检查缓冲区中的数据是否足够解析出一个消息头，不够就返回
                //使用static_cast<> 实现更加安全的类型转换
                if(_buffer.size() < static_cast<int>(sizeof(quint16)*3)){
                    return;
                }

                QDataStream stream(&_buffer, QIODevice::ReadOnly);
                stream.setVersion(QDataStream::Qt_6_0);
                stream.setByteOrder(QDataStream::BigEndian);

                //预读取消息id和消息长度
                stream >> _message_id >> _message_len;

                //将buffer中前四个字节移除，mid截取一段
                _buffer = _buffer.mid(sizeof(quint16)*3);

                //输出读取的数据
                qDebug() << "message id : " << _message_id
                         << "message len : " << _message_len << Qt::endl;
                _b_recy_pending = true;
            }

            //buffer剩余长度是否满足消息体长度，不满足就退出继续等待接受
            if(_buffer.size() < _message_len){
                return;
            }


            //读取消息体，给到回调函数处理
            QByteArray messageBody = _buffer.mid(0, _message_len);
            qDebug() << "receive resource response id:" << _message_id
                     << "bytes:" << messageBody.size();
            _buffer = _buffer.mid(_message_len);
            _b_recy_pending = false;

            //处理收到的数据
            auto iter = _handler.find(_message_id);
            if(iter == _handler.end()){
                qDebug() << "id error" << Qt::endl;
                continue;
            }

            //执行处理函数
            iter.value()(_message_id, _message_len, messageBody);
        }

    });

    //处理错误，直接问ai
    connect(_socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::errorOccurred),
            [this](QAbstractSocket::SocketError socketError){
                Q_UNUSED(socketError);
                qDebug() << "Error : " << _socket->errorString();
                emit sig_net_error(_socket->errorString());
            });

    //连接 发送数据信号和槽函数
    connect(this, &ResourceClient::sig_send_msg, this, &ResourceClient::slot_send_msg);

    initHandlers();

}

void ResourceClient::initHandlers()
{

    // 下载响应严格校验分片范围和 Base64 数据。ResourceServer 的资源协议不能把损坏
    // 数据直接交给后续的图片解码或本地文件写入逻辑。
    _handler.insert(ResourceReqId::ID_DOWNLOAD_FILE_RSP, [this](quint16 id, int len, QByteArray data) {
        Q_UNUSED(id);
        Q_UNUSED(len);
        const QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
        if (!jsonDoc.isObject()) {
            emit sig_download_error(tr("资源服务器返回了无效的下载结果。"));
            return;
        }

        const QJsonObject jsonObj = jsonDoc.object();
        // Group-avatar replies are routed before legacy private-image errors or chunks.
        if (IsGroupAvatarReply(jsonObj)) {
            const GroupAvatarRoute route = GroupAvatarRouteFromJson(jsonObj);
            const int groupError = jsonObj.value(QStringLiteral("error")).toInt(ErrorCodes::ERR_JSON);
            if (groupError != ErrorCodes::SUCCESS) {
                emit sig_group_avatar_error(route, GroupAvatarErrorText(groupError, tr("下载")));
                return;
            }
            bool totalOk = false;
            bool offsetOk = false;
            const qint64 totalSize = jsonObj.value(QStringLiteral("total_size"))
                                         .toVariant().toLongLong(&totalOk);
            const qint64 offset = jsonObj.value(QStringLiteral("offset"))
                                      .toVariant().toLongLong(&offsetOk);
            const QByteArray encodedData = jsonObj.value(QStringLiteral("data"))
                                               .toString().toLatin1();
            const QByteArray decodedData = QByteArray::fromBase64(
                encodedData, QByteArray::AbortOnBase64DecodingErrors);
            const bool isLast = jsonObj.value(QStringLiteral("is_last")).toBool();
            if (route.taskId.isEmpty() || route.threadId <= 0 || route.uid <= 0
                || route.resourceId.isEmpty() || !totalOk || !offsetOk
                || totalSize <= 0 || totalSize > 2 * 1024 * 1024
                || offset < 0 || offset >= totalSize || decodedData.isEmpty()
                || decodedData.size() > totalSize - offset
                || isLast != (offset + decodedData.size() == totalSize)) {
                emit sig_group_avatar_error(route, tr("群头像下载分片或关联身份无效。"));
                return;
            }
            emit sig_group_avatar_download_chunk(route, totalSize, offset, decodedData, isLast);
            return;
        }
        const int error = jsonObj.value("error").toInt(ErrorCodes::ERR_JSON);
        if (error != ErrorCodes::SUCCESS) {
            if (error == 1020) {
                emit sig_download_error(tr("资源服务器拒绝下载：登录已失效、您不属于该私聊，或资源不属于此会话。"));
            } else if (error == 1021) {
                emit sig_download_error(tr("暂时无法验证图片下载权限，请稍后重试。"));
            } else {
                emit sig_download_error(tr("下载资源失败，错误码：%1").arg(error));
            }
            return;
        }

        bool totalOk = false;
        bool offsetOk = false;
        const qint64 totalSize = jsonObj.value("total_size").toVariant().toLongLong(&totalOk);
        const qint64 offset = jsonObj.value("offset").toVariant().toLongLong(&offsetOk);
        const QString resourceId = jsonObj.value("resource_id").toString();
        const QByteArray encodedData = jsonObj.value("data").toString().toLatin1();
        const QByteArray decodedData = QByteArray::fromBase64(
            encodedData, QByteArray::AbortOnBase64DecodingErrors);
        const bool isLast = jsonObj.value("is_last").toBool();
        if (!totalOk || !offsetOk || totalSize < 0 || offset < 0 || offset > totalSize
            || resourceId.isEmpty() || (decodedData.isEmpty() && !encodedData.isEmpty())
            || decodedData.size() > totalSize - offset
            || isLast != (offset + decodedData.size() == totalSize)) {
            emit sig_download_error(tr("资源服务器返回了不一致的下载分片。"));
            return;
        }

        emit sig_download_chunk(resourceId, totalSize, offset, decodedData, isLast,
                                jsonObj.value("name").toString());
    });

    // 上传任务同步回包：客户端据此 seek 到服务端已经确认的字节位置。
    _handler.insert(ResourceReqId::ID_SYNC_FILE_RSP, [this](quint16 id, int len, QByteArray data) {
        Q_UNUSED(id);
        Q_UNUSED(len);
        const QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
        if (jsonDoc.isNull() || !jsonDoc.isObject()) {
            emit sig_upload_error(tr("服务端返回了无效的续传进度。"), QString());
            return;
        }

        const QJsonObject jsonObj = jsonDoc.object();
        if (IsGroupAvatarReply(jsonObj)) {
            const GroupAvatarRoute route = GroupAvatarRouteFromJson(jsonObj);
            const int error = jsonObj.value(QStringLiteral("error")).toInt(ErrorCodes::ERR_JSON);
            if (error != ErrorCodes::SUCCESS) {
                emit sig_group_avatar_error(route, GroupAvatarErrorText(error, tr("上传同步")));
                return;
            }
            bool offsetOk = false;
            bool totalOk = false;
            const qint64 offset = jsonObj.value(QStringLiteral("confirmed_offset"))
                                      .toVariant().toLongLong(&offsetOk);
            const qint64 total = jsonObj.value(QStringLiteral("total_size"))
                                     .toVariant().toLongLong(&totalOk);
            if (route.taskId.isEmpty() || route.threadId <= 0 || route.uid <= 0
                || route.resourceId.isEmpty() || !offsetOk || !totalOk
                || total <= 0 || total > 2 * 1024 * 1024 || offset < 0 || offset > total) {
                emit sig_group_avatar_error(route, tr("群头像上传同步响应或关联身份无效。"));
                return;
            }
            emit sig_group_avatar_sync(route, offset, total,
                                       jsonObj.value(QStringLiteral("completed")).toBool());
            return;
        }
        if (jsonObj["error"].toInt(ErrorCodes::ERR_JSON) != ErrorCodes::SUCCESS) {
            emit sig_upload_error(tr("同步上传任务失败，错误码：%1")
                                      .arg(jsonObj["error"].toInt()),
                                  jsonObj["upload_id"].toString());
            return;
        }

        emit sig_file_sync(jsonObj["confirmed_offset"].toVariant().toLongLong(),
                           jsonObj["total_size"].toVariant().toLongLong(),
                           jsonObj["completed"].toBool(),
                           jsonObj["upload_id"].toString(),
                           jsonObj["resource_url"].toString());
    });

    // 文件上传回包，服务端写入文件成功后才会回包。
    _handler.insert(ResourceReqId::ID_UPLOAD_FILE_RSP, [this](quint16 id, int len, QByteArray data) {
        Q_UNUSED(len);

        //将字节流转换为json文档
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

        //检查转换是否成功
        if(jsonDoc.isNull() || !jsonDoc.isObject()){
            emit sig_upload_error(tr("服务端返回了无效的上传结果。"), QString());
            return;
        }

        //将json文档转换为json对象
        QJsonObject json_obj = jsonDoc.object();
        if (IsGroupAvatarReply(json_obj)) {
            const GroupAvatarRoute route = GroupAvatarRouteFromJson(json_obj);
            const int error = json_obj.value(QStringLiteral("error")).toInt(ErrorCodes::ERR_JSON);
            if (error != ErrorCodes::SUCCESS) {
                emit sig_group_avatar_error(route, GroupAvatarErrorText(error, tr("上传")));
                return;
            }
            bool offsetOk = false;
            bool totalOk = false;
            const qint64 offset = json_obj.value(QStringLiteral("confirmed_offset"))
                                      .toVariant().toLongLong(&offsetOk);
            const qint64 total = json_obj.value(QStringLiteral("total_size"))
                                     .toVariant().toLongLong(&totalOk);
            if (route.taskId.isEmpty() || route.threadId <= 0 || route.uid <= 0
                || route.resourceId.isEmpty() || !offsetOk || !totalOk
                || total <= 0 || total > 2 * 1024 * 1024 || offset < 0 || offset > total) {
                emit sig_group_avatar_error(route, tr("群头像上传回包或关联身份无效。"));
                return;
            }
            emit sig_group_avatar_upload_progress(
                route, offset, total, json_obj.value(QStringLiteral("completed")).toBool());
            return;
        }

        //正常回包必须有error字段，用来判断服务端是否成功写入该分片
        if(!json_obj.contains("error")){
            emit sig_upload_error(tr("上传失败：服务端回包缺少错误码。"),
                                  json_obj["upload_id"].toString());
            return;
        }

        int err = json_obj["error"].toInt();
        if(err != ErrorCodes::SUCCESS){
            const qint64 expectedOffset = json_obj["confirmed_offset"].toVariant().toLongLong();
            emit sig_upload_error(tr("上传失败，错误码：%1；服务端确认到 %2 字节。")
                                      .arg(err).arg(expectedOffset),
                                  json_obj["upload_id"].toString());
            return;
        }

        // confirmed_offset 是服务端实际落盘后的字节数，不能用客户端发送量代替。
        const qint64 confirmedOffset = json_obj["confirmed_offset"].toVariant().toLongLong();
        const qint64 totalSize = json_obj["total_size"].toVariant().toLongLong();
        emit sig_upload_progress(json_obj["upload_id"].toString(), confirmedOffset, totalSize,
                                 json_obj["completed"].toBool(), json_obj["resource_url"].toString());
    });

}



//发送数据槽函数
void ResourceClient::slot_send_msg(quint16 id, QByteArray body)
{
    //如果连接异常则直接返回
    if(_socket->state() != QAbstractSocket::ConnectedState){
        emit sig_net_error(QString("断开连接无法发送"));
        return;
    }

    //获取body的长度
    quint32 bodyLength = body.size();

    //创建字节数组
    QByteArray data;
    //绑定字节数组
    QDataStream stream(&data, QIODevice::WriteOnly);
    //设置大端模式
    stream.setByteOrder(QDataStream::BigEndian);
    //写入ID
    stream << id;
    //写入长度
    stream << bodyLength;
    //写入包体
    data.append(body);

    //发送消息
    _socket->write(data);
}

void ResourceClient::slot_tcp_connect(QString Host, QString Port)
{
    //客户端连接服务器
    qDebug() << "connecting to server..." << Qt::endl;
    _host = Host;
    _port = static_cast<uint16_t>(Port.toUInt()); //QString很好用
    _socket->connectToHost(_host, _port); //通过前面的回调函数知道结果
}


void ResourceClient::sendMsg(quint16 id,QByteArray data)
{
    //发送信号，统一交给槽函数处理，这么做的好处是多线程安全
    emit sig_send_msg(id, data);
}

void ResourceClient::requestDownload(const QString& resourceId, qint64 threadId, qint64 offset,
                                     qint32 chunkSize)
{
    // 参数在客户端先做一次约束，服务端仍会重复校验，不能依赖客户端输入可信。
    const auto userMgr = UserMgr::GetInstance();
    const int uid = userMgr->GetUid();
    const QString token = userMgr->GetToken();
    if (resourceId.isEmpty() || threadId <= 0 || offset < 0 || chunkSize <= 0 || chunkSize > 2048
        || uid <= 0 || token.isEmpty()) {
        emit sig_download_error(tr("下载请求参数无效。"));
        return;
    }

    QJsonObject request;
    request["resource_id"] = resourceId;
    request["thread_id"] = static_cast<double>(threadId);
    request["uid"] = uid;
    request["token"] = token;
    request["offset"] = static_cast<double>(offset);
    request["chunk_size"] = chunkSize;
    sendMsg(ResourceReqId::ID_DOWNLOAD_FILE_REQ,
            QJsonDocument(request).toJson(QJsonDocument::Compact));
}

void ResourceClient::requestGroupAvatarDownload(const QString &taskId, const QString &resourceId,
                                                qint64 threadId, int uid, qint64 offset,
                                                qint32 chunkSize)
{
    const QString token = UserMgr::GetInstance()->GetToken();
    if (taskId.isEmpty() || resourceId.isEmpty() || threadId <= 0 || uid <= 0
        || UserMgr::GetInstance()->GetUid() != uid || token.isEmpty()
        || offset < 0 || chunkSize <= 0 || chunkSize > 2048) {
        emit sig_group_avatar_error(
            GroupAvatarRoute{QStringLiteral("group_avatar"), taskId, threadId, uid, resourceId},
            tr("群头像下载请求参数无效。"));
        return;
    }
    const QJsonObject request{
        {QStringLiteral("purpose"), QStringLiteral("group_avatar")},
        {QStringLiteral("task_id"), taskId},
        {QStringLiteral("uid"), uid},
        {QStringLiteral("token"), token},
        {QStringLiteral("thread_id"), QString::number(threadId)},
        {QStringLiteral("resource_id"), resourceId},
        {QStringLiteral("offset"), static_cast<double>(offset)},
        {QStringLiteral("chunk_size"), chunkSize}
    };
    sendMsg(ResourceReqId::ID_DOWNLOAD_FILE_REQ,
            QJsonDocument(request).toJson(QJsonDocument::Compact));
}

void ResourceClient::syncGroupAvatarUpload(const QString &taskId, qint64 threadId, int uid,
                                           const QString &md5, const QString &name,
                                           qint64 totalSize, qint32 chunkSize)
{
    const QString token = UserMgr::GetInstance()->GetToken();
    if (taskId.isEmpty() || threadId <= 0 || uid <= 0
        || UserMgr::GetInstance()->GetUid() != uid || token.isEmpty()
        || md5.size() != 32 || name.isEmpty() || totalSize <= 0
        || totalSize > 2 * 1024 * 1024 || chunkSize <= 0 || chunkSize > 2048) {
        emit sig_group_avatar_error(
            GroupAvatarRoute{QStringLiteral("group_avatar"), taskId, threadId, uid, taskId},
            tr("群头像上传同步请求参数无效。"));
        return;
    }
    const QJsonObject request{
        {QStringLiteral("purpose"), QStringLiteral("group_avatar")},
        {QStringLiteral("task_id"), taskId},
        {QStringLiteral("uid"), uid},
        {QStringLiteral("token"), token},
        {QStringLiteral("thread_id"), QString::number(threadId)},
        {QStringLiteral("resource_id"), taskId},
        {QStringLiteral("upload_id"), taskId},
        {QStringLiteral("md5"), md5},
        {QStringLiteral("name"), name},
        {QStringLiteral("total_size"), static_cast<double>(totalSize)},
        {QStringLiteral("chunk_size"), chunkSize}
    };
    sendMsg(ResourceReqId::ID_SYNC_FILE_REQ,
            QJsonDocument(request).toJson(QJsonDocument::Compact));
}

void ResourceClient::uploadGroupAvatarChunk(const QString &taskId, qint64 threadId, int uid,
                                            const QString &md5, const QString &name,
                                            qint64 totalSize, qint64 offset,
                                            const QByteArray &chunk, bool isLast)
{
    const QString token = UserMgr::GetInstance()->GetToken();
    if (taskId.isEmpty() || threadId <= 0 || uid <= 0
        || UserMgr::GetInstance()->GetUid() != uid || token.isEmpty()
        || md5.size() != 32 || name.isEmpty() || totalSize <= 0
        || totalSize > 2 * 1024 * 1024 || offset < 0
        || chunk.isEmpty() || chunk.size() > totalSize - offset
        || isLast != (offset + chunk.size() == totalSize)) {
        emit sig_group_avatar_error(
            GroupAvatarRoute{QStringLiteral("group_avatar"), taskId, threadId, uid, taskId},
            tr("群头像上传分片参数无效。"));
        return;
    }
    const QJsonObject request{
        {QStringLiteral("purpose"), QStringLiteral("group_avatar")},
        {QStringLiteral("task_id"), taskId},
        {QStringLiteral("uid"), uid},
        {QStringLiteral("token"), token},
        {QStringLiteral("thread_id"), QString::number(threadId)},
        {QStringLiteral("resource_id"), taskId},
        {QStringLiteral("upload_id"), taskId},
        {QStringLiteral("md5"), md5},
        {QStringLiteral("name"), name},
        {QStringLiteral("total_size"), static_cast<double>(totalSize)},
        {QStringLiteral("offset"), static_cast<double>(offset)},
        {QStringLiteral("data"), QString::fromLatin1(chunk.toBase64())},
        {QStringLiteral("is_last"), isLast}
    };
    sendMsg(ResourceReqId::ID_UPLOAD_FILE_REQ,
            QJsonDocument(request).toJson(QJsonDocument::Compact));
}

