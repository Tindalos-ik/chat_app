// 真实生产MysqlMgr群管理回归；仅显式隔离schema，拒绝开发库。
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
    std::uint64_t thread=0;
    int owner=0,member=0,stranger=0,sequence=0;
    GroupChatThread group;
    std::vector<GroupChatMember> members;
    std::vector<StoredTextMessage> messages;
    std::vector<int> recipients;
    Json::Value result;
    std::vector<GroupEvent> events;
    void SetUp() override {
        const char* schema=std::getenv("CHAT_GROUP_TEST_SCHEMA");const char* confirm=std::getenv("CHAT_GROUP_TEST_CONFIRM_ISOLATED");
        if(!schema||std::string(schema).rfind("chat_app_test_",0)!=0||!confirm||std::string(confirm)!="YES")GTEST_SKIP()<<"Explicit isolated group schema required";
        if(!std::all_of(schema,schema+std::char_traits<char>::length(schema),[](unsigned char c){return std::isalnum(c)||c=='_';}))GTEST_SKIP()<<"Unsafe schema";
        std::ostringstream sink;auto previous=std::cout.rdbuf(sink.rdbuf());Defer restore([&](){std::cout.rdbuf(previous);});
        auto config=ConfigMgr::Inst()["Mysql"];if(config["schema"]!=schema)GTEST_SKIP()<<"config.ini mismatches isolated schema";
        try{db=std::make_unique<mysqlx::Session>(config["host"],std::stoi(config["port"]),config["user"],config["passwd"]);db->sql("USE `"+std::string(schema)+"`").execute();mysql=MysqlMgr::GetInstance();}catch(...){FAIL()<<"Explicit isolated connection failed";}
        auto ticks=std::chrono::steady_clock::now().time_since_epoch().count();owner=1400000000+static_cast<int>(ticks%100000000);member=owner+1;stranger=owner+2;
    }
    void TearDown() override {
        if(!db)return;
        db->sql("DELETE FROM group_operation WHERE user_id IN (?,?,?)").bind(owner).bind(member).bind(stranger).execute();
        db->sql("DELETE FROM friend WHERE self_id IN (?,?,?)").bind(owner).bind(member).bind(stranger).execute();
        if(!thread)return;
        db->sql("DELETE FROM group_admission WHERE thread_id=?").bind(thread).execute();
        db->sql("DELETE FROM chat_message WHERE thread_id=?").bind(thread).execute();
        db->sql("DELETE FROM group_chat_member WHERE thread_id=?").bind(thread).execute();
        db->sql("DELETE FROM group_chat WHERE thread_id=?").bind(thread).execute();
        db->sql("DELETE FROM chat_thread WHERE id=?").bind(thread).execute();
    }
    Json::Value Request() {Json::Value r;r["thread_id"]=Json::UInt64(thread);r["request_id"]="request";r["operation_id"]="test-"+std::to_string(owner)+"-"+std::to_string(++sequence);return r;}
    int Mutate(short action,int uid,Json::Value r=Json::Value(),int target=0) {
        if(r.isNull())r=Request();if(action==ID_CREATE_GROUP_REQ)r["name"]="transaction regression";
        if(target){r["target_member_uid"]=target;if(!r.isMember("expected_membership_version"))r["expected_membership_version"]=Json::UInt64(Version(target));}
        int error=mysql->MutateGroup(uid,action,r,Json::Value(),result,events);
        if(error==Success&&result["thread_id"].isUInt64()){thread=result["thread_id"].asUInt64();group.ownerUid=result["group"]["owner_uid"].asInt();}
        return error;
    }
    std::uint64_t Version(int who) {auto r=db->sql("SELECT membership_version FROM group_chat_member WHERE thread_id=? AND user_id=?").bind(thread).bind(who).execute().fetchOne();return r?r[0].get<std::uint64_t>():0;}
    int Op(GroupAction a,int uid,std::uint64_t after=0,const std::vector<std::pair<std::string,std::string>>& t={}) {return mysql->GroupOperation(a,uid,thread,after,100,t,group,members,messages,recipients);}
    void Friend(int a,int b) {db->sql("INSERT INTO friend(self_id,friend_id,back) VALUES(?,?,'')").bind(a).bind(b).execute();}
    Json::Value Decision(const char* field,std::uint64_t id,bool yes) {auto r=Request();r[field]=Json::UInt64(id);r[std::string(field)=="invitation_id"?"accept":"approve"]=yes;return r;}
    void Approval(bool on) {auto r=Request();r["approval_required"]=on;ASSERT_EQ(Mutate(ID_SET_GROUP_APPROVAL_REQ,owner,r),Success);}
    void Create() {ASSERT_EQ(Mutate(ID_CREATE_GROUP_REQ,owner),Success);ASSERT_GT(thread,0u);}
};

