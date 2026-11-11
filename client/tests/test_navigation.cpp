#include "input/MapInput.h"
#include "navigation/NavGrid.h"
#include "navigation/PathFinder.h"
#include "scene/Entity.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

dhsh::Cel make_cel(std::uint32_t width, std::uint32_t height,
    const std::vector<dhsh::NavCell>& blocked = {}) {
    dhsh::Cel cel;
    cel.width = width;
    cel.height = height;
    const std::size_t stride = (static_cast<std::size_t>(width) + 7U) / 8U;
    cel.bits.resize(stride * height);
    for (const dhsh::NavCell cell : blocked) {
        check(cell.x >= 0 && cell.y >= 0
                && static_cast<std::uint32_t>(cell.x) < width
                && static_cast<std::uint32_t>(cell.y) < height,
            "blocked test cell is out of bounds");
        const std::size_t index = static_cast<std::size_t>(cell.y) * stride
            + static_cast<std::size_t>(cell.x) / 8U;
        cel.bits[index] |= static_cast<std::byte>(0x80U >> (cell.x & 7));
    }
    return cel;
}

void check_cells(const std::vector<dhsh::NavCell>& actual,
    const std::vector<dhsh::NavCell>& expected, const std::string& message) {
    check(actual == expected, message + ": path differs, size="
            + std::to_string(actual.size()) + ", expected="
            + std::to_string(expected.size()));
}

void test_straight_reachable_path_has_fixed_cost() {
    const dhsh::Cel cel = make_cel(5, 3);
    const dhsh::NavGrid grid(cel);
    const dhsh::PathFinder finder(grid);
    const dhsh::PathResult result = finder.find({0, 1}, {4, 1});
    check(result.status == dhsh::PathStatus::success,
        "straight walkable route was not found");
    check_cells(result.cells, {{1, 1}, {2, 1}, {3, 1}, {4, 1}},
        "straight route");
    check(result.cost == 4000, "orthogonal path cost is not 1000 per cell");
    check(dhsh::PathFinder::orthogonal_cost == 1000
            && dhsh::PathFinder::diagonal_cost == 1414,
        "published fixed costs changed");
    const dhsh::PathResult diagonal = finder.find({0, 0}, {1, 1});
    check(diagonal.status == dhsh::PathStatus::success
            && diagonal.cost == 1414,
        "open diagonal path did not use the fixed diagonal cost");
    check_cells(diagonal.cells, {{1, 1}}, "open diagonal route");
}

void test_path_routes_around_obstacle_deterministically() {
    const dhsh::Cel cel = make_cel(3, 3, {{1, 1}});
    const dhsh::NavGrid grid(cel);
    const dhsh::PathFinder finder(grid);
    const dhsh::PathResult first = finder.find({0, 1}, {2, 1});
    const dhsh::PathResult second = finder.find({0, 1}, {2, 1});
    check(first.status == dhsh::PathStatus::success,
        "route around a blocked cell was not found");
    const std::vector<dhsh::NavCell> expected = {{0, 0}, {1, 0}, {2, 0}, {2, 1}};
    check_cells(first.cells, expected, "obstacle route");
    check_cells(second.cells, expected, "repeated obstacle route");
    check(first.cost == 4000 && second.cost == first.cost,
        "fixed orthogonal route cost is not reproducible");
    const std::array<dhsh::NavCell, 8> expected_order = {{
        {0, -1}, {1, 0}, {0, 1}, {-1, 0},
        {1, -1}, {1, 1}, {-1, 1}, {-1, -1},
    }};
    check(dhsh::PathFinder::neighbor_order() == expected_order,
        "neighbor expansion order changed");
}

void test_unreachable_returns_empty_path_and_explicit_status() {
    const dhsh::Cel cel = make_cel(5, 5,
        {{1, 0}, {1, 1}, {1, 2}, {1, 3}, {1, 4}});
    const dhsh::NavGrid grid(cel);
    const dhsh::PathFinder finder(grid);
    const dhsh::PathResult result = finder.find({0, 2}, {4, 2});
    check(result.status == dhsh::PathStatus::unreachable,
        "separated regions did not report unreachable");
    check(result.cells.empty() && result.cost == 0,
        "unreachable result contains a path or movement cost");
}

