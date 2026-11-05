#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dhsh {

struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::byte> rgba;
};

} // namespace dhsh
