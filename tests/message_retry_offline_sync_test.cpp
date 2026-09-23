#include <gtest/gtest.h>

#include "../ChatServer1/include/MysqlMgr.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr char kTestSchemaPrefix[] = "chat_app_test_";
std::atomic<unsigned int> g_uidSequence{0};

std::string Trim(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

struct MysqlConfig {
    std::string host;
    std::string port;
    std::string user;
    std::string password;
    std::string schema;
};

bool ReadMysqlConfig(const std::filesystem::path& path, MysqlConfig& config)
{
    std::ifstream input(path);
    if (!input) {
        return false;
    }

    bool inMysqlSection = false;
    std::string line;
    while (std::getline(input, line)) {
        line = Trim(std::move(line));
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            inMysqlSection = Trim(line.substr(1, line.size() - 2)) == "Mysql";
            continue;
        }
        if (!inMysqlSection) {
            continue;
        }

        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const std::string key = Trim(line.substr(0, separator));
        const std::string value = Trim(line.substr(separator + 1));
        if (key == "host") config.host = value;
        else if (key == "port") config.port = value;
        else if (key == "user") config.user = value;
        else if (key == "passwd") config.password = value;
        else if (key == "schema") config.schema = value;
    }
    return !config.host.empty() && !config.port.empty() && !config.user.empty()
        && !config.password.empty() && !config.schema.empty();
}

bool ReadGuardedTestConfig(MysqlConfig& config, std::string& error)
{
    const char* expectedSchema = std::getenv("CHAT_MESSAGE_TEST_DB_SCHEMA");
    const char* expectedHost = std::getenv("CHAT_MESSAGE_TEST_DB_HOST");
    const char* expectedPort = std::getenv("CHAT_MESSAGE_TEST_DB_PORT");
    const char* expectedUser = std::getenv("CHAT_MESSAGE_TEST_DB_USER");
    if (expectedSchema == nullptr || expectedSchema[0] == '\0'
        || expectedHost == nullptr || expectedHost[0] == '\0'
        || expectedPort == nullptr || expectedPort[0] == '\0'
        || expectedUser == nullptr || expectedUser[0] == '\0') {
        error = "Set CHAT_MESSAGE_TEST_DB_SCHEMA/HOST/PORT/USER.";
        return false;
    }
    if (std::string(expectedSchema).rfind(kTestSchemaPrefix, 0) != 0) {
        error = std::string("CHAT_MESSAGE_TEST_DB_SCHEMA must start with ") + kTestSchemaPrefix;
        return false;
    }

    const std::filesystem::path configPath = std::filesystem::current_path() / "config.ini";
    if (!ReadMysqlConfig(configPath, config)) {
        error = "Could not read a complete [Mysql] section from " + configPath.string();
        return false;
    }
    if (config.schema != expectedSchema || config.host != expectedHost
        || config.port != expectedPort || config.user != expectedUser) {
        error = "[Mysql] in config.ini no longer matches CHAT_MESSAGE_TEST_DB_* guards";
        return false;
    }
    return true;
}

class ScopedCoutSilence {
public:
    explicit ScopedCoutSilence(std::ostream& sink)
        : previous_(std::cout.rdbuf(sink.rdbuf()))
    {
    }

    ~ScopedCoutSilence()
    {
        std::cout.rdbuf(previous_);
    }

    ScopedCoutSilence(const ScopedCoutSilence&) = delete;
    ScopedCoutSilence& operator=(const ScopedCoutSilence&) = delete;

private:
    std::streambuf* previous_;
};

unsigned int NextUidPairBase()
{
    const auto ticks = static_cast<unsigned long long>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const unsigned int sequence = g_uidSequence.fetch_add(1, std::memory_order_relaxed);
    // The project stores UIDs as positive signed ints in its C++ API. Use a high,
    // synthetic range; the persistence methods do not create or modify user rows.
    return 1000000000U + static_cast<unsigned int>((ticks + sequence * 7919ULL) % 999999998ULL);
}

struct ServerPage {
    std::vector<StoredTextMessage> messages;
    std::uint64_t nextMessageId = 0;
    bool loadMore = false;
};

bool LoadServerPage(MysqlMgr& mysql, int uid, std::uint64_t threadId,
                    std::uint64_t afterMessageId, int pageSize, ServerPage& page)
{
    page = {};
    std::vector<StoredTextMessage> candidates;
    if (!mysql.LoadPrivateTextMessages(uid, threadId, afterMessageId, pageSize + 1, candidates)) {
        return false;
    }

    const std::size_t returnedCount = std::min<std::size_t>(
        static_cast<std::size_t>(pageSize), candidates.size());
    page.messages.assign(candidates.begin(), candidates.begin() + returnedCount);
    page.nextMessageId = page.messages.empty() ? afterMessageId : page.messages.back().messageId;
    page.loadMore = returnedCount < candidates.size();
    return true;
}

class MessageRetryOfflineSyncTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        MysqlConfig config;
        std::string configError;
        if (!ReadGuardedTestConfig(config, configError)) {
            GTEST_SKIP() << configError
                         << " Run from a working directory whose config.ini points to the isolated test database.";
        }

        // ConfigMgr echoes config.ini, including the database password, when first initialized.
        // Silence that one-time diagnostic while MysqlMgr opens the configured test connections.
        try {
            std::ostringstream sink;
            ScopedCoutSilence silence(sink);
            mysql_ = MysqlMgr::GetInstance();
        } catch (const std::exception& error) {
            GTEST_SKIP() << "Could not connect to the explicitly configured test database: "
                         << error.what();
        }
        if (!mysql_) {
            GTEST_SKIP() << "MysqlMgr did not initialize for the isolated test database.";
        }

        const unsigned int base = NextUidPairBase();
        senderUid_ = static_cast<int>(base);
        receiverUid_ = static_cast<int>(base + 1U);
        cleanupArmed_ = true;
    }

    void TearDown() override
    {
        if (!cleanupArmed_) {
            return;
        }

        MysqlConfig config;
        std::string configError;
        if (!ReadGuardedTestConfig(config, configError)) {
            ADD_FAILURE() << "Refusing test-data cleanup because isolation could not be confirmed: "
                          << configError;
            return;
        }

        std::unique_ptr<mysqlx::Session> session;
        bool transactionStarted = false;
        try {
            session = std::make_unique<mysqlx::Session>(
                config.host, std::stoi(config.port), config.user, config.password, config.schema);

            const auto activeSchemaRow = session->sql("SELECT DATABASE()").execute().fetchOne();
            if (!activeSchemaRow) {
                throw std::runtime_error("SELECT DATABASE() returned no row");
            }
            const std::string activeSchema = activeSchemaRow[0].get<std::string>();
            if (activeSchema.rfind(kTestSchemaPrefix, 0) != 0 || activeSchema != config.schema) {
                throw std::runtime_error("cleanup connection is not using the guarded test schema");
            }

            const int uid1 = std::min(senderUid_, receiverUid_);
            const int uid2 = std::max(senderUid_, receiverUid_);
            session->startTransaction();
            transactionStarted = true;

            auto privateChatResult = session->sql(
                "SELECT thread_id, user1_id, user2_id FROM private_chat "
                "WHERE user1_id = ? AND user2_id = ?")
                .bind(uid1)
                .bind(uid2)
                .execute();
            auto privateChatRow = privateChatResult.fetchOne();
            if (!privateChatRow) {
                session->rollback();
                transactionStarted = false;
                return;
            }

            const std::uint64_t threadId = privateChatRow[0].get<std::uint64_t>();
            const int storedUid1 = privateChatRow[1].get<int>();
            const int storedUid2 = privateChatRow[2].get<int>();
            if (threadId == 0 || storedUid1 != uid1 || storedUid2 != uid2) {
                throw std::runtime_error("private_chat row does not match this fixture's UID pair");
            }
            if (privateChatResult.fetchOne()) {
                throw std::runtime_error("more than one private_chat row matched this UID pair");
            }

            session->sql("DELETE FROM chat_message WHERE thread_id = ?")
                .bind(threadId)
                .execute();
            const auto privateDelete = session->sql(
                "DELETE FROM private_chat WHERE thread_id = ? AND user1_id = ? AND user2_id = ?")
                .bind(threadId)
                .bind(uid1)
                .bind(uid2)
                .execute();
            if (privateDelete.getAffectedItemsCount() != 1) {
                throw std::runtime_error("did not delete exactly one matching private_chat row");
            }

            const auto threadDelete = session->sql("DELETE FROM chat_thread WHERE id = ?")
                .bind(threadId)
                .execute();
            if (threadDelete.getAffectedItemsCount() != 1) {
                throw std::runtime_error("did not delete exactly one matching chat_thread row");
            }

            session->commit();
            transactionStarted = false;
        } catch (const std::exception& error) {
            if (session && transactionStarted) {
                try {
                    session->rollback();
                } catch (...) {
                }
            }
            ADD_FAILURE() << "Could not clean this test's isolated chat rows: " << error.what();
        } catch (...) {
            if (session && transactionStarted) {
                try {
                    session->rollback();
                } catch (...) {
                }
            }
            ADD_FAILURE() << "Could not clean this test's isolated chat rows: unknown exception";
        }
    }

    std::shared_ptr<MysqlMgr> mysql_;
    int senderUid_ = 0;
    int receiverUid_ = 0;
    bool cleanupArmed_ = false;
};

