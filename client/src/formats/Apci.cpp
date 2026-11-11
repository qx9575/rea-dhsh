#include "formats/Apci.h"

#include "resource/ResourceError.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace dhsh {
namespace {

constexpr std::size_t max_groups = 20;
constexpr std::size_t record_size = 36;

std::uint8_t byte_at(std::span<const std::byte> data, std::size_t offset) {
    return std::to_integer<std::uint8_t>(data[offset]);
}

class Cursor {
public:
    explicit Cursor(std::span<const std::byte> data) : data_(data) {
    }

    std::size_t position() const noexcept {
        return position_;
    }

    void require(std::size_t size, const std::string& what) const {
        if (position_ > data_.size() || size > data_.size() - position_) {
            throw FormatError("truncated APCI " + what);
        }
    }

    std::uint16_t u16(const std::string& what) {
        require(2, what);
        const std::uint16_t result = static_cast<std::uint16_t>(byte_at(data_, position_))
            | static_cast<std::uint16_t>(byte_at(data_, position_ + 1)) << 8;
        position_ += 2;
        return result;
    }

    std::uint32_t u32(const std::string& what) {
        require(4, what);
        const std::uint32_t result = static_cast<std::uint32_t>(byte_at(data_, position_))
            | static_cast<std::uint32_t>(byte_at(data_, position_ + 1)) << 8
            | static_cast<std::uint32_t>(byte_at(data_, position_ + 2)) << 16
            | static_cast<std::uint32_t>(byte_at(data_, position_ + 3)) << 24;
        position_ += 4;
        return result;
    }

    std::string name() {
        require(32, "record name");
        std::string result;
        for (std::size_t index = 0; index < 32; ++index) {
            const std::byte value = data_[position_ + index];
            if (value == std::byte{0}) {
                break;
            }
            result.push_back(static_cast<char>(value));
        }
        position_ += 32;
        return result;
    }

    void skip(std::size_t size, const std::string& what) {
        require(size, what);
        position_ += size;
    }

private:
    std::span<const std::byte> data_;
    std::size_t position_ = 0;
};

std::size_t blob_size(std::uint32_t x, std::uint32_t y) {
    const std::uint32_t rows = x & 0xffffU;
    if (rows == 0) {
        return 0;
    }
    const std::uint32_t cell_size = (x >> 24) & 0xffU;
    if (((y >> 16) & 0xffU) == 1) {
        return cell_size;
    }
    return (cell_size * rows) & 0xffffU;
}

} // namespace

ApciFile ApciFile::parse(std::span<const std::byte> data) {
    if (data.size() < 8 || byte_at(data, 0) != 'A' || byte_at(data, 1) != 'P'
        || byte_at(data, 2) != 'C' || byte_at(data, 3) != 'I') {
        throw FormatError("missing APCI magic");
    }
    Cursor cursor(data);
    cursor.skip(4, "magic");
    ApciFile result;
    result.version = cursor.u32("version");
    if (result.version != 0x101) {
        throw FormatError("unsupported APCI version");
    }

    while (cursor.position() < data.size() && result.groups.size() < max_groups) {
        ApciGroup group;
        group.group_id = cursor.u16("group header");
        const std::uint16_t record_count = cursor.u16("group header");
        group.records.reserve(record_count);
        for (std::uint16_t record_index = 0; record_index < record_count; ++record_index) {
            cursor.require(record_size, "record");
            ApciRecord record;
            record.record_id = cursor.u16("record");
            const std::uint16_t entry_count = cursor.u16("record");
            record.name = cursor.name();
            record.entries.reserve(entry_count);
            for (std::uint16_t entry_index = 0; entry_index < entry_count; ++entry_index) {
                ApciEntry entry;
                entry.model_id = cursor.u32("entry");
                entry.raw_count = cursor.u32("entry");
                const std::uint16_t mid_count = static_cast<std::uint16_t>(entry.raw_count & 0xffffU);
                entry.mids.reserve(mid_count);
                for (std::uint16_t mid_index = 0; mid_index < mid_count; ++mid_index) {
                    ApciMid mid;
                    mid.header = cursor.u32("mid");
                    const std::uint16_t item_count = mid.direction_count();
                    mid.items.reserve(item_count);
                    for (std::uint16_t item_index = 0; item_index < item_count; ++item_index) {
                        ApciItem item;
                        item.x = cursor.u32("item");
                        item.y = cursor.u32("item");
                        item.blob_offset = cursor.position();
                        item.blob_size = blob_size(item.x, item.y);
                        cursor.skip(item.blob_size, "blob");
                        mid.items.push_back(item);
                    }
                    entry.mids.push_back(std::move(mid));
                }
                record.entries.push_back(std::move(entry));
            }
            group.records.push_back(std::move(record));
        }
        result.groups.push_back(std::move(group));
    }
    result.consumed = cursor.position();
    if (result.consumed != data.size()) {
        throw FormatError("trailing bytes after APCI groups");
    }
    return result;
}

} // namespace dhsh
