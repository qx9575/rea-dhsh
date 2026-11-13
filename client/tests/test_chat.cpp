#include "chat/ChatModel.h"
#include "network/ChatMessageDecoder.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Bytes bytes_from_hex(std::string_view hex) {
    check(hex.size() % 2 == 0, "odd golden hex length");
    const auto nibble = [](char value) -> unsigned {
        if (value >= '0' && value <= '9') {
            return static_cast<unsigned>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
            return static_cast<unsigned>(value - 'a' + 10);
        }
        if (value >= 'A' && value <= 'F') {
            return static_cast<unsigned>(value - 'A' + 10);
        }
        throw std::runtime_error("invalid golden hex");
    };
    Bytes bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t index = 0; index < hex.size(); index += 2) {
        bytes.push_back(static_cast<std::byte>(
            (nibble(hex[index]) << 4) | nibble(hex[index + 1])));
    }
    return bytes;
}

void append_u32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

Bytes make_payload(std::uint32_t channel, std::string_view sender, const Bytes& text) {
    check(sender.size() < 32, "synthetic sender is too long");
    Bytes payload(144, std::byte{0});
    for (unsigned index = 0; index < 4; ++index) {
        payload[4 + index] = static_cast<std::byte>((channel >> (index * 8)) & 0xffU);
    }
    for (std::size_t index = 0; index < sender.size(); ++index) {
        payload[26 + index] = static_cast<std::byte>(sender[index]);
    }
    payload.insert(payload.end(), text.begin(), text.end());
    payload.push_back(std::byte{0});
    return payload;
}

