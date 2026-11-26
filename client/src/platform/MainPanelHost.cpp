#include "platform/MainPanelHost.h"

#include "resource/NzDecoder.h"
#include "resource/ResourceError.h"
#include "resource/ResourceProvider.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dhsh {
namespace {

struct PanelBinding {
    std::string_view control;
    std::string_view type;
    std::string_view code;
    std::string_view panel;
    std::string_view source;
};

constexpr PanelBinding kPanelBindings[] = {
    {"btn_revert", "FSM_BTN_MSG", "OnRapidResponse", "chat_revert", ""},
    {"chat_font_choose_btn", "FSM_MOUSE_UP", "OnShowColor", "chat_font_choose", ""},
    {"bar_smile", "FSM_BTN_MSG", "OnSelFace", "chat_face", "emote.csv"},
    {"bar_body_action", "FSM_BTN_MSG", "OnFace", "body", "ui/body/body.scp"},
    {"home_btnlist_bg_rightpoint", "FSM_BTN_MSG", "OnHomeBtn", "home_btnlist",
        "ui/home/home_btnlist.scp"},
    {"bar_worldteam", "FSM_BTN_MSG", "OnFindTeam", "team_worldteam",
        "ui/team/team_worldteam.scp"},
    {"bar_worldmap", "FSM_BTN_MSG", "OnSwitchSmallMap", "sys_smallmap_win",
        "ui/main/smallmap.scp"},
    {"daynight_minimapbtn", "FSM_MOUSE_UP", "OnSwitchSmallestMap", "sys_map",
        "ui/main/minimap.scp"},
    {"daynight_mapname", "FSM_MOUSE_UP", "OnSwitchSmallestMap", "sys_map",
        "ui/main/minimap.scp"},
    {"newhelp_btn", "FSM_BTN_MSG", "OnOpenNewHelp", "newhelp",
        "ui/help/newhelp.scp"},
};

struct PanelCloseBinding {
    std::string_view control;
    std::string_view type;
    std::string_view code;
    std::string_view panel;
};

constexpr PanelCloseBinding kPanelCloseBindings[] = {
    {"team_worldteam_close", "FSM_BTN_MSG", "OnClose", "team_worldteam"},
    {"newhelp_close", "FSM_BTN_MSG", "OnCloseWin", "newhelp"},
    {"minimap_close", "FSM_MOUSE_UP", "OnCloseMapWnd", "sys_map"},
};

std::string trim(std::string_view value) {
    constexpr std::string_view whitespace = " \t\r\n";
    const std::size_t first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(whitespace);
    return std::string(value.substr(first, last - first + 1));
}

std::vector<std::string_view> csv_fields(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t begin = 0;
    while (true) {
        const std::size_t comma = line.find(',', begin);
        fields.push_back(line.substr(begin, comma == std::string_view::npos
            ? std::string_view::npos : comma - begin));
        if (comma == std::string_view::npos) {
            return fields;
        }
        begin = comma + 1;
    }
}

int csv_integer(std::string_view field) {
    const std::string value = trim(field);
    int result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{}
        || parsed.ptr != value.data() + value.size()) {
        throw ResourceError("invalid integer in emote.csv: " + value);
    }
    return result;
}

struct EmoteConfig {
    LogicalRect frame;
    std::string background;
    LogicalRect list;
    int item_width = 0;
    int item_height = 0;
    int frame_ms = 0;
};

