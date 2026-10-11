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
    int owner=0,member=0,stranger=0,other=0,sequence=0;
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
        auto ticks=std::chrono::steady_clock::now().time_since_epoch().count();owner=1400000000+static_cast<int>(ticks%100000000);member=owner+1;stranger=owner+2;other=owner+3;
        for(int uid:{owner,member,stranger,other})db->sql("INSERT INTO user(uid,name,email) VALUES(?,?,?)").bind(uid).bind("group_fixture_"+std::to_string(uid)).bind(std::to_string(uid)+"@group-fixture.invalid").execute();
    }
    void TearDown() override {
        if(!db)return;
        db->sql("DELETE FROM group_operation WHERE user_id IN (?,?,?,?)").bind(owner).bind(member).bind(stranger).bind(other).execute();
        db->sql("DELETE FROM friend WHERE self_id IN (?,?,?,?) OR friend_id IN (?,?,?,?)").bind(owner).bind(member).bind(stranger).bind(other).bind(owner).bind(member).bind(stranger).bind(other).execute();
        db->sql("DELETE FROM user WHERE uid IN (?,?,?,?)").bind(owner).bind(member).bind(stranger).bind(other).execute();
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
        if(error==Success)for(const auto& event:events) {
            EXPECT_EQ(event.payload["uid"].asInt(),event.targetUid);
            EXPECT_EQ(event.payload["type"].asString(),"group");
            Json::StreamWriterBuilder compact;compact["indentation"]="";
            EXPECT_LE(Json::writeString(compact,event.payload).size(),4096u);
        }
        if(error==Success&&result["thread_id"].isUInt64()){thread=result["thread_id"].asUInt64();group.ownerUid=result["group"]["owner_uid"].asInt();}
        return error;
    }
    std::uint64_t Version(int who) {auto r=db->sql("SELECT membership_version FROM group_chat_member WHERE thread_id=? AND user_id=?").bind(thread).bind(who).execute().fetchOne();return r?r[0].get<std::uint64_t>():0;}
    int Op(GroupAction a,int uid,std::uint64_t after=0,const std::vector<std::pair<std::string,std::string>>& t={}) {return mysql->GroupOperation(a,uid,thread,after,100,t,group,members,messages,recipients);}
    void Friend(int a,int b) {db->sql("INSERT INTO friend(self_id,friend_id,back) VALUES(?,?,'')").bind(a).bind(b).execute();db->sql("INSERT INTO friend(self_id,friend_id,back) VALUES(?,?,'')").bind(b).bind(a).execute();}
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
    ASSERT_EQ(Mutate(ID_INVITE_GROUP_REQ,owner,Json::Value(),member),Success);
    EXPECT_EQ(result["entry"]["id"].asUInt64(),invite); // Different business key, same single pending invitation.
    EXPECT_EQ(Op(GroupAction::History,member),GroupNotMember);EXPECT_EQ(Op(GroupAction::Authorize,member),GroupNotMember);
    EXPECT_EQ(Op(GroupAction::Send,member,0,{{"pending-send","must not persist"}}),GroupNotMember);
    auto count=db->sql("SELECT COUNT(*) FROM chat_message WHERE thread_id=?").bind(thread).execute().fetchOne();
    EXPECT_EQ(count[0].get<int>(),0);
    auto list=Request();list["scope"]="inbox";list["after_id"]=Json::UInt64(0);Json::Value inbox;
    ASSERT_EQ(mysql->LoadGroupRequests(member,list,inbox),Success);ASSERT_EQ(inbox["entries"].size(),1u);
    auto accept=Decision("invitation_id",invite,true);ASSERT_EQ(Mutate(ID_DECIDE_GROUP_INVITE_REQ,member,accept),Success);EXPECT_EQ(result["admission"].asString(),"pending");auto application=result["application"]["id"].asUInt64();
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_INVITE_REQ,member,accept),Success);EXPECT_TRUE(result["replayed"].asBool());EXPECT_EQ(Op(GroupAction::Members,member),GroupNotMember);
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_INVITE_REQ,member,Decision("invitation_id",invite,false)),Success);
    EXPECT_TRUE(result["already_processed"].asBool());EXPECT_TRUE(result["status_conflict"].asBool());
    EXPECT_EQ(result["entry"]["status"].asString(),"accepted");EXPECT_TRUE(events.empty());
    auto approve=Decision("application_id",application,true);ASSERT_EQ(Mutate(ID_DECIDE_GROUP_APPLICATION_REQ,owner,approve),Success);ASSERT_EQ(Op(GroupAction::History,member),Success);
    ASSERT_EQ(Mutate(ID_KICK_GROUP_MEMBER_REQ,owner,Json::Value(),member),Success);
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_APPLICATION_REQ,owner,Decision("application_id",application,true)),Success);EXPECT_TRUE(result["already_processed"].asBool());EXPECT_FALSE(result["status_conflict"].asBool());EXPECT_EQ(Op(GroupAction::History,member),GroupNotMember);
    list["scope"]="results";ASSERT_EQ(mysql->LoadGroupRequests(member,list,inbox),Success);ASSERT_EQ(inbox["entries"].size(),2u);EXPECT_EQ(inbox["entries"][1]["status"].asString(),"approved");
}

