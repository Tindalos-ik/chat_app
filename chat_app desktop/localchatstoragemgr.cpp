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
    thread.ownerUid = query.value(4).toLongLong();
    thread.active = query.value(5).toInt() != 0;
    thread.role = query.value(6).toInt();
    thread.invalidReason = query.value(7).toString();
    thread.version = query.value(8).toLongLong();
    thread.membershipVersion = query.value(9).toLongLong();
    thread.approvalRequired = query.value(10).toInt() != 0;
    thread.announcement = query.value(11).toString();
    thread.announcementVersion = query.value(12).toLongLong();
    thread.announcementUpdatedAtMs = query.value(13).toLongLong();
    thread.announcementAuthorUid = query.value(14).toLongLong();
    thread.avatarResourceId = query.value(15).toString();
    thread.avatarVersion = query.value(16).toLongLong();
    thread.avatarMimeType = query.value(17).toString();
    thread.avatarSize = query.value(18).toLongLong();
    thread.avatarWidth = query.value(19).toInt();
    thread.avatarHeight = query.value(20).toInt();
    thread.lastMessageId = query.value(21).toLongLong();
    thread.lastMessagePreview = query.value(22).toString();
    thread.lastMessageAtMs = query.value(23).toLongLong();
    thread.unreadCount = query.value(24).toInt();
    thread.updatedAtMs = query.value(25).toLongLong();
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
    message.senderNick = query.value(8).toString();
    message.serverStatus = query.value(9).toInt();
    message.sendState = query.value(10).toInt();
    message.deliveryState = query.value(11).toInt();
    message.isRead = query.value(12).toInt() != 0;
    return message;
}
} // namespace

LocalChatStorageMgr::LocalChatStorageMgr() = default;

LocalChatStorageMgr::~LocalChatStorageMgr()
{
    Close();
}

bool LocalChatStorageMgr::Initialize(qint64 uid, const QString &cacheRoot)
{
    if (uid <= 0) {
        _lastError = QStringLiteral("无法为无效 uid 创建本地聊天数据库");
        qWarning() << _lastError << uid;
        return false;
    }

    if (!cacheRoot.isEmpty() && !QDir::isAbsolutePath(cacheRoot)) {
        _lastError = QStringLiteral("聊天缓存根目录必须是绝对路径");
        return false;
    }

    const QString appDataPath = cacheRoot.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        : QDir::cleanPath(cacheRoot);
    const QString expectedPath = QDir(QDir(appDataPath).filePath(
        QStringLiteral("chat_cache"))).filePath(
            QStringLiteral("chat_cache_%1.db").arg(uid));
    if (IsReady() && _uid == uid && _databasePath == expectedPath) {
        _cachedThreads = LoadThreads();
        return true;
    }

    Close();
    _lastError.clear();
    _uid = uid;
    _connectionName = QStringLiteral("local_chat_cache_%1").arg(uid);

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
    if (!IsReady()) return threads;
    QSqlQuery query(_database);
    query.prepare(QStringLiteral(
        "SELECT thread_id, thread_type, title, peer_uid, owner_uid, active, role, invalid_reason, "
        "version, membership_version, approval_required, announcement, announcement_version, "
        "announcement_updated_at_ms, announcement_author_uid, avatar_resource_id, avatar_version, "
        "avatar_mime_type, avatar_size, avatar_width, avatar_height, last_message_id, "
        "last_message_preview, last_message_at_ms, unread_count, updated_at_ms FROM local_chat_thread "
        "ORDER BY last_message_at_ms DESC, thread_id DESC LIMIT ?"));
    query.addBindValue(NormalizeLimit(limit, kDefaultThreadLimit));
    if (!query.exec()) {
        SetError(QStringLiteral("读取本地会话列表失败"));
        return threads;
    }
    while (query.next()) threads.append(ReadThread(query));
    return threads;
}
QList<LocalChatThread> LocalChatStorageMgr::LoadAllThreads()
{
    QList<LocalChatThread> threads;
    if (!IsReady()) return threads;
    QSqlQuery query(_database);
    if (!query.exec(QStringLiteral(
            "SELECT thread_id, thread_type, title, peer_uid, owner_uid, active, role, invalid_reason, "
            "version, membership_version, approval_required, announcement, announcement_version, "
            "announcement_updated_at_ms, announcement_author_uid, avatar_resource_id, avatar_version, "
            "avatar_mime_type, avatar_size, avatar_width, avatar_height, last_message_id, "
            "last_message_preview, last_message_at_ms, unread_count, updated_at_ms "
            "FROM local_chat_thread ORDER BY thread_id ASC"))) {
        SetError(QStringLiteral("读取全部本地会话失败"));
        return threads;
    }
    while (query.next()) threads.append(ReadThread(query));
    return threads;
}

