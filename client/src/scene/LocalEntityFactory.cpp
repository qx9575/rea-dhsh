#include "scene/LocalEntityFactory.h"

#include "avatar/Avatar.h"
#include "avatar/AvatarCatalog.h"
#include "resource/NzDecoder.h"
#include "resource/PakResourceProvider.h"
#include "resource/ResourceError.h"
#include "resource/ResourceProvider.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <exception>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dhsh {
namespace {

constexpr std::string_view confirmed_spawn_target = "ytf_yamen.map";

AnimationClip avatar_clip(AvatarAnimation animation) {
    std::vector<AnimationFrame> frames;
    frames.reserve(animation.frames.size());
    for (AvatarFrame& source : animation.frames) {
        frames.push_back(AnimationFrame{
            std::move(source.image), 320, 240, source.duration_ms});
    }
    return AnimationClip(std::move(frames));
}

AnimationClip load_npc_clip(ResourceProvider& resources, std::uint32_t shape,
    EntityState state, Direction direction, std::ostream* diagnostics) {
    const std::uint32_t action = state == EntityState::idle ? 10U : 30U;
    const std::uint32_t requested = static_cast<std::uint32_t>(direction);
    const std::uint32_t candidates[] = {
        requested, 3U, 1U, 2U, 0U, 4U, 5U, 6U, 7U};
    for (std::size_t index = 0; index < std::size(candidates); ++index) {
        const std::uint32_t candidate = candidates[index];
        if (index != 0 && candidate == requested) {
            continue;
        }
        const std::string path = std::to_string(shape) + "/"
            + std::to_string(action) + "/" + std::to_string(candidate) + ".apg";
        if (!resources.exists(path)) {
            continue;
        }
        try {
            ApgAnimation decoded = ApgDecoder::decode(
                NzDecoder::decode(resources.bytes(path)));
            std::vector<AnimationFrame> frames;
            frames.reserve(decoded.frames.size());
            for (ApgFrame& source : decoded.frames) {
                frames.push_back(AnimationFrame{
                    std::move(source.image),
                    160 - source.anchor_x,
                    210 - source.anchor_y,
                    80,
                });
            }
            AnimationClip clip(std::move(frames));
            if (!clip.frames().empty()) {
                if (diagnostics != nullptr) {
                    *diagnostics << "NPC resource shape=" << shape
                                 << " path=" << path << '\n';
                }
                return clip;
            }
        } catch (const std::exception&) {
            // A bad directional resource must not hide the other directions.
        }
    }
    if (diagnostics != nullptr) {
        *diagnostics << "NPC resource unavailable shape=" << shape
                     << " action=" << action << " direction=" << requested << '\n';
    }
    return {};
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

Entity make_network_npc(const WorldObject& object, ResourceProvider& resources,
    std::ostream* diagnostics) {
    if (!object.shape.has_value()) {
        throw ResourceError("network NPC has no replicated shape");
    }
    const std::uint32_t shape = *object.shape;
    Entity npc(object.position, object.direction);
    npc.set_animation_loader(
        [&resources, shape, diagnostics](EntityState state, Direction direction) {
            return load_npc_clip(resources, shape, state, direction, diagnostics);
        });
    return npc;
}

} // namespace dhsh
