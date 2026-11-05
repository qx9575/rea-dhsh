#include "formats/Cti.h"

#include "resource/ResourceError.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

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

std::size_t image_size(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        throw FormatError("invalid CTI indexed-image dimensions");
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels > std::numeric_limits<std::uint32_t>::max() / 4ULL
        || pixels > std::numeric_limits<std::size_t>::max() / 4ULL
        || pixels > std::vector<std::byte>().max_size() / 4ULL) {
        throw FormatError("CTI indexed-image dimensions overflow the RGBA buffer");
    }
    return static_cast<std::size_t>(pixels);
}

} // namespace

CtiPalette CtiPalette::parse(std::span<const std::byte> data) {
    if (data.size() < 20 || byte_at(data, 0) != 'C' || byte_at(data, 1) != 'T') {
        throw FormatError("not a CTI file");
    }
    CtiPalette palette;
    palette.header = {
        read_u16(data, 2),
        read_u16(data, 4),
        read_u16(data, 6),
        read_u32(data, 8),
        read_u32(data, 12),
        read_u32(data, 16),
    };
    if (palette.header.header_size < 20 || palette.header.header_size > data.size()) {
        throw FormatError("invalid CTI header size");
    }
    const std::uint64_t table1_offset = palette.header.header_size;
    const std::uint64_t table2_offset = table1_offset + palette.header.table1_size;
    const std::uint64_t end = table2_offset + palette.header.table2_size;
    if (end > data.size()) {
        throw FormatError("truncated CTI file");
    }
    const std::size_t table1_bytes = static_cast<std::size_t>(palette.header.count) * 2;
    const std::size_t table2_bytes = static_cast<std::size_t>(palette.header.count) * 4;
    if (palette.header.table1_size < table1_bytes
        || palette.header.table2_size < table2_bytes) {
        throw FormatError("CTI palette table is smaller than its entry count");
    }

    palette.table1.reserve(palette.header.count);
    for (std::size_t index = 0; index < palette.header.count; ++index) {
        palette.table1.push_back(read_u16(data,
            static_cast<std::size_t>(table1_offset) + index * 2));
    }
    palette.table2.reserve(palette.header.count);
    for (std::size_t index = 0; index < palette.header.count; ++index) {
        palette.table2.push_back(read_u32(data,
            static_cast<std::size_t>(table2_offset) + index * 4));
    }
    return palette;
}

Image CtiPalette::indexed_image(std::span<const std::byte> indices,
    std::uint32_t width, std::uint32_t height, CtiTable table,
    std::span<const std::byte> alpha) const {
    const std::size_t pixel_count = image_size(width, height);
    if (indices.size() != pixel_count || (!alpha.empty() && alpha.size() != pixel_count)) {
        throw FormatError("CTI index or alpha count does not match the image dimensions");
    }
    if (table != CtiTable::table1 && table != CtiTable::table2) {
        throw FormatError("unsupported CTI palette table");
    }

    Image image{width, height, std::vector<std::byte>(pixel_count * 4)};
    for (std::size_t index = 0; index < pixel_count; ++index) {
        const std::size_t palette_index = byte_at(indices, index);
        const std::size_t palette_size = table == CtiTable::table1 ? table1.size() : table2.size();
        if (palette_index >= palette_size) {
            throw FormatError("CTI palette index " + std::to_string(palette_index)
                + " is out of range");
        }
        unsigned red;
        unsigned green;
        unsigned blue;
        unsigned opacity;
        if (table == CtiTable::table1) {
            const std::uint16_t color = table1[palette_index];
            red = ((color >> 11) & 0x1fU) * 255U / 31U;
            green = ((color >> 5) & 0x3fU) * 255U / 63U;
            blue = (color & 0x1fU) * 255U / 31U;
            opacity = 255;
        } else {
            const std::uint32_t color = table2[palette_index];
            red = (color >> 16) & 0xffU;
            green = (color >> 8) & 0xffU;
            blue = color & 0xffU;
            opacity = color >> 24;
        }
        if (!alpha.empty()) {
            opacity = std::min(255U, static_cast<unsigned>(byte_at(alpha, index)) * 255U / 32U);
        }
        image.rgba[index * 4] = std::byte{static_cast<std::uint8_t>(red)};
        image.rgba[index * 4 + 1] = std::byte{static_cast<std::uint8_t>(green)};
        image.rgba[index * 4 + 2] = std::byte{static_cast<std::uint8_t>(blue)};
        image.rgba[index * 4 + 3] = std::byte{static_cast<std::uint8_t>(opacity)};
    }
    return image;
}

} // namespace dhsh
