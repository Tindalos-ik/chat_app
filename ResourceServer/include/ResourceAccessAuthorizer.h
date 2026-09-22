#pragma once

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
    static ResourceAccessAuthorizer& Instance();

    ResourceAccessResult AuthorizePrivateDownload(int uid, const std::string& token,
                                                   std::uint64_t threadId,
                                                   const std::string& resourceId) const;

private:
    ResourceAccessAuthorizer() = default;
};
