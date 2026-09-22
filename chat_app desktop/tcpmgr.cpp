#include "tcpmgr.h"
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QStringList>
#include <QVariant>
#include <limits>
#include "usermgr.h"
#include "userdata.h"
#include "localchatstoragemgr.h"

namespace {
constexpr int kHeartbeatIntervalMs = 20 * 1000;
constexpr int kHeartbeatResponseTimeoutMs = 60 * 1000;
constexpr qint64 kMaxChatImageBytes = 100LL * 1024 * 1024;

// 1033/1034/1035 只共享图片元数据。这里集中做边界校验，后续 ResourceClient 不会收到
// 空 resource_id、超大尺寸或伪造 MIME 的下载任务。
bool ParseImageArray(const QJsonObject &envelope, QList<std::shared_ptr<ImageChatData>> *images)
{
    const int fromUid = envelope.value("fromuid").toInt();
    const int toUid = envelope.value("touid").toInt();
    const QJsonArray imageArray = envelope.value("imageArray").toArray();
    if (fromUid <= 0 || toUid <= 0 || imageArray.isEmpty()) {
        return false;
    }
    for (const QJsonValue &value : imageArray) {
        const QJsonObject object = value.toObject();
        bool sizeOk = false;
        const qint64 fileSize = object.value("file_size").toVariant().toLongLong(&sizeOk);
        const QString msgId = object.value("msgid").toString();
        const QString resourceId = object.value("resource_id").toString();
        const QString name = object.value("name").toString();
        const QString mimeType = object.value("mime_type").toString();
        const int width = object.value("width").toInt();
        const int height = object.value("height").toInt();
        if (msgId.isEmpty() || resourceId.isEmpty() || name.isEmpty() || !sizeOk || fileSize <= 0
            || fileSize > kMaxChatImageBytes || width <= 0 || height <= 0
            || !mimeType.startsWith(QStringLiteral("image/"))) {
            return false;
        }
        auto image = std::make_shared<ImageChatData>();
        image->messageId = object.value("message_id").toVariant().toLongLong();
        image->threadId = object.value("thread_id").toVariant().toLongLong();
        image->msgId = msgId;
        image->resourceId = resourceId;
        image->name = name;
        image->mimeType = mimeType;
        image->fileSize = fileSize;
        image->width = width;
        image->height = height;
        image->fromUid = fromUid;
        image->toUid = toUid;
        image->deliveryState = envelope.value("realtime_delivered").toBool() ? 2 : 1;
        images->append(image);
    }
    return true;
}

// 1030 离线同步把图片元数据保存到 local_chat_message.content 的紧凑 JSON 中。图片
// 文件本身永远不进入 SQLite；登录后由 ChatImageTransferTask 根据 resource_id 下载。
QString SerializeStoredImage(const QJsonObject &item)
{
    return QString::fromUtf8(QJsonDocument(item).toJson(QJsonDocument::Compact));
}
}

TcpMgr::~TcpMgr()
{
    // 先让 socket 不再投递回调，再销毁 QTimer。这里不能依赖 deleteLater()：
    // 此时主事件循环已经结束，延迟删除不会被处理，反而会让对象析构顺序不确定。
    PrepareForShutdown();

    if (_heartbeat_timer) {
        delete _heartbeat_timer;
        _heartbeat_timer = nullptr;
    }

    if (_socket) {
        delete _socket;
        _socket = nullptr;
    }
}

bool TcpMgr::IsConnected() const
{
    return _socket && _socket->state() == QAbstractSocket::ConnectedState;
}

void TcpMgr::CloseConnection()
{
    StopHeartbeat();
    if (_socket) {
        _socket->disconnect();
        _socket->abort();
        _socket->deleteLater();
        _socket = nullptr;
    }
    _buffer.clear();
    _b_recy_pending = false;
    _logged_in = false;
    _disconnect_notified = false;

    // 信号和槽是绑定在具体对象上的，需要重新连接信号和槽
    // initSigAndSlot(); 此时_socket为空，在重新进行长连接的时候再连接信号和槽

}

void TcpMgr::PrepareForShutdown()
{
    // 关闭窗口会使 QCoreApplication::exec() 返回。socket 的 abort() 可能同步发出
    // disconnected；若此时仍让回调弹出对话框或重启逻辑，就会在退出析构阶段重入 UI。
    // 因此退出路径只清理网络资源，不再对外发送“连接断开”通知。
    _is_shutting_down = true;
    _logged_in = false;
    _disconnect_notified = true;
    StopHeartbeat();

    if (_socket) {
        // initSigAndSlot() 中的连接都把 TcpMgr 作为 context；只断开这组连接，
        // 避免 abort() 同步触发 lambda 后访问正在退出的对象。
        QObject::disconnect(_socket, nullptr, this, nullptr);
        _socket->abort();
    }
}

//Qt封装的是异步，我们要在构造函数里面完成各种信号的槽，保证服务的流程进行
TcpMgr::TcpMgr() : _host(""), _port(0), _b_recy_pending(false), _logged_in(false),
    _disconnect_notified(false), _is_shutting_down(false), _message_id(0), _message_len(0),
    _heartbeat_timer(new QTimer(this))
{
    //连接服务器，在这里不好写第一个参数，我们去到LoginDialog里面写
    //connect(, &LoginDialog::sig_connect_tcp, this, &TcpMgr::slot_tcp_connect);

    _socket = new QTcpSocket(this);
    // 连接信号和槽
    initSigAndSlot();

    //连接发送信号用来发送数据，在哪里发送信号呢？可以是对话框中点击发送消息信号，在很多地方都可以，我们设计好槽函数及参数就可以统一处理
    // 其他地方把数据传过来，这里发给服务器
    connect(this, &TcpMgr::sig_send_data, this, &TcpMgr::slot_send_data);

    // 心跳仅在 TCP 登录成功后启动；定时器同时承担“发送下一次 ping”和“检查上次 pong”的职责。
    _heartbeat_timer->setInterval(kHeartbeatIntervalMs);
    connect(_heartbeat_timer, &QTimer::timeout, this, [this] {
        SendHeartbeat();
    });

    // 注册消息
    initHandlers();
}

