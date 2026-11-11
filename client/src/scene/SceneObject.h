#pragma once

#include "animation/AnimationClip.h"
#include "formats/MapFormats.h"
#include "render/Image.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dhsh {

struct MapObjectFrame {
    Image image;
    std::int32_t offset_x = 0;
    std::int32_t offset_y = 0;
    std::uint32_t duration_ms = 80;
};

using SceneAnimation = std::vector<MapObjectFrame>;
using SharedSceneAnimation = std::shared_ptr<const SceneAnimation>;

class SceneObject {
public:
    SceneObject(ManEntry entry, MapObjectFrame frame);
    SceneObject(ManEntry entry, SceneAnimation frames);
    SceneObject(ManEntry entry, SharedSceneAnimation frames);
    SceneObject(ManEntry entry, AnimationClip clip);

    ManEntry entry;
    std::int32_t x = 0;
    std::int32_t y = 0;
    MapObjectFrame frame;

    static std::int64_t sort_y(const ManEntry& entry, std::int32_t arg) noexcept;
    std::int64_t sort_y(std::int32_t arg) const noexcept;
    static void sort_by_depth(
        std::vector<SceneObject>& objects, std::int32_t arg = 0);

    bool advance(std::uint32_t milliseconds);
    std::size_t current_frame_index() const noexcept;
    const SharedSceneAnimation& animation() const noexcept;

private:
    void select_frame(std::size_t index);

    SharedSceneAnimation frames_;
    std::size_t current_frame_ = 0;
    std::uint32_t elapsed_ms_ = 0;
};

} // namespace dhsh
