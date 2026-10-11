#include "ResourceVerifier.h"
#include <chrono>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <json.h>
#include <png.h>
#include <turbojpeg.h>
#include <vector>
#include <zlib.h>

namespace
{
std::vector<unsigned char> Png(unsigned width, unsigned height)
{
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = width;
    image.height = height;
    image.format = PNG_FORMAT_RGBA;
    std::vector<unsigned char> pixels(PNG_IMAGE_SIZE(image), 127);
    png_alloc_size_t length = 0;
    if (!png_image_write_to_memory(&image, nullptr, &length, 0, pixels.data(), 0, nullptr))
        throw std::runtime_error("PNG fixture encoding failed");
    std::vector<unsigned char> bytes(length);
    if (!png_image_write_to_memory(&image, bytes.data(), &length, 0, pixels.data(), 0, nullptr))
        throw std::runtime_error("PNG fixture encoding failed");
    bytes.resize(length);
    return bytes;
}
std::vector<unsigned char> Jpeg(int width, int height)
{
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 3);
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = static_cast<unsigned char>((i * 31) % 251);
    tjhandle encoder = tjInitCompress();
    if (!encoder)
        throw std::runtime_error("JPEG encoder unavailable");
    Defer destroy([&] { tjDestroy(encoder); });
    unsigned char *data = nullptr;
    unsigned long size = 0;
    Defer release([&] { tjFree(data); });
    if (tjCompress2(encoder, pixels.data(), width, 0, height, TJPF_RGB, &data, &size, TJSAMP_444,
                    90, 0))
        throw std::runtime_error("JPEG fixture encoding failed");
    return {data, data + size};
}
std::uint32_t Be32(const unsigned char *p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
           p[3];
}
void PutBe32(unsigned char *p, std::uint32_t value)
{
    for (int i = 3; i >= 0; --i)
    {
        p[i] = static_cast<unsigned char>(value);
        value >>= 8;
    }
}
class GroupAvatarDecoderTest : public ::testing::Test
{
  protected:
    std::filesystem::path directory;
    bool owned = false;
    void SetUp() override
    {
        directory = std::filesystem::temp_directory_path() /
                    ("chat_group_avatar_decoder_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        owned = std::filesystem::create_directory(directory);
        ASSERT_TRUE(owned);
    }
    void TearDown() override
    {
        if (owned)
        {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }
    }
    std::filesystem::path Save(const std::vector<unsigned char> &bytes)
    {
        const auto path = directory / "image.bin";
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        if (!output)
            throw std::runtime_error("Could not write isolated image fixture");
        return path;
    }
};
TEST_F(GroupAvatarDecoderTest, FullPngAndJpegDecodeReturnContentMetadata)
{
    VerifiedImageResource image;
    ASSERT_EQ(DecodeGroupAvatarFile(Save(Png(4, 3)), image), Success);
    EXPECT_EQ(image.width, 4u);
    EXPECT_EQ(image.height, 3u);
    EXPECT_EQ(image.mimeType, "image/png");
    ASSERT_EQ(DecodeGroupAvatarFile(Save(Jpeg(32, 24)), image), Success);
    EXPECT_EQ(image.width, 32u);
    EXPECT_EQ(image.height, 24u);
    EXPECT_EQ(image.mimeType, "image/jpeg");
}
TEST_F(GroupAvatarDecoderTest, PngCompleteContainerDoesNotReplacePixelDecode)
{
    auto bytes = Png(8, 8);
    size_t position = 8;
    bool corrupted = false;
    while (position + 12 <= bytes.size())
    {
        const auto size = Be32(bytes.data() + position);
        if (!std::memcmp(bytes.data() + position + 4, "IDAT", 4))
        {
            ASSERT_GT(size, 0u);
            bytes[position + 8] ^=
                0x80; // Destroy compressed pixels, retain correct chunk CRC/IEND.
            PutBe32(bytes.data() + position + 8 + size,
                    crc32(0, bytes.data() + position + 4, size + 4));
            corrupted = true;
            break;
        }
        position += size + 12;
    }
    ASSERT_TRUE(corrupted);
    VerifiedImageResource image;
    EXPECT_EQ(DecodeGroupAvatarFile(Save(bytes), image), ResourceNotImage);
}
TEST_F(GroupAvatarDecoderTest, TruncationBadCrcAndTrailingContainerBytesAreRejected)
{
    VerifiedImageResource image;
    image.resourceId = "previous-result"; image.mimeType = "image/jpeg";
    image.width = 999; image.fileSize = 123;
    auto bytes = Png(4, 4);
    bytes.pop_back();
    EXPECT_EQ(DecodeGroupAvatarFile(Save(bytes), image), ResourceNotImage);
    EXPECT_TRUE(image.resourceId.empty()); EXPECT_TRUE(image.mimeType.empty());
    EXPECT_EQ(image.width, 0u); EXPECT_EQ(image.fileSize, 0u);
    bytes = Png(4, 4);
    bytes[29] ^= 1;
    EXPECT_EQ(DecodeGroupAvatarFile(Save(bytes), image), ResourceNotImage);
    bytes = Png(4, 4);
    bytes.push_back(0);
    EXPECT_EQ(DecodeGroupAvatarFile(Save(bytes), image), ResourceNotImage);
    bytes = Jpeg(32, 32);
    bytes.pop_back();
    EXPECT_EQ(DecodeGroupAvatarFile(Save(bytes), image), ResourceNotImage);
    bytes = Jpeg(32, 32);
    ASSERT_GT(bytes.size(), 64u);
    bytes.resize(bytes.size() - 32);
    bytes.push_back(0xff);
    bytes.push_back(0xd9); // EOI alone cannot make a truncated scan complete.
    EXPECT_EQ(DecodeGroupAvatarFile(Save(bytes), image), ResourceNotImage);
}
TEST_F(GroupAvatarDecoderTest, DimensionsAndByteLimitAreCheckedBeforePixelAllocation)
{
    VerifiedImageResource image;
    EXPECT_EQ(DecodeGroupAvatarFile(Save(Png(1024, 1024)), image), Success);
    EXPECT_EQ(DecodeGroupAvatarFile(Save(Png(1025, 1)), image), ResourceNotImage);
    EXPECT_EQ(DecodeGroupAvatarFile(Save(Jpeg(1, 1025)), image), ResourceNotImage);
    EXPECT_EQ(
        DecodeGroupAvatarFile(Save(std::vector<unsigned char>(2 * 1024 * 1024 + 1, 0)), image),
        UploadFileTooLarge);
    EXPECT_EQ(DecodeGroupAvatarFile(Save({}), image), ResourceNotImage);
    EXPECT_EQ(DecodeGroupAvatarFile(directory / "absent.bin", image), ResourceNotImage);
}
TEST_F(GroupAvatarDecoderTest, ProtectedTaskCannotUsePrivateVerifierOrAnotherOwnerBinding)
{
    const auto original = std::filesystem::current_path();
    Defer restore([&] { std::filesystem::current_path(original); });
    std::filesystem::current_path(directory);
    std::ofstream("config.ini") << "[ResourceServer]\nfile_path = uploads\n";
    const std::string resourceId = "secure_decoder_task";
    const auto task = directory / "uploads" / resourceId;
    std::filesystem::create_directories(task);
    const auto bytes = Png(4, 3);
    std::ofstream image(task / "avatar.png", std::ios::binary);
    image.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    image.close();
    Json::Value meta;
    meta["completed"] = true;
    meta["name"] = "avatar.png";
    meta["total_size"] = Json::UInt64(bytes.size());
    meta["purpose"] = "group_avatar";
    meta["owner_uid"] = 11;
    meta["thread_id"] = Json::UInt64(1234);
    meta["width"] = 999999;
    meta["mime_type"] = "untrusted/client-value";
    std::ofstream(task / "meta.json") << Json::writeString(Json::StreamWriterBuilder(), meta);
    std::lock_guard<std::mutex> lock(GetResourceFileMutex());
    VerifiedFileResource file;
    VerifiedImageResource verified;
    EXPECT_EQ(ResolveCompletedFileResourceLocked(resourceId, file), ResourceAccessDenied);
    EXPECT_EQ(ResolveCompletedImageResourceLocked(resourceId, verified), ResourceAccessDenied);
    EXPECT_EQ(ResolveGroupAvatarLocked(resourceId, 12, 1234, verified), ResourceAccessDenied);
    EXPECT_EQ(ResolveGroupAvatarLocked(resourceId, 11, 1235, verified), ResourceAccessDenied);
    ASSERT_EQ(ResolveGroupAvatarLocked(resourceId, 11, 1234, verified), Success);
    EXPECT_EQ(verified.resourceId, resourceId); EXPECT_EQ(verified.name, "avatar.png");
    EXPECT_EQ(verified.finalPath, task / "avatar.png");
    EXPECT_EQ(verified.width, 4u);
    EXPECT_EQ(verified.height, 3u);
    EXPECT_EQ(verified.mimeType, "image/png");
}
} // namespace
