#pragma once

#include <memory>
#include <string>
#include <string_view>

struct lua_State;

namespace dhsh {

class LuaVm {
public:
    LuaVm();
    ~LuaVm();

    LuaVm(const LuaVm&) = delete;
    LuaVm& operator=(const LuaVm&) = delete;

    void execute(std::string_view source, const std::string& chunk_name);
    int reference_function(std::string_view name);
    void call(int reference);
    lua_State* native_handle() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dhsh
