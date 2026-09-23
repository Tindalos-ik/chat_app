#include "ResourceVerifier.h"

#include "ConfigMgr.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <json.h>

namespace {

std::mutex g_resource_file_mutex;
constexpr std::size_t kImageHeaderLimit = 256 * 1024;

bool IsSafeUploadId(const std::string& uploadId) {
    if (uploadId.empty() || uploadId.size() > 160) {
        return false;
    }
    return std::all_of(uploadId.begin(), uploadId.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '-';
    });
}

bool IsNonNegativeInteger(const Json::Value& value) {
    return value.isUInt() || value.isUInt64() ||
        (value.isInt() && value.asInt() >= 0) ||
        (value.isInt64() && value.asInt64() >= 0);
}

bool ReadJsonFile(const std::filesystem::path& path, Json::Value& value) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    Json::CharReaderBuilder reader;
    std::string errors;
    return Json::parseFromStream(reader, input, &value, &errors);
}

std::uint32_t ReadBe32(const std::string& data, std::size_t offset) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset])) << 24) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 16) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 8) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 3]));
}

std::uint16_t ReadBe16(const std::string& data, std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<unsigned char>(data[offset]) << 8) |
        static_cast<unsigned char>(data[offset + 1]));
}

bool ValidDimensions(std::uint32_t width, std::uint32_t height) {
    // 防止被伪造的头部声明异常尺寸，导致客户端后续分配不合理的图像缓冲区。
    return width > 0 && height > 0 && width <= 100000 && height <= 100000;
}

std::string MimeTypeFromName(const std::string& name) {
    // MIME 由 ResourceServer 按发布文件名推导，不采用 RPC/客户端传入的 MIME。
    // 未列出的扩展名仍允许作为普通文件核验，使用通用二进制类型，不构成文件格式白名单。
    std::string ext = std::filesystem::u8path(name).extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (ext == ".txt" || ext == ".log" || ext == ".csv") return "text/plain";
    if (ext == ".pdf") return "application/pdf";
    if (ext == ".zip") return "application/zip";
    if (ext == ".json") return "application/json";
    if (ext == ".doc") return "application/msword";
    if (ext == ".docx") return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    if (ext == ".xls") return "application/vnd.ms-excel";
    if (ext == ".xlsx") return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    if (ext == ".ppt") return "application/vnd.ms-powerpoint";
    if (ext == ".pptx") return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".webp") return "image/webp";
    return "application/octet-stream";
}

bool DetectImageMetadata(const std::filesystem::path& filePath, std::string& mimeType,
                         std::uint32_t& width, std::uint32_t& height) {
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        return false;
    }
    std::string header(kImageHeaderLimit, '\0');
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    header.resize(static_cast<std::size_t>(input.gcount()));

    // PNG: 固定签名 + IHDR 中的大端宽高。
    static const std::array<unsigned char, 8> kPngSignature =
        {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    if (header.size() >= 24 && std::equal(kPngSignature.begin(), kPngSignature.end(),
        reinterpret_cast<const unsigned char*>(header.data())) && header.substr(12, 4) == "IHDR") {
        width = ReadBe32(header, 16);
        height = ReadBe32(header, 20);
        mimeType = "image/png";
        return ValidDimensions(width, height);
    }

    // GIF 宽高为小端 16 位。
    if (header.size() >= 10 && (header.compare(0, 6, "GIF87a") == 0 || header.compare(0, 6, "GIF89a") == 0)) {
        width = static_cast<unsigned char>(header[6]) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(header[7])) << 8);
        height = static_cast<unsigned char>(header[8]) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(header[9])) << 8);
        mimeType = "image/gif";
        return ValidDimensions(width, height);
    }

    // JPEG 的尺寸位于任一 SOF 段，而非固定偏移；仅接受确实含有效 SOF 的文件。
    if (header.size() >= 10 && static_cast<unsigned char>(header[0]) == 0xff &&
        static_cast<unsigned char>(header[1]) == 0xd8) {
        std::size_t pos = 2;
        while (pos + 4 <= header.size()) {
            if (static_cast<unsigned char>(header[pos++]) != 0xff) {
                return false;
            }
            while (pos < header.size() && static_cast<unsigned char>(header[pos]) == 0xff) {
                ++pos;
            }
            if (pos >= header.size()) {
                return false;
            }
            const unsigned char marker = static_cast<unsigned char>(header[pos++]);
            if (marker == 0xd8 || marker == 0xd9) {
                continue;
            }
            if (marker == 0xda) { // SOS 后是压缩数据，不应再从中猜测段。
                break;
            }
            if (pos + 2 > header.size()) {
                return false;
            }
            const std::uint16_t segmentLength = ReadBe16(header, pos);
            if (segmentLength < 2 || pos + segmentLength > header.size()) {
                return false;
            }
            const bool isSof = (marker >= 0xc0 && marker <= 0xc3) ||
                (marker >= 0xc5 && marker <= 0xc7) ||
                (marker >= 0xc9 && marker <= 0xcb) ||
                (marker >= 0xcd && marker <= 0xcf);
            if (isSof && segmentLength >= 8) {
                height = ReadBe16(header, pos + 3);
                width = ReadBe16(header, pos + 5);
                mimeType = "image/jpeg";
                return ValidDimensions(width, height);
            }
            pos += segmentLength;
        }
        return false;
    }

    // WebP 的三种常见有损/无损/扩展头格式。
    if (header.size() >= 30 && header.compare(0, 4, "RIFF") == 0 && header.compare(8, 4, "WEBP") == 0) {
        const std::string chunk = header.substr(12, 4);
        if (chunk == "VP8X" && header.size() >= 30) {
            width = 1 + static_cast<unsigned char>(header[24]) +
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[25])) << 8) +
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[26])) << 16);
            height = 1 + static_cast<unsigned char>(header[27]) +
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[28])) << 8) +
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[29])) << 16);
        } else if (chunk == "VP8L" && header.size() >= 25 && static_cast<unsigned char>(header[20]) == 0x2f) {
            const std::uint32_t bits = static_cast<unsigned char>(header[21]) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[22])) << 8) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[23])) << 16) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[24])) << 24);
            width = (bits & 0x3fff) + 1;
            height = ((bits >> 14) & 0x3fff) + 1;
        } else if (chunk == "VP8 " && header.size() >= 30 &&
                   static_cast<unsigned char>(header[23]) == 0x9d &&
                   static_cast<unsigned char>(header[24]) == 0x01 &&
                   static_cast<unsigned char>(header[25]) == 0x2a) {
            width = (static_cast<unsigned char>(header[26]) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[27])) << 8)) & 0x3fff;
            height = (static_cast<unsigned char>(header[28]) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[29])) << 8)) & 0x3fff;
        } else {
            return false;
        }
        mimeType = "image/webp";
        return ValidDimensions(width, height);
    }
    return false;
}

} // namespace