QList<LocalChatMessage> LocalChatStorageMgr::LoadRecentMessages(qint64 threadId, int limit)
{
    // 内层倒序取最新 N 条，外层恢复为时间正序，界面可直接从上到下添加气泡。
    return QueryMessages(QStringLiteral(
                             "SELECT message_id, thread_id, sender_id, recv_id, content_type, content, "
                              "created_at_ms, updated_at_ms, sender_nick, server_status, send_state, delivery_state, is_read "
                             "FROM (SELECT message_id, thread_id, sender_id, recv_id, content_type, content, "
                              "created_at_ms, updated_at_ms, sender_nick, server_status, send_state, delivery_state, is_read "
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
                              "created_at_ms, updated_at_ms, sender_nick, server_status, send_state, delivery_state, is_read "
                             "FROM (SELECT message_id, thread_id, sender_id, recv_id, content_type, content, "
                              "created_at_ms, updated_at_ms, sender_nick, server_status, send_state, delivery_state, is_read "
                             "FROM local_chat_message "
                             "WHERE thread_id = ? AND message_id < ? "
                             "ORDER BY message_id DESC LIMIT ?) ORDER BY message_id ASC"),
                         threadId, beforeMessageId, limit);
}

bool LocalChatStorageMgr::HasMessage(qint64 messageId) const
{
    if (!IsReady() || messageId <= 0) {
        return false;
    }
    QSqlQuery query(_database);
    query.prepare(QStringLiteral(
        "SELECT 1 FROM local_chat_message WHERE message_id = ? LIMIT 1"));
    query.addBindValue(messageId);
    if (!query.exec()) {
        const_cast<LocalChatStorageMgr *>(this)->SetError(QStringLiteral("检查本地消息是否存在失败"));
        return false;
    }
    return query.next();
}

bool LocalChatStorageMgr::UpsertThread(const LocalChatThread &thread,
                                       GroupCacheAuthority authority)
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
    if (!UpsertThreadInTransaction(thread, authority) || !_database.commit()) {
        _database.rollback();
        if (_lastError.isEmpty()) SetError(QStringLiteral("提交本地会话事务失败"));
        return false;
    }
    _cachedThreads = LoadThreads();
    return true;
}

bool LocalChatStorageMgr::SetGroupState(qint64 threadId, qint64 membershipVersion,
                                        const QString &reason)
{
    if (!IsReady() || threadId <= 0 || membershipVersion <= 0) return false;
    QSqlQuery query(_database);
    query.prepare(QStringLiteral(
        "INSERT INTO local_chat_thread "
        "(thread_id, thread_type, title, active, role, invalid_reason, membership_version, updated_at_ms) "
        "VALUES (?, 'group', '', 0, 0, ?, ?, ?) "
        "ON CONFLICT(thread_id) DO UPDATE SET active = 0, role = 0, invalid_reason = excluded.invalid_reason, "
        "membership_version = excluded.membership_version, updated_at_ms = excluded.updated_at_ms "
        "WHERE local_chat_thread.thread_type = 'group' "
        "AND excluded.membership_version > local_chat_thread.membership_version"));
    query.addBindValue(threadId);
    query.addBindValue(reason);
    query.addBindValue(membershipVersion);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!query.exec() || query.numRowsAffected() == 0) {
        SetError(QStringLiteral("持久化群失效状态失败或通知版本已过期"));
        return false;
    }
    _cachedThreads = LoadThreads();
    return true;
}

