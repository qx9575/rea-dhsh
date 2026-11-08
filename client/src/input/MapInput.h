#pragma once

#include "platform/Win32Window.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace dhsh {

struct MapInputView {
    double camera_x = 0.0;
    double camera_y = 0.0;
    double zoom = 1.0;
};

class MapInput {
public:
    explicit MapInput(std::vector<LogicalRect> ui_regions = {});

    std::optional<MapPoint> handle_event(
        const WindowEvent& event, const MapInputView& view);
    bool dragging() const noexcept { return pressed_; }

private:
    bool is_ui(LogicalPoint point) const noexcept;
    std::optional<MapPoint> map_point(
        LogicalPoint point, const MapInputView& view) const;

    std::vector<LogicalRect> ui_regions_;
    LogicalPoint down_point_;
    bool pressed_ = false;
    bool dragged_ = false;
};

} // namespace dhsh
