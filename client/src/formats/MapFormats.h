#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace dhsh {

using MapIni = std::map<std::string, std::map<std::string, std::string>>;

MapIni parse_map_ini(std::span<const std::byte> data);

struct Cel {
    std::uint32_t flags = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::byte> bits;

    static Cel parse(std::span<const std::byte> data);
    bool blocked(std::int32_t x, std::int32_t y) const noexcept;
    Cel for_map(std::uint32_t map_width, std::uint32_t map_height) const;
};

struct ManEntry {
    std::string path;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t f48 = 0;
    std::int32_t f4c = 0;
    std::int32_t f50 = 0;
    std::int32_t f54 = 0;
    std::int32_t f58 = 0;
    std::int32_t f5c = 0;
    std::int32_t f60 = 0;
};

struct Man {
    std::uint32_t data_offset = 0;
    std::uint32_t entry_size = 0;
    std::vector<ManEntry> entries;

    static Man parse(std::span<const std::byte> data);
};

struct MlkLink {
    std::uint32_t value = 0;
    std::string target;
    std::int32_t paired = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 0;
    std::int32_t h = 0;
    std::int32_t dest_x = 0;
    std::int32_t dest_y = 0;
    std::int32_t extra0 = 0;
    std::int32_t extra1 = 0;
    std::int32_t extra2 = 0;
    bool adjusted = false;
};

struct Mlk {
    std::vector<MlkLink> links;

    static Mlk parse(std::span<const std::byte> data);
};

struct CoreRecord {
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t base_y = 0;
    std::uint32_t kind = 0;
    std::uint32_t data_offset = 0;
    std::uint32_t data_size = 0;
    std::int32_t slope = 0;
};

struct CorePlacement {
    std::uint32_t record = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t flag = 0;
    std::uint32_t group = 0;
    std::uint32_t frame = 0;
};

struct CoreMask {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

struct Core {
    std::uint32_t version = 0;
    std::uint32_t total_size = 0;
    std::uint32_t flags = 0;
    std::uint32_t header_size = 0;
    std::uint32_t record_size = 0;
    std::uint32_t data_offset = 0;
    std::uint32_t placement_size = 0;
    std::vector<CoreRecord> records;
    std::vector<CorePlacement> placements;

    static Core parse(std::span<const std::byte> data);
    CoreMask decode_mask(std::size_t record_index) const;
    std::int64_t sort_y(std::size_t placement_index) const;

private:
    std::vector<std::byte> data_;
};

} // namespace dhsh
