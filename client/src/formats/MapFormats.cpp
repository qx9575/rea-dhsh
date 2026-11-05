#include "formats/MapFormats.h"

#include "resource/ResourceError.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <limits>
#include <sstream>
#include <string_view>

namespace dhsh {
namespace {

std::uint8_t byte_at(std::span<const std::byte> data, std::size_t offset) {
    return std::to_integer<std::uint8_t>(data[offset]);
}

std::uint16_t read_u16(std::span<const std::byte> data, std::size_t offset) {
    return static_cast<std::uint16_t>(byte_at(data, offset))
        | static_cast<std::uint16_t>(byte_at(data, offset + 1)) << 8;
}

std::uint32_t read_u32(std::span<const std::byte> data, std::size_t offset) {
    return static_cast<std::uint32_t>(byte_at(data, offset))
        | static_cast<std::uint32_t>(byte_at(data, offset + 1)) << 8
        | static_cast<std::uint32_t>(byte_at(data, offset + 2)) << 16
        | static_cast<std::uint32_t>(byte_at(data, offset + 3)) << 24;
}

std::int32_t read_i32(std::span<const std::byte> data, std::size_t offset) {
    return std::bit_cast<std::int32_t>(read_u32(data, offset));
}

bool has_magic(std::span<const std::byte> data, std::string_view magic) {
    if (data.size() < magic.size()) {
        return false;
    }
    for (std::size_t index = 0; index < magic.size(); ++index) {
        if (byte_at(data, index) != static_cast<std::uint8_t>(magic[index])) {
            return false;
        }
    }
    return true;
}

std::size_t checked_size(std::uint64_t value, std::string_view message) {
    if (value > std::numeric_limits<std::size_t>::max()) {
        throw FormatError(std::string(message));
    }
    return static_cast<std::size_t>(value);
}

void check_extent(std::uint64_t offset, std::uint64_t size, std::size_t total,
    std::string_view message) {
    if (offset > total || size > static_cast<std::uint64_t>(total) - offset) {
        throw FormatError(std::string(message));
    }
}

std::string fixed_string(std::span<const std::byte> data, std::size_t offset, std::size_t size) {
    std::size_t length = 0;
    while (length < size && byte_at(data, offset + length) != 0) {
        ++length;
    }
    std::string result;
    result.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        result.push_back(static_cast<char>(byte_at(data, offset + index)));
    }
    return result;
}

std::string wide_to_utf8(std::span<const wchar_t> input) {
    if (input.empty()) {
        return {};
    }
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw FormatError("MAP text is too large");
    }
    const int input_size = static_cast<int>(input.size());
    const int output_size = WideCharToMultiByte(CP_UTF8, 0, input.data(), input_size,
        nullptr, 0, nullptr, nullptr);
    if (output_size <= 0) {
        throw FormatError("cannot encode MAP text as UTF-8");
    }
    std::string output(static_cast<std::size_t>(output_size), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, input.data(), input_size,
            output.data(), output_size, nullptr, nullptr) != output_size) {
        throw FormatError("cannot encode MAP text as UTF-8");
    }
    return output;
}

std::string decode_code_page(std::string_view input, unsigned code_page, unsigned flags) {
    if (input.empty()) {
        return {};
    }
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw FormatError("MAP text is too large");
    }
    const int input_size = static_cast<int>(input.size());
    const int wide_size = MultiByteToWideChar(code_page, flags, input.data(), input_size,
        nullptr, 0);
    if (wide_size <= 0) {
        throw FormatError("cannot decode MAP text");
    }
    std::vector<wchar_t> wide(static_cast<std::size_t>(wide_size));
    if (MultiByteToWideChar(code_page, flags, input.data(), input_size,
            wide.data(), wide_size) != wide_size) {
        throw FormatError("cannot decode MAP text");
    }
    return wide_to_utf8(wide);
}

std::string decode_map_text(std::span<const std::byte> data) {
    std::string input;
    input.reserve(data.size());
    for (const std::byte value : data) {
        input.push_back(static_cast<char>(std::to_integer<std::uint8_t>(value)));
    }
    std::string_view view(input);
    if (view.size() >= 3 && static_cast<std::uint8_t>(view[0]) == 0xef
        && static_cast<std::uint8_t>(view[1]) == 0xbb
        && static_cast<std::uint8_t>(view[2]) == 0xbf) {
        view.remove_prefix(3);
    }
    try {
        return decode_code_page(view, CP_UTF8, MB_ERR_INVALID_CHARS);
    } catch (const FormatError&) {
        return decode_code_page(view, 54936, 0);
    }
}

