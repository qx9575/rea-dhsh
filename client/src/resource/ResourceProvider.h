#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace dhsh {

class ResourceProvider {
public:
    virtual ~ResourceProvider() = default;

    virtual std::vector<std::byte> bytes(std::string_view logical_path) = 0;
    virtual bool exists(std::string_view logical_path) const = 0;
};

} // namespace dhsh
