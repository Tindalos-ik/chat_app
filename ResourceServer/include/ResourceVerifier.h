#pragma once

#include "const.h"
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

// ResourceServer 内部使用的、已完成图片的可信描述。该结构绝不接收 ChatServer 或
// 客户端给出的元数据，所有字段均从 meta.json 和已发布文件的内容重新确认。
struct VerifiedImageResource {
    std::string resourceId;
    std::filesystem::path finalPath;
    std::string name;
    std::string mimeType;
    std::uint64_t fileSize = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// 上传、发布、下载和 gRPC 核验共用同一把锁。调用 Locked 版本前必须持有此锁，
// 防止核验时读取到刚被完成重命名替换的文件。
std::mutex& GetResourceFileMutex();

// 只核验已完成的任务，并通过文件魔数解析图片格式和尺寸。返回值使用
// ResourceServer 的 ErrorCodes；调用方必须在返回 Success 后才可发布 resource_id。
ErrorCodes ResolveCompletedImageResourceLocked(const std::string& resourceId,
                                               VerifiedImageResource& resource);