TEST_F(GroupTransactionTest, SearchJoinConvertsPendingInvitationAndApprovalOffKeepsQueue) {
    Create();Friend(owner,member);Approval(true);ASSERT_EQ(Mutate(ID_INVITE_GROUP_REQ,owner,Json::Value(),member),Success);auto invitation=result["entry"]["id"].asUInt64();
    ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);ASSERT_EQ(result["application"]["kind"].asString(),"application");auto app=result["application"]["id"].asUInt64();
    auto row=db->sql("SELECT CAST(status AS CHAR) FROM group_admission WHERE id=?").bind(invitation).execute().fetchOne();EXPECT_EQ(row[0].get<std::string>(),"accepted");
    Approval(false);ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);EXPECT_EQ(result["application"]["id"].asUInt64(),app);EXPECT_EQ(Op(GroupAction::History,member),GroupNotMember);
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_APPLICATION_REQ,owner,Decision("application_id",app,false)),Success);
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_APPLICATION_REQ,owner,Decision("application_id",app,true)),Success);EXPECT_TRUE(result["already_processed"].asBool());EXPECT_TRUE(result["status_conflict"].asBool());EXPECT_EQ(result["entry"]["status"].asString(),"rejected");EXPECT_EQ(Op(GroupAction::Authorize,member),GroupNotMember);
}

TEST_F(GroupTransactionTest, RolesAtomicTransferLeaveReplayAndInvalidInvitation) {
    Create();ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,stranger),Success);
    auto role=Request();role["role"]=1;ASSERT_EQ(Mutate(ID_SET_GROUP_ADMIN_REQ,owner,role,member),Success);
    EXPECT_EQ(Mutate(ID_KICK_GROUP_MEMBER_REQ,stranger,Json::Value(),member),GroupPermissionDenied);
    EXPECT_EQ(Mutate(ID_TRANSFER_GROUP_OWNER_REQ,member,Json::Value(),stranger),GroupPermissionDenied);
    role=Request();role["role"]=1;ASSERT_EQ(Mutate(ID_SET_GROUP_ADMIN_REQ,owner,role,stranger),Success);
    auto adminLeave=Request();const auto adminVersion=Version(stranger);
    adminLeave["expected_membership_version"]=Json::UInt64(adminVersion);
    ASSERT_EQ(Mutate(ID_LEAVE_GROUP_REQ,stranger,adminLeave),Success);
    EXPECT_GT(Version(stranger),adminVersion);EXPECT_EQ(Op(GroupAction::Authorize,stranger),GroupNotMember);
    auto leaveOwner=Request();leaveOwner["expected_membership_version"]=Json::UInt64(Version(owner));EXPECT_EQ(Mutate(ID_LEAVE_GROUP_REQ,owner,leaveOwner),GroupPermissionDenied);
    ASSERT_EQ(Mutate(ID_TRANSFER_GROUP_OWNER_REQ,owner,Json::Value(),member),Success);EXPECT_EQ(group.ownerUid,member);
    auto rows=db->sql("SELECT user_id,role FROM group_chat_member WHERE thread_id=? AND active=1 AND role=2").bind(thread).execute();auto row=rows.fetchOne();ASSERT_TRUE(row);EXPECT_EQ(row[0].get<int>(),member);EXPECT_FALSE(rows.fetchOne());
    auto leave=Request();leave["expected_membership_version"]=Json::UInt64(Version(owner));ASSERT_EQ(Mutate(ID_LEAVE_GROUP_REQ,owner,leave),Success);auto removed=Version(owner);
    ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,owner),Success);EXPECT_GT(Version(owner),removed);ASSERT_EQ(Mutate(ID_LEAVE_GROUP_REQ,owner,leave),Success);EXPECT_TRUE(result["replayed"].asBool());EXPECT_EQ(Op(GroupAction::Authorize,owner),Success);
    leave["operation_id"]="fresh-stale-leave";EXPECT_EQ(Mutate(ID_LEAVE_GROUP_REQ,owner,leave),GroupPermissionDenied);EXPECT_EQ(Op(GroupAction::Authorize,owner),Success);
}

