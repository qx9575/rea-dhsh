#include "formats/A16.h"

#include "formats/Cti.h"
#include "resource/ResourceError.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace dhsh {
namespace {

struct Header {
    std::array<char, 2> magic;
    std::uint16_t header_size;
    std::int16_t x;
    std::int16_t y;
    std::int16_t width;
    std::int16_t height;
    std::uint16_t flags;
    std::uint8_t compression;
    std::uint8_t alpha_mode;
    std::uint32_t stream1_size;
    std::uint32_t stream2_size;
};

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

Header parse_header(std::span<const std::byte> data) {
    if (data.size() < 24) {
        throw FormatError("not an A16/AI file: truncated header");
    }
    const std::array<char, 2> magic = {
        static_cast<char>(byte_at(data, 0)), static_cast<char>(byte_at(data, 1)),
    };
    if (magic != std::array<char, 2>{'A', 'P'}
        && magic != std::array<char, 2>{'A', 'L'}
        && magic != std::array<char, 2>{'A', 'I'}) {
        throw FormatError("not an A16/AI file: bad magic");
    }

    Header header{
        magic,
        read_u16(data, 2),
        static_cast<std::int16_t>(read_u16(data, 4)),
        static_cast<std::int16_t>(read_u16(data, 6)),
        static_cast<std::int16_t>(read_u16(data, 8)),
        static_cast<std::int16_t>(read_u16(data, 10)),
        read_u16(data, 12),
        byte_at(data, 14),
        byte_at(data, 15),
        read_u32(data, 16),
        read_u32(data, 20),
    };
    if (header.header_size < 24 || header.header_size > data.size()) {
        throw FormatError("invalid A16 header size " + std::to_string(header.header_size));
    }
    return header;
}

std::size_t image_size(const Header& header) {
    if (header.width <= 0 || header.height <= 0) {
        throw FormatError("invalid A16 dimensions");
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(header.width) * header.height;
    if (pixels > std::numeric_limits<std::uint32_t>::max() / 4ULL
        || pixels > std::numeric_limits<std::size_t>::max() / 4ULL
        || pixels > std::vector<std::byte>().max_size() / 4ULL) {
        throw FormatError("A16 dimensions overflow the RGBA buffer");
    }
    return static_cast<std::size_t>(pixels);
}

std::vector<std::uint16_t> rle16_decode(std::span<const std::byte> source,
    std::size_t pixel_count) {
    std::vector<std::uint16_t> output;
    output.reserve(pixel_count);
    std::size_t position = 0;
    while (position < source.size() && output.size() < pixel_count) {
        const std::uint8_t control = byte_at(source, position++);
        const std::size_t count = control & 0x7fU;
        if ((control & 0x80U) != 0) {
            if (position + 2 > source.size()) {
                break;
            }
            const std::uint16_t color = read_u16(source, position);
            position += 2;
            output.insert(output.end(), count, color);
        } else {
            const std::size_t available = source.size() - position;
            const std::size_t byte_count = std::min(count * 2, available);
            for (std::size_t index = 0; index + 1 < byte_count; index += 2) {
                output.push_back(read_u16(source, position + index));
            }
            position += byte_count;
        }
    }
    output.resize(pixel_count, 0);
    return output;
}

std::vector<std::uint8_t> rle8_decode(std::span<const std::byte> source,
    std::size_t output_size) {
    std::vector<std::uint8_t> output;
    output.reserve(output_size);
    std::size_t position = 0;
    while (position < source.size() && output.size() < output_size) {
        const std::uint8_t control = byte_at(source, position++);
        const std::size_t count = control & 0x7fU;
        if ((control & 0x80U) != 0) {
            if (position >= source.size()) {
                break;
            }
            const std::uint8_t value = byte_at(source, position++);
            output.insert(output.end(), count, value);
        } else {
            const std::size_t byte_count = std::min(count, source.size() - position);
            for (std::size_t index = 0; index < byte_count; ++index) {
                output.push_back(byte_at(source, position + index));
            }
            position += byte_count;
        }
    }
    if (output.size() > output_size) {
        output.resize(output_size);
    }
    return output;
}

std::vector<std::uint16_t> decode_pixels(const Header& header,
    std::span<const std::byte> stream, std::size_t pixel_count) {
    if (header.compression == 0) {
        std::vector<std::uint16_t> pixels;
        pixels.reserve(pixel_count);
        const std::size_t available = std::min(stream.size() / 2, pixel_count);
        for (std::size_t index = 0; index < available; ++index) {
            pixels.push_back(read_u16(stream, index * 2));
        }
        pixels.resize(pixel_count, 0);
        return pixels;
    }
    if (header.compression == 1) {
        const std::uint16_t color = stream.size() >= 2 ? read_u16(stream, 0) : 0;
        return std::vector<std::uint16_t>(pixel_count, color);
    }
    if (header.compression == 2) {
        return rle16_decode(stream, pixel_count);
    }
    throw FormatError("A16 compression " + std::to_string(header.compression)
        + " is not implemented");
}

std::optional<std::vector<std::uint8_t>> decode_alpha(const Header& header,
    std::span<const std::byte> stream, std::size_t pixel_count) {
    if (header.alpha_mode == 0) {
        return std::nullopt;
    }
    if (header.alpha_mode != 2) {
        throw FormatError("A16 alpha mode " + std::to_string(header.alpha_mode)
            + " is not implemented");
    }
    std::vector<std::uint8_t> alpha = rle8_decode(stream, pixel_count);
    for (std::uint8_t& value : alpha) {
        value = static_cast<std::uint8_t>(std::min(255U,
            static_cast<unsigned>(value) * 255U / 32U));
    }
    alpha.resize(pixel_count, 255);
    return alpha;
}

} // namespace

Image A16Decoder::decode(std::span<const std::byte> data) {
    return decode(data, nullptr);
}

Image A16Decoder::decode(std::span<const std::byte> data, const CtiPalette* palette) {
    const Header header = parse_header(data);
    if (header.compression > 2) {
        throw FormatError("A16 compression " + std::to_string(header.compression)
            + " is not implemented");
    }
    if (header.alpha_mode != 0 && header.alpha_mode != 2) {
        throw FormatError("A16 alpha mode " + std::to_string(header.alpha_mode)
            + " is not implemented");
    }
    const std::size_t pixel_count = image_size(header);
    const std::uint64_t stream1_end = static_cast<std::uint64_t>(header.header_size)
        + header.stream1_size;
    const std::uint64_t stream2_end = stream1_end + header.stream2_size;
    if (stream2_end > data.size()) {
        throw FormatError("truncated A16 pixel or alpha stream");
    }
    const auto stream1 = data.subspan(header.header_size, header.stream1_size);
    const auto stream2 = data.subspan(static_cast<std::size_t>(stream1_end), header.stream2_size);

    if (header.magic == std::array<char, 2>{'A', 'I'}) {
        if (palette == nullptr) {
            throw FormatError("AI image requires a CTI palette");
        }
        const std::vector<std::uint8_t> decoded_indices = rle8_decode(stream1, pixel_count);
        std::vector<std::byte> indices;
        indices.reserve(decoded_indices.size());
        for (const std::uint8_t index : decoded_indices) {
            indices.push_back(std::byte{index});
        }
        std::vector<std::byte> alpha_bytes;
        if (header.alpha_mode == 2) {
            const std::vector<std::uint8_t> decoded_alpha = rle8_decode(stream2, pixel_count);
            alpha_bytes.reserve(pixel_count);
            for (const std::uint8_t value : decoded_alpha) {
                alpha_bytes.push_back(std::byte{value});
            }
            alpha_bytes.resize(pixel_count, std::byte{32});
        }
        return palette->indexed_image(indices, header.width, header.height,
            CtiTable::table1, alpha_bytes);
    }

    const auto alpha = decode_alpha(header, stream2, pixel_count);
    Image image{
        static_cast<std::uint32_t>(header.width),
        static_cast<std::uint32_t>(header.height),
        std::vector<std::byte>(pixel_count * 4),
    };
    if (header.stream1_size == 0) {
        for (std::size_t index = 0; index < pixel_count; ++index) {
            image.rgba[index * 4] = std::byte{255};
            image.rgba[index * 4 + 1] = std::byte{255};
            image.rgba[index * 4 + 2] = std::byte{255};
            image.rgba[index * 4 + 3] = std::byte{static_cast<std::uint8_t>(alpha ? (*alpha)[index] : 255)};
        }
        return image;
    }

    const std::vector<std::uint16_t> pixels = decode_pixels(header, stream1, pixel_count);
    for (std::size_t index = 0; index < pixel_count; ++index) {
        const std::uint16_t color = pixels[index];
        const unsigned red = (color >> 11) & 0x1fU;
        const unsigned green = (color >> 5) & 0x3fU;
        const unsigned blue = color & 0x1fU;
        image.rgba[index * 4] = std::byte{static_cast<std::uint8_t>(red * 255U / 31U)};
        image.rgba[index * 4 + 1] = std::byte{static_cast<std::uint8_t>(green * 255U / 63U)};
        image.rgba[index * 4 + 2] = std::byte{static_cast<std::uint8_t>(blue * 255U / 31U)};
        image.rgba[index * 4 + 3] = std::byte{static_cast<std::uint8_t>(alpha ? (*alpha)[index] : 255)};
    }
    return image;
}

} // namespace dhsh
