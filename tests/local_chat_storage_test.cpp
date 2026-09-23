#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QSqlDatabase>
#include <QString>
#include <QTemporaryDir>

#include <memory>

#include "../chat_app desktop/localchatstoragemgr.h"

namespace {

class IsolatedQtRuntime {
public:
    static IsolatedQtRuntime &Instance()
    {
        static IsolatedQtRuntime runtime;
        return runtime;
    }

    IsolatedQtRuntime(const IsolatedQtRuntime &) = delete;
    IsolatedQtRuntime &operator=(const IsolatedQtRuntime &) = delete;

    ~IsolatedQtRuntime()
    {
        // 清空单例连接后再销毁 Qt 应用和临时目录，确保 SQLite WAL 文件也能关闭。
        LocalChatStorageMgr::DestroyInstance();
        application_.reset();
    }

    bool IsReady() const
    {
        return error_.isEmpty() && application_ != nullptr && temporaryRoot_.isValid();
    }

    QString Error() const { return error_; }
    QString TemporaryRoot() const { return temporaryRoot_.path(); }

    bool IsInsideTemporaryRoot(const QString &path) const
    {
        if (path.isEmpty() || temporaryRoot_.path().isEmpty()) {
            return false;
        }

        const QString root = QDir::fromNativeSeparators(QDir::cleanPath(temporaryRoot_.path()));
        const QString candidate = QDir::fromNativeSeparators(QDir::cleanPath(path));
        const QString rootPrefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
#ifdef Q_OS_WIN
        return candidate.compare(root, Qt::CaseInsensitive) == 0
            || candidate.startsWith(rootPrefix, Qt::CaseInsensitive);
#else
        return candidate == root || candidate.startsWith(rootPrefix);
#endif
    }

private:
    IsolatedQtRuntime()
        : temporaryRoot_(QDir(QDir::currentPath()).filePath(
              QStringLiteral("chat_app_local_chat_storage_test-XXXXXX")))
    {
        if (!temporaryRoot_.isValid()) {
            error_ = QStringLiteral("无法创建 SQLite 测试临时目录");
            return;
        }

        argv_[0] = argv0_.data();
        argv_[1] = nullptr;
        application_ = std::make_unique<QCoreApplication>(argc_, argv_);
        QCoreApplication::setApplicationName(QStringLiteral("LocalChatStorageTest"));
    }

    QTemporaryDir temporaryRoot_;
    QByteArray argv0_ = QByteArrayLiteral("local_chat_storage_test");
    int argc_ = 1;
    char *argv_[2] = {nullptr, nullptr};
    std::unique_ptr<QCoreApplication> application_;
    QString error_;
};

LocalChatThread MakeThread(qint64 threadId, const QString &title, int unreadCount = 0)
{
    LocalChatThread thread;
    thread.threadId = threadId;
    thread.threadType = QStringLiteral("private");
    thread.title = title;
    thread.peerUid = 2002;
    thread.lastMessageId = 0;
    thread.lastMessagePreview = title;
    thread.lastMessageAtMs = 1000;
    thread.unreadCount = unreadCount;
    thread.updatedAtMs = 1000;
    return thread;
}

LocalChatMessage MakeMessage(qint64 messageId, qint64 threadId, qint64 senderId,
                             qint64 recvId, const QString &content)
{
    LocalChatMessage message;
    message.messageId = messageId;
    message.threadId = threadId;
    message.senderId = senderId;
    message.recvId = recvId;
    message.contentType = QStringLiteral("text");
    message.content = content;
    message.createdAtMs = 2000;
    message.updatedAtMs = 2000;
    message.serverStatus = 0;
    message.sendState = 1;
    message.deliveryState = 1;
    message.isRead = true;
    return message;
}

class LocalChatStorageTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        IsolatedQtRuntime &runtime = IsolatedQtRuntime::Instance();
        if (!runtime.IsReady()) {
            GTEST_SKIP() << runtime.Error().toStdString();
        }

        ASSERT_TRUE(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")))
            << "Qt QSQLITE 驱动不可用；运行测试时需让 Qt 的 sqldrivers/qsqlite 插件可被发现。";

        storage_ = LocalChatStorageMgr::GetInstance();
        storage_->Close();
    }

    void TearDown() override
    {
        if (storage_) {
            storage_->Close();
        }
    }

    bool Initialize(qint64 uid)
    {
        if (!storage_) {
            ADD_FAILURE() << "LocalChatStorageMgr 单例未初始化";
            return false;
        }
        const bool initialized = storage_->Initialize(
            uid, IsolatedQtRuntime::Instance().TemporaryRoot());
        EXPECT_TRUE(initialized) << storage_->LastError().toStdString();
        if (initialized) {
            EXPECT_TRUE(IsolatedQtRuntime::Instance().IsInsideTemporaryRoot(storage_->DatabasePath()))
                << "数据库路径不在 QTemporaryDir 中：" << storage_->DatabasePath().toStdString();
        }
        return initialized;
    }

