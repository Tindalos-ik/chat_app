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

// 核验已完成图片，并通过文件内容解析受支持的图片格式与尺寸。
// 参数：resourceId 为不透明资源任务 ID；resource 为成功时写入的可信元数据，失败时清空。
// 返回：ResourceServer ErrorCodes；Success 表示图片元数据已确认。
ErrorCodes ResolveCompletedImageResourceLocked(const std::string& resourceId,
                                               VerifiedImageResource& resource);

// 核验已完成普通文件，不对文件内容施加图片魔数或格式白名单。
// 参数：resourceId 为不透明资源任务 ID；resource 为成功时写入可信元数据，失败时清空。
// 返回：Error_Json 表示 ID 不安全，ResourceNotFound 表示任务/文件/元数据无效，
// ResourceNotCompleted 表示任务尚未完成，Success 表示最终文件大小与 meta.json 一致。
ErrorCodes ResolveCompletedFileResourceLocked(const std::string& resourceId,
                                              VerifiedFileResource& resource);
