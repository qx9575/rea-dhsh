#include "app/App.h"
#include "chat/ChatModel.h"
#include "formats/Apg.h"
#include "input/MapInput.h"
#include "platform/MainChatBar.h"
#include "platform/MainPanelHost.h"
#include "platform/ScpChatWindow.h"
#include "render/SoftwareRenderer.h"
#include "resource/NzDecoder.h"
#include "resource/PakResourceProvider.h"
#include "resource/ResourceProvider.h"
#include "script/ScpParser.h"
#include "ui/ComboBox.h"
#include "ui/ListViewEx.h"
#include "ui/MessageBus.h"
#include "ui/RichEdit.h"
#include "ui/ScpWindow.h"
#include "ui/Widget.h"

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <imm.h>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> result;
    result.reserve(text.size());
    for (const char value : text) {
        result.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    return result;
}

class FakeResources final : public dhsh::ResourceProvider {
public:
    std::vector<std::byte> bytes(std::string_view logical_path) override {
        reads.emplace_back(logical_path);
        return {std::byte{1}};
    }

    bool exists(std::string_view) const override {
        return true;
    }

    std::vector<std::string> reads;
};

std::string resource_key(std::string_view path) {
    std::string result;
    for (const char value : path) {
        const unsigned char byte = static_cast<unsigned char>(value);
        result.push_back(value == '\\' ? '/'
            : static_cast<char>(std::tolower(byte)));
    }
    return result;
}

class MapResources final : public dhsh::ResourceProvider {
public:
    void add(std::string_view path, std::string_view text) {
        files[resource_key(path)] = ::bytes(text);
    }

    std::vector<std::byte> bytes(std::string_view logical_path) override {
        reads.emplace_back(logical_path);
        const auto found = files.find(resource_key(logical_path));
        if (found == files.end()) {
            throw std::runtime_error("missing test resource: " + std::string(logical_path));
        }
        return found->second;
    }

    bool exists(std::string_view logical_path) const override {
        return files.contains(resource_key(logical_path));
    }

    std::unordered_map<std::string, std::vector<std::byte>> files;
    std::vector<std::string> reads;
};

template <typename Callback>
void expect_throw(Callback&& callback, const std::string& message) {
    try {
        callback();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

dhsh::WindowEvent mouse_event(dhsh::WindowEvent::Type type, dhsh::LogicalPoint point) {
    dhsh::WindowEvent event;
    event.type = type;
    event.logical_position = point;
    return event;
}

dhsh::WindowEvent key_event(std::uintptr_t key) {
    dhsh::WindowEvent event;
    event.type = dhsh::WindowEvent::Type::key_down;
    event.key = key;
    return event;
}

dhsh::WindowEvent text_event(std::string text) {
    dhsh::WindowEvent event;
    event.type = dhsh::WindowEvent::Type::text_input;
    event.text = std::move(text);
    return event;
}

dhsh::WindowEvent ime_event(dhsh::WindowEvent::Type type, std::string text = {}) {
    dhsh::WindowEvent event;
    event.type = type;
    event.text = std::move(text);
    return event;
}

dhsh::WindowEvent wheel_event(dhsh::LogicalPoint point, std::int32_t delta) {
    dhsh::WindowEvent event;
    event.type = dhsh::WindowEvent::Type::mouse_wheel;
    event.logical_position = point;
    event.wheel_delta = delta;
    return event;
}

std::string gbk_text(std::initializer_list<unsigned int> bytes) {
    std::string out;
    for (const unsigned int byte : bytes) {
        out.push_back(static_cast<char>(byte));
    }
    return out;
}

// Faithful copy of the real UI/main/chatmain.scp declarations (GB18030 on
// disk) that Task 7 targets. Coordinates, message names and channel labels
// are verbatim; the switch-button strip is trimmed to the all/current/team/
// world/system filter buttons exercised here (the real file declares 15).
// bar_visible drops HIDE=1 from chatmain_barwnd so event routing can be
// exercised on an otherwise offscreen subtree. The bottom bar
// (chatmain_allbtn) is declared after the message list so its buttons stay
// clickable: the real file declares it earlier, which leaves the bar inside
// the LISTVIEWEX RECT's z-order shadow.
std::string chatmain_source(bool bar_visible) {
    std::string source = R"SCP(WINDOW TYPE=WIN
	NAME = "chatmain"
	TRANSPIC = "0"
	RECT = 0,0,640,480

	WINDOW TYPE = WIN
		NAME = "chatmain_barwnd"
		RECT = 1600,1200,360,480
		BMP = "ui/bar/chatmain/chatinputbg.a16"
)SCP";
    if (!bar_visible) {
        source += "\t\tHIDE = \"1\"\n";
    }
    source += R"SCP(
		WINDOW TYPE = COMBOBOX
			NAME = "chatmain_txtlstchanel"
			RECT = -48,0,32,30
			TEXTCOLOR = 255,255,0

			FIXEDPART TYPE = EDIT
				NAME = "chatmain_editchanel"
				RECT = 71,4,32,30
				TEXTCOLOR = 255,255,255
			END
			FIXEDPART TYPE=BUTTON
				NAME = "chatmain_btnchanel"
				RECT = 68,2,32,30
				BMPPATH = "ui/bar/Btnchannel"
			END
			FIXEDPART TYPE = TXTLIST
				NAME = "chatmain_txtlst_channel"
				RECT = 67,-90,40,0
				BMP = "ui/bar/channel.a16"
				CLIENTRECT = 4,2,26,100
				AUTOSCROLL = 1
				MOVEHIGHLIGHT = "1"
				SHOWTYPE = 12
				TEXTCOLOR = 255,255,255
)SCP";
    const std::string channels[] = {
        gbk_text({0xB5, 0xB1, 0xC7, 0xB0}), // 当前
        gbk_text({0xB6, 0xD3, 0xCE, 0xE9}), // 队伍
        gbk_text({0xB0, 0xEF, 0xC5, 0xC9}), // 帮派
        gbk_text({0xCB, 0xBD, 0xC1, 0xC4}), // 私聊
        gbk_text({0xCA, 0xC0, 0xBD, 0xE7}), // 世界
        gbk_text({0xBE, 0xBA, 0xBC, 0xBC}), // 竞技
        "G M",
        gbk_text({0xCD, 0xAC, 0xB3, 0xC7}), // 同城
        gbk_text({0xBE, 0xBA, 0xBC, 0xBC}), // 竞技 (duplicate, kept verbatim)
        gbk_text({0xB5, 0xD8, 0xCD, 0xBC}), // 地图
    };
    for (const std::string& channel : channels) {
        source += "\t\t\t\tADDTEXT = \"" + channel + "\"\n";
    }
    source += R"SCP(
				FIXEDPART TYPE=SCROLL
					RECT = 110,120,20,20
					FIXEDPART TYPE=BUTTON
						RECT = 30,0,64,30
					END
					FIXEDPART TYPE=BUTTON
						RECT = 30,40,64,30
					END
					FIXEDPART TYPE=TWOSTBTN
						RECT = 0,0,20,40
					END
					CLIENTRECT = 0,0,0,100
					VALUERGN = 0, 6
					PAGESIZE = 1
					VALUE = 0
				END
				FIXEDPART TYPE=SCROLL
					FIXEDPART TYPE=BUTTON
					END
					FIXEDPART TYPE=BUTTON
					END
					FIXEDPART TYPE=TWOSTBTN
					END
				END
			END
			MESSAGE FSM_COMBOBOX_SELECTED -1 CODE "OnChannelSel"
		END

		WINDOW TYPE = RICHEDIT
			NAME = "chatmain_input_mes"
			RECT = 56,4,300,16
			TEXTCOLOR = 0,0,0
			MESSAGE FSM_KEY_DOWN -1 CODE "OnChatInput"
		END
	END

	WINDOW TYPE=WIN
		NAME = "chatmain_switchbtn"
		RECT = 3,3,637,20

		WINDOW TYPE=BUTTON
			NAME = "chatmain_switchbtn_com"
			RECT = 0,0,24,18
			BMPPATH = "ui/chat/Btncom"
			MESSAGE FSM_BTN_MSG 1 CODE "OnSetViewChannelAll"
		END
		WINDOW TYPE=BUTTON
			NAME = "chatmain_switchbtn_cur"
			RECT = 19,0,24,18
			BMPPATH = "ui/chat/Btncurrent"
			MESSAGE FSM_BTN_MSG 1 CODE "OnSetViewChannelMap"
		END
		WINDOW TYPE=BUTTON
			NAME = "chatmain_switchbtn_team"
			RECT = 38,0,24,18
			BMPPATH = "ui/chat/Btnteam"
			MESSAGE FSM_BTN_MSG 1 CODE "OnSetViewChannelTeam"
		END
		WINDOW TYPE=BUTTON
			NAME = "chatmain_switchbtn_world"
			RECT = 95,0,24,18
			BMPPATH = "ui/chat/Btnworld"
			MESSAGE FSM_BTN_MSG 1 CODE "OnSetViewChannelWorld"
		END
		WINDOW TYPE=BUTTON
			NAME = "chatmain_switchbtn_system"
			RECT = 133,0,24,18
			BMPPATH = "ui/chat/Btnsystem"
			MESSAGE FSM_BTN_MSG 1 CODE "OnSetViewChannelSystem"
		END
	END

	WINDOW TYPE = LISTVIEWEX
		NAME = "chatmain_txtlst_msg"
		RECT = 3,30,640,480
		CLIENTRECT = 0,0,640,480
		TEXTCOLOR = 255,255,0
		SELHLTYPE = 0
		MESSAGE FSM_MOUSE_DOWN 2 CODE "OnTxtListButtonDown"
		FIXEDPART TYPE=SCROLL
			RECT = 110,120,20,20
			FIXEDPART TYPE=BUTTON
				RECT = 30,0,64,30
			END
			FIXEDPART TYPE=BUTTON
				RECT = 30,40,64,30
			END
			FIXEDPART TYPE=TWOSTBTN
				RECT = 0,0,20,40
			END
			CLIENTRECT = 0,0,0,100
			VALUERGN = 10, 100
			PAGESIZE = 10
			VALUE = 15
		END
		TRANSMSG = "1"
		DRAWTXTTYPE = 2
	END

	WINDOW TYPE=WIN
		NAME = "chatmain_allbtn"
		RECT = 0,480,640,20

		WINDOW TYPE=BUTTON
			NAME = "chatmain_switch"
			RECT = 0,0,24,18
			BMPPATH = "ui/chat/switch"
			MESSAGE FSM_BTN_MSG 1 CODE "OnSwitchButtonDown"
		END
		WINDOW TYPE=BUTTON
			NAME = "chatmain_stop"
			RECT = 23,0,24,18
			BMPPATH = "ui/chat/stop"
			MESSAGE FSM_BTN_MSG 1 CODE "OnAutoRollBtnDown"
		END
		WINDOW TYPE=BUTTON
			NAME = "chatmain_stop2"
			RECT = 23,0,24,18
			BMPPATH = "ui/chat/stop2"
			HIDE = "1"
			MESSAGE FSM_BTN_MSG 1 CODE "OnAutoRollBtnDown"
		END
		WINDOW TYPE=BUTTON
			NAME = "chatmain_screen"
			RECT = 90,0,24,18
			BMPPATH = "ui/chat/screen"
			TIPS = "shield"
			MESSAGE FSM_MOUSE_UP 1 CODE "OnShieldMsg"
			MESSAGE FSM_MOUSE_UP 2 CODE "OnShowShieldLst"
		END
		WINDOW TYPE=BUTTON
			NAME = "chat_btntop"
			RECT = 113,0,16,16
			BMPPATH = "ui/chat/PageBtn/top"
			MESSAGE FSM_BTN_MSG 1 CODE "OnSetViewUpAll"
		END
		WINDOW TYPE=BUTTON
			NAME = "chat_btnup"
			RECT = 133,0,16,16
			BMPPATH = "ui/chat/PageBtn/up"
			MESSAGE FSM_BTN_MSG 1 CODE "OnSetViewUpOne"
		END
		WINDOW TYPE=BUTTON
			NAME = "chat_btndown"
			RECT = 153,0,16,16
			BMPPATH = "ui/chat/PageBtn/down"
			MESSAGE FSM_BTN_MSG 1 CODE "OnSetViewDownOne"
		END
		WINDOW TYPE=BUTTON
			NAME = "chat_btnbottom"
			RECT = 173,0,16,16
			BMPPATH = "ui/chat/PageBtn/bottom"
			MESSAGE FSM_BTN_MSG 1 CODE "OnSetViewDownAll"
		END
	END
END
)SCP";
    return source;
}

dhsh::ScpWindow make_chatmain_window(bool bar_visible, dhsh::ResourceProvider& resources,
    dhsh::MessageBus& bus) {
    return dhsh::ScpWindow(
        dhsh::ScpParser::parse(bytes(chatmain_source(bar_visible)), "chatmain.scp"),
        resources, bus,
        [](std::string_view, std::span<const std::byte>) {
            dhsh::Image image;
            image.width = image.height = 1;
            image.rgba = {std::byte{0xff}, std::byte{0}, std::byte{0}, std::byte{0xff}};
            return image;
        });
}

// Faithful copy of the UI/main/bar.scp input row declarations (GB18030 on
// disk) that the App actually resolves: the provider mounts 800config.pak
// before config.pak, so ui/main/bar.scp is the 800x600-anchored variant
// (barwnd RECT 0,582,800,600 -- the visible input row is the 582..600 strip;
// children with negative y are popups/buttons that open above it). This
// matches the original 1.50.35 runtime at 800x600: the typed text sits in
// the bottom strip (.rea/original_main_typed_20261125.png) and the channel
// menu opens above the bar (.rea/original_main_channels_20261125.png).
// Coordinates, message names and the ADDTEXT channel labels are verbatim,
// including the bar's own item order (当前/队伍/帮派/私聊/世界/G M/同城/
// 竞技/地图 -- the bar list has no leading 竞技 unlike chatmain).
std::string bar_source() {
    std::string source = R"SCP(WINDOW TYPE=WIN
	NAME = "barwnd"
	RECT = 0,582,800,600
	BMP = "ui/bar/inputbg.a16"

	WINDOW TYPE = COMBOBOX
		NAME = "bar_txtlstchanel"
		RECT = -48,80,32,30
		TEXTCOLOR = 255,255,0

		FIXEDPART TYPE = EDIT
			NAME = "bar_editchanel"
			RECT = 71,-77,32,30
			TEXTCOLOR = 255,255,255
		END
		FIXEDPART TYPE=BUTTON
			NAME = "bar_btnchanel"
			RECT = 68,-78,32,30
			BMPPATH = "ui/bar/Btnchannel"
		END
		FIXEDPART TYPE = TXTLIST
			NAME = "bar_txtlst_channel"
			RECT = 67,-220,40,0
			BMP = "ui/bar/channel.a16"
			CLIENTRECT = 4,2,26,140
			AUTOSCROLL = 1
			MOVEHIGHLIGHT = "1"
			SHOWTYPE = 12
			TEXTCOLOR = 255,255,255
)SCP";
    const std::string channels[] = {
        gbk_text({0xB5, 0xB1, 0xC7, 0xB0}), // 当前
        gbk_text({0xB6, 0xD3, 0xCE, 0xE9}), // 队伍
        gbk_text({0xB0, 0xEF, 0xC5, 0xC9}), // 帮派
        gbk_text({0xCB, 0xBD, 0xC1, 0xC4}), // 私聊
        gbk_text({0xCA, 0xC0, 0xBD, 0xE7}), // 世界
        "G M",
        gbk_text({0xCD, 0xAC, 0xB3, 0xC7}), // 同城
        gbk_text({0xBE, 0xBA, 0xBC, 0xBC}), // 竞技
        gbk_text({0xB5, 0xD8, 0xCD, 0xBC}), // 地图
    };
    for (const std::string& channel : channels) {
        source += "\t\t\tADDTEXT = \"" + channel + "\"\n";
    }
    source += R"SCP(
			FIXEDPART TYPE=SCROLL
				RECT = 110,120,20,20
				FIXEDPART TYPE=BUTTON
					RECT = 30,0,64,30
				END
				FIXEDPART TYPE=BUTTON
					RECT = 30,40,64,30
				END
				FIXEDPART TYPE=TWOSTBTN
					RECT = 0,0,20,40
				END
				CLIENTRECT = 0,0,0,100
				VALUERGN = 0, 6
				PAGESIZE = 1
				VALUE = 0
			END
			FIXEDPART TYPE=SCROLL
				FIXEDPART TYPE=BUTTON
				END
				FIXEDPART TYPE=BUTTON
				END
				FIXEDPART TYPE=TWOSTBTN
				END
			END
		END
		MESSAGE FSM_COMBOBOX_SELECTED -1 CODE "OnChannelSel"
	END

	WINDOW TYPE = RICHEDIT
		NAME = "input_mes"
		RECT = 56,4,300,16
		TEXTCOLOR = 0,0,0
		MESSAGE FSM_KEY_DOWN -1 CODE "OnChatInput"
	END
END
)SCP";
    return source;
}

dhsh::ScpWindow make_bar_window(dhsh::ResourceProvider& resources,
    dhsh::MessageBus& bus) {
    return dhsh::ScpWindow(
        dhsh::ScpParser::parse(bytes(bar_source()), "bar.scp"),
        resources, bus,
        [](std::string_view, std::span<const std::byte>) {
            dhsh::Image image;
            image.width = image.height = 1;
            image.rgba = {std::byte{0xff}, std::byte{0}, std::byte{0}, std::byte{0xff}};
            return image;
        });
}

bool has_message(const std::vector<dhsh::UiMessage>& messages, std::string_view control,
    std::string_view type, int value, std::string_view code) {
    for (const dhsh::UiMessage& message : messages) {
        if (message.control == control && message.type == type && message.value == value
            && message.code == code) {
            return true;
        }
    }
    return false;
}

dhsh::ScpDocument sample_document() {
    std::string source = R"SCP(// main interface fixture
# include "config/ui/main/child.scp"
#ifdef WINWIDTH800
WINDOW TYPE = WIN
 NAME = "main"
 RECT = 10,20,300,200
 BMP = "ui/main/panel.a16"
 WINDOW TYPE = BUTTON
  NAME = "ok"
  RECT = 5,6,32,18
  BMPPATH = "ui/main/ok"
  TEXT = "OK"
  MESSAGE FSM_BTN_MSG 1 CODE "OnConfirm"
 END
 WINDOW TYPE = STATIC
  NAME = "caption"
  RECT = 5,30,100,16
  TEXT = "state//keep"
 END
 FIXEDPART TYPE = EDIT
  NAME = "input"
  RECT = 5,50,120,18
  TEXT = "abc"
 END
 WINDOW TYPE = CAROUSEL
  NAME = "mystery"
  RECT = 150,50,20,20
 END
END
#else
WINDOW TYPE = WIN
 NAME = "wrong_branch"
END
#endif
)SCP";
    const std::string chinese = "TEXT = \"\xd6\xd0\xce\xc4\"\n";
    const std::size_t insertion = source.find(" WINDOW TYPE = STATIC");
    source.insert(insertion, chinese);
    return dhsh::ScpParser::parse(bytes(source), "sample.scp");
}

void test_parser_tree_encoding_and_conditionals() {
    const dhsh::ScpDocument document = sample_document();
    check(document.encoding == "gb18030", "SCP fixture did not use GB18030 decoding");
    check(document.includes == std::vector<std::string>{"config/ui/main/child.scp"},
        "include was not preserved");
    check(document.windows.size() == 1, "conditional compilation left multiple roots");
    const dhsh::ScpNode& root = document.windows.front();
    check(root.type == "WIN" && root.property("NAME") != nullptr
            && *root.property("NAME") == "\"main\"",
        "root window properties differ");
    check(root.children.size() == 4, "supported and unsupported children were not retained");
    check(root.children[0].type == "BUTTON" && root.children[1].type == "STATIC"
            && root.children[2].type == "EDIT" && root.children[3].type == "CAROUSEL",
        "SCP child tree differs");
    check(root.children[1].property("TEXT") != nullptr
            && *root.children[1].property("TEXT") == "\"state//keep\"",
        "comment stripping changed quoted text");
    check(root.property("TEXT") != nullptr
            && dhsh::scp_unquote(*root.property("TEXT")) == "中文",
        "GB18030 text was not converted to UTF-8");
}

void test_widgets_resources_render_and_coordinates() {
    FakeResources resources;
    dhsh::MessageBus bus;
    auto decoder = [](std::string_view path, std::span<const std::byte>) {
        dhsh::Image image;
        image.width = 2;
        image.height = 2;
        const bool button = path.find("default.tga") != std::string_view::npos;
        const std::byte red = button ? std::byte{0} : std::byte{0xff};
        const std::byte green = button ? std::byte{0xff} : std::byte{0};
        image.rgba = {red, green, std::byte{0}, std::byte{0xff},
            red, green, std::byte{0}, std::byte{0xff},
            red, green, std::byte{0}, std::byte{0xff},
            red, green, std::byte{0}, std::byte{0xff}};
        return image;
    };
    dhsh::ScpWindow window(sample_document(), resources, bus, decoder);
    const dhsh::Widget* root = window.find_widget("main");
    const dhsh::Widget* button = window.find_widget("ok");
    const dhsh::Widget* edit = window.find_widget("input");
    const dhsh::Widget* unknown = window.find_widget("mystery");
    check(root != nullptr && button != nullptr && edit != nullptr && unknown != nullptr,
        "SCP window did not create every widget node");
    check(root->bounds() == dhsh::LogicalRect{10, 20, 300, 200},
        "root RECT was not interpreted as x,y,width,height");
    check(button->bounds() == dhsh::LogicalRect{15, 26, 32, 18},
        "button coordinates are not relative to its parent");
    check(button->image() != nullptr && root->image() != nullptr,
        "BMP/BMPPATH resources were not loaded");
    check(resources.reads == std::vector<std::string>{
              "ui/main/panel.a16", "ui/main/ok/default.tga",
              "ui/main/ok/mouseon.tga", "ui/main/ok/pressed.tga",
              "ui/main/ok/disable.tga"},
        "widget normal/hover/pressed/disabled resource paths differ");
    check(!unknown->supported(), "unknown control silently became supported");
    bool unsupported_found = false;
    for (const std::string& diagnostic : window.diagnostics()) {
        if (diagnostic.find("Unsupported") != std::string::npos
            && diagnostic.find("sample.scp") != std::string::npos
            && diagnostic.find("mystery") != std::string::npos
            && diagnostic.find("CAROUSEL") != std::string::npos) {
            unsupported_found = true;
        }
    }
    check(unsupported_found, "unknown control diagnostic lacks file, name, or type");

    dhsh::SoftwareRenderer renderer(400, 300);
    check(window.render(renderer), "visible SCP images did not draw");
    const std::size_t panel_pixel = (20U * 400U + 10U) * 4U;
    const std::size_t button_pixel = (26U * 400U + 15U) * 4U;
    check(renderer.target().rgba[panel_pixel] == std::byte{0xff}
            && renderer.target().rgba[panel_pixel + 1] == std::byte{0},
        "root BMP was not rendered at its logical coordinates");
    check(renderer.target().rgba[button_pixel] == std::byte{0}
            && renderer.target().rgba[button_pixel + 1] == std::byte{0xff},
        "button image was not rendered above its parent");
}

void test_message_dispatch_and_input_isolation() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window(sample_document(), resources, bus,
        [](std::string_view, std::span<const std::byte>) {
            dhsh::Image image;
            image.width = image.height = 1;
            image.rgba = {std::byte{0xff}, std::byte{0}, std::byte{0}, std::byte{0xff}};
            return image;
        });
    int calls = 0;
    bus.subscribe("ok", "FSM_BTN_MSG", [&](const dhsh::UiMessage& message) {
        ++calls;
        check(message.code == "OnConfirm", "message code=" + message.code);
        check(message.value == 1, "message value=" + std::to_string(message.value));
        check(message.control == "ok", "message control=" + message.control);
        check(message.source_file == "sample.scp", "message file=" + message.source_file);
    });
    using Type = dhsh::WindowEvent::Type;
    check(window.handle_event(mouse_event(Type::left_button_down, {16, 27})),
        "button down was not consumed by UI");
    check(window.handle_event(mouse_event(Type::left_button_up, {16, 27})),
        "button up was not consumed by UI");
    check(calls == 1, "button click did not dispatch exactly one message");
    bus.dispatch(dhsh::UiMessage{"other", "FSM_BTN_MSG", 1, "OnConfirm", "sample.scp"});
    bus.dispatch(dhsh::UiMessage{"ok", "FSM_EDIT_MSG", 1, "OnConfirm", "sample.scp"});
    check(calls == 1, "message registration was not keyed by control and message type");
    check(!bus.dispatch(dhsh::UiMessage{"missing", "FSM_BTN_MSG", 1, "OnMissing", "sample.scp"})
            && calls == 1,
        "missing control unexpectedly invoked a handler");
    check(!bus.diagnostics().empty()
            && bus.diagnostics().back().find("Unsupported") != std::string::npos
            && bus.diagnostics().back().find("sample.scp") != std::string::npos
            && bus.diagnostics().back().find("missing") != std::string::npos,
        "unknown API diagnostic lacks Unsupported, file, or control");

    dhsh::MapInput map_input(window.input_regions());
    const dhsh::MapInputView view{100.0, 50.0, 1.0};
    map_input.handle_event(mouse_event(Type::left_button_down, {16, 27}), view);
    check(!map_input.handle_event(mouse_event(Type::left_button_up, {16, 27}), view),
        "UI click penetrated into map navigation");
    map_input.handle_event(mouse_event(Type::left_button_down, {350, 250}), view);
    check(!map_input.handle_event(mouse_event(Type::left_button_up, {16, 27}), view),
        "map press released over UI penetrated into navigation");
    map_input.handle_event(mouse_event(Type::left_button_down, {350, 250}), view);
    const auto target = map_input.handle_event(
        mouse_event(Type::left_button_up, {350, 250}), view);
    check(target == std::optional<dhsh::MapPoint>{{450, 300}},
        "non-UI map click was incorrectly blocked");
}