std::string trim(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size()
        && std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

std::string ascii_case(std::string_view value, bool upper) {
    std::string result(value);
    for (char& character : result) {
        const unsigned char byte = static_cast<unsigned char>(character);
        character = static_cast<char>(upper ? std::toupper(byte) : std::tolower(byte));
    }
    return result;
}

std::int32_t adjusted_coordinate(std::int32_t value) {
    const std::int64_t adjusted = static_cast<std::int64_t>(value) - 32;
    if (adjusted < std::numeric_limits<std::int32_t>::min()) {
        throw FormatError("MLK adjusted coordinate underflows");
    }
    return static_cast<std::int32_t>(adjusted);
}

} // namespace

MapIni parse_map_ini(std::span<const std::byte> data) {
    const std::string text = decode_map_text(data);
    std::istringstream lines(text);
    MapIni result;
    std::string section;
    std::string raw;
    while (std::getline(lines, raw)) {
        std::string line = trim(raw);
        if (line.empty() || line.starts_with('#') || line.starts_with(';') || line.starts_with("//")) {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = ascii_case(trim(std::string_view(line).substr(1, line.size() - 2)), false);
            result.try_emplace(section);
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = ascii_case(trim(std::string_view(line).substr(0, equals)), true);
        result[section][key] = trim(std::string_view(line).substr(equals + 1));
    }
    return result;
}

Cel Cel::parse(std::span<const std::byte> data) {
    if (data.size() < 24 || !has_magic(data, std::string_view("CEL\0", 4))) {
        throw FormatError("not a CEL file");
    }
    Cel result;
    const std::uint32_t version = read_u32(data, 4);
    const std::uint32_t total = read_u32(data, 8);
    result.flags = read_u32(data, 12);
    result.width = read_u32(data, 16);
    result.height = read_u32(data, 20);
    if (version != 2) {
        throw FormatError("unsupported CEL version");
    }
    if (total != data.size()) {
        throw FormatError("CEL declared size does not match input");
    }
    if (result.width == 0 || result.height == 0) {
        throw FormatError("invalid CEL dimensions");
    }
    const std::uint64_t stride = (static_cast<std::uint64_t>(result.width) + 7) / 8;
    const std::uint64_t layer_bytes = stride * result.height;
    const std::size_t layer_size = checked_size(layer_bytes, "CEL bitmap is too large");
    const std::uint64_t layer_count = (result.flags & 1U) != 0 ? 2 : 1;
    check_extent(24, layer_bytes * layer_count, data.size(), "CEL bitmap is truncated");
    result.bits.assign(data.begin() + 24, data.begin() + 24 + layer_size);
    return result;
}

bool Cel::blocked(std::int32_t x, std::int32_t y) const noexcept {
    if (x < 0 || y < 0 || static_cast<std::uint32_t>(x) >= width
        || static_cast<std::uint32_t>(y) >= height) {
        return true;
    }
    const std::size_t stride = (static_cast<std::size_t>(width) + 7) / 8;
    const std::size_t index = static_cast<std::size_t>(y) * stride
        + static_cast<std::size_t>(x) / 8;
    const auto value = std::to_integer<std::uint8_t>(bits[index]);
    return ((value >> (7 - (x & 7))) & 1U) != 0;
}

Cel Cel::for_map(std::uint32_t map_width, std::uint32_t map_height) const {
    if (map_width == 0 || map_height == 0) {
        throw FormatError("invalid CEL map dimensions");
    }
    const std::uint64_t stride = (static_cast<std::uint64_t>(map_width) + 7) / 8;
    const std::size_t output_size = checked_size(stride * map_height, "CEL map bitmap is too large");
    Cel result;
    result.flags = flags & ~1U;
    result.width = map_width;
    result.height = map_height;
    result.bits.assign(output_size, std::byte{0xff});
    const std::uint32_t copy_width = std::min(width, map_width);
    const std::uint32_t copy_height = std::min(height, map_height);
    const std::size_t output_stride = static_cast<std::size_t>(stride);
    for (std::uint32_t y = 0; y < copy_height; ++y) {
        for (std::uint32_t x = 0; x < copy_width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * output_stride + x / 8;
            const std::byte mask = static_cast<std::byte>(0x80U >> (x & 7));
            if (blocked(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y))) {
                result.bits[index] |= mask;
            } else {
                result.bits[index] &= ~mask;
            }
        }
    }
    return result;
}

