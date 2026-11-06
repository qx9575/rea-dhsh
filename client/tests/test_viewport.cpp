#include "platform/Win32Window.h"

#include <cstdint>
#include <functional>
#include <iostream>
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

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"letterboxed scaling", test_letterboxes_wide_and_tall_windows},
        {"screen to map inverse transform", test_inverse_transform_includes_scale_offset_and_camera},
        {"black-bar click rejection", test_clicks_in_black_bars_do_not_create_map_actions},
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