void test_duplicate_visible_button_layout_is_unsupported() {
    const std::string source = R"SCP(#include "config/ui/main/layout.lua"
WINDOW TYPE = WIN
 NAME = "shortcuts"
 RECT = 473,167,304,28
 WINDOW TYPE = BUTTON
  NAME = "first"
  RECT = 0,0,32,24
  BMPPATH = "ui/main/first"
 END
 WINDOW TYPE = button
  NAME = "second"
  RECT = 0,0,32,24
  BMPPATH = "ui/main/second"
 END
END
)SCP";
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window(dhsh::ScpParser::parse(bytes(source), "layout.scp"),
        resources, bus, [](std::string_view, std::span<const std::byte>) {
            dhsh::Image image;
            image.width = image.height = 1;
            image.rgba = {std::byte{0xff}, std::byte{0}, std::byte{0}, std::byte{0xff}};
            return image;
        });
    const dhsh::Widget* first = window.find_widget("first");
    const dhsh::Widget* second = window.find_widget("second");
    check(first != nullptr && second != nullptr, "unresolved buttons were not retained");
    check(!first->supported() && !second->supported(),
        "overlapping visible buttons were presented as resolved static layout");
    check(window.input_regions().empty(),
        "unresolved shortcut container or buttons intercept map input");
    dhsh::SoftwareRenderer renderer(800, 600);
    check(!window.render(renderer), "unresolved shortcut buttons were drawn");
    check(!window.handle_event(mouse_event(
              dhsh::WindowEvent::Type::left_button_down, {480, 175})),
        "unresolved shortcut button accepted a click");
    bool first_diagnostic = false;
    bool second_diagnostic = false;
    for (const std::string& diagnostic : window.diagnostics()) {
        first_diagnostic = first_diagnostic || (diagnostic.find("Unsupported") != std::string::npos
            && diagnostic.find("layout.scp") != std::string::npos
            && diagnostic.find("first") != std::string::npos);
        second_diagnostic = second_diagnostic || (diagnostic.find("Unsupported") != std::string::npos
            && diagnostic.find("layout.scp") != std::string::npos
            && diagnostic.find("second") != std::string::npos);
    }
    check(first_diagnostic && second_diagnostic,
        "unresolved layout diagnostics do not identify both file and controls");
}

void test_parser_input_depth_and_node_budgets() {
    dhsh::ScpParseOptions options;
    options.max_input_bytes = 4;
    expect_throw([&] { dhsh::ScpParser::parse(bytes("12345"), "budget.scp", options); },
        "oversized SCP input was accepted");
    options = {};
    options.max_depth = 2;
    const std::string deep = "WINDOW TYPE=WIN\nWINDOW TYPE=WIN\nWINDOW TYPE=WIN\nEND\nEND\nEND\n";
    expect_throw([&] { dhsh::ScpParser::parse(bytes(deep), "deep.scp", options); },
        "SCP depth budget was not enforced");
    options = {};
    options.max_nodes = 1;
    expect_throw([&] {
        dhsh::ScpParser::parse(bytes("WINDOW TYPE=WIN\nEND\nWINDOW TYPE=WIN\nEND\n"),
            "nodes.scp", options);
    }, "SCP node budget was not enforced");
}

void test_safe_include_expansion() {
    MapResources resources;
    resources.add("ui/main/root.scp", "#include \"config/ui/main/child.scp\"\n"
        "WINDOW TYPE=WIN\nNAME=\"root\"\nEND\n");
    resources.add("ui/main/child.scp",
        "WINDOW TYPE=STATIC\nNAME=\"included\"\nEND\n");
    const dhsh::ScpDocument document = dhsh::ScpParser::load(
        resources, "ui/main/root.scp");
    check(document.windows.size() == 2, "SCP include was not expanded");
    check(document.windows[1].source_file == "ui/main/child.scp",
        "included node lost its source file");

    resources.add("a.scp", "#include \"b.scp\"\n");
    resources.add("b.scp", "#include \"a.scp\"\n");
    expect_throw([&] { dhsh::ScpParser::load(resources, "a.scp"); },
        "include cycle was accepted");
    resources.add("escape.scp", "#include \"../outside.scp\"\n");
    expect_throw([&] { dhsh::ScpParser::load(resources, "escape.scp"); },
        "parent include traversal was accepted");
    resources.add("absolute.scp", "#include \"C:/outside.scp\"\n");
    expect_throw([&] { dhsh::ScpParser::load(resources, "absolute.scp"); },
        "absolute include path was accepted");

    resources.add("depth-root.scp", "#include \"depth-child.scp\"\n");
    resources.add("depth-child.scp", "#include \"depth-grandchild.scp\"\n");
    resources.add("depth-grandchild.scp", "");
    dhsh::ScpParseOptions options;
    options.max_include_depth = 1;
    expect_throw([&] {
        dhsh::ScpParser::load(resources, "depth-root.scp", options);
    }, "include depth budget was not enforced");

    options = {};
    options.max_files = 1;
    expect_throw([&] {
        dhsh::ScpParser::load(resources, "ui/main/root.scp", options);
    }, "include file-count budget was not enforced");
    options = {};
    options.max_total_decoded_bytes = 48;
    expect_throw([&] {
        dhsh::ScpParser::load(resources, "ui/main/root.scp", options);
    }, "include aggregate decoded-byte budget was not enforced");

    resources.add("lua.scp", "#include \"missing-layout.lua\"\n");
    const dhsh::ScpDocument lua = dhsh::ScpParser::load(resources, "lua.scp");
    check(lua.includes.size() == 1, "non-SCP include was not retained");
}

void test_window_budget_and_invalid_layout_subtree() {
    const std::string deep = "WINDOW TYPE=WIN\nNAME=\"root\"\n"
        "WINDOW TYPE=WIN\nNAME=\"middle\"\n"
        "WINDOW TYPE=BUTTON\nNAME=\"leaf\"\nEND\nEND\nEND\n";
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindowOptions options;
    options.max_depth = 2;
    expect_throw([&] {
        dhsh::ScpWindow window(dhsh::ScpParser::parse(bytes(deep), "deep.scp"),
            resources, bus, {}, options);
    }, "window depth budget was not enforced");
    options = {};
    options.max_widgets = 1;
    expect_throw([&] {
        dhsh::ScpWindow window(dhsh::ScpParser::parse(bytes(deep), "nodes.scp"),
            resources, bus, {}, options);
    }, "window node budget was not enforced");

    const std::string invalid = "WINDOW TYPE=WIN\nNAME=\"bad_root\"\nRECT=0,0,nope,20\n"
        "WINDOW TYPE=BUTTON\nNAME=\"bad_child\"\nRECT=1,1,20,20\n"
        "BMPPATH=\"ui/main/bad\"\nMESSAGE FSM_BTN_MSG 1junk CODE \"Bad\"\nEND\nEND\n";
    dhsh::ScpWindow window(dhsh::ScpParser::parse(bytes(invalid), "invalid.scp"),
        resources, bus, [](std::string_view, std::span<const std::byte>) {
            dhsh::Image image;
            image.width = image.height = 1;
            image.rgba = {std::byte{0xff}, std::byte{0}, std::byte{0}, std::byte{0xff}};
            return image;
        });
    check(window.find_widget("bad_root") != nullptr
            && window.find_widget("bad_child") != nullptr,
        "invalid layout subtree was not retained");
    check(!window.find_widget("bad_root")->supported()
            && !window.find_widget("bad_child")->supported(),
        "invalid RECT did not disable its complete subtree");
    check(window.input_regions().empty(), "invalid layout subtree accepted input");
    dhsh::SoftwareRenderer renderer(64, 64);
    check(!window.render(renderer), "invalid layout subtree was drawn");
    bool message_diagnostic = false;
    for (const std::string& diagnostic : window.diagnostics()) {
        message_diagnostic = message_diagnostic
            || (diagnostic.find("Unsupported") != std::string::npos
                && diagnostic.find("invalid.scp") != std::string::npos
                && diagnostic.find("bad_child") != std::string::npos
                && diagnostic.find("MESSAGE") != std::string::npos);
    }
    check(message_diagnostic, "malformed MESSAGE was silently ignored");
}

void test_text_is_clipped_and_pixel_bounded() {
    FakeResources resources;
    dhsh::MessageBus bus;
    const std::string huge = "WINDOW TYPE=STATIC\nNAME=\"huge\"\n"
        "RECT=-100000,0,200000,200000\nTEXT=\"huge\"\nEND\n";
    dhsh::ScpWindow clipped(dhsh::ScpParser::parse(bytes(huge), "huge.scp"),
        resources, bus);
    dhsh::SoftwareRenderer viewport(64, 64);
    clipped.render(viewport);

    const std::string text = "WINDOW TYPE=STATIC\nNAME=\"budget\"\n"
        "RECT=0,0,16,16\nTEXT=\"bounded\"\nEND\n";
    dhsh::ScpWindowOptions options;
    options.max_text_pixels = 64;
    dhsh::ScpWindow bounded(dhsh::ScpParser::parse(bytes(text), "text.scp"),
        resources, bus, {}, options);
    dhsh::SoftwareRenderer small_viewport(16, 16);
    check(!bounded.render(small_viewport), "text exceeded its clipped pixel budget");
    check(!bounded.diagnostics().empty()
            && bounded.diagnostics().back().find("Text budget") != std::string::npos
            && bounded.diagnostics().back().find("text.scp") != std::string::npos
            && bounded.diagnostics().back().find("budget") != std::string::npos,
        "text pixel budget diagnostic lacks file or control");
}

void test_reentrant_dispatch_is_bounded() {
    dhsh::MessageBus bus(2);
    int calls = 0;
    bus.subscribe("reentrant", "FSM_BTN_MSG", [&](const dhsh::UiMessage&) {
        ++calls;
        if (calls < 4) {
            bus.dispatch(dhsh::UiMessage{"reentrant", "FSM_BTN_MSG", 1,
                "Again", "message.scp"});
        }
    });
    bus.dispatch(dhsh::UiMessage{"reentrant", "FSM_BTN_MSG", 1, "Again", "message.scp"});
    check(calls == 2, "message reentry exceeded the queue budget");
    check(!bus.diagnostics().empty(), "message queue budget failure was not diagnosed");
}

void test_callback_can_destroy_window_during_dispatch() {
    const std::string source = "WINDOW TYPE=BUTTON\nNAME=\"destroy\"\nRECT=0,0,20,20\n"
        "MESSAGE FSM_BTN_MSG 1 CODE \"First\"\n"
        "MESSAGE FSM_BTN_MSG 1 CODE \"Second\"\nEND\n";
    FakeResources resources;
    dhsh::MessageBus bus;
    auto window = std::make_unique<dhsh::ScpWindow>(
        dhsh::ScpParser::parse(bytes(source), "destroy.scp"), resources, bus);
    int calls = 0;
    bus.subscribe("destroy", "FSM_BTN_MSG", [&](const dhsh::UiMessage&) {
        ++calls;
        window.reset();
    });
    window->handle_event(mouse_event(dhsh::WindowEvent::Type::left_button_down, {2, 2}));
    window->handle_event(mouse_event(dhsh::WindowEvent::Type::left_button_up, {2, 2}));
    check(calls == 2 && window == nullptr,
        "callback-driven window destruction did not finish snapshotted messages safely");
}

void test_map_input_uses_dynamic_regions_and_ui_press_state() {
    dhsh::MapInput input;
    const dhsh::MapInputView view{};
    using Type = dhsh::WindowEvent::Type;
    input.set_ui_regions({dhsh::LogicalRect{10, 10, 20, 20}});
    input.handle_event(mouse_event(Type::left_button_down, {15, 15}), view);
    check(!input.handle_event(mouse_event(Type::left_button_up, {15, 15}), view),
        "dynamically added UI region did not block map click");
    input.set_ui_regions({});
    input.handle_event(mouse_event(Type::left_button_down, {15, 15}), view);
    check(input.handle_event(mouse_event(Type::left_button_up, {15, 15}), view).has_value(),
        "dynamically removed UI region continued blocking map click");
    input.handle_event(mouse_event(Type::left_button_down, {15, 15}), view, true);
    check(!input.handle_event(mouse_event(Type::left_button_up, {15, 15}), view, true),
        "UI-consumed press state leaked into map navigation");
}

void test_app_callback_can_clear_window_slot() {
    const std::string source = "WINDOW TYPE=BUTTON\nNAME=\"slot\"\nRECT=0,0,20,20\n"
        "MESSAGE FSM_BTN_MSG 1 CODE \"Clear\"\nEND\n";
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindows windows;
    windows.push_back(std::make_unique<dhsh::ScpWindow>(
        dhsh::ScpParser::parse(bytes(source), "slot.scp"), resources, bus));
    bus.subscribe("slot", "FSM_BTN_MSG", [&](const dhsh::UiMessage&) {
        windows[0].reset();
    });
    handle_scp_event(windows,
        mouse_event(dhsh::WindowEvent::Type::left_button_down, {2, 2}));
    check(handle_scp_event(windows,
        mouse_event(dhsh::WindowEvent::Type::left_button_up, {2, 2})),
        "App window dispatch did not consume the original button message");
    check(windows[0] == nullptr, "test callback did not clear its window slot");
    check(scp_input_regions(windows).empty(),
        "App dynamic-region collection dereferenced a cleared window slot");
}

void test_widget_rejects_null_before_initialization() {
    expect_throw([] {
        dhsh::Widget widget(nullptr, "null.scp", {}, true);
    }, "Widget null node was dereferenced before validation");
}

void test_scp_stage_diagnostics_cover_parser_and_window() {
    MapResources resources;
    resources.add("ui/main/root.scp", "#include \"config/ui/main/child.scp\"\n"
        "WINDOW TYPE=WIN\nNAME=\"root\"\nEND\n");
    resources.add("ui/main/child.scp", "WINDOW TYPE=STATIC\nNAME=\"child\"\nEND\n");
    dhsh::ScpLoadDiagnostics load_profile;
    const dhsh::ScpDocument document = dhsh::ScpParser::load(
        resources, "ui/main/root.scp", {}, &load_profile);
    check(document.windows.size() == 2 && load_profile.files == 2
            && load_profile.includes == 1,
        "SCP parser profile lost include or file counts");
    check(load_profile.resource_read_ms >= 0 && load_profile.nz_decode_ms >= 0
            && load_profile.parse_ms >= 0 && load_profile.include_resolve_ms >= 0,
        "SCP parser profile produced an invalid stage duration");

    FakeResources images;
    dhsh::MessageBus bus;
    dhsh::ScpWindowProfile window_profile;
    dhsh::ScpWindow window(sample_document(), images, bus,
        [](std::string_view, std::span<const std::byte>) {
            dhsh::Image image;
            image.width = image.height = 1;
            image.rgba = {std::byte{0xff}, std::byte{0}, std::byte{0}, std::byte{0xff}};
            return image;
        }, {}, &window_profile);
    check(window.widgets().size() == 5 && window_profile.images == 5,
        "SCP window profile lost image, button-state or widget counts");
    check(window_profile.layout_ms >= 0 && window_profile.widget_setup_ms >= 0
            && window_profile.resource_read_ms >= 0 && window_profile.nz_decode_ms >= 0
            && window_profile.wic_decode_ms == 0
            && window_profile.proprietary_decode_ms >= 0 && window_profile.lua_ms == 0,
        "SCP window profile produced invalid or misattributed stage durations");
}

void test_children_inherit_visibility_across_widget_vector_growth() {
    std::string source = "WINDOW TYPE=WIN\nNAME=\"root\"\nRECT=0,0,800,600\n";
    for (int index = 0; index < 96; ++index) {
        source += "WINDOW TYPE=STATIC\nNAME=\"child" + std::to_string(index)
            + "\"\nRECT=0,0,10,10\nTEXT=\"child\"\nEND\n";
    }
    source += "END\n";
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window(dhsh::ScpParser::parse(bytes(source)), resources, bus);
    check(window.widgets().size() == 97, "visibility probe did not build every child");
    for (std::size_t index = 1; index < window.widgets().size(); ++index) {
        check(window.widgets()[index].visible(),
            "widget vector growth corrupted a visible parent's child visibility");
    }
}

void test_chatmain_declarations_build_supported_widgets() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_chatmain_window(false, resources, bus);
    const dhsh::Widget* root = window.find_widget("chatmain");
    check(root != nullptr && root->bounds() == dhsh::LogicalRect{0, 0, 640, 480},
        "chatmain root RECT was not used verbatim");

    const dhsh::Widget* bar = window.find_widget("chatmain_barwnd");
    check(bar != nullptr && !bar->visible(), "HIDE=1 chatmain_barwnd stayed visible");
    const dhsh::Widget* list = window.find_widget("chatmain_txtlst_msg");
    check(list != nullptr && list->type() == dhsh::WidgetType::list_view_ex
            && list->supported(),
        "chatmain_txtlst_msg was not built as a supported LISTVIEWEX");
    check(list->bounds() == dhsh::LogicalRect{3, 30, 640, 480},
        "LISTVIEWEX RECT was not applied as native logical coordinates");
    check(list->client_rect().has_value()
            && *list->client_rect() == dhsh::LogicalRect{0, 0, 640, 480},
        "LISTVIEWEX CLIENTRECT was not parsed distinctly from RECT");
    const dhsh::Widget* input = window.find_widget("chatmain_input_mes");
    check(input != nullptr && input->type() == dhsh::WidgetType::rich_edit
            && input->supported() && !input->visible(),
        "chatmain_input_mes was not a supported RICHEDIT hidden with its parent");
    check(input->bounds() == dhsh::LogicalRect{1656, 1204, 300, 16},
        "RICHEDIT RECT was not offset by its hidden parent window");
    const dhsh::Widget* combo = window.find_widget("chatmain_txtlstchanel");
    check(combo != nullptr && combo->type() == dhsh::WidgetType::combo_box
            && combo->supported() && !combo->visible(),
        "chatmain_txtlstchanel was not a supported COMBOBOX hidden with its parent");
    check(combo->bounds() == dhsh::LogicalRect{1552, 1200, 32, 30},
        "COMBOBOX RECT with negative x was not applied verbatim");

    auto* view = static_cast<dhsh::ListViewEx*>(
        window.behavior_for("chatmain_txtlst_msg"));
    check(view != nullptr, "LISTVIEWEX behavior was not registered");
    check(view->draw_text_type() == 2, "DRAWTXTTYPE=2 was not parsed");
    check(view->trans_msg(), "TRANSMSG=1 was not parsed");
    check(view->scroll_range().min == 10 && view->scroll_range().max == 100
            && view->scroll_range().page == 10,
        "LISTVIEWEX SCROLL VALUERGN/PAGESIZE were not parsed");
    check(view->scroll_value() == 15, "LISTVIEWEX SCROLL VALUE was not the initial position");

    std::size_t unknown = 0;
    for (const std::string& diagnostic : window.diagnostics()) {
        if (diagnostic.find("Unsupported") == std::string::npos) {
            continue;
        }
        check(diagnostic.find("LISTVIEWEX") == std::string::npos
                && diagnostic.find("RICHEDIT") == std::string::npos
                && diagnostic.find("COMBOBOX") == std::string::npos
                && diagnostic.find("SCROLL") == std::string::npos,
            "target chatmain control was still reported unsupported: " + diagnostic);
        check(diagnostic.find("TWOSTBTN") != std::string::npos
                || diagnostic.find("TXTLIST") != std::string::npos,
            "unknown control lost its Unsupported diagnostic: " + diagnostic);
        ++unknown;
    }
    check(unknown == 4, "expected the three TWOSTBTN and one TXTLIST diagnostics");
}

void test_chatmain_message_names_cover_input_list_and_paging() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_chatmain_window(false, resources, bus);
    const std::vector<dhsh::UiMessage> declarations = window.message_declarations();
    check(has_message(declarations, "chatmain_input_mes", "FSM_KEY_DOWN", -1, "OnChatInput"),
        "chatmain_input_mes lost FSM_KEY_DOWN -1 OnChatInput");
    check(has_message(declarations, "chatmain_txtlstchanel", "FSM_COMBOBOX_SELECTED", -1,
            "OnChannelSel"),
        "chatmain_txtlstchanel lost FSM_COMBOBOX_SELECTED -1 OnChannelSel");
    check(has_message(declarations, "chatmain_txtlst_msg", "FSM_MOUSE_DOWN", 2,
            "OnTxtListButtonDown"),
        "chatmain_txtlst_msg lost FSM_MOUSE_DOWN 2 OnTxtListButtonDown");
    check(has_message(declarations, "chat_btntop", "FSM_BTN_MSG", 1, "OnSetViewUpAll")
            && has_message(declarations, "chat_btnup", "FSM_BTN_MSG", 1, "OnSetViewUpOne")
            && has_message(declarations, "chat_btndown", "FSM_BTN_MSG", 1, "OnSetViewDownOne")
            && has_message(declarations, "chat_btnbottom", "FSM_BTN_MSG", 1, "OnSetViewDownAll"),
        "chat paging buttons lost their OnSetView* declarations");
    check(has_message(declarations, "chatmain_screen", "FSM_MOUSE_UP", 1, "OnShieldMsg")
            && has_message(declarations, "chatmain_screen", "FSM_MOUSE_UP", 2, "OnShowShieldLst"),
        "chatmain_screen lost its left/right FSM_MOUSE_UP declarations");
}

void test_chatmain_paging_buttons_and_right_click_dispatch_by_button() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_chatmain_window(false, resources, bus);
    int shield_left = 0;
    int shield_right = 0;
    int page_top = 0;
    bus.subscribe("chatmain_screen", "FSM_MOUSE_UP",
        [&](const dhsh::UiMessage& message) {
            if (message.code == "OnShieldMsg") {
                ++shield_left;
            } else if (message.code == "OnShowShieldLst") {
                ++shield_right;
            }
        });
    bus.subscribe("chat_btntop", "FSM_BTN_MSG",
        [&](const dhsh::UiMessage&) { ++page_top; });
    using Type = dhsh::WindowEvent::Type;
    window.handle_event(mouse_event(Type::left_button_down, {100, 490}));
    check(window.handle_event(mouse_event(Type::left_button_up, {100, 490})),
        "left mouse up over chatmain_screen was not consumed");
    check(shield_left == 1 && shield_right == 0,
        "FSM_MOUSE_UP 1 did not fire only for the left button");
    window.handle_event(mouse_event(Type::right_button_down, {100, 490}));
    check(window.handle_event(mouse_event(Type::right_button_up, {100, 490})),
        "right mouse up over chatmain_screen was not consumed");
    check(shield_left == 1 && shield_right == 1,
        "FSM_MOUSE_UP 2 did not fire only for the right button");
    check(window.handle_event(mouse_event(Type::left_button_down, {120, 488})),
        "paging button down was not consumed");
    check(window.handle_event(mouse_event(Type::left_button_up, {120, 488})),
        "paging button up was not consumed");
    check(page_top == 1, "chat_btntop did not dispatch OnSetViewUpAll exactly once");
}

void test_richedit_focus_editing_keys_and_input_isolation() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_chatmain_window(true, resources, bus);
    int keys = 0;
    bus.subscribe("chatmain_input_mes", "FSM_KEY_DOWN",
        [&](const dhsh::UiMessage& message) {
            ++keys;
            check(message.code == "OnChatInput", "unexpected key message "
                + message.code);
        });
    auto* edit = static_cast<dhsh::RichEdit*>(
        window.behavior_for("chatmain_input_mes"));
    check(edit != nullptr, "RICHEDIT behavior was not registered");
    using Type = dhsh::WindowEvent::Type;
    check(window.handle_event(mouse_event(Type::left_button_down, {1700, 1210})),
        "click inside chatmain_input_mes was not consumed");
    check(window.handle_event(mouse_event(Type::left_button_up, {1700, 1210})),
        "click release inside chatmain_input_mes was not consumed");
    check(window.focused_widget() == window.find_widget("chatmain_input_mes"),
        "clicking chatmain_input_mes did not focus it");
    check(edit->focused(), "RICHEDIT behavior did not report focus");

    check(window.handle_event(text_event("hello")),
        "text input while focused was not consumed");
    check(edit->text() == "hello" && edit->cursor() == 5,
        "text input did not insert at the cursor");
    check(window.handle_event(key_event(0x41)),
        "key down while focused was not consumed");
    check(edit->text() == "hello", "WM_KEYDOWN was treated as text input");
    check(keys == 1, "focused key down did not dispatch FSM_KEY_DOWN OnChatInput");

    window.handle_event(key_event(VK_LEFT));
    check(edit->cursor() == 4, "VK_LEFT did not move the cursor left");
    window.handle_event(text_event("X"));
    check(edit->text() == "hellXo", "text did not insert at the moved cursor");
    window.handle_event(key_event(VK_HOME));
    window.handle_event(text_event("YZ"));
    check(edit->text() == "YZhellXo", "VK_HOME did not move to the start");
    window.handle_event(key_event(VK_END));
    window.handle_event(key_event(VK_BACK));
    check(edit->text() == "YZhellX", "VK_BACK did not delete before the cursor");
    window.handle_event(key_event(VK_DELETE));
    check(edit->text() == "YZhellX", "VK_DELETE deleted past the end");

    check(window.handle_event(mouse_event(Type::left_button_down, {100, 100})),
        "click on the message list was not consumed by the window");
    check(window.focused_widget() == nullptr && !edit->focused(),
        "clicking away did not unfocus chatmain_input_mes");
    check(!window.handle_event(text_event("no")),
        "text input without focus was consumed");
    check(edit->text() == "YZhellX", "unfocused editor changed its text");
    check(!window.handle_event(key_event(0x42)),
        "key down without focus was consumed");
    check(keys == 6, "unfocused key down dispatched FSM_KEY_DOWN messages");
}

