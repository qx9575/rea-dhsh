#pragma once

#include "resource/ByteReader.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dhsh {

struct PakEntry {
    std::string name;
    std::string path;
    std::uint32_t offset;
    std::uint32_t size;
    std::uint32_t kind;
    std::uint32_t depth;
};

class PakArchive {
public:
    explicit PakArchive(std::filesystem::path file);

    const std::vector<PakEntry>& entries() const;
    std::vector<std::byte> read(const PakEntry& entry) const;

private:
    void walk(
        std::uint64_t base,
        const std::string& base_path,
        std::uint32_t depth,
        std::uint64_t extent);
    bool has_magic(std::uint64_t base) const;

    std::filesystem::path file_;
    ByteReader reader_;
    std::vector<PakEntry> entries_;
};

} // namespace dhsh