Man Man::parse(std::span<const std::byte> data) {
    if (data.size() < 28 || !has_magic(data, std::string_view("MAN\0", 4))) {
        throw FormatError("not a MAN file");
    }
    const std::uint32_t version = read_u32(data, 4);
    const std::uint32_t total = read_u32(data, 8);
    Man result;
    result.data_offset = read_u32(data, 16);
    result.entry_size = read_u32(data, 20);
    const std::uint32_t count = read_u32(data, 24);
    if (version != 4) {
        throw FormatError("unsupported MAN version");
    }
    if (total != data.size()) {
        throw FormatError("MAN declared size does not match input");
    }
    if (result.entry_size < 100) {
        throw FormatError("MAN entry size is too small");
    }
    check_extent(result.data_offset, static_cast<std::uint64_t>(result.entry_size) * count,
        data.size(), "MAN entries are truncated");
    result.entries.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t offset = static_cast<std::size_t>(result.data_offset)
            + static_cast<std::size_t>(index) * result.entry_size;
        ManEntry entry;
        entry.path = fixed_string(data, offset, 64);
        entry.x = read_i32(data, offset + 64);
        entry.y = read_i32(data, offset + 68);
        entry.f48 = read_i32(data, offset + 72);
        entry.f4c = read_i32(data, offset + 76);
        entry.f50 = read_i32(data, offset + 80);
        entry.f54 = read_i32(data, offset + 84);
        entry.f58 = read_i32(data, offset + 88);
        entry.f5c = read_i32(data, offset + 92);
        entry.f60 = read_i32(data, offset + 96);
        result.entries.push_back(std::move(entry));
    }
    return result;
}

Mlk Mlk::parse(std::span<const std::byte> data) {
    if (data.size() < 20 || !has_magic(data, std::string_view("lnk\0", 4))) {
        throw FormatError("not an MLK file");
    }
    const std::uint32_t version = read_u32(data, 4);
    const std::uint32_t total = read_u32(data, 8);
    const std::uint32_t count = read_u32(data, 16);
    if (version != 3) {
        throw FormatError("unsupported MLK version");
    }
    if (total != data.size()) {
        throw FormatError("MLK declared size does not match input");
    }
    if (static_cast<std::uint64_t>(count) * 108 + 20 != data.size()) {
        throw FormatError("MLK record table does not match input");
    }
    Mlk result;
    result.links.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t offset = 20 + static_cast<std::size_t>(index) * 108;
        MlkLink link;
        link.value = read_u32(data, offset);
        link.target = fixed_string(data, offset + 4, 64);
        link.paired = read_i32(data, offset + 68);
        link.x = read_i32(data, offset + 72);
        link.y = read_i32(data, offset + 76);
        link.w = read_i32(data, offset + 80);
        link.h = read_i32(data, offset + 84);
        link.dest_x = read_i32(data, offset + 88);
        link.dest_y = read_i32(data, offset + 92);
        link.extra0 = read_i32(data, offset + 96);
        link.extra1 = read_i32(data, offset + 100);
        link.extra2 = read_i32(data, offset + 104);
        if (link.w == 0) {
            link.w = 32;
            if (link.x != 0) {
                link.x = adjusted_coordinate(link.x);
            }
            link.adjusted = true;
        }
        if (link.h == 0) {
            link.h = 32;
            if (link.y != 0) {
                link.y = adjusted_coordinate(link.y);
            }
            link.adjusted = true;
        }
        result.links.push_back(std::move(link));
    }
    return result;
}