TEST_F(GroupTransactionTest, MembershipSingleWriteOfflineHistoryAndLogicalDissolve) {
    Create();ASSERT_EQ(group.ownerUid,owner);
    ASSERT_EQ(Op(GroupAction::Send,owner,0,{{"shared-client-id","before join"}}),Success);auto first=messages.front().messageId;
    EXPECT_EQ(messages.front().recvId,0);EXPECT_EQ(messages.front().senderNick,std::to_string(owner));
    EXPECT_EQ(Op(GroupAction::History,member),GroupNotMember);ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);
    ASSERT_EQ(Op(GroupAction::Members,owner),Success);ASSERT_EQ(members.size(),2u);EXPECT_EQ(members[0].role,2);EXPECT_EQ(members[1].membershipVersion,1u);
    ASSERT_EQ(Op(GroupAction::History,member),Success);ASSERT_EQ(messages.size(),1u);EXPECT_EQ(messages[0].messageId,first);
    ASSERT_EQ(Op(GroupAction::Send,member,0,{{"shared-client-id","offline text"}}),Success);EXPECT_EQ(recipients.size(),2u);
    ASSERT_EQ(Op(GroupAction::History,owner,first),Success);ASSERT_EQ(messages.size(),1u);EXPECT_EQ(messages[0].content,"offline text");
    EXPECT_EQ(Mutate(ID_KICK_GROUP_MEMBER_REQ,member,Json::Value(),owner),GroupPermissionDenied);EXPECT_EQ(Mutate(ID_DISSOLVE_GROUP_REQ,stranger),GroupNotMember);
    ASSERT_EQ(Mutate(ID_KICK_GROUP_MEMBER_REQ,owner,Json::Value(),member),Success);
    EXPECT_EQ(Op(GroupAction::Send,member,0,{{"forged","denied"}}),GroupNotMember);EXPECT_EQ(Op(GroupAction::Members,member),GroupNotMember);EXPECT_EQ(Op(GroupAction::History,member),GroupNotMember);EXPECT_EQ(Op(GroupAction::Authorize,member),GroupNotMember);
    ASSERT_EQ(Mutate(ID_DISSOLVE_GROUP_REQ,owner),Success);EXPECT_EQ(Op(GroupAction::Search,owner),GroupUnavailable);EXPECT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),GroupUnavailable);
    auto count=db->sql("SELECT COUNT(*) FROM chat_message WHERE thread_id=?").bind(thread).execute().fetchOne();EXPECT_EQ(count[0].get<int>(),2);
    EXPECT_THROW(db->sql("DELETE FROM chat_thread WHERE id=?").bind(thread).execute(),std::exception);
}

