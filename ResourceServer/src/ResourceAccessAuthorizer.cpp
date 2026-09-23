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
ResourceAccessResult ResourceAccessAuthorizer::AuthorizePrivateDownload(
    int uid, const std::string& token, std::uint64_t threadId,
    const std::string& resourceId) const {
    // 基础参数校验：
    //   uid 必须大于 0；
    //   token 不能为空；
    //   threadId 不能为 0；
    //   resourceId 不能为空。
    // 只要有一项不合法，就直接拒绝，避免无效请求继续访问网络或数据库。
    if (uid <= 0 || token.empty() || threadId == 0 || resourceId.empty()) {
        return ResourceAccessResult::Denied;
    }

    // 从全局配置管理器中读取 StatusServer 配置节。
    const auto status = ConfigMgr::Inst()["StatusServer"];
    // 读取状态服务器主机地址。
    const std::string host = status["host"];
    // 读取状态服务器端口号。
    const std::string port = status["port"];
    // 如果主机或端口为空，说明无法连接状态服务器进行身份验证。
    // 此时返回 Unavailable，表示服务暂不可用，而不是权限拒绝。
    if (host.empty() || port.empty()) {
        return ResourceAccessResult::Unavailable;
    }

    // 创建到状态服务器的 gRPC 通道。
    // InsecureChannelCredentials() 表示使用不加密的连接，通常用于内网或测试环境。
    auto channel = grpc::CreateChannel(host + ":" + port, grpc::InsecureChannelCredentials());
    // 基于通道创建 StatusService 的客户端存根（stub），后续通过它调用远程方法。
    auto stub = message::StatusService::NewStub(channel);
    // 创建 gRPC 客户端上下文，用于控制本次 RPC 调用的超时、元数据等。
    grpc::ClientContext context;
    // 设置本次 RPC 的截止时间：当前时间加 3 秒，防止因网络问题长时间阻塞。
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    // 构造登录请求 protobuf 消息。
    message::LoginReq request;
    // 构造登录响应 protobuf 消息，用于接收服务端返回结果。
    message::LoginRsp response;
    request.set_uid(uid);
    request.set_token(token);
    // 同步调用 StatusServer 的 Login 方法，验证 uid 与 token 是否匹配。
    // 调用结果保存在 rpcStatus 中，服务端响应写入 response。
    const grpc::Status rpcStatus = stub->Login(&context, request, &response);
    // 如果 gRPC 调用本身失败（例如网络错误、超时、服务未启动），返回 Unavailable。
    if (!rpcStatus.ok()) {
        return ResourceAccessResult::Unavailable;
    }
    // StatusServer 的业务拒绝表示 token/uid 不匹配；不要把它伪装成可重试故障。
    // response.error() != 0 表示业务层返回了错误码；
    // response.uid() != uid 表示返回的用户 ID 与请求不一致；
    // response.token() != token 表示返回的 token 与请求不一致。
    // 这些情况都说明身份验证失败，应明确拒绝。
    if (response.error() != 0 || response.uid() != uid || response.token() != token) {
        return ResourceAccessResult::Denied;
    }

    // 身份验证通过后，再查询 MySQL，确认该用户确实有权访问该私聊图片资源。
    try {
        // 调用匿名命名空间中的 HasPrivateResourceAccess：
        // ConfigMgr::Inst()["Mysql"] 用于读取 MySQL 配置节。
        return HasPrivateResourceAccess(ConfigMgr::Inst()["Mysql"], uid, threadId, resourceId)
            ? ResourceAccessResult::Authorized
            : ResourceAccessResult::Denied;
    } catch (const std::exception&) {
        return ResourceAccessResult::Unavailable;
    }
}
