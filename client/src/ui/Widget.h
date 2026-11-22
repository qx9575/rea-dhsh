#pragma once

#include "platform/Win32Window.h"
#include "render/Image.h"
#include "script/ScpParser.h"

#include <optional>
#include <string>
#include <string_view>

namespace dhsh {

class SoftwareRenderer;

enum class WidgetType {
    window,
    button,
    static_text,
    edit,
    list_view_ex,
    rich_edit,
    combo_box,
    scroll,
    unsupported,
};

class Widget;

// Optional per-widget interaction logic owned by ScpWindow and looked up by
// widget. Mouse events arrive in the window's native logical coordinates.
class WidgetBehavior {
public:
    virtual ~WidgetBehavior() = default;
    virtual bool on_left_down(LogicalPoint point) = 0;
    virtual bool on_left_up(LogicalPoint point) = 0;
    virtual bool on_mouse_move(LogicalPoint point) = 0;
    virtual bool on_wheel(int delta) = 0;
    virtual void on_cancelled() = 0;
    virtual bool render(SoftwareRenderer& renderer) = 0;
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
    // Raw CLIENTRECT declaration (widget-relative coordinates), when present.
    // RECT positions the widget inside its parent; CLIENTRECT declares the
    // inner client area used for content layout and remains widget-relative.
    const std::optional<LogicalRect>& client_rect() const noexcept {
        return client_rect_;
    }
    WidgetType type() const noexcept { return type_; }
    bool supported() const noexcept { return type_ != WidgetType::unsupported; }
    bool visible() const noexcept { return visible_; }
    bool enabled() const noexcept { return enabled_; }
    const Image* image() const noexcept;
    bool intercepts_input() const noexcept;

    void set_image(Image image);
    void mark_unsupported() noexcept { type_ = WidgetType::unsupported; }
    void set_visible(bool visible) noexcept { visible_ = visible; }
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    // Shifts the widget without resizing; descendants keep their own
    // offsets, so callers move whole subtrees via ScpWindow.
    void translate(LogicalPoint delta) noexcept {
        bounds_.x += delta.x;
        bounds_.y += delta.y;
    }
    void set_size(std::int32_t width, std::int32_t height) noexcept {
        bounds_.width = width;
        bounds_.height = height;
    }
    void set_client_rect(LogicalRect rect) { client_rect_ = rect; }

private:
    Widget(const ScpNode& node, std::string source_file,
        LogicalRect bounds, bool visible);

    const ScpNode* node_ = nullptr;
    std::string source_file_;
    std::string name_;
    std::string text_;
    LogicalRect bounds_;
    std::optional<LogicalRect> client_rect_;
    WidgetType type_ = WidgetType::unsupported;
    bool visible_ = true;
    bool enabled_ = true;
    std::optional<Image> image_;
};

// Parses an "r,g,b" SCP color property such as TEXTCOLOR.
std::optional<COLORREF> scp_color_property(
    const ScpNode& node, std::string_view name);

// GDI rasterization of one UTF-8 text run clipped to `clip` (window
// coordinates). The result is a clip-sized RGBA image carrying per-pixel
// coverage in alpha, or an empty image when nothing is drawable.
Image rasterize_text_image(std::string_view text, const LogicalRect& bounds,
    const LogicalRect& clip, COLORREF color);

// Moves a byte offset by whole UTF-8 codepoints; the result always lands on
// a codepoint boundary clamped to [0, text.size()].
std::size_t utf8_advance(std::string_view text, std::size_t offset,
    long long delta);

} // namespace dhsh
