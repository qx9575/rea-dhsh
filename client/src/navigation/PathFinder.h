#pragma once

#include "navigation/NavGrid.h"
#include "scene/Entity.h"

#include <array>
#include <cstdint>
#include <vector>

namespace dhsh {

enum class PathStatus : std::uint8_t {
    success,
    already_at_goal,
    unreachable,
    start_out_of_bounds,
    goal_out_of_bounds,
    start_blocked,
    goal_blocked,
};

struct PathResult {
    PathStatus status = PathStatus::unreachable;
    std::vector<NavCell> cells;
    std::uint64_t cost = 0;
};

class PathFinder {
public:
    static constexpr std::uint64_t orthogonal_cost = 1000;
    static constexpr std::uint64_t diagonal_cost = 1414;

    explicit PathFinder(const NavGrid& grid) noexcept : grid_(&grid) {
    }

    PathResult find(NavCell start, NavCell goal) const;
    static const std::array<NavCell, 8>& neighbor_order() noexcept;

private:
    const NavGrid* grid_;
};

struct MovementConfig {
    double pixels_per_second;
};

class PathMovement {
public:
    PathMovement(const NavGrid& grid, MovementConfig config);

    PathStatus set_destination(Entity& entity, NavCell goal);
    bool advance(Entity& entity, double elapsed_ms);

    bool moving() const noexcept { return moving_; }
    PathStatus status() const noexcept { return status_; }
    NavCell goal() const noexcept { return goal_; }

private:
    const NavGrid* grid_;
    double pixels_per_second_;
    std::vector<NavPixel> waypoints_;
    std::size_t next_waypoint_ = 0;
    double exact_x_ = 0.0;
    double exact_y_ = 0.0;
    double travel_pixels_ = 0.0;
    NavCell goal_;
    PathStatus status_ = PathStatus::already_at_goal;
    bool moving_ = false;
};

} // namespace dhsh
