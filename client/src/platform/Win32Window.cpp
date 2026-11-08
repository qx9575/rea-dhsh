#include "platform/Win32Window.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace dhsh {
namespace {

std::int32_t clamp_to_i32(std::int64_t value) {
    return static_cast<std::int32_t>(std::clamp(value,
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()),
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
}

std::int32_t signed_low_word(LPARAM value) {
    return static_cast<std::int16_t>(LOWORD(value));
}

std::int32_t signed_high_word(LPARAM value) {
    return static_cast<std::int16_t>(HIWORD(value));
}

} // namespace

Viewport::Viewport(std::uint32_t logical_width, std::uint32_t logical_height)
    : logical_width_(logical_width), logical_height_(logical_height) {
    if (logical_width == 0 || logical_height == 0) {
        throw std::invalid_argument("logical viewport dimensions must be positive");
    }
}

void Viewport::resize(std::uint32_t window_width, std::uint32_t window_height) {
    if (window_width == 0 || window_height == 0) {
        x_ = 0;
        y_ = 0;
        width_ = 0;
        height_ = 0;
        return;
    }

    const std::uint64_t window_aspect_numerator =
        static_cast<std::uint64_t>(window_width) * logical_height_;
    const std::uint64_t logical_aspect_numerator =
        static_cast<std::uint64_t>(window_height) * logical_width_;
    if (window_aspect_numerator > logical_aspect_numerator) {
        height_ = window_height;
        width_ = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(window_height) * logical_width_
            / logical_height_);
    } else {
        width_ = window_width;
        height_ = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(window_width) * logical_height_
            / logical_width_);
    }
    x_ = (window_width - width_) / 2;
    y_ = (window_height - height_) / 2;
}

std::optional<LogicalPoint> Viewport::screen_to_logical(
    std::int32_t screen_x, std::int32_t screen_y) const {
    if (width_ == 0 || height_ == 0 || screen_x < static_cast<std::int32_t>(x_)
        || screen_y < static_cast<std::int32_t>(y_)
        || screen_x >= static_cast<std::int32_t>(x_ + width_)
        || screen_y >= static_cast<std::int32_t>(y_ + height_)) {
        return std::nullopt;
    }
    const std::int64_t relative_x = static_cast<std::int64_t>(screen_x) - x_;
    const std::int64_t relative_y = static_cast<std::int64_t>(screen_y) - y_;
    return LogicalPoint{
        clamp_to_i32(relative_x * logical_width_ / width_),
        clamp_to_i32(relative_y * logical_height_ / height_),
    };
}

std::optional<MapPoint> Viewport::screen_to_map(
    std::int32_t screen_x,
    std::int32_t screen_y,
    std::int32_t camera_x,
    std::int32_t camera_y) const {
    const auto logical = screen_to_logical(screen_x, screen_y);
    if (!logical.has_value()) {
        return std::nullopt;
    }
    return MapPoint{
        clamp_to_i32(static_cast<std::int64_t>(camera_x) + logical->x),
        clamp_to_i32(static_cast<std::int64_t>(camera_y) + logical->y),
    };
}

Win32Window::Win32Window(const std::wstring& title,
    std::uint32_t logical_width,
    std::uint32_t logical_height,
    EventHandler handler)
    : viewport_(logical_width, logical_height), handler_(std::move(handler)) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t class_name[] = L"dhsh_client_window";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.lpszClassName = class_name;
    if (RegisterClassW(&window_class) == 0
        && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(), "cannot register client window class");
    }

    RECT rectangle{0, 0, static_cast<LONG>(logical_width),
        static_cast<LONG>(logical_height)};
    AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
    window_ = CreateWindowExW(0, class_name, title.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rectangle.right - rectangle.left,
        rectangle.bottom - rectangle.top, nullptr, nullptr, instance, this);
    if (window_ == nullptr) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(), "cannot create client window");
    }
    RECT client{};
    GetClientRect(window_, &client);
    client_width_ = static_cast<std::uint32_t>(client.right - client.left);
    client_height_ = static_cast<std::uint32_t>(client.bottom - client.top);
    viewport_.resize(client_width_, client_height_);
    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);
}

Win32Window::~Win32Window() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
}

void Win32Window::set_title(const std::wstring& title) {
    if (window_ != nullptr) {
        SetWindowTextW(window_, title.c_str());
    }
}

LRESULT CALLBACK Win32Window::window_proc(
    HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    Win32Window* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        self = static_cast<Win32Window*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    } else {
        self = reinterpret_cast<Win32Window*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->handle_message(window, message, w_param, l_param);
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

std::optional<LogicalPoint> Win32Window::logical_position(LPARAM l_param) const {
    return viewport_.screen_to_logical(
        signed_low_word(l_param), signed_high_word(l_param));
}

void Win32Window::emit(WindowEvent event) {
    if (handler_) {
        handler_(event);
    }
}

LRESULT Win32Window::handle_message(
    HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_SIZE:
        client_width_ = LOWORD(l_param);
        client_height_ = HIWORD(l_param);
        viewport_.resize(client_width_, client_height_);
        emit({WindowEvent::Type::resized, client_width_, client_height_});
        return 0;
    case WM_MOUSEMOVE:
        emit({WindowEvent::Type::mouse_move, 0, 0, logical_position(l_param)});
        return 0;
    case WM_LBUTTONDOWN: {
        const auto position = logical_position(l_param);
        if (position.has_value()) {
            SetCapture(window);
        }
        emit({WindowEvent::Type::left_button_down, 0, 0, position});
        return 0;
    }
    case WM_LBUTTONUP:
        emit({WindowEvent::Type::left_button_up, 0, 0, logical_position(l_param)});
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(l_param) != window) {
            emit({WindowEvent::Type::input_cancelled});
        }
        return 0;
    case WM_KILLFOCUS:
        emit({WindowEvent::Type::input_cancelled});
        return 0;
    case WM_MOUSEWHEEL: {
        POINT point{signed_low_word(l_param), signed_high_word(l_param)};
        ScreenToClient(window, &point);
        const auto position = viewport_.screen_to_logical(point.x, point.y);
        WindowEvent event{WindowEvent::Type::mouse_wheel, 0, 0, position};
        event.wheel_delta = GET_WHEEL_DELTA_WPARAM(w_param);
        emit(event);
        return 0;
    }
    case WM_KEYDOWN: {
        WindowEvent event{WindowEvent::Type::key_down};
        event.key = static_cast<std::uintptr_t>(w_param);
        emit(event);
        return 0;
    }
    case WM_KEYUP: {
        WindowEvent event{WindowEvent::Type::key_up};
        event.key = static_cast<std::uintptr_t>(w_param);
        emit(event);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY: {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        const LRESULT result = DefWindowProcW(window, message, w_param, l_param);
        window_ = nullptr;
        return result;
    }
    default:
        return DefWindowProcW(window, message, w_param, l_param);
    }
}

} // namespace dhsh
