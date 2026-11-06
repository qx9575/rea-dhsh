#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dhsh {

struct ApciItem {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::size_t blob_offset = 0;
    std::size_t blob_size = 0;

    std::uint16_t rows() const noexcept {
        return static_cast<std::uint16_t>(x & 0xffffU);
    }

    std::uint8_t cell_size() const noexcept {
        return static_cast<std::uint8_t>((x >> 24) & 0xffU);
    }

    std::uint8_t layout() const noexcept {
        return static_cast<std::uint8_t>((y >> 16) & 0xffU);
    }
};

struct ApciMid {
    std::uint32_t header = 0;
    std::vector<ApciItem> items;

    std::uint16_t action_id() const noexcept {
        return static_cast<std::uint16_t>(header & 0xffffU);
    }

    std::uint16_t direction_count() const noexcept {
        return static_cast<std::uint16_t>(header >> 16);
    }
};

struct ApciEntry {
    std::uint32_t model_id = 0;
    std::uint32_t raw_count = 0;
    std::vector<ApciMid> mids;
};

struct ApciRecord {
    std::uint16_t record_id = 0;
    std::string name;
    std::vector<ApciEntry> entries;
};

struct ApciGroup {
    std::uint16_t group_id = 0;
    std::vector<ApciRecord> records;
};

struct ApciFile {
    std::uint32_t version = 0;
    std::size_t consumed = 0;
    std::vector<ApciGroup> groups;

    static ApciFile parse(std::span<const std::byte> data);
};

} // namespace dhsh
