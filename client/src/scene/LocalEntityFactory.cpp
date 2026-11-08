#include "scene/LocalEntityFactory.h"

#include "avatar/Avatar.h"
#include "avatar/AvatarCatalog.h"
#include "resource/NzDecoder.h"
#include "resource/PakResourceProvider.h"
#include "resource/ResourceError.h"

#include <cstdint>
#include <exception>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace dhsh {
namespace {

constexpr std::string_view confirmed_spawn_target = "ytf_yamen.map";
constexpr std::string_view npc_resource_path = "10009/10/3.apg";

AnimationClip avatar_clip(AvatarAnimation animation) {
    std::vector<AnimationFrame> frames;
    frames.reserve(animation.frames.size());
    for (AvatarFrame& source : animation.frames) {
        frames.push_back(AnimationFrame{
            std::move(source.image), -320, -240, source.duration_ms});
    }
    return AnimationClip(std::move(frames));
}

} // namespace

LocalSpawn find_local_spawn(const MapScene& scene) {
    for (const MlkLink& link : scene.mlk().links) {
        if (PakResourceProvider::normalize_logical_path(link.target)
            != confirmed_spawn_target) {
            continue;
        }
        const std::optional<Direction> direction = direction_from_mlk(link.extra0);
        if (!direction.has_value()) {
            throw ResourceError(
                "confirmed YTF_YaMen.map spawn has an invalid facing");
        }
        const EntityPosition position = spawn_from_mlk(link);
        if (scene.cel().blocked(cell_from_pixel(position.x),
                cell_from_pixel(position.y))) {
            throw ResourceError(
                "confirmed YTF_YaMen.map spawn is not on a walkable cell");
        }
        return LocalSpawn{position, *direction};
    }
    throw ResourceError("map has no confirmed YTF_YaMen.map spawn");
}

Character make_local_character(ResourceProvider& resources,
    const AvatarCatalog& catalog, const LocalSpawn& spawn) {
    const auto avatar = std::make_shared<Avatar>(resources, catalog);
    const AvatarPreset preset = AvatarCatalog::default_preset();
    const auto load = [avatar, preset](EntityState state, Direction direction) {
        const std::uint32_t action = state == EntityState::idle ? 10U : 30U;
        const std::uint32_t resource_direction = static_cast<std::uint32_t>(direction);
        AvatarAnimation assembled = avatar->assemble(
            preset, action, resource_direction);
        if (assembled.direction != resource_direction || assembled.frames.empty()) {
            throw ResourceError(
                "real avatar is missing a requested directional animation");
        }
        return avatar_clip(std::move(assembled));
    };

    Character character(spawn.position, spawn.direction);
    character.set_animation(EntityState::idle, spawn.direction,
        load(EntityState::idle, spawn.direction));
    character.set_animation(EntityState::walk, spawn.direction,
        load(EntityState::walk, spawn.direction));
    character.set_animation_loader(load);
    return character;
}

Entity make_local_npc(ResourceProvider& resources) {
    Entity npc({7000, 2152}, Direction::right_down);
    npc.set_animation(EntityState::idle, Direction::right_down,
        AnimationClip::decode(NzDecoder::decode(resources.bytes(npc_resource_path))));
    if (npc.current_frame() == nullptr) {
        throw ResourceError("local NPC animation has no frames: "
            + std::string(npc_resource_path));
    }
    return npc;
}

bool add_local_npc_if_available(MapScene& scene,
    ResourceProvider& resources, std::ostream& diagnostics) {
    try {
        scene.add_npc(make_local_npc(resources));
        return true;
    } catch (const std::exception& error) {
        diagnostics << "Skipped local NPC " << npc_resource_path
                    << ": " << error.what() << '\n';
        return false;
    }
}

bool add_local_npc_if_available(MapScene& scene,
    const std::filesystem::path& shdata, std::ostream& diagnostics) {
    try {
        PakResourceProvider resources(shdata, {"npc.pak"});
        return add_local_npc_if_available(scene, resources, diagnostics);
    } catch (const std::exception& error) {
        diagnostics << "Skipped local NPC " << npc_resource_path
                    << ": " << error.what() << '\n';
        return false;
    }
}

} // namespace dhsh
