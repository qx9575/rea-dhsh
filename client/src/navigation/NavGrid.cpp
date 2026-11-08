#include "navigation/NavGrid.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace dhsh {
namespace {

std::int32_t clamp_i64(std::int64_t value) noexcept {
    return static_cast<std::int32_t>(std::clamp(value,
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()),
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
}

} // namespace

bool NavGrid::contains(NavCell cell) const noexcept {
    return cell.x >= 0 && cell.y >= 0
        && static_cast<std::uint32_t>(cell.x) < width()
        && static_cast<std::uint32_t>(cell.y) < height();
}

bool NavGrid::walkable(NavCell cell) const noexcept {
    return contains(cell) && !cel_->blocked(cell.x, cell.y);
}

NavCell NavGrid::cell_at_pixel(NavPixel pixel) const noexcept {
    const auto floor_cell = [](std::int32_t value) {
        const std::int64_t wide = value;
        return clamp_i64(wide >= 0 ? wide / 16 : (wide - 15) / 16);
    };
    return NavCell{floor_cell(pixel.x), floor_cell(pixel.y)};
}

NavPixel NavGrid::cell_center(NavCell cell) const noexcept {
    return NavPixel{
        clamp_i64(static_cast<std::int64_t>(cell.x) * 16 + 8),
        clamp_i64(static_cast<std::int64_t>(cell.y) * 16 + 8),
    };
}

} // namespace dhsh
