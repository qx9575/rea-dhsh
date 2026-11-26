#pragma once

#include "ui/MessageBus.h"

#include <cstdint>
#include <vector>

namespace dhsh {

class ChatModel;
class ChatSendSink;
class ScpWindow;

// Binds the main HUD chat bar (ui/main/bar.scp) to the single chat send
// sink, mirroring the detached ScpChatWindow bindings: Enter on the focused
// input_mes edit sends its text through the sink exactly once and clears
// the edit, and the bar_txtlstchanel combo selection sets the send channel.
// The host feeds every key down through note_key_down() before the SCP
// windows see the event, so the OnChatInput handler can tell Enter apart
// from the other keys that share the same declared message.
class MainChatBar {
public:
    MainChatBar(ScpWindow& bar, MessageBus& messages, ChatSendSink& send_sink,
        ChatModel* local_preview = nullptr);
    MainChatBar(const MainChatBar&) = delete;
    MainChatBar& operator=(const MainChatBar&) = delete;

    void note_key_down(std::uintptr_t key) noexcept { last_key_down_ = key; }
    std::uint32_t selected_channel() const noexcept { return selected_channel_; }

private:
    std::uintptr_t last_key_down_ = 0;
    std::uint32_t selected_channel_ = 1; // 当前 default; client choice, not protocol evidence.
    std::vector<MessageBus::Subscription> subscriptions_;
};

} // namespace dhsh
