#pragma once

#include "render/Image.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dhsh {

struct CtiPalette;

struct AgIndexEntry {
    std::uint16_t frame_id = 0;
    std::uint16_t flags = 0;
    std::uint32_t delta = 0;
};

struct AgHeader {
    std::uint16_t header_size = 0;
    std::uint16_t frame_count = 0;
    std::uint16_t flags = 0;
    std::uint32_t reserved = 0;
    std::vector<AgIndexEntry> entries;
};

struct ApgFrame {
    std::size_t index = 0;
    std::size_t offset = 0;
    std::int16_t anchor_x = 0;
    std::int16_t anchor_y = 0;
    Image image;
};

struct ApgAnimation {
    AgHeader header;
    std::vector<ApgFrame> frames;
};

class ApgDecoder {
public:
    static ApgAnimation decode(std::span<const std::byte> data,
        const CtiPalette* palette = nullptr);
};

} // namespace dhsh
