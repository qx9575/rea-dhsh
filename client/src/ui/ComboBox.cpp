#include "ui/ComboBox.h"

#include "render/SoftwareRenderer.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace dhsh {

ComboBox::ComboBox(const Widget& widget, const Widget* drop_list,
    const Widget* edit_part)
    : widget_(widget),
      drop_list_(drop_list),
      edit_part_(edit_part),
      text_color_(scp_color_property(widget.node(), "TEXTCOLOR")
          .value_or(RGB(255, 255, 255))) {
    row_height_ = default_ui_list_row_height();
    if (drop_list_ != nullptr) {
        for (const auto& [key, value] : drop_list_->node().properties) {
            if (key == "ADDTEXT") {
                items_.push_back(scp_unquote(value));
            }
        }
    }
}

bool ComboBox::open_dropdown() {
    if (!enabled()) {
        return false;
    }
    dropdown_visible_ = true;
    return true;
}

bool ComboBox::close_dropdown() {
    const bool was_visible = dropdown_visible_;
    dropdown_visible_ = false;
    return was_visible;
}

bool ComboBox::select(std::size_t index) {
    if (!enabled() || index >= items_.size()) {
        return false;
    }
    selected_index_ = index;
    return true;
}

namespace {

// Do not divide CLIENTRECT by item count: its bottom is the anchor, and a
// taller content set grows upwards above the combo. Row height comes from the
// shared default font's live GDI metrics plus uniform row spacing.

std::int32_t clamp_coordinate(std::int64_t value) {
    return static_cast<std::int32_t>(std::clamp(value,
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()),
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
}

} // namespace

LogicalRect ComboBox::item_area() const {
    if (drop_list_ == nullptr) {
        return {};
    }
    const LogicalRect& bounds = drop_list_->bounds();
    LogicalRect declared = bounds;
    if (const std::optional<LogicalRect>& client = drop_list_->client_rect();
        client.has_value()) {
        declared.x = clamp_coordinate(
            static_cast<std::int64_t>(bounds.x) + client->x);
        declared.y = clamp_coordinate(
            static_cast<std::int64_t>(bounds.y) + client->y);
        declared.width = client->width;
        declared.height = client->height;
    }
    const std::int64_t content_height = std::min<std::int64_t>(
        static_cast<std::int64_t>(items_.size()) * row_height_,
        std::numeric_limits<std::int32_t>::max());
    const std::int64_t bottom =
        static_cast<std::int64_t>(declared.y) + declared.height;
    return {declared.x, clamp_coordinate(bottom - content_height),
        declared.width, static_cast<std::int32_t>(content_height)};
}

std::optional<std::size_t> ComboBox::item_at(LogicalPoint point) const {
    const LogicalRect area = item_area();
    if (items_.empty() || !area.contains(point)) {
        return std::nullopt;
    }
    const std::int64_t offset =
        static_cast<std::int64_t>(point.y) - area.y;
    return static_cast<std::size_t>(offset / row_height_);
}

bool ComboBox::on_left_down(LogicalPoint) {
    if (!enabled()) {
        return false;
    }
    if (dropdown_visible_) {
        close_dropdown();
    } else {
        open_dropdown();
    }
    return true;
}

bool ComboBox::on_left_up(LogicalPoint) {
    return false;
}

bool ComboBox::on_mouse_move(LogicalPoint) {
    return false;
}

bool ComboBox::on_wheel(int) {
    return false;
}

namespace {

bool draw_text_clip(SoftwareRenderer& renderer, const std::string& text,
    const LogicalRect& bounds, COLORREF color) {
    if (text.empty()) {
        return false;
    }
    const std::int64_t left = std::max<std::int64_t>(0, bounds.x);
    const std::int64_t top = std::max<std::int64_t>(0, bounds.y);
    const std::int64_t right = std::min<std::int64_t>(renderer.width(),
        static_cast<std::int64_t>(bounds.x) + bounds.width);
    const std::int64_t bottom = std::min<std::int64_t>(renderer.height(),
        static_cast<std::int64_t>(bounds.y) + bounds.height);
    if (right <= left || bottom <= top) {
        return false;
    }
    const LogicalRect clip{static_cast<std::int32_t>(left),
        static_cast<std::int32_t>(top), static_cast<std::int32_t>(right - left),
        static_cast<std::int32_t>(bottom - top)};
    const Image image = rasterize_text_image(text, bounds, clip, color);
    if (image.width == 0 || image.height == 0) {
        return false;
    }
    return renderer.draw(image, clip.x, clip.y);
}

} // namespace

bool ComboBox::render(SoftwareRenderer& renderer) {
    if (!widget_.visible() || !widget_.supported()) {
        return false;
    }
    bool changed = false;
    if (selected_index_.has_value() && *selected_index_ < items_.size()) {
        // The picked label shows on the EDIT fixed part (the combo's text
        // field, e.g. the bar's 当前/世界 slot); fall back to the combo
        // bounds when the declaration has no EDIT part. ScpWindow calls this
        // after fixedpart images so the overlapping button cannot cover it.
        const Widget& field = edit_part_ != nullptr ? *edit_part_ : widget_;
        if (field.visible()) {
            const COLORREF color =
                scp_color_property(field.node(), "TEXTCOLOR").value_or(text_color_);
            changed = draw_text_clip(renderer, items_[*selected_index_],
                field.bounds(), color);
        }
    }
    return render_dropdown(renderer) || changed;
}

bool ComboBox::render_dropdown(SoftwareRenderer& renderer) {
    if (!widget_.visible() || !widget_.supported() || !dropdown_visible_) {
        return false;
    }
    bool changed = false;
    const LogicalRect area = item_area();
    if (drop_list_ != nullptr) {
        // TXTLIST is a ComboBox-owned presentation part. Its BMP retains the
        // decoded native dimensions and RECT origin; CLIENTRECT positions the
        // independent text rows and never stretches the whole skin.
        if (const Image* image = drop_list_->image()) {
            const LogicalRect& bounds = drop_list_->bounds();
            changed = renderer.draw(*image, bounds.x, bounds.y) || changed;
        }
    }
    if (!items_.empty() && area.height > 0) {
        const COLORREF color = drop_list_ != nullptr
            ? scp_color_property(drop_list_->node(), "TEXTCOLOR").value_or(text_color_)
            : text_color_;
        for (std::size_t index = 0; index < items_.size(); ++index) {
            const LogicalRect row{area.x,
                static_cast<std::int32_t>(
                    static_cast<std::int64_t>(area.y)
                    + static_cast<std::int64_t>(index) * row_height_),
                area.width, row_height_};
            changed = draw_text_clip(
                renderer, items_[index], row, color) || changed;
        }
    }
    return changed;
}

} // namespace dhsh
