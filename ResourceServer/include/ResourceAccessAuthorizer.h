#pragma once

// 文件作用：组合Status当前身份与MySQL权限，分别授权私聊资源和受保护群头像的每次请求。
#include <cstdint>
#include <memory>
#include <mysqlx/xdevapi.h>
#include <string>

// 群资源只读授权租约：持群→成员行锁至本次TCP回复入队；撤权/头像替换不能越过该授权点。
class GroupResourceLease
{
  public:
    // 参数db：已开始只读授权事务的独占Session；转移所有权及其已持行锁，不共享连接。
    explicit GroupResourceLease(std::unique_ptr<mysqlx::Session> db) : db_(std::move(db))
    {
    }
    // 销毁释放锁：提交只读事务；提交失败尝试回滚，清理异常不得逃出析构函数。
    ~GroupResourceLease()
    {
        if (!db_)
            return;
        try
        {
            db_->commit();
        }
        catch (...)
        {
            try
            {
                db_->rollback();
            }
            catch (...)
            {
                // 连接析构仍负责释放资源；不能因清理失败终止请求线程。
            }
        }
    }

  private:
    std::unique_ptr<mysqlx::Session> db_;
};

// 1007 不能只凭 resource_id 读取文件：它必须同时证明当前登录身份，并且该身份
// 仍属于包含此资源消息的私聊。鉴权失败和依赖不可用必须分开，前者不应通过重试绕过。
enum class ResourceAccessResult
{
    Authorized,
    Denied,
    Unavailable,
};

class ResourceAccessAuthorizer
{
  public:
    // 获取进程内唯一授权器实例。
    static ResourceAccessAuthorizer &Instance();

    // 授权一次私聊资源下载。每个请求都会重新校验，不能复用连接或上次请求的授权结果。
    // uid：当前登录用户 ID；token：该用户当前登录凭证；threadId：私聊会话 ID；
    // resourceId：请求下载的资源 ID，必须由该 thread 的 image/file 消息引用。
    // 返回 Authorized 表示可继续读取，Denied 表示凭证或归属不匹配，Unavailable 表示鉴权依赖故障。
    ResourceAccessResult AuthorizePrivateDownload(int uid, const std::string &token,
                                                  std::uint64_t threadId,
                                                  const std::string &resourceId) const;
    // Authenticate参数uid/token：请求者当前登录身份；每次向Status校验，不复用旧认证结果。
    // 返回Authorized/Denied；Status配置或RPC故障返回Unavailable，不以内部网络位置代替认证。
    ResourceAccessResult Authenticate(int uid, const std::string &token) const;
    // AcquireGroupAccess参数uid/token为当前身份，threadId为群号，resourceId为不透明资源ID。
    // published=false仅核对有效群与当前role1/2；任务owner/purpose/thread绑定仍须由调用者核验。
    // published=true核对有效成员及resourceId恰为当前已发布头像，每片都要重新调用。
    // lease为输出独占租约：Authorized时非空，调用者持至文件读取/TCP回复入队完成；拒绝时为空。
    // 按群→成员锁顺序当前读；配置/Status/MySQL故障返回Unavailable，权限不符返回Denied。
    ResourceAccessResult AcquireGroupAccess(int uid, const std::string &token,
                                            std::uint64_t threadId, const std::string &resourceId,
                                            bool published,
                                            std::unique_ptr<GroupResourceLease> &lease) const;

  private:
    ResourceAccessAuthorizer() = default;
};
