#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace dhsh {

class NzDecoder {
public:
    static std::vector<std::byte> decode(std::span<const std::byte> data);
};

} // namespace dhsh
