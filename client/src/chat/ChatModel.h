#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dhsh {

enum class ChatSpanKind : std::uint8_t {
    text,
    item_link,
    line_break,
};

struct ChatSpan {
    ChatSpanKind kind = ChatSpanKind::text;
    std::string text; // UTF-8 visible text; line breaks use "\n".
    std::string color = "red";
    std::string link; // Evidenced item-link id, not executable markup.
};

struct ChatMessage {
    std::uint32_t channel = 0;
    std::string sender; // UTF-8.
    std::vector<ChatSpan> spans;
};

class ChatModel {
public:
    static constexpr std::size_t max_messages = 200; // Client safeguard.

    using ChangeCallback = std::function<void()>;
    void set_on_changed(ChangeCallback callback) { on_changed_ = std::move(callback); }

    void append(ChatMessage message);
    void set_channel_filter(std::optional<std::uint32_t> channel);
    void set_auto_scroll(bool enabled);
    void scroll(std::int32_t delta_lines);

    const std::vector<ChatMessage>& visible_messages() const noexcept {
        return visible_messages_;
    }
    std::optional<std::uint32_t> channel_filter() const noexcept {
        return channel_filter_;
    }
    std::size_t scroll_offset_lines() const noexcept {
        return scroll_offset_lines_;
    }
    bool auto_scroll() const noexcept {
        return auto_scroll_;
    }

private:
    void rebuild_visible();
    void clamp_scroll();
    void notify_changed() const { if (on_changed_) { on_changed_(); } }

    ChangeCallback on_changed_;
    std::vector<ChatMessage> history_;
    std::vector<ChatMessage> visible_messages_;
    std::optional<std::uint32_t> channel_filter_;
    std::size_t scroll_offset_lines_ = 0;
    bool auto_scroll_ = true;
};

} // namespace dhsh
