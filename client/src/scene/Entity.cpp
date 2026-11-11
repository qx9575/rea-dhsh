#include "scene/Entity.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace dhsh {
namespace {

constexpr std::size_t state_index(EntityState state) noexcept {
    return static_cast<std::size_t>(state);
}

constexpr std::size_t direction_index(Direction direction) noexcept {
    return static_cast<std::size_t>(direction);
}

std::int32_t clamp_i64(std::int64_t value) noexcept {
    if (value < std::numeric_limits<std::int32_t>::min()) {
        return std::numeric_limits<std::int32_t>::min();
    }
    if (value > std::numeric_limits<std::int32_t>::max()) {
        return std::numeric_limits<std::int32_t>::max();
    }
    return static_cast<std::int32_t>(value);
}

std::int32_t clamp_add(std::int32_t left, std::int32_t right) noexcept {
    return clamp_i64(static_cast<std::int64_t>(left) + right);
}

} // namespace

std::optional<Direction> direction_from_vector(
    std::int32_t x, std::int32_t y) noexcept {
    if (x == 0 && y == 0) {
        return std::nullopt;
    }
    if (x < 0) {
        if (y < 0) {
            return Direction::left_up;
        }
        if (y > 0) {
            return Direction::left_down;
        }
        return Direction::left;
    }
    if (x > 0) {
        if (y < 0) {
            return Direction::right_up;
        }
        if (y > 0) {
            return Direction::right_down;
        }
        return Direction::right;
    }
    return y < 0 ? Direction::up : Direction::down;
}

std::optional<Direction> direction_from_mlk(std::int32_t extra0) noexcept {
    if (extra0 < 0 || extra0 > 7) {
        return std::nullopt;
    }
    return static_cast<Direction>(extra0);
}

EntityPosition spawn_from_mlk(const MlkLink& link) noexcept {
    return EntityPosition{clamp_add(link.dest_x, 8), clamp_add(link.dest_y, 8)};
}

std::int32_t cell_from_pixel(std::int32_t pixel) noexcept {
    const std::int64_t value = pixel;
    const std::int64_t cell = value >= 0 ? value / 16 : (value - 15) / 16;
    return static_cast<std::int32_t>(cell);
}

Entity::Entity(EntityPosition position, Direction direction)
    : position_(position), direction_(direction) {
}

void Entity::set_position(EntityPosition position) noexcept {
    position_ = position;
}

void Entity::move_by(std::int32_t x, std::int32_t y) {
    const EntityPosition next{
        clamp_add(position_.x, x),
        clamp_add(position_.y, y),
    };
    if (next == position_) {
        return;
    }
    const std::int64_t actual_x = static_cast<std::int64_t>(next.x) - position_.x;
    const std::int64_t actual_y = static_cast<std::int64_t>(next.y) - position_.y;
    const std::optional<Direction> next_direction = direction_from_vector(
        clamp_i64(actual_x), clamp_i64(actual_y));
    if (!next_direction.has_value()) {
        return;
    }
    ensure_animation(EntityState::walk, *next_direction);
    position_ = next;
    direction_ = *next_direction;
    state_ = EntityState::walk;
}

void Entity::stop() {
    ensure_animation(EntityState::idle, direction_);
    state_ = EntityState::idle;
}

void Entity::set_animation(
    EntityState state, Direction direction, AnimationClip clip) {
    animations_[state_index(state)][direction_index(direction)] = std::move(clip);
    ++animation_revision_;
}

void Entity::set_animation_loader(AnimationLoader loader) {
    animation_loader_ = std::move(loader);
}

bool Entity::advance(std::uint32_t milliseconds) {
    AnimationClip& clip = current_clip();
    const std::size_t previous = clip.current_frame_index();
    clip.advance(milliseconds);
    return clip.current_frame_index() != previous;
}

EntityPosition Entity::position() const noexcept {
    return position_;
}

EntityPosition Entity::frame_origin() const noexcept {
    const AnimationFrame* frame = current_frame();
    if (frame == nullptr) {
        return position_;
    }
    return EntityPosition{
        clamp_add(position_.x, frame->anchor_x),
        clamp_add(position_.y, frame->anchor_y),
    };
}

Direction Entity::direction() const noexcept {
    return direction_;
}

EntityState Entity::state() const noexcept {
    return state_;
}

std::size_t Entity::current_frame_index() const noexcept {
    return current_clip().current_frame_index();
}

std::size_t Entity::current_frame_count() const noexcept {
    return current_clip().frames().size();
}

std::uint64_t Entity::animation_revision() const noexcept {
    return animation_revision_;
}

const AnimationFrame* Entity::current_frame() const noexcept {
    const auto& frames = current_clip().frames();
    if (frames.empty()) {
        return nullptr;
    }
    return &frames[current_clip().current_frame_index()];
}

AnimationClip& Entity::current_clip() noexcept {
    ensure_current_animation();
    return animations_[state_index(state_)][direction_index(direction_)];
}

const AnimationClip& Entity::current_clip() const noexcept {
    return animations_[state_index(state_)][direction_index(direction_)];
}

void Entity::ensure_animation(EntityState state, Direction direction) {
    AnimationClip& clip = animations_[state_index(state)][direction_index(direction)];
    if (clip.frames().empty() && animation_loader_) {
        clip = animation_loader_(state, direction);
        ++animation_revision_;
    }
}

void Entity::ensure_current_animation() {
    ensure_animation(state_, direction_);
}

} // namespace dhsh
