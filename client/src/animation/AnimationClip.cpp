#include "animation/AnimationClip.h"

#include "formats/Apg.h"
#include "formats/Pif.h"

#include <cstdint>
#include <utility>

namespace dhsh {

AnimationClip AnimationClip::decode(std::span<const std::byte> apg,
    const CtiPalette* palette, const Pif* pif) {
    ApgAnimation decoded = ApgDecoder::decode(apg, palette);
    const bool use_pif = pif != nullptr && pif->rows.size() == decoded.frames.size();
    AnimationClip result;
    result.frames_.reserve(decoded.frames.size());
    for (std::size_t index = 0; index < decoded.frames.size(); ++index) {
        ApgFrame& source = decoded.frames[index];
        const std::uint32_t duration = use_pif ? pif->rows[index][5] : 0;
        result.frames_.push_back(AnimationFrame{
            std::move(source.image),
            source.anchor_x,
            source.anchor_y,
            duration == 0 ? 80U : duration,
        });
    }
    return result;
}

const std::vector<AnimationFrame>& AnimationClip::frames() const noexcept {
    return frames_;
}

std::size_t AnimationClip::current_frame_index() const noexcept {
    return current_frame_;
}

void AnimationClip::advance(std::uint32_t milliseconds) {
    if (frames_.empty()) {
        return;
    }
    std::uint64_t total_ms = 0;
    std::uint64_t position = elapsed_ms_;
    for (std::size_t index = 0; index < frames_.size(); ++index) {
        total_ms += frames_[index].duration_ms;
        if (index < current_frame_) {
            position += frames_[index].duration_ms;
        }
    }
    position = (position + milliseconds) % total_ms;
    current_frame_ = 0;
    while (position >= frames_[current_frame_].duration_ms) {
        position -= frames_[current_frame_].duration_ms;
        ++current_frame_;
    }
    elapsed_ms_ = static_cast<std::uint32_t>(position);
}

} // namespace dhsh
