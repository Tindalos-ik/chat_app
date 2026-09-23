#include <gtest/gtest.h>

#include <mysqlx/xdevapi.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "MysqlMgr.h"

namespace {

constexpr char kConfirmVariable[] = "CHAT_FRIEND_TEST_CONFIRM_ISOLATED";
constexpr char kExpectedConfirmation[] = "YES";
constexpr char kFriendAuthContent[] = "我们已经是好友了，开始聊天吧。";

struct MysqlTestConfig {
    std::string host;
    std::string port;
    std::string user;
    std::string password;
    std::string schema;
};

std::optional<std::string> ReadEnvironmentVariable(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t valueSize = 0;
    if (_dupenv_s(&value, &valueSize, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

std::string MakeUniqueToken() {
    std::random_device device;
    std::mt19937_64 random(device());
    const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::ostringstream token;
    token << std::hex << ticks << random();
    return token.str();
}

bool ContainsLineBreak(const std::string& value) {
    return value.find('\n') != std::string::npos || value.find('\r') != std::string::npos;
}

bool IsSafeSchemaName(const std::string& schema) {
    if (schema.empty() || schema.size() > 64) {
        return false;
    }
    return std::all_of(schema.begin(), schema.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '_';
    });
}

std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::optional<MysqlTestConfig> ReadExplicitTestConfig(std::vector<std::string>& missing) {
    constexpr const char* names[] = {
        "CHAT_FRIEND_TEST_MYSQL_HOST",
        "CHAT_FRIEND_TEST_MYSQL_PORT",
        "CHAT_FRIEND_TEST_MYSQL_USER",
        "CHAT_FRIEND_TEST_MYSQL_PASSWORD",
        "CHAT_FRIEND_TEST_MYSQL_SCHEMA",
        kConfirmVariable,
    };
    std::vector<std::optional<std::string>> values;
    values.reserve(std::size(names));
    for (const char* name : names) {
        values.push_back(ReadEnvironmentVariable(name));
        if (!values.back()) {
            missing.emplace_back(name);
        }
    }
    if (!missing.empty()) {
        return std::nullopt;
    }

    if (*values[5] != kExpectedConfirmation) {
        missing.emplace_back(std::string(kConfirmVariable) + "=YES");
        return std::nullopt;
    }

    return MysqlTestConfig{
        *values[0], *values[1], *values[2], *values[3], *values[4]};
}

class TempConfigDirectory {
public:
    explicit TempConfigDirectory(const MysqlTestConfig& config) {
        path_ = std::filesystem::temp_directory_path() /
                ("chat_friend_transaction_" + MakeUniqueToken());
        std::filesystem::create_directories(path_);

        const auto configPath = path_ / "config.ini";
        std::ofstream output(configPath, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("could not create temporary MysqlMgr config.ini");
        }
        output << "[Mysql]\n"
               << "host = " << config.host << "\n"
               << "port = " << config.port << "\n"
               << "user = " << config.user << "\n"
               << "passwd = " << config.password << "\n"
               << "schema = " << config.schema << "\n"
               << "poolsize = 1\n";
        output.close();
        if (!output) {
            throw std::runtime_error("could not write temporary MysqlMgr config.ini");
        }
    }

    ~TempConfigDirectory() {
        std::error_code ignored;
        const auto tempRoot = std::filesystem::weakly_canonical(
            std::filesystem::temp_directory_path(), ignored);
        if (ignored) {
            return;
        }
        const auto resolved = std::filesystem::weakly_canonical(path_, ignored);
        if (!ignored && resolved.parent_path() == tempRoot &&
            resolved.filename().string().rfind("chat_friend_transaction_", 0) == 0) {
            std::filesystem::remove_all(resolved, ignored);
        }
    }

    const std::filesystem::path& Path() const { return path_; }

private:
    std::filesystem::path path_;
};

class ScopedCurrentDirectory {
public:
    explicit ScopedCurrentDirectory(const std::filesystem::path& target)
        : previous_(std::filesystem::current_path()) {
        std::filesystem::current_path(target);
    }

    ~ScopedCurrentDirectory() {
        std::error_code ignored;
        std::filesystem::current_path(previous_, ignored);
    }

private:
    std::filesystem::path previous_;
};

class ScopedCoutRedirect {
public:
    explicit ScopedCoutRedirect(std::streambuf* target)
        : previous_(std::cout.rdbuf(target)) {}

    ~ScopedCoutRedirect() { std::cout.rdbuf(previous_); }

private:
    std::streambuf* previous_;
};

std::uint64_t ReadUnsignedScalar(mysqlx::Session& session, const std::string& sql) {
    const auto row = session.sql(sql).execute().fetchOne();
    if (!row) {
        throw std::runtime_error("query returned no scalar row: " + sql);
    }
    return row[0].get<std::uint64_t>();
}

std::uint64_t CountFriend(mysqlx::Session& session, int selfUid, int friendUid) {
    return session.sql("SELECT COUNT(*) FROM friend WHERE self_id = ? AND friend_id = ?")
        .bind(selfUid).bind(friendUid).execute().fetchOne()[0].get<std::uint64_t>();
}

std::uint64_t CountApply(mysqlx::Session& session, int applicantUid, int recipientUid) {
    return session.sql("SELECT COUNT(*) FROM friend_apply WHERE from_uid = ? AND to_uid = ?")
        .bind(applicantUid).bind(recipientUid).execute().fetchOne()[0].get<std::uint64_t>();
}

std::uint64_t CountPrivateChat(mysqlx::Session& session, int user1Uid, int user2Uid) {
    const int lowerUid = (std::min)(user1Uid, user2Uid);
    const int higherUid = (std::max)(user1Uid, user2Uid);
    return session.sql("SELECT COUNT(*) FROM private_chat WHERE user1_id = ? AND user2_id = ?")
        .bind(lowerUid).bind(higherUid).execute().fetchOne()[0].get<std::uint64_t>();
}

std::uint64_t CountMessages(mysqlx::Session& session, int senderUid, int recipientUid) {
    return session.sql("SELECT COUNT(*) FROM chat_message WHERE sender_id = ? AND recv_id = ?")
        .bind(senderUid).bind(recipientUid).execute().fetchOne()[0].get<std::uint64_t>();
}

bool IsUidUnused(mysqlx::Session& session, int uid) {
    const auto userCount = session.sql("SELECT COUNT(*) FROM user WHERE uid = ?")
        .bind(uid).execute().fetchOne()[0].get<std::uint64_t>();
    const auto friendCount = session.sql(
        "SELECT COUNT(*) FROM friend WHERE self_id = ? OR friend_id = ?")
        .bind(uid).bind(uid).execute().fetchOne()[0].get<std::uint64_t>();
    const auto applyCount = session.sql(
        "SELECT COUNT(*) FROM friend_apply WHERE from_uid = ? OR to_uid = ?")
        .bind(uid).bind(uid).execute().fetchOne()[0].get<std::uint64_t>();
    const auto privateChatCount = session.sql(
        "SELECT COUNT(*) FROM private_chat WHERE user1_id = ? OR user2_id = ?")
        .bind(uid).bind(uid).execute().fetchOne()[0].get<std::uint64_t>();
    const auto messageCount = session.sql(
        "SELECT COUNT(*) FROM chat_message WHERE sender_id = ? OR recv_id = ?")
        .bind(uid).bind(uid).execute().fetchOne()[0].get<std::uint64_t>();
    return userCount == 0 && friendCount == 0 && applyCount == 0 &&
           privateChatCount == 0 && messageCount == 0;
}

class ScopedFixtureCleanup {
public:
    ScopedFixtureCleanup(mysqlx::Session& session, std::vector<int> uids)
        : session_(session), uids_(std::move(uids)) {}

    ~ScopedFixtureCleanup() {
        try {
            for (const int uid : uids_) {
                session_.sql("DELETE FROM chat_message WHERE sender_id = ? OR recv_id = ?")
                    .bind(uid).bind(uid).execute();
            }

            std::vector<std::uint64_t> threadIds;
            for (const int uid : uids_) {
                auto result = session_.sql(
                    "SELECT thread_id FROM private_chat WHERE user1_id = ? OR user2_id = ?")
                    .bind(uid).bind(uid).execute();
                for (const auto& row : result.fetchAll()) {
                    threadIds.push_back(row[0].get<std::uint64_t>());
                }
            }
            std::sort(threadIds.begin(), threadIds.end());
            threadIds.erase(std::unique(threadIds.begin(), threadIds.end()), threadIds.end());

            for (const int uid : uids_) {
                session_.sql("DELETE FROM private_chat WHERE user1_id = ? OR user2_id = ?")
                    .bind(uid).bind(uid).execute();
                session_.sql("DELETE FROM friend_apply WHERE from_uid = ? OR to_uid = ?")
                    .bind(uid).bind(uid).execute();
                session_.sql("DELETE FROM friend WHERE self_id = ? OR friend_id = ?")
                    .bind(uid).bind(uid).execute();
            }
            for (const std::uint64_t threadId : threadIds) {
                session_.sql("DELETE FROM chat_thread WHERE id = ?").bind(threadId).execute();
            }
            for (const int uid : uids_) {
                session_.sql("DELETE FROM user WHERE uid = ?").bind(uid).execute();
            }
        } catch (const std::exception& error) {
            std::cerr << "friend transaction test cleanup failed: " << error.what() << '\n';
        }
    }

private:
    mysqlx::Session& session_;
    std::vector<int> uids_;
};

class ScopedTrigger {
public:
    ScopedTrigger(mysqlx::Session& session, std::string name)
        : session_(session), name_(std::move(name)) {}

    void InstallFailureForMessage(int senderUid, int recipientUid) {
        const std::string sql =
            "CREATE TRIGGER `" + name_ + "` BEFORE INSERT ON chat_message FOR EACH ROW "
            "BEGIN IF NEW.sender_id = " + std::to_string(senderUid) +
            " AND NEW.recv_id = " + std::to_string(recipientUid) +
            " THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = "
            "'intentional friend transaction integration failure'; END IF; END";
        session_.sql(sql).execute();
        installed_ = true;
    }

    bool Remove() noexcept {
        if (!installed_) {
            return true;
        }
        try {
            session_.sql("DROP TRIGGER IF EXISTS `" + name_ + "`").execute();
            installed_ = false;
            return true;
        } catch (const std::exception& error) {
            std::cerr << "could not remove friend transaction test trigger: " << error.what() << '\n';
            return false;
        }
    }

    ~ScopedTrigger() { Remove(); }

private:
    mysqlx::Session& session_;
    std::string name_;
    bool installed_ = false;
};

void VerifyRequiredTables(mysqlx::Session& session, const std::string& schema) {
    const std::set<std::string> required = {
        "user", "friend", "friend_apply", "chat_thread", "private_chat", "chat_message"};
    std::set<std::string> found;
    auto result = session.sql(
        "SELECT table_name, engine FROM information_schema.tables "
        "WHERE table_schema = ? AND table_name IN "
        "('user', 'friend', 'friend_apply', 'chat_thread', 'private_chat', 'chat_message')")
        .bind(schema).execute();
    for (const auto& row : result.fetchAll()) {
        const std::string tableName = row[0].get<std::string>();
        const std::string engine = row[1].get<std::string>();
        if (engine != "InnoDB") {
            throw std::runtime_error("required table is not InnoDB: " + tableName);
        }
        found.insert(tableName);
    }
    if (found != required) {
        throw std::runtime_error("test schema lacks one or more required ChatServer1 tables");
    }
}

std::vector<int> AllocateUnusedUids(mysqlx::Session& session, std::size_t count) {
    std::random_device device;
    std::mt19937 random(device());
    std::uniform_int_distribution<int> distribution(1000000000, 1900000000);
    for (int attempt = 0; attempt < 64; ++attempt) {
        const int base = distribution(random);
        std::vector<int> candidate;
        candidate.reserve(count);
        bool unused = true;
        for (std::size_t offset = 0; offset < count; ++offset) {
            const int uid = base + static_cast<int>(offset);
            if (!IsUidUnused(session, uid)) {
                unused = false;
                break;
            }
            candidate.push_back(uid);
        }
        if (unused) {
            return candidate;
        }
    }
    throw std::runtime_error("could not allocate unused UID values for isolated fixtures");
}

void InsertFixtureUser(mysqlx::Session& session, int uid, const std::string& token, int ordinal) {
    const std::string suffix = token + "_" + std::to_string(ordinal);
    session.sql(
        "INSERT INTO user (uid, name, email, pwd, nick, `desc`, sex, icon) "
        "VALUES (?, ?, ?, '', ?, '', 0, '')")
        .bind(uid)
        .bind("friend_tx_" + suffix)
        .bind("friend_tx_" + suffix + "@example.invalid")
        .bind("friend-tx-" + suffix)
        .execute();
}

std::optional<std::string> ReadFriendRemark(mysqlx::Session& session, int selfUid, int friendUid) {
    const auto row = session.sql("SELECT back FROM friend WHERE self_id = ? AND friend_id = ?")
        .bind(selfUid).bind(friendUid).execute().fetchOne();
    if (!row) {
        return std::nullopt;
    }
    return row[0].get<std::string>();
}

std::uint64_t GetPrivateThreadId(mysqlx::Session& session, int user1Uid, int user2Uid) {
    const int lowerUid = (std::min)(user1Uid, user2Uid);
    const int higherUid = (std::max)(user1Uid, user2Uid);
    const auto row = session.sql(
        "SELECT thread_id FROM private_chat WHERE user1_id = ? AND user2_id = ?")
        .bind(lowerUid).bind(higherUid).execute().fetchOne();
    if (!row) {
        throw std::runtime_error("private chat was not created");
    }
    return row[0].get<std::uint64_t>();
}

} // namespace

TEST(FriendTransactionIntegration, RealApplicationAuthenticationIsAtomicAndIdempotent) {
    std::vector<std::string> missing;
    const auto config = ReadExplicitTestConfig(missing);
    if (!config) {
        std::ostringstream reason;
        reason << "requires explicit isolated MySQL test configuration; missing or unset: ";
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i != 0) reason << ", ";
            reason << missing[i];
        }
        GTEST_SKIP() << reason.str();
    }

