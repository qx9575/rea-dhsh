#include "platform/ScpChatWindow.h"

#include "resource/NzDecoder.h"
#include "resource/ResourceError.h"
#include "resource/ResourceProvider.h"
#include "script/ScpParser.h"
#include "ui/ComboBox.h"
#include "ui/ListViewEx.h"
#include "ui/RichEdit.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <imm.h>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dhsh {
namespace {

// FSChatMain lays out against client height - 0x1c (FUN_00401970 calls
// Layout(cw, ch - 28)); the reserved strip hosts the attached input row.
constexpr int kLayoutReserve = 28;
constexpr DWORD kChatWindowStyle = WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS
    | WS_CAPTION | WS_THICKFRAME; // no WS_SYSMENU: the original has no X.
constexpr DWORD kChatWindowExStyle = WS_EX_WINDOWEDGE;

ATOM register_chat_window_class() {
    static ATOM atom = 0;
    if (atom == 0) {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = &ScpChatWindow::window_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        window_class.lpszClassName = ScpChatWindow::kClassName;
        atom = RegisterClassExW(&window_class);
    }
    return atom;
}

bool is_high_surrogate(wchar_t unit) {
    return unit >= 0xd800 && unit <= 0xdbff;
}

bool is_low_surrogate(wchar_t unit) {
    return unit >= 0xdc00 && unit <= 0xdfff;
}

std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring imm_composition_string(HIMC context, DWORD flags) {
    if (context == nullptr) {
        return {};
    }
    const LONG byte_count = ImmGetCompositionStringW(context, flags, nullptr, 0);
    if (byte_count <= 0) {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(byte_count) / 2, L'\0');
    ImmGetCompositionStringW(context, flags, text.data(), byte_count);
    return text;
}

// The real chatmain.scp declares the bottom rows before the message list
// whose RECT overlaps them; our flat hit test resolves later widgets first,
// so raise the visual stacking order to match the original where the rows
// draw above the list.
void raise_message_list_below_rows(ScpDocument& document) {
    if (document.windows.empty()) {
        return;
    }
    std::vector<ScpNode>& children = document.windows.front().children;
    for (auto it = children.begin(); it != children.end(); ++it) {
        const std::string* name = it->property("NAME");
        if (name != nullptr && scp_unquote(*name) == "chatmain_txtlst_msg") {
            ScpNode list = std::move(*it);
            children.erase(it);
            children.insert(children.begin(), std::move(list));
            return;
        }
    }
}

struct ViewFilterButton {
    const char* control;
    std::optional<std::uint32_t> channel;
};

// Top-strip view filters. Channels 0 (system), 5 (world) and 9 (custom comp1)
// decode from real 0x303f packets; 1 (current) matches the model test usage;
// 2/3/4/6/7/8/10/11 follow the FSChat mode order (static report: modes
// 0..9,0xa..0x12 for the 15 channels + 3 custom). clan/map/worldteam/single/
// pay stay unmapped for lack of evidence.
constexpr ViewFilterButton kViewFilterButtons[] = {
    {"chatmain_switchbtn_com", std::nullopt},
    {"chatmain_switchbtn_cur", 1},
    {"chatmain_switchbtn_team", 2},
    {"chatmain_switchbtn_faction", 3},
    {"chatmain_switchbtn_private", 4},
    {"chatmain_switchbtn_world", 5},
    {"chatmain_switchbtn_trade", 6},
    {"chatmain_switchbtn_system", 0},
    {"chatmain_switchbtn_GM", 7},
    {"chatmain_switchbtn_city", 8},
    {"chat_show_comp1", 9},
    {"chat_show_comp2", 10},
    {"chat_show_comp3", 11},
};

// Channel picker items by ADDTEXT order. 当前/队伍/帮派/私聊/世界 map to
// 1..5 (1 current consistent with the model tests, 5 world evidenced); G M
// and 同城 follow the same inferred order as the top strip. The duplicated
// 竞技 entries and 地图 have no evidence and leave the selection unchanged.
constexpr std::optional<std::uint32_t> kComboChannels[] = {
    1, 2, 3, 4, 5,
    std::nullopt,
    7,
    8,
    std::nullopt,
    std::nullopt,
};

} // namespace

