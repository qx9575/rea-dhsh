#include "resource/NzDecoder.h"
#include "resource/PakResourceProvider.h"
#include "resource/ResourceProvider.h"
#include "script/FsLuaHost.h"
#include "script/ScpParser.h"
#include "ui/MessageBus.h"
#include "ui/ScpWindow.h"
#include "ui/Widget.h"

#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
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

class EmptyResources final : public dhsh::ResourceProvider {
public:
    std::vector<std::byte> bytes(std::string_view logical_path) override {
        throw std::runtime_error("unexpected resource read: " + std::string(logical_path));
    }

    bool exists(std::string_view) const override {
        return false;
    }
};

dhsh::WindowEvent mouse_event(dhsh::WindowEvent::Type type, dhsh::LogicalPoint point) {
    dhsh::WindowEvent event;
    event.type = type;
    event.logical_position = point;
    return event;
}

dhsh::ScpDocument bulletin_document() {
    static const std::string source = R"SCP(#script

FSLuaInstall( "FSLuaFSButton" )
FSLuaInstall( "FSLuaFSWindow" )

function OnShowPolicy( p1, p2 )
	wndPolicy:SetShow( true )
	wndBulletin:SetShow( false )
	wndLicense:SetShow( false )
	btnPolicy:Enable( false )
	btnBulletin:Enable( true )
	btnLicense:Enable( true )
end
function OnShowLicense( p1, p2 )
	wndPolicy:SetShow( false )
	wndBulletin:SetShow( false )
	wndLicense:SetShow( true )
	btnPolicy:Enable( true )
	btnBulletin:Enable( true )
	btnLicense:Enable( false )
end

function OnShowBulletin( p1, p2 )
	wndPolicy:SetShow( false )
	wndBulletin:SetShow( true )
	wndLicense:SetShow( false )
	btnPolicy:Enable( true )	
	btnBulletin:Enable( false )
	btnLicense:Enable( true )
end

FSLuaWndScript.RegMsgCall( "OnShowLicense" )
FSLuaWndScript.RegMsgCall( "OnShowBulletin" )
FSLuaWndScript.RegMsgCall( "OnShowPolicy" )

#endscript

WINDOW TYPE = WIN
 NAME = "Bulletin"
 RECT = 0,0,800,600
 WINDOW TYPE = WIN
  NAME = "Bulletin_wnd"
  RECT = 134,73,552,400
  WINDOW TYPE = BUTTON
   NAME = "Bulletin_main_title06"
   RECT = 22,37,88,32
   LUACONTROL = "btnPolicy"
   MESSAGE FSM_BTN_MSG 1 CODE "OnShowPolicy"
  END
  WINDOW TYPE = BUTTON
   NAME = "Bulletin_main_title05"
   RECT = 96,37,88,32
   ENABLE = "0"
   LUACONTROL = "btnBulletin"
   MESSAGE FSM_BTN_MSG 1 CODE "OnShowBulletin"
  END
  WINDOW TYPE = BUTTON
   NAME = "Bulletin_main_title03"
   RECT = 170,37,88,32
   LUACONTROL = "btnLicense"
   MESSAGE FSM_BTN_MSG 1 CODE "OnShowLicense"
  END
  WINDOW TYPE = WIN
   NAME = "Bulletin_win06"
   RECT = 21,62,246,315
   HIDE = "1"
   LUACONTROL = "wndPolicy"
  END
  WINDOW TYPE = WIN
   NAME = "Bulletin_win00"
   RECT = 21,62,246,315
   LUACONTROL = "wndBulletin"
  END
  WINDOW TYPE = WIN
   NAME = "Bulletin_win03"
   RECT = 21,62,246,315
   HIDE = "1"
   LUACONTROL = "wndLicense"
  END
 END
END
)SCP";
    return dhsh::ScpParser::parse(bytes(source), "ui/login/bulletin2.scp");
}

void expect_lua_error(std::string_view script, std::string_view source_name,
    std::string_view expected) {
    const std::string source = "#script\n" + std::string(script)
        + "\n#endscript\nWINDOW TYPE=WIN\nNAME=\"root\"\nEND\n";
    EmptyResources resources;
    dhsh::MessageBus bus;
    try {
        dhsh::ScpWindow window(dhsh::ScpParser::parse(bytes(source),
            std::string(source_name)), resources, bus);
    } catch (const std::exception& error) {
        const std::string text = error.what();
        check(text.find(source_name) != std::string::npos,
            "Lua error lost its script name: " + text);
        check(text.find(":1:") != std::string::npos,
            "Lua error lost its script line: " + text);
        check(text.find(expected) != std::string::npos,
            "Lua error did not identify the unsupported API: " + text);
        return;
    }
    throw std::runtime_error("invalid Lua script was accepted: " + std::string(source_name));
}

