#include "chat/ChatModel.h"
#include "input/MapInput.h"
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
// are verbatim; the switch-button strip is trimmed to the paging and shield
// buttons exercised here. bar_visible drops HIDE=1 from chatmain_barwnd so
// event routing can be exercised on an otherwise offscreen subtree. The
// bottom bar (chatmain_allbtn) is declared after the message list so its
// buttons stay clickable: the real file declares it earlier, which leaves
// the bar inside the LISTVIEWEX RECT's z-order shadow.
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
              "ui/main/panel.a16", "ui/main/ok/default.tga"},
        "widget resource paths differ");
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
    check(window.widgets().size() == 5 && window_profile.images == 2,
        "SCP window profile lost image or widget counts");
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
    check(window.handle_event(mouse_event(Type::left_button_down, {1636, 1150})),
        "click on a dropdown row was not consumed");
    check(window.handle_event(mouse_event(Type::left_button_up, {1636, 1150})),
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
    check(view->first_visible_row() == 5,
        "scroll position did not map VALUERGN minimum to the first row");

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
    };
    if (argc == 3 && std::string_view(argv[1]) == "--real") {
        const std::string shdata = argv[2];
        tests.emplace_back("real config.pak+ui.pak NZ chatmain.scp declarations",
            [shdata] { test_real_chatmain_declarations_from_config_pak(shdata); });
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