TEST_F(MessageRetryOfflineSyncTest, MessageIdCursorResumesAfterDisconnectWithoutGaps)
{
    std::vector<std::pair<std::string, std::string>> firstBatch = {
        {"offline-sync-a", "before disconnect 1"},
        {"offline-sync-b", "before disconnect 2"},
        {"offline-sync-c", "before disconnect 3"},
    };
    std::uint64_t threadId = 0;
    std::vector<StoredTextMessage> savedBeforeDisconnect;
    ASSERT_TRUE(mysql_->SavePrivateTextMessages(senderUid_, receiverUid_, firstBatch,
                                                threadId, savedBeforeDisconnect));
    ASSERT_NE(threadId, 0U);
    ASSERT_EQ(savedBeforeDisconnect.size(), firstBatch.size());

    // The receiver takes one page, then has no live session while two more messages commit.
    // This directly exercises the same durable store used by the TCP handler without Redis routing.
    ServerPage firstPage;
    ASSERT_TRUE(LoadServerPage(*mysql_, receiverUid_, threadId, 0, 2, firstPage));
    ASSERT_EQ(firstPage.messages.size(), 2U);
    ASSERT_TRUE(firstPage.loadMore);
    EXPECT_EQ(firstPage.messages[0].messageId, savedBeforeDisconnect[0].messageId);
    EXPECT_EQ(firstPage.messages[1].messageId, savedBeforeDisconnect[1].messageId);

    const std::vector<std::pair<std::string, std::string>> writtenWhileOffline = {
        {"offline-sync-d", "while receiver is offline 1"},
        {"offline-sync-e", "while receiver is offline 2"},
    };
    std::vector<StoredTextMessage> savedWhileOffline;
    std::uint64_t sameThreadId = 0;
    ASSERT_TRUE(mysql_->SavePrivateTextMessages(senderUid_, receiverUid_, writtenWhileOffline,
                                                sameThreadId, savedWhileOffline));
    ASSERT_EQ(sameThreadId, threadId);
    ASSERT_EQ(savedWhileOffline.size(), writtenWhileOffline.size());

    std::vector<StoredTextMessage> recovered = firstPage.messages;
    std::uint64_t cursor = firstPage.nextMessageId;
    bool loadMore = firstPage.loadMore;
    while (loadMore) {
        ServerPage page;
        ASSERT_TRUE(LoadServerPage(*mysql_, receiverUid_, threadId, cursor, 2, page));
        ASSERT_FALSE(page.messages.empty()) << "load_more must always advance the message_id cursor";
        ASSERT_GT(page.nextMessageId, cursor);
        recovered.insert(recovered.end(), page.messages.begin(), page.messages.end());
        cursor = page.nextMessageId;
        loadMore = page.loadMore;
    }

    ASSERT_EQ(recovered.size(), 5U);
    std::vector<StoredTextMessage> allSaved = savedBeforeDisconnect;
    allSaved.insert(allSaved.end(), savedWhileOffline.begin(), savedWhileOffline.end());
    ASSERT_EQ(allSaved.size(), recovered.size());
    for (std::size_t index = 0; index < allSaved.size(); ++index) {
        EXPECT_EQ(recovered[index].messageId, allSaved[index].messageId);
        EXPECT_EQ(recovered[index].threadId, threadId);
        EXPECT_EQ(recovered[index].content, allSaved[index].content);
        if (index > 0) {
            EXPECT_GT(recovered[index].messageId, recovered[index - 1].messageId);
        }
    }
}

