// 本文件实现资源访问授权器，负责判断用户是否有权限下载私有资源。
// 主要流程：校验参数 -> 通过 gRPC 向 StatusServer 验证登录令牌 -> 查询 MySQL 确认资源归属。

#include "ResourceAccessAuthorizer.h"

#include "ConfigMgr.h"
#include "message.grpc.pb.h" 
#include <chrono>
#include <grpcpp/grpcpp.h> 
#include <mysqlx/xdevapi.h> 
#include <stdexcept> 

namespace { // 匿名命名空间：其中定义的内容只在当前 .cpp 文件内可见，避免与其他文件的同名符号冲突。
// 检查某个用户是否有权访问指定私聊线程中的图片或普通文件资源。
// 参数说明：
//   mysql：MySQL 配置节，包含 host、user、passwd、schema、port 等字段。
//   uid：请求下载的用户 ID。
//   threadId：私聊线程 ID，用来限定资源所属的聊天。
//   resourceId：请求下载的资源 ID。
// 返回值：
//   true 表示该用户在该线程中有权访问该图片资源；
//   false 表示没有找到匹配记录，无权访问。
// 异常：
//   如果 MySQL 配置不完整，或数据库操作失败，会抛出异常，由上层统一处理。
bool HasPrivateResourceAccess(const SectionInfo& mysql, int uid, std::uint64_t threadId,
                              const std::string& resourceId) {
    const std::string host = mysql["host"];
    const std::string user = mysql["user"];
    const std::string password = mysql["passwd"];
    const std::string schema = mysql["schema"];
    const std::string port = mysql["port"];
    if (host.empty() || user.empty() || password.empty() || schema.empty() || port.empty()) {
        throw std::runtime_error("ResourceServer Mysql configuration is incomplete");
    }

    // 创建 MySQL X DevAPI 的会话对象，连接指定的 MySQL 数据库。
    mysqlx::Session session(host, std::stoi(port), user, password, schema);
    // resource_id 也必须属于请求的 thread，避免同一私聊成员拿着自己的有效 token
    // 下载另一段私聊、或尚未写入 chat_message 的上传资源。
    // 下面的 SQL 查询用于确认：
    //   1. private_chat 表中存在该 thread_id 的私聊记录；
    //   2. chat_message 表中存在同一 thread_id 的消息；
    //   3. 该消息的 resource_id 等于请求的 resourceId；
    //   4. 该消息类型是 image 或 file；
    //   5. 当前用户 uid 是该私聊的 user1_id 或 user2_id 之一。
    // LIMIT 1 表示只关心是否存在至少一条匹配记录。
    // 单条关联查询同时证明资源属于指定 thread、消息类型允许下载且用户是该私聊成员。
    // 资源 ID 必须和 thread_id 一起命中，避免成员借有效凭证读取其他会话的资源。
    const std::string sql =
        "SELECT 1 FROM private_chat AS pc "
        "INNER JOIN chat_message AS cm ON cm.thread_id = pc.thread_id "
        "WHERE pc.thread_id = ? AND cm.resource_id = ? AND cm.message_type IN ('image', 'file') "
        "AND (pc.user1_id = ? OR pc.user2_id = ?) LIMIT 1";
    // 执行 SQL，并按顺序绑定占位符 ? 的值
    // 使用参数绑定而不是拼接字符串，可以避免 SQL 注入。
    auto result = session.sql(sql)
                      .bind(threadId)
                      .bind(resourceId)
                      .bind(uid)
                      .bind(uid)
                      .execute();
    // 查询有 LIMIT 1；count() 直接返回尚未读取的结果行数。避免对
    // mysqlcppconnx 的 fetchAll() 包装类型做范围遍历，后者会让部分 VS IntelliSense 版本在库模板内部误报 operator[] 不匹配。
    // 如果结果行数不为 0，说明找到了匹配记录，用户有权限；否则无权访问。
    return result.count() != 0;
}

} // namespace

// 获取 ResourceAccessAuthorizer 的全局单例实例。
// 使用函数内静态局部变量实现单例，C++11 起保证这种初始化是线程安全的。
ResourceAccessAuthorizer& ResourceAccessAuthorizer::Instance() {
    // 静态局部变量 authorizer 只会在第一次调用 Instance() 时构造，之后一直复用同一个对象。
    static ResourceAccessAuthorizer authorizer;
    // 返回该单例对象的引用，调用方通过引用使用它。
    return authorizer;
}

