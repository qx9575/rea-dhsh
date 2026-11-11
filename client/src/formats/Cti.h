#pragma once

#include "render/Image.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dhsh {

enum class CtiTable {
    table1 = 1,
    table2 = 2,
};

struct CtiHeader {
    std::uint16_t header_size = 0;
    std::uint16_t count = 0;
    std::uint16_t bpp = 0;
    std::uint32_t format = 0;
    std::uint32_t table1_size = 0;
    std::uint32_t table2_size = 0;
};

struct CtiPalette {
    CtiHeader header;
    std::vector<std::uint16_t> table1;
    std::vector<std::uint32_t> table2;

    static CtiPalette parse(std::span<const std::byte> data);

    Image indexed_image(std::span<const std::byte> indices,
        std::uint32_t width, std::uint32_t height,
        CtiTable table = CtiTable::table1,
        std::span<const std::byte> alpha = {}) const;
};

} // namespace dhsh