void test_richedit_ime_composition_commit_and_cancel() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_chatmain_window(true, resources, bus);
    auto* edit = static_cast<dhsh::RichEdit*>(
        window.behavior_for("chatmain_input_mes"));
    check(edit != nullptr, "RICHEDIT behavior was not registered");
    using Type = dhsh::WindowEvent::Type;
    window.handle_event(mouse_event(Type::left_button_down, {1700, 1210}));
    window.handle_event(mouse_event(Type::left_button_up, {1700, 1210}));
    window.handle_event(text_event("ab"));

    check(window.handle_event(ime_event(Type::ime_begin)),
        "IME composition start was not consumed");
    check(edit->composing(), "RICHEDIT did not enter composition state");
    check(window.handle_event(ime_event(Type::ime_composition, "\xE4\xB8\xAD")),
        "IME composition update was not consumed");
    check(edit->text() == "ab" && edit->preview_text() == "ab\xE4\xB8\xAD",
        "IME preview leaked into the committed text");
    check(window.handle_event(ime_event(Type::ime_commit, "\xE4\xB8\xAD")),
        "IME composition commit was not consumed");
    check(!edit->composing() && edit->text() == "ab\xE4\xB8\xAD",
        "IME commit did not insert the composed text once");
    window.handle_event(key_event(VK_LEFT));
    check(edit->cursor() == 2,
        "cursor did not step back one codepoint across the committed CJK text");

    check(window.handle_event(ime_event(Type::ime_begin)),
        "second IME composition start was not consumed");
    window.handle_event(ime_event(Type::ime_composition, "x"));
    check(window.handle_event(ime_event(Type::ime_cancel)),
        "IME composition cancel was not consumed");
    check(!edit->composing() && edit->text() == "ab\xE4\xB8\xAD",
        "IME cancel changed the committed text");
}

void test_richedit_max_length_and_enter_does_not_fake_send() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_chatmain_window(true, resources, bus);
    dhsh::ChatModel model;
    window.set_chat_model(&model);
    int keys = 0;
    bus.subscribe("chatmain_input_mes", "FSM_KEY_DOWN",
        [&](const dhsh::UiMessage&) { ++keys; });
    auto* edit = static_cast<dhsh::RichEdit*>(
        window.behavior_for("chatmain_input_mes"));
    check(edit != nullptr, "RICHEDIT behavior was not registered");
    using Type = dhsh::WindowEvent::Type;
    window.handle_event(mouse_event(Type::left_button_down, {1700, 1210}));
    window.handle_event(mouse_event(Type::left_button_up, {1700, 1210}));

    edit->set_max_length(6);
    window.handle_event(text_event("abcd"));
    check(edit->text() == "abcd", "baseline insert failed");
    check(window.handle_event(text_event("\xE4\xB8\xAD\xE6\x96\x87x")),
        "overlong text input was not consumed");
    check(edit->text() == "abcd\xE4\xB8\xAD\xE6\x96\x87",
        "maximum length counted bytes instead of codepoints");
    window.handle_event(text_event("abcdefgh"));
    check(edit->text() == "abcd\xE4\xB8\xAD\xE6\x96\x87",
        "RICHEDIT ignored its maximum length");

    window.handle_event(key_event(VK_RETURN));
    check(keys == 1, "VK_RETURN did not dispatch the declared FSM_KEY_DOWN message");
    check(model.visible_messages().empty() && edit->text() == "abcd\xE4\xB8\xAD\xE6\x96\x87",
        "editing or Enter fabricated a sent message or cleared the editor");
    auto* view = static_cast<dhsh::ListViewEx*>(
        window.behavior_for("chatmain_txtlst_msg"));
    check(view != nullptr && view->rows().empty(),
        "bound chat model gained a fabricated local message");
}

void test_combobox_items_selection_dropdown_and_enable() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_chatmain_window(true, resources, bus);
    int selections = 0;
    std::string selected_code;
    bus.subscribe("chatmain_txtlstchanel", "FSM_COMBOBOX_SELECTED",
        [&](const dhsh::UiMessage& message) {
            ++selections;
            selected_code = message.code;
        });
    auto* combo = static_cast<dhsh::ComboBox*>(
        window.behavior_for("chatmain_txtlstchanel"));
    check(combo != nullptr, "COMBOBOX behavior was not registered");
    const std::vector<std::string> expected = {
        "\xE5\xBD\x93\xE5\x89\x8D", "\xE9\x98\x9F\xE4\xBC\x8D", "\xE5\xB8\xAE\xE6\xB4\xBE",
        "\xE7\xA7\x81\xE8\x81\x8A", "\xE4\xB8\x96\xE7\x95\x8C", "\xE7\xAB\x9E\xE6\x8A\x80",
        "G M", "\xE5\x90\x8C\xE5\x9F\x8E", "\xE7\xAB\x9E\xE6\x8A\x80",
        "\xE5\x9C\xB0\xE5\x9B\xBE"};
    check(combo->items() == expected,
        "COMBOBOX items were not the ADDTEXT channel labels in order");
    check(!combo->dropdown_visible(), "COMBOBOX dropdown started visible");

    using Type = dhsh::WindowEvent::Type;
    check(window.handle_event(mouse_event(Type::left_button_down, {1560, 1210})),
        "click inside chatmain_txtlstchanel was not consumed");
    check(window.handle_event(mouse_event(Type::left_button_up, {1560, 1210})),
        "click release inside chatmain_txtlstchanel was not consumed");
    check(combo->dropdown_visible(), "clicking the COMBOBOX did not open the dropdown");
    const dhsh::LogicalRect dropdown = combo->item_area();
    const int row_height =
        dropdown.height / static_cast<int>(combo->items().size());
    const dhsh::LogicalPoint third_row_center{
        dropdown.x + dropdown.width / 2,
        dropdown.y + 3 * row_height + row_height / 2};
    check(window.handle_event(
            mouse_event(Type::left_button_down, third_row_center)),
        "click on a dropdown row was not consumed");
    check(window.handle_event(mouse_event(Type::left_button_up, third_row_center)),
        "click release on a dropdown row was not consumed");
    check(combo->selected_index() == std::optional<std::size_t>{3}
            && !combo->dropdown_visible(),
        "clicking a dropdown row did not select it and close the dropdown");
    check(selections == 1 && selected_code == "OnChannelSel",
        "selection did not dispatch FSM_COMBOBOX_SELECTED OnChannelSel");

    window.handle_event(mouse_event(Type::left_button_down, {1560, 1210}));
    window.handle_event(mouse_event(Type::left_button_up, {1560, 1210}));
    check(combo->dropdown_visible(), "dropdown did not reopen");
    window.handle_event(mouse_event(Type::left_button_down, {5, 5}));
    window.handle_event(mouse_event(Type::left_button_up, {5, 5}));
    check(!combo->dropdown_visible() && selections == 1,
        "clicking outside did not close the dropdown without selecting");

    window.find_widget("chatmain_txtlstchanel")->set_enabled(false);
    window.handle_event(mouse_event(Type::left_button_down, {1560, 1210}));
    window.handle_event(mouse_event(Type::left_button_up, {1560, 1210}));
    check(!combo->dropdown_visible() && selections == 1,
        "disabled COMBOBOX still opened its dropdown");
}

void test_disabled_richedit_declared_with_enable_zero_ignores_input() {
    const std::string source = "WINDOW TYPE=WIN\nNAME=\"root\"\nRECT=0,0,400,300\n"
        "WINDOW TYPE=COMBOBOX\nNAME=\"chatmain_txtlstchanel\"\nRECT=10,10,60,20\n"
        "ENABLE=0\n"
        "FIXEDPART TYPE = TXTLIST\nNAME=\"chatmain_txtlst_channel\"\n"
        "RECT=0,20,60,40\nCLIENTRECT=0,0,60,40\n"
        "ADDTEXT=\"near\"\nADDTEXT=\"world\"\nEND\n"
        "MESSAGE FSM_COMBOBOX_SELECTED -1 CODE \"OnChannelSel\"\nEND\n"
        "WINDOW TYPE=RICHEDIT\nNAME=\"chatmain_input_mes\"\nRECT=10,60,200,16\n"
        "ENABLE=0\nTEXTCOLOR=0,0,0\n"
        "MESSAGE FSM_KEY_DOWN -1 CODE \"OnChatInput\"\nEND\nEND\n";
    FakeResources resources;
    dhsh::MessageBus bus;
    int keys = 0;
    int selections = 0;
    bus.subscribe("chatmain_input_mes", "FSM_KEY_DOWN",
        [&](const dhsh::UiMessage&) { ++keys; });
    bus.subscribe("chatmain_txtlstchanel", "FSM_COMBOBOX_SELECTED",
        [&](const dhsh::UiMessage&) { ++selections; });
    dhsh::ScpWindow window(dhsh::ScpParser::parse(bytes(source), "disabled.scp"),
        resources, bus, [](std::string_view, std::span<const std::byte>) {
            dhsh::Image image;
            image.width = image.height = 1;
            image.rgba = {std::byte{0xff}, std::byte{0}, std::byte{0}, std::byte{0xff}};
            return image;
        });
    auto* edit = static_cast<dhsh::RichEdit*>(
        window.behavior_for("chatmain_input_mes"));
    auto* combo = static_cast<dhsh::ComboBox*>(
        window.behavior_for("chatmain_txtlstchanel"));
    check(edit != nullptr && combo != nullptr,
        "disabled controls did not register behaviors");

    using Type = dhsh::WindowEvent::Type;
    window.handle_event(mouse_event(Type::left_button_down, {20, 65}));
    window.handle_event(mouse_event(Type::left_button_up, {20, 65}));
    check(window.focused_widget() == nullptr && !edit->focused(),
        "declared-disabled RICHEDIT accepted focus");
    check(!window.handle_event(text_event("x")),
        "declared-disabled RICHEDIT consumed text input");
    check(edit->text().empty(), "declared-disabled RICHEDIT changed its text");
    check(!window.handle_event(key_event(0x41)),
        "declared-disabled RICHEDIT consumed a key down");
    check(!window.handle_event(ime_event(Type::ime_begin)),
        "declared-disabled RICHEDIT consumed IME composition start");
    window.handle_event(ime_event(Type::ime_commit, "\xE4\xB8\xAD"));
    check(!edit->composing() && edit->text().empty(),
        "declared-disabled RICHEDIT accepted an IME commit");
    check(keys == 0, "declared-disabled RICHEDIT dispatched FSM_KEY_DOWN");

    window.handle_event(mouse_event(Type::left_button_down, {20, 15}));
    window.handle_event(mouse_event(Type::left_button_up, {20, 15}));
    check(!combo->dropdown_visible(),
        "declared-disabled COMBOBOX opened its dropdown");
    check(!combo->select(1) && !combo->selected_index().has_value(),
        "declared-disabled COMBOBOX accepted a selection");
    check(selections == 0, "declared-disabled COMBOBOX dispatched selection");
}

void test_widget_set_enabled_toggles_richedit_and_combo_live() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_chatmain_window(true, resources, bus);
    int keys = 0;
    int selections = 0;
    bus.subscribe("chatmain_input_mes", "FSM_KEY_DOWN",
        [&](const dhsh::UiMessage&) { ++keys; });
    bus.subscribe("chatmain_txtlstchanel", "FSM_COMBOBOX_SELECTED",
        [&](const dhsh::UiMessage&) { ++selections; });
    auto* edit = static_cast<dhsh::RichEdit*>(
        window.behavior_for("chatmain_input_mes"));
    auto* combo = static_cast<dhsh::ComboBox*>(
        window.behavior_for("chatmain_txtlstchanel"));
    check(edit != nullptr && combo != nullptr, "behaviors were not registered");
    using Type = dhsh::WindowEvent::Type;

    // Focus the editor, then disable it at runtime: input must stop flowing.
    window.handle_event(mouse_event(Type::left_button_down, {1700, 1210}));
    window.handle_event(mouse_event(Type::left_button_up, {1700, 1210}));
    check(window.focused_widget() == window.find_widget("chatmain_input_mes"),
        "enabled RICHEDIT did not focus before the toggle");
    window.find_widget("chatmain_input_mes")->set_enabled(false);
    check(!window.handle_event(text_event("abc")),
        "dynamically disabled RICHEDIT consumed text input");
    check(edit->text().empty(), "dynamically disabled RICHEDIT changed its text");
    check(!window.handle_event(key_event(0x41)),
        "dynamically disabled RICHEDIT consumed a key down");
    check(!window.handle_event(ime_event(Type::ime_begin)),
        "dynamically disabled RICHEDIT consumed IME composition start");
    window.handle_event(ime_event(Type::ime_commit, "\xE4\xB8\xAD"));
    check(!edit->composing() && edit->text().empty(),
        "dynamically disabled RICHEDIT accepted an IME commit");
    check(keys == 0, "dynamically disabled RICHEDIT dispatched FSM_KEY_DOWN");
    window.find_widget("chatmain_input_mes")->set_enabled(true);
    check(window.handle_event(text_event("back")),
        "re-enabled RICHEDIT did not consume text again");
    check(edit->text() == "back", "re-enabled RICHEDIT did not accept text");

    // Open the dropdown, then disable the combo at runtime: the open popup
    // must close without selecting, and selection must stay blocked.
    window.handle_event(mouse_event(Type::left_button_down, {1560, 1210}));
    window.handle_event(mouse_event(Type::left_button_up, {1560, 1210}));
    check(combo->dropdown_visible(), "COMBOBOX dropdown did not open");
    window.find_widget("chatmain_txtlstchanel")->set_enabled(false);
    check(!combo->select(2),
        "dynamically disabled COMBOBOX accepted a selection");
    window.handle_event(mouse_event(Type::left_button_down, {1636, 1150}));
    window.handle_event(mouse_event(Type::left_button_up, {1636, 1150}));
    check(!combo->dropdown_visible(),
        "dynamically disabled COMBOBOX kept its dropdown open");
    check(!combo->selected_index().has_value() && selections == 0,
        "dynamically disabled COMBOBOX selected a row");
    window.handle_event(mouse_event(Type::left_button_down, {1560, 1210}));
    window.handle_event(mouse_event(Type::left_button_up, {1560, 1210}));
    check(!combo->dropdown_visible(),
        "dynamically disabled COMBOBOX opened its dropdown");
    window.find_widget("chatmain_txtlstchanel")->set_enabled(true);
    window.handle_event(mouse_event(Type::left_button_down, {1560, 1210}));
    window.handle_event(mouse_event(Type::left_button_up, {1560, 1210}));
    check(combo->dropdown_visible(),
        "re-enabled COMBOBOX did not open its dropdown again");
}

void test_listview_rows_wrap_channel_colors_and_item_links() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_chatmain_window(true, resources, bus);
    dhsh::ChatModel model;
    window.set_chat_model(&model);
    auto* view = static_cast<dhsh::ListViewEx*>(
        window.behavior_for("chatmain_txtlst_msg"));
    check(view != nullptr, "LISTVIEWEX behavior was not registered");

    dhsh::ChatMessage short_message;
    short_message.channel = 0;
    short_message.spans = {
        {dhsh::ChatSpanKind::text, "hello ", "red", {}},
        {dhsh::ChatSpanKind::item_link, "[\xE9\x92\xA9\xE9\x95\xB0\xE6\x9E\xAA]",
            "red", "I1"},
    };
    dhsh::ChatMessage wrapped_message;
    wrapped_message.channel = 5;
    wrapped_message.spans = {
        {dhsh::ChatSpanKind::text, std::string(600, 'a'), "yellow", {}},
    };
    dhsh::ChatMessage broken_message;
    broken_message.channel = 9;
    broken_message.spans = {
        {dhsh::ChatSpanKind::text, "a", "red", {}},
        {dhsh::ChatSpanKind::line_break, "\n", "red", {}},
        {dhsh::ChatSpanKind::text, "b", "blue", {}},
    };
    model.append(short_message);
    model.append(broken_message);
    const std::size_t base_rows = view->rows().size();
    check(base_rows == 3, "short, item-link and broken messages did not yield 3 rows");
    model.append(wrapped_message);
    check(view->rows().size() >= base_rows + 6,
        "a 600-character message did not wrap onto multiple rows");
    // Fewer rows than one page clamp the first row to the last visible
    // page (the top); the bare VALUERGN offset would point past the end.
    check(view->first_visible_row() == 0,
        "a sub-page history must clamp the first row to the visible page");

    bool saw_red = false;
    bool saw_blue = false;
    bool saw_link = false;
    for (const auto& row : view->rows()) {
        for (const auto& segment : row.segments) {
            saw_red = saw_red || (segment.kind == dhsh::ChatSpanKind::text
                && segment.color == RGB(255, 0, 0));
            saw_blue = saw_blue || (segment.kind == dhsh::ChatSpanKind::text
                && segment.color == RGB(0, 0, 255));
            saw_link = saw_link || (segment.kind == dhsh::ChatSpanKind::item_link
                && segment.link == "I1"
                && segment.text == "[\xE9\x92\xA9\xE9\x95\xB0\xE6\x9E\xAA]");
        }
    }
    check(saw_red && saw_blue, "channel colors did not map to span colors");
    check(saw_link, "item-link span lost its link id or visible text");
    for (const auto& row : view->rows()) {
        for (const auto& segment : row.segments) {
            check(segment.x + segment.width <= 640,
                "a segment exceeded the LISTVIEWEX client width");
        }
    }

    dhsh::SoftwareRenderer renderer(700, 600);
    check(window.render(renderer), "chat rows did not render");
    bool any_text_pixel = false;
    for (std::size_t y = 30; y < 400 && !any_text_pixel; ++y) {
        for (std::size_t x = 3; x < 643; ++x) {
            const std::size_t offset = (y * 700 + x) * 4;
            if (renderer.target().rgba[offset + 3] != std::byte{0}) {
                any_text_pixel = true;
                break;
            }
        }
    }
    check(any_text_pixel, "no chat text pixel was drawn inside the LISTVIEWEX");
    check(renderer.target().rgba[3] == std::byte{0},
        "LISTVIEWEX drew outside the renderer bounds");
}

void test_listview_scroll_wheel_drag_range_and_right_click() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_chatmain_window(true, resources, bus);
    int list_clicks = 0;
    bus.subscribe("chatmain_txtlst_msg", "FSM_MOUSE_DOWN",
        [&](const dhsh::UiMessage& message) { ++list_clicks; });
    auto* view = static_cast<dhsh::ListViewEx*>(
        window.behavior_for("chatmain_txtlst_msg"));
    check(view != nullptr, "LISTVIEWEX behavior was not registered");
    check(view->scroll_value() == 15, "SCROLL VALUE did not initialize the position");
    using Type = dhsh::WindowEvent::Type;
    check(window.handle_event(wheel_event({100, 100}, 120)),
        "mouse wheel over the message list was not consumed");
    check(view->scroll_value() == 14, "wheel up did not scroll toward older content");
    window.handle_event(wheel_event({100, 100}, -120));
    check(view->scroll_value() == 15, "wheel down did not scroll toward newer content");
    for (int index = 0; index < 20; ++index) {
        window.handle_event(wheel_event({100, 100}, 120));
    }
    check(view->scroll_value() == 10, "scroll did not clamp at VALUERGN minimum");

    // Scroll thumb: SCROLL RECT 110,120,20,20 inside LISTVIEWEX at 3,30 puts the
    // track at (113,150,20,20) and the TWOSTBTN thumb at (113,150,20,40).
    window.handle_event(mouse_event(Type::left_button_down, {120, 155}));
    check(view->scroll_value() == 10, "grabbing the thumb jumped the scroll");
    window.handle_event(mouse_event(Type::mouse_move, {120, 170}));
    check(view->scroll_value() == 100,
        "dragging the scroll thumb did not follow the pointer");
    window.handle_event(mouse_event(Type::left_button_up, {120, 170}));
    window.handle_event(mouse_event(Type::mouse_move, {120, 100}));
    check(view->scroll_value() == 100,
        "moving after the drag release kept changing the scroll");
    for (int index = 0; index < 200; ++index) {
        window.handle_event(wheel_event({100, 100}, -120));
    }
    check(view->scroll_value() == 100, "scroll did not clamp at VALUERGN maximum");

    check(window.handle_event(mouse_event(Type::right_button_down, {50, 50})),
        "right mouse down on the message list was not consumed");
    check(list_clicks == 1, "FSM_MOUSE_DOWN 2 did not dispatch OnTxtListButtonDown");
}

void test_listview_shares_chat_model_read_only() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_chatmain_window(true, resources, bus);
    dhsh::ChatModel model;
    window.set_chat_model(&model);
    auto* view = static_cast<dhsh::ListViewEx*>(
        window.behavior_for("chatmain_txtlst_msg"));
    check(view != nullptr && view->model() == &model,
        "LISTVIEWEX did not bind the shared chat model");
    for (int index = 0; index < 3; ++index) {
        dhsh::ChatMessage message;
        message.channel = static_cast<std::uint32_t>(index);
        message.spans = {{dhsh::ChatSpanKind::text, "m" + std::to_string(index),
            "red", {}}};
        model.append(message);
    }
    check(view->rows().size() == 3,
        "LISTVIEWEX did not see messages already in the shared model");
    dhsh::ChatMessage fourth;
    fourth.channel = 0;
    fourth.spans = {{dhsh::ChatSpanKind::text, "m3", "red", {}}};
    model.append(fourth);
    check(view->rows().size() == 4,
        "LISTVIEWEX did not observe later appends without rebinding");
    model.set_channel_filter(0);
    check(view->rows().size() == 2,
        "LISTVIEWEX did not respect the shared channel filter");
    for (int index = 0; index < 10; ++index) {
        window.handle_event(wheel_event({100, 100}, 120));
    }
    check(model.scroll_offset_lines() == 0 && model.visible_messages().size() == 2,
        "LISTVIEWEX scrolling mutated the shared chat model");
    check(model.visible_messages().front().spans.front().text == "m0",
        "LISTVIEWEX mutated the shared message text");
}

