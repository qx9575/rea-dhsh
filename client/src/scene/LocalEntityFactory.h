#pragma once

#include "scene/Character.h"
#include "scene/MapScene.h"

#include <filesystem>
#include <iosfwd>

namespace dhsh {

class AvatarCatalog;
class ResourceProvider;

struct LocalSpawn {
    EntityPosition position;
    Direction direction;
};

// Selects only the confirmed YTF_YaMen return link. An absent or invalid
// explicit preset is fatal; another map link is never a substitute spawn.
LocalSpawn find_local_spawn(const MapScene& scene);

// Preloads the spawn direction and installs a fixed sixteen-slot lazy cache
// for the remaining Idle/Walk directions.
Character make_local_character(ResourceProvider& resources,
    const AvatarCatalog& catalog, const LocalSpawn& spawn);

Entity make_local_npc(ResourceProvider& resources);

// A bad NPC frame only skips the explicit local NPC preset and leaves all
// existing scene entities untouched.
bool add_local_npc_if_available(MapScene& scene,
    ResourceProvider& resources, std::ostream& diagnostics);

// Mounts npc.pak independently so a missing/corrupt archive has the same
// nonfatal behavior as a bad frame from an already mounted provider.
bool add_local_npc_if_available(MapScene& scene,
    const std::filesystem::path& shdata, std::ostream& diagnostics);

} // namespace dhsh