ChatMainLayout layout_chat_main(int client_width, int client_height,
    int switch_row_top, int switch_row_height, int bottom_row_height,
    int input_bar_height, bool attached) {
    ChatMainLayout layout;
    layout.client_width = client_width;
    layout.client_height = client_height;
    layout.content_height = client_height - kLayoutReserve;
    layout.switch_offset_y = switch_row_top + switch_row_height;
    // FSChatMain::Layout (0x490E50): H -= dy; each bottom row moves to
    // (0, H - row_height + dy) and then consumes its height from H. The
    // +dy/-dy cancel, so rows stack at the bottom of content_height.
    int remaining = layout.content_height - layout.switch_offset_y;
    if (attached && input_bar_height > 0) {
        layout.input_bar_visible = true;
        layout.input_bar = {0,
            remaining - input_bar_height + layout.switch_offset_y,
            client_width, input_bar_height};
        remaining -= input_bar_height;
    }
    layout.all_buttons = {0,
        remaining - bottom_row_height + layout.switch_offset_y,
        client_width, bottom_row_height};
    remaining -= bottom_row_height;
    layout.message_list = {0, layout.switch_offset_y, client_width, remaining};
    layout.message_text_width = client_width - 4;
    return layout;
}

ScpChatWindow::ScpChatWindow(ResourceProvider& resources,
    std::string_view scp_path, ChatModel& chat_model, ChatSendSink& send_sink,
    int x, int y, int width, int height)
    : width_(width), height_(height), chat_model_(&chat_model),
      send_sink_(&send_sink) {
    if (width_ <= 0 || height_ <= 0) {
        throw ResourceError("chat window has an invalid size");
    }
    ScpDocument document = ScpParser::load(resources, std::string(scp_path));
    raise_message_list_below_rows(document);
    scp_window_ = std::make_unique<ScpWindow>(
        std::move(document), resources, messages_, ScpWindow::ImageDecoder{});
    scp_window_->set_chat_model(chat_model_);

    // The new version loads ui/chat/chatskin/skin0.tga exactly once and
    // never selects skin2 (WApplication ctor keeps skin_count == 1).
    try {
        std::vector<std::byte> tile = resources.bytes("chat/chatskin/skin0.tga");
        if (tile.size() >= 2 && tile[0] == std::byte{'N'}
            && tile[1] == std::byte{'Z'}) {
            tile = NzDecoder::decode(tile);
        }
        background_tile_ = decode_image("chat/chatskin/skin0.tga", tile);
    } catch (const std::exception&) {
        background_tile_ = Image{};
    }

    subscribe_controls();
    apply_layout();
    log_new_diagnostics();

    if (register_chat_window_class() == 0) {
        throw ResourceError("cannot register chat window class");
    }
    RECT rect{0, 0, width_, height_};
    AdjustWindowRectEx(&rect, kChatWindowStyle, FALSE, kChatWindowExStyle);
    window_ = CreateWindowExW(kChatWindowExStyle, kClassName,
        L"\u804A\u5929\u7A97\u53E3",
        kChatWindowStyle, x, y,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        throw ResourceError("cannot create chat window");
    }
    InvalidateRect(window_, nullptr, FALSE);
}

ScpChatWindow::~ScpChatWindow() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
}

bool ScpChatWindow::visible() const {
    return window_ != nullptr && IsWindowVisible(window_) != FALSE;
}

