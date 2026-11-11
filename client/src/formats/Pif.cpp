#include "formats/Pif.h"

#include "resource/ResourceError.h"

#include <cstdint>

namespace dhsh {
namespace {

std::uint8_t byte_at(std::span<const std::byte> data, std::size_t offset) {
    return std::to_integer<std::uint8_t>(data[offset]);
}

std::uint16_t read_u16(std::span<const std::byte> data, std::size_t offset) {
    return static_cast<std::uint16_t>(byte_at(data, offset))
        | static_cast<std::uint16_t>(byte_at(data, offset + 1)) << 8;
}

std::uint32_t read_u32(std::span<const std::byte> data, std::size_t offset) {
    return static_cast<std::uint32_t>(byte_at(data, offset))
        | static_cast<std::uint32_t>(byte_at(data, offset + 1)) << 8
        | static_cast<std::uint32_t>(byte_at(data, offset + 2)) << 16
        | static_cast<std::uint32_t>(byte_at(data, offset + 3)) << 24;
}

} // namespace

Pif Pif::parse(std::span<const std::byte> data) {
    if (data.size() < 20 || byte_at(data, 0) != 'P' || byte_at(data, 1) != 'I'
        || byte_at(data, 2) != 'F' || byte_at(data, 3) != 0) {
        throw FormatError("not a PIF file");
    }
    Pif result;
    result.count = read_u32(data, 4);
    result.data_offset = read_u16(data, 8);
    result.height = read_u16(data, 10);
    result.width = read_u16(data, 12);
    result.flags = read_u16(data, 14);
    result.reserved = read_u32(data, 16);
    if (result.count > 3) {
        throw FormatError("unsupported PIF count");
    }
    if (result.data_offset < 1) {
        result.data_offset = 20;
    }
    if (result.width == 0 || (result.height != 24 && result.height != 28)) {
        throw FormatError("invalid PIF dimensions or row stride");
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(result.width) * result.height;
    if (static_cast<std::uint64_t>(result.data_offset) + bytes > data.size()) {
        throw FormatError("truncated PIF row data");
    }

    const std::size_t source_stride = result.height;
    result.rows.assign(result.width, {});
    for (std::size_t row = 0; row < result.rows.size(); ++row) {
        const std::size_t offset = result.data_offset + row * source_stride;
        for (std::size_t field = 0; field < source_stride / 2; ++field) {
            result.rows[row][field] = read_u16(data, offset + field * 2);
        }
    }
    result.count = 3;
    result.height = 28;
    return result;
}

} // namespace dhsh