    ASSERT_FALSE(ContainsLineBreak(config->host));
    ASSERT_FALSE(ContainsLineBreak(config->port));
    ASSERT_FALSE(ContainsLineBreak(config->user));
    ASSERT_FALSE(ContainsLineBreak(config->password));
    ASSERT_FALSE(config->host.empty());
    ASSERT_FALSE(config->user.empty());
    ASSERT_TRUE(IsSafeSchemaName(config->schema))
        << "schema must be a plain identifier of at most 64 characters";
    ASSERT_NE(Lowercase(config->schema).find("test"), std::string::npos)
        << "schema name must contain 'test' to guard the development database";
    ASSERT_NE(Lowercase(config->schema), "chat_app_db");

    unsigned long parsedPort = 0;
    try {
        std::size_t consumed = 0;
        parsedPort = std::stoul(config->port, &consumed);
        ASSERT_EQ(consumed, config->port.size());
    } catch (const std::exception&) {
        FAIL() << "CHAT_FRIEND_TEST_MYSQL_PORT must be a numeric X Protocol port";
    }
    ASSERT_GT(parsedPort, 0UL);
    ASSERT_LE(parsedPort, 65535UL);

    std::unique_ptr<mysqlx::Session> session;
    ASSERT_NO_THROW(session = std::make_unique<mysqlx::Session>(
        config->host, static_cast<unsigned int>(parsedPort), config->user,
        config->password, config->schema));
    ASSERT_NO_THROW(VerifyRequiredTables(*session, config->schema));