TEST_F(GroupTransactionTest, ConcurrentApprovalDoesNotResurrectAndRollbackPreservesPending) {
    Create();ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,stranger),Success);ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,other),Success);
    for(int admin:{stranger,other}){auto r=Request();r["role"]=1;ASSERT_EQ(Mutate(ID_SET_GROUP_ADMIN_REQ,owner,r,admin),Success);}
    Approval(true);ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);auto app=result["application"]["id"].asUInt64();
    const std::string trigger="group_approval_fail_"+std::to_string(owner);
    db->sql("CREATE TRIGGER `"+trigger+"` BEFORE INSERT ON group_chat_member FOR EACH ROW BEGIN IF NEW.user_id="+std::to_string(member)+" THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='isolated approval failure'; END IF; END").execute();
    bool installed=true;Defer remove([&](){if(installed)db->sql("DROP TRIGGER `"+trigger+"`").execute();});
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_APPLICATION_REQ,owner,Decision("application_id",app,true)),ServerBusy);
    auto row=db->sql("SELECT CAST(status AS CHAR) FROM group_admission WHERE id=?").bind(app).execute().fetchOne();EXPECT_EQ(row[0].get<std::string>(),"pending");
    db->sql("DROP TRIGGER `"+trigger+"`").execute();installed=false;
    auto first=Decision("application_id",app,true),second=Decision("application_id",app,false);
    auto task=[&](int admin,const Json::Value& r){Json::Value out;std::vector<GroupEvent> e;int code=mysql->MutateGroup(admin,ID_DECIDE_GROUP_APPLICATION_REQ,r,Json::Value(),out,e);return std::make_pair(code,out);};
    auto a=std::async(std::launch::async,task,stranger,first),b=std::async(std::launch::async,task,other,second);
    auto approved=a.get(),rejected=b.get();EXPECT_EQ(approved.first,Success);EXPECT_EQ(rejected.first,Success);
    EXPECT_EQ(static_cast<int>(approved.second["already_processed"].asBool())+static_cast<int>(rejected.second["already_processed"].asBool()),1);
    if(approved.second["already_processed"].asBool())EXPECT_TRUE(approved.second["status_conflict"].asBool());
    if(rejected.second["already_processed"].asBool())EXPECT_TRUE(rejected.second["status_conflict"].asBool());
    row=db->sql("SELECT CAST(status AS CHAR) FROM group_admission WHERE id=?").bind(app).execute().fetchOne();const auto terminal=row[0].get<std::string>();EXPECT_TRUE(terminal=="approved"||terminal=="rejected");
    EXPECT_EQ(Op(GroupAction::Authorize,member),terminal=="approved"?Success:GroupNotMember);
    row=db->sql("SELECT COUNT(*) FROM group_chat_member WHERE thread_id=? AND user_id=? AND active=1").bind(thread).bind(member).execute().fetchOne();
    EXPECT_EQ(row[0].get<int>(),terminal=="approved"?1:0);
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

