#pragma once

// 文件作用：声明 1007 私聊资源下载授权器，组合 StatusServer 登录校验和 MySQL 消息归属校验。
#include <cstdint>
#include <string>

// 1007 不能只凭 resource_id 读取文件：它必须同时证明当前登录身份，并且该身份
// 仍属于包含此资源消息的私聊。鉴权失败和依赖不可用必须分开，前者不应通过重试绕过。
enum class ResourceAccessResult {
    Authorized,
    Denied,
    Unavailable,
};

class ResourceAccessAuthorizer {
public:
    // 获取进程内唯一授权器实例。
    static ResourceAccessAuthorizer& Instance();

    // 授权一次私聊资源下载。每个请求都会重新校验，不能复用连接或上次请求的授权结果。
    // uid：当前登录用户 ID；token：该用户当前登录凭证；threadId：私聊会话 ID；
    // resourceId：请求下载的资源 ID，必须由该 thread 的 image/file 消息引用。
    // 返回 Authorized 表示可继续读取，Denied 表示凭证或归属不匹配，Unavailable 表示鉴权依赖故障。
    ResourceAccessResult AuthorizePrivateDownload(int uid, const std::string& token,
                                                   std::uint64_t threadId,
                                                   const std::string& resourceId) const;

private:
    ResourceAccessAuthorizer() = default;
};
