#ifndef LOCALCHATSTORAGEMGR_H
#define LOCALCHATSTORAGEMGR_H

#include <QHash>
#include <QList>
#include <QSet>
#include <QSqlDatabase>
#include <QString>
#include <QtGlobal>

#include "singleton.h"

// 聊天列表展示和本地同步需要的会话摘要。
// threadId、lastMessageId 均对应服务端的 BIGINT 消息/会话 ID。
struct LocalChatThread {
    qint64 threadId = 0;
    QString threadType;  // private / group
    QString title;
    qint64 peerUid = 0;  // 私聊对方 uid；群聊时为 0
    qint64 ownerUid = 0; // 本地展示的群主资料，不作为权限凭据
    bool active = true;  // 本地群可用状态，不代表服务器端授权
    qint64 lastMessageId = 0;
    QString lastMessagePreview;
    qint64 lastMessageAtMs = 0;
    int unreadCount = 0;
    qint64 updatedAtMs = 0;
};

// 客户端保存的消息。附件的文件内容不放入 SQLite，而是由附件表保存本地路径。
struct LocalChatMessage {
    qint64 messageId = 0;
    qint64 threadId = 0;
    qint64 senderId = 0;
    qint64 recvId = 0;  // 群消息由服务端约定为 0
    QString contentType = QStringLiteral("text");
    QString content;
    qint64 createdAtMs = 0;
    qint64 updatedAtMs = 0;
    QString senderNick; // 群历史发送者昵称；空值由 UI 使用 UID 回退
    int serverStatus = 0;  // 0=未读，1=已读，2=撤回
    int sendState = 1;     // 0=发送中，1=已发送，2=发送失败，3=已确认
    // 私聊发送端状态；群聊不使用对方显示/已读的 deliveryState 3/4。
    int deliveryState = 1; // 私聊1=已持久化、2=已投递、3=已显示、4=已读
    bool isRead = true;
};

/*
 * 本地聊天数据管理器。
 *
 * 每个登录账号对应一个独立 SQLite 文件，避免切换账号时读取到他人的聊天缓存。
 * Initialize(uid) 会打开（不存在则创建）数据库、执行幂等建表，并加载会话摘要。
 * 消息正文采用按会话、按页查询；不在初始化阶段一次性加载所有历史消息，以免
 * 聊天记录很多时阻塞 UI 或占用过多内存。
 *
 * 本类只在创建它的 Qt 线程（当前为 UI 线程）中访问。若以后在网络工作线程写库，
 * 应在该线程创建另一个 QSQLITE 连接，不能跨线程复用本连接。
 */
class LocalChatStorageMgr : public Singleton<LocalChatStorageMgr>
{
    friend class Singleton<LocalChatStorageMgr>;

public:
    ~LocalChatStorageMgr();

    // 打开 uid 的专属数据库。cacheRoot 为空时使用 Qt 应用数据目录；
    // 测试可传入独立的绝对目录，避免写入真实用户缓存。
    // 重复打开同一文件会刷新摘要；切换 uid 或根目录会先关闭旧库。
    bool Initialize(qint64 uid, const QString &cacheRoot = QString());
    void Close();

    bool IsReady() const;
    qint64 CurrentUid() const;
    QString DatabasePath() const;
    QString LastError() const;
    QList<LocalChatThread> CachedThreads() const;
    // 全量线程查询不受 CachedThreads/LoadThreads 默认100条摘要上限限制。
    QList<LocalChatThread> LoadAllThreads();
    QList<LocalChatThread> LoadThreads(int limit = 100);

    // 聊天窗口打开时调用。返回结果按时间正序，适合直接 append 到 ChatView。
    QList<LocalChatMessage> LoadRecentMessages(qint64 threadId, int limit = 50);
    QList<LocalChatMessage> LoadMessagesBefore(qint64 threadId,
                                               qint64 beforeMessageId,
                                               int limit = 50);
    // 判断正式 message_id 是否已经落库。实时通知用它去重，但不会借此推进 1030 同步游标。
    bool HasMessage(qint64 messageId) const;

    // 服务端下发会话列表时写入或更新本地会话信息。
    // authoritativeActivation 仅用于服务端明确确认的 create/join；普通列表/迟到消息不能复活失效群。
    bool UpsertThread(const LocalChatThread &thread, bool authoritativeActivation = false);
    // 仅 active=false；未知 thread 也保存不可用 tombstone。true 必定拒绝，不能充当权限证明。
    // 只有 UpsertThread(..., authoritativeActivation=true) 的服务端 create/join 确认可恢复。
    bool SetGroupState(qint64 threadId, bool active);
    // 只有完整权威快照所有分页成功后才调用；缺失群标为不可用并保留缓存历史。
    bool ReconcileGroupSnapshot(const QSet<qint64> &activeThreadIds);

    // 批量保存服务端消息和该会话的同步游标；三者位于同一事务，避免只写入一半。
    // 已有 messageId 的记录会被更新，因而撤回/已读等服务端状态也可重新同步。
    bool SaveReceivedMessages(const LocalChatThread &thread,
                              const QList<LocalChatMessage> &messages,
                              qint64 maxSyncedMessageId);

    // 登录增量同步：按会话返回“本地已同步的最大服务端 message_id”。
    QHash<qint64, qint64> SyncCursors() const;
    // 私聊服务端列表的发现游标；群列表独立地从 after_thread_id=0 做完整快照。
    qint64 MaxKnownThreadId() const;
    bool SetSyncCursor(qint64 threadId, qint64 maxMessageId);

    // 用户进入会话后清除未读状态和会话未读数。
    bool MarkThreadRead(qint64 threadId);

    // 1037/1039 到达后持久化发送端状态；返回/更新的数据会在 UI 重绘后保持一致。
    bool UpdateDeliveryState(const QList<qint64> &messageIds, int deliveryState);
    QList<qint64> MarkOutgoingMessagesRead(qint64 threadId, qint64 readerUid,
                                           qint64 readThroughMessageId);

private:
    LocalChatStorageMgr();

    bool CreateSchema();
    bool Execute(const QString &sql);
    bool UpsertThreadInTransaction(const LocalChatThread &thread,
                                   bool authoritativeActivation = false);
    QList<LocalChatMessage> QueryMessages(const QString &sql,
                                          qint64 threadId,
                                          qint64 anchorMessageId,
                                          int limit);
    void SetError(const QString &operation);

    QSqlDatabase _database;
    QString _connectionName;
    QString _databasePath;
    QString _lastError;
    qint64 _uid = 0;
    QList<LocalChatThread> _cachedThreads;
};

#endif // LOCALCHATSTORAGEMGR_H