Bytes ascii_bytes(std::string_view text) {
    Bytes bytes;
    for (const char value : text) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

std::optional<dhsh::ChatMessage> decode(Bytes payload, std::uint16_t command = 0x303f) {
    return dhsh::ChatMessageDecoder{}.decode(dhsh::DecodedMessage{
        0, command, dhsh::MessageDirection::server_to_client, std::move(payload)});
}

const dhsh::ChatSpan* find_span(const dhsh::ChatMessage& message,
    dhsh::ChatSpanKind kind, std::string_view text = {}) {
    const auto found = std::find_if(message.spans.begin(), message.spans.end(),
        [&](const dhsh::ChatSpan& span) {
            return span.kind == kind && (text.empty() || span.text.find(text) != std::string::npos);
        });
    return found == message.spans.end() ? nullptr : &*found;
}

std::string visible_text(const dhsh::ChatMessage& message) {
    std::string text;
    for (const auto& span : message.spans) {
        text += span.text;
    }
    return text;
}

dhsh::ChatMessage model_message(std::uint32_t channel, std::string text) {
    return dhsh::ChatMessage{channel, "tester", {dhsh::ChatSpan{
        dhsh::ChatSpanKind::text, std::move(text), "red", {}}}};
}

// Sanitized derivatives of real 0x303f packets. Names and message text are
// replaced; evidenced offsets, channel values, item markup and metadata remain.
std::optional<dhsh::ChatMessage> decode_system_golden() {
    return decode(bytes_from_hex(
        "000000000000000000000000a2c0000000000000000000000000cfb5cdb3000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000d5fdd4da2379656c6c6f77d3a6ccecb8ae23726564b2e2cad4a1a30ab5dab6fed0d000"));
}

std::optional<dhsh::ChatMessage> decode_world_golden() {
    return decode(bytes_from_hex(
        "000000000500000000000000968100000200a981c000663d9f00b2e2cad4d5df00000000000000000000000000000000000000000000000000000a000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000005441534b365f574f524c442023363300"));
}

std::optional<dhsh::ChatMessage> decode_custom_golden() {
    return decode(bytes_from_hex(
        "000000000900000000000000108000000200f66ec400ea62a800b2e2cad4b6d3cee900000000000000000000000000000000000000000000000004000000fddb0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000d5d0c4bcb6d3d4b105425206b5dab6fed0d000"));
}

void test_system_golden_decodes_gbk_colors_and_line_breaks() {
    const auto message = decode_system_golden();
    check(message.has_value() && message->channel == 0 && message->sender == "系统",
        "real system packet did not decode its channel and GBK sender");
    const auto* location = find_span(*message, dhsh::ChatSpanKind::text, "应天府");
    check(location != nullptr && location->color == "yellow",
        "known color marker did not apply to the following GBK text");
    check(find_span(*message, dhsh::ChatSpanKind::line_break) != nullptr,
        "real line break was not preserved as a span");
    check(visible_text(*message).find("#yellow") == std::string::npos,
        "known color marker leaked into visible text");
}

void test_channel_filters_preserve_history_and_custom_channels() {
    const auto world = decode_world_golden();
    const auto custom = decode_custom_golden();
    check(world.has_value() && world->channel == 5 && world->sender == "测试者",
        "real world packet did not decode");
    check(custom.has_value() && custom->channel == 9,
        "real non-world channel packet did not decode");

    dhsh::ChatModel model;
    model.append(*world);
    model.append(*custom);
    model.append(model_message(1, "synthetic current-channel model input"));
    model.set_channel_filter(5);
    check(model.visible_messages().size() == 1
            && model.visible_messages().front().channel == 5,
        "world filter included another channel");
    model.set_channel_filter(9);
    check(model.visible_messages().size() == 1
            && model.visible_messages().front().channel == 9,
        "custom channel filter failed");
    model.set_channel_filter(std::nullopt);
    check(model.visible_messages().size() == 3,
        "changing channel filter deleted chat history");
}

void test_real_item_link_and_metadata_are_preserved_safely() {
    const auto message = decode(bytes_from_hex(
        "0000000000000000000000001760000000000000000000000000cfb5cdb300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000023626c7565b2e2cad4d5df23726564bbf1b5c3c1cb3c4931205bb9b3c1add8ddb9c7c8adccd75d3ea1a3043030303149424241424141414b4f4349454d41414f42414141414141424141414141414141454a4242414141494143414941434141414141414141414d4141414141414146494142414141414c45414141414141414144414141414141414141c1a6c1bf202b31362cbbf9b1beb9a5bbf7c1a6202b3139332cbbf9b1bec3fcd6d0c2ca202b3130390aceefc0edc9cbbaa6202b37250a0a23677265656eb9b3c1adccd7d7b00ab9b3c1add8ddb9c7c1b420b9b3c1add8ddb9c7bde4200a0accd7d7b0cdeab3c9caf4d0d4a3ba0a23677265656e283229cce5c1a6202b342cc1a6c1bf202b382cc3f4bddd202b340a23677265656e283329d7b7bcd34850c9cfcfde2037250a0a2379656c6c6f77d7b0b1b8c6c0b7d6a3ba323235360a00"));
    check(message.has_value(), "real item-link packet did not decode");
    const auto* item = find_span(*message, dhsh::ChatSpanKind::item_link, "钩镰剌骨拳套");
    check(item != nullptr && item->link == "1",
        "item link display name or evidenced link id was lost");
    check(visible_text(*message).find("IBBABAAA") != std::string::npos,
        "item metadata was silently discarded");
    check(visible_text(*message).find("\\x04") != std::string::npos,
        "item control byte was not rendered as diagnostic-safe text");
    check(find_span(*message, dhsh::ChatSpanKind::line_break) != nullptr,
        "item stat line breaks were lost");
}

void test_unknown_markup_and_controls_remain_visible_plain_text() {
    Bytes text = ascii_bytes("#63 <unknown> ");
    text.push_back(std::byte{2});
    const auto message = decode(make_payload(7, "tester", text));
    check(message.has_value(), "unknown markup made an otherwise valid packet fail");
    const std::string visible = visible_text(*message);
    check(visible.find("#63 <unknown> \\x02") != std::string::npos,
        "unknown markup/control was executed, hidden, or corrupted: " + visible);
}

void test_decoder_rejects_invalid_boundaries_and_direction() {
    check(!decode(ascii_bytes("short")).has_value(), "short packet decoded");
    check(!decode(make_payload(5, "tester", ascii_bytes("text")), 0x301e).has_value(),
        "unrelated command decoded as chat");
    auto wrong_direction = dhsh::DecodedMessage{0, 0x303f,
        dhsh::MessageDirection::client_to_server,
        make_payload(5, "tester", ascii_bytes("text"))};
    check(!dhsh::ChatMessageDecoder{}.decode(wrong_direction).has_value(),
        "c2s message decoded as a server broadcast");

    auto embedded_nul = make_payload(5, "tester", Bytes{std::byte{'a'}, std::byte{0}, std::byte{'b'}});
    check(!decode(std::move(embedded_nul)).has_value(), "embedded NUL truncated chat text");
    check(!decode(make_payload(5, "tester", Bytes(4001, std::byte{'x'}))).has_value(),
        "defensive text-size limit was not enforced");
    check(decode(make_payload(5, "tester", Bytes(4000, std::byte{'x'}))).has_value(),
        "defensive text-size boundary rejected the maximum valid input");
}

void test_long_lines_history_cap_and_paused_scroll() {
    dhsh::ChatModel model;
    const std::string long_line(300, 'x');
    model.append(model_message(5, long_line));
    model.append(model_message(5, "second"));
    model.append(model_message(5, "third"));
    check(model.visible_messages().front().spans.front().text == long_line,
        "long wrapping input was truncated or altered in the shared model");

    model.set_auto_scroll(false);
    model.scroll(1);
    check(model.scroll_offset_lines() == 1, "manual scroll did not move away from latest");
    model.append(model_message(5, "paused append"));
    check(model.scroll_offset_lines() == 1 && !model.auto_scroll(),
        "paused scrolling jumped when a message arrived");
    model.set_auto_scroll(true);
    model.append(model_message(5, "resume append"));
    check(model.scroll_offset_lines() == 0 && model.auto_scroll(),
        "resuming auto-scroll did not return to the latest message");

    for (int index = 0; index < 205; ++index) {
        model.append(model_message(5, "cap" + std::to_string(index)));
    }
    model.set_channel_filter(std::nullopt);
    check(model.visible_messages().size() == 200,
        "defensive message-count cap was not enforced");
    check(model.visible_messages().back().spans.front().text == "cap204",
        "history cap discarded the newest message");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests = {
        {"system golden GBK colors and line breaks", test_system_golden_decodes_gbk_colors_and_line_breaks},
        {"channel filters preserve custom history", test_channel_filters_preserve_history_and_custom_channels},
        {"real item link and metadata", test_real_item_link_and_metadata_are_preserved_safely},
        {"unknown markup remains plain text", test_unknown_markup_and_controls_remain_visible_plain_text},
        {"decoder rejects invalid boundaries", test_decoder_rejects_invalid_boundaries_and_direction},
        {"long lines history cap and paused scroll", test_long_lines_history_cap_and_paused_scroll},
    };
    bool passed = true;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
            passed = false;
        }
    }
    return passed ? 0 : 1;
}