TEST_F(GroupTransactionTest, FailedBatchRollsBackAndRevocationLocksCurrentAuthorization) {
    const std::string trigger="group_owner_fail_"+std::to_string(owner);
    db->sql("CREATE TRIGGER `"+trigger+"` BEFORE INSERT ON group_chat_member FOR EACH ROW BEGIN IF NEW.user_id="+std::to_string(owner)+" AND NEW.role=2 THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='isolated owner failure'; END IF; END").execute();
    bool installed=true;Defer remove([&](){if(installed)db->sql("DROP TRIGGER `"+trigger+"`").execute();});
    ASSERT_EQ(Mutate(ID_CREATE_GROUP_REQ,owner),ServerBusy);
    auto count=db->sql("SELECT COUNT(*) FROM group_chat WHERE owner_uid=?").bind(owner).execute().fetchOne();EXPECT_EQ(count[0].get<int>(),0);
    count=db->sql("SELECT COUNT(*) FROM group_operation WHERE user_id=?").bind(owner).execute().fetchOne();EXPECT_EQ(count[0].get<int>(),0);
    db->sql("DROP TRIGGER `"+trigger+"`").execute();installed=false;Create();ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);
    EXPECT_EQ(Op(GroupAction::Send,member,0,{{"valid","first"},{"invalid",""}}),Error_Json);
    EXPECT_EQ(mysql->GroupOperation(GroupAction::Send,member,thread,0,10,{{"would-insert","rolled back at gate"}},group,members,messages,recipients,[](){return false;}),MessageTooLarge);
    count=db->sql("SELECT COUNT(*) FROM chat_message WHERE thread_id=?").bind(thread).execute().fetchOne();EXPECT_EQ(count[0].get<int>(),0);
    db->startTransaction();db->sql("SELECT thread_id FROM group_chat WHERE thread_id=? FOR UPDATE").bind(thread).execute().fetchAll();
    db->sql("UPDATE group_chat_member SET active=0,membership_version=membership_version+1 WHERE thread_id=? AND user_id=?").bind(thread).bind(member).execute();
    auto authorization=std::async(std::launch::async,[&](){GroupChatThread g;std::vector<GroupChatMember> m;std::vector<StoredTextMessage> t;std::vector<int> r;auto id=thread;return mysql->GroupOperation(GroupAction::Authorize,member,id,0,0,{},g,m,t,r);});
    EXPECT_EQ(authorization.wait_for(std::chrono::milliseconds(100)),std::future_status::timeout);db->commit();EXPECT_EQ(authorization.get(),GroupNotMember);
    std::vector<GroupChatThread> groups;ASSERT_EQ(mysql->LoadGroupSnapshot(member,groups),Success);EXPECT_TRUE(groups.empty());
}

TEST_F(GroupTransactionTest, InvitationConsentApprovalOfflineRecoveryAndTerminalReplay) {
    Create();Friend(owner,member);Approval(true);
    ASSERT_EQ(Mutate(ID_INVITE_GROUP_REQ,owner,Json::Value(),member),Success);auto invite=result["entry"]["id"].asUInt64();
    EXPECT_EQ(Op(GroupAction::History,member),GroupNotMember);EXPECT_EQ(Op(GroupAction::Authorize,member),GroupNotMember);
    auto list=Request();list["scope"]="inbox";list["after_id"]=Json::UInt64(0);Json::Value inbox;
    ASSERT_EQ(mysql->LoadGroupRequests(member,list,inbox),Success);ASSERT_EQ(inbox["entries"].size(),1u);
    auto accept=Decision("invitation_id",invite,true);ASSERT_EQ(Mutate(ID_DECIDE_GROUP_INVITE_REQ,member,accept),Success);EXPECT_EQ(result["admission"].asString(),"pending");auto application=result["application"]["id"].asUInt64();
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_INVITE_REQ,member,accept),Success);EXPECT_TRUE(result["replayed"].asBool());EXPECT_EQ(Op(GroupAction::Members,member),GroupNotMember);
    auto approve=Decision("application_id",application,true);ASSERT_EQ(Mutate(ID_DECIDE_GROUP_APPLICATION_REQ,owner,approve),Success);ASSERT_EQ(Op(GroupAction::History,member),Success);
    ASSERT_EQ(Mutate(ID_KICK_GROUP_MEMBER_REQ,owner,Json::Value(),member),Success);
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_APPLICATION_REQ,owner,Decision("application_id",application,true)),Success);EXPECT_EQ(Op(GroupAction::History,member),GroupNotMember);
    list["scope"]="results";ASSERT_EQ(mysql->LoadGroupRequests(member,list,inbox),Success);ASSERT_EQ(inbox["entries"].size(),2u);EXPECT_EQ(inbox["entries"][1]["status"].asString(),"approved");
}

