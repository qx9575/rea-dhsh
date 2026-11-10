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
#include <cstring>
#include <initializer_list>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dhsh {
namespace {

constexpr const char* widget_metatable = "dhsh.FSLuaWidget";

struct LuaWidgetRef {
    Widget* widget = nullptr;
};

LuaWidgetRef* check_widget(lua_State* state) {
    return static_cast<LuaWidgetRef*>(luaL_checkudata(state, 1, widget_metatable));
}

} // namespace

struct FsLuaHost::Impl {
    Impl(ScpWindow& window, MessageBus& messages)
        : window_(window), messages_(messages) {}

    ~Impl() {
        unsubscribe_all();
    }

    void initialize() {
        install_api();
        expose_widgets();
        const ScpDocument& document = window_.document();
        for (const std::string& script : document.scripts) {
            vm_.execute(script, document.source_file);
        }
        subscribe_messages();
    }

    std::size_t registered_callback_count() const noexcept {
        return callbacks_.size();
    }

    std::size_t callback_count() const noexcept {
        return callback_count_;
    }

    LuaVm& vm() noexcept {
        return vm_;
    }

private:
    // RegMsgCall(name:string) is the observed original form:
    //   FSLuaWndScript.RegMsgCall( "OnShowPolicy" )
    // The argument is the name of a Lua global function, not a separate
    // message name or function value. The SCP MESSAGE ... CODE "OnShowPolicy"
    // declaration is the trigger key for the captured reference.
    static int reg_msg_call(lua_State* state) {
        auto* host = static_cast<Impl*>(lua_touserdata(state, lua_upvalueindex(1)));
        const char* name = luaL_checkstring(state, 1);
        lua_getglobal(state, name);
        if (!lua_isfunction(state, -1)) {
            lua_pop(state, 1);
            return luaL_error(state,
                "FSLuaWndScript.RegMsgCall: global function '%s' is not defined", name);
        }
        const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
        host->add_callback(state, name, reference);
        lua_pushboolean(state, 1);
        return 1;
    }

    static int unsupported_wnd_api(lua_State* state) {
        const char* name = lua_tostring(state, lua_upvalueindex(1));
        return luaL_error(state,
            "FSLuaWndScript.%s is Unsupported in the native host",
            name != nullptr ? name : "<unknown>");
    }

    static int fs_lua_install(lua_State* state) {
        const char* name = luaL_checkstring(state, 1);
        // The SCP scripts begin with the two UI plugin installs below. The
        // native host supplies their observable functionality directly
        // (LUACONTROL widget globals with SetShow/Enable). Any other install
        // would silently claim support we do not have, so reject it.
        if (std::strcmp(name, "FSLuaFSButton") == 0
            || std::strcmp(name, "FSLuaFSWindow") == 0) {
            lua_pushboolean(state, 1);
            return 1;
        }
        return luaL_error(state,
            "FSLuaInstall(\"%s\") is Unsupported in the native host", name);
    }

    static int widget_index(lua_State* state) {
        (void)check_widget(state);
        const char* name = lua_tostring(state, 2);
        if (name == nullptr) {
            return luaL_error(state, "FSLua widget field name must be a string");
        }
        if (std::strcmp(name, "SetShow") == 0) {
            lua_pushcfunction(state, &Impl::widget_set_show);
            return 1;
        }
        if (std::strcmp(name, "Enable") == 0) {
            lua_pushcfunction(state, &Impl::widget_enable);
            return 1;
        }
        return luaL_error(state,
            "FSLua widget method '%s' is Unsupported in the native host", name);
    }

    static int widget_set_show(lua_State* state) {
        LuaWidgetRef* self = check_widget(state);
        self->widget->set_visible(lua_toboolean(state, 2) != 0);
        lua_pushboolean(state, 1);
        return 1;
    }

    static int widget_enable(lua_State* state) {
        LuaWidgetRef* self = check_widget(state);
        self->widget->set_enabled(lua_toboolean(state, 2) != 0);
        lua_pushboolean(state, 1);
        return 1;
    }