bool LocalChatStorageMgr::ReconcileGroupSnapshot(const QList<LocalChatThread> &memberGroups)
{
    if (!IsReady()) return false;
    if (!_database.transaction()) {
        SetError(QStringLiteral("开启群列表对账事务失败"));
        return false;
    }
    QSet<qint64> memberIds;
    for (const LocalChatThread &thread : memberGroups) {
        if (thread.threadType != QStringLiteral("group") || !thread.active || !thread.membershipProof
            || thread.threadId <= 0 || thread.membershipVersion <= 0 || thread.role < 0 || thread.role > 2
            || memberIds.contains(thread.threadId)
            || !UpsertThreadInTransaction(thread, GroupCacheAuthority::MembershipProof)) {
            _database.rollback();
            if (_lastError.isEmpty()) _lastError = QStringLiteral("群快照成员证明无效或重复");
            return false;
        }
        memberIds.insert(thread.threadId);
    }
    QSqlQuery query(_database);
    if (!query.exec(QStringLiteral("SELECT thread_id FROM local_chat_thread "
                                   "WHERE thread_type = 'group' AND active = 1"))) {
        _database.rollback();
        SetError(QStringLiteral("读取本地群列表失败"));
        return false;
    }
    QList<qint64> missing;
    while (query.next()) {
        const qint64 threadId = query.value(0).toLongLong();
        if (!memberIds.contains(threadId)) missing.append(threadId);
    }
    QSqlQuery update(_database);
    update.prepare(QStringLiteral("UPDATE local_chat_thread SET active = 0, role = 0, "
                                  "invalid_reason = 'not_in_snapshot', updated_at_ms = ? "
                                  "WHERE thread_id = ? AND thread_type = 'group' AND active = 1"));
    for (qint64 threadId : missing) {
        update.bindValue(0, QDateTime::currentMSecsSinceEpoch());
        update.bindValue(1, threadId);
        if (!update.exec()) {
            _database.rollback();
            SetError(QStringLiteral("标记已退出群聊失败"));
            return false;
        }
    }
    if (!_database.commit()) {
        _database.rollback();
        SetError(QStringLiteral("提交群列表对账失败"));
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

    bool success = UpsertThreadInTransaction(thread, GroupCacheAuthority::Metadata, false);
    QSqlQuery messageQuery(_database);
    if (success) {
        success = messageQuery.prepare(QStringLiteral(
            "INSERT INTO local_chat_message "
            "(message_id, thread_id, sender_id, recv_id, content_type, content, created_at_ms, "
            "updated_at_ms, sender_nick, server_status, send_state, delivery_state, is_read) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(message_id) DO UPDATE SET "
            "thread_id = excluded.thread_id, sender_id = excluded.sender_id, "
            "recv_id = excluded.recv_id, content_type = excluded.content_type, "
            "content = excluded.content, updated_at_ms = excluded.updated_at_ms, "
            "sender_nick = CASE WHEN excluded.sender_nick <> '' THEN excluded.sender_nick "
            "ELSE local_chat_message.sender_nick END, "
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
        messageQuery.bindValue(4, message.contentType.isNull()
                                   ? QStringLiteral("") : message.contentType);
        messageQuery.bindValue(5, message.content.isNull()
                                   ? QStringLiteral("") : message.content);
        messageQuery.bindValue(6, message.createdAtMs);
        messageQuery.bindValue(7, message.updatedAtMs > 0 ? message.updatedAtMs : message.createdAtMs);
        messageQuery.bindValue(8, message.senderNick.isNull() ? QStringLiteral("") : message.senderNick);
        messageQuery.bindValue(9, message.serverStatus);
        messageQuery.bindValue(10, message.sendState);
        messageQuery.bindValue(11, message.deliveryState);
        messageQuery.bindValue(12, message.isRead ? 1 : 0);
        if (!messageQuery.exec()) {
            _lastError = QStringLiteral("写入本地消息失败：") + messageQuery.lastError().text();
            qWarning() << _lastError;
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

// This cursor is for the private-thread incremental list only. Group membership is
// reconciled independently from a complete snapshot beginning at thread ID zero.
qint64 LocalChatStorageMgr::MaxKnownThreadId() const
{
    if (!IsReady()) {
        return 0;
    }

    QSqlQuery query(_database);
    if (!query.exec(QStringLiteral(
            "SELECT COALESCE(MAX(thread_id), 0) FROM local_chat_thread WHERE thread_type = 'private'"))
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
        "WHERE thread_id = ? AND sender_id != ? AND message_id <= ? "
        "AND server_status IN (0, 1) AND (server_status = 0 OR delivery_state < 4)"));
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
    int schemaVersion = 0;
    QSqlQuery versionQuery(_database);
    if (versionQuery.exec(QStringLiteral("PRAGMA user_version")) && versionQuery.next()) {
        schemaVersion = versionQuery.value(0).toInt();
    }

    // 所有建表使用 IF NOT EXISTS，可安全在每次登录时调用；后续字段变更应追加版本迁移。
    const QList<QString> statements = {
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS local_chat_thread ("
            "thread_id INTEGER PRIMARY KEY, "
            "thread_type TEXT NOT NULL CHECK (thread_type IN ('private', 'group')), "
            "title TEXT NOT NULL DEFAULT '', peer_uid INTEGER NOT NULL DEFAULT 0, "
            "owner_uid INTEGER NOT NULL DEFAULT 0, active INTEGER NOT NULL DEFAULT 1 CHECK (active IN (0, 1)), "
            "role INTEGER NOT NULL DEFAULT 0 CHECK (role IN (0, 1, 2)), "
            "invalid_reason TEXT NOT NULL DEFAULT '', version INTEGER NOT NULL DEFAULT 0, "
            "membership_version INTEGER NOT NULL DEFAULT 0, approval_required INTEGER NOT NULL DEFAULT 0 "
            "CHECK (approval_required IN (0, 1)), announcement TEXT NOT NULL DEFAULT '', "
            "announcement_version INTEGER NOT NULL DEFAULT 0, announcement_updated_at_ms INTEGER NOT NULL DEFAULT 0, "
            "announcement_author_uid INTEGER NOT NULL DEFAULT 0, avatar_resource_id TEXT NOT NULL DEFAULT '', "
            "avatar_version INTEGER NOT NULL DEFAULT 0, avatar_mime_type TEXT NOT NULL DEFAULT '', "
            "avatar_size INTEGER NOT NULL DEFAULT 0, avatar_width INTEGER NOT NULL DEFAULT 0, "
            "avatar_height INTEGER NOT NULL DEFAULT 0, last_message_id INTEGER NOT NULL DEFAULT 0, "
            "last_message_preview TEXT NOT NULL DEFAULT '', last_message_at_ms INTEGER NOT NULL DEFAULT 0, "
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
            "updated_at_ms INTEGER NOT NULL, sender_nick TEXT NOT NULL DEFAULT '', "
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
    QSqlQuery columns(_database);
    if (!columns.exec(QStringLiteral("PRAGMA table_info(local_chat_message)"))) {
        SetError(QStringLiteral("读取本地消息表结构失败"));
        return false;
    }
    bool hasDeliveryState = false;
    bool hasSenderNick = false;
    while (columns.next()) {
        const QString columnName = columns.value(1).toString();
        hasDeliveryState = hasDeliveryState || columnName == QStringLiteral("delivery_state");
        hasSenderNick = hasSenderNick || columnName == QStringLiteral("sender_nick");
    }
    if (!hasDeliveryState && !Execute(QStringLiteral(
            "ALTER TABLE local_chat_message ADD COLUMN delivery_state INTEGER NOT NULL DEFAULT 1 "
            "CHECK (delivery_state IN (1, 2, 3, 4))"))) return false;
    if (!hasSenderNick && !Execute(QStringLiteral(
            "ALTER TABLE local_chat_message ADD COLUMN sender_nick TEXT NOT NULL DEFAULT ''"))) return false;
    QSqlQuery threadColumns(_database);
    if (!threadColumns.exec(QStringLiteral("PRAGMA table_info(local_chat_thread)"))) {
        SetError(QStringLiteral("读取本地会话表结构失败"));
        return false;
    }
    QSet<QString> existingColumns;
    while (threadColumns.next()) existingColumns.insert(threadColumns.value(1).toString());
    const QList<QPair<QString, QString>> threadAdditions = {
        {QStringLiteral("owner_uid"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("active"), QStringLiteral("INTEGER NOT NULL DEFAULT 1 CHECK (active IN (0, 1))")},
        {QStringLiteral("role"), QStringLiteral("INTEGER NOT NULL DEFAULT 0 CHECK (role IN (0, 1, 2))")},
        {QStringLiteral("invalid_reason"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
        {QStringLiteral("version"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("membership_version"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("approval_required"), QStringLiteral("INTEGER NOT NULL DEFAULT 0 CHECK (approval_required IN (0, 1))")},
        {QStringLiteral("announcement"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
        {QStringLiteral("announcement_version"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("announcement_updated_at_ms"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("announcement_author_uid"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("avatar_resource_id"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
        {QStringLiteral("avatar_version"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("avatar_mime_type"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
        {QStringLiteral("avatar_size"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("avatar_width"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("avatar_height"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")}
    };
    for (const auto &column : threadAdditions) {
        if (!existingColumns.contains(column.first)
            && !Execute(QStringLiteral("ALTER TABLE local_chat_thread ADD COLUMN %1 %2")
                            .arg(column.first, column.second))) return false;
    }
    if (schemaVersion < 3) {
        if (!Execute(QStringLiteral(
                "UPDATE local_sync_cursor SET max_message_id = 0, synced_at_ms = 0"))) return false;
    }
    return Execute(QStringLiteral("PRAGMA user_version = 5"));
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

bool LocalChatStorageMgr::UpsertThreadInTransaction(const LocalChatThread &thread,
                                                     GroupCacheAuthority authority,
                                                     bool preserveGroupUnread)
{
    if (thread.threadId <= 0 || (thread.threadType != QStringLiteral("private")
                                 && thread.threadType != QStringLiteral("group"))) {
        _lastError = QStringLiteral("保存本地会话失败：会话类型或 ID 无效");
        return false;
    }
    if (thread.threadType == QStringLiteral("group")
        && authority != GroupCacheAuthority::Metadata
        && (!thread.membershipProof || !thread.active || thread.membershipVersion <= 0
            || thread.role < 0 || thread.role > 2)) {
        _lastError = QStringLiteral("拒绝无效或非成员群证明");
        return false;
    }
    LocalChatThread merged = thread;
    QSqlQuery current(_database);
    current.prepare(QStringLiteral(
        "SELECT title, owner_uid, active, role, invalid_reason, version, membership_version, "
        "approval_required, announcement, announcement_version, announcement_updated_at_ms, "
        "announcement_author_uid, avatar_resource_id, avatar_version, avatar_mime_type, "
        "avatar_size, avatar_width, avatar_height, unread_count FROM local_chat_thread WHERE thread_id = ?"));
    current.addBindValue(thread.threadId);
    if (!current.exec()) {
        SetError(QStringLiteral("读取待更新的本地会话版本失败"));
        return false;
    }
    const bool exists = current.next();
    if (exists && thread.threadType == QStringLiteral("group")) {
        if (preserveGroupUnread) merged.unreadCount = current.value(18).toInt();
        const qint64 oldVersion = current.value(5).toLongLong();
        const qint64 oldMembershipVersion = current.value(6).toLongLong();
        const bool wasActive = current.value(2).toInt() != 0;
        if (thread.version < oldVersion) {
            merged.title = current.value(0).toString();
            merged.ownerUid = current.value(1).toLongLong();
            merged.approvalRequired = current.value(7).toInt() != 0;
            merged.version = oldVersion;
        }
        if (thread.announcementVersion < current.value(9).toLongLong()) {
            merged.announcement = current.value(8).toString();
            merged.announcementVersion = current.value(9).toLongLong();
            merged.announcementUpdatedAtMs = current.value(10).toLongLong();
            merged.announcementAuthorUid = current.value(11).toLongLong();
        }
        if (thread.avatarVersion < current.value(13).toLongLong()) {
            merged.avatarResourceId = current.value(12).toString();
            merged.avatarVersion = current.value(13).toLongLong();
            merged.avatarMimeType = current.value(14).toString();
            merged.avatarSize = current.value(15).toLongLong();
            merged.avatarWidth = current.value(16).toInt();
            merged.avatarHeight = current.value(17).toInt();
        }
        if (authority == GroupCacheAuthority::CurrentMemberMetadata) {
            merged.active = wasActive;
            merged.role = current.value(3).toInt();
            merged.invalidReason = current.value(4).toString();
            merged.membershipVersion = oldMembershipVersion;
            if (wasActive && thread.membershipVersion > oldMembershipVersion) {
                merged.membershipVersion = thread.membershipVersion;
                merged.role = thread.role;
                merged.invalidReason.clear();
            }
        } else if (authority == GroupCacheAuthority::MembershipProof
                   && thread.membershipVersion > oldMembershipVersion) {
            merged.active = true;
            merged.role = thread.role;
            merged.membershipVersion = thread.membershipVersion;
            merged.invalidReason.clear();
        } else {
            merged.active = wasActive;
            merged.role = current.value(3).toInt();
            merged.invalidReason = current.value(4).toString();
            merged.membershipVersion = oldMembershipVersion;
        }
    } else if (thread.threadType == QStringLiteral("group")
               && authority != GroupCacheAuthority::MembershipProof) {
        merged.active = false;
        merged.role = 0;
        merged.membershipVersion = 0;
        if (authority == GroupCacheAuthority::CurrentMemberMetadata)
            merged.invalidReason = QStringLiteral("membership_unconfirmed");
    } else if (thread.threadType == QStringLiteral("group")
               && thread.membershipVersion <= 0) {
        _lastError = QStringLiteral("群成员证明缺少有效 membership_version");
        return false;
    }

    QSqlQuery query(_database);
    query.prepare(QStringLiteral(
        "INSERT INTO local_chat_thread "
        "(thread_id, thread_type, title, peer_uid, owner_uid, active, role, invalid_reason, version, "
        "membership_version, approval_required, announcement, announcement_version, "
        "announcement_updated_at_ms, announcement_author_uid, avatar_resource_id, avatar_version, "
        "avatar_mime_type, avatar_size, avatar_width, avatar_height, last_message_id, "
        "last_message_preview, last_message_at_ms, unread_count, updated_at_ms) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(thread_id) DO UPDATE SET "
        "title=excluded.title, peer_uid=excluded.peer_uid, owner_uid=excluded.owner_uid, "
        "active=excluded.active, role=excluded.role, invalid_reason=excluded.invalid_reason, "
        "version=excluded.version, membership_version=excluded.membership_version, "
        "approval_required=excluded.approval_required, announcement=excluded.announcement, "
        "announcement_version=excluded.announcement_version, "
        "announcement_updated_at_ms=excluded.announcement_updated_at_ms, "
        "announcement_author_uid=excluded.announcement_author_uid, "
        "avatar_resource_id=excluded.avatar_resource_id, avatar_version=excluded.avatar_version, "
        "avatar_mime_type=excluded.avatar_mime_type, avatar_size=excluded.avatar_size, "
        "avatar_width=excluded.avatar_width, avatar_height=excluded.avatar_height, "
        "last_message_id=MAX(local_chat_thread.last_message_id, excluded.last_message_id), "
        "last_message_preview=CASE WHEN excluded.last_message_id >= local_chat_thread.last_message_id "
        "THEN excluded.last_message_preview ELSE local_chat_thread.last_message_preview END, "
        "last_message_at_ms=MAX(local_chat_thread.last_message_at_ms, excluded.last_message_at_ms), "
        "unread_count=excluded.unread_count, updated_at_ms=excluded.updated_at_ms "
        "WHERE local_chat_thread.thread_type=excluded.thread_type"));
    query.addBindValue(merged.threadId);
    query.addBindValue(merged.threadType);
    query.addBindValue(merged.title.isNull() ? QStringLiteral("") : merged.title);
    query.addBindValue(merged.peerUid);
    query.addBindValue(merged.ownerUid);
    query.addBindValue(merged.active ? 1 : 0);
    query.addBindValue(merged.role);
    query.addBindValue(merged.invalidReason.isNull() ? QStringLiteral("") : merged.invalidReason);
    query.addBindValue(merged.version);
    query.addBindValue(merged.membershipVersion);
    query.addBindValue(merged.approvalRequired ? 1 : 0);
    query.addBindValue(merged.announcement.isNull() ? QStringLiteral("") : merged.announcement);
    query.addBindValue(merged.announcementVersion);
    query.addBindValue(merged.announcementUpdatedAtMs);
    query.addBindValue(merged.announcementAuthorUid);
    query.addBindValue(merged.avatarResourceId.isNull() ? QStringLiteral("") : merged.avatarResourceId);
    query.addBindValue(merged.avatarVersion);
    query.addBindValue(merged.avatarMimeType.isNull() ? QStringLiteral("") : merged.avatarMimeType);
    query.addBindValue(merged.avatarSize);
    query.addBindValue(merged.avatarWidth);
    query.addBindValue(merged.avatarHeight);
    query.addBindValue(merged.lastMessageId);
    query.addBindValue(merged.lastMessagePreview.isNull()
                           ? QStringLiteral("") : merged.lastMessagePreview);
    query.addBindValue(merged.lastMessageAtMs);
    query.addBindValue(merged.unreadCount);
    query.addBindValue(merged.updatedAtMs > 0 ? merged.updatedAtMs : QDateTime::currentMSecsSinceEpoch());
    if (!query.exec() || query.numRowsAffected() == 0) {
        _lastError = QStringLiteral("保存本地会话失败：") + query.lastError().text();
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
