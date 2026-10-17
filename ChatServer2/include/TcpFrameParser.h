#pragma once
#ifndef TCP_FRAME_PARSER_H
#define TCP_FRAME_PARSER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "const.h"

namespace chat_protocol {

/*
 * 一条已完成的 TCP 应用层帧。
 * 线上格式保持为 [消息 ID: 2 字节网络序] [包体长度: 2 字节网络序] [包体]。
 */
struct TcpFrame {
    std::uint16_t message_id = 0;
    std::string body;
};

/* 增量解析结果；非法头部会使解析器进入失败状态。 */
enum class TcpFrameParseStatus {
    Ok,
    InvalidMessageId,
    InvalidBodyLength,
    InvalidInput,
};

struct TcpFrameParseResult {
    TcpFrameParseStatus status = TcpFrameParseStatus::Ok;
    std::uint16_t message_id = 0;
    std::uint16_t body_length = 0;

    bool IsOk() const { return status == TcpFrameParseStatus::Ok; }
};

/*
 * TCP 字节流解析器。Feed 可接收任意大小的数据片段，并在 frames 中按顺序
 * 返回本次新完成的帧。未收全的包头或包体留在解析器内部，等待下次 Feed。
 * 包头校验规则与 CSession 原有行为相同：消息 ID 和包体长度必须在 1..MAX_LENGTH。
 */
class TcpFrameParser {
public:
    TcpFrameParseResult Feed(const void* data, std::size_t length,
                             std::vector<TcpFrame>& frames) {
        frames.clear();

        if (_failed) {
            return _failure;
        }
        if (length != 0 && data == nullptr) {
            _failed = true;
            _failure.status = TcpFrameParseStatus::InvalidInput;
            return _failure;
        }

        const auto* input = static_cast<const std::uint8_t*>(data);
        std::size_t offset = 0;
        while (offset < length) {
            if (_header_size < _header.size()) {
                const std::size_t count = (_header.size() - _header_size < length - offset)
                    ? _header.size() - _header_size
                    : length - offset;
                for (std::size_t index = 0; index < count; ++index) {
                    _header[_header_size + index] = input[offset + index];
                }
                _header_size += count;
                offset += count;

                if (_header_size != _header.size()) {
                    continue;
                }

                const std::uint16_t message_id = DecodeNetworkShort(_header.data());
                const std::uint16_t body_length = DecodeNetworkShort(_header.data() + HEAD_ID_LEN);
                if (message_id == 0 || message_id > MAX_LENGTH) {
                    return Fail(TcpFrameParseStatus::InvalidMessageId, message_id, body_length);
                }
                if (body_length == 0 || body_length > MAX_LENGTH) {
                    return Fail(TcpFrameParseStatus::InvalidBodyLength, message_id, body_length);
                }

                _message_id = message_id;
                _body.resize(body_length);
                _body_size = 0;
            }

            const std::size_t remaining_body = _body.size() - _body_size;
            const std::size_t count = remaining_body < length - offset
                ? remaining_body
                : length - offset;
            for (std::size_t index = 0; index < count; ++index) {
                _body[_body_size + index] = static_cast<char>(input[offset + index]);
            }
            _body_size += count;
            offset += count;

            if (_body_size == _body.size()) {
                frames.push_back(TcpFrame{_message_id, std::move(_body)});
                _body.clear();
                _header_size = 0;
                _body_size = 0;
                _message_id = 0;
            }
        }

        return {};
    }

private:
    static std::uint16_t DecodeNetworkShort(const std::uint8_t* bytes) {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(bytes[0]) << 8) |
            static_cast<std::uint16_t>(bytes[1]));
    }

    TcpFrameParseResult Fail(TcpFrameParseStatus status, std::uint16_t message_id,
                             std::uint16_t body_length) {
        _failed = true;
        _failure = TcpFrameParseResult{status, message_id, body_length};
        return _failure;
    }

    std::array<std::uint8_t, HEAD_TOTAL_LEN> _header{};
    std::size_t _header_size = 0;
    std::uint16_t _message_id = 0;
    std::string _body;
    std::size_t _body_size = 0;
    bool _failed = false;
    TcpFrameParseResult _failure{};
};

} // namespace chat_protocol

#endif // TCP_FRAME_PARSER_H