void TcpMgr::initHandlers()
{
    //连接聊天服务器和发送消息过去是分开的
    //登录聊天服务器回包，这一块是和tcp服务通信，和logindialog里面的不一样
    _handler.insert(ReqId::ID_CHAT_LOGIN_RSP, [this](ReqId id, int len, QByteArray data){
        Q_UNUSED(len);
        qDebug() << "handle id is " << id << "data is " << data;

        //将字节流转换为json文档
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

        //检查转换是否成功
        if(jsonDoc.isNull()){
            qDebug() << "failed to create QJsonDocument";
            emit sig_login_failed(ErrorCodes::ERR_JSON);
            return;
        }

        //将json文档转换为json对象
        QJsonObject json_obj = jsonDoc.object();

        if(!json_obj.contains("error")){ //正常解析成功会有error键
            int err = ErrorCodes::ERR_JSON;
            qDebug() << "Login Failed, err is Json Prase Err" << err;
            emit sig_login_failed(err);
            return;
        }

        int err = json_obj["error"].toInt();

        if(err != ErrorCodes::SUCCESS){
            qDebug() << "Login Failed, err is" << err;
            emit sig_login_failed(err);
            return;
        }

        int uid = json_obj["uid"].toInt();
        int sex = json_obj["sex"].toInt();
        QString name = json_obj["user"].toString();
        QString icon = json_obj["icon"].toString();
        QString desc = json_obj["desc"].toString();
        QString nick = json_obj["nick"].toString();
        auto userinfo = std::make_shared<UserInfo>(uid, name, nick, desc, sex, icon);
        auto userMgr = UserMgr::GetInstance();
        userMgr->SetUserInfo(std::move(userinfo));
        userMgr->SetToken(json_obj["token"].toString());

        // 本地缓存以登录 uid 隔离。首次登录自动建库建表，已有数据库则读取会话摘要；
        // 缓存打开失败不应阻断在线登录，后续消息仍可正常收发，只是不能离线查看。
        if (!LocalChatStorageMgr::GetInstance()->Initialize(uid)) {
            qWarning() << "local chat cache is unavailable:"
                       << LocalChatStorageMgr::GetInstance()->LastError();
        }

        std::vector<std::shared_ptr<ApplyInfo>> applyList;
        const QJsonArray applyArray = json_obj.value("apply_list").toArray();
        for (const auto &value : applyArray) {
            const QJsonObject apply = value.toObject();
            applyList.push_back(std::make_shared<ApplyInfo>(
                apply.value("uid").toInt(),
                apply.value("name").toString(),
                apply.value("desc").toString(),
                apply.value("icon").toString(),
                apply.value("nick").toString(),
                apply.value("sex").toInt(),
                apply.value("status").toInt()));
        }
        userMgr->SetApplyList(std::move(applyList));

        std::vector<std::shared_ptr<UserInfo>> friendList;
        const QJsonArray friendArray = json_obj.value("friend_list").toArray();
        for (const auto &value : friendArray) {
            const QJsonObject friendObject = value.toObject();
            friendList.push_back(std::make_shared<UserInfo>(
                friendObject.value("uid").toInt(),
                friendObject.value("name").toString(),
                friendObject.value("nick").toString(),
                friendObject.value("desc").toString(),
                friendObject.value("sex").toInt(),
                friendObject.value("icon").toString()));
        }
        userMgr->SetFriendList(std::move(friendList));

        _logged_in = true;
        _disconnect_notified = false;
        StartHeartbeat();

        // 本地最大 thread_id 是“已发现会话”的游标。登录后先拉取比它更新的私聊；
        // 空数据库传 0，即可分页获得该账号全部私聊。
        QJsonObject loadThreadsRequest;
        loadThreadsRequest["after_thread_id"] = QString::number(
            LocalChatStorageMgr::GetInstance()->MaxKnownThreadId());
        loadThreadsRequest["page_size"] = 100;
        emit sig_send_data(ID_LOAD_CHAT_THREAD_REQ,
                           QJsonDocument(loadThreadsRequest).toJson(QJsonDocument::Compact));

        // 已知会话也可能在本账号离线期间收到了新消息；逐会话使用自己的游标同步。
        const auto storage = LocalChatStorageMgr::GetInstance();
        for (const LocalChatThread &thread : storage->CachedThreads()) {
            if (thread.threadType != QStringLiteral("private") || thread.threadId <= 0) {
                continue;
            }
            QJsonObject loadMessagesRequest;
            loadMessagesRequest["thread_id"] = QString::number(thread.threadId);
            loadMessagesRequest["after_message_id"] = QString::number(
                storage->SyncCursors().value(thread.threadId, 0));
            loadMessagesRequest["page_size"] = 100;
            emit sig_send_data(ID_LOAD_CHAT_MSG_REQ,
                               QJsonDocument(loadMessagesRequest).toJson(QJsonDocument::Compact));
        }
        emit sig_switch_chatdlg();
    });

    // 搜索列表搜索回包
    _handler.insert(ReqId::ID_SEARCH_USER_RSP, [this](ReqId id, int len, QByteArray data){
        Q_UNUSED(len);
        qDebug() << "handle id is " << id << "data is " << data;

        // 搜索失败统一走 sig_user_search(nullptr)，让 SearchList 弹 FindFailDlg
        auto emit_search_fail = [this]{
            std::shared_ptr<SearchInfo> null_si;
            emit sig_user_search(null_si);
        };

        //将字节流转换为json文档
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

        //检查转换是否成功
        if(jsonDoc.isNull()){
            qDebug() << "failed to create QJsonDocument";
            emit_search_fail();
            return;
        }

        //将json文档转换为json对象
        QJsonObject json_obj = jsonDoc.object();

        if(!json_obj.contains("error")){ //正常解析成功会有error键
            int err = ErrorCodes::ERR_JSON;
            qDebug() << "Search Failed, err is Json Parse Err" << err;
            emit_search_fail();
            return;
        }

        int err = json_obj["error"].toInt();

        if(err != ErrorCodes::SUCCESS){
            qDebug() << "Search Failed, err is" << err;
            emit_search_fail();
            return;
        }

        auto si = std::make_shared<SearchInfo>(json_obj["uid"].toInt(),
                                               json_obj["user"].toString(),
                                               json_obj["nick"].toString(),
                                               json_obj["desc"].toString(),
                                               json_obj["sex"].toInt());

        emit sig_user_search(si);
    });

    // 客户端监听服务器发来的添加好友请求，由新朋友那一栏接收信号做出反应
    // 这个id是通知用户好友申请
    _handler.insert(ID_NOTIFY_ADD_FRIEND_REQ, [this](ReqId id, int len, QByteArray data){
        Q_UNUSED(len);
        qDebug() << "handle id is " << id << "data is " << data;

        //将字节流转换为json文档
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

        //检查转换是否成功
        if(jsonDoc.isNull()){
            qDebug() << "failed to create QJsonDocument";
            return;
        }

        //将json文档转换为json对象
        QJsonObject json_obj = jsonDoc.object();

        if(!json_obj.contains("error")){ //正常解析成功会有error键
            int err = ErrorCodes::ERR_JSON;
            qDebug() << "notify friend Failed, err is Json Parse Err" << err;
            return;
        }

        int err = json_obj["error"].toInt();

        if(err != ErrorCodes::SUCCESS){
            qDebug() << "notify friend Failed, err is" << err;
            return;
        }

        // 当前 ChatServer 协议字段：applyuid、name、nick、desc、sex、icon。
        auto apply_user = std::make_shared<AddFriendApply>(json_obj.value("applyuid").toInt(),
                                               json_obj.value("name").toString(),
                                               json_obj.value("nick").toString(),
                                               json_obj.value("desc").toString(),
                                               json_obj.value("sex").toInt(),
                                               json_obj.value("icon").toString());
        emit sig_friend_apply(apply_user);
    });

    // 认证者收到 1014 回包、原申请人收到 1015 通知，二者携带相同的好友资料和
    // textmsgs 初始消息。当前客户端只解析该正式协议，不再兼容历史字段名。
    auto handle_auth_friend = [this](ReqId id, int len, QByteArray data){
        Q_UNUSED(len);
        qDebug() << "handle id is " << id << "data is " << data;

        //将字节流转换为json文档
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

        //检查转换是否成功
        if(jsonDoc.isNull()){
            qDebug() << "failed to create QJsonDocument";
            return;
        }

        //将json文档转换为json对象
        QJsonObject json_obj = jsonDoc.object();

        if(!json_obj.contains("error")){ //正常解析成功会有error键
            int err = ErrorCodes::ERR_JSON;
            qDebug() << "auth friend Failed, err is Json Parse Err" << err;
            return;
        }

        int err = json_obj["error"].toInt();

        if(err != ErrorCodes::SUCCESS){
            qDebug() << "auth friend Failed, err is" << err;
            return;
        }

        int uid = json_obj["uid"].toInt();
        QString name = json_obj["name"].toString();
        QString nick = json_obj["nick"].toString();
        QString bakname = json_obj["bakname"].toString();
        QString desc = json_obj["desc"].toString();
        int sex = json_obj["sex"].toInt();
        QString icon = json_obj["icon"].toString();
        const qint64 receiverId = json_obj.value("touid").toVariant().toLongLong();

        auto authResult = std::make_shared<FriendAuthResult>();
        authResult->friendInfo = std::make_shared<FriendInfo>(uid, name, nick, desc, icon, bakname, sex);

        QJsonArray textMessages = json_obj.value("textmsgs").toArray();
        for (const QJsonValue &value : textMessages) {
            const QJsonObject textObj = value.toObject();
            bool messageIdOk = false;
            bool threadIdOk = false;
            const qint64 messageId = textObj.value("msg_id").toVariant().toLongLong(&messageIdOk);
            const qint64 threadId = textObj.value("thread_id").toVariant().toLongLong(&threadIdOk);
            const qint64 senderId = textObj.value("sender_id").toVariant().toLongLong();
            const QString uniqueId = textObj.value("unique_id").toString();
            const QString content = textObj.value("msgcontent").toString();
            if (!messageIdOk || !threadIdOk || messageId <= 0 || threadId <= 0
                || senderId <= 0 || receiverId <= 0 || uniqueId.isEmpty() || content.isEmpty()) {
                qWarning() << "ignore invalid friend auth text message";
                continue;
            }

            authResult->textMessages.append(std::make_shared<TextChatData>(
                messageId, uniqueId, threadId, content, senderId, receiverId));
        }

        emit sig_auth_friend(authResult);

    };

    _handler.insert(ID_AUTH_FRIEND_RSP, handle_auth_friend);
    _handler.insert(ID_NOTIFY_AUTH_FRIEND_REQ, handle_auth_friend);

    // 1028 是“会话已由服务端确认”的边界：只有拿到这个回包中的 thread_id，
    // 客户端才能把好友条目升级为可用于同步和持久化的正式聊天会话。
    _handler.insert(ID_CREATE_PRIVATE_CHAT_RSP, [this](ReqId id, int len, QByteArray data) {
        Q_UNUSED(id);
        Q_UNUSED(len);
        const QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
        if (!jsonDoc.isObject()) {
            qWarning() << "create private chat response is not a JSON object";
            return;
        }

        const QJsonObject jsonObj = jsonDoc.object();
        const int error = jsonObj.value("error").toInt(ErrorCodes::ERR_JSON);
        if (error != ErrorCodes::SUCCESS) {
            qWarning() << "create private chat failed, error:" << error;
            return;
        }
        // JsonCpp 服务端写入的是 UINT64；QJson 的数值经 QVariant 转换后再取无符号值，
        // 避免先转成 int 造成高位 thread_id 截断。
        bool threadIdOk = false;
        const qulonglong rawThreadId = jsonObj.value("thread_id").toVariant().toULongLong(&threadIdOk);
        // SQLite 的 INTEGER 和当前 UI 都使用有符号 64 位 ID，拒绝超出可表示范围的服务端值。
        if (rawThreadId > static_cast<qulonglong>(std::numeric_limits<qint64>::max())) {
            threadIdOk = false;
        }
        const qint64 threadId = threadIdOk ? static_cast<qint64>(rawThreadId) : 0;
        const int uid = jsonObj.value("uid").toInt();
        const int otherUid = jsonObj.value("other_id").toInt();
        if (!threadIdOk || threadId <= 0 || uid <= 0 || otherUid <= 0 || uid == otherUid) {
            qWarning() << "create private chat response has invalid fields";
            return;
        }

        // TcpMgr 不直接操作 UI 或 SQLite，交给 ChatDialog 在同一条 UI 流程中完成。
        emit sig_create_private_chat(uid, otherUid, threadId);
    });

    _handler.insert(ID_LOAD_CHAT_THREAD_RSP, [this](ReqId id, int len, QByteArray data) {
        Q_UNUSED(id);
        Q_UNUSED(len);
        const QJsonDocument document = QJsonDocument::fromJson(data);
        if (!document.isObject()) {
            qWarning() << "load chat threads response is not a JSON object";
            return;
        }
        const QJsonObject response = document.object();
        if (response.value("error").toInt(ErrorCodes::ERR_JSON) != ErrorCodes::SUCCESS) {
            qWarning() << "load chat threads failed:" << response.value("error").toInt();
            return;
        }
        const auto currentUser = UserMgr::GetInstance()->GetUserInfo();
        const auto storage = LocalChatStorageMgr::GetInstance();
        if (!currentUser || !storage->IsReady() || response.value("uid").toInt() != currentUser->_uid) {
            return;
        }

        for (const QJsonValue &value : response.value("threads").toArray()) {
            const QJsonObject item = value.toObject();
            bool threadIdOk = false;
            const qint64 threadId = item.value("thread_id").toVariant().toLongLong(&threadIdOk);
            const int user1Id = item.value("user1_id").toInt();
            const int user2Id = item.value("user2_id").toInt();
            const int peerUid = user1Id == currentUser->_uid ? user2Id : user1Id;
            if (!threadIdOk || threadId <= 0 || peerUid <= 0 || peerUid == currentUser->_uid) {
                continue;
            }
            LocalChatThread thread;
            thread.threadId = threadId;
            thread.threadType = QStringLiteral("private");
            thread.peerUid = peerUid;
            for (const auto &friendInfo : UserMgr::GetInstance()->GetFriendList()) {
                if (friendInfo && friendInfo->_uid == peerUid) {
                    thread.title = friendInfo->_name;
                    break;
                }
            }
            if (thread.title.isEmpty()) {
                thread.title = QString::number(peerUid);
            }
            // 增量发现会话只补齐元数据，不能将已有的本地消息摘要、时间和未读数清零。
            for (const LocalChatThread &cachedThread : storage->CachedThreads()) {
                if (cachedThread.threadId == threadId) {
                    thread.lastMessageId = cachedThread.lastMessageId;
                    thread.lastMessagePreview = cachedThread.lastMessagePreview;
                    thread.lastMessageAtMs = cachedThread.lastMessageAtMs;
                    thread.unreadCount = cachedThread.unreadCount;
                    break;
                }
            }
            if (!storage->UpsertThread(thread)) {
                qWarning() << "save synced chat thread failed:" << storage->LastError();
                continue;
            }
            emit sig_create_private_chat(currentUser->_uid, peerUid, threadId);

            // 每个会话独立维护 message_id 游标：新会话从 0 开始，已有会话只取缺失部分。
            QJsonObject loadMessagesRequest;
            loadMessagesRequest["thread_id"] = QString::number(threadId);
            loadMessagesRequest["after_message_id"] = QString::number(
                storage->SyncCursors().value(threadId, 0));
            loadMessagesRequest["page_size"] = 100;
            emit sig_send_data(ID_LOAD_CHAT_MSG_REQ,
                               QJsonDocument(loadMessagesRequest).toJson(QJsonDocument::Compact));
        }

        if (response.value("load_more").toBool()) {
            QJsonObject nextRequest;
            nextRequest["after_thread_id"] = response.value("next_thread_id").toVariant().toString();
            nextRequest["page_size"] = 100;
            emit sig_send_data(ID_LOAD_CHAT_THREAD_REQ,
                               QJsonDocument(nextRequest).toJson(QJsonDocument::Compact));
        }
    });

    _handler.insert(ID_LOAD_CHAT_MSG_RSP, [this](ReqId id, int len, QByteArray data) {
        Q_UNUSED(id);
        Q_UNUSED(len);
        const QJsonDocument document = QJsonDocument::fromJson(data);
        if (!document.isObject()) {
            qWarning() << "load chat messages response is not a JSON object";
            return;
        }
        const QJsonObject response = document.object();
        if (response.value("error").toInt(ErrorCodes::ERR_JSON) != ErrorCodes::SUCCESS) {
            qWarning() << "load chat messages failed:" << response.value("error").toInt();
            return;
        }
        bool threadIdOk = false;
        const qint64 threadId = response.value("thread_id").toVariant().toLongLong(&threadIdOk);
        const auto currentUser = UserMgr::GetInstance()->GetUserInfo();
        const auto storage = LocalChatStorageMgr::GetInstance();
        if (!threadIdOk || threadId <= 0 || !currentUser || !storage->IsReady()) {
            return;
        }

        LocalChatThread thread;
        bool foundThread = false;
        for (const LocalChatThread &cachedThread : storage->CachedThreads()) {
            if (cachedThread.threadId == threadId) {
                thread = cachedThread;
                foundThread = true;
                break;
            }
        }
        if (!foundThread) {
            qWarning() << "ignore messages for an unknown local thread:" << threadId;
            return;
        }

        QList<LocalChatMessage> localMessages;
        qint64 maxMessageId = storage->SyncCursors().value(threadId, 0);
        const qint64 previousSyncCursor = maxMessageId;
        for (const QJsonValue &value : response.value("messages").toArray()) {
            const QJsonObject item = value.toObject();
            bool messageIdOk = false;
            const qint64 messageId = item.value("message_id").toVariant().toLongLong(&messageIdOk);
            if (!messageIdOk || messageId <= 0) {
                continue;
            }
            LocalChatMessage message;
            message.messageId = messageId;
            message.threadId = threadId;
            message.senderId = item.value("sender_id").toInt();
            message.recvId = item.value("recv_id").toInt();
            message.contentType = item.value("message_type").toString(QStringLiteral("text"));
            if (message.contentType != QStringLiteral("text")
                && message.contentType != QStringLiteral("image")) {
                qWarning() << "ignore unsupported synced message type:" << message.contentType;
                continue;
            }
            message.content = item.value("content").toString();
            message.createdAtMs = item.value("created_at_ms").toVariant().toLongLong();
            message.updatedAtMs = message.createdAtMs;
            message.serverStatus = item.value("status").toInt();
            message.sendState = 3;
            if (message.senderId == currentUser->_uid) {
                message.deliveryState = message.serverStatus == 1 ? 4
                    : (item.value("peer_displayed").toBool() ? 3 : 1);
            }
            message.isRead = message.senderId == currentUser->_uid;
            if (message.senderId <= 0 || (message.contentType == QStringLiteral("text")
                                          && message.content.isEmpty())) {
                continue;
            }
            if (message.contentType == QStringLiteral("image")) {
                // 复用 1034/1035 的严格字段校验。将单项包装为统一图片信封，可避免
                // 离线同步因字段缺失而在之后的下载阶段才暴露错误。
                QJsonObject envelope;
                envelope["fromuid"] = message.senderId;
                envelope["touid"] = message.recvId;
                envelope["imageArray"] = QJsonArray{item};
                QList<std::shared_ptr<ImageChatData>> images;
                if (!ParseImageArray(envelope, &images) || images.size() != 1) {
                    qWarning() << "ignore invalid offline image metadata, message_id=" << message.messageId;
                    continue;
                }
                images.front()->messageId = message.messageId;
                images.front()->threadId = threadId;
                // 存服务端回包的完整可信元数据；content 对 image 不展示给用户。
                message.content = SerializeStoredImage(item);
            }
            localMessages.append(message);
            maxMessageId = qMax(maxMessageId, message.messageId);
            // 登录增量同步的每条新入站未读消息都要计入摘要；实时 1019 的路径
            // 已自行计数。当前会话随后会由 ChatDialog::SetCurrentChatUser 原子清零。
            if (message.messageId > previousSyncCursor && message.senderId != currentUser->_uid
                && message.serverStatus == 0) {
                ++thread.unreadCount;
            }
            if (message.messageId >= thread.lastMessageId) {
                thread.lastMessageId = message.messageId;
                thread.lastMessagePreview = message.contentType == QStringLiteral("image")
                    ? QStringLiteral("[图片]") : message.content;
                thread.lastMessageAtMs = message.createdAtMs;
            }
        }

        if (!localMessages.isEmpty() && !storage->SaveReceivedMessages(thread, localMessages, maxMessageId)) {
            qWarning() << "save synced chat messages failed:" << storage->LastError();
            return;
        }
        if (!localMessages.isEmpty()) {
            // 先确保 SQLite 事务提交成功，再让 UI 从缓存刷新；不能提前发信号，
            // 否则 ChatDialog 可能读到旧摘要或不完整消息页。
            emit sig_local_chat_synced(threadId);
        }

        if (response.value("load_more").toBool()) {
            QJsonObject nextRequest;
            nextRequest["thread_id"] = QString::number(threadId);
            nextRequest["after_message_id"] = response.value("next_message_id").toVariant().toString();
            nextRequest["page_size"] = 100;
            emit sig_send_data(ID_LOAD_CHAT_MSG_REQ,
                               QJsonDocument(nextRequest).toJson(QJsonDocument::Compact));
        }
    });

    _handler.insert(ID_TEXT_CHAT_MSG_RSP, [this](ReqId id, int len, QByteArray data){
        Q_UNUSED(id);
        Q_UNUSED(len);
        const QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
        if (!jsonDoc.isObject()) {
            qWarning() << "text chat response is not a JSON object";
            return;
        }

        const QJsonObject jsonObj = jsonDoc.object();
        const int error = jsonObj.value("error").toInt(ErrorCodes::ERR_JSON);
        const QJsonArray textArray = jsonObj.value("textArray").toArray();
        if (error != ErrorCodes::SUCCESS) {
            // 未被服务端确认的发送消息没有可靠的 message_id，不能写入以服务端
            // message_id 为主键的本地表；发送失败状态仍由现有发送 UI 处理。
            for (const QJsonValue &value : textArray) {
                const QString uniqueId = value.toObject().value("msgid").toString();
                if (!uniqueId.isEmpty()) {
                    emit sig_text_chat_send_result(uniqueId, false);
                }
            }
            return;
        }
        const int deliveryState = jsonObj.value("realtime_delivered").toBool()
            ? 2 : 1; // 1018 成功必为已持久化；实时投递是独立、可选的第二阶段。

        const auto currentUser = UserMgr::GetInstance()->GetUserInfo();
        const auto storage = LocalChatStorageMgr::GetInstance();
        if (!currentUser || !storage->IsReady()) {
            return;
        }

        // 一次 1017 可批量发送多条文本。按 thread_id 聚合后，每个会话只提交一
        // 次 SQLite 事务，避免每确认一条消息就重复刷新会话列表和当前聊天窗口。
        QHash<qint64, LocalChatThread> threads;
        QHash<qint64, QList<LocalChatMessage>> messagesByThread;
        QHash<qint64, qint64> maxMessageIds;
        QHash<qint64, QStringList> uniqueIdsByThread;
        for (const QJsonValue &value : textArray) {
            const QJsonObject textObj = value.toObject();
            bool messageIdOk = false;
            bool threadIdOk = false;
            bool createdAtOk = false;
            const qint64 messageId = textObj.value("message_id").toVariant()
                                         .toLongLong(&messageIdOk);
            const qint64 threadId = textObj.value("thread_id").toVariant()
                                        .toLongLong(&threadIdOk);
            const qint64 senderId = textObj.value("sender_id").toVariant().toLongLong();
            const qint64 recvId = textObj.value("recv_id").toVariant().toLongLong();
            const qint64 createdAtMs = textObj.value("created_at_ms").toVariant()
                                           .toLongLong(&createdAtOk);
            const int status = textObj.value("status").toInt(-1);
            const QString uniqueId = textObj.value("msgid").toString();
            const QString content = textObj.value("content").toString();
            if (!messageIdOk || messageId <= 0 || threadId <= 0 || senderId != currentUser->_uid
                || recvId <= 0 || recvId == currentUser->_uid || !createdAtOk || createdAtMs <= 0
                || status < 0 || status > 2 || uniqueId.isEmpty() || content.isEmpty()) {
                qWarning() << "ignore invalid confirmed text message" << messageId << threadId;
                // 服务端回包标记为成功却缺少持久化元数据时，不能确认乐观气泡。
                // 若仍能取到客户端 UUID，则显示失败状态，避免界面永久停留在发送中。
                if (!uniqueId.isEmpty()) {
                    emit sig_text_chat_send_result(uniqueId, false);
                }
                continue;
            }

            if (!threads.contains(threadId)) {
                LocalChatThread thread;
                thread.threadId = threadId;
                thread.threadType = QStringLiteral("private");
                thread.peerUid = recvId;
                thread.title = QString::number(recvId);
                for (const auto &friendInfo : UserMgr::GetInstance()->GetFriendList()) {
                    if (friendInfo && friendInfo->_uid == recvId) {
                        thread.title = friendInfo->_name;
                        break;
                    }
                }
                // 发送回包可能和登录增量同步并发到达。已有摘要优先保留，只用
                // 本批次中更大的 message_id 覆盖，不能让迟到的确认回包倒退摘要。
                for (const LocalChatThread &cachedThread : storage->CachedThreads()) {
                    if (cachedThread.threadId == threadId) {
                        thread = cachedThread;
                        thread.threadType = QStringLiteral("private");
                        thread.peerUid = recvId;
                        if (thread.title.isEmpty()) {
                            thread.title = QString::number(recvId);
                        }
                        break;
                    }
                }
                threads.insert(threadId, thread);
            }

            LocalChatThread &thread = threads[threadId];
            if (messageId >= thread.lastMessageId) {
                thread.lastMessageId = messageId;
                thread.lastMessagePreview = content;
                thread.lastMessageAtMs = createdAtMs;
            }
            thread.updatedAtMs = createdAtMs;

            LocalChatMessage localMessage;
            localMessage.messageId = messageId;
            localMessage.threadId = threadId;
            localMessage.senderId = senderId;
            localMessage.recvId = recvId;
            localMessage.contentType = QStringLiteral("text");
            localMessage.content = content;
            localMessage.createdAtMs = createdAtMs;
            localMessage.updatedAtMs = createdAtMs;
            localMessage.serverStatus = status;
            localMessage.sendState = 3;
            localMessage.deliveryState = deliveryState;
            localMessage.isRead = true;
            messagesByThread[threadId].append(localMessage);
            maxMessageIds[threadId] = qMax(maxMessageIds.value(threadId, 0), messageId);
            uniqueIdsByThread[threadId].append(uniqueId);
        }

        for (auto threadIter = threads.cbegin(); threadIter != threads.cend(); ++threadIter) {
            const qint64 threadId = threadIter.key();
            if (!storage->SaveReceivedMessages(threadIter.value(), messagesByThread.value(threadId),
                                               maxMessageIds.value(threadId))) {
                qWarning() << "save confirmed text messages failed:" << storage->LastError();
                for (const QString &uniqueId : uniqueIdsByThread.value(threadId)) {
                    emit sig_text_chat_send_result(uniqueId, false);
                }
                continue;
            }
            // SQLite 事务提交后才确认乐观气泡，并通知 UI 从本地记录刷新当前会话。
            for (const QString &uniqueId : uniqueIdsByThread.value(threadId)) {
                emit sig_text_chat_send_result(uniqueId, true, deliveryState);
            }
            emit sig_local_chat_synced(threadId);
        }
    });

    _handler.insert(ID_NOTIFY_TEXT_CHAT_MSG_REQ, [this](ReqId id, int len, QByteArray data){
        Q_UNUSED(id);
        Q_UNUSED(len);
        const QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
        if (!jsonDoc.isObject()) {
            qWarning() << "text chat notification is not a JSON object";
            return;
        }

        const QJsonObject jsonObj = jsonDoc.object();
        if (jsonObj.value("error").toInt(ErrorCodes::ERR_JSON) != ErrorCodes::SUCCESS) {
            qWarning() << "text chat notification failed:" << jsonObj.value("error").toInt();
            return;
        }

        const int fromUid = jsonObj.value("fromuid").toInt();
        const int toUid = jsonObj.value("touid").toInt();
        const QJsonArray textArray = jsonObj.value("textArray").toArray();
        if (fromUid <= 0 || toUid <= 0 || textArray.isEmpty()) {
            qWarning() << "text chat notification has invalid envelope";
            return;
        }
        qInfo() << "received text chat notification, from:" << fromUid
                << "to:" << toUid << "count:" << textArray.size();
        for (const QJsonValue &value : textArray) {
            const QJsonObject textObj = value.toObject();
            bool messageIdOk = false;
            bool threadIdOk = false;
            bool createdAtOk = false;
            const qint64 messageId = textObj.value("message_id").toVariant()
                                         .toLongLong(&messageIdOk);
            const qint64 threadId = textObj.value("thread_id").toVariant()
                                       .toLongLong(&threadIdOk);
            const QString uniqueId = textObj.value("msgid").toString();
            const QString content = textObj.value("content").toString();
            const qint64 senderId = textObj.value("sender_id").toVariant().toLongLong();
            const qint64 recvId = textObj.value("recv_id").toVariant().toLongLong();
            const qint64 createdAtMs = textObj.value("created_at_ms").toVariant()
                                           .toLongLong(&createdAtOk);
            const int status = textObj.value("status").toInt(-1);
            // 文本通知必须来自已持久化的服务端消息。缺少任何元数据时直接拒绝，
            // 不再降级为内存消息，确保聊天列表、SQLite 和同步游标只有一个事实来源。
            if (!messageIdOk || !threadIdOk || !createdAtOk || messageId <= 0 || threadId <= 0
                || senderId != fromUid || recvId != toUid || uniqueId.isEmpty() || content.isEmpty()
                || createdAtMs <= 0 || status < 0 || status > 2) {
                qWarning() << "ignore incomplete text chat notification";
                continue;
            }

            auto message = std::make_shared<TextChatData>(messageId, uniqueId, threadId,
                                                           content, senderId, recvId,
                                                           status, createdAtMs);
            emit sig_text_chat(message);
        }
    });

    // 1034 是发送方的确认，1035 是接收方的通知。二者遵循同一个图片信封；确认
    // 失败时仍解析 msgid，以便界面把对应的乐观图片气泡标记为发送失败。
    auto handle_image_chat = [this](bool isSendResult, ReqId id, int len, QByteArray data) {
        Q_UNUSED(id);
        Q_UNUSED(len);
        const QJsonDocument document = QJsonDocument::fromJson(data);
        if (!document.isObject()) {
            qWarning() << "image chat message is not a JSON object";
            return;
        }
        const QJsonObject envelope = document.object();
        QList<std::shared_ptr<ImageChatData>> images;
        if (!ParseImageArray(envelope, &images)) {
            // 1035 到达但字段不完整时明确打印完整信封，避免误把客户端解析失败当成
            // ChatServer 跨服路由失败。图片二进制不会输出到日志。
            qWarning() << "image chat message has invalid envelope:" << document.toJson(QJsonDocument::Compact);
            return;
        }
        const auto currentUser = UserMgr::GetInstance()->GetUserInfo();
        if (!currentUser) {
            return;
        }
        const int error = envelope.value("error").toInt(ErrorCodes::SUCCESS);
        qInfo() << (isSendResult ? "received image chat confirmation (1034)"
                                 : "received image chat notification (1035)")
                << "from=" << envelope.value("fromuid").toInt()
                << "to=" << envelope.value("touid").toInt()
                << "count=" << images.size() << "error=" << error;
        if (isSendResult && error != ErrorCodes::SUCCESS) {
            qWarning() << "image chat request rejected by ChatServer, error:" << error;
        }
        // 信号沿用现有 TextChatData 的 shared_ptr& 约定；这里不能用 const auto&，
        // 否则无法把列表内的智能指针交给 Qt 信号的可变引用参数。
        for (auto &image : images) {
            if ((isSendResult && (image->fromUid != currentUser->_uid || image->toUid == currentUser->_uid))
                || (!isSendResult && (image->toUid != currentUser->_uid || image->fromUid == currentUser->_uid))) {
                qWarning() << "ignore image chat message whose envelope does not belong to current user";
                continue;
            }
            if (isSendResult) {
                emit sig_image_chat_send_result(image, error == ErrorCodes::SUCCESS);
            } else if (error == ErrorCodes::SUCCESS) {
                qInfo() << "start receiving image download task, resource_id=" << image->resourceId;
                emit sig_image_chat(image);
            }
        }
    };
    _handler.insert(ID_IMAGE_CHAT_MSG_RSP, [handle_image_chat](ReqId id, int len, QByteArray data) {
        handle_image_chat(true, id, len, data);
    });
    _handler.insert(ID_NOTIFY_IMAGE_CHAT_MSG_REQ, [handle_image_chat](ReqId id, int len, QByteArray data) {
        handle_image_chat(false, id, len, data);
    });

    _handler.insert(ID_NOTIFY_MESSAGE_DISPLAYED, [this](ReqId id, int len, QByteArray data) {
        Q_UNUSED(id);
        Q_UNUSED(len);
        const QJsonDocument document = QJsonDocument::fromJson(data);
        if (!document.isObject()) {
            return;
        }
        const QJsonObject payload = document.object();
        bool threadOk = false;
        const qint64 threadId = payload.value("thread_id").toVariant().toLongLong(&threadOk);
        const auto currentUser = UserMgr::GetInstance()->GetUserInfo();
        if (payload.value("error").toInt(ErrorCodes::ERR_JSON) != ErrorCodes::SUCCESS || !threadOk
            || threadId <= 0 || !payload.value("peer_displayed").toBool() || !currentUser) {
            return;
        }
        QList<qint64> messageIds;
        for (const QJsonValue &value : payload.value("message_ids").toArray()) {
            bool idOk = false;
            const qint64 messageId = value.toVariant().toLongLong(&idOk);
            if (idOk && messageId > 0 && !messageIds.contains(messageId)) {
                messageIds.append(messageId);
            }
        }
        const auto storage = LocalChatStorageMgr::GetInstance();
        if (messageIds.isEmpty() || !storage->IsReady()
            || !storage->UpdateDeliveryState(messageIds, 3)) {
            return;
        }
        emit sig_message_delivery_updated(threadId, messageIds, 3);
    });

    _handler.insert(ID_NOTIFY_THREAD_READ, [this](ReqId id, int len, QByteArray data) {
        Q_UNUSED(id);
        Q_UNUSED(len);
        const QJsonDocument document = QJsonDocument::fromJson(data);
        if (!document.isObject()) {
            return;
        }
        const QJsonObject payload = document.object();
        bool threadOk = false;
        bool throughOk = false;
        const qint64 threadId = payload.value("thread_id").toVariant().toLongLong(&threadOk);
        const qint64 readThrough = payload.value("read_through_message_id").toVariant().toLongLong(&throughOk);
        const qint64 readerUid = payload.value("reader_id").toVariant().toLongLong();
        const auto storage = LocalChatStorageMgr::GetInstance();
        if (payload.value("error").toInt(ErrorCodes::ERR_JSON) != ErrorCodes::SUCCESS || !threadOk
            || !throughOk || threadId <= 0 || readThrough <= 0 || readerUid <= 0 || !storage->IsReady()) {
            return;
        }
        const QList<qint64> messageIds = storage->MarkOutgoingMessagesRead(threadId, readerUid, readThrough);
        if (!messageIds.isEmpty()) {
            emit sig_message_delivery_updated(threadId, messageIds, 4);
        }
    });

    _handler[ID_NOTIFY_OFF_LINE_REQ] = [this](ReqId id, int len, QByteArray data){
        Q_UNUSED(id);
        Q_UNUSED(len);
        const QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
        if (!jsonDoc.isObject()) {
            qWarning() << "text chat notification is not a JSON object";
            return;
        }

        const QJsonObject jsonObj = jsonDoc.object();
        if (jsonObj.value("error").toInt(ErrorCodes::ERR_JSON) != ErrorCodes::SUCCESS) {
            qWarning() << "text chat notification failed:" << jsonObj.value("error").toInt();
            return;
        }
        if (_disconnect_notified) {
            return;
        }
        _disconnect_notified = true;
        _logged_in = false;
        StopHeartbeat(); // 弹窗可能进入嵌套事件循环，先停止，避免期间继续发送心跳。
        emit sig_off_line();
    };

    _handler[ID_HEARTBEAT_RSP] = [this](ReqId id, int len, QByteArray data){
        Q_UNUSED(id);
        Q_UNUSED(len);
        const QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
        if (!jsonDoc.isObject()
            || jsonDoc.object().value("error").toInt(ErrorCodes::ERR_JSON) != ErrorCodes::SUCCESS) {
            qWarning() << "invalid heartbeat response";
            return;
        }

        // 只接受当前已登录连接的成功心跳回复，用单调时钟避免系统时间调整影响超时判断。
        if (_logged_in) {
            _last_heartbeat_rsp.restart();
        }
    };

    // 资料回包沿用 ChatServer 的 2+2 字节 framing 与 {"error": ...} 风格。
    // SettingDialog 收到成功结果后才更新 UserMgr，避免请求失败却显示成已保存。
    _handler[ID_UPDATE_USER_PROFILE_RSP] = [this](ReqId id, int len, QByteArray data) {
        Q_UNUSED(id);
        Q_UNUSED(len);
        const QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
        if (!jsonDoc.isObject() || !jsonDoc.object().contains("error")) {
            qWarning() << "invalid update profile response";
            emit sig_update_profile_result(ErrorCodes::ERR_JSON);
            return;
        }
        emit sig_update_profile_result(jsonDoc.object().value("error").toInt(ErrorCodes::ERR_JSON));
    };

}

