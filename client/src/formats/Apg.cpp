#include "formats/Apg.h"

#include "formats/A16.h"
#include "platform/WicImage.h"
#include "resource/ResourceError.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

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

bool magic_is(std::span<const std::byte> data, std::size_t offset, char second) {
    return byte_at(data, offset) == 'A' && byte_at(data, offset + 1) == second;
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
            const std::size_t take = std::min(count, output_size - output.size());
            output.insert(output.end(), take, value);
        } else {
            const std::size_t take = std::min({count, source.size() - position,
                output_size - output.size()});
            for (std::size_t index = 0; index < take; ++index) {
                output.push_back(byte_at(source, position + index));
            }
            position += take;
        }
    }
    output.resize(output_size, 255);
    return output;
}

void apply_alpha(Image& image, std::span<const std::byte> stream) {
    const std::size_t pixel_count = image.rgba.size() / 4;
    const std::vector<std::uint8_t> alpha = rle8_decode(stream, pixel_count);
    for (std::size_t index = 0; index < pixel_count; ++index) {
        image.rgba[index * 4 + 3] = std::byte{static_cast<std::uint8_t>(
            std::min(255U, static_cast<unsigned>(alpha[index]) * 255U / 32U))};
    }
}

std::size_t checked_rgba_size(std::span<const std::byte> data, std::size_t position,
    const ApgDecodeOptions& options, std::size_t animation_rgba_bytes) {
    const std::int16_t width = static_cast<std::int16_t>(read_u16(data, position + 8));
    const std::int16_t height = static_cast<std::int16_t>(read_u16(data, position + 10));
    if (width <= 0 || height <= 0) {
        throw FormatError("invalid APG dimensions");
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels > std::numeric_limits<std::uint32_t>::max() / 4ULL
        || pixels > std::numeric_limits<std::size_t>::max() / 4ULL
        || pixels > std::vector<std::byte>().max_size() / 4ULL) {
        throw FormatError("APG dimensions overflow the RGBA buffer");
    }
    const std::uint64_t rgba_bytes = pixels * 4;
    if (rgba_bytes > options.max_frame_rgba_bytes) {
        throw FormatError("APG frame RGBA budget exceeded");
    }
    if (rgba_bytes > options.max_animation_rgba_bytes
        || animation_rgba_bytes > options.max_animation_rgba_bytes - rgba_bytes) {
        throw FormatError("APG animation RGBA budget exceeded");
    }
    return static_cast<std::size_t>(rgba_bytes);
}

} // namespace

ApgAnimation ApgDecoder::decode(std::span<const std::byte> data, const CtiPalette* palette,
    ApgDecodeOptions options) {
    if (data.size() < 8 || !magic_is(data, 0, 'G')) {
        throw FormatError("not an APG file");
    }
    ApgAnimation result;
    result.header.header_size = read_u16(data, 2);
    result.header.frame_count = read_u16(data, 4);
    result.header.flags = read_u16(data, 6);
    result.header.reserved = 0;
    const std::size_t index_size = static_cast<std::size_t>(result.header.frame_count) * 8;
    if (result.header.header_size < 8 + index_size
        || result.header.header_size > data.size()) {
        throw FormatError("invalid AG header size");
    }
    if (result.header.header_size >= 12) {
        result.header.reserved = read_u32(data, 8);
    }
    result.header.entries.reserve(result.header.frame_count);
    for (std::size_t index = 0; index < result.header.frame_count; ++index) {
        const std::size_t offset = 8 + index * 8;
        result.header.entries.push_back(AgIndexEntry{
            read_u16(data, offset),
            read_u16(data, offset + 2),
            read_u32(data, offset + 4),
        });
    }

    std::size_t position = result.header.header_size;
    std::size_t animation_rgba_bytes = 0;
    result.frames.reserve(result.header.frame_count);
    for (std::size_t index = 0; index < result.header.frame_count; ++index) {
        const std::uint32_t delta = result.header.entries[index].delta;
        if (delta < 24) {
            throw FormatError("invalid AG frame delta");
        }
        if (position > data.size() || data.size() - position < 24) {
            throw FormatError("truncated APG frame header");
        }
        const std::uint64_t delta_end = static_cast<std::uint64_t>(position) + delta;
        if (delta_end > data.size()) {
            throw FormatError("truncated APG frame delta");
        }
        const bool ai = magic_is(data, position, 'I');
        if (!ai && !magic_is(data, position, 'P')) {
            throw FormatError("APG frame has bad AP/AI magic");
        }
        const std::uint16_t frame_header_size = read_u16(data, position + 2);
        if (frame_header_size < 24 || frame_header_size > delta) {
            throw FormatError("invalid APG frame header size or delta");
        }
        const std::size_t rgba_bytes = checked_rgba_size(
            data, position, options, animation_rgba_bytes);
        animation_rgba_bytes += rgba_bytes;
        const std::uint32_t stream1_size = read_u32(data, position + 16);
        const std::uint32_t stream2_size = read_u32(data, position + 20);
        const std::uint64_t stream1_end = static_cast<std::uint64_t>(position)
            + frame_header_size + stream1_size;
        const std::uint64_t frame_end = stream1_end + stream2_size;
        if (frame_end > delta_end) {
            throw FormatError("AG delta is smaller than its frame block");
        }
        const std::size_t frame_size = static_cast<std::size_t>(frame_end - position);
        const auto block = data.subspan(position, frame_size);
        const auto stream1 = data.subspan(
            static_cast<std::size_t>(position + frame_header_size), stream1_size);
        const auto stream2 = data.subspan(static_cast<std::size_t>(stream1_end), stream2_size);

        Image image;
        if (ai) {
            image = A16Decoder::decode(block, palette);
        } else if (byte_at(data, position + 14) == 5) {
            image = WicImage::decode(stream1);
            const std::uint32_t width = read_u16(data, position + 8);
            const std::uint32_t height = read_u16(data, position + 10);
            if (image.width != width || image.height != height) {
                throw FormatError("JPEG APG dimensions differ from the frame header");
            }
            const std::uint8_t alpha_mode = byte_at(data, position + 15);
            if (alpha_mode == 2) {
                apply_alpha(image, stream2);
            } else if (alpha_mode != 0) {
                throw FormatError("APG alpha mode " + std::to_string(alpha_mode)
                    + " is not implemented");
            }
        } else {
            image = A16Decoder::decode(block);
        }
        result.frames.push_back(ApgFrame{
            index,
            position,
            static_cast<std::int16_t>(read_u16(data, position + 4)),
            static_cast<std::int16_t>(read_u16(data, position + 6)),
            std::move(image),
        });
        position = static_cast<std::size_t>(delta_end);
    }
    return result;
}

} // namespace dhsh
