#include "input/MapInput.h"
#include "render/SoftwareRenderer.h"
#include "resource/ResourceProvider.h"
#include "script/ScpParser.h"
#include "ui/MessageBus.h"
#include "ui/ScpWindow.h"
#include "ui/Widget.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
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

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
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
    };
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
