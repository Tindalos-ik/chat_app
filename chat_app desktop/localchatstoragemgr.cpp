#include "localchatstoragemgr.h"

#include <QDateTime>
#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

namespace {
constexpr int kDefaultThreadLimit = 100;
constexpr int kDefaultMessageLimit = 50;

int NormalizeLimit(int value, int fallback)
{
    // 防止调用方无意传入过大的值而一次性把大量数据加载到 UI 线程。
    return qBound(1, value > 0 ? value : fallback, 1000);
}

LocalChatThread ReadThread(QSqlQuery &query)
{
    LocalChatThread thread;
    thread.threadId = query.value(0).toLongLong();
    thread.threadType = query.value(1).toString();
    thread.title = query.value(2).toString();
    thread.peerUid = query.value(3).toLongLong();
    thread.lastMessageId = query.value(4).toLongLong();
    thread.lastMessagePreview = query.value(5).toString();
    thread.lastMessageAtMs = query.value(6).toLongLong();
    thread.unreadCount = query.value(7).toInt();
    thread.updatedAtMs = query.value(8).toLongLong();
    return thread;
}

LocalChatMessage ReadMessage(QSqlQuery &query)
{
    LocalChatMessage message;
    message.messageId = query.value(0).toLongLong();
    message.threadId = query.value(1).toLongLong();
    message.senderId = query.value(2).toLongLong();
    message.recvId = query.value(3).toLongLong();
    message.contentType = query.value(4).toString();
    message.content = query.value(5).toString();
    message.createdAtMs = query.value(6).toLongLong();
    message.updatedAtMs = query.value(7).toLongLong();
    message.serverStatus = query.value(8).toInt();
    message.sendState = query.value(9).toInt();
    message.deliveryState = query.value(10).toInt();
    message.isRead = query.value(11).toInt() != 0;
    return message;
}
} // namespace

LocalChatStorageMgr::LocalChatStorageMgr() = default;

LocalChatStorageMgr::~LocalChatStorageMgr()
{
    Close();
}

bool LocalChatStorageMgr::Initialize(qint64 uid)
{
    if (uid <= 0) {
        _lastError = QStringLiteral("无法为无效 uid 创建本地聊天数据库");
        qWarning() << _lastError << uid;
        return false;
    }

    if (IsReady() && _uid == uid) {
        _cachedThreads = LoadThreads();
        return true;
    }

    Close();
    _lastError.clear();
    _uid = uid;
    _connectionName = QStringLiteral("local_chat_cache_%1").arg(uid);

    const QString appDataPath = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    const QString cacheDirectory = QDir(appDataPath).filePath(QStringLiteral("chat_cache"));
    if (!QDir().mkpath(cacheDirectory)) {
        _lastError = QStringLiteral("无法创建本地聊天缓存目录：%1").arg(cacheDirectory);
        qWarning() << _lastError;
        _uid = 0;
        return false;
    }

    _databasePath = QDir(cacheDirectory).filePath(
        QStringLiteral("chat_cache_%1.db").arg(uid));
    _database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), _connectionName);
    _database.setDatabaseName(_databasePath);
    if (!_database.open()) {
        SetError(QStringLiteral("打开 SQLite 数据库失败"));
        Close();
        return false;
    }

    // foreign_keys 是 SQLite 的“每连接”设置，所以即使数据库已存在也必须重新开启。
    if (!Execute(QStringLiteral("PRAGMA foreign_keys = ON"))
        || !Execute(QStringLiteral("PRAGMA journal_mode = WAL"))
        || !CreateSchema()) {
        Close();
        return false;
    }

    _cachedThreads = LoadThreads();
    qInfo() << "local chat cache ready for uid" << uid << "at" << _databasePath
            << ", loaded thread summaries:" << _cachedThreads.size();
    return true;
}

void LocalChatStorageMgr::Close()
{
    _cachedThreads.clear();
    _uid = 0;
    _databasePath.clear();

    const QString connectionName = _connectionName;
    _connectionName.clear();
    if (_database.isValid()) {
        _database.close();
    }
    // QSqlDatabase::removeDatabase 前必须解除当前对象对连接的引用。
    _database = QSqlDatabase();
    if (!connectionName.isEmpty() && QSqlDatabase::contains(connectionName)) {
        QSqlDatabase::removeDatabase(connectionName);
    }
}

