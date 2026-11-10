#pragma once

#include "script/LuaVm.h"

#include <cstddef>
#include <memory>

namespace dhsh {

class MessageBus;
class ScpWindow;

class FsLuaHost {
public:
    FsLuaHost(ScpWindow& window, MessageBus& messages,
        LuaVm::Allocator allocator = nullptr, void* userdata = nullptr);
    ~FsLuaHost();

    FsLuaHost(const FsLuaHost&) = delete;
    FsLuaHost& operator=(const FsLuaHost&) = delete;

    std::size_t registered_callback_count() const noexcept;
    std::size_t callback_count() const noexcept;
    LuaVm& vm() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dhsh