    std::shared_ptr<LocalChatStorageMgr> storage_;
};

TEST_F(LocalChatStorageTest, AccountsUseSeparateDatabaseFilesAndCaches)
{
    constexpr qint64 uidA = 91001;
    constexpr qint64 uidB = 91002;
    constexpr qint64 threadId = 81001;
    constexpr qint64 messageId = 71001;

    ASSERT_TRUE(Initialize(uidA));
    const QString databasePathA = storage_->DatabasePath();
    ASSERT_TRUE(storage_->SaveReceivedMessages(
        MakeThread(threadId, QStringLiteral("account A")),
        {MakeMessage(messageId, threadId, uidA, 2002, QStringLiteral("message from A"))},
        messageId));

    storage_->Close();
    ASSERT_TRUE(Initialize(uidB));
    const QString databasePathB = storage_->DatabasePath();
    EXPECT_NE(databasePathA, databasePathB);
    EXPECT_TRUE(storage_->LoadThreads().isEmpty());
    EXPECT_TRUE(storage_->LoadRecentMessages(threadId).isEmpty());
    EXPECT_FALSE(storage_->HasMessage(messageId));
    EXPECT_TRUE(storage_->SyncCursors().isEmpty());

    ASSERT_TRUE(storage_->SaveReceivedMessages(
        MakeThread(threadId, QStringLiteral("account B")),
        {MakeMessage(messageId, threadId, uidB, 2002, QStringLiteral("message from B"))},
        messageId));
    storage_->Close();

    ASSERT_TRUE(Initialize(uidA));
    const QList<LocalChatMessage> messagesA = storage_->LoadRecentMessages(threadId);
    ASSERT_EQ(messagesA.size(), 1);
    EXPECT_EQ(messagesA.front().content, QStringLiteral("message from A"));
}

TEST_F(LocalChatStorageTest, ThreadAndMessageUpsertsDeduplicateAndCursorNeverRegresses)
{
    constexpr qint64 uid = 92001;
    constexpr qint64 threadId = 82001;
    constexpr qint64 messageId = 72001;
    ASSERT_TRUE(Initialize(uid));

    ASSERT_TRUE(storage_->UpsertThread(MakeThread(threadId, QStringLiteral("first title"), 2)));
    ASSERT_TRUE(storage_->UpsertThread(MakeThread(threadId, QStringLiteral("updated title"), 3)));
    QList<LocalChatThread> threads = storage_->LoadThreads();
    ASSERT_EQ(threads.size(), 1);
    EXPECT_EQ(threads.front().title, QStringLiteral("updated title"));
    EXPECT_EQ(threads.front().unreadCount, 3);

    LocalChatMessage first = MakeMessage(messageId, threadId, uid, 2002, QStringLiteral("first body"));
    first.isRead = false;
    ASSERT_TRUE(storage_->SaveReceivedMessages(
        MakeThread(threadId, QStringLiteral("first title"), 3), {first}, 800));

    LocalChatMessage updated = first;
    updated.content = QStringLiteral("updated body");
    updated.updatedAtMs = 3000;
    updated.serverStatus = 1;
    updated.deliveryState = 2;
    updated.isRead = true;
    ASSERT_TRUE(storage_->SaveReceivedMessages(
        MakeThread(threadId, QStringLiteral("updated title"), 3), {updated}, 700));

    QList<LocalChatMessage> messages = storage_->LoadRecentMessages(threadId);
    ASSERT_EQ(messages.size(), 1);
    EXPECT_EQ(messages.front().content, QStringLiteral("updated body"));
    EXPECT_EQ(messages.front().serverStatus, 1);
    EXPECT_EQ(messages.front().deliveryState, 2);
    EXPECT_TRUE(messages.front().isRead);

    ASSERT_TRUE(storage_->SetSyncCursor(threadId, 1000));
    ASSERT_TRUE(storage_->SetSyncCursor(threadId, 500));
    EXPECT_EQ(storage_->SyncCursors().value(threadId), 1000);

    const qint64 rolledBackThreadId = threadId + 1;
    LocalChatMessage validBeforeInvalid = MakeMessage(
        messageId + 1, rolledBackThreadId, uid, 2002, QStringLiteral("must roll back"));
    LocalChatMessage invalid = MakeMessage(
        messageId + 2, rolledBackThreadId + 1, uid, 2002, QStringLiteral("wrong thread"));
    EXPECT_FALSE(storage_->SaveReceivedMessages(
        MakeThread(rolledBackThreadId, QStringLiteral("rolled back")),
        {validBeforeInvalid, invalid}, 5000));
    EXPECT_FALSE(storage_->HasMessage(messageId + 1));
    EXPECT_FALSE(storage_->SyncCursors().contains(rolledBackThreadId));
    EXPECT_EQ(storage_->MaxKnownThreadId(), threadId);
}