void ScpChatWindow::show() {
    if (window_ != nullptr) {
        ShowWindow(window_, SW_SHOW);
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void ScpChatWindow::hide() {
    if (window_ != nullptr) {
        ShowWindow(window_, SW_HIDE);
    }
}

void ScpChatWindow::set_attached(bool attached) {
    if (attached_ == attached) {
        return;
    }
    attached_ = attached;
    apply_layout();
    scp_window_->set_widget_subtree_visible("chatmain_barwnd", attached);
    log_new_diagnostics();
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void ScpChatWindow::set_on_switch(SwitchCallback callback) {
    on_switch_ = std::move(callback);
}

void ScpChatWindow::invalidate() {
    if (window_ != nullptr && IsWindowVisible(window_)) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void ScpChatWindow::apply_layout() {
    const Widget* strip = scp_window_->find_widget("chatmain_switchbtn");
    const Widget* allbtn = scp_window_->find_widget("chatmain_allbtn");
    const int switch_top = strip != nullptr ? strip->bounds().y : 0;
    const int switch_height = strip != nullptr ? strip->bounds().height : 0;
    const int bottom_height = allbtn != nullptr ? allbtn->bounds().height : 0;
    const ChatMainLayout layout = layout_chat_main(width_, height_,
        switch_top, switch_height, bottom_height, kDefaultInputBarHeight,
        attached_);
    scp_window_->set_widget_bounds("chatmain_txtlst_msg", layout.message_list);
    scp_window_->set_widget_client_rect("chatmain_txtlst_msg",
        {1, 0, layout.message_text_width, layout.message_list.height});
    scp_window_->set_widget_bounds("chatmain_allbtn", layout.all_buttons);
    scp_window_->set_widget_bounds("chatmain_barwnd", layout.input_bar);
}

void ScpChatWindow::sync_auto_scroll() {
    if (chat_model_ == nullptr || !chat_model_->auto_scroll()) {
        return;
    }
    auto* view = static_cast<ListViewEx*>(
        scp_window_->behavior_for("chatmain_txtlst_msg"));
    if (view == nullptr) {
        return;
    }
    // The row position is authoritative inside the view; the SCROLL
    // VALUERGN value is only its approximate display coordinate, so writing
    // the bottom row pins the view exactly even when the rows outnumber the
    // virtual range.
    view->set_first_row(view->bottom_first_row());
}

void ScpChatWindow::log_new_diagnostics() {
    const std::vector<std::string>& all = scp_window_->diagnostics();
    while (logged_diagnostics_ < all.size()) {
        std::cout << "SCP chatmain diagnostic: " << all[logged_diagnostics_]
                  << std::endl;
        ++logged_diagnostics_;
    }
}

void ScpChatWindow::subscribe_controls() {
    const auto subscribe = [this](std::string control, std::string type,
                              MessageBus::Handler handler) {
        subscriptions_.push_back(messages_.subscribe(
            std::move(control), std::move(type), std::move(handler)));
    };
    auto* view = static_cast<ListViewEx*>(
        scp_window_->behavior_for("chatmain_txtlst_msg"));

    subscribe("chatmain_switch", "FSM_BTN_MSG", [this](const UiMessage& message) {
        if (message.code != "OnSwitchButtonDown") {
            return;
        }
        set_attached(!attached_);
        if (on_switch_) {
            on_switch_();
        }
    });

    const auto set_roll_buttons = [this](bool rolling) {
        scp_window_->set_widget_subtree_visible("chatmain_stop", rolling);
        scp_window_->set_widget_subtree_visible("chatmain_stop2", !rolling);
    };
    subscribe("chatmain_stop", "FSM_BTN_MSG", [this, set_roll_buttons](
            const UiMessage& message) {
        if (message.code != "OnAutoRollBtnDown") {
            return;
        }
        chat_model_->set_auto_scroll(false);
        set_roll_buttons(false);
        if (window_ != nullptr) {
            InvalidateRect(window_, nullptr, FALSE);
        }
    });
    subscribe("chatmain_stop2", "FSM_BTN_MSG", [this, set_roll_buttons](
            const UiMessage& message) {
        if (message.code != "OnAutoRollBtnDown") {
            return;
        }
        chat_model_->set_auto_scroll(true);
        set_roll_buttons(true);
        if (window_ != nullptr) {
            InvalidateRect(window_, nullptr, FALSE);
        }
    });

    for (const ViewFilterButton& button : kViewFilterButtons) {
        subscribe(button.control, "FSM_BTN_MSG",
            [this, channel = button.channel](const UiMessage& message) {
                if (message.code.rfind("OnSetViewChannel", 0) != 0) {
                    return;
                }
                chat_model_->set_channel_filter(channel);
                if (window_ != nullptr) {
                    InvalidateRect(window_, nullptr, FALSE);
                }
            });
    }

    if (view != nullptr) {
        const auto manual_scroll = [this, view](std::size_t first_row) {
            chat_model_->set_auto_scroll(false);
            view->set_first_row(first_row);
            if (window_ != nullptr) {
                InvalidateRect(window_, nullptr, FALSE);
            }
        };
        subscribe("chat_btntop", "FSM_BTN_MSG",
            [this, manual_scroll](const UiMessage& message) {
                if (message.code == "OnSetViewUpAll") {
                    manual_scroll(0);
                }
            });
        subscribe("chat_btnup", "FSM_BTN_MSG",
            [this, view, manual_scroll](const UiMessage& message) {
                if (message.code == "OnSetViewUpOne") {
                    manual_scroll(view->first_visible_row() == 0
                        ? 0 : view->first_visible_row() - 1);
                }
            });
        subscribe("chat_btndown", "FSM_BTN_MSG",
            [this, view, manual_scroll](const UiMessage& message) {
                if (message.code == "OnSetViewDownOne") {
                    manual_scroll(view->first_visible_row() + 1);
                }
            });
        subscribe("chat_btnbottom", "FSM_BTN_MSG",
            [this, view, manual_scroll](const UiMessage& message) {
                if (message.code == "OnSetViewDownAll") {
                    manual_scroll(view->bottom_first_row());
                }
            });
    }

    subscribe("chatmain_txtlstchanel", "FSM_COMBOBOX_SELECTED",
        [this](const UiMessage& message) {
            if (message.code != "OnChannelSel") {
                return;
            }
            auto* combo = static_cast<ComboBox*>(
                scp_window_->behavior_for("chatmain_txtlstchanel"));
            if (combo == nullptr || !combo->selected_index().has_value()
                || *combo->selected_index() >= std::size(kComboChannels)) {
                return;
            }
            if (kComboChannels[*combo->selected_index()].has_value()) {
                selected_channel_ = *kComboChannels[*combo->selected_index()];
            }
        });

    subscribe("chatmain_input_mes", "FSM_KEY_DOWN",
        [this](const UiMessage& message) {
            if (message.code != "OnChatInput" || last_key_down_ != VK_RETURN) {
                return;
            }
            auto* edit = static_cast<RichEdit*>(
                scp_window_->behavior_for("chatmain_input_mes"));
            if (edit == nullptr || edit->text().empty()) {
                return;
            }
            // Single sink boundary: replay has no live 0x4050 and never
            // fakes success; the text is neither echoed into the shared
            // model nor shown as sent.
            send_sink_->send_chat(selected_channel_, edit->text());
            edit->clear();
            if (window_ != nullptr) {
                InvalidateRect(window_, nullptr, FALSE);
            }
        });
}

LRESULT CALLBACK ScpChatWindow::window_proc(
    HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == WM_NCCREATE) {
        auto* self = reinterpret_cast<ScpChatWindow*>(
            reinterpret_cast<CREATESTRUCTW*>(l_param)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->window_ = window;
        }
    }
    auto* self = reinterpret_cast<ScpChatWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self != nullptr) {
        return self->handle_message(window, message, w_param, l_param);
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

void ScpChatWindow::deliver(const WindowEvent& event) {
    if (event.type == WindowEvent::Type::key_down) {
        last_key_down_ = event.key;
    }
    if (scp_window_ == nullptr || !scp_window_->handle_event(event)) {
        return;
    }
    if (event.type == WindowEvent::Type::mouse_wheel
        && event.logical_position.has_value() && chat_model_ != nullptr) {
        // Manual scrolling stop auto-roll, like the original behavior.
        if (const Widget* list = scp_window_->find_widget("chatmain_txtlst_msg");
            list != nullptr && list->bounds().contains(*event.logical_position)) {
            chat_model_->set_auto_scroll(false);
        }
    }
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

std::optional<WindowEvent> ScpChatWindow::translate_message(
    UINT message, WPARAM w_param, LPARAM l_param) {
    using Type = WindowEvent::Type;
    const auto client_point = [&]() -> std::optional<LogicalPoint> {
        const auto x = static_cast<std::int16_t>(LOWORD(l_param));
        const auto y = static_cast<std::int16_t>(HIWORD(l_param));
        return LogicalPoint{x, y};
    };
    switch (message) {
    case WM_MOUSEMOVE:
        return WindowEvent{Type::mouse_move, 0, 0, client_point()};
    case WM_LBUTTONDOWN:
        return WindowEvent{Type::left_button_down, 0, 0, client_point()};
    case WM_LBUTTONUP:
        return WindowEvent{Type::left_button_up, 0, 0, client_point()};
    case WM_RBUTTONDOWN:
        return WindowEvent{Type::right_button_down, 0, 0, client_point()};
    case WM_RBUTTONUP:
        return WindowEvent{Type::right_button_up, 0, 0, client_point()};
    case WM_MOUSEWHEEL: {
        POINT point{static_cast<std::int16_t>(LOWORD(l_param)),
            static_cast<std::int16_t>(HIWORD(l_param))};
        if (window_ != nullptr) {
            ScreenToClient(window_, &point);
        }
        WindowEvent event{Type::mouse_wheel, 0, 0,
            LogicalPoint{static_cast<std::int32_t>(point.x),
                static_cast<std::int32_t>(point.y)}};
        event.wheel_delta = GET_WHEEL_DELTA_WPARAM(w_param);
        return event;
    }
    case WM_KEYDOWN:
        return WindowEvent{Type::key_down, 0, 0, std::nullopt, 0,
            static_cast<std::uintptr_t>(w_param)};
    case WM_KEYUP:
        return WindowEvent{Type::key_up, 0, 0, std::nullopt, 0,
            static_cast<std::uintptr_t>(w_param)};
    case WM_CHAR: {
        const wchar_t unit = static_cast<wchar_t>(
            static_cast<std::uint16_t>(w_param));
        // Control characters that TranslateMessage synthesizes for keys
        // (CR/LF for Enter, BS for Backspace, TAB, ESC) carry no text
        // meaning in the single-line chat edit: their behavior lives in the
        // WM_KEYDOWN path (Enter sends and clears, Backspace edits). Letting
        // them through would re-insert "\r" into the just-cleared buffer, so
        // the next send would carry a leading CR and auto-repeat could
        // re-send. Printable text (>= 0x20) and IME commits are unaffected.
        if (unit < 0x20) {
            return std::nullopt;
        }
        std::wstring codepoint;
        if (is_high_surrogate(unit)) {
            pending_high_surrogate_ = unit;
            return WindowEvent{Type::text_input, 0, 0, std::nullopt, 0, 0, {}};
        }
        if (is_low_surrogate(unit) && pending_high_surrogate_ != L'\0') {
            codepoint.push_back(pending_high_surrogate_);
            codepoint.push_back(unit);
            pending_high_surrogate_ = L'\0';
        } else {
            pending_high_surrogate_ = L'\0';
            codepoint.push_back(unit);
        }
        return WindowEvent{Type::text_input, 0, 0, std::nullopt, 0, 0,
            wide_to_utf8(codepoint)};
    }
    case WM_IME_STARTCOMPOSITION:
        ime_composing_ = true;
        return WindowEvent{Type::ime_begin};
    case WM_IME_COMPOSITION: {
        const HIMC context = ImmGetContext(window_);
        std::wstring text;
        Type type = Type::ime_composition;
        if ((l_param & GCS_RESULTSTR) != 0) {
            text = imm_composition_string(context, GCS_RESULTSTR);
            type = Type::ime_commit;
            ime_composing_ = false;
        } else if ((l_param & GCS_COMPSTR) != 0) {
            text = imm_composition_string(context, GCS_COMPSTR);
        }
        ImmReleaseContext(window_, context);
        return WindowEvent{type, 0, 0, std::nullopt, 0, 0, wide_to_utf8(text)};
    }
    case WM_IME_ENDCOMPOSITION:
        if (ime_composing_) {
            ime_composing_ = false;
            return WindowEvent{Type::ime_cancel};
        }
        return std::nullopt;
    case WM_KILLFOCUS:
        pending_high_surrogate_ = L'\0';
        return WindowEvent{Type::input_cancelled};
    default:
        return std::nullopt;
    }
}

LRESULT ScpChatWindow::handle_message(
    HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_PAINT:
        paint(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        width_ = std::max(1, static_cast<int>(LOWORD(l_param)));
        height_ = std::max(1, static_cast<int>(HIWORD(l_param)));
        apply_layout();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        // The original detached window has no close button and no normal
        // close path; switching is the only way out. Swallow the message so
        // Alt-F4 / WM_CLOSE never destroys the window.
        return 0;
    case WM_DESTROY:
        closed_ = true;
        window_ = nullptr;
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, w_param, l_param);
    default:
        break;
    }
    if (std::optional<WindowEvent> event = translate_message(message, w_param, l_param)) {
        if (event->type != WindowEvent::Type::text_input
            || !event->text.empty()) {
            deliver(*event);
        }
        return 0;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

void ScpChatWindow::paint(HWND window) {
    PAINTSTRUCT paint_struct{};
    HDC dc = BeginPaint(window, &paint_struct);
    if (scp_window_ != nullptr) {
        sync_auto_scroll();
        SoftwareRenderer renderer(static_cast<std::uint32_t>(width_),
            static_cast<std::uint32_t>(height_));
        // The window background is skin0 tiled with its origin at the
        // destination (0,0) top-left corner (type 0x100 tile).
        const int tile_width = static_cast<int>(background_tile_.width);
        const int tile_height = static_cast<int>(background_tile_.height);
        if (tile_width > 0 && tile_height > 0) {
            for (int y = 0; y < height_; y += tile_height) {
                for (int x = 0; x < width_; x += tile_width) {
                    renderer.draw(background_tile_, x, y);
                }
            }
        }
        scp_window_->render(renderer);

        const Image& target = renderer.target();
        std::vector<std::uint32_t> pixels(
            static_cast<std::size_t>(width_) * height_);
        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * width_ + x) * 4;
                const std::uint32_t red = std::to_integer<std::uint32_t>(
                    target.rgba[offset]);
                const std::uint32_t green = std::to_integer<std::uint32_t>(
                    target.rgba[offset + 1]);
                const std::uint32_t blue = std::to_integer<std::uint32_t>(
                    target.rgba[offset + 2]);
                // 32-bit BI_RGB byte order is B,G,R,A.
                pixels[static_cast<std::size_t>(y) * width_ + x] =
                    (red << 16) | (green << 8) | blue;
            }
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width_;
        info.bmiHeader.biHeight = -height_;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(dc, 0, 0,
            static_cast<DWORD>(width_), static_cast<DWORD>(height_),
            0, 0, 0, static_cast<UINT>(height_),
            pixels.data(), &info, DIB_RGB_COLORS);
    }
    EndPaint(window, &paint_struct);
}

} // namespace dhsh
