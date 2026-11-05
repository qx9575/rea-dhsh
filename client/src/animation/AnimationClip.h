#pragma once

#include "formats/Apg.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dhsh {

struct CtiPalette;
struct Pif;

struct AnimationFrame {
    Image image;
    std::int16_t anchor_x = 0;
    std::int16_t anchor_y = 0;
    std::uint32_t duration_ms = 80;
};

class AnimationClip {
public:
    static AnimationClip decode(std::span<const std::byte> apg,
        const CtiPalette* palette = nullptr, const Pif* pif = nullptr,
        ApgDecodeOptions options = {});

    const std::vector<AnimationFrame>& frames() const noexcept;
    std::size_t current_frame_index() const noexcept;
    void advance(std::uint32_t milliseconds);

private:
    std::vector<AnimationFrame> frames_;
    std::size_t current_frame_ = 0;
    std::uint32_t elapsed_ms_ = 0;
};

} // namespace dhsh