std::mutex& GetResourceFileMutex() {
    return g_resource_file_mutex;
}

ErrorCodes ResolveCompletedImageResourceLocked(const std::string& resourceId,
                                               VerifiedImageResource& resource) {
    resource = {};
    VerifiedFileResource file;
    // 先复用通用文件核验，确保图片与普通文件遵守相同的 ID、完成状态和实际大小检查。
    const ErrorCodes fileResult = ResolveCompletedFileResourceLocked(resourceId, file);
    if (fileResult != ErrorCodes::Success) return fileResult;
    resource.resourceId = resourceId;
    resource.finalPath = file.finalPath;
    resource.name = file.name;
    resource.fileSize = file.fileSize;
    if (!DetectImageMetadata(file.finalPath, resource.mimeType, resource.width, resource.height)) {
        resource = {};
        return ErrorCodes::ResourceNotImage;
    }
    return ErrorCodes::Success;
}

ErrorCodes ResolveCompletedFileResourceLocked(const std::string& resourceId,
                                              VerifiedFileResource& resource) {
    resource = {};
    // resource_id 会成为任务目录名，限制字符集以阻止路径分隔符和目录穿越。
    if (!IsSafeUploadId(resourceId)) return ErrorCodes::Error_Json;
    const std::filesystem::path taskDir = ConfigMgr::Inst().GetFilePath() / resourceId;
    Json::Value meta;
    // 只信任 ResourceServer 自己保存的任务元数据，并要求明确的 bool 完成标志。
    if (!ReadJsonFile(taskDir / "meta.json", meta)) return ErrorCodes::ResourceNotFound;
    if (!meta["completed"].isBool()) return ErrorCodes::ResourceNotFound;
    if (!meta["completed"].asBool()) return ErrorCodes::ResourceNotCompleted;
    const auto storedName = std::filesystem::u8path(meta["name"].asString()).filename();
    if (storedName.empty() || !IsNonNegativeInteger(meta["total_size"])) return ErrorCodes::ResourceNotFound;
    std::error_code ec;
    const std::filesystem::path finalPath = taskDir / storedName;
    // 只允许从任务目录内以元数据文件名定位普通文件，并重新读取磁盘大小和元数据对比。
    if (!std::filesystem::is_regular_file(finalPath, ec) || ec) return ErrorCodes::ResourceNotFound;
    const auto actualSize = std::filesystem::file_size(finalPath, ec);
    if (ec || actualSize != meta["total_size"].asUInt64()) return ErrorCodes::ResourceNotFound;
    resource.resourceId = resourceId;
    resource.finalPath = finalPath;
    resource.name = storedName.u8string();
    // 可信 MIME 与其他返回字段均由本服务计算；未知扩展名保持可下载并标记为通用二进制。
    resource.mimeType = MimeTypeFromName(resource.name);
    resource.fileSize = actualSize;
    return ErrorCodes::Success;
}
