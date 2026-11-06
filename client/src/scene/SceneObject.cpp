#include "scene/SceneObject.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace dhsh {
namespace {

std::int32_t clamp_add(std::int32_t left, std::int32_t right) {
    const std::int64_t result = static_cast<std::int64_t>(left) + right;
    if (result < std::numeric_limits<std::int32_t>::min()) {
        return std::numeric_limits<std::int32_t>::min();
    }
    if (result > std::numeric_limits<std::int32_t>::max()) {
        return std::numeric_limits<std::int32_t>::max();
    }
    return static_cast<std::int32_t>(result);
}

} // namespace

SceneObject::SceneObject(ManEntry value, MapObjectFrame first)
    : SceneObject(std::move(value), std::vector<MapObjectFrame>{std::move(first)}) {
}

SceneObject::SceneObject(ManEntry value, std::vector<MapObjectFrame> frames)
    : entry(std::move(value)), frames_(std::move(frames)) {
    select_frame(0);
}

SceneObject::SceneObject(ManEntry value, AnimationClip clip)
    : entry(std::move(value)) {
    frames_.reserve(clip.frames().size());
    for (const AnimationFrame& source : clip.frames()) {
        frames_.push_back(MapObjectFrame{
            source.image,
            source.anchor_x,
            source.anchor_y,
            source.duration_ms,
        });
    }
    select_frame(clip.current_frame_index());
}

std::int64_t SceneObject::sort_y(const ManEntry& entry, std::int32_t arg) noexcept {
    const std::int64_t product = static_cast<std::int64_t>(entry.f58) * arg;
    return static_cast<std::int64_t>(entry.y) + entry.f48 + (product >> 8);
}

std::int64_t SceneObject::sort_y(std::int32_t arg) const noexcept {
    return sort_y(entry, arg);
}

void SceneObject::sort_by_depth(
    std::vector<SceneObject>& objects, std::int32_t arg) {
    std::stable_sort(objects.begin(), objects.end(),
        [arg](const SceneObject& left, const SceneObject& right) {
            return left.sort_y(arg) < right.sort_y(arg);
        });
}

bool SceneObject::advance(std::uint32_t milliseconds) {
    if (frames_.size() < 2) {
        return false;
    }
    std::uint64_t total_ms = 0;
    std::uint64_t position = elapsed_ms_;
    for (std::size_t index = 0; index < frames_.size(); ++index) {
        const std::uint32_t duration = frames_[index].duration_ms == 0
            ? 80U : frames_[index].duration_ms;
        total_ms += duration;
        if (index < current_frame_) {
            position += duration;
        }
    }
    position = (position + milliseconds) % total_ms;
    std::size_t next = 0;
    while (true) {
        const std::uint32_t duration = frames_[next].duration_ms == 0
            ? 80U : frames_[next].duration_ms;
        if (position < duration) {
            break;
        }
        position -= duration;
        ++next;
    }
    elapsed_ms_ = static_cast<std::uint32_t>(position);
    if (next == current_frame_) {
        return false;
    }
    select_frame(next);
    return true;
}

std::size_t SceneObject::current_frame_index() const noexcept {
    return current_frame_;
}

void SceneObject::select_frame(std::size_t index) {
    if (frames_.empty()) {
        frame = {};
        x = entry.x;
        y = entry.y;
        current_frame_ = 0;
        elapsed_ms_ = 0;
        return;
    }
    current_frame_ = std::min(index, frames_.size() - 1);
    frame = frames_[current_frame_];
    x = clamp_add(entry.x, frame.offset_x);
    y = clamp_add(entry.y, frame.offset_y);
}

} // namespace dhsh
