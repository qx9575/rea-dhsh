#include "render/SoftwareRenderer.h"

#include "resource/ResourceError.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
    return draw_impl(source, x, y, false);
}

bool SoftwareRenderer::draw_opaque(
    const Image& source, std::int32_t x, std::int32_t y) {
    return draw_impl(source, x, y, true);
}

bool SoftwareRenderer::draw_ellipse_ring(double center_x, double center_y,
    double radius_x, double radius_y, std::uint8_t alpha) {
    if (!(radius_x > 0.0) || !(radius_y > 0.0) || alpha == 0
        || !std::isfinite(center_x) || !std::isfinite(center_y)) {
        return false;
    }
    const std::int64_t x_begin = std::max<std::int64_t>(0,
        static_cast<std::int64_t>(std::floor(center_x - radius_x - 1.0)));
    const std::int64_t y_begin = std::max<std::int64_t>(0,
        static_cast<std::int64_t>(std::floor(center_y - radius_y - 1.0)));
    const std::int64_t x_end = std::min<std::int64_t>(width_,
        static_cast<std::int64_t>(std::ceil(center_x + radius_x + 1.0)));
    const std::int64_t y_end = std::min<std::int64_t>(height_,
        static_cast<std::int64_t>(std::ceil(center_y + radius_y + 1.0)));
    const unsigned outline_alpha = alpha;
    const unsigned inner_alpha = (static_cast<unsigned>(alpha) * 3U) / 4U;
    bool changed = false;
    for (std::int64_t y = y_begin; y < y_end; ++y) {
        const double dy = (static_cast<double>(y) + 0.5 - center_y) / radius_y;
        for (std::int64_t x = x_begin; x < x_end; ++x) {
            const double dx = (static_cast<double>(x) + 0.5 - center_x) / radius_x;
            const double distance = dx * dx + dy * dy;
            if (distance > 1.0 || distance < 0.60) {
                continue;
            }
            const bool outline = distance >= 0.78;
            const unsigned a = outline ? outline_alpha : inner_alpha;
            if (a == 0) {
                continue;
            }
            const std::size_t index =
                (static_cast<std::size_t>(y) * width_
                    + static_cast<std::size_t>(x))
                * 4;
            const unsigned source_red = 255U;
            const unsigned source_green = 255U;
            const unsigned source_blue = 255U;
            const unsigned red = blend_channel(source_red,
                channel(target_.rgba[index]), a);
            const unsigned green = blend_channel(source_green,
                channel(target_.rgba[index + 1]), a);
            const unsigned blue = blend_channel(source_blue,
                channel(target_.rgba[index + 2]), a);
            if (channel(target_.rgba[index + 3]) != 255
                || channel(target_.rgba[index]) != red
                || channel(target_.rgba[index + 1]) != green
                || channel(target_.rgba[index + 2]) != blue) {
                changed = true;
            }
            target_.rgba[index] = to_byte(red);
            target_.rgba[index + 1] = to_byte(green);
            target_.rgba[index + 2] = to_byte(blue);
            target_.rgba[index + 3] = std::byte{0xff};
        }
    }
    return changed;
}

