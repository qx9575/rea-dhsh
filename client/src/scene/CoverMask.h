#pragma once

#include "formats/MapFormats.h"
#include "render/Image.h"

#include <cstddef>
#include <cstdint>

namespace dhsh {

class CoverMask {
public:
    CoverMask(const Core& core, std::size_t placement_index);

    bool active() const noexcept;
    void set_selected(bool selected) noexcept;
    std::uint32_t group() const noexcept;
    std::uint32_t frame() const noexcept;

    bool covers(std::int32_t x, std::int32_t y) const noexcept;
    bool apply(Image& character, std::int32_t x, std::int32_t y) const;

private:
    CorePlacement placement_;
    CoreRecord record_;
    CoreMask mask_;
    std::int64_t sort_y_ = 0;
    bool selected_ = false;
};

} // namespace dhsh
