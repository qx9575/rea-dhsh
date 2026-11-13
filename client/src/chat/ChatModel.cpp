#include "chat/ChatModel.h"

#include <algorithm>
#include <utility>

namespace dhsh {

void ChatModel::append(ChatMessage message) {
    if (history_.size() == max_messages) {
        history_.erase(history_.begin());
    }
    history_.push_back(std::move(message));
    rebuild_visible();
    if (auto_scroll_) {
        scroll_offset_lines_ = 0;
    } else {
        clamp_scroll();
    }
}

void ChatModel::set_channel_filter(std::optional<std::uint32_t> channel) {
    channel_filter_ = channel;
    rebuild_visible();
    clamp_scroll();
}

void ChatModel::set_auto_scroll(bool enabled) {
    auto_scroll_ = enabled;
    if (enabled) {
        scroll_offset_lines_ = 0;
    }
}

void ChatModel::scroll(std::int32_t delta_lines) {
    auto_scroll_ = false;
    const std::int64_t requested = static_cast<std::int64_t>(scroll_offset_lines_)
        + static_cast<std::int64_t>(delta_lines);
    scroll_offset_lines_ = requested <= 0 ? 0 : static_cast<std::size_t>(requested);
    clamp_scroll();
}

void ChatModel::rebuild_visible() {
    visible_messages_.clear();
    for (const auto& message : history_) {
        if (!channel_filter_.has_value() || message.channel == *channel_filter_) {
            visible_messages_.push_back(message);
        }
    }
}

void ChatModel::clamp_scroll() {
    const std::size_t maximum = visible_messages_.empty() ? 0 : visible_messages_.size() - 1;
    scroll_offset_lines_ = std::min(scroll_offset_lines_, maximum);
}

} // namespace dhsh
