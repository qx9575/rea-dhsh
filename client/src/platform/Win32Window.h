#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace dhsh {

struct LogicalPoint {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

struct MapPoint {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

class Viewport {
public:
    Viewport(std::uint32_t logical_width, std::uint32_t logical_height);

    void resize(std::uint32_t window_width, std::uint32_t window_height);

    std::uint32_t x() const noexcept { return x_; }
    std::uint32_t y() const noexcept { return y_; }
    std::uint32_t width() const noexcept { return width_; }
    std::uint32_t height() const noexcept { return height_; }

    std::optional<LogicalPoint> screen_to_logical(
        std::int32_t screen_x, std::int32_t screen_y) const;
    std::optional<MapPoint> screen_to_map(
        std::int32_t screen_x,
        std::int32_t screen_y,
        std::int32_t camera_x,
        std::int32_t camera_y) const;

private:
    std::uint32_t logical_width_;
    std::uint32_t logical_height_;
    std::uint32_t x_ = 0;
    std::uint32_t y_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

struct WindowEvent {
    enum class Type {
        resized,
        mouse_move,
        left_button_down,
        left_button_up,
        mouse_wheel,
        key_down,
        key_up,
        input_cancelled,
    };

    Type type = Type::resized;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::optional<LogicalPoint> logical_position;
    std::int32_t wheel_delta = 0;
    std::uintptr_t key = 0;
};

class Win32Window {
public:
    using EventHandler = std::function<void(const WindowEvent&)>;

    Win32Window(const std::wstring& title,
        std::uint32_t logical_width,
        std::uint32_t logical_height,
        EventHandler handler);
    ~Win32Window();

    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    HWND handle() const noexcept { return window_; }
    const Viewport& viewport() const noexcept { return viewport_; }
    std::uint32_t client_width() const noexcept { return client_width_; }
    std::uint32_t client_height() const noexcept { return client_height_; }
    void set_title(const std::wstring& title);

private:
    static LRESULT CALLBACK window_proc(
        HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    LRESULT handle_message(
        HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    void emit(WindowEvent event);
    std::optional<LogicalPoint> logical_position(LPARAM l_param) const;

    HWND window_ = nullptr;
    Viewport viewport_;
    EventHandler handler_;
    std::uint32_t client_width_ = 0;
    std::uint32_t client_height_ = 0;
};

} // namespace dhsh
