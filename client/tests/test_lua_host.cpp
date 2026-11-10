#include "resource/NzDecoder.h"
#include "resource/PakResourceProvider.h"
#include "resource/ResourceProvider.h"
#include "script/FsLuaHost.h"
#include "script/ScpParser.h"
#include "ui/MessageBus.h"
#include "ui/ScpWindow.h"
#include "ui/Widget.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
thread_local std::size_t fail_any_new_countdown = 0;
thread_local std::size_t fail_new_size = 0;
thread_local std::size_t fail_new_countdown = 0;
thread_local std::size_t tracked_new_size = 0;
thread_local std::size_t tracked_new_count = 0;
thread_local bool track_all_new = false;
}

void* operator new(std::size_t size) {
    if (tracked_new_size == size || track_all_new) {
        ++tracked_new_count;
    }
    const bool matching_size = fail_new_size != 0 && fail_new_size == size
        && --fail_new_countdown == 0;
    const bool matching_any = fail_any_new_countdown != 0
        && --fail_any_new_countdown == 0;
    if (matching_any || matching_size) {
        fail_new_size = 0;
        throw std::bad_alloc();
    }
    if (void* result = std::malloc(size)) {
        return result;
    }
    throw std::bad_alloc();
}

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

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

struct AllocationControl {
    std::size_t allocations = 0;
    std::size_t fail_at = 0;
};