TEST_F(GroupTransactionTest, InvitationsRequireExistingAccountAndReciprocalFriendship) {
    Create();
    db->sql("INSERT INTO friend(self_id,friend_id) VALUES(?,?)").bind(owner).bind(member).execute();
    EXPECT_EQ(Mutate(ID_INVITE_GROUP_REQ,owner,Json::Value(),member),GroupPermissionDenied);
    db->sql("INSERT INTO friend(self_id,friend_id) VALUES(?,?)").bind(member).bind(owner).execute();
    ASSERT_EQ(Mutate(ID_INVITE_GROUP_REQ,owner,Json::Value(),member),Success);
    const int nonexistent=owner+4;
    Friend(owner,nonexistent);
    EXPECT_EQ(Mutate(ID_INVITE_GROUP_REQ,owner,Json::Value(),nonexistent),SearchUserNoExist);
    auto count=db->sql("SELECT COUNT(*) FROM group_admission WHERE thread_id=?").bind(thread).execute().fetchOne();
    EXPECT_EQ(count[0].get<int>(),1);
}

TEST_F(GroupTransactionTest, AdministratorRoleAndKickPermissionMatrix) {
    Create();ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,stranger),Success);
    for(int admin:{member,stranger}){auto r=Request();r["role"]=1;ASSERT_EQ(Mutate(ID_SET_GROUP_ADMIN_REQ,owner,r,admin),Success);}
    auto demote=Request();demote["role"]=0;
    EXPECT_EQ(Mutate(ID_SET_GROUP_ADMIN_REQ,member,demote,stranger),GroupPermissionDenied);
    EXPECT_EQ(Mutate(ID_KICK_GROUP_MEMBER_REQ,member,Json::Value(),stranger),GroupPermissionDenied);
    EXPECT_EQ(Mutate(ID_KICK_GROUP_MEMBER_REQ,member,Json::Value(),owner),GroupPermissionDenied);
    ASSERT_EQ(Mutate(ID_KICK_GROUP_MEMBER_REQ,owner,Json::Value(),stranger),Success);
    ASSERT_EQ(Mutate(ID_KICK_GROUP_MEMBER_REQ,owner,Json::Value(),member),Success);
    EXPECT_EQ(Op(GroupAction::Authorize,stranger),GroupNotMember);
    EXPECT_EQ(Op(GroupAction::Authorize,member),GroupNotMember);
}

TEST_F(GroupTransactionTest, TransferOwnerUpdateFailureRollsBackBothRolesAndVersions) {
    Create();ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);
    auto appoint=Request();appoint["role"]=1;ASSERT_EQ(Mutate(ID_SET_GROUP_ADMIN_REQ,owner,appoint,member),Success);
    ASSERT_EQ(Op(GroupAction::Members,owner),Success);const auto groupVersion=group.version;
    const auto ownerVersion=Version(owner),memberVersion=Version(member);
    const std::string trigger="group_transfer_fail_"+std::to_string(owner);
    db->sql("CREATE TRIGGER `"+trigger+"` BEFORE UPDATE ON group_chat FOR EACH ROW BEGIN IF NEW.thread_id="+std::to_string(thread)+" AND NEW.owner_uid<>OLD.owner_uid THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='isolated owner update failure'; END IF; END").execute();
    bool installed=true;Defer remove([&](){if(installed)db->sql("DROP TRIGGER `"+trigger+"`").execute();});
    EXPECT_EQ(Mutate(ID_TRANSFER_GROUP_OWNER_REQ,owner,Json::Value(),member),ServerBusy);EXPECT_TRUE(events.empty());
    ASSERT_EQ(Op(GroupAction::Members,owner),Success);EXPECT_EQ(group.ownerUid,owner);EXPECT_EQ(group.version,groupVersion);
    ASSERT_EQ(members.size(),2u);EXPECT_EQ(members[0].uid,owner);EXPECT_EQ(members[0].role,2);EXPECT_EQ(members[1].uid,member);EXPECT_EQ(members[1].role,1);
    EXPECT_EQ(Version(owner),ownerVersion);EXPECT_EQ(Version(member),memberVersion);
    db->sql("DROP TRIGGER `"+trigger+"`").execute();installed=false;
    ASSERT_EQ(Mutate(ID_TRANSFER_GROUP_OWNER_REQ,owner,Json::Value(),member),Success);
}