Core Core::parse(std::span<const std::byte> data) {
    if (data.size() < 40 || !has_magic(data, std::string_view("MSK\0", 4))) {
        throw FormatError("not an MSK/core file");
    }
    Core result;
    result.version = read_u32(data, 4);
    result.total_size = read_u32(data, 8);
    result.flags = read_u32(data, 12);
    result.header_size = read_u32(data, 16);
    result.record_size = read_u32(data, 20);
    const std::uint32_t record_count = read_u32(data, 24);
    result.data_offset = read_u32(data, 28);
    result.placement_size = read_u32(data, 32);
    const std::uint32_t placement_count = read_u32(data, 36);
    if (result.version != 5) {
        throw FormatError("unsupported MSK/core version");
    }
    if (result.total_size != data.size()) {
        throw FormatError("MSK/core declared size does not match input");
    }
    if (result.header_size != 40) {
        throw FormatError("unexpected MSK/core header size");
    }
    if (result.record_size < 28) {
        throw FormatError("unexpected MSK/core record size");
    }
    const std::uint64_t expected_data_offset = 40
        + static_cast<std::uint64_t>(result.record_size) * record_count;
    if (expected_data_offset != result.data_offset) {
        throw FormatError("MSK/core data offset does not match records");
    }
    if (result.placement_size != 24) {
        throw FormatError("unexpected MSK/core placement size");
    }
    const std::uint64_t placement_bytes = static_cast<std::uint64_t>(result.placement_size)
        * placement_count;
    check_extent(result.data_offset, placement_bytes, data.size(), "MSK/core placements are truncated");
    const std::uint64_t blob_offset = static_cast<std::uint64_t>(result.data_offset) + placement_bytes;

    result.records.reserve(record_count);
    for (std::uint32_t index = 0; index < record_count; ++index) {
        const std::size_t offset = 40 + static_cast<std::size_t>(index) * result.record_size;
        const std::size_t name_size = result.record_size - 28;
        CoreRecord record;
        record.name = fixed_string(data, offset, name_size);
        const std::size_t fields = offset + name_size;
        record.width = read_u32(data, fields);
        record.height = read_u32(data, fields + 4);
        record.base_y = read_i32(data, fields + 8);
        record.kind = read_u32(data, fields + 12);
        record.data_offset = read_u32(data, fields + 16);
        record.data_size = read_u32(data, fields + 20);
        record.slope = read_i32(data, fields + 24);
        if (record.data_offset < blob_offset) {
            throw FormatError("MSK/core blob overlaps the tables");
        }
        check_extent(record.data_offset, record.data_size, data.size(), "MSK/core blob overruns input");
        result.records.push_back(std::move(record));
    }

    result.placements.reserve(placement_count);
    for (std::uint32_t index = 0; index < placement_count; ++index) {
        const std::size_t offset = static_cast<std::size_t>(result.data_offset)
            + static_cast<std::size_t>(index) * result.placement_size;
        CorePlacement placement;
        placement.record = read_u32(data, offset);
        placement.x = read_i32(data, offset + 4);
        placement.y = read_i32(data, offset + 8);
        placement.flag = read_u32(data, offset + 12);
        placement.group = read_u32(data, offset + 16);
        placement.frame = read_u32(data, offset + 20);
        if (placement.record >= record_count) {
            throw FormatError("MSK/core placement references an invalid record");
        }
        result.placements.push_back(placement);
    }
    result.data_.assign(data.begin(), data.end());
    return result;
}

CoreMask Core::decode_mask(std::size_t record_index) const {
    if (record_index >= records.size()) {
        throw FormatError("MSK/core record index is out of range");
    }
    const CoreRecord& record = records[record_index];
    if (record.width == 0 || record.height == 0) {
        throw FormatError("invalid MSK/core mask dimensions");
    }
    const std::uint64_t pixel_count = static_cast<std::uint64_t>(record.width) * record.height;
    CoreMask result;
    result.width = record.width;
    result.height = record.height;
    result.pixels.assign(checked_size(pixel_count, "MSK/core mask is too large"), 0);
    if ((record.data_size & 1U) != 0) {
        throw FormatError("odd MSK/core RLE blob size");
    }

    std::uint32_t x = 0;
    std::uint32_t y = 0;
    for (std::size_t offset = record.data_offset;
         offset < static_cast<std::size_t>(record.data_offset) + record.data_size; offset += 2) {
        const std::uint16_t word = read_u16(data_, offset);
        const bool covered = (word & 0x8000U) != 0;
        const std::uint32_t length = word & 0x7fffU;
        if (length == 0) {
            throw FormatError("zero-length MSK/core RLE run");
        }
        if (y >= record.height) {
            throw FormatError("MSK/core RLE has extra rows");
        }
        if (length > record.width - x) {
            throw FormatError("MSK/core RLE run crosses a row boundary");
        }
        if (covered) {
            const std::size_t begin = static_cast<std::size_t>(y) * record.width + x;
            std::fill_n(result.pixels.begin() + begin, length, std::uint8_t{1});
        }
        x += length;
        if (x == record.width) {
            x = 0;
            ++y;
        }
    }
    if (x != 0 || y != record.height) {
        throw FormatError("MSK/core RLE rows do not match mask dimensions");
    }
    return result;
}

std::int64_t Core::sort_y(std::size_t placement_index) const {
    if (placement_index >= placements.size()) {
        throw FormatError("MSK/core placement index is out of range");
    }
    const CorePlacement& placement = placements[placement_index];
    const CoreRecord& record = records[placement.record];
    const std::int64_t product = static_cast<std::int64_t>(placement.x) * record.slope;
    return static_cast<std::int64_t>(placement.y) + record.base_y - (product >> 8);
}

} // namespace dhsh
