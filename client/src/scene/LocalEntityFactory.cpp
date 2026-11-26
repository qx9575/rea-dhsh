#include "scene/LocalEntityFactory.h"

#include "avatar/Avatar.h"
#include "avatar/AvatarCatalog.h"
#include "resource/NzDecoder.h"
#include "resource/PakResourceProvider.h"
#include "resource/ResourceError.h"
#include "resource/ResourceProvider.h"
#include "scene/NpcRoster.h"

#include <cstddef>
#include <cstdint>
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

std::string read_config_text(
    ResourceProvider& resources, std::string_view logical_path) {
    std::vector<std::byte> stored = resources.bytes(logical_path);
    if (stored.size() >= 2 && stored[0] == std::byte{'N'}
        && stored[1] == std::byte{'Z'}) {
        stored = NzDecoder::decode(stored);
    }
    return std::string(reinterpret_cast<const char*>(stored.data()), stored.size());
}

AnimationClip load_npc_clip(ResourceProvider& resources, std::uint32_t shape,
    EntityState state, Direction direction) {
    const std::uint32_t action = state == EntityState::idle ? 10U : 30U;
    const std::uint32_t requested = static_cast<std::uint32_t>(direction);
    const std::uint32_t candidates[] = {
        requested, 3U, 1U, 2U, 0U, 4U, 5U, 6U, 7U};
    for (const std::uint32_t candidate : candidates) {
        const std::string path = std::to_string(shape) + "/"
            + std::to_string(action) + "/" + std::to_string(candidate) + ".apg";
        if (!resources.exists(path)) {
            continue;
        }
        try {
            AnimationClip clip = AnimationClip::decode(
                NzDecoder::decode(resources.bytes(path)));
            if (!clip.frames().empty()) {
                return clip;
            }
        } catch (const std::exception&) {
            // Try the next direction; a single missing APG must not hide the
            // rest of the configured NPC roster.
        }
    }
    return {};
}

std::size_t add_npcs_impl(MapScene& scene, ResourceProvider& config_resources,
    ResourceProvider& npc_resources, std::string_view map_file,
    bool require_live_hint, std::ostream& diagnostics) {
    const std::string text = read_config_text(config_resources, "misc/npc.csv");
    const std::vector<NpcSpawn> spawns = load_npc_roster(text, map_file);
    if (spawns.empty()) {
        diagnostics << "NPC roster for " << map_file
                    << " is empty or misc/npc.csv is missing\n";
        return 0;
    }
    std::size_t added = 0;
    std::size_t failures = 0;
    std::size_t filtered = 0;
    for (const NpcSpawn& spawn : spawns) {
        if (require_live_hint && !npc_has_live_hint(spawn)) {
            ++filtered;
            continue;
        }
        try {
            bool hint_matched = false;
            const std::uint32_t shape =
                resolve_npc_model(spawn, npc_resources, &hint_matched);
            Entity npc(
                {spawn.cell_x * 16 + 8, spawn.cell_y * 16 + 8},
                npc_hint_direction(spawn));
            npc.set_animation_loader(
                [&npc_resources, shape](EntityState state, Direction direction) {
                    return load_npc_clip(npc_resources, shape, state, direction);
                });
            scene.add_npc(std::move(npc));
            ++added;
            if (!hint_matched && failures < 8) {
                diagnostics << "NPC " << spawn.id << ' ' << spawn.name
                            << " uses fallback model " << shape << '\n';
            }
        } catch (const std::exception& error) {
            ++failures;
            if (failures <= 8) {
                diagnostics << "Skipped NPC " << spawn.id << ' ' << spawn.name
                            << ": " << error.what() << '\n';
            }
        }
    }
    diagnostics << "NPC roster " << map_file << ": configured=" << spawns.size()
                << " added=" << added << " failed=" << failures << '\n';
    return added;
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

std::size_t add_local_npcs_if_available(MapScene& scene,
    ResourceProvider& config_resources, ResourceProvider& npc_resources,
    std::string_view map_file, bool require_live_hint,
    std::ostream& diagnostics) {
    try {
        return add_npcs_impl(scene, config_resources, npc_resources, map_file,
            require_live_hint, diagnostics);
    } catch (const std::exception& error) {
        diagnostics << "Skipped NPC roster " << map_file << ": "
                    << error.what() << '\n';
        return 0;
    }
}

} // namespace dhsh