TEST_F(LocalChatStorageTest, ReadAndDeliveryStatesOnlyAdvance)
{
    constexpr qint64 uid = 93001;
    constexpr qint64 peerUid = 2002;
    constexpr qint64 threadId = 83001;
    constexpr qint64 incomingId = 73001;
    constexpr qint64 outgoingId = 73002;
    ASSERT_TRUE(Initialize(uid));

    LocalChatMessage incoming = MakeMessage(
        incomingId, threadId, peerUid, uid, QStringLiteral("incoming"));
    incoming.isRead = false;
    LocalChatMessage outgoing = MakeMessage(
        outgoingId, threadId, uid, peerUid, QStringLiteral("outgoing"));
    ASSERT_TRUE(storage_->SaveReceivedMessages(
        MakeThread(threadId, QStringLiteral("read states"), 2), {incoming, outgoing}, outgoingId));

    ASSERT_TRUE(storage_->MarkThreadRead(threadId));
    QList<LocalChatThread> threads = storage_->LoadThreads();
    ASSERT_EQ(threads.size(), 1);
    EXPECT_EQ(threads.front().unreadCount, 0);
    QList<LocalChatMessage> messages = storage_->LoadRecentMessages(threadId);
    ASSERT_EQ(messages.size(), 2);
    EXPECT_TRUE(messages[0].isRead);
    EXPECT_TRUE(messages[1].isRead);

    ASSERT_TRUE(storage_->UpdateDeliveryState({outgoingId}, 3));
    ASSERT_TRUE(storage_->UpdateDeliveryState({outgoingId}, 2));
    messages = storage_->LoadRecentMessages(threadId);
    ASSERT_EQ(messages.size(), 2);
    EXPECT_EQ(messages[1].deliveryState, 3);

    const QList<qint64> markedOutgoing = storage_->MarkOutgoingMessagesRead(
        threadId, peerUid, outgoingId);
    ASSERT_EQ(markedOutgoing.size(), 1);
    EXPECT_EQ(markedOutgoing.front(), outgoingId);

    // 后续较旧同步不得撤销本地已读，也不得回退服务端已读状态和 deliveryState。
    incoming.isRead = false;
    outgoing.isRead = false;
    ASSERT_TRUE(storage_->SaveReceivedMessages(
        MakeThread(threadId, QStringLiteral("read states"), 0),
        {incoming, outgoing}, outgoingId - 1));
    messages = storage_->LoadRecentMessages(threadId);
    ASSERT_EQ(messages.size(), 2);
    EXPECT_TRUE(messages[0].isRead);
    EXPECT_TRUE(messages[1].isRead);
    EXPECT_EQ(messages[1].serverStatus, 1);
    EXPECT_EQ(messages[1].deliveryState, 4);

    ASSERT_TRUE(storage_->UpdateDeliveryState({outgoingId}, 1));
    messages = storage_->LoadRecentMessages(threadId);
    ASSERT_EQ(messages.size(), 2);
    EXPECT_EQ(messages[1].deliveryState, 4);
}

TEST_F(LocalChatStorageTest, ReopeningDatabaseRestoresThreadsMessagesAndSyncCursor)
{
    constexpr qint64 uid = 94001;
    constexpr qint64 threadId = 84001;
    constexpr qint64 messageId = 74001;
    ASSERT_TRUE(Initialize(uid));

    LocalChatMessage message = MakeMessage(
        messageId, threadId, 2002, uid, QStringLiteral("persisted message"));
    message.isRead = false;
    ASSERT_TRUE(storage_->SaveReceivedMessages(
        MakeThread(threadId, QStringLiteral("persisted thread"), 1), {message}, messageId));
    ASSERT_TRUE(storage_->SetSyncCursor(threadId, messageId + 50));
    const QString databasePath = storage_->DatabasePath();

    storage_->Close();
    ASSERT_TRUE(Initialize(uid));
    EXPECT_EQ(storage_->DatabasePath(), databasePath);

    const QList<LocalChatThread> threads = storage_->CachedThreads();
    ASSERT_EQ(threads.size(), 1);
    EXPECT_EQ(threads.front().threadId, threadId);
    EXPECT_EQ(threads.front().title, QStringLiteral("persisted thread"));
    EXPECT_EQ(threads.front().unreadCount, 1);

    const QList<LocalChatMessage> messages = storage_->LoadRecentMessages(threadId);
    ASSERT_EQ(messages.size(), 1);
    EXPECT_EQ(messages.front().messageId, messageId);
    EXPECT_EQ(messages.front().content, QStringLiteral("persisted message"));
    EXPECT_FALSE(messages.front().isRead);
    EXPECT_TRUE(storage_->HasMessage(messageId));
    EXPECT_EQ(storage_->SyncCursors().value(threadId), messageId + 50);
    EXPECT_EQ(storage_->MaxKnownThreadId(), threadId);
}

} // namespace
