#pragma once

#include "formats/MapFormats.h"
#include "render/Image.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace dhsh {

class CoverMask {
public:
    CoverMask(const Core& core, std::size_t placement_index);

    bool active() const noexcept;
    void set_selected(bool selected) noexcept;
    std::uint32_t group() const noexcept;
    std::uint32_t frame() const noexcept;

    bool covers(std::int32_t x, std::int32_t y) const noexcept;
    bool intersects_sprite(const Image& character, std::int32_t left,
        std::int32_t top) const noexcept;
    // Original ApplyMask ghost: covered sprite pixels become alpha >> 3
    // while RGB is preserved.
    bool apply(Image& character, std::int32_t x, std::int32_t y,
        std::int32_t left, std::int32_t top) const;

private:
    struct Intersection {
        std::int64_t left;
        std::int64_t top;
        std::int64_t right;
        std::int64_t bottom;
    };

    std::optional<Intersection> sprite_intersection(const Image& character,
        std::int32_t left, std::int32_t top) const noexcept;

    CorePlacement placement_;
    CoreRecord record_;
    CoreMask mask_;
    std::int64_t sort_y_ = 0;
    bool selected_ = false;
};

} // namespace dhsh