bool SoftwareRenderer::draw_star(double center_x, double center_y,
    double radius, std::uint8_t alpha) {
    if (!(radius > 0.0) || alpha == 0 || !std::isfinite(center_x)
        || !std::isfinite(center_y)) {
        return false;
    }
    constexpr double pi = 3.14159265358979323846;
    bool changed = false;
    const auto pass = [&](double pass_radius, unsigned source_red,
                          unsigned source_green, unsigned source_blue,
                          unsigned pass_alpha) {
        if (pass_alpha == 0) {
            return;
        }
        std::array<double, 10> xs{};
        std::array<double, 10> ys{};
        for (int index = 0; index < 10; ++index) {
            const double angle =
                -pi / 2.0 + static_cast<double>(index) * pi / 5.0;
            const double point_radius =
                (index % 2 == 0) ? pass_radius : pass_radius * 0.42;
            xs[index] = center_x + std::cos(angle) * point_radius;
            ys[index] = center_y + std::sin(angle) * point_radius;
        }
        const double bound = pass_radius * 1.2 + 1.0;
        const std::int64_t x_begin = std::max<std::int64_t>(0,
            static_cast<std::int64_t>(std::floor(center_x - bound)));
        const std::int64_t y_begin = std::max<std::int64_t>(0,
            static_cast<std::int64_t>(std::floor(center_y - bound)));
        const std::int64_t x_end = std::min<std::int64_t>(width_,
            static_cast<std::int64_t>(std::ceil(center_x + bound)));
        const std::int64_t y_end = std::min<std::int64_t>(height_,
            static_cast<std::int64_t>(std::ceil(center_y + bound)));
        for (std::int64_t y = y_begin; y < y_end; ++y) {
            const double py = static_cast<double>(y) + 0.5;
            for (std::int64_t x = x_begin; x < x_end; ++x) {
                const double px = static_cast<double>(x) + 0.5;
                bool inside = false;
                for (int index = 0, previous = 9; index < 10;
                     previous = index++) {
                    if (((ys[index] > py) != (ys[previous] > py))
                        && (px < (xs[previous] - xs[index])
                                * (py - ys[index])
                                / (ys[previous] - ys[index])
                            + xs[index])) {
                        inside = !inside;
                    }
                }
                if (!inside) {
                    continue;
                }
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * width_
                        + static_cast<std::size_t>(x))
                    * 4;
                const unsigned red = blend_channel(source_red,
                    channel(target_.rgba[offset]), pass_alpha);
                const unsigned green = blend_channel(source_green,
                    channel(target_.rgba[offset + 1]), pass_alpha);
                const unsigned blue = blend_channel(source_blue,
                    channel(target_.rgba[offset + 2]), pass_alpha);
                if (channel(target_.rgba[offset + 3]) != 255
                    || channel(target_.rgba[offset]) != red
                    || channel(target_.rgba[offset + 1]) != green
                    || channel(target_.rgba[offset + 2]) != blue) {
                    changed = true;
                }
                target_.rgba[offset] = to_byte(red);
                target_.rgba[offset + 1] = to_byte(green);
                target_.rgba[offset + 2] = to_byte(blue);
                target_.rgba[offset + 3] = std::byte{0xff};
            }
        }
    };
    pass(radius * 1.5, 120U, 190U, 255U,
        (static_cast<unsigned>(alpha) * 35U) / 100U);
    pass(radius, 255U, 255U, 255U, alpha);
    return changed;
}

bool SoftwareRenderer::draw_impl(const Image& source, std::int32_t x,
    std::int32_t y, bool source_opaque) {
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
        bool opaque_row = source_opaque;
        bool transparent_row = false;
        if (!source_opaque) {
            opaque_row = true;
            transparent_row = true;
            for (std::size_t column = 0; column < copy_width; ++column) {
                const std::byte alpha = source.rgba[source_index + column * 4 + 3];
                if (alpha != std::byte{0}) {
                    transparent_row = false;
                }
                if (alpha != std::byte{0xff}) {
                    opaque_row = false;
                }
                if (!opaque_row && !transparent_row) {
                    break;
                }
            }
        }
        if (transparent_row) {
            continue;
        }
        if (opaque_row) {
            const std::size_t row_bytes = copy_width * 4;
            if (std::memcmp(target_.rgba.data() + destination_index,
                    source.rgba.data() + source_index, row_bytes) != 0) {
                changed = true;
            }
            std::memcpy(target_.rgba.data() + destination_index,
                source.rgba.data() + source_index, row_bytes);
            continue;
        }
        for (std::size_t column = 0; column < copy_width; ++column) {
            const unsigned alpha = channel(source.rgba[source_index + 3]);
            if (alpha == 255) {
                if (channel(target_.rgba[destination_index + 3]) != 255
                    || target_.rgba[destination_index] != source.rgba[source_index]
                    || target_.rgba[destination_index + 1] != source.rgba[source_index + 1]
                    || target_.rgba[destination_index + 2] != source.rgba[source_index + 2]) {
                    changed = true;
                }
                target_.rgba[destination_index] = source.rgba[source_index];
                target_.rgba[destination_index + 1] = source.rgba[source_index + 1];
                target_.rgba[destination_index + 2] = source.rgba[source_index + 2];
                target_.rgba[destination_index + 3] = std::byte{0xff};
            } else if (alpha != 0) {
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