void test_win32_window_emits_utf8_text_and_ime_boundaries() {
    using Type = dhsh::WindowEvent::Type;
    std::vector<dhsh::WindowEvent> events;
    {
        dhsh::Win32Window probe(L"task7_text_probe", 320, 240,
            [&](const dhsh::WindowEvent& event) { events.push_back(event); });
        SendMessageW(probe.handle(), WM_CHAR, L'A', 0);
        SendMessageW(probe.handle(), WM_CHAR, 0xD83D, 0);
        SendMessageW(probe.handle(), WM_CHAR, 0xDE00, 0);
        SendMessageW(probe.handle(), WM_KEYDOWN, 0x41, 0);
        SendMessageW(probe.handle(), WM_KEYUP, 0x41, 0);
        SendMessageW(probe.handle(), WM_IME_STARTCOMPOSITION, 0, 0);
        SendMessageW(probe.handle(), WM_IME_COMPOSITION, 0, GCS_COMPSTR);
        SendMessageW(probe.handle(), WM_IME_ENDCOMPOSITION, 0, 0);
        SendMessageW(probe.handle(), WM_IME_STARTCOMPOSITION, 0, 0);
        SendMessageW(probe.handle(), WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
        SendMessageW(probe.handle(), WM_IME_ENDCOMPOSITION, 0, 0);
    }
    bool key_down_a = false;
    bool text_a = false;
    bool text_emoji = false;
    bool ime_begin = false;
    bool ime_begin_second = false;
    bool ime_cancel = false;
    bool stray_text_from_key = false;
    for (std::size_t index = 0; index < events.size(); ++index) {
        const dhsh::WindowEvent& event = events[index];
        if (event.type == Type::key_down && event.key == 0x41) {
            key_down_a = true;
            if (index + 1 < events.size()
                && events[index + 1].type == Type::text_input) {
                stray_text_from_key = true;
            }
        }
        text_a = text_a || (event.type == Type::text_input && event.text == "A");
        text_emoji = text_emoji || (event.type == Type::text_input
            && event.text == "\xF0\x9F\x98\x80");
        if (event.type == Type::ime_begin) {
            if (ime_begin) {
                ime_begin_second = true;
            }
            ime_begin = true;
        }
        ime_cancel = ime_cancel || event.type == Type::ime_cancel;
    }
    check(key_down_a, "WM_KEYDOWN did not emit a key down event");
    check(!stray_text_from_key, "WM_KEYDOWN also emitted a text input event");
    check(text_a, "WM_CHAR did not emit UTF-8 text input");
    check(text_emoji, "surrogate pair WM_CHARs did not combine into one UTF-8 text");
    check(ime_begin && ime_begin_second, "WM_IME_STARTCOMPOSITION did not emit ime_begin");
    check(ime_cancel, "an uncommitted IME composition did not emit ime_cancel");
}

// Records send attempts so tests can prove text leaves through the single
// sink and is never echoed into the shared model as a fake success.
struct RecordingSendSink final : dhsh::ChatSendSink {
    void send_chat(std::uint32_t channel, std::string_view text) override {
        sends.emplace_back(channel, std::string(text));
    }
    std::vector<std::pair<std::uint32_t, std::string>> sends;
};

// TranslateMessage synthesizes WM_CHAR control codes for keys (CR for Enter,
// BS for Backspace, TAB, ESC). In the single-line chat edit their behavior
// lives in the WM_KEYDOWN path -- Enter sends and clears the edit -- so a
// control WM_CHAR must not become a text input: it would re-insert "\r" into
// the just-cleared buffer and the next send would carry a leading CR. The
// detached chat window already filters these in
// ScpChatWindow::translate_message; the main window must match (the field
// evidence .rea/replica_online_20261125_attempt24.console.log:114-146 shows
// key auto-repeat on the main input, where a repeat Enter would re-send).
void test_win32_window_filters_control_character_text() {
    using Type = dhsh::WindowEvent::Type;
    std::vector<dhsh::WindowEvent> events;
    {
        dhsh::Win32Window probe(L"main_chat_bar_ctrl_probe", 320, 240,
            [&](const dhsh::WindowEvent& event) { events.push_back(event); });
        SendMessageW(probe.handle(), WM_CHAR, L'A', 0);
        SendMessageW(probe.handle(), WM_CHAR, 0x0D, 0);
        SendMessageW(probe.handle(), WM_CHAR, 0x08, 0);
        SendMessageW(probe.handle(), WM_CHAR, 0x09, 0);
        SendMessageW(probe.handle(), WM_CHAR, 0x1B, 0);
        SendMessageW(probe.handle(), WM_CHAR, 0x00, 0);
    }
    bool text_a = false;
    bool control_text = false;
    for (const dhsh::WindowEvent& event : events) {
        if (event.type != Type::text_input) {
            continue;
        }
        text_a = text_a || event.text == "A";
        const unsigned char first = event.text.empty()
            ? 0 : static_cast<unsigned char>(event.text.front());
        control_text = control_text || first < 0x20;
    }
    check(text_a, "printable WM_CHAR did not emit UTF-8 text input");
    check(!control_text,
        "a control-character WM_CHAR became a text input event");
}

// End-to-end replay of the main-window input path exactly as App.cpp wires
// it: every Win32 event feeds note_key_down() on key down and then
// ScpWindow::handle_event(), at the real 800x600 client size so the bar
// bottom strip (input at client y 586..602) is reachable. Clicking the
// bottom input focuses it and typed text stays visible; the physical Enter
// sequence (WM_KEYDOWN, the TranslateMessage WM_CHAR(0x0D), WM_KEYUP) must
// send the typed text exactly once and leave the edit empty, so a held
// Enter key can never re-send.
void test_main_chat_bar_physical_enter_sequence_leaves_edit_empty() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_bar_window(resources, bus);
    RecordingSendSink sink;
    dhsh::MainChatBar bar(window, bus, sink);
    auto* edit = static_cast<dhsh::RichEdit*>(window.behavior_for("input_mes"));
    check(edit != nullptr, "input_mes RICHEDIT behavior was not registered");

    const auto deliver = [&window, &bar](const dhsh::WindowEvent& event) {
        if (event.type == dhsh::WindowEvent::Type::key_down) {
            bar.note_key_down(event.key);
        }
        window.handle_event(event);
    };
    {
        dhsh::Win32Window probe(L"main_chat_bar_enter_probe", 800, 600,
            [&](const dhsh::WindowEvent& event) { deliver(event); });
        SendMessageW(probe.handle(), WM_LBUTTONDOWN, MK_LBUTTON,
            MAKELPARAM(100, 590));
        SendMessageW(probe.handle(), WM_LBUTTONUP, 0, MAKELPARAM(100, 590));
        check(window.focused_widget() == window.find_widget("input_mes"),
            "clicking the bottom input_mes did not focus it");
        SendMessageW(probe.handle(), WM_CHAR, L'a', 0);
        SendMessageW(probe.handle(), WM_CHAR, L'b', 0);
        check(edit->text() == "ab",
            "typed text did not stay visible in the bottom input edit");
        SendMessageW(probe.handle(), WM_KEYDOWN, VK_RETURN, 0);
        SendMessageW(probe.handle(), WM_CHAR, 0x0D, 0);
        SendMessageW(probe.handle(), WM_KEYUP, VK_RETURN, 0);
    }
    check(sink.sends.size() == 1 && sink.sends.front().first == 1
            && sink.sends.front().second == "ab",
        "the physical Enter sequence did not send the typed text exactly once");
    check(edit->text().empty(),
        "the Enter WM_CHAR re-inserted a carriage return into the cleared edit");

    {
        dhsh::Win32Window probe(L"main_chat_bar_enter_probe2", 800, 600,
            [&](const dhsh::WindowEvent& event) { deliver(event); });
        SendMessageW(probe.handle(), WM_LBUTTONDOWN, MK_LBUTTON,
            MAKELPARAM(100, 590));
        SendMessageW(probe.handle(), WM_LBUTTONUP, 0, MAKELPARAM(100, 590));
        SendMessageW(probe.handle(), WM_CHAR, L'x', 0);
        SendMessageW(probe.handle(), WM_KEYDOWN, VK_RETURN, 0);
        SendMessageW(probe.handle(), WM_CHAR, 0x0D, 0);
        SendMessageW(probe.handle(), WM_KEYUP, VK_RETURN, 0);
        SendMessageW(probe.handle(), WM_KEYDOWN, VK_RETURN, 0);
        SendMessageW(probe.handle(), WM_CHAR, 0x0D, 0);
        SendMessageW(probe.handle(), WM_KEYUP, VK_RETURN, 0);
    }
    check(sink.sends.size() == 2 && sink.sends.back().second == "x",
        "a held Enter key must not re-send after the edit was cleared");
}

int count_class_windows(const wchar_t* class_name) {
    struct State {
        const wchar_t* name;
        int count;
    };
    State state{class_name, 0};
    EnumWindows([](HWND handle, LPARAM param) -> BOOL {
        auto* state = reinterpret_cast<State*>(param);
        wchar_t name[128]{};
        if (GetClassNameW(handle, name, 128) > 0 && wcscmp(name, state->name) == 0) {
            ++state->count;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&state));
    return state.count;
}

std::unique_ptr<dhsh::ScpChatWindow> make_chat_window(MapResources& resources,
    dhsh::ChatModel& model, dhsh::ChatSendSink& sink, int x, int y, int w, int h,
    bool local_preview = false) {
    resources.add("ui/main/chatmain.scp", chatmain_source(false));
    resources.add("chat/chatskin/skin0.tga", std::string(8, '\0'));
    return std::make_unique<dhsh::ScpChatWindow>(resources, "ui/main/chatmain.scp",
        model, sink, x, y, w, h, local_preview);
}

void click_client(HWND handle, int x, int y) {
    SendMessageW(handle, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    SendMessageW(handle, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
}

// The physical wheel message carries screen coordinates; translate them
// exactly like the window proc does so the point lands where requested.
void wheel_client(HWND handle, int x, int y, int delta) {
    POINT point{x, y};
    ClientToScreen(handle, &point);
    SendMessageW(handle, WM_MOUSEWHEEL, MAKEWPARAM(0, delta),
        MAKELPARAM(static_cast<std::int16_t>(point.x),
            static_cast<std::int16_t>(point.y)));
}

void paint_client(HWND handle) {
    SendMessageW(handle, WM_PAINT, 0, 0);
}

void test_chat_main_layout_matches_native_geometry_at_549x590() {
    // 549x590 is the measured runtime client area of the original detached
    // window; Layout receives (cw, ch-28) and never scales the SCP tree.
    const dhsh::ChatMainLayout layout =
        dhsh::layout_chat_main(549, 590, 3, 20, 20, 28, false);
    check(layout.client_width == 549 && layout.client_height == 590,
        "layout must keep the runtime client size it was given");
    check(layout.content_height == 562,
        "Layout input height must be ch-28 (FUN_00401970 -> FSChatMain::Layout)");
    check(layout.switch_offset_y == 23,
        "dy must be switchbtn top+height (3+20), not a scaled value");
    check(layout.all_buttons == dhsh::LogicalRect{0, 542, 549, 20},
        "allbtn must be moved to the absolute bottom row y=H-bottom+dy=542");
    check(layout.message_list == dhsh::LogicalRect{0, 23, 549, 519},
        "message list must fill (0, dy, cw, H-dy-bottom)");
    check(layout.message_text_width == 545,
        "list text width must be cw-4 (right inset)");
    check(!layout.input_bar_visible,
        "detached layout must hide the input row like the original");
}

void test_chat_main_layout_second_size_proves_absolute_rows() {
    // A second size must recompute the same absolute formulas; proportional
    // scaling of the 549x590 rows would land elsewhere.
    const dhsh::ChatMainLayout compact =
        dhsh::layout_chat_main(400, 300, 3, 20, 20, 28, false);
    check(compact.content_height == 272 && compact.switch_offset_y == 23
            && compact.all_buttons == dhsh::LogicalRect{0, 252, 400, 20}
            && compact.message_list == dhsh::LogicalRect{0, 23, 400, 229}
            && compact.message_text_width == 396,
        "400x300 must recompute absolute rows (y=ch-28-20=252; scaling would give ~275)");

    const dhsh::ChatMainLayout attached =
        dhsh::layout_chat_main(549, 590, 3, 20, 20, 28, true);
    check(attached.input_bar_visible
            && attached.input_bar == dhsh::LogicalRect{0, 534, 549, 28}
            && attached.all_buttons == dhsh::LogicalRect{0, 514, 549, 20}
            && attached.message_list == dhsh::LogicalRect{0, 23, 549, 491},
        "attached layout must stack list, allbtn and input bar by the same H=ch-28 formula");
    check(attached.message_list.y + attached.message_list.height
            == attached.all_buttons.y
            && attached.all_buttons.y + attached.all_buttons.height
            == attached.input_bar.y
            && attached.input_bar.y + attached.input_bar.height
            == attached.content_height,
        "attached rows must stack exactly without gaps or overlaps");
}

void test_chat_window_native_layout_skin_frame_and_close_behavior() {
    MapResources resources;
    dhsh::ChatModel model;
    RecordingSendSink sink;
    auto window = make_chat_window(resources, model, sink, 60, 60, 549, 590);
    const HWND handle = window->handle();
    check(handle != nullptr, "chat window was not created");
    const dhsh::ScpWindow* ui = window->ui();
    check(ui != nullptr, "chat window must expose its SCP UI for inspection");

    const dhsh::Widget* strip = ui->find_widget("chatmain_switchbtn");
    check(strip != nullptr && strip->bounds() == dhsh::LogicalRect{3, 3, 637, 20},
        "switch strip must keep its SCP origin: source x=3 stays 3 (no scaling)");
    const dhsh::Widget* list = ui->find_widget("chatmain_txtlst_msg");
    check(list != nullptr && list->bounds() == dhsh::LogicalRect{0, 23, 549, 519},
        "message list must be laid out at (0,23,cw,H-dy-bottom) in native units");
    check(list != nullptr && list->client_rect().has_value()
            && *list->client_rect() == dhsh::LogicalRect{1, 0, 545, 519},
        "list client rect must give the evidenced (1,dy) origin with cw-4 text width");
    const dhsh::Widget* allbtn = ui->find_widget("chatmain_allbtn");
    check(allbtn != nullptr && allbtn->bounds() == dhsh::LogicalRect{0, 542, 549, 20},
        "allbtn must move to the absolute bottom row (0,542,cw,20)");
    const dhsh::Widget* stop = ui->find_widget("chatmain_stop");
    check(stop != nullptr && stop->bounds() == dhsh::LogicalRect{23, 542, 24, 18},
        "allbtn children must move with the row: (23,0) -> (23,542)");
    const dhsh::Widget* bar = ui->find_widget("chatmain_barwnd");
    check(bar != nullptr && !bar->visible(),
        "detached chat window must hide the input row like the original");
    const dhsh::Widget* edit = ui->find_widget("chatmain_input_mes");
    check(edit != nullptr && edit->supported() && !edit->visible(),
        "input edit stays supported but hidden in detached mode");

    RECT client_rect{};
    RECT window_rect{};
    GetClientRect(handle, &client_rect);
    GetWindowRect(handle, &window_rect);
    check(client_rect.right - client_rect.left == 549
            && client_rect.bottom - client_rect.top == 590,
        "client area must stay 549x590");
    check(window_rect.right - window_rect.left == 565
            && window_rect.bottom - window_rect.top == 629,
        "frame must be 565x629 like the original (caption + thick frame)");
    const LONG style = GetWindowLongW(handle, GWL_STYLE);
    check((style & WS_SYSMENU) == 0 && (style & WS_CAPTION) != 0
            && (style & WS_THICKFRAME) != 0,
        "chat window must have no WS_SYSMENU (no X button) but keep caption+thickframe");
    const LONG ex_style = GetWindowLongW(handle, GWL_EXSTYLE);
    check((ex_style & WS_EX_WINDOWEDGE) != 0,
        "chat window must keep the WS_EX_WINDOWEDGE border");

    std::size_t skin0_reads = 0;
    std::size_t skin2_reads = 0;
    for (const std::string& read : resources.reads) {
        skin0_reads += read == "chat/chatskin/skin0.tga" ? 1 : 0;
        skin2_reads += read.find("skin2") != std::string::npos ? 1 : 0;
    }
    check(skin0_reads == 1, "skin0 must be requested exactly once");
    check(skin2_reads == 0, "the new-version client must never request skin2");

    SendMessageW(handle, WM_CLOSE, 0, 0);
    check(!window->closed() && IsWindow(handle),
        "WM_CLOSE must not close the chat window (the original has no close path)");
}

void test_chat_window_resize_recomputes_absolute_layout() {
    MapResources resources;
    dhsh::ChatModel model;
    RecordingSendSink sink;
    auto window = make_chat_window(resources, model, sink, 60, 60, 549, 590);
    const HWND handle = window->handle();
    RECT target{0, 0, 400, 300};
    AdjustWindowRectEx(&target, GetWindowLongW(handle, GWL_STYLE), FALSE,
        GetWindowLongW(handle, GWL_EXSTYLE));
    SetWindowPos(handle, nullptr, 60, 60, target.right - target.left,
        target.bottom - target.top, SWP_NOZORDER | SWP_NOACTIVATE);
    const dhsh::ScpWindow* ui = window->ui();
    check(ui->find_widget("chatmain_allbtn")->bounds()
            == dhsh::LogicalRect{0, 252, 400, 20},
        "resizing to 400x300 must move allbtn to y=ch-28-20=252");
    check(ui->find_widget("chatmain_switchbtn")->bounds()
            == dhsh::LogicalRect{3, 3, 637, 20},
        "resizing must not move the switch strip: source x=3 stays 3");
    check(ui->find_widget("chatmain_txtlst_msg")->bounds()
            == dhsh::LogicalRect{0, 23, 400, 229},
        "resizing must recompute the list rect natively (no scale factor)");
}

void test_chat_window_shares_model_and_filter_preserves_history() {
    MapResources resources;
    dhsh::ChatModel model;
    RecordingSendSink sink;
    auto window = make_chat_window(resources, model, sink, 60, 60, 549, 590);
    auto* view = static_cast<dhsh::ListViewEx*>(
        window->ui()->behavior_for("chatmain_txtlst_msg"));
    check(view != nullptr && view->model() == &model,
        "chat window must bind the shared chat model without copying history");
    const auto append = [&](std::uint32_t channel, const std::string& text) {
        dhsh::ChatMessage message;
        message.channel = channel;
        message.spans = {{dhsh::ChatSpanKind::text, text, "red", {}}};
        model.append(std::move(message));
    };
    append(0, "a");
    append(5, "b");
    append(9, "c");
    check(view->rows().size() == 3, "shared appends must reach the chat list");
    // World filter through the real top-strip button (abs x=98..122, y=3..21).
    click_client(window->handle(), 105, 12);
    check(view->rows().size() == 1,
        "world filter button must show only world messages");
    // Back to "all" through the first strip button (abs x=3..27).
    click_client(window->handle(), 10, 12);
    check(view->rows().size() == 3,
        "filter changes must keep history: all messages return");
}

void test_chat_window_pause_appends_without_jump_and_resume_returns() {
    MapResources resources;
    dhsh::ChatModel model;
    RecordingSendSink sink;
    auto window = make_chat_window(resources, model, sink, 60, 60, 549, 590);
    const HWND handle = window->handle();
    const auto append = [&](const std::string& text) {
        dhsh::ChatMessage message;
        message.channel = 5;
        message.spans = {{dhsh::ChatSpanKind::text, text, "red", {}}};
        model.append(std::move(message));
    };
    for (int index = 0; index < 40; ++index) {
        append(std::string(80, static_cast<char>('a' + index % 26)));
    }
    auto* view = static_cast<dhsh::ListViewEx*>(
        window->ui()->behavior_for("chatmain_txtlst_msg"));
    paint_client(handle);
    check(view->first_visible_row() == view->bottom_first_row()
            && view->first_visible_row() > 0,
        "auto-scroll must pin the view to the newest message");
    // Pause through the stop button (abs x=23..47, y=542..560).
    click_client(handle, 35, 551);
    check(!model.auto_scroll(), "stop button must pause auto-scroll");
    const std::size_t paused_first = view->first_visible_row();
    for (int index = 0; index < 5; ++index) {
        append(std::string(80, 'z'));
    }
    paint_client(handle);
    check(view->first_visible_row() == paused_first,
        "appending while paused must not jump the view");
    check(window->ui()->find_widget("chatmain_stop")->visible() == false
            && window->ui()->find_widget("chatmain_stop2")->visible(),
        "pausing must swap the stop buttons like the original");
    // Resume through stop2, which occupies the same spot while paused.
    click_client(handle, 35, 551);
    check(model.auto_scroll(), "stop2 button must resume auto-scroll");
    paint_client(handle);
    check(view->first_visible_row() == view->bottom_first_row()
            && view->first_visible_row() > paused_first,
        "resuming must return the view to the newest message");
}

// Auto-scroll must keep pinning the newest message once the wrapped history
// grows past the SCROLL VALUERGN span (the real chatmain declares 10..100,
// span 90): the maximum scroll value is the bottom endpoint, so the first
// visible row must follow bottom_first_row() instead of stopping "span rows
// down" from the top.
void test_chat_window_auto_scroll_reaches_bottom_past_range_span() {
    MapResources resources;
    dhsh::ChatModel model;
    RecordingSendSink sink;
    auto window = make_chat_window(resources, model, sink, 60, 60, 549, 590);
    const HWND handle = window->handle();
    auto* view = static_cast<dhsh::ListViewEx*>(
        window->ui()->behavior_for("chatmain_txtlst_msg"));
    check(view != nullptr, "LISTVIEWEX behavior was not registered");
    const int span = view->scroll_range().max - view->scroll_range().min;
    // ChatModel caps history at 200 messages; one short message per visual
    // row pushes the bottom first row well past the 90-row virtual range.
    for (int index = 0;
            index < static_cast<int>(dhsh::ChatModel::max_messages); ++index) {
        dhsh::ChatMessage message;
        message.channel = 5;
        message.spans = {{dhsh::ChatSpanKind::text, "m" + std::to_string(index),
            "red", {}}};
        model.append(std::move(message));
    }
    check(view->rows().size() == dhsh::ChatModel::max_messages,
        "short messages must each occupy exactly one visual row");
    check(view->bottom_first_row() > static_cast<std::size_t>(span),
        "fixture must push the bottom first row past the VALUERGN span");
    paint_client(handle);
    check(view->first_visible_row() == view->bottom_first_row(),
        "auto-scroll past the VALUERGN span must pin the first row to the bottom");
}

// Fills the shared model to the 200-message cap with one-row messages and
// paints once, returning the pinned bottom first row (past the VALUERGN span).
std::size_t paint_to_bottom(dhsh::ChatModel& model, HWND handle,
    dhsh::ListViewEx* view) {
    for (int index = 0;
            index < static_cast<int>(dhsh::ChatModel::max_messages); ++index) {
        dhsh::ChatMessage message;
        message.channel = 5;
        message.spans = {{dhsh::ChatSpanKind::text, "m" + std::to_string(index),
            "red", {}}};
        model.append(std::move(message));
    }
    paint_client(handle);
    const std::size_t bottom = view->bottom_first_row();
    const int span = view->scroll_range().max - view->scroll_range().min;
    check(bottom > static_cast<std::size_t>(span),
        "fixture must push the bottom first row past the VALUERGN span");
    check(view->first_visible_row() == bottom,
        "auto-scroll must pin the view to the bottom first");
    return bottom;
}

// Leaving the bottom through the real paging button must work even when the
// history outgrows the SCROLL VALUERGN span: the visual row target and the
// virtual scroll value must not clamp each other back onto the bottom.
void test_chat_window_up_button_leaves_bottom_by_one_row() {
    MapResources resources;
    dhsh::ChatModel model;
    RecordingSendSink sink;
    auto window = make_chat_window(resources, model, sink, 60, 60, 549, 590);
    const HWND handle = window->handle();
    auto* view = static_cast<dhsh::ListViewEx*>(
        window->ui()->behavior_for("chatmain_txtlst_msg"));
    check(view != nullptr, "LISTVIEWEX behavior was not registered");
    const std::size_t bottom = paint_to_bottom(model, handle, view);
    // Click the declared center of chat_btnup (computed from its SCP bounds,
    // not a hand-picked pixel).
    const dhsh::LogicalRect up = window->ui()->find_widget("chat_btnup")->bounds();
    click_client(handle, up.x + up.width / 2, up.y + up.height / 2);
    check(!model.auto_scroll(), "chat_btnup must pause auto-scroll");
    check(view->first_visible_row() == bottom - 1,
        "chat_btnup from the bottom must move exactly one row toward older content");
    paint_client(handle);
    check(view->first_visible_row() == bottom - 1,
        "pausing must keep the one-row-up position across paints");
}

// One wheel notch up from the auto-scrolled bottom must move the view
// exactly one row toward older content, not land on the raw virtual offset
// (row 90 of the 10..100 VALUERGN) far above the true bottom; one notch
// down must return to the bottom.
void test_chat_window_wheel_steps_one_row_off_the_bottom() {
    MapResources resources;
    dhsh::ChatModel model;
    RecordingSendSink sink;
    auto window = make_chat_window(resources, model, sink, 60, 60, 549, 590);
    const HWND handle = window->handle();
    auto* view = static_cast<dhsh::ListViewEx*>(
        window->ui()->behavior_for("chatmain_txtlst_msg"));
    check(view != nullptr, "LISTVIEWEX behavior was not registered");
    const std::size_t bottom = paint_to_bottom(model, handle, view);
    wheel_client(handle, 100, 100, 120);
    check(!model.auto_scroll(), "wheel up must pause auto-scroll");
    check(view->first_visible_row() == bottom - 1,
        "one wheel notch up from the bottom must move exactly one row");
    wheel_client(handle, 100, 100, -120);
    check(view->first_visible_row() == bottom,
        "one wheel notch down must return to the bottom");
}

// Repeated chat_btnup/chat_btndown paging must move exactly one visual row
// per click in both directions, even with far more rows than the SCROLL
// VALUERGN can address: the actual row position is the authoritative state
// and must not be re-quantized through the virtual scroll value on every
// click (91 virtual values cannot name 173 row positions exactly).
void test_chat_window_repeated_paging_steps_one_row() {
    MapResources resources;
    dhsh::ChatModel model;
    RecordingSendSink sink;
    auto window = make_chat_window(resources, model, sink, 60, 60, 549, 590);
    const HWND handle = window->handle();
    auto* view = static_cast<dhsh::ListViewEx*>(
        window->ui()->behavior_for("chatmain_txtlst_msg"));
    check(view != nullptr, "LISTVIEWEX behavior was not registered");
    const std::size_t bottom = paint_to_bottom(model, handle, view);
    const dhsh::LogicalRect up = window->ui()->find_widget("chat_btnup")->bounds();
    const dhsh::LogicalRect down =
        window->ui()->find_widget("chat_btndown")->bounds();
    const auto click_up = [&] {
        click_client(handle, up.x + up.width / 2, up.y + up.height / 2);
    };
    const auto click_down = [&] {
        click_client(handle, down.x + down.width / 2, down.y + down.height / 2);
    };
    for (int step = 1; step <= 4; ++step) {
        click_up();
        check(view->first_visible_row() == bottom - static_cast<std::size_t>(step),
            "each chat_btnup click must move exactly one row toward older content");
    }
    for (int step = 3; step >= 0; --step) {
        click_down();
        check(view->first_visible_row() == bottom - static_cast<std::size_t>(step),
            "each chat_btndown click must move exactly one row toward newer content");
    }
    check(view->first_visible_row() == bottom,
        "paging down must be able to return to the exact bottom");
}

void test_chat_window_input_enters_through_the_single_sink_without_fake_success() {
    MapResources resources;
    dhsh::ChatModel model;
    RecordingSendSink sink;
    auto window = make_chat_window(resources, model, sink, 60, 60, 549, 590);
    const HWND handle = window->handle();
    window->set_attached(true);
    check(window->ui()->find_widget("chatmain_barwnd")->visible(),
        "attached mode must show the input row");
    auto* edit = static_cast<dhsh::RichEdit*>(
        window->ui()->behavior_for("chatmain_input_mes"));
    check(edit != nullptr, "input edit behavior was not registered");
    // The edit sits at (56,538,300,16) once the bar row is shown.
    click_client(handle, 100, 545);
    SendMessageW(handle, WM_CHAR, L'h', 0);
    SendMessageW(handle, WM_CHAR, L'i', 0);
    check(edit->text() == "hi", "typed text must reach the focused edit");
    SendMessageW(handle, WM_KEYDOWN, VK_RETURN, 0);
    check(sink.sends.size() == 1 && sink.sends.front().second == "hi",
        "Enter must send the edit text through the single sink exactly once");
    check(sink.sends.front().first == 1,
        "the default send channel must be 当前 (current, channel 1)");
    check(edit->text().empty(), "a completed send must clear the edit");
    check(model.visible_messages().empty(),
        "sending must not echo into the shared model as a fake success");
    // The combo selection drives the send channel (world = index 4).
    auto* combo = static_cast<dhsh::ComboBox*>(
        window->ui()->behavior_for("chatmain_txtlstchanel"));
    check(combo != nullptr, "channel combo behavior was not registered");
    check(combo->select(4), "world combo item must be selectable");
    window->messages().dispatch({"chatmain_txtlstchanel", "FSM_COMBOBOX_SELECTED",
        -1, "OnChannelSel", "chatmain.scp"});
    check(window->selected_channel() == 5,
        "combo selection must set the evidenced world channel 5");
    SendMessageW(handle, WM_CHAR, L'x', 0);
    SendMessageW(handle, WM_KEYDOWN, VK_RETURN, 0);
    check(sink.sends.size() == 2 && sink.sends.back().first == 5,
        "subsequent sends must use the combo-selected channel");
}

void test_chat_window_switch_button_toggles_attached_without_new_window() {
    MapResources resources;
    dhsh::ChatModel model;
    RecordingSendSink sink;
    const int baseline =
        count_class_windows(dhsh::ScpChatWindow::kClassName);
    auto window = make_chat_window(resources, model, sink, 60, 60, 549, 590);
    int switches = 0;
    window->set_on_switch([&switches] { ++switches; });
    check(!window->attached(), "a new chat window must start detached");
    // chatmain_switch is the first allbtn button: y=542 detached, y=514
    // attached (the row stacks above the input bar). Click where it is.
    click_client(window->handle(), 12, 551);
    check(switches == 1 && window->attached(),
        "chatmain_switch must toggle attached mode and report the switch");
    check(window->ui()->find_widget("chatmain_barwnd")->visible(),
        "attached mode must reveal the input row");
    click_client(window->handle(), 12, 523);
    check(switches == 2 && !window->attached(),
        "a second click must toggle back to detached");
    check(!window->ui()->find_widget("chatmain_barwnd")->visible(),
        "detached mode must hide the input row again");
    click_client(window->handle(), 12, 551);
    check(switches == 3 && window->attached(),
        "repeated switch clicks must keep toggling idempotently");
    check(count_class_windows(dhsh::ScpChatWindow::kClassName) == baseline + 1,
        "switch clicks must never create additional Win32 windows");
}

void test_chat_window_host_toggle_is_idempotent_and_leaks_no_window() {
    const int baseline = count_class_windows(dhsh::ScpChatWindow::kClassName);
    MapResources resources;
    dhsh::ChatModel model;
    RecordingSendSink sink;
    int creates = 0;
    {
        dhsh::ChatWindowHost host([&]() {
            ++creates;
            return make_chat_window(resources, model, sink, 60, 60, 549, 590);
        });
        host.toggle();
        check(creates == 1 && host.visible(),
            "the first toggle must create and show the chat window");
        host.toggle();
        check(creates == 1 && !host.visible(),
            "the second toggle must hide the same window");
        host.toggle();
        check(creates == 1 && host.visible(),
            "the third toggle must show the same window again");
        host.toggle();
        host.toggle();
        host.toggle();
        check(creates == 1,
            "repeated toggles must never recreate the window");
        check(count_class_windows(dhsh::ScpChatWindow::kClassName)
                == baseline + 1,
            "exactly one chat window may exist while the host is alive");
    }
    check(count_class_windows(dhsh::ScpChatWindow::kClassName) == baseline,
        "destroying the host must destroy its Win32 window");
}

// Hiding the detached window while the pause buttons are swapped and
// showing it again must restore the SCP-declared visibility (the HIDE=1
// stop2 button stays hidden); a forced subtree-wide "visible" would leak
// the paused button back onto the bottom row.
void test_chat_window_hide_show_keeps_runtime_state() {
    MapResources resources;
    dhsh::ChatModel model;
    RecordingSendSink sink;
    dhsh::ChatWindowHost host([&]() {
        return make_chat_window(resources, model, sink, 60, 60, 549, 590);
    });
    host.toggle();
    const HWND handle = host.window()->handle();
    const auto state = [&]() {
        return std::string("stop=")
            + (host.window()->ui()->find_widget("chatmain_stop")->visible() ? "V" : "h")
            + " stop2="
            + (host.window()->ui()->find_widget("chatmain_stop2")->visible() ? "V" : "h")
            + " bar="
            + (host.window()->ui()->find_widget("chatmain_barwnd")->visible() ? "V" : "h")
            + " list="
            + (host.window()->ui()->find_widget("chatmain_txtlst_msg")->visible() ? "V" : "h");
    };
    click_client(handle, 35, 551); // pause: stop hides, stop2 shows
    check(!host.window()->ui()->find_widget("chatmain_stop")->visible()
            && host.window()->ui()->find_widget("chatmain_stop2")->visible(),
        "pausing must swap the stop buttons first");
    host.toggle(); // hide
    host.toggle(); // show again
    check(host.window()->ui()->find_widget("chatmain_stop2")->visible()
            && !host.window()->ui()->find_widget("chatmain_stop")->visible(),
        ("the paused stop2 button keeps its runtime state across hide/show ("
            + state() + ")").c_str());
    check(!host.window()->ui()->find_widget("chatmain_barwnd")->visible(),
        ("hide/show must keep the detached input row hidden (" + state() + ")").c_str());
    dhsh::ChatMessage message;
    message.channel = 5;
    message.spans = {{dhsh::ChatSpanKind::text, "hello", "red", {}}};
    model.append(std::move(message));
    auto* view = static_cast<dhsh::ListViewEx*>(
        host.window()->ui()->behavior_for("chatmain_txtlst_msg"));
    check(view != nullptr && view->rows().size() == 1,
        "the shared model history must still render after hide/show");
    check(host.window()->ui()->find_widget("chatmain_txtlst_msg")->visible(),
        ("the message list must stay visible across hide/show (" + state() + ")").c_str());
}

// Scrolled views must rebase rows into the content clip: rows carry their
// absolute layout offset, and drawing them as-is pushes every row below the
// first visible one outside the clip (nothing renders once the auto-roll or
// the user scrolls past the first page).
void test_listview_rows_rebase_into_view_when_scrolled() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_chatmain_window(true, resources, bus);
    dhsh::ChatModel model;
    window.set_chat_model(&model);
    auto* view = static_cast<dhsh::ListViewEx*>(
        window.behavior_for("chatmain_txtlst_msg"));
    check(view != nullptr, "LISTVIEWEX behavior was not registered");
    for (int index = 0; index < 60; ++index) {
        dhsh::ChatMessage message;
        message.channel = 5;
        message.spans = {{dhsh::ChatSpanKind::text,
            std::string(60, static_cast<char>('a' + index % 26)), "red", {}}};
        model.append(std::move(message));
    }
    check(view->rows().size() > view->visible_row_capacity(),
        "fixture must produce more rows than one page");
    const int bottom = static_cast<int>(
        view->rows().size() - view->visible_row_capacity());
    view->set_scroll_value(view->scroll_range().min + std::max(0, bottom));
    const std::size_t first = view->first_visible_row();
    check(first > 0 && first + view->visible_row_capacity() <= view->rows().size(),
        "scrolling to the bottom must move the first row without passing the end");

    dhsh::SoftwareRenderer renderer(700, 600);
    check(window.render(renderer), "scrolled chat rows did not render");
    const dhsh::Image& target = renderer.target();
    int text_pixels = 0;
    int first_text_y = -1;
    // Scan only the message-list band: the fixture's allbtn row renders its
    // button images lower down and must not satisfy this assertion.
    for (int y = 23; y < 460 && first_text_y < 0; ++y) {
        for (int x = 1; x < 546; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * 700 + x) * 4;
            if (target.rgba[offset + 3] != std::byte{0}) {
                first_text_y = y;
                break;
            }
        }
    }
    for (int y = 23; y < 460; ++y) {
        for (int x = 1; x < 546; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * 700 + x) * 4;
            if (target.rgba[offset + 3] != std::byte{0}) {
                ++text_pixels;
            }
        }
    }
    check(text_pixels > 0,
        "rows scrolled past the first page must still render inside the clip");
    check(first_text_y >= 23 && first_text_y < 23 + 40,
        "the first visible row must start at the content top, not its absolute offset");
}