void check_original_button_callback(dhsh::ScpWindow& window) {
    check(window.lua_host() != nullptr, "SCP #script did not create a Lua host");
    check(window.lua_host()->registered_callback_count() == 3,
        "original RegMsgCall calls did not register all three callbacks");
    const dhsh::Widget* policy = window.find_widget("Bulletin_win06");
    const dhsh::Widget* bulletin = window.find_widget("Bulletin_win00");
    const dhsh::Widget* license = window.find_widget("Bulletin_win03");
    const dhsh::Widget* policy_button = window.find_widget("Bulletin_main_title06");
    const dhsh::Widget* bulletin_button = window.find_widget("Bulletin_main_title05");
    const dhsh::Widget* license_button = window.find_widget("Bulletin_main_title03");
    check(policy != nullptr && bulletin != nullptr && license != nullptr
            && policy_button != nullptr && bulletin_button != nullptr
            && license_button != nullptr,
        "original SCP controls were not created");
    check(!policy->visible() && bulletin->visible() && !license->visible()
            && policy_button->enabled() && !bulletin_button->enabled()
            && license_button->enabled(),
        "original SCP initial window state differs");

    using Type = dhsh::WindowEvent::Type;
    check(window.handle_event(mouse_event(Type::left_button_down, {160, 115})),
        "original policy button did not consume mouse down");
    check(window.handle_event(mouse_event(Type::left_button_up, {160, 115})),
        "original policy button did not consume mouse up");
    check(window.lua_host()->callback_count() == 1,
        "SCP button did not invoke the original Lua function exactly once");
    check(policy->visible() && !bulletin->visible() && !license->visible()
            && !policy_button->enabled() && bulletin_button->enabled()
            && license_button->enabled(),
        "OnShowPolicy did not apply the original Lua window state changes");
}

void test_original_button_callback_changes_window_state() {
    EmptyResources resources;
    dhsh::MessageBus bus;
    dhsh::ScpWindow window(bulletin_document(), resources, bus);
    check_original_button_callback(window);
}

void test_real_button_callback_changes_window_state(const std::string& shdata) {
    dhsh::PakResourceProvider resources(shdata, {"config.pak"});
    const std::string path = "ui/login/bulletin2.scp";
    std::vector<std::byte> source = resources.bytes(path);
    check(source.size() >= 2 && source[0] == std::byte{'N'}
            && source[1] == std::byte{'Z'},
        "real bulletin2.scp was not read as its original NZ resource");
    source = dhsh::NzDecoder::decode(source);
    dhsh::ScpDocument document = dhsh::ScpParser::parse(source, path);
    check(document.scripts.size() == 1,
        "real bulletin2.scp did not retain its complete original #script");
    dhsh::MessageBus bus;
    dhsh::ScpWindow window(std::move(document), resources, bus);
    check_original_button_callback(window);
    std::cout << "REAL_LUA_HOST PASS pak=config.pak path=" << path
              << " callbacks=3 calls=1 OnShowPolicy=true\n";
}

void test_syntax_error_has_script_name_and_line() {
    expect_lua_error("function Broken(", "broken_window.scp", "syntax error");
}

void test_unsupported_host_apis_are_explicit_errors() {
    expect_lua_error("FSLuaWndScript.RegAfterCreate( \"AfterCreate\" )",
        "unsupported_callback.scp", "FSLuaWndScript.RegAfterCreate");
    expect_lua_error("FSLuaInstall( \"FSGameExchange\" )",
        "unsupported_install.scp", "FSLuaInstall");
}

void test_lua_callback_preserves_destruction_and_queue_safety() {
    EmptyResources resources;
    dhsh::MessageBus bus;
    auto window = std::make_unique<dhsh::ScpWindow>(
        bulletin_document(), resources, bus);
    bus.subscribe("Bulletin_main_title06", "FSM_BTN_MSG",
        [&](const dhsh::UiMessage&) { window.reset(); });
    using Type = dhsh::WindowEvent::Type;
    window->handle_event(mouse_event(Type::left_button_down, {160, 115}));
    window->handle_event(mouse_event(Type::left_button_up, {160, 115}));
    check(window == nullptr, "test callback did not destroy its window");
    check(bus.diagnostics().empty(), "Lua callback dispatch produced queue diagnostics");
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"original SCP button runs Lua and changes window state",
            test_original_button_callback_changes_window_state},
        {"Lua syntax error includes script name and line",
            test_syntax_error_has_script_name_and_line},
        {"unsupported host APIs are explicit errors",
            test_unsupported_host_apis_are_explicit_errors},
        {"Lua callback preserves destruction and queue safety",
            test_lua_callback_preserves_destruction_and_queue_safety},
    };
    if (argc == 3 && std::string_view(argv[1]) == "--real") {
        const std::string shdata = argv[2];
        tests.emplace_back("real config.pak NZ bulletin callback",
            [shdata] { test_real_button_callback_changes_window_state(shdata); });
    } else if (argc != 1) {
        std::cerr << "usage: dhsh_test_lua_host [--real <shdata-directory>]\n";
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
