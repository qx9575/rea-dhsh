#pragma once

#include "scene/Entity.h"

namespace dhsh {

class Character final : public Entity {
public:
    explicit Character(EntityPosition position,
        Direction direction = Direction::down);
};

} // namespace dhsh
