#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dhsh {

struct Pif {
    std::uint32_t count = 0;
    std::uint16_t data_offset = 0;
    std::uint16_t height = 0;
    std::uint16_t width = 0;
    std::uint16_t flags = 0;
    std::uint32_t reserved = 0;
    std::vector<std::array<std::uint16_t, 14>> rows;

    static Pif parse(std::span<const std::byte> data);
};

} // namespace dhsh