TEST_F(GroupTransactionTest, SearchJoinConvertsPendingInvitationAndApprovalOffKeepsQueue) {
    Create();Friend(owner,member);Approval(true);ASSERT_EQ(Mutate(ID_INVITE_GROUP_REQ,owner,Json::Value(),member),Success);auto invitation=result["entry"]["id"].asUInt64();
    ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);ASSERT_EQ(result["application"]["kind"].asString(),"application");auto app=result["application"]["id"].asUInt64();
    auto row=db->sql("SELECT CAST(status AS CHAR) FROM group_admission WHERE id=?").bind(invitation).execute().fetchOne();EXPECT_EQ(row[0].get<std::string>(),"accepted");
    Approval(false);ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);EXPECT_EQ(result["application"]["id"].asUInt64(),app);EXPECT_EQ(Op(GroupAction::History,member),GroupNotMember);
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_APPLICATION_REQ,owner,Decision("application_id",app,false)),Success);
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_APPLICATION_REQ,owner,Decision("application_id",app,true)),Success);EXPECT_EQ(Op(GroupAction::Authorize,member),GroupNotMember);
}

TEST_F(GroupTransactionTest, RolesAtomicTransferLeaveReplayAndInvalidInvitation) {
    Create();ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,stranger),Success);
    auto role=Request();role["role"]=1;ASSERT_EQ(Mutate(ID_SET_GROUP_ADMIN_REQ,owner,role,member),Success);
    EXPECT_EQ(Mutate(ID_KICK_GROUP_MEMBER_REQ,stranger,Json::Value(),member),GroupPermissionDenied);
    EXPECT_EQ(Mutate(ID_TRANSFER_GROUP_OWNER_REQ,member,Json::Value(),stranger),GroupPermissionDenied);
    auto leaveOwner=Request();leaveOwner["expected_membership_version"]=Json::UInt64(Version(owner));EXPECT_EQ(Mutate(ID_LEAVE_GROUP_REQ,owner,leaveOwner),GroupPermissionDenied);
    ASSERT_EQ(Mutate(ID_TRANSFER_GROUP_OWNER_REQ,owner,Json::Value(),member),Success);EXPECT_EQ(group.ownerUid,member);
    auto rows=db->sql("SELECT user_id,role FROM group_chat_member WHERE thread_id=? AND active=1 AND role=2").bind(thread).execute();auto row=rows.fetchOne();ASSERT_TRUE(row);EXPECT_EQ(row[0].get<int>(),member);EXPECT_FALSE(rows.fetchOne());
    auto leave=Request();leave["expected_membership_version"]=Json::UInt64(Version(owner));ASSERT_EQ(Mutate(ID_LEAVE_GROUP_REQ,owner,leave),Success);auto removed=Version(owner);
    ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,owner),Success);EXPECT_GT(Version(owner),removed);ASSERT_EQ(Mutate(ID_LEAVE_GROUP_REQ,owner,leave),Success);EXPECT_TRUE(result["replayed"].asBool());EXPECT_EQ(Op(GroupAction::Authorize,owner),Success);
    leave["operation_id"]="fresh-stale-leave";EXPECT_EQ(Mutate(ID_LEAVE_GROUP_REQ,owner,leave),GroupPermissionDenied);EXPECT_EQ(Op(GroupAction::Authorize,owner),Success);
}

TEST_F(GroupTransactionTest, ConcurrentApprovalDoesNotResurrectAndRollbackPreservesPending) {
    Create();Approval(true);ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);auto app=result["application"]["id"].asUInt64();
    const std::string trigger="group_approval_fail_"+std::to_string(owner);
    db->sql("CREATE TRIGGER `"+trigger+"` BEFORE INSERT ON group_chat_member FOR EACH ROW BEGIN IF NEW.user_id="+std::to_string(member)+" THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='isolated approval failure'; END IF; END").execute();
    bool installed=true;Defer remove([&](){if(installed)db->sql("DROP TRIGGER `"+trigger+"`").execute();});
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_APPLICATION_REQ,owner,Decision("application_id",app,true)),ServerBusy);
    auto row=db->sql("SELECT CAST(status AS CHAR) FROM group_admission WHERE id=?").bind(app).execute().fetchOne();EXPECT_EQ(row[0].get<std::string>(),"pending");
    db->sql("DROP TRIGGER `"+trigger+"`").execute();installed=false;
    auto first=Decision("application_id",app,true),second=Decision("application_id",app,false);
    auto task=[&](const Json::Value& r){Json::Value out;std::vector<GroupEvent> e;return mysql->MutateGroup(owner,ID_DECIDE_GROUP_APPLICATION_REQ,r,Json::Value(),out,e);};
    auto a=std::async(std::launch::async,task,first),b=std::async(std::launch::async,task,second);EXPECT_EQ(a.get(),Success);EXPECT_EQ(b.get(),Success);
    row=db->sql("SELECT CAST(status AS CHAR) FROM group_admission WHERE id=?").bind(app).execute().fetchOne();const auto terminal=row[0].get<std::string>();EXPECT_TRUE(terminal=="approved"||terminal=="rejected");
    EXPECT_EQ(Op(GroupAction::Authorize,member),terminal=="approved"?Success:GroupNotMember);
}

