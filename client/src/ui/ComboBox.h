#pragma once

#include "platform/Win32Window.h"
#include "ui/Widget.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace dhsh {

// Channel-picker combo box state for COMBOBOX SCP controls. Items come from
// the ADDTEXT labels of the control's TXTLIST fixed part (verbatim, in
// declaration order, duplicates included) and the popup geometry from that
// part's RECT plus CLIENTRECT. The behavior owns no message sending; callers
// dispatch FSM_COMBOBOX_SELECTED when a selection change succeeds.
class ComboBox final : public WidgetBehavior {
public:
    // `drop_list` is the built TXTLIST child widget supplying items and
    // popup geometry; may be null when the declaration has none.
    ComboBox(const Widget& widget, const Widget* drop_list);

    const Widget& widget() const noexcept { return widget_; }
    const std::vector<std::string>& items() const noexcept { return items_; }
    std::optional<std::size_t> selected_index() const noexcept {
        return selected_index_;
    }
    // Live view of the widget ENABLE flag so runtime Lua widget:Enable()
    // toggles stay in sync (never cached at construction).
    bool enabled() const noexcept { return widget_.enabled(); }
    bool dropdown_visible() const noexcept { return dropdown_visible_; }

    // Disabled combos neither open nor change selection.
    bool open_dropdown();
    bool close_dropdown();

    // Selects an item by index; fails when disabled or out of range and
    // leaves the previous selection untouched.
    bool select(std::size_t index);

    // Popup item area: the TXTLIST RECT offset by its CLIENTRECT.
    LogicalRect item_area() const;
    // Maps a window-logical point to a dropdown item index when the point
    // falls inside the item area.
    std::optional<std::size_t> item_at(LogicalPoint point) const;

    COLORREF text_color() const noexcept { return text_color_; }

    // Clicking the combo box toggles its dropdown; disabled combos ignore it.
    bool on_left_down(LogicalPoint point) override;
    bool on_left_up(LogicalPoint point) override;
    bool on_mouse_move(LogicalPoint point) override;
    bool on_wheel(int delta) override;
    void on_cancelled() override {}
    bool render(SoftwareRenderer& renderer) override;

private:
    const Widget& widget_;
    const Widget* drop_list_ = nullptr;
    std::vector<std::string> items_;
    std::optional<std::size_t> selected_index_;
    COLORREF text_color_ = RGB(255, 255, 255);
    bool dropdown_visible_ = false;
};

} // namespace dhsh
