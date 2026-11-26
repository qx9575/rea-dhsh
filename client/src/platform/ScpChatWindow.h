#pragma once

#include "chat/ChatModel.h"
#include "platform/Win32Window.h"
#include "render/Image.h"
#include "render/SoftwareRenderer.h"
#include "ui/MessageBus.h"
#include "ui/ScpWindow.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace dhsh {

class ResourceProvider;

// The single boundary for outgoing chat text. The detached chat window
// funnels every user send through exactly one sink; the replay client does
// not implement the live 0x4050 path and never reports a fabricated success.
class ChatSendSink {
public:
    virtual ~ChatSendSink() = default;
    virtual void send_chat(std::uint32_t channel, std::string_view text) = 0;
};

// Appends an explicit offline-only local preview, not a network success or
// protocol message. User text remains one opaque text span without parsing.
void append_local_chat_preview(
    ChatModel& model, std::uint32_t channel, std::string_view text);

// Absolute client-area geometry of the detached chat window, mirroring
// FSChatMain::Layout (new Game.exe 0x490E50): the SCP control tree is never
// proportionally scaled; the bottom rows are positioned absolutely for the
// runtime client size. 549x590 is only a measured input, never a constant.
struct ChatMainLayout {
    int client_width = 0;
    int client_height = 0;
    int content_height = 0;      // client height - kLayoutReserve
    int switch_offset_y = 0;     // switch strip top + height (message list top)
    LogicalRect message_list;    // (0, switch_offset_y, cw, remaining)
    int message_text_width = 0;  // cw - 4 (list right inset)
    LogicalRect all_buttons;     // bottom row, absolute
    LogicalRect input_bar;       // attached mode only
    bool input_bar_visible = false;
};

// Computes the absolute layout for a runtime client size. switch_row_* come
// from the parsed SCP switch strip (3,3,637,20 -> dy 23) and
// bottom_row_height from the parsed allbtn row (20); both stay in source
// units. input_bar_height is the caller's measured/derived row height.
ChatMainLayout layout_chat_main(int client_width, int client_height,
    int switch_row_top, int switch_row_height, int bottom_row_height,
    int input_bar_height, bool attached);

// A separate top-level Win32 window rendering the detached chat (FSChatMain)
// in native client coordinates: skin0 tiled from the (0,0) top-left origin,
// the filter strip kept at its SCP position, and the message list plus
// bottom rows absolutely positioned by layout_chat_main. There is no
// WS_SYSMENU: the original window has no close button and WM_CLOSE is not a
// normal close path; switching is the only way out. The window binds the
// caller's shared ChatModel without copying history and sends through the
// caller's ChatSendSink.
class ScpChatWindow {
public:
    static constexpr wchar_t kClassName[] = L"dhsh_scp_chat_window";
    // Attached-mode input row height. The evidenced FSChatMain layout
    // reserves client height - 28 for its rows; the original runtime bar
    // height is not statically closed, so the row uses that reservation
    // instead of a guessed constant.
    static constexpr int kDefaultInputBarHeight = 28;

    using SwitchCallback = std::function<void()>;

    ScpChatWindow(ResourceProvider& resources, std::string_view scp_path,
        ChatModel& chat_model, ChatSendSink& send_sink,
        int x, int y, int width, int height, bool local_preview = false);
    ~ScpChatWindow();

    ScpChatWindow(const ScpChatWindow&) = delete;
    ScpChatWindow& operator=(const ScpChatWindow&) = delete;

    HWND handle() const noexcept { return window_; }
    bool closed() const noexcept { return closed_; }
    bool visible() const;
    void show();
    void hide();

    // Attached mode reveals the FSChatMain barwnd input row; detached mode
    // hides it (the original detached window renders no input row).
    void set_attached(bool attached);
    bool attached() const noexcept { return attached_; }

    // Called after the window's own chatmain_switch button toggles the
    // attached state; lets the host log or react.
    void set_on_switch(SwitchCallback callback);

    // Requests a repaint (e.g. after the shared chat model gained messages
    // while the window had no user input of its own).
    void invalidate();

    // The currently selected send channel (combo selection; 当前 default).
    std::uint32_t selected_channel() const noexcept { return selected_channel_; }

    MessageBus& messages() noexcept { return messages_; }
    ScpWindow* ui() noexcept { return scp_window_.get(); }
    const ScpWindow* ui() const noexcept { return scp_window_.get(); }

