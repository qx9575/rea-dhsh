#pragma once

#include "animation/AnimationClip.h"
#include "formats/MapFormats.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace dhsh {

enum class EntityState : std::uint8_t {
    idle = 0,
    walk = 1,
};

enum class Direction : std::uint8_t {
    left = 0,
    left_down = 1,
    down = 2,
    right_down = 3,
    right = 4,
    right_up = 5,
    up = 6,
    left_up = 7,
};

struct EntityPosition {
    std::int32_t x = 0;
    std::int32_t y = 0;

    bool operator==(const EntityPosition&) const = default;
};

std::optional<Direction> direction_from_vector(
    std::int32_t x, std::int32_t y) noexcept;
std::optional<Direction> direction_from_mlk(std::int32_t extra0) noexcept;
EntityPosition spawn_from_mlk(const MlkLink& link) noexcept;

class Entity {
public:
    explicit Entity(EntityPosition position,
        Direction direction = Direction::down);

    void set_position(EntityPosition position) noexcept;
    void move_by(std::int32_t x, std::int32_t y) noexcept;
    void stop() noexcept;

    void set_animation(EntityState state, Direction direction, AnimationClip clip);
    bool advance(std::uint32_t milliseconds);

    EntityPosition position() const noexcept;
    EntityPosition frame_origin() const noexcept;
    Direction direction() const noexcept;
    EntityState state() const noexcept;
    std::size_t current_frame_index() const noexcept;
    const AnimationFrame* current_frame() const noexcept;

private:
    AnimationClip& current_clip() noexcept;
    const AnimationClip& current_clip() const noexcept;

    EntityPosition position_;
    Direction direction_;
    EntityState state_ = EntityState::idle;
    std::array<std::array<AnimationClip, 8>, 2> animations_;
};

} // namespace dhsh
