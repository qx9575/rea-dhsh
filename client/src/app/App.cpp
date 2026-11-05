#include "app/App.h"

#include <windows.h>

namespace {

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(window, message, w_param, l_param);
}

} // namespace

namespace dhsh {

int App::logical_width() const noexcept {
    return 800;
}

int App::logical_height() const noexcept {
    return 600;
}

int App::run() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t class_name[] = L"dhsh_client_window";

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;

    if (RegisterClassW(&window_class) == 0) {
        return 1;
    }

    const HWND window = CreateWindowExW(
        0,
        class_name,
        L"dhsh_client",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        logical_width(),
        logical_height(),
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (window == nullptr) {
        return 1;
    }

    ShowWindow(window, SW_SHOW);

    MSG message{};
    while (true) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result == -1) {
            return 1;
        }
        if (result == 0) {
            return static_cast<int>(message.wParam);
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

} // namespace dhsh
