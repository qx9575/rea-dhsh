#include "input/MapInput.h"
#include "render/SoftwareRenderer.h"
#include "resource/ResourceProvider.h"
#include "script/ScpParser.h"
#include "ui/MessageBus.h"
#include "ui/ScpWindow.h"
#include "ui/Widget.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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
 WINDOW TYPE = BUTTON
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

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"parser tree, encoding, includes and conditionals", test_parser_tree_encoding_and_conditionals},
        {"widget resources, rendering and coordinates", test_widgets_resources_render_and_coordinates},
        {"message dispatch and map input isolation", test_message_dispatch_and_input_isolation},
        {"duplicate visible button layout is unsupported", test_duplicate_visible_button_layout_is_unsupported},
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
