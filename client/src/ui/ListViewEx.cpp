#include "ui/ListViewEx.h"

#include "render/SoftwareRenderer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace dhsh {
namespace {

int parse_int_prefix(std::string_view value, bool* ok) {
    const std::string text(value);
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || end == nullptr || *end != '\0') {
        *ok = false;
        return 0;
    }
    *ok = true;
    return static_cast<int>(parsed);
}

// Parses "min, max" (the real files include whitespace after the comma).
bool parse_range(const std::string* value, int* low, int* high) {
    if (value == nullptr) {
        return false;
    }
    const std::size_t comma = value->find(',');
    if (comma == std::string::npos) {
        return false;
    }
    bool ok_low = false;
    bool ok_high = false;
    *low = parse_int_prefix(std::string_view(*value).substr(0, comma), &ok_low);
    *high = parse_int_prefix(
        std::string_view(*value).substr(comma + 1), &ok_high);
    return ok_low && ok_high && *high >= *low;
}

bool parse_int(const std::string* value, int* out) {
    if (value == nullptr) {
        return false;
    }
    bool ok = false;
    *out = parse_int_prefix(*value, &ok);
    return ok;
}

COLORREF channel_color(std::string_view name, COLORREF fallback) {
    if (name == "red") {
        return RGB(255, 0, 0);
    }
    if (name == "blue") {
        return RGB(0, 0, 255);
    }
    if (name == "yellow") {
        return RGB(255, 255, 0);
    }
    if (name == "green") {
        return RGB(0, 255, 0);
    }
    return fallback;
}

class TextMetrics {
public:
    explicit TextMetrics(int /*bounds_height*/) {
        font_ = create_default_ui_font();
        dc_ = CreateCompatibleDC(nullptr);
        if (dc_ != nullptr && font_ != nullptr) {
            old_font_ = SelectObject(dc_, font_);
            GetTextMetricsW(dc_, &metrics_);
        }
    }

    ~TextMetrics() {
        if (dc_ != nullptr && old_font_ != nullptr) {
            SelectObject(dc_, old_font_);
        }
        if (font_ != nullptr) {
            DeleteObject(font_);
        }
        if (dc_ != nullptr) {
            DeleteDC(dc_);
        }
    }

    TextMetrics(const TextMetrics&) = delete;
    TextMetrics& operator=(const TextMetrics&) = delete;

    int width_of(const std::wstring& text) const {
        if (dc_ == nullptr || text.empty()) {
            return 0;
        }
        SIZE size{};
        GetTextExtentPoint32W(dc_, text.data(), static_cast<int>(text.size()), &size);
        return size.cx;
    }

    int width_of_utf8(std::string_view text) const {
        return width_of(utf8_to_wide(text));
    }

    int line_height() const {
        return metrics_.tmHeight + metrics_.tmExternalLeading;
    }

private:
    static std::wstring utf8_to_wide(std::string_view text) {
        if (text.empty()) {
            return {};
        }
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (length <= 0) {
            return {};
        }
        std::wstring result(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), result.data(), length);
        return result;
    }

    HDC dc_ = nullptr;
    HFONT font_ = nullptr;
    HGDIOBJ old_font_ = nullptr;
    TEXTMETRICW metrics_{};
};

} // namespace

