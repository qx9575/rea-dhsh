#include "platform/MainChatBar.h"

#include "platform/ScpChatWindow.h"
#include "ui/ComboBox.h"
#include "ui/RichEdit.h"
#include "ui/ScpWindow.h"

#include <optional>

namespace dhsh {
namespace {

// Channel picker rows by bar.scp ADDTEXT order. 当前/队伍/帮派/私聊/世界 map
// to 1..5 (1 current consistent with the model tests, 5 world evidenced);
// G M -> 7 and 同城 -> 8 follow the same inferred order as the detached
// window's kComboChannels. The bar list has no leading 竞技 and 竞技/地图
// have no evidence, so those rows leave the selection unchanged.
constexpr std::optional<std::uint32_t> kBarChannels[] = {
    1, 2, 3, 4, 5,
    7,
    8,
    std::nullopt,
    std::nullopt,
};

} // namespace

MainChatBar::MainChatBar(ScpWindow& bar, MessageBus& messages,
    ChatSendSink& send_sink, ChatModel* local_preview) {
    // Keep the native picker state in step with selected_channel_'s initial
    // 当前 channel. The label still comes from the SCP ADDTEXT item itself.
    auto* combo = static_cast<ComboBox*>(
        bar.behavior_for("bar_txtlstchanel"));
    if (combo != nullptr && !combo->selected_index().has_value()
        && !combo->items().empty()) {
        combo->select(0);
    }
    subscriptions_.push_back(messages.subscribe("input_mes", "FSM_KEY_DOWN",
        [&bar, &send_sink, local_preview, this](const UiMessage& message) {
            if (message.code != "OnChatInput" || last_key_down_ != VK_RETURN) {
                return;
            }
            auto* edit = static_cast<RichEdit*>(bar.behavior_for("input_mes"));
            if (edit == nullptr || edit->text().empty()) {
                return;
            }
            // Sending remains a single sink operation. Only the explicit
            // offline mode appends a clearly labelled local preview.
            send_sink.send_chat(selected_channel_, edit->text());
            if (local_preview != nullptr) {
                append_local_chat_preview(
                    *local_preview, selected_channel_, edit->text());
            }
            edit->clear();
        }));
    subscriptions_.push_back(messages.subscribe("bar_txtlstchanel",
        "FSM_COMBOBOX_SELECTED",
        [&bar, this](const UiMessage& message) {
            if (message.code != "OnChannelSel") {
                return;
            }
            auto* combo = static_cast<ComboBox*>(
                bar.behavior_for("bar_txtlstchanel"));
            if (combo == nullptr || !combo->selected_index().has_value()
                || *combo->selected_index() >= std::size(kBarChannels)) {
                return;
            }
            if (kBarChannels[*combo->selected_index()].has_value()) {
                selected_channel_ = *kBarChannels[*combo->selected_index()];
            }
        }));
}

} // namespace dhsh