// The first visible row must never pass the last visible page: (a) the
// wheel only honors the parsed VALUERGN, so with few rows scrolling down
// clamps scroll_value at the range maximum while the row offset runs past
// the end; (b) a paused (auto-scroll off) view keeps its offset when the
// channel filter shrinks the row count below it. Both must still show the
// last visible page instead of going blank.
void test_listview_first_row_clamps_to_last_visible_page() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_chatmain_window(true, resources, bus);
    dhsh::ChatModel model;
    window.set_chat_model(&model);
    auto* view = static_cast<dhsh::ListViewEx*>(
        window.behavior_for("chatmain_txtlst_msg"));
    check(view != nullptr, "LISTVIEWEX behavior was not registered");
    const auto band_pixels = [&]() {
        dhsh::SoftwareRenderer renderer(700, 600);
        window.render(renderer);
        const dhsh::Image& target = renderer.target();
        int pixels = 0;
        for (int y = 23; y < 460; ++y) {
            for (int x = 1; x < 546; ++x) {
                if (target.rgba[(static_cast<std::size_t>(y) * 700 + x) * 4 + 3]
                        != std::byte{0}) {
                    ++pixels;
                }
            }
        }
        return pixels;
    };
    const auto append = [&](std::uint32_t channel, const std::string& text) {
        dhsh::ChatMessage message;
        message.channel = channel;
        message.spans = {{dhsh::ChatSpanKind::text, text, "red", {}}};
        model.append(std::move(message));
    };

    // (a) Fewer rows than a page: wheel down to the VALUERGN maximum must
    // not push the first row past the content.
    append(5, "one");
    append(5, "two");
    append(5, "three");
    check(view->rows().size() < view->visible_row_capacity(),
        "fixture must start with less than one page of rows");
    for (int index = 0; index < 120; ++index) {
        window.handle_event(wheel_event({100, 100}, -120));
    }
    check(view->scroll_value() == view->scroll_range().max,
        "wheel down must reach the VALUERGN maximum");
    check(view->first_visible_row() == 0,
        "first row must clamp to the last visible page, not past the end");
    check(band_pixels() > 0,
        "scrolling past a short history must not blank the view");

    // (b) Paused view at an offset, then a filter that leaves fewer rows.
    for (int index = 0; index < 40; ++index) {
        append(5, std::string(80, static_cast<char>('a' + index % 26)));
    }
    append(9, "lonely other-channel line");
    check(view->rows().size() > view->visible_row_capacity(),
        "fixture must grow beyond one page");
    model.set_auto_scroll(false);
    const int span = view->scroll_range().max - view->scroll_range().min;
    view->set_scroll_value(view->scroll_range().min
        + std::min(12, span));
    check(view->first_visible_row() > 0,
        "the paused view must hold a non-zero first row");
    model.set_channel_filter(9);
    // Filter applied: only the single channel-9 message remains.
    check(view->rows().size() == 1,
        "filter must shrink below one page");
    check(view->first_visible_row() == 0,
        "first row must follow the shrunk content while paused");
    check(band_pixels() > 0,
        "a paused view must not go blank when the filter shrinks the rows");
    model.set_channel_filter(std::nullopt);
}

// The physical Enter sequence is WM_KEYDOWN, then TranslateMessage's
// WM_CHAR(0x0D), then WM_KEYUP. The key down sends and clears the edit; an
// unfiltered WM_CHAR would re-insert "" into the cleared buffer, so the
// next send carries a leading CR and key auto-repeat can re-send. Backspace
// has the same physical shape (its control WM_CHAR must not pollute either).
void test_chat_window_physical_enter_sequence_leaves_edit_empty() {
    MapResources resources;
    dhsh::ChatModel model;
    RecordingSendSink sink;
    auto window = make_chat_window(resources, model, sink, 60, 60, 549, 590);
    const HWND handle = window->handle();
    window->set_attached(true);
    auto* edit = static_cast<dhsh::RichEdit*>(
        window->ui()->behavior_for("chatmain_input_mes"));
    check(edit != nullptr, "input edit behavior was not registered");
    click_client(handle, 100, 545); // focus the input edit

    SendMessageW(handle, WM_CHAR, L'a', 0);
    SendMessageW(handle, WM_KEYDOWN, VK_RETURN, 0);
    SendMessageW(handle, WM_CHAR, 0x0D, 0);
    SendMessageW(handle, WM_KEYUP, VK_RETURN, 0);
    check(sink.sends.size() == 1 && sink.sends.front().second == "a",
        "the physical Enter sequence must send the typed text exactly once");
    check(edit->text().empty(),
        "the Enter WM_CHAR must not leave a carriage return in the edit");

    SendMessageW(handle, WM_CHAR, L'b', 0);
    SendMessageW(handle, WM_KEYDOWN, VK_RETURN, 0);
    SendMessageW(handle, WM_CHAR, 0x0D, 0);
    SendMessageW(handle, WM_KEYUP, VK_RETURN, 0);
    check(sink.sends.size() == 2 && sink.sends.back().second == "b",
        "the second physical Enter must send only the new text, without a leading CR");

    SendMessageW(handle, WM_CHAR, L'c', 0);
    SendMessageW(handle, WM_KEYDOWN, VK_BACK, 0);
    SendMessageW(handle, WM_CHAR, 0x08, 0);
    SendMessageW(handle, WM_KEYUP, VK_BACK, 0);
    check(edit->text().empty(),
        "the Backspace WM_CHAR must not insert a control byte into the edit");
    SendMessageW(handle, WM_CHAR, L'd', 0);
    SendMessageW(handle, WM_KEYDOWN, VK_RETURN, 0);
    SendMessageW(handle, WM_CHAR, 0x0D, 0);
    SendMessageW(handle, WM_KEYUP, VK_RETURN, 0);
    check(sink.sends.size() == 3 && sink.sends.back().second == "d",
        "text edited after Backspace must send cleanly");
}

// Records send attempts so tests can prove text leaves through the single
// sink and is never echoed into the shared model as a fake success.
// The main HUD chat bar (ui/main/bar.scp) is the on-screen counterpart of
// the detached chat window input row: clicking input_mes focuses it, typed
// text stays in the edit, and Enter must send the text through the single
// ChatSendSink and clear the edit. Before the MainChatBar bindings existed,
// the real client logged "Unsupported API: control=input_mes code=OnChatInput"
// on every key down and Enter never reached the sink (field evidence:
// .rea/replica_online_20261125_attempt24.console.log:114-146).
void test_main_chat_bar_enter_sends_through_sink_and_clears() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_bar_window(resources, bus);
    RecordingSendSink sink;
    dhsh::MainChatBar bar(window, bus, sink);
    auto* edit = static_cast<dhsh::RichEdit*>(window.behavior_for("input_mes"));
    check(edit != nullptr, "input_mes RICHEDIT behavior was not registered");
    using Type = dhsh::WindowEvent::Type;

    // The 800x600 bottom strip: barwnd sits at (0,582) and input_mes is at
    // client (56,586,300,16). Clicking anywhere on the visible input row
    // (here x=100, y=590) must focus it.
    window.handle_event(mouse_event(Type::left_button_down, {100, 590}));
    window.handle_event(mouse_event(Type::left_button_up, {100, 590}));
    check(window.focused_widget() == window.find_widget("input_mes"),
        "clicking input_mes did not focus it");
    window.handle_event(text_event("hello"));
    check(edit->text() == "hello",
        "typed text did not stay visible in the input edit");

    bar.note_key_down(VK_RETURN);
    window.handle_event(key_event(VK_RETURN));
    check(sink.sends.size() == 1 && sink.sends.front().first == 1
            && sink.sends.front().second == "hello",
        "Enter on the focused input did not send its text once through the sink");
    check(edit->text().empty(), "Enter did not clear the input edit");
    check(bus.diagnostics().empty(),
        "the OnChatInput message was still dispatched without a subscriber: "
            + (bus.diagnostics().empty() ? std::string() : bus.diagnostics().front()));

    // The send must not fire twice on the physical key sequence (key repeat
    // or the Enter WM_CHAR) and must stay silent for other keys / empty text.
    bar.note_key_down(VK_RETURN);
    window.handle_event(key_event(VK_RETURN));
    check(sink.sends.size() == 1,
        "Enter on an empty edit must not send");
    window.handle_event(text_event("x"));
    bar.note_key_down(VK_LEFT);
    window.handle_event(key_event(VK_LEFT));
    check(sink.sends.size() == 1 && edit->text() == "x",
        "a non-Enter key down must not send");
    bar.note_key_down(VK_RETURN);
    window.handle_event(key_event(VK_RETURN));
    check(sink.sends.size() == 2 && sink.sends.back().second == "x",
        "Enter after editing must send only the new text");
}

// Original 1.50.35 client screenshot .rea/original_dynamic_main_client_20261125.png
// provides runtime evidence missing from the older PAK declarations. Its known
// input sample "test111" has a compact 39x8 dark-ink core, rather than the
// much larger Microsoft YaHei UI 14 raster used by the initial clone. The
// generic rasterizer must therefore use the small native-style default font;
// this test deliberately contains no channel label or widget-name special case.
void test_original_runtime_default_text_is_small_native_font() {
    const dhsh::LogicalRect bounds{0, 0, 300, 16};
    const dhsh::Image image = dhsh::rasterize_text_image(
        "test111", bounds, bounds, RGB(0, 0, 0));
    int left = bounds.width;
    int top = bounds.height;
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < bounds.height; ++y) {
        for (int x = 0; x < bounds.width; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * bounds.width + x) * 4;
            if (std::to_integer<unsigned char>(image.rgba[offset + 3]) < 128) {
                continue;
            }
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x);
            bottom = std::max(bottom, y);
        }
    }
    check(right >= left && bottom >= top,
        "the original-style ASCII text sample did not rasterize");
    const int width = right - left + 1;
    const int height = bottom - top + 1;
    check(width >= 35 && width <= 43 && height <= 10,
        "generic default text does not match the original small-font metrics: "
        "width=" + std::to_string(width) + " height=" + std::to_string(height));
}

// The original popup's nine rows are 18 px apart and grow upwards from the
// declared CLIENTRECT bottom: the first text origin is y=422 (ink near 424),
// not y=444 from dividing the old 140 px declaration into nine equal rows.
// This is a generic TXTLIST layout rule; the expected geometry comes only from
// RECT, CLIENTRECT, item count and the observed native row height.
void test_original_runtime_txtlist_rows_are_bottom_anchored() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_bar_window(resources, bus);
    auto* combo = static_cast<dhsh::ComboBox*>(
        window.behavior_for("bar_txtlstchanel"));
    check(combo != nullptr, "channel combo behavior missing for TXTLIST layout");
    check(combo->item_area() == dhsh::LogicalRect{23, 422, 26, 162},
        "TXTLIST must use nine 18px rows anchored to the CLIENTRECT bottom");
    check(combo->item_at({30, 424}) == std::optional<std::size_t>{0}
            && combo->item_at({30, 502}) == std::optional<std::size_t>{4}
            && combo->item_at({30, 568}) == std::optional<std::size_t>{8},
        "bottom-anchored TXTLIST rows do not hit-test at original coordinates");
}

// Physical path for the bar channel combo, mirroring the original 1.50.35
// runtime at 800x600: clicking the SCP-declared channel BUTTON fixedpart
// (bar_btnchanel, client 20,584,32,30) opens the menu above the bar
// (.rea/original_main_channels_20261125.png), clicking the 世界 row selects
// it through FSM_COMBOBOX_SELECTED OnChannelSel, the label on the EDIT
// fixedpart updates to 世界, and the unsent input text is preserved without
// auto-sending (.rea/original_main_world_selected_20261125.png). The next
// Enter then sends on channel 5; the same physical path back to 当前 sends
// on channel 1.
void test_main_chat_bar_channel_combo_physical_click() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_bar_window(resources, bus);
    RecordingSendSink sink;
    dhsh::MainChatBar bar(window, bus, sink);
    auto* combo = static_cast<dhsh::ComboBox*>(
        window.behavior_for("bar_txtlstchanel"));
    auto* edit = static_cast<dhsh::RichEdit*>(window.behavior_for("input_mes"));
    check(combo != nullptr && edit != nullptr,
        "bar_txtlstchanel COMBOBOX or input_mes RICHEDIT behavior missing");
    using Type = dhsh::WindowEvent::Type;
    const auto click = [&window](dhsh::LogicalPoint point) {
        window.handle_event(mouse_event(Type::left_button_down, point));
        window.handle_event(mouse_event(Type::left_button_up, point));
    };
    // Selected channel label band on the bar_editchanel display field
    // (client 23,585,32,30; the 582..600 strip is what the 800x600 window
    // shows at the bottom).
    const auto label_band = [&window]() {
        dhsh::SoftwareRenderer renderer(800, 600);
        window.render(renderer);
        const dhsh::Image& target = renderer.target();
        std::vector<std::byte> band;
        for (int y = 585; y < 600; ++y) {
            for (int x = 23; x < 55; ++x) {
                for (int c = 0; c < 4; ++c) {
                    band.push_back(
                        target.rgba[(static_cast<std::size_t>(y) * 800 + x) * 4 + c]);
                }
            }
        }
        return band;
    };

    // Initial state matches the original main window at login
    // (.rea/original_main_screen_20261125.png shows 当前 before any pick):
    // the combo natively starts on the first ADDTEXT item and the visible
    // EDIT fixedpart already shows it.
    check(combo->selected_index() == std::optional<std::size_t>{0},
        "channel combo must start selected on the first ADDTEXT item (当前)");
    const std::vector<std::byte> initial = label_band();
    check(std::any_of(initial.begin(), initial.end(),
            [](std::byte value) { return value != std::byte{0}; }),
        "the 当前 label is not visible on the EDIT fixedpart before any pick");

    // Type first: the channel pick must preserve the unsent text.
    click({100, 590});
    check(window.focused_widget() == window.find_widget("input_mes"),
        "clicking input_mes in the bottom strip did not focus it");
    window.handle_event(text_event("hello"));
    check(edit->text() == "hello",
        "typed text did not stay visible in the input edit");

    // Channel BUTTON fixedpart (bar_btnchanel at client 20,584,32,30).
    click({35, 590});
    check(combo->dropdown_visible(),
        "clicking the channel BUTTON fixedpart did not open the dropdown");
    // 世界 is row 4 of the 9 ADDTEXT items rendered inside the bottom-anchored
    // TXTLIST CLIENTRECT area (client 23,422,26,162). Rows are 18px high, so
    // row 4 spans y 494..511.
    click({30, 511});
    check(combo->selected_index() == std::optional<std::size_t>{4}
            && !combo->dropdown_visible(),
        "clicking the 世界 row did not select it and close the dropdown");
    check(edit->text() == "hello",
        "selecting a channel must preserve the unsent input text");
    check(bus.diagnostics().empty(),
        "the OnChannelSel message was dispatched without a subscriber");
    const std::vector<std::byte> world = label_band();
    check(world != initial,
        "the channel display did not update to the selected 世界 label");

    // No refocus click: after the channel pick the unsent text must send
    // with a plain Enter, exactly like the original main bar.
    bar.note_key_down(VK_RETURN);
    window.handle_event(key_event(VK_RETURN));
    check(sink.sends.size() == 1 && sink.sends.front().first == 5
            && sink.sends.front().second == "hello",
        "selecting 世界 did not route the plain Enter after the pick through channel 5");
    check(edit->text().empty(), "Enter did not clear the input edit");

    // Same physical path back to 当前 (row 0, y 422..439), plain Enter again.
    window.handle_event(text_event("c"));
    click({35, 590});
    click({30, 430});
    check(combo->selected_index() == std::optional<std::size_t>{0},
        "clicking the 当前 row did not select it");
    check(label_band() == initial,
        "the channel display did not return to the 当前 label");
    bar.note_key_down(VK_RETURN);
    window.handle_event(key_event(VK_RETURN));
    check(sink.sends.size() == 2 && sink.sends.back().first == 1
            && sink.sends.back().second == "c",
        "selecting 当前 did not route the plain Enter after the pick through channel 1");
    check(edit->text().empty(), "Enter did not clear the input edit");
}

