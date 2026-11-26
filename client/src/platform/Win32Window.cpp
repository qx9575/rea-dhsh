#include "platform/Win32Window.h"

#include <imm.h>

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

bool is_high_surrogate(wchar_t unit) {
    return unit >= 0xd800 && unit <= 0xdbff;
}

bool is_low_surrogate(wchar_t unit) {
    return unit >= 0xdc00 && unit <= 0xdfff;
}

std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring imm_composition_string(HIMC context, DWORD flags) {
    if (context == nullptr) {
        return {};
    }
    const LONG byte_count = ImmGetCompositionStringW(context, flags, nullptr, 0);
    if (byte_count <= 0) {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(byte_count) / 2, L'\0');
    ImmGetCompositionStringW(context, flags, text.data(), byte_count);
    return text;
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

void Win32Window::rethrow_event_error() const {
    if (event_error_ != nullptr) {
        std::rethrow_exception(event_error_);
    }
}

void Win32Window::emit(WindowEvent event) {
    if (handler_ && event_error_ == nullptr) {
        try {
            handler_(event);
        } catch (...) {
            event_error_ = std::current_exception();
            PostQuitMessage(1);
        }
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
    case WM_SETCURSOR:
        if (LOWORD(l_param) == HTCLIENT) {
            SetCursor(cursor_hidden_
                    ? nullptr
                    : LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
            return TRUE;
        }
        return DefWindowProcW(window, message, w_param, l_param);
    case WM_LBUTTONDOWN: {
        const auto position = logical_position(l_param);
        if (position.has_value()) {
            captured_buttons_ |= MK_LBUTTON;
            SetCapture(window);
        }
        emit({WindowEvent::Type::left_button_down, 0, 0, position});
        return 0;
    }
    case WM_LBUTTONUP:
        emit({WindowEvent::Type::left_button_up, 0, 0, logical_position(l_param)});
        captured_buttons_ &= ~static_cast<std::uint32_t>(MK_LBUTTON);
        if (captured_buttons_ == 0 && GetCapture() == window) {
            ReleaseCapture();
        }
        return 0;
    case WM_RBUTTONDOWN: {
        const auto position = logical_position(l_param);
        if (position.has_value()) {
            captured_buttons_ |= MK_RBUTTON;
            SetCapture(window);
        }
        emit({WindowEvent::Type::right_button_down, 0, 0, position});
        return 0;
    }
    case WM_RBUTTONUP:
        emit({WindowEvent::Type::right_button_up, 0, 0, logical_position(l_param)});
        captured_buttons_ &= ~static_cast<std::uint32_t>(MK_RBUTTON);
        if (captured_buttons_ == 0 && GetCapture() == window) {
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED: {
        const auto new_capture = reinterpret_cast<HWND>(l_param);
        if (new_capture != window) {
            captured_buttons_ = 0;
            if (new_capture != nullptr) {
                emit({WindowEvent::Type::input_cancelled});
            }
        }
        return 0;
    }
    case WM_KILLFOCUS:
        captured_buttons_ = 0;
        pending_high_surrogate_ = L'\0';
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
    case WM_CHAR: {
        const wchar_t unit = static_cast<wchar_t>(
            static_cast<std::uint16_t>(w_param));
        // Control characters that TranslateMessage synthesizes for keys
        // (CR/LF for Enter, BS for Backspace, TAB, ESC) carry no text
        // meaning in the single-line SCP chat edits: their behavior lives
        // in the WM_KEYDOWN path (Enter sends and clears, Backspace edits).
        // Letting them through would re-insert "\r" into the just-cleared
        // buffer, so the next send would carry a leading CR and a held Enter
        // key could re-send. ScpChatWindow::translate_message applies the
        // same filter to the detached chat window. Printable text (>= 0x20)
        // and IME commits are unaffected.
        if (unit < 0x20) {
            pending_high_surrogate_ = L'\0';
            return 0;
        }
        std::wstring codepoint;
        if (is_high_surrogate(unit)) {
            pending_high_surrogate_ = unit;
            return 0;
        }
        if (is_low_surrogate(unit) && pending_high_surrogate_ != L'\0') {
            codepoint.push_back(pending_high_surrogate_);
            codepoint.push_back(unit);
            pending_high_surrogate_ = L'\0';
        } else {
            // An orphaned low surrogate or BMP character; a previously
            // buffered high surrogate without its pair is dropped.
            pending_high_surrogate_ = L'\0';
            codepoint.push_back(unit);
        }
        WindowEvent event{WindowEvent::Type::text_input};
        event.text = wide_to_utf8(codepoint);
        if (!event.text.empty()) {
            emit(event);
        }
        return 0;
    }
    case WM_IME_STARTCOMPOSITION:
        ime_composing_ = true;
        emit({WindowEvent::Type::ime_begin});
        return 0;
    case WM_IME_COMPOSITION: {
        if ((l_param & GCS_RESULTSTR) != 0) {
            const HIMC context = ImmGetContext(window);
            std::wstring result = imm_composition_string(context, GCS_RESULTSTR);
            ImmReleaseContext(window, context);
            WindowEvent event{WindowEvent::Type::ime_commit};
            event.text = wide_to_utf8(result);
            if (!event.text.empty()) {
                ime_composing_ = false;
                emit(event);
            }
        } else if ((l_param & GCS_COMPSTR) != 0) {
            const HIMC context = ImmGetContext(window);
            std::wstring partial = imm_composition_string(context, GCS_COMPSTR);
            ImmReleaseContext(window, context);
            WindowEvent event{WindowEvent::Type::ime_composition};
            event.text = wide_to_utf8(partial);
            if (!event.text.empty()) {
                emit(event);
            }
        }
        return 0;
    }
    case WM_IME_ENDCOMPOSITION:
        if (ime_composing_) {
            ime_composing_ = false;
            emit({WindowEvent::Type::ime_cancel});
        }
        return 0;
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