TEST_F(GroupTransactionTest, OnlyExplicitMembershipConfirmationCanGrantActivation) {
    Create();EXPECT_TRUE(result["membership_granted"].asBool());
    auto join=Request();
    ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member,join),Success);EXPECT_TRUE(result["membership_granted"].asBool());
    bool newMemberGrant=false,ownerGrant=false;
    for(const auto& event:events){if(event.targetUid==member&&event.type=="members")newMemberGrant=event.payload["membership_granted"].asBool();if(event.targetUid==owner)ownerGrant|=event.payload["membership_granted"].asBool();}
    EXPECT_TRUE(newMemberGrant);EXPECT_FALSE(ownerGrant);
    ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member,join),Success);EXPECT_TRUE(result["replayed"].asBool());EXPECT_TRUE(result["membership_granted"].asBool());EXPECT_TRUE(events.empty());
    ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);EXPECT_TRUE(result["membership_granted"].asBool());EXPECT_TRUE(events.empty());
    auto announcement=Request();announcement["announcement"]="metadata cannot grant";announcement["expected_announcement_version"]=Json::UInt64(0);
    ASSERT_EQ(Mutate(ID_UPDATE_GROUP_ANNOUNCEMENT_REQ,owner,announcement),Success);
    for(const auto& event:events){EXPECT_EQ(event.type,"metadata");EXPECT_FALSE(event.payload["membership_granted"].asBool());}
    ASSERT_EQ(Mutate(ID_KICK_GROUP_MEMBER_REQ,owner,Json::Value(),member),Success);
    ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member,join),Success);EXPECT_TRUE(result["replayed"].asBool());EXPECT_FALSE(result["membership_granted"].asBool());EXPECT_TRUE(events.empty());EXPECT_EQ(Op(GroupAction::Authorize,member),GroupNotMember);
    Approval(true);ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);EXPECT_FALSE(result["membership_granted"].asBool());
    auto application=result["application"]["id"].asUInt64();
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_APPLICATION_REQ,owner,Decision("application_id",application,true)),Success);
    newMemberGrant=false;
    for(const auto& event:events){if(event.targetUid==member&&event.type=="members")newMemberGrant=event.payload["membership_granted"].asBool();if(event.type=="result"){EXPECT_FALSE(event.payload["membership_proof"].asBool());EXPECT_FALSE(event.payload["membership_granted"].asBool());}}
    EXPECT_TRUE(newMemberGrant);EXPECT_FALSE(result["membership_granted"].asBool());
    ASSERT_EQ(Mutate(ID_KICK_GROUP_MEMBER_REQ,owner,Json::Value(),member),Success);
    auto terminalRetry=Decision("application_id",application,true);
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_APPLICATION_REQ,owner,terminalRetry),Success);EXPECT_TRUE(result["already_processed"].asBool());EXPECT_FALSE(result["membership_granted"].asBool());EXPECT_TRUE(events.empty());
}