// The barwnd subtree ships with HIDE=1 in the real chatmain.scp, so
// build_node skips image loading for the whole input row; showing the row
// at runtime (FSChatMain attached mode) must load those images on demand,
// otherwise the input background/combo render nothing at all.
void test_chatmain_hidden_subtree_loads_images_when_shown() {
    MapResources resources;
    resources.add("ui/bar/chatmain/chatinputbg.a16", std::string(8, ''));
    resources.add("ui/bar/channel.a16", std::string(8, ''));
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_chatmain_window(false, resources, bus);
    const dhsh::Widget* bar = window.find_widget("chatmain_barwnd");
    check(bar != nullptr && !bar->visible() && bar->image() == nullptr,
        "hidden barwnd must build without an image");
    const dhsh::Widget* drop = window.find_widget("chatmain_txtlst_channel");
    check(drop != nullptr && !drop->visible() && drop->image() == nullptr,
        "hidden channel list must build without an image");
    check(window.set_widget_subtree_visible("chatmain_barwnd", true),
        "showing the input row subtree failed");
    const dhsh::Widget* shown_bar = window.find_widget("chatmain_barwnd");
    const dhsh::Widget* shown_drop = window.find_widget("chatmain_txtlst_channel");
    check(shown_bar->visible() && shown_bar->image() != nullptr,
        "showing the input row must load the bar background image");
    check(shown_drop->visible() && shown_drop->image() != nullptr,
        "showing the input row must load the channel list image");
}

// Loads the shipped UI/main/chatmain.scp straight out of the real config.pak
// (NZ-packed on disk, decoded through the project's NZ decoder) and verifies
// the Task 7 control families against the real declarations.
void test_real_chatmain_declarations_from_config_pak(const std::string& shdata) {
    dhsh::PakResourceProvider resources(shdata, {"config.pak", "ui.pak"});
    const std::string path = "ui/main/chatmain.scp";
    const std::vector<std::byte> packed = resources.bytes(path);
    check(packed.size() >= 2 && packed[0] == std::byte{'N'}
            && packed[1] == std::byte{'Z'},
        "real chatmain.scp was not read as its original NZ resource");
    const dhsh::ScpDocument document =
        dhsh::ScpParser::parse(dhsh::NzDecoder::decode(packed), path);
    check(document.includes.empty(),
        "real chatmain.scp unexpectedly gained includes the fixture does not cover");

    dhsh::MessageBus bus;
    dhsh::ScpWindow window(std::move(document), resources, bus);

    const dhsh::Widget* list = window.find_widget("chatmain_txtlst_msg");
    check(list != nullptr && list->type() == dhsh::WidgetType::list_view_ex
            && list->supported(),
        "real chatmain_txtlst_msg is not a supported LISTVIEWEX");
    check(list->bounds() == dhsh::LogicalRect{3, 30, 640, 480}
            && list->client_rect().has_value()
            && *list->client_rect() == dhsh::LogicalRect{0, 0, 640, 480},
        "real LISTVIEWEX RECT/CLIENTRECT differ from the new-version declarations");
    const dhsh::Widget* input = window.find_widget("chatmain_input_mes");
    check(input != nullptr && input->type() == dhsh::WidgetType::rich_edit
            && input->supported() && !input->visible(),
        "real chatmain_input_mes is not a supported hidden RICHEDIT");
    check(input->bounds() == dhsh::LogicalRect{1656, 1204, 300, 16},
        "real RICHEDIT RECT differs from the new-version declaration");
    const dhsh::Widget* combo = window.find_widget("chatmain_txtlstchanel");
    check(combo != nullptr && combo->type() == dhsh::WidgetType::combo_box
            && combo->supported() && !combo->visible(),
        "real chatmain_txtlstchanel is not a supported hidden COMBOBOX");
    check(combo->bounds() == dhsh::LogicalRect{1552, 1200, 32, 30},
        "real COMBOBOX RECT differs from the new-version declaration");

    auto* view = static_cast<dhsh::ListViewEx*>(
        window.behavior_for("chatmain_txtlst_msg"));
    auto* picker = static_cast<dhsh::ComboBox*>(
        window.behavior_for("chatmain_txtlstchanel"));
    check(view != nullptr && picker != nullptr,
        "real chatmain behaviors were not registered");
    check(view->draw_text_type() == 2 && view->trans_msg(),
        "real LISTVIEWEX DRAWTXTTYPE/TRANSMSG differ from the new-version declarations");
    check(view->scroll_range().min == 10 && view->scroll_range().max == 100
            && view->scroll_range().page == 10 && view->scroll_value() == 15,
        "real LISTVIEWEX SCROLL VALUERGN/PAGESIZE/VALUE were not parsed");
    check(view->track_rect() == dhsh::LogicalRect{113, 150, 20, 20},
        "real LISTVIEWEX SCROLL track geometry differs");

    // Every SCROLL fixed part in the real file (message list plus both
    // channel-list scrolls) must be a supported widget now.
    std::size_t scrolls = 0;
    for (const dhsh::Widget& widget : window.widgets()) {
        if (widget.type() == dhsh::WidgetType::scroll) {
            check(widget.supported(), "real SCROLL control was not supported");
            ++scrolls;
        }
    }
    check(scrolls >= 3, "real chatmain.scp lost its SCROLL fixed parts");

    const std::vector<std::string> expected = {
        "\xE5\xBD\x93\xE5\x89\x8D", "\xE9\x98\x9F\xE4\xBC\x8D", "\xE5\xB8\xAE\xE6\xB4\xBE",
        "\xE7\xA7\x81\xE8\x81\x8A", "\xE4\xB8\x96\xE7\x95\x8C", "\xE7\xAB\x9E\xE6\x8A\x80",
        "G M", "\xE5\x90\x8C\xE5\x9F\x8E", "\xE7\xAB\x9E\xE6\x8A\x80",
        "\xE5\x9C\xB0\xE5\x9B\xBE"};
    check(picker->items() == expected,
        "real COMBOBOX items differ from the ADDTEXT channel labels");

    const std::vector<dhsh::UiMessage> declarations = window.message_declarations();
    check(has_message(declarations, "chatmain_input_mes", "FSM_KEY_DOWN", -1, "OnChatInput")
            && has_message(declarations, "chatmain_txtlstchanel",
                "FSM_COMBOBOX_SELECTED", -1, "OnChannelSel")
            && has_message(declarations, "chatmain_txtlst_msg", "FSM_MOUSE_DOWN", 2,
                "OnTxtListButtonDown"),
        "real chatmain message declarations differ from the new-version names");

    std::size_t unknown = 0;
    for (const std::string& diagnostic : window.diagnostics()) {
        if (diagnostic.find("Unsupported") == std::string::npos) {
            continue;
        }
        check(diagnostic.find("type=LISTVIEWEX") == std::string::npos
                && diagnostic.find("type=RICHEDIT") == std::string::npos
                && diagnostic.find("type=COMBOBOX") == std::string::npos
                && diagnostic.find("type=SCROLL") == std::string::npos,
            "real target control was reported unsupported: " + diagnostic);
        ++unknown;
    }
    check(unknown > 0, "real chatmain.scp produced no unknown-control diagnostics");

    std::cout << "REAL_SCP_UI PASS pak=config.pak,ui.pak path=" << path
              << " widgets=" << window.widgets().size()
              << " scrolls=" << scrolls << " unsupported=" << unknown << "\n";
}

// Locks the layout fact the main chat bar tests rely on: the App mounts
// 800config.pak before config.pak, so ui/main/bar.scp resolves to the
// 800x600-anchored variant (barwnd 0,582,800,600 -- input row in the
// 582..600 bottom strip). This is the shipped configuration table the
// original 1.50.35 runtime uses at 800x600 (see
// .rea/original_main_typed_20261125.png); no replica-side layout function
// repositions the bar.
void test_real_bar_input_row_from_800config_pak(const std::string& shdata) {
    dhsh::PakResourceProvider resources(
        shdata, {"800config.pak", "config.pak", "ui.pak"});
    const std::string path = "ui/main/bar.scp";
    const std::vector<std::byte> packed = resources.bytes(path);
    check(packed.size() >= 2 && packed[0] == std::byte{'N'}
            && packed[1] == std::byte{'Z'},
        "real bar.scp was not read as its original NZ resource");
    dhsh::ScpDocument document =
        dhsh::ScpParser::parse(dhsh::NzDecoder::decode(packed), path);
    check(document.includes.empty(),
        "real bar.scp unexpectedly gained includes the fixture does not cover");
    dhsh::MessageBus bus;
    dhsh::ScpWindow window(std::move(document), resources, bus);

    const dhsh::Widget* barwnd = window.find_widget("barwnd");
    check(barwnd != nullptr && barwnd->bounds() == dhsh::LogicalRect(0, 582, 800, 600),
        "app-resolved bar.scp is not the 800config 800x600 variant");
    const dhsh::Widget* input = window.find_widget("input_mes");
    check(input != nullptr && input->type() == dhsh::WidgetType::rich_edit
            && input->supported()
            && input->bounds() == dhsh::LogicalRect(56, 586, 300, 16),
        "real input_mes is not the supported RICHEDIT in the bottom strip");
    const dhsh::Widget* button = window.find_widget("bar_btnchanel");
    check(button != nullptr
            && button->bounds() == dhsh::LogicalRect(20, 584, 32, 30),
        "real bar_btnchanel FIXEDPART is not at the clickable bottom position");
    const dhsh::Widget* drop = window.find_widget("bar_txtlst_channel");
    check(drop != nullptr && drop->client_rect().has_value()
            && *drop->client_rect() == dhsh::LogicalRect(4, 2, 26, 140),
        "real bar_txtlst_channel CLIENTRECT is not the 9-row channel area");
    check(drop->image() != nullptr,
        "real bar_txtlst_channel did not load ui/bar/channel.a16");
    auto* combo = static_cast<dhsh::ComboBox*>(
        window.behavior_for("bar_txtlstchanel"));
    check(combo != nullptr && combo->items().size() == 9,
        "real channel combo does not carry the 9 bar ADDTEXT items");
    const auto declarations = window.message_declarations();
    check(has_message(declarations, "input_mes", "FSM_KEY_DOWN", -1, "OnChatInput")
            && has_message(declarations, "bar_txtlstchanel",
                "FSM_COMBOBOX_SELECTED", -1, "OnChannelSel"),
        "real bar.scp lost the OnChatInput / OnChannelSel declarations");

    std::cout << "REAL_SCP_UI PASS pak=800config.pak,config.pak,ui.pak path="
              << path << " widgets=" << window.widgets().size() << "\n";
}

void test_real_button_states_native_channel_skin_and_chat_messages(
    const std::string& shdata) {
    dhsh::PakResourceProvider resources(
        shdata, {"800config.pak", "config.pak", "ui.pak"});
    dhsh::MessageBus bus;
    dhsh::ScpWindow window(dhsh::ScpParser::load(resources, "ui/main/bar.scp"),
        resources, bus);
    dhsh::Widget* button = window.find_widget("bar_btnchanel");
    check(button != nullptr, "real bar_btnchanel was not built");
    for (const dhsh::WidgetImageState state : {
             dhsh::WidgetImageState::normal, dhsh::WidgetImageState::hover,
             dhsh::WidgetImageState::pressed, dhsh::WidgetImageState::disabled}) {
        check(button->image(state) != nullptr,
            "a real Btnchannel button state image was not loaded");
    }
    const auto sources_for = [&resources](std::string_view logical) {
        auto sources = resources.sources(logical);
        if (sources.empty() && logical.starts_with("ui/")) {
            sources = resources.sources(logical.substr(3));
        }
        return sources;
    };
    const auto has_source_suffix = [&sources_for](std::string_view logical,
                                       std::string_view suffix) {
        const auto sources = sources_for(logical);
        return !sources.empty() && sources.front().path.ends_with(suffix);
    };
    for (const char* path : {"ui/bar/Btnchannel/default.tga",
             "ui/bar/Btnchannel/mouseon.tga", "ui/bar/Btnchannel/pressed.tga",
             "ui/bar/Btnchannel/disable.tga"}) {
        const auto sources = sources_for(path);
        check(!sources.empty(), "real Btnchannel state has no PAK source");
        std::cout << "REAL_BUTTON_SOURCE logical=" << path
                  << " actual=" << sources.front().path << '\n';
    }
    check(has_source_suffix("ui/bar/Btnchannel/default.tga", ".TGA")
            && has_source_suffix("ui/bar/Btnchannel/mouseon.tga", ".TGA")
            && has_source_suffix("ui/bar/Btnchannel/pressed.tga", ".TGA")
            && has_source_suffix("ui/bar/Btnchannel/disable.tga", ".TGA"),
        "real Btnchannel states must resolve case-insensitively to uppercase .TGA");
    for (const char* path : {"ui/bar/close/default.tga",
             "ui/chat/switch/default.tga", "ui/chat/PageBtn/up/default.tga"}) {
        check(has_source_suffix(path, ".tga"),
            "lowercase .tga state resource did not resolve: " + std::string(path));
    }

    using Type = dhsh::WindowEvent::Type;
    const auto button_pixels = [&window, button] {
        dhsh::SoftwareRenderer renderer(800, 600);
        window.render(renderer);
        std::vector<std::byte> result;
        const dhsh::LogicalRect bounds = button->bounds();
        const int bottom = std::min(600, bounds.y + bounds.height);
        const int right = std::min(800, bounds.x + bounds.width);
        for (int y = std::max(0, bounds.y); y < bottom; ++y) {
            for (int x = std::max(0, bounds.x); x < right; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * 800 + x) * 4;
                result.insert(result.end(), renderer.target().rgba.begin() + offset,
                    renderer.target().rgba.begin() + offset + 4);
            }
        }
        return result;
    };
    const auto normal = button_pixels();
    window.handle_event(mouse_event(Type::mouse_move, {35, 590}));
    const auto hover = button_pixels();
    window.handle_event(mouse_event(Type::left_button_down, {35, 590}));
    const auto pressed = button_pixels();
    window.handle_event(mouse_event(Type::left_button_up, {35, 590}));
    button->set_enabled(false);
    const auto disabled = button_pixels();
    button->set_enabled(true);
    const auto matches_state = [button](const std::vector<std::byte>& actual,
                                   dhsh::WidgetImageState state) {
        const dhsh::Image* expected = button->image(state);
        const dhsh::LogicalRect bounds = button->bounds();
        const int bottom = std::min(600, bounds.y + bounds.height);
        const int right = std::min(800, bounds.x + bounds.width);
        std::size_t actual_offset = 0;
        for (int y = std::max(0, bounds.y); y < bottom; ++y) {
            for (int x = std::max(0, bounds.x); x < right; ++x, actual_offset += 4) {
                const std::size_t expected_offset =
                    (static_cast<std::size_t>(y - bounds.y) * expected->width
                        + (x - bounds.x)) * 4;
                if (expected->rgba[expected_offset + 3] != std::byte{0}
                    && !std::equal(actual.begin() + actual_offset,
                        actual.begin() + actual_offset + 4,
                        expected->rgba.begin() + expected_offset)) {
                    return false;
                }
            }
        }
        return true;
    };
    std::cout << "REAL_BUTTON_PIXELS hover_changed=" << (normal != hover)
              << " pressed_changed=" << (normal != pressed)
              << " disabled_changed=" << (normal != disabled) << '\n';
    check(matches_state(normal, dhsh::WidgetImageState::normal)
            && matches_state(hover, dhsh::WidgetImageState::hover)
            && matches_state(pressed, dhsh::WidgetImageState::pressed)
            && matches_state(disabled, dhsh::WidgetImageState::disabled),
        "real button did not render the image for its actual interaction state");

    const dhsh::Widget* drop = window.find_widget("bar_txtlst_channel");
    auto* combo = static_cast<dhsh::ComboBox*>(
        window.behavior_for("bar_txtlstchanel"));
    check(drop != nullptr && drop->image() != nullptr && combo != nullptr,
        "real native channel skin or combo is missing");
    const dhsh::Image& skin = *drop->image();
    std::cout << "REAL_CHANNEL_SKIN native=" << skin.width << 'x' << skin.height
              << " rect=" << drop->bounds().x << ',' << drop->bounds().y << ','
              << drop->bounds().width << ',' << drop->bounds().height << '\n';
    const dhsh::LogicalRect expected_area{23, 422, 26, 162};
    check(combo->item_area() == expected_area,
        "real TXTLIST no longer has the original bottom-anchored row area");
    check(combo->open_dropdown(), "real native channel dropdown did not open");
    dhsh::SoftwareRenderer popup(800, 600);
    window.render_popups(popup);
    // CLIENTRECT leaves a four-pixel left margin outside the text rows. That
    // strip is a direct native-image copy and cannot be affected by glyphs.
    const dhsh::LogicalRect skin_bounds = drop->bounds();
    for (std::uint32_t y = 0; y < skin.height; ++y) {
        const int target_y = skin_bounds.y + static_cast<int>(y);
        if (skin_bounds.x < 0 || target_y < 0 || target_y >= 600) {
            continue;
        }
        const std::size_t source_offset = static_cast<std::size_t>(y) * skin.width * 4;
        const std::size_t target_offset =
            (static_cast<std::size_t>(target_y) * 800 + skin_bounds.x) * 4;
        check(std::equal(skin.rgba.begin() + source_offset,
                  skin.rgba.begin() + source_offset + 4,
                  popup.target().rgba.begin() + target_offset),
            "real channel.a16 was scaled instead of drawn at native RECT origin");
    }
    const auto click = [&window](dhsh::LogicalPoint point) {
        window.handle_event(mouse_event(Type::left_button_down, point));
        window.handle_event(mouse_event(Type::left_button_up, point));
    };
    click({30, 511});
    check(combo->selected_index() == std::optional<std::size_t>{4},
        "real world row no longer hits at (30,511)");
    click({35, 590});
    click({30, 430});
    check(combo->selected_index() == std::optional<std::size_t>{0},
        "real current row no longer hits at (30,430)");

    dhsh::ScpWindow chat(dhsh::ScpParser::load(resources, "ui/main/chat.scp"),
        resources, bus);
    const auto bar_declarations = window.message_declarations();
    const auto chat_declarations = chat.message_declarations();
    std::size_t close_codes = 0;
    bool show_hide = false;
    for (const auto* declarations : {&bar_declarations, &chat_declarations}) {
        for (const dhsh::UiMessage& message : *declarations) {
            close_codes += message.control == "bar_close"
                && (message.code == "OnChatClick"
                    || message.code == "OnShowHideChatInfo");
            show_hide = show_hide || (message.control == "chat_choose"
                && message.code == "chat_show" && message.show_hide);
        }
    }
    check(close_codes == 2 && show_hide,
        "real bar_close or chat_choose declarations were not parsed");
    for (const dhsh::ScpWindow* loaded : {&window, &chat}) {
        for (const std::string& diagnostic : loaded->diagnostics()) {
            check(diagnostic.find("Unsupported MESSAGE") == std::string::npos,
                "real chat SHOWHIDE produced a diagnostic: " + diagnostic);
        }
    }
    check(chat.find_widget("chat_show") != nullptr
            && !chat.find_widget("chat_show")->visible(),
        "real chat_show must retain its declared initial HIDE state");
}

void test_real_apg_button_states_match_decoded_frames(const std::string& shdata) {
    dhsh::PakResourceProvider resources(
        shdata, {"800config.pak", "config.pak", "ui.pak"});
    dhsh::MessageBus bus;
    dhsh::ScpWindow window(dhsh::ScpParser::load(resources, "ui/main/daynight.scp"),
        resources, bus);
    constexpr dhsh::WidgetImageState states[] = {
        dhsh::WidgetImageState::normal, dhsh::WidgetImageState::hover,
        dhsh::WidgetImageState::pressed, dhsh::WidgetImageState::disabled,
    };
    struct ApgCase {
        const char* control;
        const char* path;
    };
    for (const ApgCase item : {ApgCase{"home_btnlist_bg_rightpoint", "ui/bar/Btnhome.apg"},
             ApgCase{"bar_worldteam", "ui/bar/Btnteam.apg"}}) {
        std::vector<std::byte> packed;
        try {
            packed = resources.bytes(item.path);
        } catch (const std::exception&) {
            packed = resources.bytes(std::string_view(item.path).substr(3));
        }
        if (packed.size() >= 2 && packed[0] == std::byte{'N'}
            && packed[1] == std::byte{'Z'}) {
            packed = dhsh::NzDecoder::decode(packed);
        }
        const dhsh::ApgAnimation animation = dhsh::ApgDecoder::decode(packed);
        check(animation.frames.size() == 4,
            "real APG button does not contain exactly four frames: " + std::string(item.path));
        dhsh::Widget* button = window.find_widget(item.control);
        check(button != nullptr, "real APG button was not built: " + std::string(item.control));
        std::array<std::size_t, 4> differences{};
        for (std::size_t index = 0; index < 4; ++index) {
            const dhsh::Image& expected = animation.frames[index].image;
            const dhsh::Image* actual = button->image(states[index]);
            check(actual != nullptr && actual->width == expected.width
                    && actual->height == expected.height && actual->rgba == expected.rgba,
                "APG state does not match its independently decoded frame: "
                    + std::string(item.control) + " state=" + std::to_string(index));
            for (std::size_t byte = 0; byte < expected.rgba.size(); ++byte) {
                differences[index] += animation.frames.front().image.rgba[byte]
                    != expected.rgba[byte];
            }
            if (index != 0) {
                check(differences[index] != 0,
                    "real APG test frames are not distinct: " + std::string(item.path));
            }
        }
        std::cout << "REAL_APG_FRAMES control=" << item.control << " path=" << item.path
                  << " size=" << animation.frames.front().image.width << 'x'
                  << animation.frames.front().image.height << " rgba_differences="
                  << differences[0] << ',' << differences[1] << ',' << differences[2]
                  << ',' << differences[3] << '\n';

        const dhsh::LogicalRect bounds = button->bounds();
        const int image_width = static_cast<int>(animation.frames.front().image.width);
        const int image_height = static_cast<int>(animation.frames.front().image.height);
        const auto pixels = [&] {
            dhsh::SoftwareRenderer renderer(800, 600);
            window.render(renderer);
            std::vector<std::byte> result;
            for (int y = std::max(0, bounds.y);
                 y < std::min(600, bounds.y + image_height); ++y) {
                for (int x = std::max(0, bounds.x);
                     x < std::min(800, bounds.x + image_width); ++x) {
                    const std::size_t offset =
                        (static_cast<std::size_t>(y) * 800 + x) * 4;
                    result.insert(result.end(), renderer.target().rgba.begin() + offset,
                        renderer.target().rgba.begin() + offset + 4);
                }
            }
            return result;
        };
        const auto matches_frame = [&](const std::vector<std::byte>& actual,
                                       std::size_t frame) {
            const dhsh::Image& expected = animation.frames[frame].image;
            std::size_t opaque = 0;
            std::size_t actual_offset = 0;
            for (int y = std::max(0, bounds.y);
                 y < std::min(600, bounds.y + image_height); ++y) {
                for (int x = std::max(0, bounds.x);
                     x < std::min(800, bounds.x + image_width); ++x, actual_offset += 4) {
                    const std::size_t expected_offset =
                        (static_cast<std::size_t>(y - bounds.y) * expected.width
                            + (x - bounds.x)) * 4;
                    if (expected.rgba[expected_offset + 3] == std::byte{0xff}) {
                        ++opaque;
                        if (!std::equal(actual.begin() + actual_offset,
                                actual.begin() + actual_offset + 4,
                                expected.rgba.begin() + expected_offset)) {
                            return false;
                        }
                    }
                }
            }
            return opaque != 0;
        };
        using Type = dhsh::WindowEvent::Type;
        window.handle_event(mouse_event(Type::mouse_move, {0, 0}));
        const auto normal = pixels();
        window.handle_event(mouse_event(Type::mouse_move,
            {bounds.x + bounds.width / 2, bounds.y + bounds.height / 2}));
        const auto hover = pixels();
        window.handle_event(mouse_event(Type::left_button_down,
            {bounds.x + bounds.width / 2, bounds.y + bounds.height / 2}));
        const auto pressed = pixels();
        window.handle_event(mouse_event(Type::left_button_up,
            {bounds.x + bounds.width / 2, bounds.y + bounds.height / 2}));
        button->set_enabled(false);
        const auto disabled = pixels();
        const std::array<std::vector<std::byte>, 4> rendered{
            normal, hover, pressed, disabled};
        for (std::size_t index = 0; index < rendered.size(); ++index) {
            check(matches_frame(rendered[index], index),
                "real APG interaction state rendered the wrong frame: "
                    + std::string(item.control) + " state=" + std::to_string(index));
            if (index != 0) {
                check(rendered[index] != normal,
                    "real APG interaction pixels did not change: "
                        + std::string(item.control) + " state=" + std::to_string(index));
            }
        }
    }
}

