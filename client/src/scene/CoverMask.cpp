#include "scene/CoverMask.h"

#include "resource/ResourceError.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace dhsh {
namespace {

// ApplyMask modifies the sprite's 8-bit alpha channel, not its RGB: the
// original MMX helper (Game.exe 0x4264b0) runs psraw 3 + pand 0x1f1f on
// packed 16-bit lanes, which reduces each covered alpha byte to alpha >> 3
// (a faint ghost with max alpha 31). RGB and the mask coverage stay as-is.
bool fade_covered_alpha(Image& character, std::size_t pixel) {
    const unsigned alpha = std::to_integer<unsigned>(character.rgba[pixel + 3]);
    const unsigned faded = alpha >> 3;
    if (faded == alpha) {
        return false;
    }
    character.rgba[pixel + 3] = static_cast<std::byte>(faded);
    return true;
}

} // namespace

CoverMask::CoverMask(const Core& core, std::size_t placement_index)
    : placement_(core.placements.at(placement_index)),
      record_(core.records.at(placement_.record)),
      mask_(core.decode_mask(placement_.record)),
      sort_y_(core.sort_y(placement_index)) {
}

bool CoverMask::active() const noexcept {
    return placement_.flag != 0 || selected_;
}

void CoverMask::set_selected(bool selected) noexcept {
    selected_ = selected;
}

std::uint32_t CoverMask::group() const noexcept {
    return placement_.group;
}

std::uint32_t CoverMask::frame() const noexcept {
    return placement_.frame;
}

bool CoverMask::covers(std::int32_t x, std::int32_t y) const noexcept {
    if (!active()) {
        return false;
    }
    const std::int64_t boundary =
        ((static_cast<std::int64_t>(record_.slope) * x) >> 8) + sort_y_;
    return static_cast<std::int64_t>(y) < boundary;
}

std::optional<CoverMask::Intersection> CoverMask::sprite_intersection(
    const Image& character, std::int32_t left, std::int32_t top) const noexcept {
    if (character.width == 0 || character.height == 0) {
        return std::nullopt;
    }
    Intersection result{
        std::max<std::int64_t>(left, placement_.x),
        std::max<std::int64_t>(top, placement_.y),
        std::min<std::int64_t>(
            static_cast<std::int64_t>(left) + character.width,
            static_cast<std::int64_t>(placement_.x) + mask_.width),
        std::min<std::int64_t>(
            static_cast<std::int64_t>(top) + character.height,
            static_cast<std::int64_t>(placement_.y) + mask_.height),
    };
    if (result.right <= result.left || result.bottom <= result.top) {
        return std::nullopt;
    }
    return result;
}

bool CoverMask::intersects_sprite(const Image& character, std::int32_t left,
    std::int32_t top) const noexcept {
    return sprite_intersection(character, left, top).has_value();
}

bool CoverMask::apply(Image& character, std::int32_t x, std::int32_t y,
    std::int32_t left, std::int32_t top) const {
    if (!covers(x, y) || character.width == 0 || character.height == 0) {
        return false;
    }
    const std::size_t max_bytes = std::min(
        character.rgba.max_size(), std::numeric_limits<std::size_t>::max());
    if (character.width > max_bytes / 4) {
        throw FormatError("character image buffer size does not match its dimensions");
    }
    const std::size_t row_bytes = static_cast<std::size_t>(character.width) * 4;
    if (character.height > max_bytes / row_bytes) {
        throw FormatError("character image buffer size does not match its dimensions");
    }
    const std::size_t expected = row_bytes * character.height;
    if (character.rgba.size() != expected) {
        throw FormatError("character image buffer size does not match its dimensions");
    }

    const auto intersection = sprite_intersection(character, left, top);
    if (!intersection.has_value()) {
        return false;
    }

    bool changed = false;
    for (std::int64_t dest_y = intersection->top; dest_y < intersection->bottom;
         ++dest_y) {
        for (std::int64_t dest_x = intersection->left; dest_x < intersection->right;
             ++dest_x) {
            const std::size_t mask_offset = static_cast<std::size_t>(
                (dest_y - placement_.y) * mask_.width + dest_x - placement_.x);
            if (mask_.pixels[mask_offset] == 0) {
                continue;
            }
            const std::size_t pixel = static_cast<std::size_t>(
                (dest_y - top) * character.width + dest_x - left) * 4;
            changed = fade_covered_alpha(character, pixel) || changed;
        }
    }
    return changed;
}

} // namespace dhsh