bool LocalChatStorageMgr::IsReady() const
{
    return _uid > 0 && _database.isValid() && _database.isOpen();
}

qint64 LocalChatStorageMgr::CurrentUid() const
{
    return _uid;
}

QString LocalChatStorageMgr::DatabasePath() const
{
    return _databasePath;
}

QString LocalChatStorageMgr::LastError() const
{
    return _lastError;
}

QList<LocalChatThread> LocalChatStorageMgr::CachedThreads() const
{
    return _cachedThreads;
}

QList<LocalChatThread> LocalChatStorageMgr::LoadThreads(int limit)
{
    QList<LocalChatThread> threads;
    if (!IsReady()) {
        return threads;
    }

    QSqlQuery query(_database);
    query.prepare(QStringLiteral(
        "SELECT thread_id, thread_type, title, peer_uid, last_message_id, "
        "last_message_preview, last_message_at_ms, unread_count, updated_at_ms "
        "FROM local_chat_thread "
        "ORDER BY last_message_at_ms DESC, thread_id DESC LIMIT ?"));
    query.addBindValue(NormalizeLimit(limit, kDefaultThreadLimit));
    if (!query.exec()) {
        SetError(QStringLiteral("读取本地会话列表失败"));
        return threads;
    }

    while (query.next()) {
        threads.append(ReadThread(query));
    }
    return threads;
}

QList<LocalChatMessage> LocalChatStorageMgr::LoadRecentMessages(qint64 threadId, int limit)
{
    // 内层倒序取最新 N 条，外层恢复为时间正序，界面可直接从上到下添加气泡。
    return QueryMessages(QStringLiteral(
                             "SELECT message_id, thread_id, sender_id, recv_id, content_type, content, "
                              "created_at_ms, updated_at_ms, server_status, send_state, delivery_state, is_read "
                             "FROM (SELECT message_id, thread_id, sender_id, recv_id, content_type, content, "
                              "created_at_ms, updated_at_ms, server_status, send_state, delivery_state, is_read "
                             "FROM local_chat_message WHERE thread_id = ? "
                             "ORDER BY message_id DESC LIMIT ?) ORDER BY message_id ASC"),
                         threadId, 0, limit);
}

QList<LocalChatMessage> LocalChatStorageMgr::LoadMessagesBefore(qint64 threadId,
                                                                 qint64 beforeMessageId,
                                                                 int limit)
{
    if (beforeMessageId <= 0) {
        return LoadRecentMessages(threadId, limit);
    }

    return QueryMessages(QStringLiteral(
                             "SELECT message_id, thread_id, sender_id, recv_id, content_type, content, "
                              "created_at_ms, updated_at_ms, server_status, send_state, delivery_state, is_read "
                             "FROM (SELECT message_id, thread_id, sender_id, recv_id, content_type, content, "
                              "created_at_ms, updated_at_ms, server_status, send_state, delivery_state, is_read "
                             "FROM local_chat_message "
                             "WHERE thread_id = ? AND message_id < ? "
                             "ORDER BY message_id DESC LIMIT ?) ORDER BY message_id ASC"),
                         threadId, beforeMessageId, limit);
}

bool LocalChatStorageMgr::UpsertThread(const LocalChatThread &thread)
{
    if (!IsReady() || thread.threadId <= 0
        || (thread.threadType != QStringLiteral("private")
            && thread.threadType != QStringLiteral("group"))) {
        _lastError = QStringLiteral("保存本地会话失败：会话数据无效");
        return false;
    }

    if (!_database.transaction()) {
        SetError(QStringLiteral("开启本地会话事务失败"));
        return false;
    }
    if (!UpsertThreadInTransaction(thread) || !_database.commit()) {
        _database.rollback();
        if (_lastError.isEmpty()) {
            SetError(QStringLiteral("提交本地会话事务失败"));
        }
        return false;
    }
    _cachedThreads = LoadThreads();
    return true;
}

