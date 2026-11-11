#include "script/LuaVm.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <new>
#include <stdexcept>
#include <string>

namespace dhsh {
namespace {

[[noreturn]] void throw_lua_error(lua_State* state, std::string_view kind) {
    const char* detail = lua_tostring(state, -1);
    std::string message = "Lua " + std::string(kind) + ": "
        + (detail != nullptr ? detail : "unknown error");
    lua_pop(state, 1);
    throw std::runtime_error(message);
}

struct StackGuard {
    lua_State* state;
    int top;

    ~StackGuard() {
        lua_settop(state, top);
    }
};

struct OpenLibraryRequest {
    lua_CFunction open;
    const char* name;
};

int open_library_entry(lua_State* state) {
    auto* request = static_cast<OpenLibraryRequest*>(lua_touserdata(state, 1));
    lua_pushcfunction(state, request->open);
    lua_pushstring(state, request->name);
    lua_call(state, 1, 0);
    return 0;
}

void open_library(lua_State* state, lua_CFunction open, const char* name) {
    StackGuard stack{state, lua_gettop(state)};
    OpenLibraryRequest request{open, name};
    if (lua_cpcall(state, &open_library_entry, &request) != 0) {
        throw_lua_error(state, "library initialization error");
    }
}

} // namespace

struct LuaVm::Impl {
    lua_State* state = nullptr;

    ~Impl() {
        if (state != nullptr) {
            lua_close(state);
        }
    }
};

LuaVm::LuaVm() : LuaVm(nullptr, nullptr) {}

LuaVm::LuaVm(Allocator allocator, void* userdata) : impl_(std::make_unique<Impl>()) {
    impl_->state = allocator != nullptr
        ? lua_newstate(allocator, userdata) : luaL_newstate();
    if (impl_->state == nullptr) {
        throw std::bad_alloc();
    }
    open_library(impl_->state, luaopen_base, "");
    open_library(impl_->state, luaopen_table, LUA_TABLIBNAME);
    open_library(impl_->state, luaopen_string, LUA_STRLIBNAME);
    open_library(impl_->state, luaopen_math, LUA_MATHLIBNAME);
}

LuaVm::~LuaVm() = default;

void LuaVm::execute(std::string_view source, const std::string& chunk_name) {
    StackGuard stack{impl_->state, lua_gettop(impl_->state)};
    const std::string name = "@" + chunk_name;
    const int status = luaL_loadbuffer(impl_->state, source.data(), source.size(),
        name.c_str());
    if (status != 0) {
        throw_lua_error(impl_->state, "syntax error");
    }
    if (lua_pcall(impl_->state, 0, 0, 0) != 0) {
        throw_lua_error(impl_->state, "runtime error");
    }
}

int LuaVm::reference_function(std::string_view name) {
    StackGuard stack{impl_->state, lua_gettop(impl_->state)};
    const std::string callback_name(name);
    lua_getglobal(impl_->state, callback_name.c_str());
    if (!lua_isfunction(impl_->state, -1)) {
        throw std::runtime_error("Lua callback is not a function: " + callback_name);
    }
    return luaL_ref(impl_->state, LUA_REGISTRYINDEX);
}

void LuaVm::call(int reference) {
    StackGuard stack{impl_->state, lua_gettop(impl_->state)};
    lua_rawgeti(impl_->state, LUA_REGISTRYINDEX, reference);
    if (!lua_isfunction(impl_->state, -1)) {
        throw std::runtime_error("Lua callback reference is not a function");
    }
    // The original bulletin callbacks are declared as function(p1, p2) but the
    // observed script bodies ignore both parameters. Their real semantics stay
    // Unknown, so call with no arguments instead of inventing values.
    if (lua_pcall(impl_->state, 0, 0, 0) != 0) {
        throw_lua_error(impl_->state, "callback error");
    }
}

lua_State* LuaVm::native_handle() const noexcept {
    return impl_->state;
}

} // namespace dhsh