    static LRESULT CALLBACK window_proc(
        HWND window, UINT message, WPARAM w_param, LPARAM l_param);

private:
    LRESULT handle_message(
        HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    void apply_layout();
    void sync_auto_scroll();
    void subscribe_controls();
    void log_new_diagnostics();
    void deliver(const WindowEvent& event);
    std::optional<WindowEvent> translate_message(
        UINT message, WPARAM w_param, LPARAM l_param);
    void paint(HWND window);

    HWND window_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool closed_ = false;
    bool attached_ = false;
    std::uint32_t selected_channel_ = 1; // 当前 default; client choice, not protocol evidence.
    ChatModel* chat_model_ = nullptr;
    ChatSendSink* send_sink_ = nullptr;
    bool local_preview_ = false;
    SwitchCallback on_switch_;
    Image background_tile_;
    MessageBus messages_;
    std::unique_ptr<ScpWindow> scp_window_;
    std::vector<MessageBus::Subscription> subscriptions_;
    std::size_t logged_diagnostics_ = 0;
    wchar_t pending_high_surrogate_ = L'\0';
    bool ime_composing_ = false;
    std::uintptr_t last_key_down_ = 0;
};

// Owns the single detached chat window. The main-window chat_switch button
// and the detached chatmain_switch button both funnel into Toggle(), which
// creates the Win32 window once and then alternates visibility; repeated
// toggles never create a second window and destroying the host destroys the
// window (no Win32 window leak).
class ChatWindowHost {
public:
    using Factory = std::function<std::unique_ptr<ScpChatWindow>()>;

    explicit ChatWindowHost(Factory factory) : factory_(std::move(factory)) {}
    ~ChatWindowHost() {
        if (embedded_messages_ != nullptr && close_subscription_ != 0) {
            embedded_messages_->unsubscribe(close_subscription_);
        }
    }

    // The main bar and embedded chat are separate SCP roots sharing one bus.
    // Both bar_close declarations request the same idempotent restore; their
    // presence together must not turn one click into two toggles.
    void bind_main_chat(ScpWindow& embedded, MessageBus& messages) {
        if (embedded_messages_ != nullptr && close_subscription_ != 0) {
            embedded_messages_->unsubscribe(close_subscription_);
        }
        embedded_chat_ = &embedded;
        embedded_messages_ = &messages;
        close_subscription_ = messages.subscribe("bar_close", "FSM_BTN_MSG",
            [this](const UiMessage& message) {
                if (message.code == "OnChatClick"
                    || message.code == "OnShowHideChatInfo") {
                    restore_embedded();
                }
            });
        if (visible()) {
            embedded.set_widget_subtree_visible("chat", false);
        }
    }

    void toggle() {
        if (window_ != nullptr && window_->visible()) {
            restore_embedded();
            return;
        }
        if (window_ == nullptr) {
            window_ = factory_();
            window_->set_on_switch([this] { restore_embedded(); });
        }
        if (embedded_chat_ != nullptr) {
            embedded_chat_->set_widget_subtree_visible("chat", false);
        }
        window_->set_attached(false);
        window_->show();
    }

    void bind_chat_model(ChatModel& model) {
        model.set_on_changed([this] { notify_content_changed(); });
    }

    bool has_window() const noexcept { return window_ != nullptr; }
    bool visible() const noexcept {
        return window_ != nullptr && window_->visible();
    }
    ScpChatWindow* window() const noexcept { return window_.get(); }
    // Repaints the window when the shared chat model changed without any
    // user input inside the window itself (replay appends, filters set by
    // other views, ...). Safe to call with no window.
    void notify_content_changed() const {
        if (window_ != nullptr) {
            window_->invalidate();
        }
    }

private:
    void restore_embedded() {
        if (window_ != nullptr) {
            window_->set_attached(false);
            window_->hide();
        }
        if (embedded_chat_ != nullptr) {
            embedded_chat_->set_widget_subtree_visible("chat", true);
        }
    }

    Factory factory_;
    std::unique_ptr<ScpChatWindow> window_;
    ScpWindow* embedded_chat_ = nullptr;
    MessageBus* embedded_messages_ = nullptr;
    MessageBus::Subscription close_subscription_ = 0;
};

} // namespace dhsh