bool LocalChatStorageMgr::SaveReceivedMessages(const LocalChatThread &thread,
                                                const QList<LocalChatMessage> &messages,
                                                qint64 maxSyncedMessageId)
{
    if (!IsReady() || thread.threadId <= 0 || maxSyncedMessageId < 0) {
        _lastError = QStringLiteral("保存本地消息失败：参数无效");
        return false;
    }
    if (!_database.transaction()) {
        SetError(QStringLiteral("开启本地消息事务失败"));
        return false;
    }

    bool success = UpsertThreadInTransaction(thread);
    QSqlQuery messageQuery(_database);
    if (success) {
        success = messageQuery.prepare(QStringLiteral(
            "INSERT INTO local_chat_message "
            "(message_id, thread_id, sender_id, recv_id, content_type, content, created_at_ms, "
            "updated_at_ms, server_status, send_state, delivery_state, is_read) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(message_id) DO UPDATE SET "
            "thread_id = excluded.thread_id, sender_id = excluded.sender_id, "
            "recv_id = excluded.recv_id, content_type = excluded.content_type, "
            "content = excluded.content, updated_at_ms = excluded.updated_at_ms, "
            "server_status = MAX(local_chat_message.server_status, excluded.server_status), "
            "send_state = excluded.send_state, "
            "delivery_state = MAX(local_chat_message.delivery_state, excluded.delivery_state), "
            "is_read = MAX(local_chat_message.is_read, excluded.is_read)"));
        if (!success) {
            SetError(QStringLiteral("准备本地消息写入失败"));
        }
    }

    for (const LocalChatMessage &message : messages) {
        if (!success) {
            break;
        }
        if (message.messageId <= 0 || message.threadId != thread.threadId) {
            _lastError = QStringLiteral("保存本地消息失败：消息 ID 或会话 ID 无效");
            success = false;
            break;
        }
        messageQuery.bindValue(0, message.messageId);
        messageQuery.bindValue(1, message.threadId);
        messageQuery.bindValue(2, message.senderId);
        messageQuery.bindValue(3, message.recvId);
        messageQuery.bindValue(4, message.contentType);
        messageQuery.bindValue(5, message.content);
        messageQuery.bindValue(6, message.createdAtMs);
        messageQuery.bindValue(7, message.updatedAtMs > 0 ? message.updatedAtMs : message.createdAtMs);
        messageQuery.bindValue(8, message.serverStatus);
        messageQuery.bindValue(9, message.sendState);
        messageQuery.bindValue(10, message.deliveryState);
        messageQuery.bindValue(11, message.isRead ? 1 : 0);
        if (!messageQuery.exec()) {
            SetError(QStringLiteral("写入本地消息失败"));
            success = false;
        }
    }

    if (success) {
        QSqlQuery cursorQuery(_database);
        cursorQuery.prepare(QStringLiteral(
            "INSERT INTO local_sync_cursor (thread_id, max_message_id, synced_at_ms) "
            "VALUES (?, ?, ?) "
            "ON CONFLICT(thread_id) DO UPDATE SET "
            "max_message_id = MAX(max_message_id, excluded.max_message_id), "
            "synced_at_ms = excluded.synced_at_ms"));
        cursorQuery.addBindValue(thread.threadId);
        cursorQuery.addBindValue(maxSyncedMessageId);
        cursorQuery.addBindValue(QDateTime::currentMSecsSinceEpoch());
        if (!cursorQuery.exec()) {
            SetError(QStringLiteral("更新本地同步游标失败"));
            success = false;
        }
    }

    if (!success || !_database.commit()) {
        _database.rollback();
        if (success) {
            SetError(QStringLiteral("提交本地消息事务失败"));
        }
        return false;
    }
    _cachedThreads = LoadThreads();
    return true;
}

QHash<qint64, qint64> LocalChatStorageMgr::SyncCursors() const
{
    QHash<qint64, qint64> cursors;
    if (!IsReady()) {
        return cursors;
    }

    QSqlQuery query(_database);
    if (!query.exec(QStringLiteral("SELECT thread_id, max_message_id FROM local_sync_cursor"))) {
        const_cast<LocalChatStorageMgr *>(this)->SetError(QStringLiteral("读取本地同步游标失败"));
        return cursors;
    }
    while (query.next()) {
        cursors.insert(query.value(0).toLongLong(), query.value(1).toLongLong());
    }
    return cursors;
}