ListViewEx::ListViewEx(const Widget& widget)
    : widget_(widget),
      text_color_(scp_color_property(widget.node(), "TEXTCOLOR")
          .value_or(RGB(255, 255, 0))) {
    if (const std::string* value = widget.node().property("DRAWTXTTYPE");
        value != nullptr) {
        bool ok = false;
        const int parsed = parse_int_prefix(*value, &ok);
        if (ok) {
            draw_txt_type_ = parsed;
        }
    }
    if (const std::string* value = widget.node().property("TRANSMSG");
        value != nullptr) {
        const std::string normalized = [&] {
            std::string out(*value);
            std::transform(out.begin(), out.end(), out.begin(),
                [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
            return out;
        }();
        trans_msg_ = normalized == "\"1\"" || normalized == "1"
            || normalized == "\"TRUE\"" || normalized == "TRUE";
    }
}

void ListViewEx::attach_scroll(const Widget* scroll, const Widget* thumb) {
    scroll_ = scroll;
    thumb_ = thumb;
    int low = 0;
    int high = 0;
    if (parse_range(scroll != nullptr
                ? scroll->node().property("VALUERGN") : nullptr, &low, &high)) {
        range_.min = low;
        range_.max = high;
    }
    int page = 0;
    if (parse_int(scroll != nullptr ? scroll->node().property("PAGESIZE") : nullptr,
            &page) && page > 0) {
        range_.page = page;
    }
    int initial = range_.min;
    if (parse_int(scroll != nullptr ? scroll->node().property("VALUE") : nullptr,
            &initial)) {
        scroll_value_ = initial;
    } else {
        scroll_value_ = range_.min;
    }
    set_scroll_value(scroll_value_);
}

void ListViewEx::set_scroll_value(int value) {
    scroll_value_ = std::clamp(value, range_.min, range_.max);
    // Entered from the virtual side (wheel, thumb drag, track clicks): the
    // actual row follows the value through the approximate mapping, so a
    // position set this way is only as exact as the virtual range allows.
    first_row_ = value_to_row(static_cast<std::int64_t>(scroll_value_)
        - range_.min);
}

LogicalRect ListViewEx::content_rect() const {
    const LogicalRect& bounds = widget_.bounds();
    if (const std::optional<LogicalRect>& client = widget_.client_rect();
        client.has_value()) {
        return {static_cast<std::int32_t>(
                    static_cast<std::int64_t>(bounds.x) + client->x),
            static_cast<std::int32_t>(
                    static_cast<std::int64_t>(bounds.y) + client->y),
            client->width, client->height};
    }
    return bounds;
}

int ListViewEx::line_height() {
    if (line_height_ <= 0) {
        TextMetrics metrics(widget_.bounds().height);
        line_height_ = std::max(1, metrics.line_height());
    }
    return line_height_;
}

const std::vector<ListViewEx::Row>& ListViewEx::rows() {
    layout();
    return rows_;
}

std::size_t ListViewEx::first_visible_row() {
    layout();
    // The stored row is authoritative; a filter or resize may have shrunk
    // the content below it, so clamp to the last visible page on read.
    return std::min(first_row_, bottom_first_row());
}

std::size_t ListViewEx::value_to_row(std::int64_t offset) {
    const std::size_t bottom = bottom_first_row();
    const std::int64_t span =
        static_cast<std::int64_t>(range_.max) - range_.min;
    if (bottom <= static_cast<std::size_t>(span)) {
        // The content fits the virtual range: one scroll unit per row,
        // clamped to the last visible page, never past the (possibly
        // filtered) row count.
        if (offset <= 0) {
            return 0;
        }
        return static_cast<std::size_t>(std::min<std::int64_t>(
            offset, static_cast<std::int64_t>(bottom)));
    }
    // More rows than the virtual range addresses: spread the scroll values
    // over the whole row space so both range ends stay the exact top and
    // bottom endpoints. Only the bottom endpoint steps one row per unit
    // (offset span-1 lands on bottom-1); near the top a unit step covers
    // about (bottom-1)/(span-1) rows, so offset 1 can land two rows down.
    // The original's mid-range semantics are not evidenced; this mapping
    // only promises endpoint-exact, monotonic steps, so positions reached
    // through the virtual value are only as exact as that range allows.
    if (offset >= span) {
        return bottom;
    }
    if (span <= 1) {
        return 0;
    }
    const std::int64_t scaled =
        (offset * (static_cast<std::int64_t>(bottom) - 1) + (span - 1) / 2)
        / (span - 1);
    return static_cast<std::size_t>(
        std::min<std::int64_t>(scaled, static_cast<std::int64_t>(bottom)));
}

std::int64_t ListViewEx::row_to_value(std::size_t row) {
    const std::size_t bottom = bottom_first_row();
    const std::int64_t span =
        static_cast<std::int64_t>(range_.max) - range_.min;
    if (bottom <= static_cast<std::size_t>(span)) {
        return static_cast<std::int64_t>(std::min(row, bottom));
    }
    if (row >= bottom) {
        return span;
    }
    if (span <= 1) {
        return 0;
    }
    return (static_cast<std::int64_t>(row) * (span - 1)
            + (static_cast<std::int64_t>(bottom) - 1) / 2)
        / (static_cast<std::int64_t>(bottom) - 1);
}

void ListViewEx::set_first_row(std::size_t row) {
    // The row is the authoritative position: store it exactly and only
    // re-sync the virtual value for the thumb/drag display afterwards.
    first_row_ = std::min(row, bottom_first_row());
    scroll_value_ = range_.min
        + static_cast<int>(row_to_value(first_row_));
}

std::size_t ListViewEx::visible_row_capacity() {
    const int height = line_height();
    const LogicalRect content = content_rect();
    return height > 0
        ? static_cast<std::size_t>(std::max(0, content.height) / height)
        : 0;
}

std::size_t ListViewEx::bottom_first_row() {
    layout();
    const std::size_t capacity = visible_row_capacity();
    return rows_.size() > capacity ? rows_.size() - capacity : 0;
}

LogicalRect ListViewEx::thumb_rect() const {
    return thumb_ != nullptr ? thumb_->bounds() : LogicalRect{};
}

LogicalRect ListViewEx::track_rect() const {
    return scroll_ != nullptr ? scroll_->bounds() : LogicalRect{};
}

void ListViewEx::layout() {
    rows_.clear();
    if (model_ == nullptr || !widget_.supported()) {
        return;
    }
    const LogicalRect content = content_rect();
    if (content.width <= 0 || content.height <= 0) {
        return;
    }
    TextMetrics metrics(widget_.bounds().height);
    const int row_height = std::max(1, metrics.line_height());

    auto flush_row = [&](Row&& row) {
        if (!row.segments.empty()) {
            row.height = row_height;
            rows_.push_back(std::move(row));
        }
    };

    Row row;
    row.height = row_height;
    int x = 0;
    for (const ChatMessage& message : model_->visible_messages()) {
        // Every chat message starts on its own visual row; only spans inside
        // one message flow inline (with explicit line_break spans between).
        if (!row.segments.empty()) {
            flush_row(std::move(row));
            row = Row{};
            row.height = row_height;
            x = 0;
        }
        for (const ChatSpan& span : message.spans) {
            if (span.kind == ChatSpanKind::line_break) {
                flush_row(std::move(row));
                row = Row{};
                row.height = row_height;
                x = 0;
                continue;
            }
            const COLORREF color = span.kind == ChatSpanKind::item_link
                ? RGB(255, 255, 0)
                : channel_color(span.color, text_color_);
            std::string piece;
            int piece_x = x;
            std::size_t offset = 0;
            while (offset < span.text.size()) {
                const std::size_t next = utf8_advance(span.text, offset, 1);
                const std::string candidate =
                    piece + std::string(span.text.substr(offset, next - offset));
                const int width = metrics.width_of_utf8(candidate);
                if (!piece.empty() && piece_x + width > content.width) {
                    row.segments.push_back(Segment{piece, color, piece_x,
                        metrics.width_of_utf8(piece), span.kind, span.link});
                    flush_row(std::move(row));
                    row = Row{};
                    row.height = row_height;
                    piece.clear();
                    piece_x = 0;
                    x = 0;
                    piece = std::string(span.text.substr(offset, next - offset));
                } else {
                    piece = std::move(candidate);
                }
                offset = next;
            }
            if (!piece.empty()) {
                const int width = metrics.width_of_utf8(piece);
                row.segments.push_back(Segment{piece, color, piece_x, width,
                    span.kind, span.link});
                x = piece_x + width;
            }
        }
    }
    flush_row(std::move(row));

    int y = 0;
    for (Row& laid : rows_) {
        laid.y = y;
        y += laid.height;
    }
}

bool ListViewEx::on_wheel(int delta) {
    scroll_by(-(delta / WHEEL_DELTA));
    return true;
}

void ListViewEx::drag_to(std::int32_t y) {
    const LogicalRect track = track_rect();
    if (track.height <= 0 || range_.max <= range_.min) {
        return;
    }
    const double fraction = static_cast<double>(y - track.y) / track.height;
    const double value = static_cast<double>(range_.min)
        + fraction * static_cast<double>(range_.max - range_.min);
    set_scroll_value(static_cast<int>(std::lround(value)));
}

bool ListViewEx::on_left_down(LogicalPoint point) {
    const LogicalRect thumb = thumb_rect();
    if (thumb.width > 0 && thumb.height > 0 && thumb.contains(point)) {
        dragging_ = true;
        return true;
    }
    const LogicalRect track = track_rect();
    if (track.width > 0 && track.height > 0 && track.contains(point)
        && range_.page > 0) {
        const std::int32_t thumb_center = thumb.y + thumb.height / 2;
        scroll_by(point.y < thumb_center ? -range_.page : range_.page);
        return true;
    }
    return false;
}

bool ListViewEx::on_mouse_move(LogicalPoint point) {
    if (!dragging_) {
        return false;
    }
    drag_to(point.y);
    return true;
}

bool ListViewEx::on_left_up(LogicalPoint point) {
    if (!dragging_) {
        return false;
    }
    dragging_ = false;
    drag_to(point.y);
    return true;
}

bool ListViewEx::render(SoftwareRenderer& renderer) {
    if (!widget_.visible() || !widget_.supported()) {
        return false;
    }
    layout();
    if (rows_.empty()) {
        return false;
    }
    const LogicalRect content = content_rect();
    const std::int64_t left = std::max<std::int64_t>(0, content.x);
    const std::int64_t top = std::max<std::int64_t>(0, content.y);
    const std::int64_t right = std::min<std::int64_t>(renderer.width(),
        static_cast<std::int64_t>(content.x) + content.width);
    const std::int64_t bottom = std::min<std::int64_t>(renderer.height(),
        static_cast<std::int64_t>(content.y) + content.height);
    if (right <= left || bottom <= top) {
        return false;
    }
    const LogicalRect clip{static_cast<std::int32_t>(left),
        static_cast<std::int32_t>(top), static_cast<std::int32_t>(right - left),
        static_cast<std::int32_t>(bottom - top)};

    const std::size_t first = first_visible_row();
    const std::size_t capacity = visible_row_capacity();
    const std::size_t last = capacity == 0
        ? first : std::min(rows_.size(), first + capacity);
    // Rows carry their absolute layout offset; rebase the first visible row
    // to the content top or scrolled views would draw every row below the
    // clip (nothing renders past the first page).
    const int base_y = first < rows_.size() ? rows_[first].y : 0;
    bool changed = false;
    for (std::size_t index = first; index < last; ++index) {
        const Row& row = rows_[index];
        for (const Segment& segment : row.segments) {
            if (segment.text.empty()) {
                continue;
            }
            const LogicalRect segment_bounds{
                static_cast<std::int32_t>(
                    static_cast<std::int64_t>(content.x) + segment.x),
                static_cast<std::int32_t>(
                    static_cast<std::int64_t>(content.y) + row.y - base_y),
                std::max(segment.width, 1), row.height};
            const Image text = rasterize_text_image(
                segment.text, segment_bounds, clip, segment.color);
            if (text.width != 0 && text.height != 0) {
                changed = renderer.draw(text, clip.x, clip.y) || changed;
            }
        }
    }
    return changed;
}

} // namespace dhsh
