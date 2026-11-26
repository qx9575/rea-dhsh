#include "platform/ScpChatWindow.h"

#include "platform/WicImage.h"
#include "resource/NzDecoder.h"
#include "resource/ResourceError.h"
#include "resource/ResourceProvider.h"
#include "script/ScpParser.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace dhsh {
namespace {

constexpr wchar_t kClassName[] = L"dhsh_scp_chat_window";

ATOM register_chat_window_class() {
    static ATOM atom = 0;
    if (atom == 0) {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = &ScpChatWindow::window_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        window_class.lpszClassName = kClassName;
        atom = RegisterClassExW(&window_class);
    }
    return atom;
}

} // namespace

ScpChatWindow::ScpChatWindow(ResourceProvider& resources,
    std::string_view scp_path, int x, int y, int width, int height)
    : width_(width), height_(height) {
    if (width_ <= 0 || height_ <= 0) {
        throw ResourceError("chat window has an invalid size");
    }
    ScpDocument document = ScpParser::load(
        resources, std::string(scp_path));
    if (!document.windows.empty()) {
        if (const std::string* rect = document.windows.front().property("RECT")) {
            int x = 0;
            int y = 0;
            int w = 0;
            int h = 0;
            const std::string value = scp_unquote(*rect);
            if (std::sscanf(value.c_str(), "%d,%d,%d,%d", &x, &y, &w, &h) == 4
                && w > 0 && h > 0) {
                source_width_ = w;
                // chatmain places its bottom button row at y=480, just past
                // the 640x480 root; include that 20px band in the source.
                source_height_ = h + 20;
            }
        }
    }
    try {
        std::vector<std::byte> tile = resources.bytes(
            "chat/chatskin/skin0.tga");
        if (tile.size() >= 2 && tile[0] == std::byte{'N'}
            && tile[1] == std::byte{'Z'}) {
            tile = NzDecoder::decode(tile);
        }
        background_tile_ = WicImage::decode(tile);
    } catch (const std::exception&) {
        background_tile_ = Image{};
    }
    scp_window_ = std::make_unique<ScpWindow>(
        std::move(document), resources, messages_, ScpWindow::ImageDecoder{});

    if (register_chat_window_class() == 0) {
        throw ResourceError("cannot register chat window class");
    }
    RECT rect{0, 0, width_, height_};
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    window_ = CreateWindowExW(0, kClassName,
        L"\u804A\u5929\u7A97\u53E3",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, y,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        throw ResourceError("cannot create chat window");
    }
    InvalidateRect(window_, nullptr, FALSE);
}

ScpChatWindow::~ScpChatWindow() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
}

LRESULT CALLBACK ScpChatWindow::window_proc(
    HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == WM_NCCREATE) {
        auto* self = reinterpret_cast<ScpChatWindow*>(
            reinterpret_cast<CREATESTRUCTW*>(l_param)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->window_ = window;
        }
    }
    auto* self = reinterpret_cast<ScpChatWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self != nullptr) {
        return self->handle_message(window, message, w_param, l_param);
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

LRESULT ScpChatWindow::handle_message(
    HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_PAINT:
        paint(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        if (scp_window_ != nullptr) {
            const int client_x = static_cast<short>(LOWORD(l_param));
            const int client_y = static_cast<short>(HIWORD(l_param));
            const int x = client_x * source_width_ / width_;
            const int y = client_y * source_height_ / height_;
            WindowEvent event;
            event.type = message == WM_LBUTTONDOWN
                ? WindowEvent::Type::left_button_down
                : WindowEvent::Type::left_button_up;
            event.logical_position = LogicalPoint{x, y};
            scp_window_->handle_event(event);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        closed_ = true;
        window_ = nullptr;
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, w_param, l_param);
    default:
        return DefWindowProcW(window, message, w_param, l_param);
    }
}

void ScpChatWindow::paint(HWND window) {
    PAINTSTRUCT paint_struct{};
    HDC dc = BeginPaint(window, &paint_struct);
    if (scp_window_ != nullptr) {
        SoftwareRenderer renderer(
            static_cast<std::uint32_t>(source_width_),
            static_cast<std::uint32_t>(source_height_));
        scp_window_->render(renderer);
        const Image& target = renderer.target();
        const int tile_width = static_cast<int>(background_tile_.width);
        const int tile_height = static_cast<int>(background_tile_.height);
        std::vector<std::uint32_t> pixels(
            static_cast<std::size_t>(width_) * height_);
        for (int y = 0; y < height_; ++y) {
            const int source_y = y * source_height_ / height_;
            for (int x = 0; x < width_; ++x) {
                const int source_x = x * source_width_ / width_;
                const std::size_t source_offset =
                    (static_cast<std::size_t>(source_y) * source_width_
                        + source_x) * 4;
                const unsigned alpha = std::to_integer<unsigned>(
                    target.rgba[source_offset + 3]);
                unsigned red = std::to_integer<unsigned>(
                    target.rgba[source_offset]);
                unsigned green = std::to_integer<unsigned>(
                    target.rgba[source_offset + 1]);
                unsigned blue = std::to_integer<unsigned>(
                    target.rgba[source_offset + 2]);
                if (alpha < 255) {
                    unsigned bg_red = 30;
                    unsigned bg_green = 47;
                    unsigned bg_blue = 47;
                    if (tile_width > 0 && tile_height > 0) {
                        const int tx = x % tile_width;
                        const int ty = y % tile_height;
                        const std::size_t tile_offset =
                            (static_cast<std::size_t>(ty) * tile_width + tx) * 4;
                        bg_red = std::to_integer<unsigned>(
                            background_tile_.rgba[tile_offset]);
                        bg_green = std::to_integer<unsigned>(
                            background_tile_.rgba[tile_offset + 1]);
                        bg_blue = std::to_integer<unsigned>(
                            background_tile_.rgba[tile_offset + 2]);
                    }
                    red = (red * alpha + bg_red * (255 - alpha) + 127) / 255;
                    green = (green * alpha + bg_green * (255 - alpha) + 127) / 255;
                    blue = (blue * alpha + bg_blue * (255 - alpha) + 127) / 255;
                }
                pixels[static_cast<std::size_t>(y) * width_ + x] =
                    (blue << 16) | (green << 8) | red;
            }
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width_;
        info.bmiHeader.biHeight = -height_;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(dc, 0, 0,
            static_cast<DWORD>(width_), static_cast<DWORD>(height_),
            0, 0, 0, static_cast<UINT>(height_),
            pixels.data(), &info, DIB_RGB_COLORS);
    }
    EndPaint(window, &paint_struct);
}

} // namespace dhsh
