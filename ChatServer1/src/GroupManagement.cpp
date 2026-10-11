// 群管理事务：群行 -> 当前成员 -> 准入，提交前建立带版本的接收者证明和业务重放结果。
#include "GroupDatabase.h"
#include "MysqlMgr.h"
#include "const.h"
#include <algorithm>
#include <limits>
#include <sstream>

namespace
{
std::string Compact(const Json::Value &v)
{
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    return Json::writeString(b, v);
}
bool Id(const Json::Value &v, std::uint64_t &n)
{
    if (v.isUInt64())
    {
        n = v.asUInt64();
        return true;
    }
    if (!v.isString() || v.asString().empty())
        return false;
    n = 0;
    for (unsigned char c : v.asString())
    {
        if (c < '0' || c > '9' || n > (std::numeric_limits<std::uint64_t>::max() - (c - '0')) / 10)
            return false;
        n = n * 10 + c - '0';
    }
    return true;
}
bool Utf8(const std::string &s)
{
    for (size_t i = 0; i < s.size();)
    {
        unsigned char c = s[i++];
        if (c < 0x80)
            continue;
        unsigned count = 0;
        std::uint32_t cp = 0, min = 0;
        if (c >= 0xc2 && c <= 0xdf)
        {
            count = 1;
            cp = c & 31;
            min = 0x80;
        }
        else if (c >= 0xe0 && c <= 0xef)
        {
            count = 2;
            cp = c & 15;
            min = 0x800;
        }
        else if (c >= 0xf0 && c <= 0xf4)
        {
            count = 3;
            cp = c & 7;
            min = 0x10000;
        }
        else
            return false;
        if (i + count > s.size())
            return false;
        while (count--)
        {
            unsigned char t = s[i++];
            if ((t & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (t & 63);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
    }
    return true;
}
Json::Value Admission(const mysqlx::Row &r)
{
    Json::Value e;
    e["id"] = Json::UInt64(r[0].get<std::uint64_t>());
    e["thread_id"] = Json::UInt64(r[1].get<std::uint64_t>());
    e["kind"] = r[2].get<std::string>();
    int user = r[3].get<int>();
    e["target_uid"] = user;
    e["applicant_uid"] = user;
    e["inviter_uid"] = r[4].get<int>();
    e["status"] = r[5].get<std::string>();
    e["explanation"] = group_db::Text(r, 6);
    e["created_at_ms"] = Json::UInt64(r[7].get<std::uint64_t>());
    e["updated_at_ms"] = Json::UInt64(r[8].get<std::uint64_t>());
    e["group_name"] = group_db::Text(r, 9);
    return e;
}
constexpr const char *AdmissionColumns =
    "a.id,a.thread_id,CAST(a.kind AS CHAR),a.user_id,a.inviter_uid,CAST(a.status AS "
    "CHAR),a.explanation,CAST(UNIX_TIMESTAMP(a.created_at)*1000 AS "
    "UNSIGNED),CAST(UNIX_TIMESTAMP(a.updated_at)*1000 AS UNSIGNED),g.name";
Json::Value ReadAdmission(mysqlx::Session &db, std::uint64_t id)
{
    auto r =
        db.sql(std::string("SELECT ") + AdmissionColumns +
               " FROM group_admission a JOIN group_chat g ON g.thread_id=a.thread_id WHERE a.id=?")
            .bind(id)
            .execute()
            .fetchOne();
    return r ? Admission(r) : Json::Value();
}
} // namespace

Json::Value BuildGroupJson(const GroupChatThread &g, const char *source)
{
    Json::Value v;
    v["thread_id"] = Json::UInt64(g.threadId);
    v["type"] = "group";
    v["name"] = g.name;
    v["owner_uid"] = g.ownerUid;
    v["active"] = g.active;
    v["version"] = Json::UInt64(g.version);
    v["approval_required"] = g.approvalRequired;
    v["announcement"] = g.announcement;
    v["announcement_version"] = Json::UInt64(g.announcementVersion);
    v["announcement_author_uid"] = g.announcementAuthorUid;
    v["announcement_updated_at_ms"] = Json::UInt64(g.announcementUpdatedAtMs);
    v["avatar_resource_id"] = g.avatarResourceId;
    v["avatar_mime_type"] = g.avatarMimeType;
    v["avatar_size"] = Json::UInt64(g.avatarSize);
    v["avatar_width"] = g.avatarWidth;
    v["avatar_height"] = g.avatarHeight;
    v["avatar_version"] = Json::UInt64(g.avatarVersion);
    v["member"] = g.member;
    v["membership_proof"] = g.member;
    v["membership_version"] = Json::UInt64(g.membershipVersion);
    v["role"] = g.role;
    v["source"] = source;
    return v;
}

int MysqlMgr::MutateGroup(int uid, short action, const Json::Value &request,
                          const Json::Value &avatar, Json::Value &response,
                          std::vector<GroupEvent> &events)
{
    events.clear();
    response = Json::Value(Json::objectValue);
    if (uid <= 0 || !request.isObject() || !request["operation_id"].isString())
        return Error_Json;
    if (Compact(request).size() > 2048)
        return MessageTooLarge;
    const std::string operation = request["operation_id"].asString();
    if (operation.empty() || operation.size() > 128 ||
        !std::all_of(operation.begin(), operation.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '-';
        }))
        return Error_Json;
    // operation_id代表业务意图；request_id仅匹配回包，不参与持久重放身份。
    Json::Value input = request;
    input.removeMember("request_id");
    input.removeMember("token");
    const std::string identity = Compact(input);
    std::uint64_t thread = 0, expected = 0, decision = 0;
    if (action != ID_CREATE_GROUP_REQ && (!Id(request["thread_id"], thread) || thread == 0))
        return Error_Json;
    int target = 0;
    if (action == ID_INVITE_GROUP_REQ || action == ID_KICK_GROUP_MEMBER_REQ ||
        action == ID_SET_GROUP_ADMIN_REQ || action == ID_TRANSFER_GROUP_OWNER_REQ)
    {
        if (!request["target_member_uid"].isInt() ||
            (target = request["target_member_uid"].asInt()) <= 0)
            return Error_Json;
    }
    // kick/admin/transfer核对目标成员版本，leave核对调用者自身版本；
    // 同UID重入后的成员版本不同，旧窗口不能作用于新成员身份。
    if (action == ID_KICK_GROUP_MEMBER_REQ || action == ID_SET_GROUP_ADMIN_REQ ||
        action == ID_TRANSFER_GROUP_OWNER_REQ || action == ID_LEAVE_GROUP_REQ)
        if (!Id(request["expected_membership_version"], expected) || !expected)
            return Error_Json;
    std::string explanation;
    if (request.isMember("explanation"))
    {
        if (!request["explanation"].isString())
            return Error_Json;
        explanation = request["explanation"].asString();
        if (explanation.size() > 256 || !Utf8(explanation))
            return Error_Json;
    }
    if (action == ID_CREATE_GROUP_REQ &&
        (!request["name"].isString() || request["name"].asString().empty() ||
         request["name"].asString().size() > 255 || !Utf8(request["name"].asString())))
        return Error_Json;
    if (action == ID_SET_GROUP_ADMIN_REQ &&
        (!request["role"].isInt() ||
         (request["role"].asInt() != 0 && request["role"].asInt() != 1)))
        return Error_Json;
    if (action == ID_SET_GROUP_APPROVAL_REQ && !request["approval_required"].isBool())
        return Error_Json;
    if (action == ID_UPDATE_GROUP_ANNOUNCEMENT_REQ &&
        (!request["announcement"].isString() || request["announcement"].asString().size() > 1024 ||
         !Utf8(request["announcement"].asString()) ||
         !Id(request["expected_announcement_version"], expected)))
        return Error_Json;
    if (action == ID_DECIDE_GROUP_INVITE_REQ &&
        (!Id(request["invitation_id"], decision) || !request["accept"].isBool()))
        return Error_Json;
    if (action == ID_DECIDE_GROUP_APPLICATION_REQ &&
        (!Id(request["application_id"], decision) || !request["approve"].isBool()))
        return Error_Json;
    auto con = pool_->GetConnection();
    if (!con)
        return ServerBusy;
    Defer release([&]() { pool_->ReturnConnection(std::move(con)); });
    try
    {
        // 先查重只为定位创建操作原群；任何返回成功都必须经过群锁下当前权限。
        auto replay = con->sql("SELECT action,input_json,result_json FROM group_operation WHERE "
                               "user_id=? AND operation_id=?")
                          .bind(uid)
                          .bind(operation)
                          .execute()
                          .fetchOne();
        if (replay)
        {
            if (replay[0].get<int>() != action || replay[1].get<std::string>() != identity)
                return Error_Json;
            Json::CharReaderBuilder b;
            std::string err;
            std::istringstream s(replay[2].get<std::string>());
            if (!Json::parseFromStream(b, s, &response, &err))
                return ServerBusy;
            if (action == ID_CREATE_GROUP_REQ)
                thread = response["thread_id"].asUInt64();
        }
        con->startTransaction();
        bool committed = false;
        Defer rollback([&]() {
            if (!committed)
                try
                {
                    con->rollback();
                }
                catch (...)
                {
                }
        });
        if (action == ID_CREATE_GROUP_REQ && !replay)
        {
            thread = con->sql("INSERT INTO chat_thread(type) VALUES('group')")
                         .execute()
                         .getAutoIncrementValue();
            con->sql("INSERT INTO group_chat(thread_id,name,owner_uid,active) VALUES(?,?,?,1)")
                .bind(thread)
                .bind(request["name"].asString())
                .bind(uid)
                .execute();
            con->sql("INSERT INTO "
                     "group_chat_member(thread_id,user_id,role,active,membership_version) "
                     "VALUES(?,?,2,1,1)")
                .bind(thread)
                .bind(uid)
                .execute();
        }
        GroupChatThread group;
        if (!group_db::Load(*con, thread, uid, group))
            return GroupUnavailable;
        // 群锁序列化所有成员修改，当前锁定读包括 tombstone 防止旧generation操作新身份。
        auto mr = con->sql("SELECT user_id,role,CAST(active AS UNSIGNED),membership_version FROM "
                           "group_chat_member WHERE thread_id=? ORDER BY user_id FOR UPDATE")
                      .bind(thread)
                      .execute();
        struct Member
        {
            int uid, role;
            bool active;
            std::uint64_t version;
        };
        std::vector<Member> members;
        for (auto r = mr.fetchOne(); r; r = mr.fetchOne())
            members.push_back({r[0].get<int>(), r[1].get<int>(), r[2].get<int>() == 1,
                               r[3].get<std::uint64_t>()});
        auto lookup = [&](int who) -> Member * {
            auto i = std::find_if(members.begin(), members.end(),
                                  [&](const Member &m) { return m.uid == who; });
            return i == members.end() ? nullptr : &*i;
        };
        // 同群并发 replay 在获取群锁后再次检查，不依赖事务开始时的快照。
        replay = con->sql("SELECT action,input_json,result_json FROM group_operation WHERE "
                          "user_id=? AND operation_id=? FOR UPDATE")
                     .bind(uid)
                     .bind(operation)
                     .execute()
                     .fetchOne();
        if (replay && (replay[0].get<int>() != action || replay[1].get<std::string>() != identity))
            return Error_Json;
        if (!group.active)
            return GroupUnavailable;
        const auto owner = lookup(group.ownerUid);
        int owners = 0;
        for (const auto &m : members)
            if (m.active && m.role == 2)
                ++owners;
        if (!owner || !owner->active || owner->role != 2 || owners != 1)
            return GroupUnavailable;
        auto self = lookup(uid);
        bool isMember = self && self->active;
        bool manager = isMember && (self->role == 1 || self->role == 2);
        bool publicAdmission = action == ID_JOIN_GROUP_REQ || action == ID_DECIDE_GROUP_INVITE_REQ;
        if (!isMember && !publicAdmission && !(replay && action == ID_LEAVE_GROUP_REQ))
            return GroupNotMember;
        bool ownerOnly = action == ID_SET_GROUP_ADMIN_REQ ||
                         action == ID_TRANSFER_GROUP_OWNER_REQ || action == ID_DISSOLVE_GROUP_REQ ||
                         action == ID_SET_GROUP_APPROVAL_REQ;
        if (ownerOnly && uid != group.ownerUid)
            return GroupPermissionDenied;
        if ((action == ID_DECIDE_GROUP_APPLICATION_REQ ||
             action == ID_UPDATE_GROUP_ANNOUNCEMENT_REQ || action == ID_UPDATE_GROUP_AVATAR_REQ) &&
            !manager)
            return GroupPermissionDenied;
        if (action == ID_KICK_GROUP_MEMBER_REQ && !manager)
            return GroupPermissionDenied;
        if (replay)
        {
            Json::CharReaderBuilder b;
            std::string err;
            std::istringstream s(replay[2].get<std::string>());
            if (!Json::parseFromStream(b, s, &response, &err))
                return ServerBusy;
            if (action == ID_CREATE_GROUP_REQ && response["thread_id"].asUInt64() != thread)
                return ServerBusy;
            response["replayed"] = true;
            response["group"] = BuildGroupJson(group, "mutation");
            if (group.member && (action == ID_CREATE_GROUP_REQ || action == ID_JOIN_GROUP_REQ))
                response["membership_granted"] = true;
            else
                response.removeMember("membership_granted");
            if (action == ID_LEAVE_GROUP_REQ && group.member)
                response.removeMember("reason");
            return Success;
        }
        bool changed = action == ID_CREATE_GROUP_REQ;
        int invalidUid = 0;
        std::string invalidReason;
        std::vector<std::uint64_t> admissionChanges;
        int grantedUid = action == ID_CREATE_GROUP_REQ ? uid : 0;
        auto addMember = [&](int who) {
            auto m = lookup(who);
            if (m && m->active)
                return;
            con->sql("INSERT INTO "
                     "group_chat_member(thread_id,user_id,role,active,membership_version) "
                     "VALUES(?,?,0,1,1) ON DUPLICATE KEY UPDATE "
                     "active=1,role=0,membership_version=membership_version+1,joined_at=CURRENT_"
                     "TIMESTAMP")
                .bind(thread)
                .bind(who)
                .execute();
            if (m)
            {
                m->active = true;
                m->role = 0;
                ++m->version;
            }
            else
                members.push_back({who, 0, true, 1});
            changed = true;
            grantedUid = who;
        };
        auto pending = [&](int who) {
            return con
                ->sql("SELECT id,CAST(kind AS CHAR),inviter_uid FROM group_admission WHERE "
                      "thread_id=? AND user_id=? AND status='pending' FOR UPDATE")
                .bind(thread)
                .bind(who)
                .execute()
                .fetchOne();
        };
        auto apply = [&](int who) {
            auto p = pending(who);
            std::uint64_t id = 0;
            if (p)
                id = p[0].get<std::uint64_t>();
            else
            {
                id = con->sql("INSERT INTO group_admission(thread_id,kind,user_id,explanation) "
                              "VALUES(?,'application',?,?)")
                         .bind(thread)
                         .bind(who)
                         .bind(explanation)
                         .execute()
                         .getAutoIncrementValue();
                admissionChanges.push_back(id);
            }
            response["admission"] = "pending";
            response["application"] = ReadAdmission(*con, id);
        };
        auto invalidateInvitations = [&](int who) {
            auto r = con->sql("SELECT id FROM group_admission WHERE thread_id=? AND inviter_uid=? "
                              "AND kind='invitation' AND status='pending' FOR UPDATE")
                         .bind(thread)
                         .bind(who)
                         .execute();
            for (auto a = r.fetchOne(); a; a = r.fetchOne())
                admissionChanges.push_back(a[0].get<std::uint64_t>());
            con->sql("UPDATE group_admission SET status='invalid' WHERE thread_id=? AND "
                     "inviter_uid=? AND kind='invitation' AND status='pending'")
                .bind(thread)
                .bind(who)
                .execute();
        };
        if (action == ID_JOIN_GROUP_REQ)
        {
            if (isMember)
                response["admission"] = "joined";
            else
            {
                auto p = pending(uid);
                const bool existingApplication = p && p[1].get<std::string>() == "application";
                if (p && p[1].get<std::string>() == "invitation")
                {
                    auto id = p[0].get<std::uint64_t>();
                    con->sql("UPDATE group_admission SET status='accepted' WHERE id=? AND "
                             "status='pending'")
                        .bind(id)
                        .execute();
                    admissionChanges.push_back(id);
                }
                if (group.approvalRequired || existingApplication)
                    apply(uid);
                else
                {
                    addMember(uid);
                    response["admission"] = "joined";
                }
            }
        }
        else if (action == ID_INVITE_GROUP_REQ)
        {
            auto t = lookup(target);
            if (t && t->active)
                return GroupPermissionDenied;
            auto account = con->sql("SELECT uid FROM user WHERE uid=? FOR SHARE")
                               .bind(target)
                               .execute()
                               .fetchOne();
            if (!account)
                return SearchUserNoExist;
            auto friendship =
                con->sql("SELECT 1 FROM friend f JOIN friend reciprocal ON "
                         "reciprocal.self_id=f.friend_id AND reciprocal.friend_id=f.self_id WHERE "
                         "f.self_id=? AND f.friend_id=? FOR SHARE")
                    .bind(uid)
                    .bind(target)
                    .execute()
                    .fetchOne();
            if (!friendship)
                return GroupPermissionDenied;
            auto p = pending(target);
            std::uint64_t id = 0;
            if (p)
                id = p[0].get<std::uint64_t>();
            else
            {
                id = con->sql("INSERT INTO group_admission(thread_id,kind,user_id,inviter_uid) "
                              "VALUES(?,'invitation',?,?)")
                         .bind(thread)
                         .bind(target)
                         .bind(uid)
                         .execute()
                         .getAutoIncrementValue();
                admissionChanges.push_back(id);
            }
            response["entry"] = ReadAdmission(*con, id);
        }
        else if (action == ID_DECIDE_GROUP_INVITE_REQ || action == ID_DECIDE_GROUP_APPLICATION_REQ)
        {
            auto a = con->sql("SELECT CAST(kind AS CHAR),user_id,inviter_uid,CAST(status AS CHAR) "
                              "FROM group_admission WHERE thread_id=? AND id=? FOR UPDATE")
                         .bind(thread)
                         .bind(decision)
                         .execute()
                         .fetchOne();
            if (!a)
                return Error_Json;
            const bool invite = action == ID_DECIDE_GROUP_INVITE_REQ;
            int who = a[1].get<int>();
            if (a[0].get<std::string>() != (invite ? "invitation" : "application") ||
                (invite && who != uid))
                return GroupPermissionDenied;
            const bool allowRequested =
                invite ? request["accept"].asBool() : request["approve"].asBool();
            const std::string requestedStatus =
                allowRequested ? (invite ? "accepted" : "approved") : "rejected";
            const std::string currentStatus = a[3].get<std::string>();
            if (currentStatus == "pending")
            {
                bool allow = allowRequested;
                std::string state = requestedStatus;
                if (invite)
                {
                    auto inviter = lookup(a[2].get<int>());
                    if (!inviter || !inviter->active)
                    {
                        state = "invalid";
                        allow = false;
                    }
                }
                con->sql("UPDATE group_admission SET status=? WHERE id=? AND status='pending'")
                    .bind(state)
                    .bind(decision)
                    .execute();
                admissionChanges.push_back(decision);
                if (allow)
                {
                    if (invite && group.approvalRequired)
                        apply(who);
                    else
                    {
                        addMember(who);
                        response["admission"] = "joined";
                    }
                }
            }
            else
            {
                response["already_processed"] = true;
                response["status_conflict"] = currentStatus != requestedStatus;
            }
            response["entry"] = ReadAdmission(*con, decision);
        }
        else if (action == ID_KICK_GROUP_MEMBER_REQ || action == ID_LEAVE_GROUP_REQ)
        {
            int who = action == ID_LEAVE_GROUP_REQ ? uid : target;
            auto m = lookup(who);
            if (who == group.ownerUid || !m || !m->active)
                return GroupPermissionDenied;
            if (action == ID_KICK_GROUP_MEMBER_REQ &&
                (!manager || (uid != group.ownerUid && m->role != 0)))
                return GroupPermissionDenied;
            if (m->version != expected)
                return GroupPermissionDenied;
            con->sql("UPDATE group_chat_member SET "
                     "active=0,role=0,membership_version=membership_version+1 WHERE thread_id=? "
                     "AND user_id=? AND active=1 AND membership_version=?")
                .bind(thread)
                .bind(who)
                .bind(expected)
                .execute();
            m->active = false;
            m->role = 0;
            ++m->version;
            changed = true;
            invalidUid = who;
            invalidReason = action == ID_LEAVE_GROUP_REQ ? "left" : "kicked";
            invalidateInvitations(who);
        }
        else if (action == ID_SET_GROUP_ADMIN_REQ || action == ID_TRANSFER_GROUP_OWNER_REQ)
        {
            auto m = lookup(target);
            if (!m || !m->active || target == group.ownerUid || m->version != expected)
                return GroupPermissionDenied;
            if (action == ID_SET_GROUP_ADMIN_REQ)
            {
                int role = request["role"].asInt();
                if (m->role != role)
                {
                    con->sql("UPDATE group_chat_member SET "
                             "role=?,membership_version=membership_version+1 WHERE thread_id=? AND "
                             "user_id=?")
                        .bind(role)
                        .bind(thread)
                        .bind(target)
                        .execute();
                    m->role = role;
                    ++m->version;
                    changed = true;
                }
            }
            else
            {
                con->sql("UPDATE group_chat_member SET role=CASE WHEN user_id=? THEN 2 ELSE 0 "
                         "END,membership_version=membership_version+1 WHERE thread_id=? AND "
                         "user_id IN (?,?)")
                    .bind(target)
                    .bind(thread)
                    .bind(uid)
                    .bind(target)
                    .execute();
                self = lookup(uid);
                self->role = 0;
                ++self->version;
                m = lookup(target);
                m->role = 2;
                ++m->version;
                con->sql("UPDATE group_chat SET owner_uid=? WHERE thread_id=?")
                    .bind(target)
                    .bind(thread)
                    .execute();
                changed = true;
            }
            response["target_member_uid"] = target;
            response["target_membership_version"] = Json::UInt64(lookup(target)->version);
        }
        else if (action == ID_DISSOLVE_GROUP_REQ)
        {
            con->sql("UPDATE group_chat SET active=0 WHERE thread_id=?").bind(thread).execute();
            con->sql("UPDATE group_chat_member SET "
                     "active=0,role=0,membership_version=membership_version+1 WHERE thread_id=? "
                     "AND active=1")
                .bind(thread)
                .execute();
            for (auto &m : members)
                if (m.active)
                {
                    m.active = false;
                    m.role = 0;
                    ++m.version;
                }
            auto ar = con->sql("SELECT id FROM group_admission WHERE thread_id=? AND "
                               "status='pending' FOR UPDATE")
                          .bind(thread)
                          .execute();
            for (auto r = ar.fetchOne(); r; r = ar.fetchOne())
                admissionChanges.push_back(r[0].get<std::uint64_t>());
            con->sql("UPDATE group_admission SET status='invalid' WHERE thread_id=? AND "
                     "status='pending'")
                .bind(thread)
                .execute();
            changed = true;
            invalidReason = "dissolved";
        }
        else if (action == ID_SET_GROUP_APPROVAL_REQ)
        {
            if (group.approvalRequired != request["approval_required"].asBool())
            {
                con->sql("UPDATE group_chat SET approval_required=? WHERE thread_id=?")
                    .bind(request["approval_required"].asBool() ? 1 : 0)
                    .bind(thread)
                    .execute();
                changed = true;
            }
        }
        else if (action == ID_UPDATE_GROUP_ANNOUNCEMENT_REQ)
        {
            if (group.announcementVersion != expected)
                return GroupPermissionDenied;
            con->sql("UPDATE group_chat SET "
                     "announcement=?,announcement_version=announcement_version+1,announcement_"
                     "author_uid=?,announcement_updated_at=NOW(3) WHERE thread_id=?")
                .bind(request["announcement"].asString())
                .bind(uid)
                .bind(thread)
                .execute();
            changed = true;
        }
        else if (action == ID_UPDATE_GROUP_AVATAR_REQ)
        {
            if (!avatar.isObject() || !avatar["resource_id"].isString() ||
                avatar["resource_id"] != request["resource_id"] ||
                !avatar["file_size"].isUInt64() || !avatar["width"].isUInt() ||
                !avatar["height"].isUInt())
                return Error_Json;
            con->sql("UPDATE group_chat SET "
                     "avatar_resource_id=?,avatar_mime_type=?,avatar_size=?,avatar_width=?,avatar_"
                     "height=?,avatar_version=avatar_version+1 WHERE thread_id=?")
                .bind(avatar["resource_id"].asString())
                .bind(avatar["mime_type"].asString())
                .bind(avatar["file_size"].asUInt64())
                .bind(avatar["width"].asUInt())
                .bind(avatar["height"].asUInt())
                .bind(thread)
                .execute();
            changed = true;
        }
        else if (action != ID_CREATE_GROUP_REQ)
            return Error_Json;
        if (changed && action != ID_CREATE_GROUP_REQ)
            con->sql("UPDATE group_chat SET version=version+1 WHERE thread_id=?")
                .bind(thread)
                .execute();
        group_db::Load(*con, thread, uid, group);
        response["error"] = Success;
        response["type"] = "group";
        response["thread_id"] = Json::UInt64(thread);
        response["operation_id"] = operation;
        response["group"] = BuildGroupJson(group, "mutation");
        if (grantedUid == uid || (action == ID_JOIN_GROUP_REQ && group.member))
            response["membership_granted"] = true;
        if (invalidUid)
            response["target_member_uid"] = invalidUid;
        if (!invalidReason.empty())
            response["reason"] = invalidReason;
        // 全体成员已在群锁内当前读并同步到members，构造每个收件人自己的证明。
        // 普通元数据证明只刷新现有身份，唯有实际新增成员的grant能激活客户端墓碑。
        auto emit = [&](int who, const std::string &type, const Json::Value &entry,
                        const std::string &reason) {
            const auto recipient = lookup(who);
            const bool activeRecipient = group.active && recipient && recipient->active;
            const auto recipientVersion = recipient ? recipient->version : 0;
            Json::Value p;
            p["error"] = Success;
            p["event_type"] = type;
            p["target_uid"] = who;
            p["target_member_uid"] = who;
            p["thread_id"] = Json::UInt64(thread);
            p["operation_id"] = operation;
            p["version"] = Json::UInt64(group.version);
            p["membership_version"] = Json::UInt64(recipientVersion);
            const bool proof = activeRecipient && (type == "metadata" || type == "members");
            p["member"] = proof;
            p["membership_proof"] = proof;
            p["source"] = "event";
            if (type == "members" && who == grantedUid)
                p["membership_granted"] = true;
            if (!reason.empty())
            {
                p["reason"] = reason;
                p["active"] = false;
            }
            if (type == "metadata" || type == "members" || type == "invalidated")
            {
                p["group"] = BuildGroupJson(group, "event");
                p["group"]["member"] = activeRecipient;
                p["group"]["membership_proof"] = activeRecipient;
                p["group"]["role"] = activeRecipient ? recipient->role : -1;
                p["group"]["membership_version"] = Json::UInt64(recipientVersion);
            }
            if (!entry.isNull())
                p["entry"] = entry;
            events.push_back({who, type, std::move(p)});
        };
        if (changed)
            for (const auto &m : members)
            {
                if (m.active)
                    emit(m.uid,
                         (action == ID_UPDATE_GROUP_ANNOUNCEMENT_REQ ||
                          action == ID_UPDATE_GROUP_AVATAR_REQ ||
                          action == ID_SET_GROUP_APPROVAL_REQ)
                             ? "metadata"
                             : "members",
                         Json::Value(), "");
                else if ((invalidUid && m.uid == invalidUid) || invalidReason == "dissolved")
                    emit(m.uid, "invalidated", Json::Value(), invalidReason);
            }
        for (auto id : admissionChanges)
        {
            auto entry = ReadAdmission(*con, id);
            bool isPending = entry["status"].asString() == "pending";
            emit(entry["target_uid"].asInt(), isPending ? "inbox" : "result", entry, "");
            if (entry["inviter_uid"].asInt() > 0 &&
                entry["inviter_uid"].asInt() != entry["target_uid"].asInt())
                emit(entry["inviter_uid"].asInt(), "result", entry, "");
            if (entry["kind"].asString() == "application")
                for (const auto &m : members)
                    if (m.active && m.role >= 1)
                        emit(m.uid, isPending ? "inbox" : "result", entry, "");
        }
        Json::Value finalResponse = response;
        finalResponse["uid"] = uid;
        if (request["request_id"].isString())
            finalResponse["request_id"] = request["request_id"];
        if (Compact(finalResponse).size() > 4096)
            return MessageTooLarge;
        for (const auto &e : events)
            if (Compact(e.payload).size() > 4096)
                return MessageTooLarge;
        con->sql("INSERT INTO group_operation(user_id,operation_id,action,input_json,result_json) "
                 "VALUES(?,?,?,?,?)")
            .bind(uid)
            .bind(operation)
            .bind(action)
            .bind(identity)
            .bind(Compact(response))
            .execute();
        con->commit();
        committed = true;
        return Success;
    }
    catch (const std::exception &)
    {
        events.clear();
        response = Json::Value();
        return ServerBusy;
    }
}

int MysqlMgr::LoadGroupRequests(int uid, const Json::Value &request, Json::Value &response)
{
    response = Json::Value(Json::objectValue);
    response["uid"] = uid;
    response["type"] = "group";
    if (request["request_id"].isString())
        response["request_id"] = request["request_id"];
    std::uint64_t after = 0, thread = 0;
    if (uid <= 0 || !request["scope"].isString() || !Id(request["after_id"], after))
        return Error_Json;
    const std::string scope = request["scope"].asString();
    int limit = request.get("page_size", 50).asInt();
    if (limit < 1 || limit > 100)
        return Error_Json;
    if (scope != "inbox" && scope != "results" && scope != "approvals")
        return Error_Json;
    if (scope == "approvals" && (!Id(request["thread_id"], thread) || !thread))
        return Error_Json;
    auto con = pool_->GetConnection();
    if (!con)
        return ServerBusy;
    Defer release([&]() { pool_->ReturnConnection(std::move(con)); });
    try
    {
        con->startTransaction();
        bool committed = false;
        Defer rollback([&]() {
            if (!committed)
                try
                {
                    con->rollback();
                }
                catch (...)
                {
                }
        });
        if (scope == "approvals")
        {
            GroupChatThread g;
            if (!group_db::Load(*con, thread, uid, g) || !g.active)
                return GroupUnavailable;
            if (!g.member)
                return GroupNotMember;
            if (g.role < 1)
                return GroupPermissionDenied;
            response["group"] = BuildGroupJson(g, "snapshot");
        }
        const std::string where =
            scope == "approvals" ? "a.thread_id=? AND a.kind='application' AND a.status='pending'"
            : scope == "inbox"   ? "a.user_id=? AND a.status='pending'"
                                 : "(a.user_id=? OR a.inviter_uid=?) AND a.status<>'pending'";
        auto statement =
            con->sql(std::string("SELECT ") + AdmissionColumns +
                     " FROM group_admission a JOIN group_chat g ON g.thread_id=a.thread_id WHERE " +
                     where + " AND a.id>? ORDER BY a.id LIMIT ?");
        if (scope == "approvals")
            statement.bind(thread);
        else
        {
            statement.bind(uid);
            if (scope == "results")
                statement.bind(uid);
        }
        auto result = statement.bind(after).bind(limit + 1).execute();
        response["entries"] = Json::Value(Json::arrayValue);
        response["load_more"] = false;
        response["next_id"] = Json::UInt64(after);
        response["scope"] = scope;
        response["error"] = Success;
        for (auto row = result.fetchOne(); row; row = result.fetchOne())
        {
            if (response["entries"].size() >= static_cast<unsigned>(limit))
            {
                response["load_more"] = true;
                break;
            }
            Json::Value candidate = response;
            candidate["entries"].append(Admission(row));
            candidate["next_id"] = Json::UInt64(row[0].get<std::uint64_t>());
            candidate["load_more"] = false; // 以较大的最终false字段计算完整包。
            if (Compact(candidate).size() > 4096)
            {
                if (response["entries"].empty())
                    return MessageTooLarge;
                response["load_more"] = true;
                break;
            }
            response = std::move(candidate);
            response["load_more"] = false;
        }
        con->commit();
        committed = true;
        return Success;
    }
    catch (const std::exception &)
    {
        return ServerBusy;
    }
}