void test_out_of_bounds_start_and_goal_are_distinct() {
    const dhsh::Cel cel = make_cel(3, 3);
    const dhsh::NavGrid grid(cel);
    const dhsh::PathFinder finder(grid);
    const dhsh::PathResult bad_start = finder.find({-1, 0}, {2, 2});
    const dhsh::PathResult bad_goal = finder.find({0, 0}, {3, 2});
    check(bad_start.status == dhsh::PathStatus::start_out_of_bounds
            && bad_start.cells.empty(),
        "out-of-bounds start was not rejected without a path");
    check(bad_goal.status == dhsh::PathStatus::goal_out_of_bounds
            && bad_goal.cells.empty(),
        "out-of-bounds goal was not rejected without a path");
}

void test_blocked_start_and_goal_are_distinct() {
    const dhsh::Cel cel = make_cel(3, 3, {{0, 0}, {2, 2}});
    const dhsh::NavGrid grid(cel);
    const dhsh::PathFinder finder(grid);
    const dhsh::PathResult blocked_start = finder.find({0, 0}, {1, 1});
    const dhsh::PathResult blocked_goal = finder.find({1, 1}, {2, 2});
    check(blocked_start.status == dhsh::PathStatus::start_blocked
            && blocked_start.cells.empty(),
        "blocked start was not rejected without a path");
    check(blocked_goal.status == dhsh::PathStatus::goal_blocked
            && blocked_goal.cells.empty(),
        "blocked goal was not rejected without a path");
}

void test_diagonal_cannot_cut_through_blocked_orthogonal_cell() {
    const dhsh::Cel one_side = make_cel(2, 2, {{1, 0}});
    const dhsh::NavGrid one_side_grid(one_side);
    const dhsh::PathFinder one_side_finder(one_side_grid);
    const dhsh::PathResult detour = one_side_finder.find({0, 0}, {1, 1});
    check(detour.status == dhsh::PathStatus::success,
        "open orthogonal detour was rejected");
    check_cells(detour.cells, {{0, 1}, {1, 1}},
        "diagonal corner cut through a blocked orthogonal cell");
    check(detour.cost == 2000, "corner detour used diagonal cost");

    const dhsh::Cel both_sides = make_cel(2, 2, {{1, 0}, {0, 1}});
    const dhsh::NavGrid both_sides_grid(both_sides);
    const dhsh::PathFinder both_sides_finder(both_sides_grid);
    const dhsh::PathResult blocked = both_sides_finder.find({0, 0}, {1, 1});
    check(blocked.status == dhsh::PathStatus::unreachable
            && blocked.cells.empty(),
        "diagonal crossed two blocked orthogonal cells");
}

void test_repeated_goal_is_empty_and_does_not_repath() {
    const dhsh::Cel cel = make_cel(3, 3);
    const dhsh::NavGrid grid(cel);
    const dhsh::PathFinder finder(grid);
    const dhsh::PathResult result = finder.find({1, 1}, {1, 1});
    check(result.status == dhsh::PathStatus::already_at_goal,
        "repeated goal did not report already_at_goal");
    check(result.cells.empty() && result.cost == 0,
        "repeated goal produced a nonempty path or nonzero cost");
}

void test_grid_centers_and_negative_pixels_use_floor_cells() {
    const dhsh::Cel cel = make_cel(3, 2);
    const dhsh::NavGrid grid(cel);
    check(grid.cell_at_pixel({15, 16}) == dhsh::NavCell{0, 1},
        "positive pixel boundary did not select its floor cell");
    check(grid.cell_at_pixel({-1, -16}) == dhsh::NavCell{-1, -1},
        "negative pixel did not use floor division");
    const dhsh::NavPixel center = grid.cell_center({2, 1});
    check(center.x == 40 && center.y == 24, "cell center is not pixel +8");
    check(grid.walkable({2, 1}) && !grid.walkable({3, 1}),
        "grid walkability did not preserve CEL boundaries");
}