TEST_F(GroupTransactionTest, VersionedAnnouncementUtf8LimitsAndBusinessInputReplay) {
    Create();ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);
    auto r=Request();r["announcement"]="公告\n😀\n保持原文";r["expected_announcement_version"]=Json::UInt64(0);
    EXPECT_EQ(Mutate(ID_UPDATE_GROUP_ANNOUNCEMENT_REQ,member,r),GroupPermissionDenied);ASSERT_EQ(Mutate(ID_UPDATE_GROUP_ANNOUNCEMENT_REQ,owner,r),Success);EXPECT_EQ(result["group"]["announcement"].asString(),r["announcement"].asString());EXPECT_EQ(result["group"]["announcement_version"].asUInt64(),1u);
    auto changed=r;changed["announcement"]="changed input same operation";EXPECT_EQ(Mutate(ID_UPDATE_GROUP_ANNOUNCEMENT_REQ,owner,changed),Error_Json);
    ASSERT_EQ(Mutate(ID_UPDATE_GROUP_ANNOUNCEMENT_REQ,owner,r),Success);EXPECT_TRUE(result["replayed"].asBool());
    r=Request();r["announcement"]=std::string(1025,'a');r["expected_announcement_version"]=Json::UInt64(1);EXPECT_EQ(Mutate(ID_UPDATE_GROUP_ANNOUNCEMENT_REQ,owner,r),Error_Json);
    r["announcement"]=std::string(1024,'"');EXPECT_EQ(Mutate(ID_UPDATE_GROUP_ANNOUNCEMENT_REQ,owner,r),MessageTooLarge);
    r["announcement"]=std::string("\xc0\x80",2);EXPECT_EQ(Mutate(ID_UPDATE_GROUP_ANNOUNCEMENT_REQ,owner,r),Error_Json);
    ASSERT_EQ(Op(GroupAction::Members,owner),Success);EXPECT_EQ(group.announcementVersion,1u);EXPECT_EQ(group.announcement,"公告\n😀\n保持原文");
    r=Request();r["announcement"]=std::string("a\0b",3);r["expected_announcement_version"]=Json::UInt64(1);
    ASSERT_EQ(Mutate(ID_UPDATE_GROUP_ANNOUNCEMENT_REQ,owner,r),Success);
    ASSERT_EQ(Op(GroupAction::Members,owner),Success);EXPECT_EQ(group.announcement,std::string("a\0b",3));
}

