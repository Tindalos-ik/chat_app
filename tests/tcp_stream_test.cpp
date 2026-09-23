#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "TcpFrameParser.h"

namespace {

std::string MakeFrame(std::uint16_t message_id, const std::string& body) {
    std::string frame;
    frame.reserve(HEAD_TOTAL_LEN + body.size());
    frame.push_back(static_cast<char>((message_id >> 8) & 0xff));
    frame.push_back(static_cast<char>(message_id & 0xff));
    frame.push_back(static_cast<char>((body.size() >> 8) & 0xff));
    frame.push_back(static_cast<char>(body.size() & 0xff));
    frame.append(body);
    return frame;
}

void ExpectFrame(const chat_protocol::TcpFrame& actual, std::uint16_t message_id,
                 const std::string& body) {
    EXPECT_EQ(actual.message_id, message_id);
    EXPECT_EQ(actual.body, body);
}

} // namespace

TEST(TcpStreamParserTest, BuffersPartialHeaderAndBodyUntilFrameIsComplete) {
    chat_protocol::TcpFrameParser parser;
    const std::string wire = MakeFrame(1017, "half packet body");
    std::vector<chat_protocol::TcpFrame> frames;

    auto result = parser.Feed(wire.data(), 2, frames);
    ASSERT_TRUE(result.IsOk());
    EXPECT_TRUE(frames.empty());

    result = parser.Feed(wire.data() + 2, 5, frames);
    ASSERT_TRUE(result.IsOk());
    EXPECT_TRUE(frames.empty());

    result = parser.Feed(wire.data() + 7, wire.size() - 7, frames);
    ASSERT_TRUE(result.IsOk());
    ASSERT_EQ(frames.size(), 1u);
    ExpectFrame(frames.front(), 1017, "half packet body");
}

TEST(TcpStreamParserTest, SplitsTwoCoalescedFramesFromOneRead) {
    chat_protocol::TcpFrameParser parser;
    const std::string first = MakeFrame(1017, "first");
    const std::string second = MakeFrame(1019, "second");
    const std::string coalesced = first + second;
    std::vector<chat_protocol::TcpFrame> frames;

    const auto result = parser.Feed(coalesced.data(), coalesced.size(), frames);

    ASSERT_TRUE(result.IsOk());
    ASSERT_EQ(frames.size(), 2u);
    ExpectFrame(frames[0], 1017, "first");
    ExpectFrame(frames[1], 1019, "second");
}

TEST(TcpStreamParserTest, PreservesOrderAcrossContinuousFramesAndArbitraryChunks) {
    chat_protocol::TcpFrameParser parser;
    const std::vector<std::pair<std::uint16_t, std::string>> expected = {
        {1005, "one"}, {1023, "two"}, {1033, "three"}, {1040, "four"},
    };
    std::string stream;
    for (const auto& item : expected) {
        stream += MakeFrame(item.first, item.second);
    }

    std::vector<chat_protocol::TcpFrame> actual;
    for (std::size_t offset = 0; offset < stream.size();) {
        const std::size_t chunk_size = (offset % 7) + 1;
        const std::size_t count = chunk_size < stream.size() - offset
            ? chunk_size
            : stream.size() - offset;
        std::vector<chat_protocol::TcpFrame> newly_completed;
        const auto result = parser.Feed(stream.data() + offset, count, newly_completed);
        ASSERT_TRUE(result.IsOk());
        actual.insert(actual.end(), newly_completed.begin(), newly_completed.end());
        offset += count;
    }

    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        ExpectFrame(actual[index], expected[index].first, expected[index].second);
    }
}

TEST(TcpStreamParserTest, RejectsZeroAndOversizedBodyLengths) {
    const std::uint16_t invalid_lengths[] = {
        0, static_cast<std::uint16_t>(MAX_LENGTH + 1),
    };
    for (const std::uint16_t body_length : invalid_lengths) {
        chat_protocol::TcpFrameParser parser;
        const std::string invalid_header = {
            static_cast<char>(0x03), static_cast<char>(0xf5),
            static_cast<char>((body_length >> 8) & 0xff),
            static_cast<char>(body_length & 0xff),
        };
        std::vector<chat_protocol::TcpFrame> frames;

        const auto result = parser.Feed(invalid_header.data(), invalid_header.size(), frames);

        EXPECT_EQ(result.status, chat_protocol::TcpFrameParseStatus::InvalidBodyLength);
        EXPECT_EQ(result.message_id, 1013);
        EXPECT_EQ(result.body_length, body_length);
        EXPECT_TRUE(frames.empty());
    }
}

TEST(TcpStreamParserTest, RejectsZeroAndOutOfRangeMessageIds) {
    const std::uint16_t invalid_ids[] = {
        0, static_cast<std::uint16_t>(MAX_LENGTH + 1),
    };
    for (const std::uint16_t message_id : invalid_ids) {
        chat_protocol::TcpFrameParser parser;
        const std::string invalid_header = {
            static_cast<char>((message_id >> 8) & 0xff),
            static_cast<char>(message_id & 0xff),
            static_cast<char>(0x00), static_cast<char>(0x01),
        };
        std::vector<chat_protocol::TcpFrame> frames;

        const auto result = parser.Feed(invalid_header.data(), invalid_header.size(), frames);

        EXPECT_EQ(result.status, chat_protocol::TcpFrameParseStatus::InvalidMessageId);
        EXPECT_EQ(result.message_id, message_id);
        EXPECT_EQ(result.body_length, 1);
        EXPECT_TRUE(frames.empty());
    }
}

TEST(TcpStreamParserTest, KeepsCompletedFramesBeforeAnInvalidFollowingHeader) {
    chat_protocol::TcpFrameParser parser;
    const std::string valid = MakeFrame(1017, "delivered first");
    const std::string invalid = {
        static_cast<char>(0x00), static_cast<char>(0x00),
        static_cast<char>(0x00), static_cast<char>(0x01),
    };
    const std::string stream = valid + invalid;
    std::vector<chat_protocol::TcpFrame> frames;

    const auto result = parser.Feed(stream.data(), stream.size(), frames);

    EXPECT_EQ(result.status, chat_protocol::TcpFrameParseStatus::InvalidMessageId);
    ASSERT_EQ(frames.size(), 1u);
    ExpectFrame(frames.front(), 1017, "delivered first");
}
