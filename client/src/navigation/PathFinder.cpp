#include "navigation/PathFinder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <queue>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dhsh {
namespace {

std::uint64_t heuristic(NavCell from, NavCell to) {
    const auto dx = static_cast<std::uint64_t>(
        std::abs(static_cast<std::int64_t>(from.x) - to.x));
    const auto dy = static_cast<std::uint64_t>(
        std::abs(static_cast<std::int64_t>(from.y) - to.y));
    const auto diagonal = std::min(dx, dy);
    return diagonal * PathFinder::diagonal_cost
        + (std::max(dx, dy) - diagonal) * PathFinder::orthogonal_cost;
}

std::int32_t rounded_pixel(double value) {
    const double rounded = std::round(value);
    return static_cast<std::int32_t>(std::clamp(rounded,
        static_cast<double>(std::numeric_limits<std::int32_t>::min()),
        static_cast<double>(std::numeric_limits<std::int32_t>::max())));
}

} // namespace

const std::array<NavCell, 8>& PathFinder::neighbor_order() noexcept {
    static constexpr std::array<NavCell, 8> order = {{
        {0, -1}, {1, 0}, {0, 1}, {-1, 0},
        {1, -1}, {1, 1}, {-1, 1}, {-1, -1},
    }};
    return order;
}

PathResult PathFinder::find(NavCell start, NavCell goal) const {
    if (!grid_->contains(start)) {
        return PathResult{PathStatus::start_out_of_bounds};
    }
    if (!grid_->contains(goal)) {
        return PathResult{PathStatus::goal_out_of_bounds};
    }
    if (!grid_->walkable(start)) {
        return PathResult{PathStatus::start_blocked};
    }
    if (!grid_->walkable(goal)) {
        return PathResult{PathStatus::goal_blocked};
    }
    if (start == goal) {
        return PathResult{PathStatus::already_at_goal};
    }

    const auto index = [&](NavCell cell) {
        return static_cast<std::size_t>(cell.y) * grid_->width()
            + static_cast<std::size_t>(cell.x);
    };
    const std::size_t count = static_cast<std::size_t>(grid_->width())
        * grid_->height();
    constexpr std::uint64_t infinity = std::numeric_limits<std::uint64_t>::max();
    std::vector<std::uint64_t> scores(count, infinity);
    std::vector<NavCell> parents(count);
    std::vector<bool> has_parent(count, false);
    std::vector<bool> closed(count, false);

    struct OpenNode {
        std::uint64_t f;
        std::uint64_t h;
        std::uint64_t g;
        std::uint64_t sequence;
        NavCell cell;
    };
    const auto later = [](const OpenNode& left, const OpenNode& right) {
        if (left.f != right.f) {
            return left.f > right.f;
        }
        if (left.h != right.h) {
            return left.h > right.h;
        }
        return left.sequence > right.sequence;
    };
    std::priority_queue<OpenNode, std::vector<OpenNode>, decltype(later)> open(later);
    std::uint64_t sequence = 0;
    const std::size_t start_index = index(start);
    scores[start_index] = 0;
    const std::uint64_t start_h = heuristic(start, goal);
    open.push(OpenNode{start_h, start_h, 0, sequence++, start});

    while (!open.empty()) {
        const OpenNode current = open.top();
        open.pop();
        const std::size_t current_index = index(current.cell);
        if (closed[current_index] || scores[current_index] != current.g) {
            continue;
        }
        if (current.cell == goal) {
            PathResult result;
            result.status = PathStatus::success;
            result.cost = current.g;
            NavCell cursor = goal;
            while (!(cursor == start)) {
                result.cells.push_back(cursor);
                const std::size_t cursor_index = index(cursor);
                if (!has_parent[cursor_index]) {
                    return PathResult{PathStatus::unreachable};
                }
                cursor = parents[cursor_index];
            }
            std::reverse(result.cells.begin(), result.cells.end());
            return result;
        }
        closed[current_index] = true;

        for (const NavCell offset : neighbor_order()) {
            const NavCell next{current.cell.x + offset.x, current.cell.y + offset.y};
            if (!grid_->walkable(next)) {
                continue;
            }
            if (offset.x != 0 && offset.y != 0
                && (!grid_->walkable({current.cell.x + offset.x, current.cell.y})
                    || !grid_->walkable({current.cell.x, current.cell.y + offset.y}))) {
                continue;
            }
            const std::size_t next_index = index(next);
            if (closed[next_index]) {
                continue;
            }
            const std::uint64_t step = offset.x == 0 || offset.y == 0
                ? orthogonal_cost : diagonal_cost;
            const std::uint64_t candidate = current.g + step;
            if (candidate >= scores[next_index]) {
                continue;
            }
            scores[next_index] = candidate;
            parents[next_index] = current.cell;
            has_parent[next_index] = true;
            const std::uint64_t h = heuristic(next, goal);
            open.push(OpenNode{candidate + h, h, candidate, sequence++, next});
        }
    }
    return PathResult{PathStatus::unreachable};
}

