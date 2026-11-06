#pragma once

#include "render/Image.h"

#include <cstdint>

namespace dhsh {

// CPU framebuffer used for the M1 static scene. Pixels are tightly packed
// RGBA8; the framebuffer itself is always opaque once anything is drawn.
class SoftwareRenderer {
public:
    SoftwareRenderer(std::uint32_t width, std::uint32_t height);

    std::uint32_t width() const noexcept {
        return width_;
    }

    std::uint32_t height() const noexcept {
        return height_;
    }

    const Image& target() const noexcept {
        return target_;
    }

    // Source-over blend:
    //   rgb = (src * alpha + dst * (255 - alpha) + 127) / 255
    // Fully transparent source pixels leave the destination untouched.
    // Returns true only when at least one destination pixel changed.
    // Placements outside the viewport are clipped; negative and extreme
    // 32-bit coordinates are safe.
    bool draw(const Image& source, std::int32_t x, std::int32_t y);

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    Image target_;
};

} // namespace dhsh
