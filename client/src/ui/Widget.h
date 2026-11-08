#pragma once

#include "platform/Win32Window.h"
#include "render/Image.h"
#include "script/ScpParser.h"

#include <optional>
#include <string>

namespace dhsh {

enum class WidgetType {
    window,
    button,
    static_text,
    edit,
    unsupported,
};

class Widget {
public:
    Widget(const ScpNode* node, std::string source_file,
        LogicalRect bounds, bool visible);

    const ScpNode& node() const noexcept { return *node_; }
    const std::string& name() const noexcept { return name_; }
    const std::string& source_file() const noexcept { return source_file_; }
    const std::string& text() const noexcept { return text_; }
    const LogicalRect& bounds() const noexcept { return bounds_; }
    WidgetType type() const noexcept { return type_; }
    bool supported() const noexcept { return type_ != WidgetType::unsupported; }
    bool visible() const noexcept { return visible_; }
    bool enabled() const noexcept { return enabled_; }
    const Image* image() const noexcept;
    bool intercepts_input() const noexcept;

    void set_image(Image image);
    void mark_unsupported() noexcept { type_ = WidgetType::unsupported; }

private:
    const ScpNode* node_ = nullptr;
    std::string source_file_;
    std::string name_;
    std::string text_;
    LogicalRect bounds_;
    WidgetType type_ = WidgetType::unsupported;
    bool visible_ = true;
    bool enabled_ = true;
    std::optional<Image> image_;
};

} // namespace dhsh