    TempConfigDirectory tempConfig(*config);
    ScopedCurrentDirectory workingDirectory(tempConfig.Path());
    std::shared_ptr<MysqlMgr> mysql;
    {
        std::ostringstream ignoredConfigOutput;
        ScopedCoutRedirect silenceConfigLog(ignoredConfigOutput.rdbuf());
        ASSERT_NO_THROW(mysql = MysqlMgr::GetInstance());
    }

    const std::string token = MakeUniqueToken();
    std::vector<int> uids;
    ASSERT_NO_THROW(uids = AllocateUnusedUids(*session, 4));
    ScopedFixtureCleanup cleanup(*session, uids);
    for (std::size_t i = 0; i < uids.size(); ++i) {
        ASSERT_NO_THROW(InsertFixtureUser(*session, uids[i], token, static_cast<int>(i)));
    }

    const int applicantUid = uids[0];
    const int recipientUid = uids[1];
    const int rollbackApplicantUid = uids[2];
    const int rollbackRecipientUid = uids[3];

    ASSERT_TRUE(mysql->AddFriendApply(applicantUid, recipientUid, "applicant-remark-v1"));
    ASSERT_TRUE(mysql->AddFriendApply(applicantUid, recipientUid, "applicant-remark-final"));
    EXPECT_EQ(CountApply(*session, applicantUid, recipientUid), 1U);