void TcpMgr::initSigAndSlot()
{
    //_socket连接服务器成功之后，发送信号通知一下
    connect(_socket, &QTcpSocket::connected, this, [this](){
        if (_is_shutting_down) {
            return;
        }
        qDebug() << "connect to server" << Qt::endl;
        emit sig_con_success(true);
    });

    // 记录断开原因，便于区分客户端主动退出、服务端关闭和网络错误。
    connect(_socket, &QTcpSocket::disconnected, this, [this]{
        if (_is_shutting_down) {
            return;
        }
        qWarning() << "disconnected from server:" << _socket->errorString();
        const bool should_notify = _logged_in && !_disconnect_notified;
        _logged_in = false;
        StopHeartbeat();
        _disconnect_notified = true;
        _buffer.clear();
        _b_recy_pending = false;
        if (should_notify) {
            emit sig_connection_lost();
        }
    });

    //在有数据可读时候进行处理
    connect(_socket, &QTcpSocket::readyRead, this, [this](){
        if (_is_shutting_down || !_socket) {
            return;
        }
        //读取所有数据到缓冲区
        _buffer.append(_socket->readAll());

        // 一次 readyRead 可能包含多个完整包；半包则保留已读包头，等下次数据补齐包体。
        while (true) {
            //解析头部，消息头是消息id + 消息长度 每个是short，两个字节
            if(!_b_recy_pending){
                //检查缓冲区中的数据是否足够解析出一个消息头，不够就返回
                //使用static_cast<> 实现更加安全的类型转换
                if(_buffer.size() < static_cast<int>(sizeof(quint16)*2)){
                    return;
                }

                QDataStream stream(&_buffer, QIODevice::ReadOnly);
                stream.setVersion(QDataStream::Qt_6_0);
                stream.setByteOrder(QDataStream::BigEndian);

                //预读取消息id和消息长度
                stream >> _message_id >> _message_len;

                //将buffer中前四个字节移除，mid截取一段
                _buffer = _buffer.mid(sizeof(quint16)*2);

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
            QByteArray messageBody = _buffer.mid(0,_message_len);
            qDebug() << "receive message : " << messageBody << Qt::endl;
            _buffer = _buffer.mid(_message_len);
            _b_recy_pending = false;

            //处理收到的数据
            auto iter = _handler.find(ReqId(_message_id));
            if(iter == _handler.end()){
                qDebug() << "id error" << Qt::endl;
                continue;
            }

            //执行处理函数
            iter.value()(ReqId(_message_id), _message_len, messageBody);
        }

    });

    //处理错误，直接问ai
    connect(_socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::errorOccurred),
            this, [this](QAbstractSocket::SocketError socketError){
                if (_is_shutting_down || !_socket) {
                    return;
                }
                Q_UNUSED(socketError);
                qDebug() << "Error : " << _socket->errorString();
            });
}

