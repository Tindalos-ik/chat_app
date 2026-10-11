// 文件作用：集中群元数据列映射与锁定身份读取；普通行解析不授予成员权限。
#pragma once
#include "MysqlMgr.h"
namespace group_db
{
inline constexpr const char *Columns =
    "g.thread_id,g.name,g.owner_uid,CAST(g.active AS UNSIGNED),g.version,CAST(g.approval_required "
    "AS "
    "UNSIGNED),g.announcement,g.announcement_version,g.announcement_author_uid,CAST(COALESCE(UNIX_"
    "TIMESTAMP(g.announcement_updated_at)*1000,0) AS "
    "UNSIGNED),g.avatar_resource_id,g.avatar_mime_type,g.avatar_size,g.avatar_width,g.avatar_"
    "height,g.avatar_version";
// Text参数row/column：SQL结果行及STRING/BYTES列序号；返回原始UTF8正文。
// 前提：列采用X Protocol文本表示（含额外尾0x00）；不持锁、不校验权限，保留正文NUL。
inline std::string Text(const mysqlx::Row &row, unsigned column)
{
    const auto bytes = row.getBytes(column);
    if (!bytes.length())
        return {};
    return std::string(reinterpret_cast<const char *>(bytes.begin()), bytes.length() - 1);
}
// FromRow参数r：按Columns顺序查询的非NULL群行；返回群元数据，不填成员身份。
// 本函数只解析、不加锁；snapshot调用者必须自行绑定UID并按成员版本/完整轮应用证明。
inline GroupChatThread FromRow(const mysqlx::Row &r)
{
    GroupChatThread g;
    g.threadId = r[0].get<std::uint64_t>();
    g.name = Text(r, 1);
    g.ownerUid = r[2].get<int>();
    g.active = r[3].get<int>() == 1;
    g.version = r[4].get<std::uint64_t>();
    g.approvalRequired = r[5].get<int>() == 1;
    g.announcement = Text(r, 6);
    g.announcementVersion = r[7].get<std::uint64_t>();
    g.announcementAuthorUid = r[8].get<int>();
    g.announcementUpdatedAtMs = r[9].get<std::uint64_t>();
    g.avatarResourceId = r[10].get<std::string>();
    g.avatarMimeType = r[11].get<std::string>();
    g.avatarSize = r[12].get<std::uint64_t>();
    g.avatarWidth = r[13].get<std::uint32_t>();
    g.avatarHeight = r[14].get<std::uint32_t>();
    g.avatarVersion = r[15].get<std::uint64_t>();
    return g;
}
// Load参数db：已开始的事务；thread：uint64群号；uid：认证会话用户；g：输出元数据/身份。
// 返回false仅表示群不存在，true不等于有效成员，必须检查g.active与g.member。
// 锁顺序群→成员；调用者持事务至授权操作结束。墓碑返回版本，但不返回旧role权限。
inline bool Load(mysqlx::Session &db, std::uint64_t thread, int uid, GroupChatThread &g)
{
    auto row = db.sql(std::string("SELECT ") + Columns +
                      " FROM group_chat g WHERE g.thread_id=? FOR UPDATE")
                   .bind(thread)
                   .execute()
                   .fetchOne();
    if (!row)
        return false;
    g = FromRow(row);
    auto member = db.sql("SELECT role,membership_version,CAST(active AS UNSIGNED) FROM "
                         "group_chat_member WHERE thread_id=? AND user_id=? FOR UPDATE")
                      .bind(thread)
                      .bind(uid)
                      .execute()
                      .fetchOne();
    if (member)
    {
        g.membershipVersion = member[1].get<std::uint64_t>();
        g.member = g.active && member[2].get<int>() == 1;
        if (g.member)
            g.role = member[0].get<int>();
    }
    return true;
}
} // namespace group_db