    auto pendingApply = session->sql(
        "SELECT applicant_remark, status FROM friend_apply WHERE from_uid = ? AND to_uid = ?")
        .bind(applicantUid).bind(recipientUid).execute().fetchOne();
    ASSERT_TRUE(pendingApply);
    EXPECT_EQ(pendingApply[0].get<std::string>(), "applicant-remark-final");
    EXPECT_EQ(pendingApply[1].get<int>(), 0);

    std::vector<std::shared_ptr<ApplyInfo>> applications;
    ASSERT_TRUE(mysql->GetFriendApplyInfo(recipientUid, applications));
    ASSERT_EQ(applications.size(), 1U);
    ASSERT_TRUE(applications.front());
    EXPECT_EQ(applications.front()->_uid, applicantUid);
    EXPECT_EQ(applications.front()->_status, 0);

    std::vector<FriendAuthMessage> authMessages;
    ASSERT_TRUE(mysql->AddFriend(recipientUid, applicantUid, "recipient-remark", authMessages));
    ASSERT_EQ(authMessages.size(), 1U);
    EXPECT_EQ(authMessages.front().senderId, static_cast<std::uint64_t>(recipientUid));
    EXPECT_GT(authMessages.front().messageId, 0U);
    EXPECT_GT(authMessages.front().threadId, 0U);
    EXPECT_EQ(authMessages.front().uniqueId,
              "friend-auth-" + std::to_string(authMessages.front().messageId));
    EXPECT_EQ(authMessages.front().content, kFriendAuthContent);

