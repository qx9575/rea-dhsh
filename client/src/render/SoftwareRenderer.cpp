#include "render/SoftwareRenderer.h"

#include "resource/ResourceError.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace dhsh {
namespace {

std::size_t checked_rgba_bytes(
    std::uint32_t width,
    std::uint32_t height,
    std::string_view what) {
    const std::uint64_t row = static_cast<std::uint64_t>(width) * 4;
    if (row != 0
        && static_cast<std::uint64_t>(height)
            > std::numeric_limits<std::uint64_t>::max() / row) {
        throw FormatError(std::string(what) + " dimensions overflow");
    }
    const std::uint64_t total = row * height;
    if (total > std::numeric_limits<std::size_t>::max()) {
        throw FormatError(std::string(what) + " is too large");
    }
    return static_cast<std::size_t>(total);
}

unsigned channel(std::byte value) {
    return std::to_integer<unsigned>(value);
}

std::byte to_byte(unsigned value) {
    return static_cast<std::byte>(value & 0xffU);
}

unsigned blend_channel(unsigned source, unsigned destination, unsigned alpha) {
    return (source * alpha + destination * (255U - alpha) + 127U) / 255U;
}

} // namespace

SoftwareRenderer::SoftwareRenderer(std::uint32_t width, std::uint32_t height)
    : width_(width), height_(height) {
    target_.width = width;
    target_.height = height;
    // Transparent black: the first opaque draw must report a change even when
    // its RGB is also black, because it makes the framebuffer opaque.
    target_.rgba.assign(checked_rgba_bytes(width, height, "framebuffer"), std::byte{0});
}

bool SoftwareRenderer::draw(const Image& source, std::int32_t x, std::int32_t y) {
    if (source.width == 0 || source.height == 0) {
        return false;
    }
    const std::size_t source_bytes =
        checked_rgba_bytes(source.width, source.height, "image");
    if (source.rgba.size() != source_bytes) {
        throw FormatError("image buffer size does not match its dimensions");
    }

    const std::int64_t origin_x = x;
    const std::int64_t origin_y = y;
    const std::int64_t source_width = source.width;
    const std::int64_t source_height = source.height;
    const std::int64_t viewport_width = width_;
    const std::int64_t viewport_height = height_;

    const std::int64_t dest_x_begin = std::max<std::int64_t>(0, origin_x);
    const std::int64_t dest_y_begin = std::max<std::int64_t>(0, origin_y);
    const std::int64_t dest_x_end =
        std::min<std::int64_t>(viewport_width, origin_x + source_width);
    const std::int64_t dest_y_end =
        std::min<std::int64_t>(viewport_height, origin_y + source_height);
    if (dest_x_end <= dest_x_begin || dest_y_end <= dest_y_begin) {
        return false;
    }

    const std::size_t source_x_begin =
        static_cast<std::size_t>(dest_x_begin - origin_x);
    const std::size_t source_y_begin =
        static_cast<std::size_t>(dest_y_begin - origin_y);
    const std::size_t copy_width =
        static_cast<std::size_t>(dest_x_end - dest_x_begin);

    bool changed = false;
    for (std::int64_t dest_y = dest_y_begin; dest_y < dest_y_end; ++dest_y) {
        const std::size_t source_row =
            (source_y_begin + static_cast<std::size_t>(dest_y - dest_y_begin))
            * source.width;
        const std::size_t destination_row =
            static_cast<std::size_t>(dest_y) * width_;
        std::size_t source_index =
            (source_row + source_x_begin) * 4;
        std::size_t destination_index =
            (destination_row + static_cast<std::size_t>(dest_x_begin)) * 4;
        for (std::size_t column = 0; column < copy_width; ++column) {
            const unsigned alpha = channel(source.rgba[source_index + 3]);
            if (alpha != 0) {
                const unsigned destination_alpha =
                    channel(target_.rgba[destination_index + 3]);
                const unsigned red = blend_channel(
                    channel(source.rgba[source_index]),
                    channel(target_.rgba[destination_index]), alpha);
                const unsigned green = blend_channel(
                    channel(source.rgba[source_index + 1]),
                    channel(target_.rgba[destination_index + 1]), alpha);
                const unsigned blue = blend_channel(
                    channel(source.rgba[source_index + 2]),
                    channel(target_.rgba[destination_index + 2]), alpha);
                if (destination_alpha != 255
                    || channel(target_.rgba[destination_index]) != red
                    || channel(target_.rgba[destination_index + 1]) != green
                    || channel(target_.rgba[destination_index + 2]) != blue) {
                    changed = true;
                }
                target_.rgba[destination_index] = to_byte(red);
                target_.rgba[destination_index + 1] = to_byte(green);
                target_.rgba[destination_index + 2] = to_byte(blue);
                target_.rgba[destination_index + 3] = to_byte(255);
            }
            source_index += 4;
            destination_index += 4;
        }
    }
    return changed;
}

} // namespace dhsh