qint64 LocalChatStorageMgr::MaxKnownThreadId() const
{
    if (!IsReady()) {
        return 0;
    }

    QSqlQuery query(_database);
    if (!query.exec(QStringLiteral("SELECT COALESCE(MAX(thread_id), 0) FROM local_chat_thread"))
        || !query.next()) {
        const_cast<LocalChatStorageMgr *>(this)->SetError(QStringLiteral("读取最大本地会话 ID 失败"));
        return 0;
    }
    return query.value(0).toLongLong();
}

bool LocalChatStorageMgr::SetSyncCursor(qint64 threadId, qint64 maxMessageId)
{
    if (!IsReady() || threadId <= 0 || maxMessageId < 0) {
        _lastError = QStringLiteral("更新本地同步游标失败：参数无效");
        return false;
    }
    QSqlQuery query(_database);
    query.prepare(QStringLiteral(
        "INSERT INTO local_sync_cursor (thread_id, max_message_id, synced_at_ms) VALUES (?, ?, ?) "
        "ON CONFLICT(thread_id) DO UPDATE SET "
        "max_message_id = MAX(max_message_id, excluded.max_message_id), "
        "synced_at_ms = excluded.synced_at_ms"));
    query.addBindValue(threadId);
    query.addBindValue(maxMessageId);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!query.exec()) {
        SetError(QStringLiteral("更新本地同步游标失败"));
        return false;
    }
    return true;
}

bool LocalChatStorageMgr::MarkThreadRead(qint64 threadId)
{
    if (!IsReady() || threadId <= 0 || !_database.transaction()) {
        _lastError = QStringLiteral("标记本地消息已读失败：数据库未就绪或会话 ID 无效");
        return false;
    }

    QSqlQuery messageQuery(_database);
    messageQuery.prepare(QStringLiteral(
        "UPDATE local_chat_message SET is_read = 1 "
        "WHERE thread_id = ? AND is_read = 0"));
    messageQuery.addBindValue(threadId);
    QSqlQuery threadQuery(_database);
    threadQuery.prepare(QStringLiteral(
        "UPDATE local_chat_thread SET unread_count = 0, updated_at_ms = ? "
        "WHERE thread_id = ?"));
    threadQuery.addBindValue(QDateTime::currentMSecsSinceEpoch());
    threadQuery.addBindValue(threadId);

    if (!messageQuery.exec() || !threadQuery.exec() || !_database.commit()) {
        _database.rollback();
        SetError(QStringLiteral("标记本地消息已读失败"));
        return false;
    }
    _cachedThreads = LoadThreads();
    return true;
}

bool LocalChatStorageMgr::UpdateDeliveryState(const QList<qint64> &messageIds, int deliveryState)
{
    if (!IsReady() || messageIds.isEmpty() || deliveryState < 1 || deliveryState > 4) {
        return false;
    }
    QSqlQuery query(_database);
    if (!query.prepare(QStringLiteral(
            "UPDATE local_chat_message SET delivery_state = MAX(delivery_state, ?) WHERE message_id = ?"))) {
        SetError(QStringLiteral("准备更新本地投递状态失败"));
        return false;
    }
    for (qint64 messageId : messageIds) {
        if (messageId <= 0) {
            return false;
        }
        query.bindValue(0, deliveryState);
        query.bindValue(1, messageId);
        if (!query.exec()) {
            SetError(QStringLiteral("更新本地投递状态失败"));
            return false;
        }
    }
    return true;
}

