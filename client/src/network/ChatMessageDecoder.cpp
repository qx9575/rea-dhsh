#include "network/ChatMessageDecoder.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dhsh {
namespace {

constexpr std::uint16_t chat_command = 0x303f;
constexpr std::size_t channel_offset = 4;
constexpr std::size_t sender_offset = 26;
constexpr std::size_t sender_size = 32;
constexpr std::size_t text_offset = 144;
constexpr std::size_t max_text_bytes = 4000; // Client safeguard, not an original limit.
constexpr std::size_t max_item_marker_bytes = 256;

std::uint32_t read_u32(const std::vector<std::byte>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(
            std::to_integer<unsigned char>(bytes[offset + index])) << (index * 8);
    }
    return value;
}

std::optional<std::string> gbk_to_utf8(std::string_view input) {
    if (input.empty()) {
        return std::string{};
    }
    const int input_size = static_cast<int>(input.size());
    const int wide_size = MultiByteToWideChar(936, 0, input.data(), input_size, nullptr, 0);
    if (wide_size <= 0) {
        return std::nullopt;
    }
    std::vector<wchar_t> wide(static_cast<std::size_t>(wide_size));
    if (MultiByteToWideChar(936, 0, input.data(), input_size, wide.data(), wide_size) <= 0) {
        return std::nullopt;
    }
    const int utf8_size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size,
        nullptr, 0, nullptr, nullptr);
    if (utf8_size <= 0) {
        return std::nullopt;
    }
    std::string output(static_cast<std::size_t>(utf8_size), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size,
            output.data(), utf8_size, nullptr, nullptr) <= 0) {
        return std::nullopt;
    }
    return output;
}

std::optional<std::string> decode_sender(const std::vector<std::byte>& payload) {
    std::string bytes;
    bytes.reserve(sender_size);
    bool terminated = false;
    for (std::size_t index = 0; index < sender_size; ++index) {
        const char value = static_cast<char>(payload[sender_offset + index]);
        if (value == '\0') {
            terminated = true;
        } else {
            if (terminated) {
                return std::nullopt;
            }
            bytes.push_back(value);
        }
    }
    return gbk_to_utf8(bytes);
}

class RichTextParser {
public:
    std::optional<std::vector<ChatSpan>> parse(std::string_view input) {
        std::size_t index = 0;
        while (index < input.size()) {
            if (const auto color = color_at(input, index); color.has_value()) {
                if (!flush()) {
                    return std::nullopt;
                }
                color_ = *color;
                index += color->size() + 1;
                continue;
            }
            if (input[index] == '\n') {
                if (!flush()) {
                    return std::nullopt;
                }
                spans_.push_back(ChatSpan{ChatSpanKind::line_break, "\n", color_, {}});
                ++index;
                continue;
            }
            if (input[index] == '<') {
                if (const auto item = item_at(input, index); item.has_value()) {
                    if (!flush()) {
                        return std::nullopt;
                    }
                    auto name = gbk_to_utf8(item->name);
                    if (!name.has_value()) {
                        return std::nullopt;
                    }
                    spans_.push_back(ChatSpan{ChatSpanKind::item_link,
                        std::move(*name), color_, std::move(item->id)});
                    index = item->end;
                    continue;
                }
            }
            const auto byte = static_cast<unsigned char>(input[index]);
            if (byte < 0x20 || byte == 0x7f) {
                if (!flush()) {
                    return std::nullopt;
                }
                constexpr char hex[] = "0123456789abcdef";
                std::string escaped = "\\x00";
                escaped[2] = hex[byte >> 4];
                escaped[3] = hex[byte & 0x0f];
                spans_.push_back(ChatSpan{ChatSpanKind::text, std::move(escaped), color_, {}});
                ++index;
                continue;
            }
            pending_.push_back(input[index]);
            ++index;
        }
        if (!flush()) {
            return std::nullopt;
        }
        return std::move(spans_);
    }

private:
    struct ItemMarker {
        std::string id;
        std::string_view name;
        std::size_t end = 0;
    };

    static std::optional<std::string_view> color_at(std::string_view input, std::size_t index) {
        constexpr std::string_view colors[] = {"#red", "#blue", "#yellow", "#green"};
        for (const auto color : colors) {
            if (input.substr(index, color.size()) == color) {
                return color.substr(1);
            }
        }
        return std::nullopt;
    }

    static std::optional<ItemMarker> item_at(std::string_view input, std::size_t index) {
        if (input.substr(index, 2) != "<I") {
            return std::nullopt;
        }
        const std::size_t limit = (std::min)(input.size(), index + max_item_marker_bytes);
        const std::size_t close = input.find('>', index + 2);
        if (close == std::string_view::npos || close >= limit) {
            return std::nullopt;
        }
        std::size_t cursor = index + 2;
        const std::size_t id_begin = cursor;
        while (cursor < close && input[cursor] >= '0' && input[cursor] <= '9') {
            ++cursor;
        }
        if (cursor == id_begin || cursor >= close || input[cursor] != ' ') {
            return std::nullopt;
        }
        const std::string id(input.substr(id_begin, cursor - id_begin));
        while (cursor < close && input[cursor] == ' ') {
            ++cursor;
        }
        if (cursor >= close || input[cursor] != '[' || input[close - 1] != ']') {
            return std::nullopt;
        }
        const std::size_t name_begin = cursor + 1;
        return ItemMarker{id, input.substr(name_begin, close - 1 - name_begin), close + 1};
    }

    bool flush() {
        if (pending_.empty()) {
            return true;
        }
        const std::string bytes = pending_;
        pending_.clear();
        auto text = gbk_to_utf8(bytes);
        if (!text.has_value()) {
            return false;
        }
        spans_.push_back(ChatSpan{ChatSpanKind::text, std::move(*text), color_, {}});
        return true;
    }

    std::vector<ChatSpan> spans_;
    std::string pending_;
    std::string color_ = "red";
};

} // namespace

std::optional<ChatMessage> ChatMessageDecoder::decode(const DecodedMessage& message) const {
    if (message.direction != MessageDirection::server_to_client
        || message.command != chat_command
        || message.payload.size() <= text_offset
        || message.payload.back() != std::byte{0}) {
        return std::nullopt;
    }
    const std::size_t text_size = message.payload.size() - text_offset - 1;
    if (text_size > max_text_bytes) {
        return std::nullopt;
    }
    const auto text_begin = message.payload.begin() + static_cast<std::ptrdiff_t>(text_offset);
    if (std::find(text_begin, message.payload.end() - 1, std::byte{0}) != message.payload.end() - 1) {
        return std::nullopt;
    }
    auto sender = decode_sender(message.payload);
    if (!sender.has_value()) {
        return std::nullopt;
    }
    std::string text;
    text.reserve(text_size);
    for (std::size_t index = text_offset; index + 1 < message.payload.size(); ++index) {
        text.push_back(static_cast<char>(message.payload[index]));
    }
    auto spans = RichTextParser{}.parse(text);
    if (!spans.has_value()) {
        return std::nullopt;
    }
    return ChatMessage{read_u32(message.payload, channel_offset),
        std::move(*sender), std::move(*spans)};
}

} // namespace dhsh
