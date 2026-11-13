#pragma once

#include "scene/Character.h"
#include "scene/MapScene.h"
#include "scene/WorldObjectStore.h"

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

// Builds a replicated NPC directly from its network shape and transform.
// Directional APG fallback stays inside the supplied shape; no other model is
// substituted. When supplied, diagnostics records the actual resource path.
Entity make_network_npc(const WorldObject& object, ResourceProvider& resources,
    std::ostream* diagnostics = nullptr);

} // namespace dhsh
