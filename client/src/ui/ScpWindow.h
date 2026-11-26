#pragma once

#include "platform/Win32Window.h"
#include "render/Image.h"
#include "render/SoftwareRenderer.h"
#include "resource/ResourceProvider.h"
#include "script/ScpParser.h"
#include "ui/ComboBox.h"
#include "ui/ListViewEx.h"
#include "ui/MessageBus.h"
#include "ui/RichEdit.h"
#include "ui/Widget.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dhsh {

class FsLuaHost;

struct ScpWindowOptions {
    std::size_t max_widgets = 32768;
    std::size_t max_depth = 128;
    std::size_t max_text_pixels = 16384;
};

struct ScpWindowProfile {
    double layout_ms = 0.0;
    double widget_setup_ms = 0.0;
    double resource_read_ms = 0.0;
    double nz_decode_ms = 0.0;
    double wic_decode_ms = 0.0;
    double proprietary_decode_ms = 0.0;
    double lua_ms = 0.0;
    std::size_t images = 0;
};

class ScpWindow {
public:
    using ImageDecoder = std::function<Image(
        std::string_view logical_path, std::span<const std::byte> data)>;

    ScpWindow(ScpDocument document, ResourceProvider& resources,
        MessageBus& messages, ImageDecoder decoder = {}, ScpWindowOptions options = {},
        ScpWindowProfile* profile = nullptr);
    ~ScpWindow();

    const ScpDocument& document() const noexcept { return document_; }
    const std::vector<Widget>& widgets() const noexcept { return widgets_; }
    const Widget* find_widget(std::string_view name) const;
    Widget* find_widget(std::string_view name);
    Widget* widget_at(std::size_t index) noexcept {
        return index < widgets_.size() ? &widgets_[index] : nullptr;
    }
    // Interaction behavior registered for a widget, or nullptr. Used by
    // callers that own the corresponding widget state (tests, App).
    WidgetBehavior* behavior_for(std::string_view name);
    const Widget* focused_widget() const noexcept { return focused_edit_; }
    void set_chat_model(const ChatModel* model);
    // Runtime layout helpers (FSChatMain::Layout moves whole rows
    // absolutely; see platform/ScpChatWindow).
    // Sets a widget's bounds and translates its whole subtree by the origin
    // delta so descendants keep their internal offsets. False when unknown.
    bool set_widget_bounds(std::string_view name, LogicalRect bounds);
    // Shows or hides a widget and its whole subtree. False when unknown.
    bool set_widget_subtree_visible(std::string_view name, bool visible);
    // Overwrites the parsed CLIENTRECT of a widget (widget-relative).
    // False when unknown.
    bool set_widget_client_rect(std::string_view name, LogicalRect rect);
    const std::vector<std::string>& diagnostics() const noexcept {
        return diagnostics_;
    }
    FsLuaHost* lua_host() noexcept { return lua_host_.get(); }
    std::vector<UiMessage> message_declarations() const;

    std::vector<LogicalRect> input_regions() const;
    // A visible combo dropdown (and its paired release) is modal across all
    // SCP windows, even when the popup overlaps another window's widgets.
    bool owns_popup_event(const WindowEvent& event) const;
    bool handle_event(const WindowEvent& event);
    bool render(SoftwareRenderer& renderer);
    bool render_popups(SoftwareRenderer& renderer);

private:
    void find_unresolved_layout(const ScpNode& node, std::size_t depth);
    void build_node(const ScpNode& node, LogicalPoint parent, bool parent_visible,
        std::size_t depth, bool layout_valid);
    void build_behaviors();
    WidgetBehavior* behavior_for_node(const ScpNode* node);
    // The behavior that owns the widget's events: the widget's own behavior
    // when present, else the nearest ancestor control with one (an SCP
    // FIXEDPART such as a COMBOBOX button/row routes to its COMBOBOX).
    WidgetBehavior* routing_behavior_for(const Widget* widget);
    void index_parent_nodes(const ScpNode& node);
    RichEdit* focused_richedit();
    void set_focus(const Widget* widget);
    void dispatch_messages(const Widget& widget, std::string_view type, int value);
    void load_image(Widget& widget);
    void load_image_path(Widget& widget, std::string path, WidgetImageState state);
    Widget* mutable_widget_for_node(const ScpNode* node);
    std::optional<std::size_t> hit_test(LogicalPoint point) const;
    std::vector<UiMessage> collect_messages(const Widget& widget, std::string_view type) const;

    ScpDocument document_;
    ResourceProvider& resources_;
    MessageBus& messages_;
    ImageDecoder decoder_;
    ScpWindowOptions options_;
    ScpWindowProfile* profile_ = nullptr;
    std::vector<Widget> widgets_;
    std::unordered_map<const ScpNode*, const Widget*> node_widgets_;
    std::unordered_map<const ScpNode*, std::unique_ptr<WidgetBehavior>> behaviors_;
    // Child -> parent node index used to route a fixedpart hit to the
    // behavior of the control that owns it (e.g. a COMBOBOX button part).
    std::unordered_map<const ScpNode*, const ScpNode*> parent_nodes_;
    std::unordered_set<const ScpNode*> unresolved_layout_;
    std::vector<std::string> diagnostics_;
    std::optional<std::size_t> pressed_;
    std::optional<std::size_t> hovered_;
    const Widget* focused_edit_ = nullptr;
    WidgetBehavior* active_behavior_ = nullptr;
    bool popup_click_ = false;
    std::unique_ptr<FsLuaHost> lua_host_;
};

using ScpWindows = std::vector<std::unique_ptr<ScpWindow>>;

// Decodes a UI image from its raw bytes based on the logical path suffix:
// .a16/.apg through the proprietary decoders, .tga through the project's
// TGA decoder, everything else through WIC. Used by ScpWindow and by the
// detached chat window for its skin tile.
Image decode_image(std::string_view path, std::span<const std::byte> data);

bool handle_scp_event(ScpWindows& windows, const WindowEvent& event);
std::vector<LogicalRect> scp_input_regions(const ScpWindows& windows);

} // namespace dhsh