// The real App loads these windows in this order, renders them forwards and
// dispatches events in reverse. Keeping that topology is essential: the old
// isolated bar fixture used a 1x1 button image and could not expose the real
// channel button skin covering the selected EDIT label.
struct RealAppChatFixture {
    explicit RealAppChatFixture(const std::string& shdata)
        : resources(shdata, {"800config.pak", "config.pak", "ui.pak"}) {
        const auto load_window = [this](std::string_view path) {
            ++load_counts[std::string(path)];
            auto window = std::make_unique<dhsh::ScpWindow>(
                dhsh::ScpParser::load(resources, std::string(path)), resources, bus);
            window->set_chat_model(&model);
            return window;
        };
        constexpr std::string_view paths[] = {
            "ui/main/headwnd.scp",
            "ui/main/minimap.scp",
            "ui/main/smallmap.scp",
            "ui/main/bar.scp",
            "ui/main/chat.scp",
            "ui/main/daynight.scp",
            "ui/main/fucnbtn_shortcut.scp",
        };
        for (const std::string_view path : paths) {
            auto window = load_window(path);
            if (path == "ui/main/bar.scp") {
                bar = window.get();
                main_chat_bar.emplace(*window, bus, sink);
            }
            windows.push_back(std::move(window));
        }
        check(bar != nullptr, "real App bar.scp did not load");
        main_panel_host.emplace(windows, bus, resources, load_window);
    }

    void render(dhsh::SoftwareRenderer& renderer) {
        for (auto& window : windows) {
            window->render(renderer);
        }
        for (auto& window : windows) {
            window->render_popups(renderer);
        }
    }

    bool handle(const dhsh::WindowEvent& event) {
        return dhsh::handle_scp_event(windows, event);
    }

    dhsh::PakResourceProvider resources;
    dhsh::ChatModel model;
    dhsh::MessageBus bus;
    RecordingSendSink sink;
    dhsh::ScpWindows windows;
    std::optional<dhsh::MainChatBar> main_chat_bar;
    std::optional<dhsh::MainPanelHost> main_panel_host;
    std::unordered_map<std::string, std::size_t> load_counts;
    dhsh::ScpWindow* bar = nullptr;
};

std::vector<std::byte> real_app_label_band(const dhsh::Image& image) {
    std::vector<std::byte> band;
    for (int y = 585; y < 600; ++y) {
        for (int x = 23; x < 55; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * 800 + x) * 4;
            band.insert(band.end(), image.rgba.begin() + offset,
                image.rgba.begin() + offset + 4);
        }
    }
    return band;
}

void test_real_app_main_bar_initial_current_is_visible(const std::string& shdata) {
    RealAppChatFixture app(shdata);
    auto* combo = static_cast<dhsh::ComboBox*>(
        app.bar->behavior_for("bar_txtlstchanel"));
    const dhsh::Widget* field = app.bar->find_widget("bar_editchanel");
    check(combo != nullptr && field != nullptr && field->visible(),
        "real App channel EDIT fixedpart is not visible");
    check(combo->selected_index() == std::optional<std::size_t>{0},
        "real App channel combo does not start on 当前");

    dhsh::SoftwareRenderer current(800, 600);
    app.render(current);
    combo->select(4);
    dhsh::SoftwareRenderer world(800, 600);
    app.render(world);
    check(real_app_label_band(current.target()) != real_app_label_band(world.target()),
        "real App channel skin covers the initial 当前 EDIT label");
}

void test_real_app_main_bar_full_window_input_order(const std::string& shdata) {
    RealAppChatFixture app(shdata);
    auto* edit = static_cast<dhsh::RichEdit*>(app.bar->behavior_for("input_mes"));
    auto* combo = static_cast<dhsh::ComboBox*>(
        app.bar->behavior_for("bar_txtlstchanel"));
    check(edit != nullptr && combo != nullptr, "real App chat behaviors are missing");
    using Type = dhsh::WindowEvent::Type;
    const auto click = [&app](dhsh::LogicalPoint point) {
        app.handle(mouse_event(Type::left_button_down, point));
        app.handle(mouse_event(Type::left_button_up, point));
    };

    click({100, 590});
    check(app.bar->focused_widget() == app.bar->find_widget("input_mes"),
        "full App SCP window order prevented input_mes from receiving focus");
    dhsh::SoftwareRenderer focused_empty(800, 600);
    app.render(focused_empty);
    app.handle(text_event("a"));
    check(edit->text() == "a", "real App input_mes did not accept text");
    dhsh::SoftwareRenderer typed(800, 600);
    app.render(typed);
    check(focused_empty.target().rgba != typed.target().rgba,
        "real App black input text did not change any rendered pixel");

    click({35, 590});
    check(combo->dropdown_visible(), "real App channel popup did not open");
    dhsh::SoftwareRenderer popup(800, 600);
    app.render(popup);
    const dhsh::Widget* drop = app.bar->find_widget("bar_txtlst_channel");
    check(drop != nullptr && drop->image() != nullptr,
        "real App channel popup has no channel.a16 skin");
    int white_pixels = 0;
    int yellow_pixels = 0;
    const dhsh::LogicalRect area = combo->item_area();
    for (int y = area.y; y < area.y + area.height; ++y) {
        for (int x = area.x; x < area.x + area.width; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * 800 + x) * 4;
            const int red = std::to_integer<unsigned char>(popup.target().rgba[offset]);
            const int green = std::to_integer<unsigned char>(popup.target().rgba[offset + 1]);
            const int blue = std::to_integer<unsigned char>(popup.target().rgba[offset + 2]);
            white_pixels += red > 180 && green > 180 && blue > 180;
            yellow_pixels += red > 120 && green > 120 && blue < 100;
        }
    }
    std::cout << "REAL_APP_POPUP area=" << area.x << ',' << area.y << ','
              << area.width << ',' << area.height << " white=" << white_pixels
              << " yellow=" << yellow_pixels << '\n';
    check(white_pixels > 0 && yellow_pixels == 0,
        "real App channel popup text is not white or still uses yellow");

    click({30, 511});
    check(combo->selected_index() == std::optional<std::size_t>{4}
            && edit->text() == "a",
        "real App 世界 selection did not preserve the unsent edit");
    app.main_chat_bar->note_key_down(VK_RETURN);
    app.handle(key_event(VK_RETURN));
    check(app.sink.sends.size() == 1 && app.sink.sends[0].first == 5
            && app.sink.sends[0].second == "a" && edit->text().empty(),
        "real App direct Enter after 世界 did not send once and clear");

    app.handle(text_event("c"));
    click({35, 590});
    click({30, 430});
    app.main_chat_bar->note_key_down(VK_RETURN);
    app.handle(key_event(VK_RETURN));
    check(app.sink.sends.size() == 2 && app.sink.sends[1].first == 1
            && app.sink.sends[1].second == "c" && edit->text().empty(),
        "real App direct Enter after 当前 did not send once and clear");
    app.main_chat_bar->note_key_down(VK_RETURN);
    app.handle(key_event(VK_RETURN));
    check(app.sink.sends.size() == 2, "real App empty Enter sent a duplicate");
}

void test_real_app_shift_arrow_full_window_message_order(const std::string& shdata) {
    RealAppChatFixture app(shdata);
    dhsh::InputState input;
    using Type = dhsh::WindowEvent::Type;
    const auto dispatch = [&app, &input](const dhsh::WindowEvent& event) {
        const bool always_clear_input = event.type == Type::key_up
            || event.type == Type::input_cancelled;
        if (always_clear_input) {
            input.handle_event(event);
        }
        if (event.type == Type::key_down) {
            app.main_chat_bar->note_key_down(event.key);
        }
        const bool ui_consumed = app.handle(event);
        if (!always_clear_input && !ui_consumed) {
            input.handle_event(event);
        }
    };
    const auto release = [&dispatch](std::uintptr_t key) {
        dhsh::WindowEvent event = key_event(key);
        event.type = Type::key_up;
        dispatch(event);
    };

    app.handle(mouse_event(Type::left_button_down, {700, 590}));
    app.handle(mouse_event(Type::left_button_up, {700, 590}));
    for (const std::uintptr_t key : {std::uintptr_t{'A'}, std::uintptr_t{'D'},
             std::uintptr_t{VK_RIGHT}, std::uintptr_t{VK_DOWN}}) {
        dispatch(key_event(key));
        check(input.horizontal() == 0.0 && input.vertical() == 0.0,
            "real App allowed an unmodified WASD/arrow key to pan");
        release(key);
    }

    dispatch(key_event(VK_SHIFT));
    dispatch(key_event(std::uintptr_t{'D'}));
    check(input.horizontal() == 0.0, "real App allowed Shift+D to pan");
    release(std::uintptr_t{'D'});
    dispatch(key_event(VK_RIGHT));
    check(input.horizontal() == 1.0,
        "real App seven-window dispatch consumed Shift+Right before InputState");
    release(VK_RIGHT);
    check(input.horizontal() == 0.0, "real App arrow release did not stop panning");

    dispatch(key_event(VK_RIGHT));
    check(input.horizontal() == 1.0, "real App did not resume Shift+Right");
    release(VK_SHIFT);
    check(input.horizontal() == 0.0, "real App Shift release did not stop panning");
    release(VK_RIGHT);

    dispatch(key_event(VK_SHIFT));
    dispatch(key_event(VK_RIGHT));
    dhsh::WindowEvent cancelled;
    cancelled.type = Type::input_cancelled;
    dispatch(cancelled);
    check(input.horizontal() == 0.0, "real App input cancellation did not clear panning");
    release(VK_RIGHT);
    release(VK_SHIFT);
}

const dhsh::Widget* real_app_widget(
    const RealAppChatFixture& app, std::string_view name) {
    for (const auto& window : app.windows) {
        if (const dhsh::Widget* widget = window->find_widget(name)) {
            return widget;
        }
    }
    return nullptr;
}

void real_app_click(RealAppChatFixture& app, std::string_view name) {
    const dhsh::Widget* widget = real_app_widget(app, name);
    check(widget != nullptr && widget->visible(),
        "real App click target is missing or hidden: " + std::string(name));
    const dhsh::LogicalRect bounds = widget->bounds();
    const dhsh::LogicalPoint center{bounds.x + bounds.width / 2,
        bounds.y + bounds.height / 2};
    app.handle(mouse_event(dhsh::WindowEvent::Type::left_button_down, center));
    app.handle(mouse_event(dhsh::WindowEvent::Type::left_button_up, center));
}

void real_app_expect_toggle(RealAppChatFixture& app,
    std::string_view button, std::string_view panel) {
    const dhsh::Widget* target = real_app_widget(app, panel);
    const bool before = target != nullptr && target->visible();
    real_app_click(app, button);
    target = real_app_widget(app, panel);
    check(target != nullptr && target->visible() != before,
        "real App button did not toggle its native panel: " + std::string(button)
            + " -> " + std::string(panel));
    real_app_click(app, button);
    target = real_app_widget(app, panel);
    check(target != nullptr && target->visible() == before,
        "real App button did not restore its native panel: " + std::string(button)
            + " -> " + std::string(panel));
}

// Matches App::load_scp_window's profiling lifetime exactly. MainPanelHost's
// initial function-arrow synchronization reveals the previously hidden arrow,
// which loads its APG after the loader's stack-local ScpWindowProfile died.
// Keeping a profiling output pointer in ScpWindow makes this crash in Release.
void test_real_app_profiled_shortcut_arrow_initialization(const std::string& shdata) {
    dhsh::PakResourceProvider resources(
        shdata, {"800config.pak", "config.pak", "ui.pak", "map.pak"});
    dhsh::MessageBus bus;
    dhsh::ScpWindows windows;
    const auto load_window = [&](std::string_view path) {
        dhsh::ScpLoadDiagnostics load_profile;
        dhsh::ScpDocument document = dhsh::ScpParser::load(
            resources, std::string(path), {}, &load_profile);
        dhsh::ScpWindowProfile window_profile;
        return std::make_unique<dhsh::ScpWindow>(std::move(document), resources, bus,
            dhsh::ScpWindow::ImageDecoder{}, dhsh::ScpWindowOptions{}, &window_profile);
    };
    windows.push_back(load_window("ui/main/bar.scp"));
    windows.push_back(load_window("ui/main/fucnbtn_shortcut.scp"));
    dhsh::MainPanelHost host(windows, bus, resources, load_window);
    const dhsh::Widget* hide = windows.front()->find_widget("bar_funcbtn_hide");
    check(hide != nullptr && hide->visible() && hide->image() != nullptr,
        "profiled App arrow initialization did not load the real deferred APG");
}

void test_real_app_main_chat_buttons_toggle_native_panels(const std::string& shdata) {
    RealAppChatFixture app(shdata);
    real_app_expect_toggle(app, "btn_revert", "chat_revert");
    real_app_expect_toggle(app, "chat_font_choose_btn", "chat_font_choose");
    real_app_expect_toggle(app, "bar_smile", "chat_face");
    real_app_click(app, "bar_smile");
    const dhsh::Widget* face = real_app_widget(app, "chat_face");
    check(face != nullptr && face->visible()
            && face->bounds() == dhsh::LogicalRect{472, 238, 328, 328}
            && face->image() != nullptr,
        "emote.csv frame did not apply the native 800px offset and smilebg.a16");
    real_app_click(app, "bar_smile");
    real_app_expect_toggle(app, "bar_body_action", "body");
}

void test_real_app_daynight_buttons_toggle_native_panels(const std::string& shdata) {
    RealAppChatFixture app(shdata);
    real_app_expect_toggle(app, "home_btnlist_bg_rightpoint", "home_btnlist");
    real_app_expect_toggle(app, "bar_worldteam", "team_worldteam");
    real_app_expect_toggle(app, "bar_worldmap", "sys_smallmap_win");
    real_app_expect_toggle(app, "newhelp_btn", "newhelp");
}

dhsh::ScpWindow* real_app_window_for(
    RealAppChatFixture& app, std::string_view widget_name) {
    for (const auto& window : app.windows) {
        if (window->find_widget(widget_name) != nullptr) {
            return window.get();
        }
    }
    return nullptr;
}

void real_app_expect_two_internal_close_cycles(RealAppChatFixture& app,
    std::string_view opener, std::string_view closer, std::string_view panel,
    std::string_view source) {
    const std::size_t initial_windows = app.windows.size();
    const std::size_t initial_loads = app.load_counts[std::string(source)];
    const dhsh::ScpWindow* reused_window = nullptr;
    const dhsh::Widget* reused_panel = nullptr;
    for (int round = 0; round < 2; ++round) {
        real_app_click(app, opener);
        const dhsh::Widget* target = real_app_widget(app, panel);
        dhsh::ScpWindow* owner = real_app_window_for(app, panel);
        check(target != nullptr && target->visible() && owner != nullptr,
            "native panel did not open before internal close: " + std::string(panel));
        check(app.load_counts[std::string(source)] == initial_loads + (initial_loads == 0 ? 1 : 0)
                && app.windows.size() == initial_windows + (initial_loads == 0 ? 1 : 0),
            "native panel loaded more than once: " + std::string(panel));
        if (round == 0) {
            reused_window = owner;
            reused_panel = target;
        } else {
            check(owner == reused_window && target == reused_panel,
                "native panel was recreated instead of reused: " + std::string(panel));
        }
        real_app_click(app, closer);
        target = real_app_widget(app, panel);
        check(target == reused_panel && real_app_window_for(app, panel) == reused_window
                && !target->visible(),
            "native internal close did not hide the reused panel: " + std::string(panel)
                + " round=" + std::to_string(round + 1));
    }
    std::cout << "REAL_PANEL_CLOSE panel=" << panel << " rounds=2 reused_window="
              << reused_window << " reused_widget=" << reused_panel << " loads="
              << app.load_counts[std::string(source)] << '\n';
}

void test_real_app_internal_close_buttons_hide_and_reopen_panels(const std::string& shdata) {
    RealAppChatFixture app(shdata);
    real_app_expect_two_internal_close_cycles(app, "bar_worldteam", "team_worldteam_close",
        "team_worldteam", "ui/team/team_worldteam.scp");
    real_app_expect_two_internal_close_cycles(app, "newhelp_btn", "newhelp_close",
        "newhelp", "ui/help/newhelp.scp");

    constexpr std::string_view minimap_source = "ui/main/minimap.scp";
    constexpr std::string_view smallmap_source = "ui/main/smallmap.scp";
    const std::size_t initial_windows = app.windows.size();
    const std::size_t minimap_loads = app.load_counts[std::string(minimap_source)];
    const std::size_t smallmap_loads = app.load_counts[std::string(smallmap_source)];
    dhsh::ScpWindow* const minimap_window = real_app_window_for(app, "sys_map");
    dhsh::ScpWindow* const smallmap_window = real_app_window_for(app, "sys_smallmap_win");
    const dhsh::Widget* const minimap = real_app_widget(app, "sys_map");
    const dhsh::Widget* const smallmap = real_app_widget(app, "sys_smallmap_win");
    check(minimap_window != nullptr && minimap != nullptr && smallmap_window != nullptr
            && smallmap != nullptr && minimap_loads == 1 && smallmap_loads == 1,
        "real minimap/smallmap fixture did not preload both native panels");
    const bool smallmap_visible = smallmap->visible();
    if (minimap->visible()) {
        real_app_click(app, "daynight_minimapbtn");
        check(!minimap->visible(),
            "minimap entry did not hide the initially visible sys_map");
    }
    for (int round = 0; round < 2; ++round) {
        real_app_click(app, round == 0 ? "daynight_minimapbtn" : "daynight_mapname");
        check(minimap->visible() && real_app_window_for(app, "sys_map") == minimap_window,
            "minimap entry did not show the preloaded sys_map in round "
                + std::to_string(round + 1));
        real_app_click(app, "minimap_close");
        check(!minimap->visible() && real_app_widget(app, "sys_map") == minimap
                && real_app_window_for(app, "sys_map") == minimap_window
                && app.load_counts[std::string(minimap_source)] == minimap_loads
                && app.windows.size() == initial_windows,
            "minimap close did not hide and reuse sys_map in round "
                + std::to_string(round + 1));
        check(real_app_widget(app, "sys_smallmap_win") == smallmap
                && real_app_window_for(app, "sys_smallmap_win") == smallmap_window
                && smallmap->visible() == smallmap_visible
                && app.load_counts[std::string(smallmap_source)] == smallmap_loads,
            "minimap interaction incorrectly toggled sys_smallmap_win");
    }
    std::cout << "REAL_PANEL_CLOSE panel=sys_map rounds=2 reused_window=" << minimap_window
              << " reused_widget=" << minimap << " loads=" << minimap_loads
              << " smallmap_loads=" << smallmap_loads
              << " smallmap_visible=" << smallmap_visible << '\n';
}

bool real_app_has_region(
    const RealAppChatFixture& app, const dhsh::LogicalRect& bounds) {
    const auto regions = dhsh::scp_input_regions(app.windows);
    return std::find(regions.begin(), regions.end(), bounds) != regions.end();
}

void test_real_app_shortcut_arrows_toggle_bars_and_input(const std::string& shdata) {
    RealAppChatFixture app(shdata);
    real_app_click(app, "bar_barbtn_show");
    const dhsh::Widget* skills = real_app_widget(app, "skill_shortcut");
    check(skills != nullptr && skills->visible()
            && !real_app_widget(app, "bar_barbtn_show")->visible()
            && real_app_widget(app, "bar_barbtn_hide")->visible()
            && real_app_has_region(app, skills->bounds()),
        "skill arrow did not show the native bar, switch arrows and add MapInput region");
    real_app_click(app, "bar_barbtn_hide");
    check(!skills->visible()
            && real_app_widget(app, "bar_barbtn_show")->visible()
            && !real_app_widget(app, "bar_barbtn_hide")->visible()
            && !real_app_has_region(app, skills->bounds()),
        "skill hide arrow did not hide the bar and remove its MapInput region");

    const dhsh::Widget* functions = real_app_widget(app, "funcbtn_shortcut");
    check(functions != nullptr, "real funcbtn_shortcut is missing");
    const bool initially_visible = functions->visible();
    real_app_click(app, initially_visible ? "bar_funcbtn_hide" : "bar_funcbtn_show");
    check(functions->visible() != initially_visible
            && real_app_has_region(app, functions->bounds()) == functions->visible()
            && real_app_widget(app, "bar_funcbtn_show")->visible() != functions->visible()
            && real_app_widget(app, "bar_funcbtn_hide")->visible() == functions->visible(),
        "function arrow did not synchronize native bar, arrows and MapInput");
}

// Real-resource coverage for the Task 8 interaction paths: the shipped
// chatmain.scp (barwnd HIDE=1) must load its input-row images when the
// attached mode shows them, and rows scrolled past the first page must
// render inside the clip after a hide/show cycle. Render-layer pixels are
// asserted (no display-session dependency).
void test_real_chatmain_attach_and_reopen_render(const std::string& shdata) {
    dhsh::PakResourceProvider resources(shdata, {"config.pak", "ui.pak"});
    dhsh::ChatModel model;
    RecordingSendSink sink;
    for (int index = 0; index < 60; ++index) {
        dhsh::ChatMessage message;
        message.channel = 5;
        message.spans = {{dhsh::ChatSpanKind::text,
            std::string(60, static_cast<char>('a' + index % 26)), "red", {}}};
        model.append(std::move(message));
    }
    dhsh::ChatWindowHost host([&]() {
        return std::make_unique<dhsh::ScpChatWindow>(resources,
            "ui/main/chatmain.scp", model, sink, 60, 60, 549, 590);
    });
    host.toggle();
    check(host.has_window(), "real chat window was not created");

    // Attached mode reveals the input row: the real chatinputbg.a16 must
    // decode and render (hidden-at-build images load on first show).
    host.window()->set_attached(true);
    const dhsh::Widget* bar = host.window()->ui()->find_widget("chatmain_barwnd");
    check(bar != nullptr && bar->visible() && bar->image() != nullptr,
        "real chatinputbg.a16 did not load when the input row was shown");

    // Reopen path: every detached presentation starts without the attached
    // input row, even if the previous host lifetime used attached mode.
    host.toggle();
    host.toggle();
    check(!host.window()->attached() && !bar->visible(),
        "real detached chat reopen leaked the attached input row");
    auto* view = static_cast<dhsh::ListViewEx*>(
        host.window()->ui()->behavior_for("chatmain_txtlst_msg"));
    const int bottom = static_cast<int>(
        view->rows().size() - view->visible_row_capacity());
    view->set_scroll_value(view->scroll_range().min + std::max(0, bottom));
    dhsh::SoftwareRenderer renderer(549, 590);
    check(host.window()->ui()->render(renderer), "real chat UI did not render");
    const dhsh::Image& target = renderer.target();
    int list_band = 0;
    for (int y = 23; y < 460; ++y) {
        for (int x = 1; x < 546; ++x) {
            if (target.rgba[(static_cast<std::size_t>(y) * 549 + x) * 4 + 3]
                    != std::byte{0}) {
                ++list_band;
            }
        }
    }
    check(list_band > 0,
        "real chatmain rows scrolled past the first page must render after reopen");
    std::cout << "REAL_CHAT_RENDER PASS list_band=" << list_band
              << " input_visible=" << bar->visible()
              << " attached=" << host.window()->attached() << '\n';
}

void test_txtlist_font_metrics_native_skin_and_hit_geometry() {
    const std::string source = R"SCP(WINDOW TYPE=WIN
 NAME = "root"
 RECT = 0,0,100,100
 WINDOW TYPE=COMBOBOX
  NAME = "combo"
  RECT = 0,0,20,20
  FIXEDPART TYPE=TXTLIST
   NAME = "list"
   RECT = 0,0,20,0
   BMP = "ui/test/native.a16"
   CLIENTRECT = 1,2,18,40
   ADDTEXT = "first"
   ADDTEXT = "second"
  END
 END
END
)SCP";
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window(dhsh::ScpParser::parse(::bytes(source), "native.scp"),
        resources, bus, [](std::string_view, std::span<const std::byte>) {
            dhsh::Image image;
            image.width = 20;
            image.height = 30;
            image.rgba.resize(20U * 30U * 4U, std::byte{0xff});
            return image;
        });
    auto* combo = static_cast<dhsh::ComboBox*>(window.behavior_for("combo"));
    check(combo != nullptr && combo->open_dropdown(), "native-skin combo did not open");

    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = dhsh::create_default_ui_font();
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICW metrics{};
    check(GetTextMetricsW(dc, &metrics) != FALSE, "default UI font metrics unavailable");
    SelectObject(dc, old);
    DeleteObject(font);
    DeleteDC(dc);
    // Generic single-line row chrome adds two logical pixels above and below
    // the shared font's actual line height; no per-channel or widget constant.
    const int row_height = metrics.tmHeight + metrics.tmExternalLeading + 4;
    const dhsh::LogicalRect expected{1, 42 - 2 * row_height, 18, 2 * row_height};
    const dhsh::LogicalRect actual = combo->item_area();
    check(actual == expected,
        "TXTLIST row geometry does not derive from shared GDI font metrics: actual="
            + std::to_string(actual.x) + "," + std::to_string(actual.y) + ","
            + std::to_string(actual.width) + "," + std::to_string(actual.height)
            + " expected=" + std::to_string(expected.x) + "," + std::to_string(expected.y)
            + "," + std::to_string(expected.width) + "," + std::to_string(expected.height)
            + " font=" + std::to_string(metrics.tmHeight) + "+"
            + std::to_string(metrics.tmExternalLeading));
    check(combo->item_at({2, expected.y + 1}) == std::optional<std::size_t>{0}
            && combo->item_at({2, expected.y + row_height + 1})
                == std::optional<std::size_t>{1},
        "font-derived TXTLIST geometry and hit testing disagree");

    dhsh::SoftwareRenderer renderer(100, 100);
    window.render_popups(renderer);
    // The native skin starts at its RECT origin. The former whole-image
    // vertical scaling shifted it down to fit the text area and left y=0 empty.
    const std::size_t offset = 0;
    check(renderer.target().rgba[offset + 3] == std::byte{0xff},
        "TXTLIST BMP was scaled/shifted instead of drawn at native RECT size");
}

