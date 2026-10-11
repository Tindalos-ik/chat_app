#pragma once

// 文件作用：集中完成 ResourceServer 资源文件的可信核验，供 TCP 下载和 gRPC 元数据核验共用。
// 所有 Resolve...Locked 函数都要求调用者先持有 GetResourceFileMutex()，避免与上传发布并发。
#include "const.h"
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

// ResourceServer 内部使用的已完成图片可信描述。字段只由本服务从任务元数据和发布文件确认，
// 不接收 ChatServer 或客户端提交的名字、MIME、大小和尺寸。
struct VerifiedImageResource {
    std::string resourceId;
    std::filesystem::path finalPath;
    std::string name;
    std::string mimeType;
    std::uint64_t fileSize = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// 已完成普通文件的可信描述；普通文件不做内容魔数白名单判断。
// resourceId 是安全校验后的任务 ID；finalPath 仅供 ResourceServer 内部读取，不能回传调用方；
// name、mimeType、fileSize 是服务端确认后可供 RPC 返回的元数据。
struct VerifiedFileResource {
    std::string resourceId;
    std::filesystem::path finalPath;
    std::string name;
    std::string mimeType;
    std::uint64_t fileSize = 0;
};

// 返回上传、发布、下载和 gRPC 核验共用的文件锁。
// 返回：全局互斥锁引用；调用者使用 std::lock_guard 持锁后再调用下方 Locked 函数。
std::mutex& GetResourceFileMutex();

// 旧私聊图片核验只确认完整任务、文件头格式与尺寸，不承诺完整像素解码；行为保留。
// 参数resourceId为不透明任务ID，resource成功输出可信头部元数据、失败清空。
// 返回ResourceServer错误码；群头像必须调用下方完整解码/绑定接口，不借用此旧接口。
ErrorCodes ResolveCompletedImageResourceLocked(const std::string& resourceId,
                                               VerifiedImageResource& resource);

// 核验已完成普通文件，不对文件内容施加图片魔数或格式白名单。
// 参数：resourceId 为不透明资源任务 ID；resource 为成功时写入可信元数据，失败时清空。
// 返回：Error_Json 表示 ID 不安全，ResourceNotFound 表示任务/文件/元数据无效，
// ResourceNotCompleted 表示任务尚未完成，Success 表示最终文件大小与 meta.json 一致。
// allowGroupAvatar只由服务端授权流程选择：私聊调用默认false，拒绝受保护群任务。
// 群下载须先持有效成员/恰为当前发布资源租约；群核验须持经理租约并另验任务绑定，绝非客户端开关。
ErrorCodes ResolveCompletedFileResourceLocked(const std::string& resourceId,
                                              VerifiedFileResource& resource, bool allowGroupAvatar = false);

// DecodeGroupAvatarFile参数path为服务内部完整文件（含发布前已写完的part），不是客户端路径。
// resource成功输出完整解码的可信size/MIME/尺寸，失败清空；调用者另行填资源身份。
// 返回Success、超2MiB的UploadFileTooLarge，或非PNG/JPEG、缺失/截断/解码/尺寸失败的ResourceNotImage。
ErrorCodes DecodeGroupAvatarFile(const std::filesystem::path& path, VerifiedImageResource& resource);
// ResolveGroupAvatarLocked参数resourceId为group_avatar不透明任务ID（客户端UUID，非路径/内容MD5）。
// uid为已认证上传者，threadId为目标群；核对完成状态及存储owner/purpose/thread，再完整解码。
// 前提：持文件锁及当前群管理授权租约；resource成功为可信身份+图片信息，任意失败清空。
// 返回任务ID/不存在/未完成错误、绑定不符ResourceAccessDenied，或上述解码错误；RPC/TCP不得回传finalPath。
ErrorCodes ResolveGroupAvatarLocked(const std::string& resourceId, int uid,
    std::uint64_t threadId, VerifiedImageResource& resource);
