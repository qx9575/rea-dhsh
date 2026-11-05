#include "resource/ByteReader.h"

#include <limits>
#include <string>
#include <utility>

namespace dhsh {

ByteReader::ByteReader(std::filesystem::path file)
    : file_(std::move(file)), stream_(file_, std::ios::binary | std::ios::ate) {
    if (!stream_) {
        throw FormatError("cannot open archive: " + file_.string());
    }

    const std::streampos end = stream_.tellg();
    if (end < 0) {
        throw FormatError("cannot determine archive size: " + file_.string());
    }
    size_ = static_cast<std::uint64_t>(end);
}

std::uint64_t ByteReader::size() const noexcept {
    return size_;
}

std::vector<std::byte> ByteReader::read(std::uint64_t offset, std::uint64_t size) const {
    if (offset > size_ || size > size_ - offset
        || size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
        || size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        throw FormatError("archive read is out of bounds");
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (size == 0) {
        return bytes;
    }

    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream_) {
        throw FormatError("cannot seek within archive");
    }
    stream_.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!stream_) {
        throw FormatError("archive data is truncated");
    }
    return bytes;
}

std::uint32_t ByteReader::read_u32(std::uint64_t offset) const {
    const auto bytes = read(offset, 4);
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

} // namespace dhsh