QList<qint64> LocalChatStorageMgr::MarkOutgoingMessagesRead(qint64 threadId, qint64 readerUid,
                                                             qint64 readThroughMessageId)
{
    QList<qint64> messageIds;
    if (!IsReady() || threadId <= 0 || readerUid <= 0 || readThroughMessageId <= 0) {
        return messageIds;
    }
    QSqlQuery select(_database);
    select.prepare(QStringLiteral(
        "SELECT message_id FROM local_chat_message "
        "WHERE thread_id = ? AND sender_id != ? AND message_id <= ? AND server_status = 0"));
    select.addBindValue(threadId);
    select.addBindValue(readerUid);
    select.addBindValue(readThroughMessageId);
    if (!select.exec()) {
        SetError(QStringLiteral("读取本地已读回执目标失败"));
        return messageIds;
    }
    while (select.next()) {
        messageIds.append(select.value(0).toLongLong());
    }
    if (messageIds.isEmpty()) {
        return messageIds;
    }
    QSqlQuery update(_database);
    update.prepare(QStringLiteral(
        "UPDATE local_chat_message SET server_status = 1, delivery_state = MAX(delivery_state, 4) "
        "WHERE message_id = ?"));
    for (qint64 messageId : messageIds) {
        update.bindValue(0, messageId);
        if (!update.exec()) {
            SetError(QStringLiteral("更新本地已读回执失败"));
            return {};
        }
    }
    return messageIds;
}

bool LocalChatStorageMgr::CreateSchema()
{
    // 所有建表使用 IF NOT EXISTS，可安全在每次登录时调用；后续字段变更应追加版本迁移。
    const QList<QString> statements = {
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS local_chat_thread ("
            "thread_id INTEGER PRIMARY KEY, "
            "thread_type TEXT NOT NULL CHECK (thread_type IN ('private', 'group')), "
            "title TEXT NOT NULL DEFAULT '', peer_uid INTEGER NOT NULL DEFAULT 0, "
            "last_message_id INTEGER NOT NULL DEFAULT 0, "
            "last_message_preview TEXT NOT NULL DEFAULT '', "
            "last_message_at_ms INTEGER NOT NULL DEFAULT 0, "
            "unread_count INTEGER NOT NULL DEFAULT 0 CHECK (unread_count >= 0), "
            "updated_at_ms INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS local_sync_cursor ("
            "thread_id INTEGER PRIMARY KEY, max_message_id INTEGER NOT NULL DEFAULT 0, "
            "synced_at_ms INTEGER NOT NULL DEFAULT 0, "
            "FOREIGN KEY (thread_id) REFERENCES local_chat_thread(thread_id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS local_chat_message ("
            "message_id INTEGER PRIMARY KEY, thread_id INTEGER NOT NULL, sender_id INTEGER NOT NULL, "
            "recv_id INTEGER NOT NULL DEFAULT 0, "
            "content_type TEXT NOT NULL DEFAULT 'text' "
            "CHECK (content_type IN ('text', 'image', 'audio', 'file', 'system')), "
            "content TEXT NOT NULL DEFAULT '', created_at_ms INTEGER NOT NULL, "
            "updated_at_ms INTEGER NOT NULL, "
            "server_status INTEGER NOT NULL DEFAULT 0 CHECK (server_status IN (0, 1, 2)), "
            "send_state INTEGER NOT NULL DEFAULT 1 CHECK (send_state IN (0, 1, 2, 3)), "
            "delivery_state INTEGER NOT NULL DEFAULT 1 CHECK (delivery_state IN (1, 2, 3, 4)), "
            "is_read INTEGER NOT NULL DEFAULT 1 CHECK (is_read IN (0, 1)), "
            "FOREIGN KEY (thread_id) REFERENCES local_chat_thread(thread_id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS local_message_attachment ("
            "attachment_id INTEGER PRIMARY KEY, message_id INTEGER NOT NULL, "
            "local_path TEXT NOT NULL DEFAULT '', remote_url TEXT NOT NULL DEFAULT '', "
            "mime_type TEXT NOT NULL DEFAULT '', file_size INTEGER NOT NULL DEFAULT 0 CHECK (file_size >= 0), "
            "download_state INTEGER NOT NULL DEFAULT 0 CHECK (download_state IN (0, 1, 2, 3)), "
            "FOREIGN KEY (message_id) REFERENCES local_chat_message(message_id) ON DELETE CASCADE)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_local_message_thread_id "
                       "ON local_chat_message(thread_id, message_id DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_local_message_created_at "
                       "ON local_chat_message(created_at_ms)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_local_attachment_message "
                       "ON local_message_attachment(message_id)")
    };

    for (const QString &statement : statements) {
        if (!Execute(statement)) {
            return false;
        }
    }
    // 已存在的每账号 SQLite 文件不会重新执行 CREATE TABLE；单独探测并迁移，避免
    // 1037/1039 到达时因缺字段而丢失回执状态。
    QSqlQuery columns(_database);
    if (!columns.exec(QStringLiteral("PRAGMA table_info(local_chat_message)"))) {
        SetError(QStringLiteral("读取本地消息表结构失败"));
        return false;
    }
    bool hasDeliveryState = false;
    while (columns.next()) {
        hasDeliveryState = hasDeliveryState || columns.value(1).toString() == QStringLiteral("delivery_state");
    }
    if (!hasDeliveryState && !Execute(QStringLiteral(
            "ALTER TABLE local_chat_message ADD COLUMN delivery_state INTEGER NOT NULL DEFAULT 1 "
            "CHECK (delivery_state IN (1, 2, 3, 4))"))) {
        return false;
    }
    return Execute(QStringLiteral("PRAGMA user_version = 2"));
}