void test_generic_button_visual_states_and_cross_window_hover() {
    const std::string source = R"SCP(WINDOW TYPE=WIN
 NAME = "root"
 RECT = 0,0,20,20
 WINDOW TYPE=BUTTON
  NAME = "button"
  RECT = 0,0,20,20
  BMPPATH = "ui/test/state"
 END
END
)SCP";
    MapResources resources;
    for (const char* state : {"default.tga", "mouseon.tga", "pressed.tga", "disable.tga"}) {
        resources.add(std::string("ui/test/state/") + state, "x");
    }
    const auto decoder = [](std::string_view path, std::span<const std::byte>) {
        dhsh::Image image;
        image.width = image.height = 20;
        const bool disabled = path.ends_with("disable.tga");
        const std::byte red = path.ends_with("default.tga") || disabled ? std::byte{0xff} : std::byte{0};
        const std::byte green = path.ends_with("mouseon.tga") ? std::byte{0xff} : std::byte{0};
        const std::byte blue = path.ends_with("pressed.tga") || disabled ? std::byte{0xff} : std::byte{0};
        const std::byte alpha = std::byte{0xff};
        image.rgba.resize(20U * 20U * 4U);
        for (std::size_t offset = 0; offset < image.rgba.size(); offset += 4) {
            image.rgba[offset] = red;
            image.rgba[offset + 1] = green;
            image.rgba[offset + 2] = blue;
            image.rgba[offset + 3] = alpha;
        }
        return image;
    };
    dhsh::MessageBus bus;
    dhsh::ScpWindows windows;
    for (int index = 0; index < 2; ++index) {
        auto document = dhsh::ScpParser::parse(::bytes(source), "state.scp");
        for (auto& [key, value] : document.windows.front().properties) {
            if (key == "RECT") {
                value = std::to_string(index * 20) + ",0,20,20";
            }
        }
        windows.push_back(std::make_unique<dhsh::ScpWindow>(
            std::move(document), resources, bus, decoder));
    }
    const auto pixel = [&windows](int x) {
        dhsh::SoftwareRenderer renderer(40, 20);
        for (auto& window : windows) {
            window->render(renderer);
        }
        const std::size_t offset = (5U * 40U + static_cast<std::size_t>(x)) * 4U;
        return std::array<std::byte, 4>{renderer.target().rgba[offset],
            renderer.target().rgba[offset + 1], renderer.target().rgba[offset + 2],
            renderer.target().rgba[offset + 3]};
    };
    const auto move = [&windows](int x) {
        dhsh::handle_scp_event(windows,
            mouse_event(dhsh::WindowEvent::Type::mouse_move, {x, 5}));
    };
    check(pixel(5) == std::array<std::byte, 4>{std::byte{0xff}, std::byte{0}, std::byte{0}, std::byte{0xff}},
        "generic button did not load and render its normal state");
    move(5);
    check(pixel(5) == std::array<std::byte, 4>{std::byte{0}, std::byte{0xff}, std::byte{0}, std::byte{0xff}},
        "generic button did not render its mouseon state");
    move(25);
    check(pixel(5) == std::array<std::byte, 4>{std::byte{0xff}, std::byte{0}, std::byte{0}, std::byte{0xff}}
            && pixel(25) == std::array<std::byte, 4>{std::byte{0}, std::byte{0xff}, std::byte{0}, std::byte{0xff}},
        "moving to another SCP window left the old button hovered");
    dhsh::handle_scp_event(windows,
        mouse_event(dhsh::WindowEvent::Type::left_button_down, {5, 5}));
    check(pixel(5) == std::array<std::byte, 4>{std::byte{0}, std::byte{0}, std::byte{0xff}, std::byte{0xff}},
        "generic button did not render its pressed state");
    dhsh::handle_scp_event(windows,
        mouse_event(dhsh::WindowEvent::Type::left_button_up, {5, 5}));
    windows[0]->find_widget("button")->set_enabled(false);
    check(pixel(5) == std::array<std::byte, 4>{std::byte{0xff}, std::byte{0}, std::byte{0xff}, std::byte{0xff}},
        "disabled did not take priority over the generic button hover state");
}

void test_showhide_toggles_target_without_revealing_hidden_descendants() {
    const std::string source = R"SCP(WINDOW TYPE=WIN
 NAME = "root"
 RECT = 0,0,100,30
 WINDOW TYPE=BUTTON
  NAME = "chat_choose"
  RECT = 0,0,20,20
  MESSAGE = FSM_BTN_MSG 1 SHOWHIDE "chat_show"
 END
 WINDOW TYPE=WIN
  NAME = "chat_show"
  RECT = 30,0,30,20
  HIDE = 1
  WINDOW TYPE=BUTTON
   NAME = "declared_hidden_child"
   RECT = 0,0,10,10
   HIDE = 1
  END
 END
END
)SCP";
    MapResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window(dhsh::ScpParser::parse(::bytes(source), "showhide.scp"),
        resources, bus);
    std::size_t bus_messages = 0;
    bus.subscribe("chat_choose", "FSM_BTN_MSG",
        [&bus_messages](const dhsh::UiMessage&) { ++bus_messages; });
    const auto click_choose = [&window] {
        window.handle_event(mouse_event(
            dhsh::WindowEvent::Type::left_button_down, {5, 5}));
        window.handle_event(mouse_event(
            dhsh::WindowEvent::Type::left_button_up, {5, 5}));
    };
    check(!window.find_widget("chat_show")->visible()
            && !window.find_widget("declared_hidden_child")->visible(),
        "SHOWHIDE fixture must start with its target subtree hidden");
    click_choose();
    check(window.find_widget("chat_show")->visible(),
        "SHOWHIDE button click did not reveal its target");
    check(!window.find_widget("declared_hidden_child")->visible(),
        "SHOWHIDE revealed a descendant whose own HIDE state must be preserved");
    check(bus_messages == 0,
        "SHOWHIDE is local UI visibility, not a MessageBus CODE dispatch");
    click_choose();
    check(!window.find_widget("chat_show")->visible()
            && !window.find_widget("declared_hidden_child")->visible(),
        "a second SHOWHIDE click must hide the target again");
    const auto declarations = window.message_declarations();
    check(declarations.size() == 1 && declarations.front().control == "chat_choose"
            && declarations.front().code == "chat_show",
        "SHOWHIDE target is missing from message_declarations");
    check(window.diagnostics().empty(),
        "valid SHOWHIDE produced an Unsupported MESSAGE diagnostic");
}

void test_chat_host_restores_embedded_chat_from_real_buttons() {
    const std::string source = R"SCP(WINDOW TYPE=WIN
 NAME = "root"
 RECT = 0,0,200,100
 WINDOW TYPE=BUTTON
  NAME = "bar_close"
  RECT = 0,0,20,20
  MESSAGE = FSM_BTN_MSG 1 CODE "OnChatClick"
  MESSAGE = FSM_BTN_MSG 1 CODE "OnShowHideChatInfo"
 END
 WINDOW TYPE=WIN
  NAME = "chat"
  RECT = 30,0,100,60
  WINDOW TYPE=BUTTON
   NAME = "embedded_child"
   RECT = 0,0,20,20
  END
 END
END
)SCP";
    MapResources embedded_resources;
    dhsh::MessageBus embedded_messages;
    dhsh::ScpWindow embedded(
        dhsh::ScpParser::parse(::bytes(source), "embedded_chat.scp"),
        embedded_resources, embedded_messages);
    std::size_t close_messages = 0;
    embedded_messages.subscribe("bar_close", "FSM_BTN_MSG",
        [&close_messages](const dhsh::UiMessage&) { ++close_messages; });

    MapResources resources;
    dhsh::ChatModel model;
    RecordingSendSink sink;
    dhsh::ChatWindowHost host([&]() {
        return make_chat_window(resources, model, sink, 60, 60, 549, 590);
    });
    host.bind_main_chat(embedded, embedded_messages);
    const auto embedded_visible = [&embedded] {
        return embedded.find_widget("chat")->visible()
            && embedded.find_widget("embedded_child")->visible();
    };
    check(embedded_visible(), "embedded chat fixture must start visible");

    host.toggle();
    check(host.visible() && !embedded_visible(),
        "opening the detached chat must hide the embedded chat subtree");
    embedded.handle_event(mouse_event(
        dhsh::WindowEvent::Type::left_button_down, {5, 5}));
    embedded.handle_event(mouse_event(
        dhsh::WindowEvent::Type::left_button_up, {5, 5}));
    check(close_messages == 2,
        "the real bar_close click must dispatch both declared CODE messages");
    check(!host.visible() && embedded_visible(),
        "two bar_close CODE messages must idempotently restore embedded chat");

    for (int round = 0; round < 2; ++round) {
        host.toggle();
        check(host.visible() && !embedded_visible(),
            "reopening the detached chat must hide embedded chat again");
        check(!host.window()->attached(),
            "each detached open must start without the attached input row");
        const dhsh::LogicalRect switch_bounds =
            host.window()->ui()->find_widget("chatmain_switch")->bounds();
        click_client(host.window()->handle(), switch_bounds.x + 2, switch_bounds.y + 2);
        check(!host.visible() && embedded_visible(),
            "chatmain_switch must close the detached window and restore embedded chat");
        check(!host.window()->attached()
                && !host.window()->ui()->find_widget("chatmain_barwnd")->visible(),
            "chatmain_switch must reset attached input state before the next open");
        host.toggle();
        check(host.visible() && !host.window()->attached()
                && !host.window()->ui()->find_widget("chatmain_barwnd")->visible(),
            "reopening after chatmain_switch leaked an attached input row");
        host.toggle();
        check(!host.visible() && embedded_visible(),
            "the second host close did not restore embedded chat");
    }
}

void test_main_bar_preview_invalidates_visible_detached_window() {
    MapResources resources;
    dhsh::ChatModel model;
    RecordingSendSink sink;
    dhsh::ChatWindowHost host([&]() {
        return make_chat_window(resources, model, sink, 60, 60, 549, 590);
    });
    host.bind_chat_model(model);
    host.toggle();
    ValidateRect(host.window()->handle(), nullptr);
    dhsh::MessageBus bus;
    dhsh::ScpWindow bar_window = make_bar_window(resources, bus);
    dhsh::MainChatBar bar(bar_window, bus, sink, &model);
    bar_window.handle_event(mouse_event(
        dhsh::WindowEvent::Type::left_button_down, {100, 590}));
    bar_window.handle_event(mouse_event(
        dhsh::WindowEvent::Type::left_button_up, {100, 590}));
    bar_window.handle_event(text_event("refresh now"));
    bar.note_key_down(VK_RETURN);
    bar_window.handle_event(key_event(VK_RETURN));
    RECT update{};
    check(model.visible_messages().size() == 1
            && GetUpdateRect(host.window()->handle(), &update, FALSE) != FALSE,
        "main-bar offline preview did not synchronously invalidate the visible detached window");
}

void test_main_chat_bar_offline_preview_uses_real_enter_path() {
    FakeResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window = make_bar_window(resources, bus);
    dhsh::ChatModel model;
    RecordingSendSink sink;
    dhsh::MainChatBar bar(window, bus, sink, &model);
    auto* edit = static_cast<dhsh::RichEdit*>(window.behavior_for("input_mes"));
    check(edit != nullptr, "offline preview input edit was not registered");
    using Type = dhsh::WindowEvent::Type;
    window.handle_event(mouse_event(Type::left_button_down, {100, 590}));
    window.handle_event(mouse_event(Type::left_button_up, {100, 590}));
    const auto enter = [&window, &bar] {
        bar.note_key_down(VK_RETURN);
        window.handle_event(key_event(VK_RETURN));
    };
    enter();
    check(sink.sends.empty() && model.visible_messages().empty(),
        "empty main-bar Enter must neither send nor append a preview");

    const std::string long_text(120, 'L');
    window.handle_event(text_event(long_text));
    enter();
    check(sink.sends.size() == 1 && model.visible_messages().size() == 1,
        "offline main-bar Enter must send once and append one local preview");
    const dhsh::ChatMessage& first = model.visible_messages().front();
    check(first.channel == 1 && first.sender == "\xE6\x9C\xAC\xE5\x9C\xB0\xE9\xA2\x84\xE8\xA7\x88",
        "local preview must use the current channel and explicit sender");
    check(first.spans.size() == 1
            && first.spans.front().kind == dhsh::ChatSpanKind::text
            && first.spans.front().text == long_text
            && first.spans.front().color == "red"
            && first.spans.front().link.empty(),
        "local preview text mismatch: expected_bytes=" + std::to_string(long_text.size())
            + " actual_bytes=" + (first.spans.empty() ? std::string("0")
                : std::to_string(first.spans.front().text.size())));
    check(edit->text().empty(), "offline main-bar Enter must clear the edit");

    auto* combo = static_cast<dhsh::ComboBox*>(
        window.behavior_for("bar_txtlstchanel"));
    check(combo != nullptr && combo->select(4), "world channel was not selectable");
    bus.dispatch({"bar_txtlstchanel", "FSM_COMBOBOX_SELECTED", -1,
        "OnChannelSel", "bar.scp"});
    window.handle_event(text_event("world"));
    enter();
    check(sink.sends.size() == 2 && sink.sends.back().first == 5
            && model.visible_messages().size() == 2
            && model.visible_messages().back().channel == 5
            && model.visible_messages().back().spans.front().text == "world"
            && bar.selected_channel() == 5,
        "world-channel preview must use and retain the selected channel");
    enter();
    check(sink.sends.size() == 2 && model.visible_messages().size() == 2,
        "empty Enter after a preview must not duplicate the last message");
}

void test_detached_chat_offline_preview_uses_real_enter_path() {
    MapResources resources;
    dhsh::ChatModel model;
    RecordingSendSink sink;
    auto window = make_chat_window(resources, model, sink, 60, 60, 549, 590, true);
    window->set_attached(true);
    auto* edit = static_cast<dhsh::RichEdit*>(
        window->ui()->behavior_for("chatmain_input_mes"));
    auto* view = static_cast<dhsh::ListViewEx*>(
        window->ui()->behavior_for("chatmain_txtlst_msg"));
    check(edit != nullptr && view != nullptr,
        "offline detached chat input or shared list was not registered");
    click_client(window->handle(), 100, 545);
    SendMessageW(window->handle(), WM_KEYDOWN, VK_RETURN, 0);
    check(sink.sends.empty() && model.visible_messages().empty() && view->rows().empty(),
        "empty detached Enter must neither send nor append a preview");

    const std::string long_text(120, 'D');
    for (const char value : long_text) {
        SendMessageW(window->handle(), WM_CHAR,
            static_cast<unsigned char>(value), 0);
    }
    SendMessageW(window->handle(), WM_KEYDOWN, VK_RETURN, 0);
    check(sink.sends.size() == 1 && model.visible_messages().size() == 1
            && sink.sends.front().second == long_text
            && model.visible_messages().front().sender == "\xE6\x9C\xAC\xE5\x9C\xB0\xE9\xA2\x84\xE8\xA7\x88"
            && model.visible_messages().front().spans.front().text == long_text
            && edit->text().empty(),
        "detached offline Enter must preserve the complete long text");
    check(!view->rows().empty(),
        "detached offline preview did not update the shared ListViewEx rows");

    auto* combo = static_cast<dhsh::ComboBox*>(
        window->ui()->behavior_for("chatmain_txtlstchanel"));
    check(combo != nullptr && combo->select(4), "detached world channel was not selectable");
    window->messages().dispatch({"chatmain_txtlstchanel", "FSM_COMBOBOX_SELECTED",
        -1, "OnChannelSel", "chatmain.scp"});
    SendMessageW(window->handle(), WM_CHAR, L'W', 0);
    SendMessageW(window->handle(), WM_KEYDOWN, VK_RETURN, 0);
    check(sink.sends.size() == 2 && sink.sends.back().first == 5
            && model.visible_messages().size() == 2
            && model.visible_messages().back().channel == 5
            && model.visible_messages().back().spans.front().text == "W"
            && window->selected_channel() == 5 && view->rows().size() >= 2,
        "detached world preview must use the selected channel and remain visible");
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"parser tree, encoding, includes and conditionals", test_parser_tree_encoding_and_conditionals},
        {"widget resources, rendering and coordinates", test_widgets_resources_render_and_coordinates},
        {"message dispatch and map input isolation", test_message_dispatch_and_input_isolation},
        {"duplicate visible button layout is unsupported", test_duplicate_visible_button_layout_is_unsupported},
        {"parser input, depth and node budgets", test_parser_input_depth_and_node_budgets},
        {"safe include expansion", test_safe_include_expansion},
        {"window budget and invalid layout subtree", test_window_budget_and_invalid_layout_subtree},
        {"text clipping and pixel budget", test_text_is_clipped_and_pixel_bounded},
        {"bounded reentrant dispatch", test_reentrant_dispatch_is_bounded},
        {"callback destroys window during dispatch", test_callback_can_destroy_window_during_dispatch},
        {"dynamic UI regions and press state", test_map_input_uses_dynamic_regions_and_ui_press_state},
        {"App callback clears a window slot", test_app_callback_can_clear_window_slot},
        {"Widget rejects null before initialization", test_widget_rejects_null_before_initialization},
        {"SCP stage diagnostics cover parser and window", test_scp_stage_diagnostics_cover_parser_and_window},
        {"child visibility survives widget vector growth", test_children_inherit_visibility_across_widget_vector_growth},
        {"chatmain declarations build supported widgets", test_chatmain_declarations_build_supported_widgets},
        {"chatmain message names cover input, list and paging", test_chatmain_message_names_cover_input_list_and_paging},
        {"chatmain paging buttons and right click dispatch by button", test_chatmain_paging_buttons_and_right_click_dispatch_by_button},
        {"richedit focus, editing, keys and input isolation", test_richedit_focus_editing_keys_and_input_isolation},
        {"richedit ime composition commit and cancel", test_richedit_ime_composition_commit_and_cancel},
        {"richedit max length and enter does not fake send", test_richedit_max_length_and_enter_does_not_fake_send},
        {"combobox items, selection, dropdown and enable", test_combobox_items_selection_dropdown_and_enable},
        {"disabled richedit declared with ENABLE=0 ignores input", test_disabled_richedit_declared_with_enable_zero_ignores_input},
        {"widget set_enabled toggles richedit and combo live", test_widget_set_enabled_toggles_richedit_and_combo_live},
        {"listview rows wrap channel colors and item links", test_listview_rows_wrap_channel_colors_and_item_links},
        {"listview scroll wheel drag range and right click", test_listview_scroll_wheel_drag_range_and_right_click},
        {"listview shares chat model read only", test_listview_shares_chat_model_read_only},
        {"win32 window emits utf8 text and ime boundaries", test_win32_window_emits_utf8_text_and_ime_boundaries},
        {"chat main layout matches native geometry at 549x590", test_chat_main_layout_matches_native_geometry_at_549x590},
        {"chat main layout second size proves absolute rows", test_chat_main_layout_second_size_proves_absolute_rows},
        {"chat window native layout skin frame and close behavior", test_chat_window_native_layout_skin_frame_and_close_behavior},
        {"chat window resize recomputes absolute layout", test_chat_window_resize_recomputes_absolute_layout},
        {"chat window shares model and filter preserves history", test_chat_window_shares_model_and_filter_preserves_history},
        {"chat window pause appends without jump and resume returns", test_chat_window_pause_appends_without_jump_and_resume_returns},
        {"chat window auto scroll reaches bottom past range span", test_chat_window_auto_scroll_reaches_bottom_past_range_span},
        {"chat window up button leaves bottom by one row", test_chat_window_up_button_leaves_bottom_by_one_row},
        {"chat window wheel steps one row off the bottom", test_chat_window_wheel_steps_one_row_off_the_bottom},
        {"chat window repeated paging steps one row", test_chat_window_repeated_paging_steps_one_row},
        {"chat window input enters through the single sink without fake success", test_chat_window_input_enters_through_the_single_sink_without_fake_success},
        {"chat window switch button toggles attached without new window", test_chat_window_switch_button_toggles_attached_without_new_window},
        {"chat window host toggle is idempotent and leaks no window", test_chat_window_host_toggle_is_idempotent_and_leaks_no_window},
        {"chat window hide show keeps runtime state", test_chat_window_hide_show_keeps_runtime_state},
        {"chatmain hidden subtree loads images when shown", test_chatmain_hidden_subtree_loads_images_when_shown},
        {"listview rows rebase into view when scrolled", test_listview_rows_rebase_into_view_when_scrolled},
        {"listview first row clamps to last visible page", test_listview_first_row_clamps_to_last_visible_page},
        {"chat window physical enter sequence leaves edit empty", test_chat_window_physical_enter_sequence_leaves_edit_empty},
        {"main chat bar enter sends through sink and clears", test_main_chat_bar_enter_sends_through_sink_and_clears},
        {"original runtime default text is small native font", test_original_runtime_default_text_is_small_native_font},
        {"original runtime TXTLIST rows are bottom anchored", test_original_runtime_txtlist_rows_are_bottom_anchored},
        {"main chat bar channel combo physical click", test_main_chat_bar_channel_combo_physical_click},
        {"main chat bar physical enter sequence leaves edit empty", test_main_chat_bar_physical_enter_sequence_leaves_edit_empty},
        {"win32 window filters control character text", test_win32_window_filters_control_character_text},
        {"generic button visual states and cross-window hover", test_generic_button_visual_states_and_cross_window_hover},
        {"TXTLIST font metrics, native skin and hit geometry", test_txtlist_font_metrics_native_skin_and_hit_geometry},
        {"SHOWHIDE toggles target without revealing hidden descendants", test_showhide_toggles_target_without_revealing_hidden_descendants},
        {"chat host restores embedded chat from real buttons", test_chat_host_restores_embedded_chat_from_real_buttons},
        {"main bar preview invalidates visible detached window", test_main_bar_preview_invalidates_visible_detached_window},
        {"main chat bar offline preview uses real Enter path", test_main_chat_bar_offline_preview_uses_real_enter_path},
        {"detached chat offline preview uses real Enter path", test_detached_chat_offline_preview_uses_real_enter_path},
    };
    if (argc == 3 && std::string_view(argv[1]) == "--real") {
        const std::string shdata = argv[2];
        tests.emplace_back("real config.pak+ui.pak NZ chatmain.scp declarations",
            [shdata] { test_real_chatmain_declarations_from_config_pak(shdata); });
        tests.emplace_back("real chatmain attach and reopen render",
            [shdata] { test_real_chatmain_attach_and_reopen_render(shdata); });
        tests.emplace_back("real 800config bar.scp input row declarations",
            [shdata] { test_real_bar_input_row_from_800config_pak(shdata); });
        tests.emplace_back("real button states, native channel skin and chat messages",
            [shdata] { test_real_button_states_native_channel_skin_and_chat_messages(shdata); });
        tests.emplace_back("real APG button states match all four decoded frames",
            [shdata] { test_real_apg_button_states_match_decoded_frames(shdata); });
        tests.emplace_back("real App main bar initial 当前 is visible",
            [shdata] { test_real_app_main_bar_initial_current_is_visible(shdata); });
        tests.emplace_back("real App main bar full window input order",
            [shdata] { test_real_app_main_bar_full_window_input_order(shdata); });
        tests.emplace_back("real App Shift+arrow full window message order",
            [shdata] { test_real_app_shift_arrow_full_window_message_order(shdata); });
        tests.emplace_back("real App profiled shortcut arrow initialization",
            [shdata] { test_real_app_profiled_shortcut_arrow_initialization(shdata); });
        tests.emplace_back("real App main chat buttons toggle native panels",
            [shdata] { test_real_app_main_chat_buttons_toggle_native_panels(shdata); });
        tests.emplace_back("real App daynight buttons toggle native panels",
            [shdata] { test_real_app_daynight_buttons_toggle_native_panels(shdata); });
        tests.emplace_back("real App internal close buttons hide and reopen native panels",
            [shdata] { test_real_app_internal_close_buttons_hide_and_reopen_panels(shdata); });
        tests.emplace_back("real App shortcut arrows toggle bars and input",
            [shdata] { test_real_app_shortcut_arrows_toggle_bars_and_input(shdata); });
    } else if (argc != 1) {
        std::cerr << "usage: dhsh_test_scp_ui [--real <shdata-directory>]\n";
        return 2;
    }
    bool passed = true;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
            passed = false;
        }
    }
    return passed ? 0 : 1;
}