EmoteConfig load_emote_config(ResourceProvider& resources) {
    std::vector<std::byte> bytes = resources.bytes("emote.csv");
    if (bytes.size() >= 2 && bytes[0] == std::byte{'N'} && bytes[1] == std::byte{'Z'}) {
        bytes = NzDecoder::decode(bytes);
    }
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::vector<std::vector<std::string_view>> rows;
    std::size_t begin = 0;
    while (begin < text.size()) {
        const std::size_t end = text.find('\n', begin);
        const std::string_view line(text.data() + begin,
            (end == std::string::npos ? text.size() : end) - begin);
        if (!trim(line).empty()) {
            rows.push_back(csv_fields(line));
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    if (rows.size() < 5 || rows[0].size() < 5 || rows[1].size() < 2
        || rows[2].size() < 5 || rows[3].size() < 3 || rows[4].size() < 2) {
        throw ResourceError("emote.csv is missing its native frame/list declarations");
    }
    EmoteConfig config{
        {csv_integer(rows[0][1]), csv_integer(rows[0][2]),
            csv_integer(rows[0][3]), csv_integer(rows[0][4])},
        trim(rows[1][1]),
        {csv_integer(rows[2][1]), csv_integer(rows[2][2]),
            csv_integer(rows[2][3]), csv_integer(rows[2][4])},
        csv_integer(rows[3][1]), csv_integer(rows[3][2]), csv_integer(rows[4][1]),
    };
    if (config.frame.width <= 0 || config.frame.height <= 0
        || config.list.width <= 0 || config.list.height <= 0
        || config.item_width <= 0 || config.item_height <= 0 || config.frame_ms <= 0
        || config.background.empty()) {
        throw ResourceError("emote.csv contains an invalid frame/list declaration");
    }
    return config;
}

} // namespace

MainPanelHost::MainPanelHost(ScpWindows& windows, MessageBus& messages,
    ResourceProvider& resources, WindowLoader loader)
    : windows_(windows), messages_(messages), resources_(resources),
      loader_(std::move(loader)) {
    for (const PanelBinding& binding : kPanelBindings) {
        subscriptions_.push_back(messages_.subscribe(std::string(binding.control),
            std::string(binding.type), [this, binding](const UiMessage& message) {
                if (message.code == binding.code) {
                    toggle_panel(binding.panel, binding.source);
                }
            }));
    }
    for (const PanelCloseBinding& binding : kPanelCloseBindings) {
        subscriptions_.push_back(messages_.subscribe(std::string(binding.control),
            std::string(binding.type), [this, binding](const UiMessage& message) {
                if (message.code == binding.code) {
                    close_panel(binding.panel);
                }
            }));
    }
    for (const std::string_view control : {std::string_view("bar_barbtn_show"),
             std::string_view("bar_barbtn_hide")}) {
        subscriptions_.push_back(messages_.subscribe(std::string(control), "FSM_BTN_MSG",
            [this](const UiMessage& message) {
                if (message.code == "OnSwitchSkillList") {
                    toggle_shortcut("skill_shortcut", "ui/user/skillskey.scp",
                        "bar_barbtn_show", "bar_barbtn_hide");
                }
            }));
    }
    for (const std::string_view control : {std::string_view("bar_funcbtn_show"),
             std::string_view("bar_funcbtn_hide")}) {
        subscriptions_.push_back(messages_.subscribe(std::string(control), "FSM_BTN_MSG",
            [this](const UiMessage& message) {
                if (message.code == "OnSwitchFuncList") {
                    toggle_shortcut("funcbtn_shortcut", "ui/main/fucnbtn_shortcut.scp",
                        "bar_funcbtn_show", "bar_funcbtn_hide");
                }
            }));
    }
    // Synchronize only already loaded state. In particular this does not load
    // the skill bar (or any other on-demand panel) during startup.
    synchronize_arrows("skill_shortcut", "bar_barbtn_show", "bar_barbtn_hide");
    synchronize_arrows("funcbtn_shortcut", "bar_funcbtn_show", "bar_funcbtn_hide");
}

void MainPanelHost::toggle_panel(std::string_view panel, std::string_view source) {
    ScpWindow* owner = nullptr;
    Widget* target = nullptr;
    for (const auto& window : windows_) {
        if (Widget* widget = window->find_widget(panel)) {
            owner = window.get();
            target = widget;
            break;
        }
    }
    const bool was_visible = target != nullptr && target->visible();
    if (target == nullptr) {
        if (source.empty()) {
            std::cerr << "SCP panel target missing: " << panel << '\n';
            return;
        }
        try {
            auto window = source == "emote.csv"
                ? load_emote_frame() : loader_(source);
            if (window == nullptr) {
                return;
            }
            owner = window.get();
            windows_.push_back(std::move(window));
            target = owner->find_widget(panel);
        } catch (const std::exception& error) {
            std::cerr << "SCP load failed file=" << source << ": " << error.what() << '\n';
            return;
        }
    }
    if (target == nullptr) {
        std::cerr << "SCP panel target missing: file=" << source
                  << " control=" << panel << '\n';
        return;
    }
    owner->set_widget_subtree_visible(panel, !was_visible);
}

void MainPanelHost::close_panel(std::string_view panel) {
    for (const auto& window : windows_) {
        if (window->find_widget(panel) != nullptr) {
            window->set_widget_subtree_visible(panel, false);
            return;
        }
    }
}

void MainPanelHost::toggle_shortcut(std::string_view panel, std::string_view source,
    std::string_view show_arrow, std::string_view hide_arrow) {
    toggle_panel(panel, source);
    synchronize_arrows(panel, show_arrow, hide_arrow);
}

void MainPanelHost::synchronize_arrows(std::string_view panel,
    std::string_view show_arrow, std::string_view hide_arrow) {
    bool visible = false;
    for (const auto& window : windows_) {
        if (const Widget* target = window->find_widget(panel)) {
            visible = target->visible();
            break;
        }
    }
    for (const auto& window : windows_) {
        if (window->find_widget(show_arrow)) {
            window->set_widget_subtree_visible(show_arrow, !visible);
        }
        if (window->find_widget(hide_arrow)) {
            window->set_widget_subtree_visible(hide_arrow, visible);
        }
    }
}

std::unique_ptr<ScpWindow> MainPanelHost::load_emote_frame() {
    const EmoteConfig emote = load_emote_config(resources_);
    ScpDocument document;
    document.source_file = "emote.csv";
    ScpNode root;
    root.kind = "WINDOW";
    root.type = "WIN";
    root.source_file = document.source_file;
    // FSFaceWnd applies this additional native 800-wide-client offset to the
    // CSV's base RECT (0x46C5A0). This is a configuration-driven frame, not an
    // invented SCP and not an IMGLIST implementation: no emote items are made.
    root.properties = {
        {"NAME", "\"chat_face\""},
        {"RECT", std::to_string(emote.frame.x + 160) + ','
            + std::to_string(emote.frame.y + 120) + ','
            + std::to_string(emote.frame.width) + ','
            + std::to_string(emote.frame.height)},
        {"HIDE", "1"},
        {"BMP", '"' + emote.background + '"'},
        {"NATIVE_LIST_RECT", std::to_string(emote.list.x) + ','
            + std::to_string(emote.list.y) + ',' + std::to_string(emote.list.width)
            + ',' + std::to_string(emote.list.height)},
        {"NATIVE_ITEM_SIZE", std::to_string(emote.item_width) + ','
            + std::to_string(emote.item_height)},
        {"NATIVE_FRAME_MS", std::to_string(emote.frame_ms)},
    };
    document.windows.push_back(std::move(root));
    return std::make_unique<ScpWindow>(std::move(document), resources_, messages_);
}

} // namespace dhsh
