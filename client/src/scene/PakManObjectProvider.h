#pragma once

#include "scene/MapScene.h"

#include <string>
#include <unordered_map>

namespace dhsh {

class PakManObjectProvider final : public ManObjectProvider {
public:
    explicit PakManObjectProvider(ResourceProvider& resources);

    std::optional<MapObjectFrame> load_first_frame(const ManEntry& entry) override;
    SharedSceneAnimation load_animation(const ManEntry& entry) override;

private:
    std::optional<std::string> numbered_a16(
        std::string_view directory, std::int32_t index);
    MapObjectFrame load_frame(const std::string& asset);
    void load_delays(std::string_view directory, SceneAnimation& frames);
    SharedSceneAnimation load(std::string_view man_path);

    ResourceProvider& resources_;
    std::unordered_map<std::string, SharedSceneAnimation> cache_;
};

} // namespace dhsh
