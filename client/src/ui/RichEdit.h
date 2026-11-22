#pragma once

#include "platform/Win32Window.h"
#include "ui/Widget.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace dhsh {

// Single-line rich edit state for RICHEDIT SCP controls: committed UTF-8
// text, a codepoint-aware cursor, an optional maximum length, and an IME
// composition preview that never leaks into the committed text. The behavior
// owns no message sending; callers dispatch the control's declared
// FSM_KEY_DOWN messages and decide what a key means.
class RichEdit final : public WidgetBehavior {
public:
    // Client safeguard; the real chatmain.scp declares no explicit cap.
    static constexpr std::size_t kDefaultMaxLength = 256;

    explicit RichEdit(const Widget& widget);

    const Widget& widget() const noexcept { return widget_; }
    bool focused() const noexcept { return focused_; }
    void set_focus(bool focused) noexcept { focused_ = focused; }

    const std::string& text() const noexcept { return text_; }
    // Committed text with any in-progress IME composition appended.
    std::string preview_text() const;
    bool composing() const noexcept { return composing_; }

    std::size_t cursor() const noexcept { return cursor_; }
    void set_cursor(std::size_t offset) {
        cursor_ = clamp_boundary(offset);
    }

    std::size_t max_length() const noexcept { return max_length_; }
    void set_max_length(std::size_t codepoints) noexcept {
        max_length_ = codepoints;
    }

    COLORREF text_color() const noexcept { return text_color_; }

    void insert_text(const std::string& utf8);
    bool delete_backward();
    bool delete_forward();
    void move_left();
    void move_right();
    void move_home() noexcept { cursor_ = 0; }
    void move_end() noexcept { cursor_ = text_.size(); }
    // Discards committed text and any IME composition.
    void clear() {
        ime_cancel();
        text_.clear();
        cursor_ = 0;
    }

    // IME composition boundaries. update/commit/cancel all end any active
    // composition as appropriate; commit inserts once at the cursor.
    void ime_begin();
    void ime_update(const std::string& utf8);
    void ime_commit(const std::string& utf8);
    void ime_cancel();

    // Applies editing keys (arrows, home/end, backspace/delete). Returns true
    // when the key was an editing key. Key events are not text; ordinary keys
    // leave the text untouched and are reported to the caller for message
    // dispatch instead.
    bool apply_key(std::uintptr_t key);

    bool on_left_down(LogicalPoint point) override;
    bool on_left_up(LogicalPoint point) override;
    bool on_mouse_move(LogicalPoint point) override;
    bool on_wheel(int delta) override;
    void on_cancelled() override {}
    bool render(SoftwareRenderer& renderer) override;

private:
    std::size_t clamp_boundary(std::size_t offset) const;

    const Widget& widget_;
    std::string text_;
    std::string composition_;
    std::size_t cursor_ = 0;
    std::size_t max_length_ = kDefaultMaxLength;
    COLORREF text_color_ = RGB(255, 255, 255);
    bool focused_ = false;
    bool composing_ = false;
};

} // namespace dhsh