// 对外主要入口：授权私有下载请求。
// 它会完成三步：
//   1. 校验请求参数是否合法；
//   2. 通过 gRPC 调用 StatusServer 的 Login 接口验证 uid 和 token；
//   3. 查询 MySQL 确认该用户是否有权访问指定私聊图片资源。
// 参数说明：
//   uid：发起下载请求的用户 ID。
//   token：用户登录令牌，用于身份验证。
//   threadId：私聊线程 ID。
//   resourceId：要下载的资源 ID。
// 返回值说明：
//   ResourceAccessResult::Authorized 表示允许下载；
//   ResourceAccessResult::Denied 表示明确拒绝；
//   ResourceAccessResult::Unavailable 表示依赖服务暂时不可用，调用方可稍后重试。
ResourceAccessResult ResourceAccessAuthorizer::Authenticate(int uid, const std::string& token) const {
    if (uid <= 0 || token.empty() || token.size() > 512) {
        return ResourceAccessResult::Denied;
    }
    auto status = ConfigMgr::Inst()["StatusServer"];
    const std::string host = status["host"], port = status["port"];
    if (host.empty() || port.empty()) {
        return ResourceAccessResult::Unavailable;
    }
    auto channel = grpc::CreateChannel(host + ":" + port, grpc::InsecureChannelCredentials());
    auto stub = message::StatusService::NewStub(channel);
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    message::LoginReq request;
    message::LoginRsp response;
    request.set_uid(uid);
    request.set_token(token);
    if (!stub->Login(&context, request, &response).ok()) {
        return ResourceAccessResult::Unavailable;
    }
    return response.error() == 0 && response.uid() == uid && response.token() == token ?
        ResourceAccessResult::Authorized : ResourceAccessResult::Denied;
}

ResourceAccessResult ResourceAccessAuthorizer::AuthorizePrivateDownload(
    int uid, const std::string& token, std::uint64_t threadId, const std::string& resourceId) const {
    if (!threadId || resourceId.empty()) {
        return ResourceAccessResult::Denied;
    }
    const auto identity = Authenticate(uid, token);
    if (identity != ResourceAccessResult::Authorized) {
        return identity;
    }
    try {
        return HasPrivateResourceAccess(ConfigMgr::Inst()["Mysql"], uid, threadId, resourceId) ?
            ResourceAccessResult::Authorized : ResourceAccessResult::Denied;
    } catch (...) {
        return ResourceAccessResult::Unavailable;
    }
}

ResourceAccessResult ResourceAccessAuthorizer::AcquireGroupAccess(
    int uid, const std::string& token, std::uint64_t threadId, const std::string& resourceId,
    bool published, std::unique_ptr<GroupResourceLease>& lease) const {
    lease.reset();
    if (!threadId || resourceId.empty()) {
        return ResourceAccessResult::Denied;
    }
    const auto identity = Authenticate(uid, token);
    if (identity != ResourceAccessResult::Authorized) {
        return identity;
    }
    try {
        auto cfg = ConfigMgr::Inst()["Mysql"];
        auto db = std::make_unique<mysqlx::Session>(cfg["host"], std::stoi(cfg["port"]),
            cfg["user"], cfg["passwd"], cfg["schema"]);
        // 先锁群再锁成员，与管理事务一致；持锁到响应入队，退群/撤权不能穿透分块。
        db->startTransaction();
        auto group = db->sql("SELECT CAST(active AS UNSIGNED),avatar_resource_id,owner_uid "
            "FROM group_chat WHERE thread_id=? FOR UPDATE").bind(threadId).execute().fetchOne();
        if (!group || group[0].get<int>() != 1) {
            return ResourceAccessResult::Denied;
        }
        auto member = db->sql("SELECT role FROM group_chat_member WHERE thread_id=? "
            "AND user_id=? AND active=1 FOR UPDATE").bind(threadId).bind(uid).execute().fetchOne();
        const bool managerMissing = !published && member && member[0].get<int>() < 1;
        const bool publicationChanged = published && group[1].get<std::string>() != resourceId;
        if (!member || managerMissing || publicationChanged) {
            return ResourceAccessResult::Denied;
        }
        auto owner = db->sql("SELECT role FROM group_chat_member WHERE thread_id=? "
            "AND user_id=? AND active=1 FOR UPDATE").bind(threadId).bind(group[2].get<int>())
            .execute().fetchOne();
        if (!owner || owner[0].get<int>() != 2) {
            return ResourceAccessResult::Denied;
        }
        lease = std::make_unique<GroupResourceLease>(std::move(db));
        return ResourceAccessResult::Authorized;
    } catch (...) {
        return ResourceAccessResult::Unavailable;
    }
}
