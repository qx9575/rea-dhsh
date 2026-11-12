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

    // Same clipping and change reporting as draw(), but skips alpha
    // classification. The caller must have verified every source alpha is 255.
    bool draw_opaque(const Image& source, std::int32_t x, std::int32_t y);

    // Draws the click-marker ring: a white/blue elliptical annulus with a
    // glowing inner band. The centre hole stays transparent. Returns true
    // when any destination pixel changed.
    bool draw_ellipse_ring(double center_x, double center_y, double radius_x,
        double radius_y, std::uint8_t alpha);

    // Draws a five-pointed star with a soft blue halo. Used as the short
    // flash at the click point; the ring keeps expanding after it fades.
    bool draw_star(double center_x, double center_y, double radius,
        std::uint8_t alpha);

private:
    bool draw_impl(const Image& source, std::int32_t x, std::int32_t y,
        bool source_opaque);

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    Image target_;
};

} // namespace dhsh
