#pragma once

#include "formats/MapFormats.h"

#include <cstdint>

namespace dhsh {

struct NavCell {
    std::int32_t x = 0;
    std::int32_t y = 0;

    bool operator==(const NavCell&) const = default;
};

struct NavPixel {
    std::int32_t x = 0;
    std::int32_t y = 0;

    bool operator==(const NavPixel&) const = default;
};

class NavGrid {
public:
    explicit NavGrid(const Cel& cel) noexcept : cel_(&cel) {
    }

    std::uint32_t width() const noexcept { return cel_->width; }
    std::uint32_t height() const noexcept { return cel_->height; }
    bool contains(NavCell cell) const noexcept;
    bool walkable(NavCell cell) const noexcept;
    NavCell cell_at_pixel(NavPixel pixel) const noexcept;
    NavPixel cell_center(NavCell cell) const noexcept;

private:
    const Cel* cel_;
};

} // namespace dhsh
