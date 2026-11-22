#include "ui/ComboBox.h"

#include "render/SoftwareRenderer.h"

#include <algorithm>
#include <string>
#include <utility>

namespace dhsh {

ComboBox::ComboBox(const Widget& widget, const Widget* drop_list)
    : widget_(widget), drop_list_(drop_list),
      text_color_(scp_color_property(widget.node(), "TEXTCOLOR")
          .value_or(RGB(255, 255, 255))) {
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

LogicalRect ComboBox::item_area() const {
    if (drop_list_ == nullptr) {
        return {};
    }
    const LogicalRect& bounds = drop_list_->bounds();
    if (const std::optional<LogicalRect>& client = drop_list_->client_rect();
        client.has_value()) {
        return {static_cast<std::int32_t>(
                    static_cast<std::int64_t>(bounds.x) + client->x),
            static_cast<std::int32_t>(
                    static_cast<std::int64_t>(bounds.y) + client->y),
            client->width, client->height};
    }
    return bounds;
}

std::optional<std::size_t> ComboBox::item_at(LogicalPoint point) const {
    const LogicalRect area = item_area();
    if (items_.empty() || !area.contains(point)) {
        return std::nullopt;
    }
    const std::int64_t offset =
        static_cast<std::int64_t>(point.y) - area.y;
    return static_cast<std::size_t>(
        offset * static_cast<std::int64_t>(items_.size()) / area.height);
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
        changed = draw_text_clip(renderer, items_[*selected_index_],
            widget_.bounds(), text_color_);
    }
    if (dropdown_visible_) {
        const LogicalRect area = item_area();
        if (!items_.empty() && area.height > 0) {
            const int row_height =
                std::max(1, area.height / static_cast<int>(items_.size()));
            for (std::size_t index = 0; index < items_.size(); ++index) {
                const LogicalRect row{area.x,
                    static_cast<std::int32_t>(
                        static_cast<std::int64_t>(area.y)
                        + static_cast<std::int64_t>(index) * row_height),
                    area.width, row_height};
                changed = draw_text_clip(
                    renderer, items_[index], row, text_color_) || changed;
            }
        }
    }
    return changed;
}

} // namespace dhsh
