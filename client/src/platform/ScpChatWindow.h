#pragma once

#include "platform/Win32Window.h"
#include "render/SoftwareRenderer.h"
#include "ui/MessageBus.h"
#include "ui/ScpWindow.h"

#include <memory>
#include <string>
#include <string_view>

namespace dhsh {

class ResourceProvider;

// A separate top-level Win32 window that renders one SCP document. It is used
// for the detached chat window opened by chat_switch, matching the original
// client's separate "聊天窗口" instead of overlaying the game canvas.
class ScpChatWindow {
public:
    ScpChatWindow(ResourceProvider& resources, std::string_view scp_path,
        int x, int y, int width, int height);
    ~ScpChatWindow();

    ScpChatWindow(const ScpChatWindow&) = delete;
    ScpChatWindow& operator=(const ScpChatWindow&) = delete;

    HWND handle() const noexcept { return window_; }
    bool closed() const noexcept { return closed_; }

    static LRESULT CALLBACK window_proc(
        HWND window, UINT message, WPARAM w_param, LPARAM l_param);

private:
    LRESULT handle_message(
        HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    void paint(HWND window);

    HWND window_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int source_width_ = 640;
    int source_height_ = 480;
    bool closed_ = false;
    Image background_tile_;
    MessageBus messages_;
    std::unique_ptr<ScpWindow> scp_window_;
};

} // namespace dhsh