void TcpMgr::slot_tcp_connect(ServerInfo si)
{
    if (!_socket) {
        _socket = new QTcpSocket(this);
        initSigAndSlot();          // 新对象，重新绑
    }
    _is_shutting_down = false;
    StopHeartbeat();
    _logged_in = false;
    _disconnect_notified = false;
    //客户端连接服务器
    qDebug() << "connecting to server..." << Qt::endl;
    _host = si.Host;
    _port = static_cast<uint16_t>(si.Port.toUInt()); //QString很好用
    _socket->connectToHost(_host, _port); //这个也是异步的，通过前面的回调函数知道结果
}

void TcpMgr::StartHeartbeat()
{
    if (!_logged_in || !IsConnected()) {
        return;
    }

    // 先建立有效的单调计时起点，再立即发送首个 ping，随后每 20 秒发送。
    _last_heartbeat_rsp.start();
    _heartbeat_timer->start();
    SendHeartbeat();
}

void TcpMgr::StopHeartbeat()
{
    if (_heartbeat_timer) {
        _heartbeat_timer->stop();
    }
    _last_heartbeat_rsp.invalidate();
}

void TcpMgr::SendHeartbeat()
{
    if (!_logged_in || !IsConnected()) {
        StopHeartbeat();
        return;
    }

    // 连续 60 秒未收到服务端 1024，连接即使仍显示 ConnectedState 也按假在线处理。
    if (!_last_heartbeat_rsp.isValid()
        || _last_heartbeat_rsp.elapsed() >= kHeartbeatResponseTimeoutMs) {
        qWarning() << "heartbeat response timed out; closing stale TCP connection";

        // 心跳超时有单独的界面提示。先标记本次断线已经通知，避免 abort() 触发
        // disconnected 后又发送 sig_connection_lost，造成连续弹出两个对话框。
        _disconnect_notified = true;
        _logged_in = false;
        StopHeartbeat();
        _socket->abort();
        emit sig_heartbeat_timeout();
        return;
    }

    QJsonObject heartbeat_req;
    heartbeat_req["client_time"] = QString::number(QDateTime::currentMSecsSinceEpoch());
    emit sig_send_data(ID_HEART_BEAT_REQ,QJsonDocument(heartbeat_req).toJson(QJsonDocument::Compact));
}



void TcpMgr::slot_send_data(ReqId reqId, QByteArray dataByte)
{
    if (!IsConnected()) {
        qWarning() << "cannot send TCP message: socket is not connected";
        return;
    }

    if (dataByte.size() > std::numeric_limits<quint16>::max()) {
        qWarning() << "cannot send TCP message: body exceeds protocol limit";
        return;
    }

    uint16_t id = reqId;

    // 计算长度，使用网络字节序转换
    quint16 len = static_cast<quint16>(dataByte.size());

    //创建一个QByteArray用于存储要发送的所有数据，也就是拼接一下
    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);

    //设置数据流使用网络字节序
    out.setByteOrder(QDataStream::BigEndian);

    out << id << len;
    block.append(dataByte);

    //发送数据
    if (_socket->write(block) < 0) {
        qWarning() << "TCP write failed:" << _socket->errorString();
    }
}