bool LocalChatStorageMgr::Execute(const QString &sql)
{
    QSqlQuery query(_database);
    if (query.exec(sql)) {
        return true;
    }
    SetError(QStringLiteral("执行 SQLite 初始化语句失败"));
    qWarning().noquote() << sql;
    return false;
}

bool LocalChatStorageMgr::UpsertThreadInTransaction(const LocalChatThread &thread)
{
    if (thread.threadId <= 0 || (thread.threadType != QStringLiteral("private")
                                 && thread.threadType != QStringLiteral("group"))) {
        _lastError = QStringLiteral("保存本地会话失败：会话类型或 ID 无效");
        return false;
    }

    QSqlQuery query(_database);
    query.prepare(QStringLiteral(
        "INSERT INTO local_chat_thread "
        "(thread_id, thread_type, title, peer_uid, last_message_id, last_message_preview, "
        "last_message_at_ms, unread_count, updated_at_ms) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(thread_id) DO UPDATE SET "
        "thread_type = excluded.thread_type, title = excluded.title, peer_uid = excluded.peer_uid, "
        "last_message_id = excluded.last_message_id, "
        "last_message_preview = excluded.last_message_preview, "
        "last_message_at_ms = excluded.last_message_at_ms, "
        "unread_count = excluded.unread_count, updated_at_ms = excluded.updated_at_ms"));
    query.addBindValue(thread.threadId);
    query.addBindValue(thread.threadType);
    query.addBindValue(thread.title);
    query.addBindValue(thread.peerUid);
    query.addBindValue(thread.lastMessageId);
    query.addBindValue(thread.lastMessagePreview);
    query.addBindValue(thread.lastMessageAtMs);
    query.addBindValue(thread.unreadCount);
    query.addBindValue(thread.updatedAtMs > 0 ? thread.updatedAtMs : QDateTime::currentMSecsSinceEpoch());
    if (!query.exec()) {
        SetError(QStringLiteral("保存本地会话失败"));
        return false;
    }
    return true;
}

QList<LocalChatMessage> LocalChatStorageMgr::QueryMessages(const QString &sql,
                                                            qint64 threadId,
                                                            qint64 anchorMessageId,
                                                            int limit)
{
    QList<LocalChatMessage> messages;
    if (!IsReady() || threadId <= 0) {
        return messages;
    }

    QSqlQuery query(_database);
    query.prepare(sql);
    query.addBindValue(threadId);
    if (anchorMessageId > 0) {
        query.addBindValue(anchorMessageId);
    }
    query.addBindValue(NormalizeLimit(limit, kDefaultMessageLimit));
    if (!query.exec()) {
        SetError(QStringLiteral("读取本地聊天历史失败"));
        return messages;
    }
    while (query.next()) {
        messages.append(ReadMessage(query));
    }
    return messages;
}

void LocalChatStorageMgr::SetError(const QString &operation)
{
    const QString detail = _database.lastError().text();
    _lastError = detail.isEmpty() ? operation : operation + QStringLiteral("：") + detail;
    qWarning() << _lastError;
}