TEST_F(GroupTransactionTest, TransferLeaveRacePreservesExactlyOneOwner) {
    Create();ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);
    auto transfer=Request();transfer["target_member_uid"]=member;transfer["expected_membership_version"]=Json::UInt64(Version(member));
    auto leave=Request();leave["expected_membership_version"]=Json::UInt64(Version(member));
    auto t=std::async(std::launch::async,[&](){Json::Value out;std::vector<GroupEvent> e;return mysql->MutateGroup(owner,ID_TRANSFER_GROUP_OWNER_REQ,transfer,Json::Value(),out,e);});
    auto l=std::async(std::launch::async,[&](){Json::Value out;std::vector<GroupEvent> e;return mysql->MutateGroup(member,ID_LEAVE_GROUP_REQ,leave,Json::Value(),out,e);});
    int tr=t.get(),lr=l.get();EXPECT_TRUE((tr==Success&&lr==GroupPermissionDenied)||(lr==Success&&tr==GroupPermissionDenied));
    auto row=db->sql("SELECT COUNT(*) FROM group_chat g JOIN group_chat_member m ON m.thread_id=g.thread_id AND m.user_id=g.owner_uid WHERE g.thread_id=? AND g.active=1 AND m.active=1 AND m.role=2").bind(thread).execute().fetchOne();EXPECT_EQ(row[0].get<int>(),1);
    row=db->sql("SELECT COUNT(*) FROM group_chat_member WHERE thread_id=? AND active=1 AND role=2").bind(thread).execute().fetchOne();EXPECT_EQ(row[0].get<int>(),1);
}

TEST_F(GroupTransactionTest, RevokedAdminCannotReplayApprovalKickOrAnnouncementSuccess) {
    Create();ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);
    auto role=Request();role["role"]=1;ASSERT_EQ(Mutate(ID_SET_GROUP_ADMIN_REQ,owner,role,member),Success);
    Approval(true);ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,stranger),Success);
    auto decision=Decision("application_id",result["application"]["id"].asUInt64(),true);
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_APPLICATION_REQ,member,decision),Success);
    auto announcement=Request();announcement["announcement"]="admin text";announcement["expected_announcement_version"]=Json::UInt64(0);
    ASSERT_EQ(Mutate(ID_UPDATE_GROUP_ANNOUNCEMENT_REQ,member,announcement),Success);
    auto kick=Request();kick["target_member_uid"]=stranger;kick["expected_membership_version"]=Json::UInt64(Version(stranger));
    ASSERT_EQ(Mutate(ID_KICK_GROUP_MEMBER_REQ,member,kick),Success);
    role=Request();role["role"]=0;ASSERT_EQ(Mutate(ID_SET_GROUP_ADMIN_REQ,owner,role,member),Success);
    EXPECT_EQ(Mutate(ID_DECIDE_GROUP_APPLICATION_REQ,member,decision),GroupPermissionDenied);
    EXPECT_EQ(Mutate(ID_UPDATE_GROUP_ANNOUNCEMENT_REQ,member,announcement),GroupPermissionDenied);
    EXPECT_EQ(Mutate(ID_KICK_GROUP_MEMBER_REQ,member,kick),GroupPermissionDenied);
}

TEST_F(GroupTransactionTest, InviterLeaveInvalidatesOnlyUnacceptedInvitations) {
    Create();ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);Friend(member,stranger);
    ASSERT_EQ(Mutate(ID_INVITE_GROUP_REQ,member,Json::Value(),stranger),Success);auto invitation=result["entry"]["id"].asUInt64();
    auto leave=Request();leave["expected_membership_version"]=Json::UInt64(Version(member));ASSERT_EQ(Mutate(ID_LEAVE_GROUP_REQ,member,leave),Success);
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_INVITE_REQ,stranger,Decision("invitation_id",invitation,true)),Success);
    EXPECT_EQ(result["entry"]["status"].asString(),"invalid");EXPECT_EQ(Op(GroupAction::Authorize,stranger),GroupNotMember);
    ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);Approval(true);
    ASSERT_EQ(Mutate(ID_INVITE_GROUP_REQ,member,Json::Value(),stranger),Success);invitation=result["entry"]["id"].asUInt64();
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_INVITE_REQ,stranger,Decision("invitation_id",invitation,true)),Success);auto application=result["application"]["id"].asUInt64();
    leave=Request();leave["expected_membership_version"]=Json::UInt64(Version(member));ASSERT_EQ(Mutate(ID_LEAVE_GROUP_REQ,member,leave),Success);
    auto row=db->sql("SELECT CAST(status AS CHAR) FROM group_admission WHERE id=?").bind(application).execute().fetchOne();EXPECT_EQ(row[0].get<std::string>(),"pending");
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_APPLICATION_REQ,owner,Decision("application_id",application,true)),Success);
    EXPECT_EQ(Op(GroupAction::Authorize,stranger),Success);
}
} // namespace
