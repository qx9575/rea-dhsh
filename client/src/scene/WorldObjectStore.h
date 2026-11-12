#pragma once

#include "scene/Entity.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace dhsh {

enum class WorldObjectKind : std::uint8_t {
    npc = 0,
    player = 1,
    other = 2,
};

struct WorldObject {
    std::uint32_t object_id = 0;
    WorldObjectKind kind = WorldObjectKind::other;
    std::optional<std::uint32_t> static_npc_id;
    std::optional<std::uint32_t> shape;
    std::string name; // UTF-8
    EntityPosition position;
    Direction direction = Direction::down;
    std::uint64_t revision = 0;
};

class WorldObjectStore {
public:
    // Returns true only when a field affecting the rendered object changes.
    bool upsert(const WorldObject& object);
    bool remove(std::uint32_t object_id) noexcept;

    const WorldObject* find(std::uint32_t object_id) const noexcept;
    std::size_t size() const noexcept;

private:
    std::map<std::uint32_t, WorldObject> objects_;
};

} // namespace dhsh
