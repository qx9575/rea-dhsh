#include "app/App.h"
#include "platform/Win32Window.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
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

void check_point(
    const std::optional<dhsh::MapPoint>& actual,
    std::int32_t x,
    std::int32_t y,
    const std::string& message) {
    check(actual.has_value(), message + ": point was rejected");
    check(actual->x == x && actual->y == y,
        message + ": expected (" + std::to_string(x) + "," + std::to_string(y)
            + "), got (" + std::to_string(actual->x) + ","
            + std::to_string(actual->y) + ")");
}

void test_letterboxes_wide_and_tall_windows() {
    dhsh::Viewport viewport(800, 600);

    viewport.resize(800, 600);
    check(viewport.x() == 0 && viewport.y() == 0
            && viewport.width() == 800 && viewport.height() == 600,
        "native-size viewport does not fill the window");

    viewport.resize(1000, 600);
    check(viewport.x() == 100 && viewport.y() == 0
            && viewport.width() == 800 && viewport.height() == 600,
        "wide window did not receive equal left and right bars");

    viewport.resize(800, 1000);
    check(viewport.x() == 0 && viewport.y() == 200
            && viewport.width() == 800 && viewport.height() == 600,
        "tall window did not receive equal top and bottom bars");

    viewport.resize(1000, 800);
    check(viewport.x() == 0 && viewport.y() == 25
            && viewport.width() == 1000 && viewport.height() == 750,
        "scaled viewport changed the 4:3 aspect ratio");
}

void test_inverse_transform_includes_scale_offset_and_camera() {
    dhsh::Viewport viewport(800, 600);
    viewport.resize(1000, 600);
    check_point(viewport.screen_to_map(100, 0, 120, 80), 120, 80,
        "wide viewport origin");
    check_point(viewport.screen_to_map(500, 300, 120, 80), 520, 380,
        "wide viewport center");

    viewport.resize(1600, 1200);
    check_point(viewport.screen_to_map(400, 300, 40, 20), 240, 170,
        "2x viewport inverse transform");
}

void test_clicks_in_black_bars_do_not_create_map_actions() {
    dhsh::Viewport viewport(800, 600);
    viewport.resize(1000, 800);
    int map_actions = 0;
    for (const auto [x, y] : {std::pair{-1, 20}, {10, 10}, {500, 24},
             {500, 775}, {999, 799}}) {
        if (viewport.screen_to_map(x, y, 0, 0).has_value()) {
            ++map_actions;
        }
    }
    check(map_actions == 0, "a black-bar click produced a map action");

    check_point(viewport.screen_to_map(0, 25, 10, 20), 10, 20,
        "top-left content click");
    check_point(viewport.screen_to_map(999, 774, 10, 20), 809, 619,
        "bottom-right content click");

    viewport.resize(0, 0);
    check(!viewport.screen_to_map(0, 0, 0, 0).has_value(),
        "minimized window produced a map action");
}

void test_frame_elapsed_time_is_monotonic_finite_and_safe() {
    check(std::abs(dhsh::frame_elapsed_ms(100, 250, 1000) - 150.0) < 1e-9,
        "valid QPC ticks did not produce actual elapsed milliseconds");
    check(dhsh::frame_elapsed_ms(250, 100, 1000) == 0.0,
        "non-monotonic QPC ticks produced negative elapsed time");
    check(dhsh::frame_elapsed_ms(100, 250, 0) == 0.0
            && dhsh::frame_elapsed_ms(100, 250, -1) == 0.0,
        "invalid QPC frequency produced elapsed time");
    const double extreme = dhsh::frame_elapsed_ms(
        std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max(), 1);
    check(std::isfinite(extreme) && extreme >= 0.0,
        "extreme QPC values produced non-finite or negative elapsed time");
}

void test_input_cancellation_clears_movement_keys() {
    dhsh::InputState input;
    dhsh::WindowEvent key;
    key.type = dhsh::WindowEvent::Type::key_down;
    key.key = VK_RIGHT;
    input.handle_event(key);
    check(input.horizontal() == 1.0, "right key did not enter movement state");

    dhsh::WindowEvent cancel;
    cancel.type = dhsh::WindowEvent::Type::input_cancelled;
    input.handle_event(cancel);
    check(input.horizontal() == 0.0 && input.vertical() == 0.0,
        "capture/focus cancellation left movement keys active");

    key.key = VK_DOWN;
    input.handle_event(key);
    check(input.vertical() == 1.0, "down key did not enter movement state");
    input.handle_event(cancel);
    check(input.vertical() == 0.0, "second cancellation left a key active");
}

void test_camera_follow_approaches_without_jump() {
    const double first = dhsh::approach_camera_coordinate(100.0, 500.0, 16.0);
    check(first > 100.0 && first < 500.0,
        "follow resumed with a camera jump");
    check(dhsh::approach_camera_coordinate(100.0, 500.0, 0.0) == 100.0
            && dhsh::approach_camera_coordinate(100.0, 500.0, -1.0) == 100.0
            && dhsh::approach_camera_coordinate(100.0, 500.0,
                std::numeric_limits<double>::quiet_NaN()) == 100.0,
        "invalid frame duration moved the camera");
    check(dhsh::approach_camera_coordinate(499.999, 500.0, 16.0) == 500.0,
        "camera did not converge to its target");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"letterboxed scaling", test_letterboxes_wide_and_tall_windows},
        {"screen to map inverse transform", test_inverse_transform_includes_scale_offset_and_camera},
        {"black-bar click rejection", test_clicks_in_black_bars_do_not_create_map_actions},
        {"safe frame elapsed time", test_frame_elapsed_time_is_monotonic_finite_and_safe},
        {"input cancellation", test_input_cancellation_clears_movement_keys},
        {"smooth camera follow", test_camera_follow_approaches_without_jump},
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
