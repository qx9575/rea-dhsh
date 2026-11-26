#pragma once

#include "ui/MessageBus.h"
#include "ui/ScpWindow.h"

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace dhsh {

class ResourceProvider;

// Connects the native main-bar/daynight buttons to their declared panels.
// SCP panels are loaded on demand through the same loader as the base HUD;
// already loaded windows (including smallmap and funcbtn_shortcut) are reused.
class MainPanelHost {
public:
    using WindowLoader = std::function<std::unique_ptr<ScpWindow>(std::string_view)>;

    MainPanelHost(ScpWindows& windows, MessageBus& messages,
        ResourceProvider& resources, WindowLoader loader);
    MainPanelHost(const MainPanelHost&) = delete;
    MainPanelHost& operator=(const MainPanelHost&) = delete;

private:
    void toggle_panel(std::string_view panel, std::string_view source);
    void close_panel(std::string_view panel);
    void toggle_shortcut(std::string_view panel, std::string_view source,
        std::string_view show_arrow, std::string_view hide_arrow);
    void synchronize_arrows(std::string_view panel,
        std::string_view show_arrow, std::string_view hide_arrow);
    std::unique_ptr<ScpWindow> load_emote_frame();

    ScpWindows& windows_;
    MessageBus& messages_;
    ResourceProvider& resources_;
    WindowLoader loader_;
    std::vector<MessageBus::Subscription> subscriptions_;
};

} // namespace dhsh