TEST_F(GroupTransactionTest, RequestScopesPreservePendingTerminalKindsAndGlobalPagination) {
    Create(); Friend(owner,member); Approval(true);
    ASSERT_EQ(Mutate(ID_INVITE_GROUP_REQ,owner,Json::Value(),member),Success);
    const auto invitation=result["entry"]["id"].asUInt64();
    auto originalJoin=Request();ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member,originalJoin),Success);
    const auto firstApplication=result["application"]["id"].asUInt64();
    auto page=[&](const char* scope,std::uint64_t after=0) {
        auto request=Request();request["scope"]=scope;request["after_id"]=Json::UInt64(after);
        request["page_size"]=1;request["uid"]=stranger; // Body UID must not change authenticated ownership.
        Json::Value out;EXPECT_EQ(mysql->LoadGroupRequests(member,request,out),Success);
        EXPECT_EQ(out["uid"].asInt(),member);return out;
    };
    auto p=page("applications");ASSERT_EQ(p["entries"].size(),1u);
    EXPECT_EQ(p["entries"][0]["id"].asUInt64(),firstApplication);
    EXPECT_EQ(p["entries"][0]["status"].asString(),"pending");
    EXPECT_FALSE(p.isMember("group"));EXPECT_EQ(Op(GroupAction::Authorize,member),GroupNotMember);
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_APPLICATION_REQ,owner,Decision("application_id",firstApplication,false)),Success);
    ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member,originalJoin),Success);EXPECT_TRUE(result["replayed"].asBool());
    EXPECT_TRUE(result["already_processed"].asBool());EXPECT_TRUE(result["status_conflict"].asBool());
    EXPECT_EQ(result["application"]["status"].asString(),"rejected");
    auto pending=db->sql("SELECT COUNT(*) FROM group_admission WHERE thread_id=? AND user_id=? AND status='pending'").bind(thread).bind(member).execute().fetchOne();
    EXPECT_EQ(pending[0].get<int>(),0);EXPECT_TRUE(events.empty());
    ASSERT_EQ(Mutate(ID_JOIN_GROUP_REQ,member),Success);
    const auto secondApplication=result["application"]["id"].asUInt64();
    p=page("applications");ASSERT_EQ(p["entries"].size(),1u);
    EXPECT_EQ(p["entries"][0]["kind"].asString(),"application");
    EXPECT_EQ(p["entries"][0]["status"].asString(),"rejected");EXPECT_TRUE(p["load_more"].asBool());
    p=page("applications",p["next_id"].asUInt64());ASSERT_EQ(p["entries"].size(),1u);
    EXPECT_EQ(p["entries"][0]["id"].asUInt64(),secondApplication);
    EXPECT_EQ(p["entries"][0]["status"].asString(),"pending");EXPECT_FALSE(p["load_more"].asBool());
    ASSERT_EQ(Mutate(ID_DECIDE_GROUP_APPLICATION_REQ,owner,Decision("application_id",secondApplication,false)),Success);
    ASSERT_EQ(Mutate(ID_INVITE_GROUP_REQ,owner,Json::Value(),member),Success);
    const auto pendingInvitation=result["entry"]["id"].asUInt64();
    p=page("inbox");ASSERT_EQ(p["entries"].size(),1u);
    EXPECT_EQ(p["entries"][0]["id"].asUInt64(),invitation);EXPECT_EQ(p["entries"][0]["status"].asString(),"accepted");
    EXPECT_EQ(p["entries"][0]["kind"].asString(),"invitation");EXPECT_TRUE(p["load_more"].asBool());
    p=page("inbox",p["next_id"].asUInt64());ASSERT_EQ(p["entries"].size(),1u);
    EXPECT_EQ(p["entries"][0]["id"].asUInt64(),pendingInvitation);EXPECT_EQ(p["entries"][0]["status"].asString(),"pending");
    EXPECT_FALSE(p["load_more"].asBool());
    std::uint64_t after=0;unsigned terminals=0;
    do {p=page("results",after);ASSERT_EQ(p["entries"].size(),1u);
        EXPECT_NE(p["entries"][0]["status"].asString(),"pending");
        const auto next=p["next_id"].asUInt64();EXPECT_GT(next,after);after=next;++terminals;
    }while(p["load_more"].asBool());
    EXPECT_EQ(terminals,3u);EXPECT_EQ(after,secondApplication);
}
} // namespace