void test_movement_follows_centers_at_configured_m1_speed() {
    const dhsh::Cel cel = make_cel(4, 2);
    const dhsh::NavGrid grid(cel);
    dhsh::PathMovement movement(grid, dhsh::MovementConfig{100.0});
    dhsh::Entity entity({8, 8});
    check(movement.set_destination(entity, {2, 0}) == dhsh::PathStatus::success,
        "valid movement destination was rejected");
    check(movement.advance(entity, 80.0), "first movement step did not change pixels");
    check(entity.position() == dhsh::EntityPosition{16, 8}
            && entity.state() == dhsh::EntityState::walk,
        "80ms at 100px/s did not move exactly eight pixels toward the center");
    movement.advance(entity, 1000.0);
    check(entity.position() == dhsh::EntityPosition{40, 8}
            && entity.state() == dhsh::EntityState::idle && !movement.moving(),
        "large frame did not consume segments and stop at the final center");
    check(movement.set_destination(entity, {2, 0})
            == dhsh::PathStatus::already_at_goal,
        "repeated destination restarted a completed movement");
    check(entity.position() == dhsh::EntityPosition{40, 8}
            && entity.state() == dhsh::EntityState::idle,
        "repeated destination teleported or re-entered Walk");
}

void test_failed_movement_never_teleports_or_enters_walk() {
    const dhsh::Cel cel = make_cel(3, 3, {{2, 2}});
    const dhsh::NavGrid grid(cel);
    dhsh::PathMovement movement(grid, dhsh::MovementConfig{120.0});
    dhsh::Entity entity({8, 8});
    const dhsh::PathStatus status = movement.set_destination(entity, {2, 2});
    check(status == dhsh::PathStatus::goal_blocked && !movement.moving(),
        "blocked destination did not leave an explicit stopped status");
    movement.advance(entity, 1000.0);
    check(entity.position() == dhsh::EntityPosition{8, 8}
            && entity.state() == dhsh::EntityState::idle,
        "unreachable destination changed position or entered Walk");
}

dhsh::WindowEvent mouse_event(dhsh::WindowEvent::Type type,
    std::optional<dhsh::LogicalPoint> position) {
    dhsh::WindowEvent event;
    event.type = type;
    event.logical_position = position;
    return event;
}

void test_map_input_rejects_ui_black_bars_and_left_drag_release() {
    dhsh::MapInput input({dhsh::LogicalRect{600, 0, 200, 100}});
    const dhsh::MapInputView view{100.0, 50.0, 2.0};
    using Type = dhsh::WindowEvent::Type;

    input.handle_event(mouse_event(Type::left_button_down, dhsh::LogicalPoint{700, 50}), view);
    check(!input.handle_event(
              mouse_event(Type::left_button_up, dhsh::LogicalPoint{700, 50}), view)
              .has_value(),
        "UI click created a navigation target");

    input.handle_event(mouse_event(Type::left_button_down, std::nullopt), view);
    check(!input.handle_event(mouse_event(Type::left_button_up, std::nullopt), view)
              .has_value(),
        "black-bar click created a navigation target");

    input.handle_event(mouse_event(Type::left_button_down, dhsh::LogicalPoint{10, 20}), view);
    input.handle_event(mouse_event(Type::mouse_move, dhsh::LogicalPoint{30, 40}), view);
    check(!input.camera_dragging(), "left drag incorrectly became a camera drag");
    check(!input.handle_event(
              mouse_event(Type::left_button_up, dhsh::LogicalPoint{30, 40}), view)
              .has_value(),
        "left drag release created a navigation target");
    check(!input.camera_dragging(), "left release left camera drag active");
}