    EXPECT_EQ(CountFriend(*session, applicantUid, recipientUid), 1U);
    EXPECT_EQ(CountFriend(*session, recipientUid, applicantUid), 1U);
    EXPECT_EQ(ReadFriendRemark(*session, applicantUid, recipientUid).value_or(""),
              "applicant-remark-final");
    EXPECT_EQ(ReadFriendRemark(*session, recipientUid, applicantUid).value_or(""),
              "recipient-remark");
    EXPECT_EQ(CountApply(*session, applicantUid, recipientUid), 1U);
    EXPECT_EQ(CountPrivateChat(*session, applicantUid, recipientUid), 1U);

    const std::uint64_t threadId = GetPrivateThreadId(*session, applicantUid, recipientUid);
    EXPECT_EQ(authMessages.front().threadId, threadId);
    EXPECT_EQ(ReadUnsignedScalar(*session,
        "SELECT COUNT(*) FROM chat_thread WHERE id = " + std::to_string(threadId) +
        " AND type = 'private'"), 1U);

    auto acceptedApply = session->sql(
        "SELECT applicant_remark, status FROM friend_apply WHERE from_uid = ? AND to_uid = ?")
        .bind(applicantUid).bind(recipientUid).execute().fetchOne();
    ASSERT_TRUE(acceptedApply);
    EXPECT_EQ(acceptedApply[0].get<std::string>(), "applicant-remark-final");
    EXPECT_EQ(acceptedApply[1].get<int>(), 1);

    auto authMessageRow = session->sql(
        "SELECT message_id, sender_id, recv_id, content, status FROM chat_message "
        "WHERE thread_id = ? AND sender_id = ? AND recv_id = ?")
        .bind(threadId).bind(recipientUid).bind(applicantUid).execute().fetchOne();
    ASSERT_TRUE(authMessageRow);
    EXPECT_EQ(authMessageRow[0].get<std::uint64_t>(), authMessages.front().messageId);
    EXPECT_EQ(authMessageRow[1].get<int>(), recipientUid);
    EXPECT_EQ(authMessageRow[2].get<int>(), applicantUid);
    EXPECT_EQ(authMessageRow[3].get<std::string>(), kFriendAuthContent);
    EXPECT_EQ(authMessageRow[4].get<int>(), 0);
    EXPECT_EQ(CountMessages(*session, recipientUid, applicantUid), 1U);