TEST_F(MessageRetryOfflineSyncTest, RetryingTheSameClientMessageIdCurrentlyCreatesAnotherRow)
{
    const std::string clientMessageId = "retry-same-client-msg-id";
    const std::vector<std::pair<std::string, std::string>> request = {
        {clientMessageId, "the retried text"},
    };

    std::uint64_t firstThreadId = 0;
    std::vector<StoredTextMessage> firstAck;
    ASSERT_TRUE(mysql_->SavePrivateTextMessages(senderUid_, receiverUid_, request,
                                                firstThreadId, firstAck));
    ASSERT_EQ(firstAck.size(), 1U);
    EXPECT_EQ(firstAck[0].uniqueId, clientMessageId);

    // Repeat the same request body and client_msg_id as a client retry after an uncertain ACK.
    std::uint64_t retryThreadId = 0;
    std::vector<StoredTextMessage> retryAck;
    ASSERT_TRUE(mysql_->SavePrivateTextMessages(senderUid_, receiverUid_, request,
                                                retryThreadId, retryAck));
    ASSERT_EQ(retryAck.size(), 1U);
    EXPECT_EQ(retryAck[0].uniqueId, clientMessageId);
    EXPECT_EQ(retryThreadId, firstThreadId);

    // ChatServer1's text INSERT writes client_msg_id as NULL and has no dedupe lookup/constraint.
    // Keep the observed duplicate behavior explicit instead of asserting idempotency.
    EXPECT_NE(retryAck[0].messageId, firstAck[0].messageId);
    std::vector<StoredTextMessage> history;
    ASSERT_TRUE(mysql_->LoadPrivateTextMessages(senderUid_, firstThreadId, 0, 10, history));
    ASSERT_EQ(history.size(), 2U);
    EXPECT_EQ(history[0].messageId, firstAck[0].messageId);
    EXPECT_EQ(history[1].messageId, retryAck[0].messageId);
    EXPECT_EQ(history[0].content, request[0].second);
    EXPECT_EQ(history[1].content, request[0].second);
}

TEST_F(MessageRetryOfflineSyncTest, HistoryReloadRestoresDisplayedAndReadStateSeparately)
{
    const std::vector<std::pair<std::string, std::string>> request = {
        {"receipt-a", "first incoming message"},
        {"receipt-b", "second incoming message"},
        {"receipt-c", "third incoming message"},
    };
    std::uint64_t threadId = 0;
    std::vector<StoredTextMessage> saved;
    ASSERT_TRUE(mysql_->SavePrivateTextMessages(senderUid_, receiverUid_, request, threadId, saved));
    ASSERT_EQ(saved.size(), request.size());

    std::vector<DisplayReceipt> displayReceipts;
    ASSERT_TRUE(mysql_->MarkMessagesDisplayed(receiverUid_, threadId,
                                              {saved[0].messageId}, displayReceipts));
    ASSERT_EQ(displayReceipts.size(), 1U);
    EXPECT_EQ(displayReceipts[0].senderId, senderUid_);
    EXPECT_EQ(displayReceipts[0].readerId, receiverUid_);
    EXPECT_EQ(displayReceipts[0].messageIds, std::vector<std::uint64_t>{saved[0].messageId});

    // A repeated display ACK is accepted but does not create a second state-change receipt.
    displayReceipts.clear();
    ASSERT_TRUE(mysql_->MarkMessagesDisplayed(receiverUid_, threadId,
                                              {saved[0].messageId}, displayReceipts));
    EXPECT_TRUE(displayReceipts.empty());

    std::vector<ReadReceipt> readReceipts;
    ASSERT_TRUE(mysql_->MarkPrivateThreadRead(receiverUid_, threadId,
                                              saved[1].messageId, readReceipts));
    ASSERT_EQ(readReceipts.size(), 1U);
    EXPECT_EQ(readReceipts[0].senderId, senderUid_);
    EXPECT_EQ(readReceipts[0].readerId, receiverUid_);
    EXPECT_EQ(readReceipts[0].readThroughMessageId, saved[1].messageId);

    // A repeated read cursor changes no additional rows and produces no new receipt.
    readReceipts.clear();
    ASSERT_TRUE(mysql_->MarkPrivateThreadRead(receiverUid_, threadId,
                                              saved[1].messageId, readReceipts));
    EXPECT_TRUE(readReceipts.empty());

    std::vector<StoredTextMessage> senderHistory;
    ASSERT_TRUE(mysql_->LoadPrivateTextMessages(senderUid_, threadId, 0, 10, senderHistory));
    ASSERT_EQ(senderHistory.size(), 3U);
    for (std::size_t index = 0; index < saved.size(); ++index) {
        EXPECT_EQ(senderHistory[index].messageId, saved[index].messageId);
    }
    EXPECT_TRUE(senderHistory[0].peerDisplayed);
    EXPECT_FALSE(senderHistory[1].peerDisplayed);
    EXPECT_FALSE(senderHistory[2].peerDisplayed);
    EXPECT_EQ(senderHistory[0].status, 1);
    EXPECT_EQ(senderHistory[1].status, 1);
    EXPECT_EQ(senderHistory[2].status, 0);
}

} // namespace
