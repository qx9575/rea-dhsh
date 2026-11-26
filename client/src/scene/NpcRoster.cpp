#include "scene/NpcRoster.h"

#include "resource/ResourceProvider.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dhsh {
namespace {

struct NpcModelHint {
    std::string_view name;
    std::int32_t cell_x;
    std::int32_t cell_y;
    std::uint32_t shape;
    std::uint8_t direction;
};

// Captured from live Game.exe 1.50.29 NPC objects (vtable 0x984554):
// name at +4, shape at +0x28, pixel centre at +0x2a/+0x2e. One entry may match
// a CSV row by name (gatekeeper/beggar-monk/huang-wenbing) or by cell because
// the live name prefix differs from the CSV ("YingTianFu yamen runner" vs
// "runner"). Regenerate with tools/capture_npc_objects.py after visiting more
// of the map.
constexpr NpcModelHint kNpcModelHints[] = {
    {"\xE5\xAE\x88" "\xE9\x97\xA8" "\xE4\xBA\xBA", 441, 134, 10051, 1},
    {"\xE5\xBA\x94" "\xE5\xA4\xA9" "\xE5\xBA\x9C" "\xE8\xA1\x99" "\xE5\xBD\xB9",
        432, 130, 10040, 1},
    {"\xE9\x87\x91" "\xE6\x9E\xBC" "\xE5\xA7\xBF", 456, 148, 10093, 1},
    {"\xE9\xBB\x84" "\xE6\x96\x87" "\xE7\x82\xB3", 449, 140, 20113, 1},
    {"\xE5\xBA\x84" "\xE5\x9B\xAD" "\xE7\xAE\xA1" "\xE7\x90\x86" "\xE5\x91\x98",
        422, 130, 26152, 1},
    {"\xE6\x88\xBF" "\xE5\xB1\x8B" "\xE7\xAE\xA1" "\xE7\x90\x86" "\xE5\x91\x98",
        416, 127, 15026, 1},
};

char ascii_lower(char value) {
    const auto byte = static_cast<unsigned char>(value);
    if (byte >= 'A' && byte <= 'Z') {
        return static_cast<char>(byte - 'A' + 'a');
    }
    return value;
}

bool equals_ascii_case_insensitive(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view value) {
    while (!value.empty()
        && (value.front() == ' ' || value.front() == '\t'
            || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty()
        && (value.back() == ' ' || value.back() == '\t'
            || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

// CSV rows in misc/npc.csv are comma-separated; quoted fields are accepted
// even though the shipped table does not currently need them.
std::vector<std::string_view> split_csv_row(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t begin = 0;
    bool quoted = false;
    std::size_t index = 0;
    while (index <= line.size()) {
        const bool at_end = index == line.size();
        const char value = at_end ? ',' : line[index];
        if (value == '"') {
            quoted = !quoted;
        } else if (value == ',' && !quoted) {
            fields.push_back(trim(line.substr(begin, index - begin)));
            begin = index + 1;
        }
        ++index;
    }
    return fields;
}

std::optional<std::int32_t> parse_int(std::string_view text) {
    text = trim(text);
    if (text.empty()) {
        return std::nullopt;
    }
    std::int32_t value = 0;
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::string shape_path(std::uint32_t shape, std::uint32_t direction) {
    return std::to_string(shape) + "/10/"
        + std::to_string(direction) + ".apg";
}

bool model_available(ResourceProvider& resources, std::uint32_t shape) {
    return resources.exists(shape_path(shape, 3))
        || resources.exists(shape_path(shape, 1));
}

const NpcModelHint* find_hint(const NpcSpawn& spawn) {
    for (const NpcModelHint& hint : kNpcModelHints) {
        if (hint.name == spawn.name) {
            return &hint;
        }
    }
    for (const NpcModelHint& hint : kNpcModelHints) {
        if (hint.cell_x == spawn.cell_x && hint.cell_y == spawn.cell_y) {
            return &hint;
        }
    }
    return nullptr;
}

} // namespace

std::string gbk_to_utf8(std::string_view gbk) {
#ifdef _WIN32
    if (gbk.empty()) {
        return {};
    }
    const int wide_length = MultiByteToWideChar(936, 0, gbk.data(),
        static_cast<int>(gbk.size()), nullptr, 0);
    if (wide_length <= 0) {
        return std::string(gbk);
    }
    std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
    MultiByteToWideChar(936, 0, gbk.data(), static_cast<int>(gbk.size()),
        wide.data(), wide_length);
    const int utf8_length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
        wide_length, nullptr, 0, nullptr, nullptr);
    if (utf8_length <= 0) {
        return std::string(gbk);
    }
    std::string utf8(static_cast<std::size_t>(utf8_length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length,
        utf8.data(), utf8_length, nullptr, nullptr);
    return utf8;
#else
    return std::string(gbk);
#endif
}

std::vector<NpcSpawn> load_npc_roster(
    std::string_view csv_text, std::string_view map_file) {
    std::vector<NpcSpawn> spawns;
    std::size_t line_begin = 0;
    while (line_begin <= csv_text.size()) {
        const std::size_t line_end = csv_text.find('\n', line_begin);
        const std::size_t end = line_end == std::string_view::npos
            ? csv_text.size() : line_end;
        const std::string_view line = trim(csv_text.substr(line_begin, end - line_begin));
        line_begin = end + 1;
        if (line.empty() || line.front() == '#') {
            if (line_end == std::string_view::npos) {
                break;
            }
            continue;
        }
        const std::vector<std::string_view> fields = split_csv_row(line);
        if (fields.size() < 6
            || !equals_ascii_case_insensitive(fields[3], map_file)) {
            if (line_end == std::string_view::npos) {
                break;
            }
            continue;
        }
        const std::optional<std::int32_t> id = parse_int(fields[0]);
        const std::optional<std::int32_t> x = parse_int(fields[4]);
        const std::optional<std::int32_t> y = parse_int(fields[5]);
        if (!id.has_value() || !x.has_value() || !y.has_value()) {
            if (line_end == std::string_view::npos) {
                break;
            }
            continue;
        }
        NpcSpawn spawn;
        spawn.id = *id;
        spawn.name = gbk_to_utf8(fields[1]);
        spawn.cell_x = *x;
        spawn.cell_y = *y;
        if (fields.size() > 9) {
            if (const std::optional<std::int32_t> category = parse_int(fields[9]);
                category.has_value()) {
                spawn.category = *category;
            }
        }
        spawns.push_back(std::move(spawn));
        if (line_end == std::string_view::npos) {
            break;
        }
    }
    return spawns;
}

std::uint32_t resolve_npc_model(const NpcSpawn& spawn,
    ResourceProvider& npc_resources, bool* hint_matched) {
    if (hint_matched != nullptr) {
        *hint_matched = false;
    }
    if (const NpcModelHint* hint = find_hint(spawn); hint != nullptr) {
        if (model_available(npc_resources, hint->shape)) {
            if (hint_matched != nullptr) {
                *hint_matched = true;
            }
            return hint->shape;
        }
    }
    std::vector<std::uint32_t> candidates;
    const auto add_candidate = [&candidates](std::int64_t shape) {
        if (shape < 0 || shape > 60001) {
            return;
        }
        const auto value = static_cast<std::uint32_t>(shape);
        if (std::find(candidates.begin(), candidates.end(), value)
            == candidates.end()) {
            candidates.push_back(value);
        }
    };
    add_candidate(10000 + spawn.id);
    add_candidate(10000 + (spawn.id % 1000));
    add_candidate(10000 + (spawn.id % 100));
    for (const std::uint32_t candidate : candidates) {
        if (model_available(npc_resources, candidate)) {
            return candidate;
        }
    }
    return 10009;
}

bool npc_has_live_hint(const NpcSpawn& spawn) {
    return find_hint(spawn) != nullptr;
}

Direction npc_hint_direction(const NpcSpawn& spawn) {
    if (const NpcModelHint* hint = find_hint(spawn); hint != nullptr) {
        if (hint->direction <= 7) {
            return static_cast<Direction>(hint->direction);
        }
    }
    return Direction::right_down;
}

} // namespace dhsh
