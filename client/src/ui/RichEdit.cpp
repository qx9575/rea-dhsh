#include "ui/RichEdit.h"

#include "render/SoftwareRenderer.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace dhsh {
namespace {

std::size_t codepoint_count(std::string_view text) {
    std::size_t count = 0;
    for (const char value : text) {
        count += (static_cast<unsigned char>(value) & 0xc0U) != 0x80U ? 1U : 0U;
    }
    return count;
}

// Truncates to at most `limit` UTF-8 codepoints.
std::string truncate_codepoints(std::string_view text, std::size_t limit) {
    if (codepoint_count(text) <= limit) {
        return std::string(text);
    }
    std::size_t offset = 0;
    for (std::size_t count = 0; count < limit && offset < text.size(); ++count) {
        do {
            ++offset;
        } while (offset < text.size()
            && (static_cast<unsigned char>(text[offset]) & 0xc0U) == 0x80U);
    }
    return std::string(text.substr(0, offset));
}

} // namespace

RichEdit::RichEdit(const Widget& widget)
    : widget_(widget),
      text_color_(scp_color_property(widget.node(), "TEXTCOLOR")
            .value_or(RGB(255, 255, 255))) {
}

std::string RichEdit::preview_text() const {
    return composing_ ? text_ + composition_ : text_;
}

std::size_t RichEdit::clamp_boundary(std::size_t offset) const {
    offset = std::min(offset, text_.size());
    while (offset > 0
        && (static_cast<unsigned char>(text_[offset]) & 0xc0U) == 0x80U) {
        --offset;
    }
    return offset;
}

void RichEdit::insert_text(const std::string& utf8) {
    if (utf8.empty()) {
        return;
    }
    const std::size_t room = max_length_ > codepoint_count(text_)
        ? max_length_ - codepoint_count(text_)
        : 0;
    std::string accepted = truncate_codepoints(utf8, room);
    if (accepted.empty()) {
        return;
    }
    text_.insert(cursor_, accepted);
    cursor_ += accepted.size();
}

bool RichEdit::delete_backward() {
    if (composing_ || cursor_ == 0) {
        return false;
    }
    const std::size_t previous = utf8_advance(text_, cursor_, -1);
    text_.erase(previous, cursor_ - previous);
    cursor_ = previous;
    return true;
}

bool RichEdit::delete_forward() {
    if (composing_ || cursor_ >= text_.size()) {
        return false;
    }
    const std::size_t next = utf8_advance(text_, cursor_, 1);
    text_.erase(cursor_, next - cursor_);
    return true;
}

void RichEdit::move_left() {
    if (!composing_) {
        cursor_ = utf8_advance(text_, cursor_, -1);
    }
}

void RichEdit::move_right() {
    if (!composing_) {
        cursor_ = utf8_advance(text_, cursor_, 1);
    }
}

void RichEdit::ime_begin() {
    composing_ = true;
    composition_.clear();
}

void RichEdit::ime_update(const std::string& utf8) {
    composing_ = true;
    composition_ = utf8;
}

void RichEdit::ime_commit(const std::string& utf8) {
    composition_.clear();
    composing_ = false;
    insert_text(utf8);
}

void RichEdit::ime_cancel() {
    composition_.clear();
    composing_ = false;
}

bool RichEdit::apply_key(std::uintptr_t key) {
    switch (key) {
    case VK_LEFT:
        move_left();
        return true;
    case VK_RIGHT:
        move_right();
        return true;
    case VK_HOME:
        move_home();
        return true;
    case VK_END:
        move_end();
        return true;
    case VK_BACK:
        delete_backward();
        return true;
    case VK_DELETE:
        delete_forward();
        return true;
    default:
        return false;
    }
}

bool RichEdit::on_left_down(LogicalPoint) {
    return false;
}

bool RichEdit::on_left_up(LogicalPoint) {
    return false;
}

bool RichEdit::on_mouse_move(LogicalPoint) {
    return false;
}

bool RichEdit::on_wheel(int) {
    return false;
}

bool RichEdit::render(SoftwareRenderer& renderer) {
    if (!widget_.visible() || !widget_.supported()) {
        return false;
    }
    const std::string& shown = preview_text();
    if (shown.empty()) {
        return false;
    }
    const LogicalRect& bounds = widget_.bounds();
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
    const Image text = rasterize_text_image(shown, bounds, clip, text_color_);
    if (text.width == 0 || text.height == 0) {
        return false;
    }
    return renderer.draw(text, clip.x, clip.y);
}

} // namespace dhsh
