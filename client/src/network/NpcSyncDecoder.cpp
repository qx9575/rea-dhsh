#include "network/NpcSyncDecoder.h"

#include <windows.h>

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dhsh {
namespace {

constexpr std::uint16_t object_info_command = 0x301e;
constexpr std::size_t short_npc_payload_size = 0x9f;
constexpr std::size_t name_offset = 0x10;
constexpr std::size_t name_size = 0x20;
constexpr std::size_t object_id_offset = 0x30;
constexpr std::size_t shape_offset = 0x34;
constexpr std::size_t cell_x_offset = 0x36;
constexpr std::size_t cell_y_offset = 0x3a;
constexpr std::size_t direction_offset = 0x3e;
constexpr std::size_t object_type_offset = 0x3f;
constexpr std::uint8_t evidenced_npc_type = 3;

std::uint16_t read_u16(const std::vector<std::byte>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset]))
        | static_cast<std::uint16_t>(
            std::to_integer<unsigned char>(bytes[offset + 1]) << 8);
}

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

std::optional<EntityPosition> pixel_position(
    std::uint32_t cell_x, std::uint32_t cell_y) {
    constexpr std::uint32_t maximum_cell =
        (static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()) - 8) / 16;
    if (cell_x > maximum_cell || cell_y > maximum_cell) {
        return std::nullopt;
    }
    return EntityPosition{
        static_cast<std::int32_t>(cell_x * 16 + 8),
        static_cast<std::int32_t>(cell_y * 16 + 8)};
}

} // namespace

NpcSyncEvent NpcSyncDecoder::decode(const DecodedMessage& message) const {
    if (message.direction != MessageDirection::server_to_client
        || message.command != object_info_command
        || message.payload.size() != short_npc_payload_size
        || std::to_integer<unsigned char>(message.payload[object_type_offset]) != evidenced_npc_type) {
        return std::monostate{};
    }

    const std::uint8_t wire_direction =
        std::to_integer<unsigned char>(message.payload[direction_offset]);
    if (wire_direction > 7) {
        return std::monostate{};
    }
    const std::uint32_t shape = read_u16(message.payload, shape_offset);
    if (shape == 0) {
        return std::monostate{};
    }
    const auto position = pixel_position(
        read_u32(message.payload, cell_x_offset), read_u32(message.payload, cell_y_offset));
    if (!position.has_value()) {
        return std::monostate{};
    }

    std::string name_bytes;
    name_bytes.reserve(name_size);
    for (std::size_t index = 0; index < name_size; ++index) {
        const char value = static_cast<char>(message.payload[name_offset + index]);
        if (value == '\0') {
            break;
        }
        name_bytes.push_back(value);
    }
    auto name = gbk_to_utf8(name_bytes);
    if (!name.has_value()) {
        return std::monostate{};
    }

    return ObjectUpsert{WorldObject{
        read_u32(message.payload, object_id_offset),
        WorldObjectKind::npc,
        std::nullopt,
        shape,
        std::move(*name),
        *position,
        static_cast<Direction>(wire_direction),
        0}};
}

} // namespace dhsh
