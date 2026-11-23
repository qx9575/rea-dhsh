#pragma once

#include "chat/ChatModel.h"
#include "platform/Win32Window.h"
#include "ui/Widget.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dhsh {

// Message-list behavior for LISTVIEWEX SCP controls. The control reads the
// shared ChatModel (history, channel filter and network decoding stay owned
// by the model and are never copied here), lays the visible messages out
// into wrapped visual rows with per-span channel colors and item links, and
// owns its scrollbar position parsed from the control's SCROLL fixed part
// (VALUERGN/PAGESIZE/VALUE).
class ListViewEx final : public WidgetBehavior {
public:
    struct ScrollRange {
        int min = 0;
        int max = 0;
        int page = 1;
    };

    struct Segment {
        std::string text; // UTF-8 visible text.
        COLORREF color = RGB(255, 255, 255);
        int x = 0; // Row-relative offset in logical pixels.
        int width = 0;
        ChatSpanKind kind = ChatSpanKind::text;
        std::string link; // Evidenced item-link id, empty for plain text.
    };

    struct Row {
        std::vector<Segment> segments;
        int y = 0; // Content-relative row offset in logical pixels.
        int height = 0;
    };

    explicit ListViewEx(const Widget& widget);

    const Widget& widget() const noexcept { return widget_; }
    void attach_scroll(const Widget* scroll, const Widget* thumb);

    void bind_model(const ChatModel* model) noexcept { model_ = model; }
    const ChatModel* model() const noexcept { return model_; }

    int draw_text_type() const noexcept { return draw_txt_type_; }
    bool trans_msg() const noexcept { return trans_msg_; }
    COLORREF text_color() const noexcept { return text_color_; }

    const ScrollRange& scroll_range() const noexcept { return range_; }
    int scroll_value() const noexcept { return scroll_value_; }
    // Sets the virtual VALUERGN position directly (wheel, thumb drag, track
    // clicks). The actual first row is derived from it through the
    // approximate value->row mapping, so a position set this way is only as
    // exact as the virtual range allows.
    void set_scroll_value(int value);
    // Sets the position by first visual row. The row is stored as the
    // authoritative state and never re-quantized through the virtual range,
    // so per-row navigation stays exact no matter how the rows outnumber the
    // VALUERGN; the virtual scroll value is re-synced afterwards for the
    // thumb/drag display only.
    void set_first_row(std::size_t row);
    void scroll_by(int delta) { set_scroll_value(scroll_value_ + delta); }

    // Lays out the current model contents into wrapped visual rows.
    const std::vector<Row>& rows();
    // First row shown at the current scroll position.
    std::size_t first_visible_row();
    // First row that keeps the newest content visible; the chat window
    // wiring uses it to auto-roll while the shared model auto-scrolls.
    std::size_t bottom_first_row();
    std::size_t visible_row_capacity();
    LogicalRect thumb_rect() const;
    LogicalRect track_rect() const;

    bool on_left_down(LogicalPoint point) override;
    bool on_left_up(LogicalPoint point) override;
    bool on_mouse_move(LogicalPoint point) override;
    bool on_wheel(int delta) override;
    void on_cancelled() override { dragging_ = false; }
    bool render(SoftwareRenderer& renderer) override;

private:
    void layout();
    LogicalRect content_rect() const;
    int line_height();
    void drag_to(std::int32_t y);
    // Approximate display-only mappings between the finite VALUERGN and the
    // (possibly much larger) row space. Neither is exact in both directions
    // (91 virtual values cannot name 173 row positions), so positions set
    // through the virtual value (wheel/drag) inherit that coarseness, while
    // set_first_row() keeps the exact row as the authoritative state.
    std::size_t value_to_row(std::int64_t offset);
    std::int64_t row_to_value(std::size_t row);

    const Widget& widget_;
    const ChatModel* model_ = nullptr;
    const Widget* scroll_ = nullptr;
    const Widget* thumb_ = nullptr;
    ScrollRange range_;
    int scroll_value_ = 0;
    std::size_t first_row_ = 0; // Authoritative scroll position in rows.
    int draw_txt_type_ = 0;
    bool trans_msg_ = false;
    COLORREF text_color_ = RGB(255, 255, 0);
    bool dragging_ = false;
    std::vector<Row> rows_;
    int line_height_ = 0;
};

} // namespace dhsh
