#include "network/GraphKeyDecoder.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace dhsh {
namespace {

constexpr std::size_t header_size = 0x16;
constexpr std::size_t command_offset = 0x00;
constexpr std::size_t width_offset = 0x12;
constexpr std::size_t height_offset = 0x14;
constexpr std::uint16_t graph_key_command = 0x2005;
// Real challenges are 96x32; leave modest headroom for small variations while
// keeping every derived buffer size strictly bounded.
constexpr std::uint16_t max_dimension = 256;
// Real captures show a dominant background share of ~0.8; anything below 1/2
// means the "black background" premise does not hold and the payload must not
// be treated as an image.
constexpr std::uint8_t dominant_share_numerator = 1;
constexpr std::uint8_t dominant_share_denominator = 2;

std::uint16_t read_u16(const std::vector<std::byte>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset]))
        | static_cast<std::uint16_t>(
            std::to_integer<unsigned char>(bytes[offset + 1]) << 8);
}

std::uint8_t expand5(std::uint8_t value) {
    return static_cast<std::uint8_t>(value * 255 / 31);
}

std::uint8_t expand6(std::uint8_t value) {
    return static_cast<std::uint8_t>(value * 255 / 63);
}

} // namespace

const char* graphKeyDecodeMessage(GraphKeyDecodeError error) {
    switch (error) {
    case GraphKeyDecodeError::none:
        return "";
    case GraphKeyDecodeError::malformed_payload:
        return "graph-key payload is shorter than the image header";
    case GraphKeyDecodeError::unexpected_command:
        return "graph-key payload has an unexpected command";
    case GraphKeyDecodeError::invalid_dimensions:
        return "graph-key image dimensions are out of range";
    case GraphKeyDecodeError::pixel_size_mismatch:
        return "graph-key pixel stream does not match the image dimensions";
    case GraphKeyDecodeError::weak_background:
        return "graph-key background candidate is too weak";
    }
    return "graph-key decode failed";
}

GraphKeyDecodeResult GraphKeyDecoder::decode(const std::vector<std::byte>& payload) const {
    if (payload.size() < header_size) {
        return GraphKeyDecodeResult{GraphKeyDecodeError::malformed_payload, {}};
    }
    if (read_u16(payload, command_offset) != graph_key_command) {
        return GraphKeyDecodeResult{GraphKeyDecodeError::unexpected_command, {}};
    }

    const std::uint16_t width = read_u16(payload, width_offset);
    const std::uint16_t height = read_u16(payload, height_offset);
    if (width == 0 || height == 0 || width > max_dimension || height > max_dimension) {
        return GraphKeyDecodeResult{GraphKeyDecodeError::invalid_dimensions, {}};
    }

    // 64-bit math: width*height*2 must equal the pixel region exactly, with no
    // room for truncation, trailing bytes, or overflow.
    const std::uint64_t pixel_bytes =
        static_cast<std::uint64_t>(width) * height * 2;
    if (pixel_bytes != static_cast<std::uint64_t>(payload.size() - header_size)) {
        return GraphKeyDecodeResult{GraphKeyDecodeError::pixel_size_mismatch, {}};
    }

    const std::size_t pixel_offset = header_size;
    const std::size_t u16_count = static_cast<std::size_t>(width) * height;

    // The black RGB565 background dominates real challenges, so the most
    // frequent aligned ciphertext u16 is the repeating two-byte key.
    std::vector<std::uint32_t> histogram(0x10000, 0);
    for (std::size_t i = 0; i < u16_count; ++i) {
        ++histogram[read_u16(payload, pixel_offset + i * 2)];
    }
    std::uint16_t dominant = 0;
    std::uint32_t dominant_count = 0;
    for (std::size_t value = 0; value < histogram.size(); ++value) {
        if (histogram[value] > dominant_count) {
            dominant_count = histogram[value];
            dominant = static_cast<std::uint16_t>(value);
        }
    }
    if (static_cast<std::uint64_t>(dominant_count) * dominant_share_denominator
        < static_cast<std::uint64_t>(u16_count) * dominant_share_numerator) {
        return GraphKeyDecodeResult{GraphKeyDecodeError::weak_background, {}};
    }

    GraphKeyImage image;
    image.width = width;
    image.height = height;
    image.rgba.resize(static_cast<std::size_t>(u16_count) * 4);
    for (std::size_t i = 0; i < u16_count; ++i) {
        const std::uint16_t value =
            static_cast<std::uint16_t>(read_u16(payload, pixel_offset + i * 2) ^ dominant);
        image.rgba[i * 4 + 0] = expand5(static_cast<std::uint8_t>((value >> 11) & 0x1f));
        image.rgba[i * 4 + 1] = expand6(static_cast<std::uint8_t>((value >> 5) & 0x3f));
        image.rgba[i * 4 + 2] = expand5(static_cast<std::uint8_t>(value & 0x1f));
        image.rgba[i * 4 + 3] = 255;
    }
    return GraphKeyDecodeResult{GraphKeyDecodeError::none, std::move(image)};
}

} // namespace dhsh
