#pragma once

#include "platform/Win32Window.h"
#include "render/Image.h"
#include "render/SoftwareRenderer.h"
#include "resource/ResourceProvider.h"
#include "script/ScpParser.h"
#include "ui/MessageBus.h"
#include "ui/Widget.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace dhsh {

class FsLuaHost;

struct ScpWindowOptions {
    std::size_t max_widgets = 32768;
    std::size_t max_depth = 128;
    std::size_t max_text_pixels = 16384;
};

class ScpWindow {
public:
    using ImageDecoder = std::function<Image(
        std::string_view logical_path, std::span<const std::byte> data)>;

    ScpWindow(ScpDocument document, ResourceProvider& resources,
        MessageBus& messages, ImageDecoder decoder = {}, ScpWindowOptions options = {});
    ~ScpWindow();

    const ScpDocument& document() const noexcept { return document_; }
    const std::vector<Widget>& widgets() const noexcept { return widgets_; }
    const Widget* find_widget(std::string_view name) const;
    Widget* find_widget(std::string_view name);
    Widget* widget_at(std::size_t index) noexcept {
        return index < widgets_.size() ? &widgets_[index] : nullptr;
    }
    const std::vector<std::string>& diagnostics() const noexcept {
        return diagnostics_;
    }
    FsLuaHost* lua_host() noexcept { return lua_host_.get(); }
    std::vector<UiMessage> message_declarations() const;

    std::vector<LogicalRect> input_regions() const;
    bool handle_event(const WindowEvent& event);
    bool render(SoftwareRenderer& renderer);

private:
    void find_unresolved_layout(const ScpNode& node, std::size_t depth);
    void build_node(const ScpNode& node, LogicalPoint parent, bool parent_visible,
        std::size_t depth, bool layout_valid);
    void load_image(Widget& widget);
    std::optional<std::size_t> hit_test(LogicalPoint point) const;
    std::vector<UiMessage> collect_messages(const Widget& widget, std::string_view type) const;

    ScpDocument document_;
    ResourceProvider& resources_;
    MessageBus& messages_;
    ImageDecoder decoder_;
    ScpWindowOptions options_;
    std::vector<Widget> widgets_;
    std::unordered_set<const ScpNode*> unresolved_layout_;
    std::vector<std::string> diagnostics_;
    std::optional<std::size_t> pressed_;
    std::unique_ptr<FsLuaHost> lua_host_;
};

using ScpWindows = std::vector<std::unique_ptr<ScpWindow>>;

bool handle_scp_event(ScpWindows& windows, const WindowEvent& event);
std::vector<LogicalRect> scp_input_regions(const ScpWindows& windows);

} // namespace dhsh
