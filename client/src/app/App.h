#pragma once

#include "platform/Win32Window.h"

#include <cstdint>

namespace dhsh {

double frame_elapsed_ms(
    std::int64_t previous, std::int64_t current, std::int64_t frequency);
double approach_camera_coordinate(
    double current, double target, double elapsed_ms) noexcept;

class InputState {
public:
    void handle_event(const WindowEvent& event);
    double horizontal() const noexcept;
    double vertical() const noexcept;

private:
    bool move_left_ = false;
    bool move_right_ = false;
    bool move_up_ = false;
    bool move_down_ = false;
};

class App {
public:
    int logical_width() const noexcept;
    int logical_height() const noexcept;
    int run(int argc, char** argv);
};

} // namespace dhsh