void* controlled_lua_alloc(void* userdata, void* pointer, std::size_t, std::size_t size) {
    auto& control = *static_cast<AllocationControl*>(userdata);
    if (size != 0) {
        ++control.allocations;
        if (control.fail_at == control.allocations) {
            return nullptr;
        }
    }
    if (size == 0) {
        std::free(pointer);
        return nullptr;
    }
    return std::realloc(pointer, size);
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

void test_execute_discards_returns_and_restores_stack() {
    dhsh::LuaVm vm;
    lua_State* state = vm.native_handle();
    vm.execute("return 1, 2, 3", "returns.lua");
    check(lua_gettop(state) == 0, "Lua execute retained script return values");
    vm.execute("return 4, 5", "more_returns.lua");
    check(lua_gettop(state) == 0, "Lua return values accumulated across executions");

    try {
        vm.execute("error( 'runtime boom' )", "runtime.lua");
    } catch (const std::exception& error) {
        const std::string text = error.what();
        check(text.find("runtime.lua:1") != std::string::npos
                && text.find("runtime boom") != std::string::npos,
            "Lua runtime error lacks script location or detail: " + text);
        check(lua_gettop(state) == 0, "Lua runtime error left values on the stack");
        return;
    }
    throw std::runtime_error("Lua runtime error was not reported");
}

void test_registration_allocation_failure_is_lua_error_and_rolls_back() {
    EmptyResources resources;
    tracked_new_count = 0;
    track_all_new = true;
    {
        dhsh::MessageBus probe_bus;
        dhsh::ScpWindow probe_window(bulletin_document(), resources, probe_bus);
        probe_window.lua_host()->vm().execute("function ProbeCallback() end", "x");
        tracked_new_count = 0;
        probe_window.lua_host()->vm().execute(
            "FSLuaWndScript.RegMsgCall( \"ProbeCallback\" )", "x");
    }
    track_all_new = false;
    check(tracked_new_count > 0, "registration probe observed no host allocation");

    dhsh::MessageBus bus;
    dhsh::ScpWindow window(bulletin_document(), resources, bus);
    dhsh::LuaVm& vm = window.lua_host()->vm();
    vm.execute("function NewCallback() end", "x");
    lua_State* state = vm.native_handle();
    lua_getglobal(state, "NewCallback");
    const int before = luaL_ref(state, LUA_REGISTRYINDEX);

    std::string diagnostic;
    fail_any_new_countdown = tracked_new_count;
    try {
        vm.execute("FSLuaWndScript.RegMsgCall( \"NewCallback\" )", "x");
    } catch (const std::exception& error) {
        diagnostic = error.what();
    }
    fail_any_new_countdown = 0;
    lua_getglobal(state, "NewCallback");
    const int after = luaL_ref(state, LUA_REGISTRYINDEX);
    luaL_unref(state, LUA_REGISTRYINDEX, after);
    luaL_unref(state, LUA_REGISTRYINDEX, before);

    check(diagnostic.find("Lua runtime error") != std::string::npos
            && diagnostic.find("native host allocation failure") != std::string::npos,
        "C++ allocation failure escaped the Lua C callback: " + diagnostic);
    check(after == before + 1,
        "failed RegMsgCall leaked its registry reference");
    check(window.lua_host()->registered_callback_count() == 3,
        "failed RegMsgCall changed the callback registry");
}

void test_subscription_token_allocation_failure_leaves_no_handler() {
    EmptyResources resources;
    const dhsh::ScpDocument document = bulletin_document();
    const std::size_t token_size = sizeof(dhsh::MessageBus::Subscription);
    tracked_new_size = token_size;
    tracked_new_count = 0;
    {
        dhsh::MessageBus bus;
        dhsh::ScpWindow window(document, resources, bus);
    }
    tracked_new_size = 0;
    check(tracked_new_count > 0, "test did not observe token storage allocation");

    dhsh::MessageBus bus;
    std::unique_ptr<dhsh::ScpWindow> window;
    fail_new_size = token_size;
    fail_new_countdown = tracked_new_count;
    try {
        window = std::make_unique<dhsh::ScpWindow>(document, resources, bus);
    } catch (const std::bad_alloc&) {
    }
    fail_new_size = 0;
    check(window == nullptr, "subscription allocation failure did not abort construction");
    const bool handled = bus.dispatch(dhsh::UiMessage{"Bulletin_main_title06",
        "FSM_BTN_MSG", 1, "OnShowPolicy", "ui/login/bulletin2.scp"});
    check(!handled, "failed subscription token push left a dangling Lua handler");
    check(!bus.diagnostics().empty(),
        "rolled-back subscription did not leave an unhandled-message diagnostic");
}

void test_library_allocation_failure_is_diagnostic() {
    AllocationControl probe;
    lua_State* state = lua_newstate(&controlled_lua_alloc, &probe);
    check(state != nullptr, "Lua allocation probe could not create a state");
    const std::size_t state_allocations = probe.allocations;
    lua_close(state);

    const std::size_t offsets[] = {1, 2, 8};
    for (const std::size_t offset : offsets) {
        AllocationControl control;
        control.fail_at = state_allocations + offset;
        try {
            dhsh::LuaVm vm(&controlled_lua_alloc, &control);
        } catch (const std::exception& error) {
            const std::string text = error.what();
            check(text.find("library initialization") != std::string::npos
                    && text.find("memory") != std::string::npos,
                "Lua library allocation failure was not diagnostic at offset "
                + std::to_string(offset) + ": " + text);
            continue;
        }
        throw std::runtime_error("Lua library allocation failure was ignored at offset "
            + std::to_string(offset));
    }
}

void test_host_api_allocation_failure_is_diagnostic() {
    EmptyResources resources;
    dhsh::ScpDocument document = bulletin_document();
    document.scripts.clear();
    dhsh::MessageBus bus;
    dhsh::ScpWindow window(std::move(document), resources, bus);

    AllocationControl probe;
    {
        dhsh::LuaVm vm(&controlled_lua_alloc, &probe);
    }
    const std::size_t offsets[] = {1, 2, 8};
    for (const std::size_t offset : offsets) {
        AllocationControl control;
        control.fail_at = probe.allocations + offset;
        try {
            dhsh::FsLuaHost host(window, bus, &controlled_lua_alloc, &control);
        } catch (const std::exception& error) {
            const std::string text = error.what();
            check(text.find("Lua host initialization error") != std::string::npos
                    && text.find("memory") != std::string::npos,
                "host API allocation failure was not diagnostic at offset "
                + std::to_string(offset) + ": " + text);
            continue;
        }
        throw std::runtime_error("host API allocation failure was ignored at offset "
            + std::to_string(offset));
    }
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

void test_destroy_handler_before_lua_skips_snapshot_token() {
    EmptyResources resources;
    dhsh::MessageBus bus;
    std::unique_ptr<dhsh::ScpWindow> window;
    bus.subscribe("Bulletin_main_title06", "FSM_BTN_MSG",
        [&](const dhsh::UiMessage&) { window.reset(); });
    window = std::make_unique<dhsh::ScpWindow>(bulletin_document(), resources, bus);
    using Type = dhsh::WindowEvent::Type;
    window->handle_event(mouse_event(Type::left_button_down, {160, 115}));
    window->handle_event(mouse_event(Type::left_button_up, {160, 115}));
    check(window == nullptr, "early test handler did not destroy its window");
    check(bus.diagnostics().empty(), "destroyed Lua snapshot token was dispatched");
}

void test_lua_message_reentry_uses_bounded_queue() {
    EmptyResources resources;
    dhsh::MessageBus bus(4);
    dhsh::ScpWindow window(bulletin_document(), resources, bus);
    bool reentered = false;
    bus.subscribe("Bulletin_main_title06", "FSM_BTN_MSG",
        [&](const dhsh::UiMessage& message) {
            if (!reentered) {
                reentered = true;
                bus.dispatch(message);
            }
        });
    using Type = dhsh::WindowEvent::Type;
    window.handle_event(mouse_event(Type::left_button_down, {160, 115}));
    window.handle_event(mouse_event(Type::left_button_up, {160, 115}));
    check(reentered && window.lua_host()->callback_count() == 2,
        "Lua message reentry did not run exactly the outer and queued callbacks");
    check(bus.diagnostics().empty(), "bounded Lua reentry produced queue diagnostics");
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"original SCP button runs Lua and changes window state",
            test_original_button_callback_changes_window_state},
        {"Lua execute restores stack and reports runtime errors",
            test_execute_discards_returns_and_restores_stack},
        {"RegMsgCall allocation failure is caught and rolled back",
            test_registration_allocation_failure_is_lua_error_and_rolls_back},
        {"subscription token allocation failure rolls back handler",
            test_subscription_token_allocation_failure_leaves_no_handler},
        {"Lua library allocation failure is diagnostic",
            test_library_allocation_failure_is_diagnostic},
        {"host API allocation failure is diagnostic",
            test_host_api_allocation_failure_is_diagnostic},
        {"Lua syntax error includes script name and line",
            test_syntax_error_has_script_name_and_line},
        {"unsupported host APIs are explicit errors",
            test_unsupported_host_apis_are_explicit_errors},
        {"Lua callback preserves destruction and queue safety",
            test_lua_callback_preserves_destruction_and_queue_safety},
        {"early destruction skips snapshotted Lua token",
            test_destroy_handler_before_lua_skips_snapshot_token},
        {"Lua message reentry uses bounded queue",
            test_lua_message_reentry_uses_bounded_queue},
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
