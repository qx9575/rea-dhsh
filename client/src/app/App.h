#pragma once

#include "platform/Win32Window.h"

#include <cstdint>

namespace dhsh {

double frame_elapsed_ms(
    std::int64_t previous, std::int64_t current, std::int64_t frequency);

class InputState {
public:
    void handle_event(const WindowEvent& event);
    bool dragging() const noexcept { return dragging_; }
    double horizontal() const noexcept;
    double vertical() const noexcept;

private:
    bool dragging_ = false;
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
