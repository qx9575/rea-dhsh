#include "input/MapInput.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace dhsh {
namespace {

std::int32_t clamp_i64(std::int64_t value) noexcept {
    return static_cast<std::int32_t>(std::clamp(value,
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()),
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
}

} // namespace

bool LogicalRect::contains(LogicalPoint point) const noexcept {
    const std::int64_t right = static_cast<std::int64_t>(x) + width;
    const std::int64_t bottom = static_cast<std::int64_t>(y) + height;
    return width > 0 && height > 0 && point.x >= x && point.y >= y
        && point.x < right && point.y < bottom;
}

MapInput::MapInput(std::vector<LogicalRect> ui_regions)
    : ui_regions_(std::move(ui_regions)) {
}

std::optional<MapPoint> MapInput::handle_event(
    const WindowEvent& event, const MapInputView& view, bool ui_consumed) {
    using Type = WindowEvent::Type;
    if (event.type == Type::input_cancelled) {
        left_pressed_ = false;
        left_dragged_ = false;
        right_dragging_ = false;
        return std::nullopt;
    }
    if (event.type == Type::right_button_down) {
        right_dragging_ = event.logical_position.has_value() && !ui_consumed
            && !is_ui(*event.logical_position);
        left_pressed_ = false;
        left_dragged_ = false;
        return std::nullopt;
    }
    if (event.type == Type::right_button_up) {
        right_dragging_ = false;
        left_pressed_ = false;
        left_dragged_ = false;
        return std::nullopt;
    }
    if (event.type == Type::left_button_down) {
        left_pressed_ = !right_dragging_ && event.logical_position.has_value()
            && !ui_consumed && !is_ui(*event.logical_position);
        left_dragged_ = false;
        if (left_pressed_) {
            down_point_ = *event.logical_position;
        }
        return std::nullopt;
    }
    if (event.type == Type::mouse_move && left_pressed_) {
        if (!event.logical_position.has_value()
            || event.logical_position->x != down_point_.x
            || event.logical_position->y != down_point_.y) {
            left_dragged_ = true;
        }
        return std::nullopt;
    }
    if (event.type != Type::left_button_up) {
        return std::nullopt;
    }

    const bool click = left_pressed_ && !left_dragged_ && !right_dragging_
        && !ui_consumed && event.logical_position.has_value()
        && !is_ui(*event.logical_position);
    left_pressed_ = false;
    left_dragged_ = false;
    if (!click) {
        return std::nullopt;
    }
    return map_point(*event.logical_position, view);
}

bool MapInput::is_ui(LogicalPoint point) const noexcept {
    for (const LogicalRect& region : ui_regions_) {
        if (region.contains(point)) {
            return true;
        }
    }
    return false;
}

std::optional<MapPoint> MapInput::map_point(
    LogicalPoint point, const MapInputView& view) const {
    if (!std::isfinite(view.camera_x) || !std::isfinite(view.camera_y)
        || !std::isfinite(view.zoom) || view.zoom <= 0.0) {
        return std::nullopt;
    }
    const double map_x = std::floor(view.camera_x + point.x / view.zoom);
    const double map_y = std::floor(view.camera_y + point.y / view.zoom);
    if (map_x < std::numeric_limits<std::int32_t>::min()
        || map_x > std::numeric_limits<std::int32_t>::max()
        || map_y < std::numeric_limits<std::int32_t>::min()
        || map_y > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    return MapPoint{clamp_i64(static_cast<std::int64_t>(map_x)),
        clamp_i64(static_cast<std::int64_t>(map_y))};
}

} // namespace dhsh
