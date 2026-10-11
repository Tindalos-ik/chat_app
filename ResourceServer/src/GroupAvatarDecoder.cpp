// libpng + libjpeg-turbo完整解码群头像，不复用旧私聊图片的头部嗅探。
#include "ResourceVerifier.h"
#include <cstring>
#include <fstream>
#include <json.h>
#include <memory>
#include <png.h>
#include <turbojpeg.h>
#include <vector>
#include <zlib.h>
namespace
{
std::uint32_t Be32(const unsigned char *p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
           p[3];
}
// libpng可能宽容截断；先限定每块边界/CRC及唯一终结位置，再要求完整像素解码。
bool CompletePng(const std::vector<unsigned char> &bytes)
{
    if (bytes.size() < 8 || png_sig_cmp(bytes.data(), 0, 8))
        return false;
    size_t pos = 8;
    bool header = false, data = false;
    while (pos + 12 <= bytes.size())
    {
        auto n = Be32(bytes.data() + pos);
        if (n > bytes.size() - pos - 12)
            return false;
        const unsigned char *type = bytes.data() + pos + 4;
        if (!header)
        {
            if (n != 13 || std::memcmp(type, "IHDR", 4))
                return false;
            header = true;
        }
        auto crc = crc32(0, type, n + 4);
        if (crc != Be32(type + 4 + n))
            return false;
        if (!std::memcmp(type, "IDAT", 4))
            data = true;
        pos += 12 + n;
        if (!std::memcmp(type, "IEND", 4))
            return n == 0 && data && pos == bytes.size();
    }
    return false;
}
} // namespace
ErrorCodes DecodeGroupAvatarFile(const std::filesystem::path &path, VerifiedImageResource &resource)
{
    // 输出仅在完整解码成功后成立；失败不得留下上一次核验的可信属性。
    resource = {};
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec || !size)
        return ResourceNotImage;
    if (size > 2 * 1024 * 1024)
        return UploadFileTooLarge;
    // 输入上限先于文件缓冲分配，尺寸上限先于像素分配，拒绝伪头部/半图而非仅嗅探魔数。
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    std::ifstream in(path, std::ios::binary);
    if (!in.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
        return ResourceNotImage;
    if (bytes.size() >= 8 && !png_sig_cmp(bytes.data(), 0, 8))
    {
        if (!CompletePng(bytes))
            return ResourceNotImage;
        png_image image{};
        image.version = PNG_IMAGE_VERSION;
        if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size()))
            return ResourceNotImage;
        Defer freeImage([&]() { png_image_free(&image); });
        if (!image.width || !image.height || image.width > 1024 || image.height > 1024)
            return ResourceNotImage;
        image.format = PNG_FORMAT_RGBA;
        std::vector<unsigned char> pixels(PNG_IMAGE_SIZE(image));
        if (!png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr) ||
            image.warning_or_error)
            return ResourceNotImage;
        resource.width = image.width;
        resource.height = image.height;
        resource.mimeType = "image/png";
    }
    else
    {
        if (bytes.size() < 4 || bytes[0] != 0xff || bytes[1] != 0xd8 ||
            bytes[bytes.size() - 2] != 0xff || bytes.back() != 0xd9)
            return ResourceNotImage;
        tjhandle decoder = tjInitDecompress();
        if (!decoder)
            return ResourceNotImage;
        Defer destroy([&]() { tjDestroy(decoder); });
        int width = 0, height = 0, subsampling = 0, colorspace = 0;
        if (tjDecompressHeader3(decoder, bytes.data(), static_cast<unsigned long>(bytes.size()),
                                &width, &height, &subsampling, &colorspace) ||
            width <= 0 || height <= 0 || width > 1024 || height > 1024)
            return ResourceNotImage;
        std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 3);
        // EOI存在不代表扫描完整；STOPONWARNING使缺扫描数据等宽容解码警告也失败。
        if (tjDecompress2(decoder, bytes.data(), static_cast<unsigned long>(bytes.size()),
                          pixels.data(), width, 0, height, TJPF_RGB, TJFLAG_STOPONWARNING))
            return ResourceNotImage;
        resource.width = width;
        resource.height = height;
        resource.mimeType = "image/jpeg";
    }
    resource.fileSize = size;
    return Success;
}
ErrorCodes ResolveGroupAvatarLocked(const std::string &resourceId, int uid, std::uint64_t threadId,
                                    VerifiedImageResource &resource)
{
    resource = {};
    VerifiedFileResource file;
    auto error = ResolveCompletedFileResourceLocked(resourceId, file, true);
    if (error != Success)
        return error;
    Json::Value meta;
    Json::CharReaderBuilder reader;
    std::string errors;
    std::ifstream input(file.finalPath.parent_path() / "meta.json", std::ios::binary);
    if (!Json::parseFromStream(reader, input, &meta, &errors) ||
        meta["purpose"].asString() != "group_avatar" || !meta["owner_uid"].isInt() ||
        meta["owner_uid"].asInt() != uid || !meta["thread_id"].isUInt64() ||
        meta["thread_id"].asUInt64() != threadId)
        return ResourceAccessDenied;
    error = DecodeGroupAvatarFile(file.finalPath, resource);
    if (error == Success) {
        resource.resourceId = resourceId;
        resource.name = std::move(file.name);
        resource.finalPath = std::move(file.finalPath);
    }
    return error;
}
