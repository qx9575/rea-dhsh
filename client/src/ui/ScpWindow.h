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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dhsh {

class ScpWindow {
public:
    using ImageDecoder = std::function<Image(
        std::string_view logical_path, std::span<const std::byte> data)>;

    ScpWindow(ScpDocument document, ResourceProvider& resources,
        MessageBus& messages, ImageDecoder decoder = {});

    const ScpDocument& document() const noexcept { return document_; }
    const std::vector<Widget>& widgets() const noexcept { return widgets_; }
    const Widget* find_widget(std::string_view name) const;
    Widget* find_widget(std::string_view name);
    const std::vector<std::string>& diagnostics() const noexcept {
        return diagnostics_;
    }

    std::vector<LogicalRect> input_regions() const;
    bool handle_event(const WindowEvent& event);
    bool render(SoftwareRenderer& renderer);

private:
    void build_node(const ScpNode& node, LogicalPoint parent, bool parent_visible);
    void load_image(Widget& widget);
    std::optional<std::size_t> hit_test(LogicalPoint point) const;
    void dispatch_messages(const Widget& widget, std::string_view type);

    ScpDocument document_;
    ResourceProvider& resources_;
    MessageBus& messages_;
    ImageDecoder decoder_;
    std::vector<Widget> widgets_;
    std::vector<std::string> diagnostics_;
    std::optional<std::size_t> pressed_;
};

} // namespace dhsh
