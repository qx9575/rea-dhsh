#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace dhsh {

class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ByteReader {
public:
    explicit ByteReader(std::filesystem::path file);

    std::uint64_t size() const noexcept;
    std::vector<std::byte> read(std::uint64_t offset, std::uint64_t size) const;
    std::uint32_t read_u32(std::uint64_t offset) const;

private:
    std::filesystem::path file_;
    mutable std::ifstream stream_;
    std::uint64_t size_;
};

} // namespace dhsh
