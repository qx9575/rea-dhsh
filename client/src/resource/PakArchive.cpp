#include "resource/PakArchive.h"

#include <array>
#include <limits>
#include <utility>

namespace dhsh {
namespace {

constexpr std::uint32_t max_entries = 200000;
constexpr std::uint32_t max_depth = 8;
constexpr std::uint64_t header_size = 16;
constexpr std::uint64_t record_size = 48;
constexpr std::array<char, 8> magic = {'P', 'A', 'K', 'F', '3', '0', '0', '1'};

std::uint64_t checked_add(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        throw FormatError("archive offset overflow");
    }
    return left + right;
}

void check_range(std::uint64_t file_size, std::uint64_t offset, std::uint64_t size) {
    if (offset > file_size || size > file_size - offset) {
        throw FormatError("archive entry is out of bounds");
    }
}

std::string normalize_path(const std::string& path) {
    std::string normalized;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = path.find_first_of("/\\", start);
        const std::size_t length = end == std::string::npos ? path.size() - start : end - start;
        const std::string component = path.substr(start, length);
        if (component == "..") {
            throw FormatError("unsafe archive path: " + path);
        }
        if (!component.empty() && component != ".") {
            if (!normalized.empty()) {
                normalized.push_back('/');
            }
            normalized += component;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    if (normalized.empty()) {
        throw FormatError("empty archive path");
    }
    return normalized;
}

std::string decode_name(const std::vector<std::byte>& record) {
    std::string name;
    for (std::size_t index = 0; index < 32 && record[index] != std::byte{0}; ++index) {
        name.push_back(static_cast<char>(record[index]));
    }
    return name;
}

std::uint32_t decode_u32(const std::vector<std::byte>& record, std::size_t offset) {
    return static_cast<std::uint32_t>(record[offset])
        | (static_cast<std::uint32_t>(record[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(record[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(record[offset + 3]) << 24);
}

} // namespace

PakArchive::PakArchive(std::filesystem::path file) : file_(std::move(file)), reader_(file_) {
    walk(0, "", 0);
}

const std::vector<PakEntry>& PakArchive::entries() const {
    return entries_;
}

std::vector<std::byte> PakArchive::read(const PakEntry& entry) const {
    bool belongs = false;
    for (const auto& candidate : entries_) {
        if (&candidate == &entry) {
            belongs = true;
            break;
        }
    }
    if (!belongs) {
        throw FormatError("entry does not belong to this archive");
    }
    const ByteReader current_reader(file_);
    return current_reader.read(entry.offset, entry.size);
}

bool PakArchive::has_magic(std::uint64_t base) const {
    if (base > reader_.size() || reader_.size() - base < magic.size()) {
        return false;
    }
    const auto bytes = reader_.read(base, magic.size());
    for (std::size_t index = 0; index < magic.size(); ++index) {
        if (bytes[index] != static_cast<std::byte>(magic[index])) {
            return false;
        }
    }
    return true;
}

void PakArchive::walk(std::uint64_t base, const std::string& base_path, std::uint32_t depth) {
    const auto header = reader_.read(base, header_size);
    for (std::size_t index = 0; index < magic.size(); ++index) {
        if (header[index] != static_cast<std::byte>(magic[index])) {
            throw FormatError("invalid PAKF magic");
        }
    }

    const std::uint32_t count = reader_.read_u32(checked_add(base, 12));
    if (count > max_entries) {
        throw FormatError("too many PAKF entries");
    }

    const std::uint64_t table = checked_add(base, header_size);
    const std::uint64_t table_size = static_cast<std::uint64_t>(count) * record_size;
    check_range(reader_.size(), table, table_size);

    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint64_t record_offset = checked_add(table, static_cast<std::uint64_t>(index) * record_size);
        const auto record = reader_.read(record_offset, record_size);
        const std::string name = decode_name(record);
        const std::uint32_t offset = decode_u32(record, 32);
        const std::uint32_t size = decode_u32(record, 36);
        const std::uint32_t kind = decode_u32(record, 40);
        const std::string path = normalize_path(base_path.empty() ? name : base_path + "/" + name);
        const std::uint64_t payload_offset = checked_add(base, offset);

        if (kind == 1 && has_magic(payload_offset)) {
            if (depth >= max_depth) {
                throw FormatError("PAKF nesting exceeds maximum depth");
            }
            walk(payload_offset, path, depth + 1);
            continue;
        }

        check_range(reader_.size(), payload_offset, size);
        if (payload_offset > std::numeric_limits<std::uint32_t>::max()) {
            throw FormatError("absolute PAKF entry offset is too large");
        }
        entries_.push_back(PakEntry{
            name,
            path,
            static_cast<std::uint32_t>(payload_offset),
            size,
            kind,
            depth,
        });
    }
}

} // namespace dhsh