void test_map_input_right_drag_never_creates_navigation() {
    dhsh::MapInput input({dhsh::LogicalRect{600, 0, 200, 100}});
    const dhsh::MapInputView view{100.0, 50.0, 2.0};
    using Type = dhsh::WindowEvent::Type;

    input.handle_event(mouse_event(Type::right_button_down, dhsh::LogicalPoint{20, 30}), view);
    check(input.camera_dragging(), "right down did not start camera drag");
    input.handle_event(mouse_event(Type::left_button_down, dhsh::LogicalPoint{20, 30}), view);
    check(!input.handle_event(
              mouse_event(Type::left_button_up, dhsh::LogicalPoint{20, 30}), view)
              .has_value() && input.camera_dragging(),
        "left click during right drag created a target or cancelled the drag");
    input.handle_event(mouse_event(Type::mouse_move, dhsh::LogicalPoint{80, 90}), view);
    check(!input.handle_event(
              mouse_event(Type::right_button_up, dhsh::LogicalPoint{80, 90}), view)
              .has_value() && !input.camera_dragging(),
        "right drag release created a target or left drag active");

    input.handle_event(mouse_event(Type::right_button_down, dhsh::LogicalPoint{20, 30}), view);
    check(!input.handle_event(
              mouse_event(Type::right_button_up, dhsh::LogicalPoint{20, 30}), view)
              .has_value(),
        "simple right click created a navigation target");

    input.handle_event(mouse_event(Type::right_button_down, dhsh::LogicalPoint{700, 50}), view);
    check(!input.camera_dragging(), "UI right down started camera drag");
    check(!input.handle_event(
              mouse_event(Type::right_button_up, dhsh::LogicalPoint{700, 50}), view)
              .has_value(),
        "UI right click created a navigation target");

    input.handle_event(mouse_event(Type::right_button_down, dhsh::LogicalPoint{20, 30}), view);
    input.handle_event(mouse_event(Type::input_cancelled, std::nullopt), view);
    check(!input.camera_dragging(), "input cancellation left right drag active");
    check(!input.handle_event(
              mouse_event(Type::right_button_up, dhsh::LogicalPoint{20, 30}), view)
              .has_value(),
        "cancelled right drag created a navigation target");
}

void test_map_input_converts_simple_click_with_zoom() {
    dhsh::MapInput input;
    const dhsh::MapInputView view{100.0, 50.0, 2.0};
    using Type = dhsh::WindowEvent::Type;
    input.handle_event(mouse_event(Type::left_button_down, dhsh::LogicalPoint{20, 40}), view);
    const std::optional<dhsh::MapPoint> target = input.handle_event(
        mouse_event(Type::left_button_up, dhsh::LogicalPoint{20, 40}), view);
    check(target.has_value() && target->x == 110 && target->y == 70,
        "simple map click did not apply camera and zoom exactly once");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"straight reachable path and fixed cost", test_straight_reachable_path_has_fixed_cost},
        {"deterministic obstacle route", test_path_routes_around_obstacle_deterministically},
        {"unreachable empty result", test_unreachable_returns_empty_path_and_explicit_status},
        {"out-of-bounds endpoints", test_out_of_bounds_start_and_goal_are_distinct},
        {"blocked endpoints", test_blocked_start_and_goal_are_distinct},
        {"diagonal wall rejection", test_diagonal_cannot_cut_through_blocked_orthogonal_cell},
        {"repeated goal", test_repeated_goal_is_empty_and_does_not_repath},
        {"grid centers and floor cells", test_grid_centers_and_negative_pixels_use_floor_cells},
        {"continuous configured-speed movement", test_movement_follows_centers_at_configured_m1_speed},
        {"failed movement remains stopped", test_failed_movement_never_teleports_or_enters_walk},
        {"UI black-bar and left-drag filtering", test_map_input_rejects_ui_black_bars_and_left_drag_release},
        {"right drag never creates navigation", test_map_input_right_drag_never_creates_navigation},
        {"zoomed map click conversion", test_map_input_converts_simple_click_with_zoom},
    };
    bool passed = true;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
            passed = false;
        }
    }
    return passed ? 0 : 1;
}