PathMovement::PathMovement(const NavGrid& grid, MovementConfig config)
    : grid_(&grid), pixels_per_second_(config.pixels_per_second) {
    if (!std::isfinite(pixels_per_second_) || pixels_per_second_ <= 0.0) {
        throw std::invalid_argument("navigation speed must be finite and positive");
    }
}

PathStatus PathMovement::set_destination(Entity& entity, NavCell goal) {
    if (moving_ && goal_ == goal) {
        return status_;
    }
    const NavPixel current{entity.position().x, entity.position().y};
    const NavCell start = grid_->cell_at_pixel(current);
    const PathFinder finder(*grid_);
    const PathResult path = finder.find(start, goal);
    status_ = path.status;
    goal_ = goal;
    waypoints_.clear();
    next_waypoint_ = 0;
    travel_pixels_ = 0.0;
    moving_ = false;
    exact_x_ = current.x;
    exact_y_ = current.y;
    if (path.status != PathStatus::success) {
        entity.stop();
        return status_;
    }

    const NavPixel start_center = grid_->cell_center(start);
    if (!(current == start_center)) {
        waypoints_.push_back(start_center);
    }
    for (const NavCell cell : path.cells) {
        waypoints_.push_back(grid_->cell_center(cell));
    }
    moving_ = !waypoints_.empty();
    if (!moving_) {
        status_ = PathStatus::already_at_goal;
        entity.stop();
    }
    return status_;
}

bool PathMovement::advance(Entity& entity, double elapsed_ms) {
    if (!moving_ || !std::isfinite(elapsed_ms) || elapsed_ms <= 0.0) {
        return false;
    }
    const EntityPosition old_position = entity.position();
    const EntityState old_state = entity.state();
    travel_pixels_ += pixels_per_second_ * elapsed_ms / 1000.0;
    if (!std::isfinite(travel_pixels_)) {
        travel_pixels_ = std::numeric_limits<double>::max();
    }

    while (moving_ && travel_pixels_ > 0.0) {
        const NavPixel target = waypoints_[next_waypoint_];
        const double dx = static_cast<double>(target.x) - exact_x_;
        const double dy = static_cast<double>(target.y) - exact_y_;
        const double distance = std::hypot(dx, dy);
        if (distance == 0.0) {
            exact_x_ = target.x;
            exact_y_ = target.y;
            ++next_waypoint_;
            if (next_waypoint_ == waypoints_.size()) {
                moving_ = false;
                status_ = PathStatus::already_at_goal;
            }
            continue;
        }
        const double step = std::min(distance, travel_pixels_);
        exact_x_ += dx * step / distance;
        exact_y_ += dy * step / distance;
        travel_pixels_ -= step;
        if (step == distance) {
            exact_x_ = target.x;
            exact_y_ = target.y;
            ++next_waypoint_;
            if (next_waypoint_ == waypoints_.size()) {
                moving_ = false;
                status_ = PathStatus::already_at_goal;
            }
        }
    }

    const EntityPosition rounded{rounded_pixel(exact_x_), rounded_pixel(exact_y_)};
    entity.move_by(rounded.x - old_position.x, rounded.y - old_position.y);
    if (!moving_) {
        entity.set_position(rounded);
        entity.stop();
    }
    return old_position != entity.position() || old_state != entity.state();
}

} // namespace dhsh
