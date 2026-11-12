#pragma once

#include "network/DecodedMessage.h"
#include "scene/WorldObjectStore.h"

#include <cstdint>
#include <variant>

namespace dhsh {

struct ObjectUpsert {
    WorldObject object;
};

struct ObjectRemove {
    std::uint32_t object_id = 0;
};

using NpcSyncEvent = std::variant<std::monostate, ObjectUpsert, ObjectRemove>;

class NpcSyncDecoder {
public:
    NpcSyncEvent decode(const DecodedMessage& message) const;
};

} // namespace dhsh