    std::vector<std::shared_ptr<UserInfo>> applicantFriends;
    std::vector<std::shared_ptr<UserInfo>> recipientFriends;
    ASSERT_TRUE(mysql->GetFriendInfo(applicantUid, applicantFriends));
    ASSERT_TRUE(mysql->GetFriendInfo(recipientUid, recipientFriends));
    EXPECT_TRUE(std::any_of(applicantFriends.begin(), applicantFriends.end(), [recipientUid](const auto& info) {
        return info && info->uid == recipientUid;
    }));
    EXPECT_TRUE(std::any_of(recipientFriends.begin(), recipientFriends.end(), [applicantUid](const auto& info) {
        return info && info->uid == applicantUid;
    }));

    authMessages.push_back(FriendAuthMessage{});
    ASSERT_TRUE(mysql->AddFriend(recipientUid, applicantUid, "changed-recipient-remark", authMessages));
    ASSERT_EQ(authMessages.size(), 1U);
    EXPECT_EQ(authMessages.front().messageId, authMessageRow[0].get<std::uint64_t>());
    EXPECT_EQ(authMessages.front().threadId, threadId);
    EXPECT_EQ(CountFriend(*session, applicantUid, recipientUid), 1U);
    EXPECT_EQ(CountFriend(*session, recipientUid, applicantUid), 1U);
    EXPECT_EQ(CountPrivateChat(*session, applicantUid, recipientUid), 1U);
    EXPECT_EQ(CountMessages(*session, recipientUid, applicantUid), 1U);
    EXPECT_EQ(ReadFriendRemark(*session, recipientUid, applicantUid).value_or(""),
              "recipient-remark");

    ASSERT_TRUE(mysql->AddFriendApply(
        rollbackApplicantUid, rollbackRecipientUid, "rollback-applicant-remark"));
    EXPECT_EQ(CountFriend(*session, rollbackApplicantUid, rollbackRecipientUid), 0U);
    EXPECT_EQ(CountFriend(*session, rollbackRecipientUid, rollbackApplicantUid), 0U);
    EXPECT_EQ(CountPrivateChat(*session, rollbackApplicantUid, rollbackRecipientUid), 0U);
    EXPECT_EQ(CountMessages(*session, rollbackRecipientUid, rollbackApplicantUid), 0U);

    const std::uint64_t greatestThreadIdBeforeFailure =
        ReadUnsignedScalar(*session, "SELECT COALESCE(MAX(id), 0) FROM chat_thread");
    ScopedTrigger failMessageInsert(*session, "friend_tx_fail_" + token);
    ASSERT_NO_THROW(failMessageInsert.InstallFailureForMessage(
        rollbackRecipientUid, rollbackApplicantUid));
    authMessages.push_back(FriendAuthMessage{});
    EXPECT_FALSE(mysql->AddFriend(
        rollbackRecipientUid, rollbackApplicantUid, "rollback-recipient-remark", authMessages));
    EXPECT_TRUE(authMessages.empty());
    ASSERT_TRUE(failMessageInsert.Remove());

    EXPECT_EQ(CountFriend(*session, rollbackApplicantUid, rollbackRecipientUid), 0U);
    EXPECT_EQ(CountFriend(*session, rollbackRecipientUid, rollbackApplicantUid), 0U);
    EXPECT_EQ(CountApply(*session, rollbackApplicantUid, rollbackRecipientUid), 1U);
    EXPECT_EQ(CountPrivateChat(*session, rollbackApplicantUid, rollbackRecipientUid), 0U);
    EXPECT_EQ(CountMessages(*session, rollbackRecipientUid, rollbackApplicantUid), 0U);
    EXPECT_EQ(ReadUnsignedScalar(*session,
        "SELECT COUNT(*) FROM chat_thread WHERE id > " +
        std::to_string(greatestThreadIdBeforeFailure)), 0U);
    auto rolledBackApply = session->sql(
        "SELECT applicant_remark, status FROM friend_apply "
        "WHERE from_uid = ? AND to_uid = ?")
        .bind(rollbackApplicantUid).bind(rollbackRecipientUid).execute().fetchOne();
    ASSERT_TRUE(rolledBackApply);
    EXPECT_EQ(rolledBackApply[0].get<std::string>(), "rollback-applicant-remark");
    EXPECT_EQ(rolledBackApply[1].get<int>(), 0);
}
