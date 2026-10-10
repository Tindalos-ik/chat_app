// 群聊真实 MySQL 回归：只运行显式确认的隔离 schema，不使用开发库。
#include <gtest/gtest.h>
#include "MysqlMgr.h"
#include "ConfigMgr.h"
#include "const.h"
#include <cstdlib>
#include <sstream>
#include <chrono>
#include <future>
#include <algorithm>
#include <cctype>

namespace {
class GroupTransactionTest : public ::testing::Test {
protected:
    std::shared_ptr<MysqlMgr> mysql;
    std::unique_ptr<mysqlx::Session> db;
    std::uint64_t thread = 0;
    int owner = 0, member = 0, stranger = 0;
    GroupChatThread group;
    std::vector<GroupChatMember> members;
    std::vector<StoredTextMessage> messages;
    std::vector<int> recipients;
    void SetUp() override {
        const char* schema = std::getenv("CHAT_GROUP_TEST_SCHEMA");
        const char* confirm = std::getenv("CHAT_GROUP_TEST_CONFIRM_ISOLATED");
        if (!schema || std::string(schema).rfind("chat_app_test_",0)!=0 || !confirm || std::string(confirm)!="YES")
            GTEST_SKIP() << "Set CHAT_GROUP_TEST_SCHEMA=chat_app_test_* and CHAT_GROUP_TEST_CONFIRM_ISOLATED=YES.";
        if (!std::all_of(schema, schema+std::char_traits<char>::length(schema),
            [](unsigned char c) { return std::isalnum(c) || c=='_'; }))
            GTEST_SKIP() << "Schema identifier must contain only letters, digits and underscore.";
        std::ostringstream sink;
        auto previous = std::cout.rdbuf(sink.rdbuf());
        Defer restore([&]() { std::cout.rdbuf(previous); });
        auto config = ConfigMgr::Inst()["Mysql"];
        if (config["schema"] != schema) GTEST_SKIP() << "config.ini does not match guarded isolated schema.";
        try {
            db = std::make_unique<mysqlx::Session>(config["host"],std::stoi(config["port"]),config["user"],config["passwd"]);
            db->sql("USE `" + std::string(schema) + "`").execute();
            mysql = MysqlMgr::GetInstance();
        } catch (...) { FAIL() << "Explicit isolated MySQL connection failed."; }
        auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        owner = 1400000000 + static_cast<int>(ticks % 100000000); member=owner+1; stranger=owner+2;
    }
    void TearDown() override {
        if (!db || !thread) return;
        // 仅删除本用例创建的 thread；先消息再群成员，遵守生产 RESTRICT 外键。
        db->sql("DELETE FROM chat_message WHERE thread_id=?").bind(thread).execute();
        db->sql("DELETE FROM group_chat_member WHERE thread_id=?").bind(thread).execute();
        db->sql("DELETE FROM group_chat WHERE thread_id=?").bind(thread).execute();
        db->sql("DELETE FROM chat_thread WHERE id=?").bind(thread).execute();
    }
    int Op(GroupAction action, int uid, int target=0, std::uint64_t after=0,
           const std::vector<std::pair<std::string,std::string>>& texts={}) {
        return mysql->GroupOperation(action,uid,thread,"transaction regression",target,after,10,texts,
            group,members,messages,recipients);
    }
};

TEST_F(GroupTransactionTest, MembershipSingleWriteOfflineHistoryAndLogicalDissolve) {
    ASSERT_EQ(Op(GroupAction::Create,owner),Success);
    ASSERT_GT(thread,0u); EXPECT_EQ(group.ownerUid,owner);
    ASSERT_EQ(Op(GroupAction::Send,owner,0,0,{{"shared-client-id","before join"}}),Success);
    auto first=messages.front().messageId;
    EXPECT_EQ(messages.front().recvId,0); EXPECT_EQ(messages.front().senderNick,std::to_string(owner));
    EXPECT_EQ(Op(GroupAction::History,member),GroupNotMember);
    ASSERT_EQ(Op(GroupAction::Join,member),Success);
    ASSERT_EQ(Op(GroupAction::Join,member),Success);
    ASSERT_EQ(Op(GroupAction::Members,owner),Success);
    ASSERT_EQ(members.size(),2u); EXPECT_EQ(members[0].role,2); EXPECT_EQ(members[1].role,0);
    ASSERT_EQ(Op(GroupAction::History,member),Success);
    ASSERT_EQ(messages.size(),1u); EXPECT_EQ(messages[0].messageId,first); // 加入前历史允许
    ASSERT_EQ(Op(GroupAction::Send,member,0,0,{{"shared-client-id","offline text"}}),Success);
    EXPECT_NE(messages[0].messageId,first); EXPECT_EQ(recipients.size(),2u);
    auto count=db->sql("SELECT COUNT(*) FROM chat_message WHERE thread_id=?").bind(thread).execute().fetchOne();
    EXPECT_EQ(count[0].get<int>(),2); // 不按成员复制消息；不同发送者可用相同客户端 ID
    ASSERT_EQ(Op(GroupAction::History,owner,0,first),Success);
    ASSERT_EQ(messages.size(),1u); EXPECT_EQ(messages[0].content,"offline text");
    EXPECT_EQ(Op(GroupAction::Kick,member,owner),GroupPermissionDenied);
    EXPECT_EQ(Op(GroupAction::Kick,owner,owner),GroupPermissionDenied);
    EXPECT_EQ(Op(GroupAction::Dissolve,stranger),GroupNotMember);
    ASSERT_EQ(Op(GroupAction::Kick,owner,member),Success);
    EXPECT_EQ(Op(GroupAction::Send,member,0,0,{{"forged","denied"}}),GroupNotMember);
    EXPECT_EQ(Op(GroupAction::Members,member),GroupNotMember);
    EXPECT_EQ(Op(GroupAction::History,member),GroupNotMember);
    EXPECT_EQ(Op(GroupAction::Authorize,member),GroupNotMember);
    ASSERT_EQ(Op(GroupAction::Dissolve,owner),Success);
    EXPECT_EQ(Op(GroupAction::Search,owner),GroupUnavailable);
    EXPECT_EQ(Op(GroupAction::Join,member),GroupUnavailable);
    EXPECT_EQ(Op(GroupAction::History,owner),GroupUnavailable);
    count=db->sql("SELECT COUNT(*) FROM chat_message WHERE thread_id=?").bind(thread).execute().fetchOne();
    EXPECT_EQ(count[0].get<int>(),2); // 解散后历史物理保留
    auto active=db->sql("SELECT active FROM group_chat WHERE thread_id=?").bind(thread).execute().fetchOne();
    EXPECT_EQ(active[0].get<int>(),0);
    EXPECT_THROW(db->sql("DELETE FROM chat_thread WHERE id=?").bind(thread).execute(),std::exception);
}

TEST_F(GroupTransactionTest, FailedBatchRollsBackAndRevocationLocksCurrentAuthorization) {
    // owner INSERT 在真实数据库中失败：thread/群/成员必须作为一个事务全回滚。
    const std::string trigger = "group_owner_fail_" + std::to_string(owner);
    db->sql("CREATE TRIGGER `" + trigger + "` BEFORE INSERT ON group_chat_member FOR EACH ROW "
        "BEGIN IF NEW.user_id=" + std::to_string(owner) + " AND NEW.role=2 THEN "
        "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='isolated owner insert failure'; END IF; END").execute();
    bool triggerInstalled = true;
    Defer removeTrigger([&]() { if (triggerInstalled) db->sql("DROP TRIGGER `" + trigger + "`").execute(); });
    ASSERT_EQ(Op(GroupAction::Create,owner),ServerBusy);
    ASSERT_GT(thread,0u);
    for (const auto& sql : {"SELECT COUNT(*) FROM chat_thread WHERE id=?",
                           "SELECT COUNT(*) FROM group_chat WHERE thread_id=?",
                           "SELECT COUNT(*) FROM group_chat_member WHERE thread_id=?"}) {
        auto row=db->sql(sql).bind(thread).execute().fetchOne();
        EXPECT_EQ(row[0].get<int>(),0);
    }
    db->sql("DROP TRIGGER `" + trigger + "`").execute(); triggerInstalled=false;
    thread=0;
    ASSERT_EQ(Op(GroupAction::Create,owner),Success);
    ASSERT_EQ(Op(GroupAction::Join,member),Success);
    EXPECT_EQ(Op(GroupAction::Send,member,0,0,{{"valid","first"},{"invalid",""}}),Error_Json);
    auto count=db->sql("SELECT COUNT(*) FROM chat_message WHERE thread_id=?").bind(thread).execute().fetchOne();
    EXPECT_EQ(count[0].get<int>(),0);
    EXPECT_EQ(mysql->GroupOperation(GroupAction::Send,member,thread,"",0,0,10,
        {{"would-insert","rolled back at frame gate"}},group,members,messages,recipients,
        []() { return false; }),MessageTooLarge);
    count=db->sql("SELECT COUNT(*) FROM chat_message WHERE thread_id=?").bind(thread).execute().fetchOne();
    EXPECT_EQ(count[0].get<int>(),0); // 在真实INSERT后拒绝，验证事务回滚而非仅输入预校验
    // 持有撤权行锁，让并发授权等待；提交删成员后，等待者必须读到当前权限而非快照。
    db->startTransaction();
    db->sql("SELECT thread_id FROM group_chat WHERE thread_id=? FOR UPDATE").bind(thread).execute().fetchAll();
    db->sql("DELETE FROM group_chat_member WHERE thread_id=? AND user_id=?").bind(thread).bind(member).execute();
    auto authorization=std::async(std::launch::async,[&]() {
        GroupChatThread g; std::vector<GroupChatMember> m; std::vector<StoredTextMessage> t; std::vector<int> r;
        auto id=thread;
        return mysql->GroupOperation(GroupAction::Authorize,member,id,"",0,0,0,{},g,m,t,r);
    });
    EXPECT_EQ(authorization.wait_for(std::chrono::milliseconds(100)),std::future_status::timeout);
    db->commit();
    EXPECT_EQ(authorization.get(),GroupNotMember);
}
} // namespace