    void install_api() {
        lua_State* state = vm_.native_handle();
        luaL_newmetatable(state, widget_metatable);
        lua_pushcfunction(state, &Impl::widget_index);
        lua_setfield(state, -2, "__index");
        lua_pop(state, 1);

        lua_newtable(state);
        lua_pushlightuserdata(state, this);
        lua_pushcclosure(state, &Impl::reg_msg_call, 1);
        lua_setfield(state, -2, "RegMsgCall");
        for (const char* name : {"RegAfterCreate", "RegBeforeRelease", "RegWinRun",
                 "RegWinDrawThis", "RegWinPreDraw", "RegWinSysClose", "LexOutput",
                 "LexGoto"}) {
            lua_pushstring(state, name);
            lua_pushcclosure(state, &Impl::unsupported_wnd_api, 1);
            lua_setfield(state, -2, name);
        }
        lua_setglobal(state, "FSLuaWndScript");

        lua_pushcfunction(state, &Impl::fs_lua_install);
        lua_setglobal(state, "FSLuaInstall");
    }

    void expose_widgets() {
        lua_State* state = vm_.native_handle();
        for (std::size_t index = 0; index < window_.widgets().size(); ++index) {
            const Widget& widget = window_.widgets()[index];
            const std::string* property = widget.node().property("LUACONTROL");
            if (property == nullptr) {
                continue;
            }
            const std::string control = scp_unquote(*property);
            if (control.empty()) {
                continue;
            }
            Widget* target = window_.widget_at(index);
            if (target == nullptr) {
                continue;
            }
            void* storage = lua_newuserdata(state, sizeof(LuaWidgetRef));
            new (storage) LuaWidgetRef{target};
            luaL_getmetatable(state, widget_metatable);
            lua_setmetatable(state, -2);
            lua_setglobal(state, control.c_str());
        }
    }

    void subscribe_messages() {
        std::unordered_set<std::string> subscribed;
        for (const UiMessage& binding : window_.message_declarations()) {
            if (callbacks_.find(binding.code) == callbacks_.end()) {
                continue;
            }
            const std::string key = binding.control + '\n' + binding.type;
            if (!subscribed.insert(key).second) {
                continue;
            }
            subscriptions_.push_back(messages_.subscribe(binding.control,
                binding.type, [this](const UiMessage& message) { dispatch(message); }));
        }
    }

    void add_callback(lua_State* state, const char* name, int reference) {
        const auto found = callbacks_.find(name);
        if (found != callbacks_.end()) {
            luaL_unref(state, LUA_REGISTRYINDEX, found->second);
            found->second = reference;
            return;
        }
        callbacks_.emplace(name, reference);
    }

    void dispatch(const UiMessage& message) {
        const auto found = callbacks_.find(message.code);
        if (found == callbacks_.end()) {
            return;
        }
        ++callback_count_;
        vm_.call(found->second);
    }

    void unsubscribe_all() noexcept {
        for (const MessageBus::Subscription token : subscriptions_) {
            messages_.unsubscribe(token);
        }
        subscriptions_.clear();
    }

    friend class FsLuaHost;

    ScpWindow& window_;
    MessageBus& messages_;
    LuaVm vm_;
    std::unordered_map<std::string, int> callbacks_;
    std::vector<MessageBus::Subscription> subscriptions_;
    std::size_t callback_count_ = 0;
};

FsLuaHost::FsLuaHost(ScpWindow& window, MessageBus& messages)
    : impl_(std::make_unique<Impl>(window, messages)) {
    try {
        impl_->initialize();
    } catch (...) {
        impl_->unsubscribe_all();
        throw;
    }
}

FsLuaHost::~FsLuaHost() = default;

std::size_t FsLuaHost::registered_callback_count() const noexcept {
    return impl_->registered_callback_count();
}

std::size_t FsLuaHost::callback_count() const noexcept {
    return impl_->callback_count();
}

LuaVm& FsLuaHost::vm() noexcept {
    return impl_->vm();
}

} // namespace dhsh
