#include "avatar/Avatar.h"
#include "avatar/AvatarCatalog.h"
#include "formats/Apci.h"
#include "render/Image.h"
#include "resource/PakResourceProvider.h"
#include "resource/ResourceError.h"
#include "resource/ResourceProvider.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    result.reserve(values.size());
    for (const unsigned value : values) {
        check(value <= 0xffU, "test byte is out of range");
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

void append_u16(Bytes& output, std::uint16_t value) {
    output.push_back(static_cast<std::byte>(value & 0xffU));
    output.push_back(static_cast<std::byte>(value >> 8));
}

void append_u32(Bytes& output, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_text(Bytes& output, std::string_view value) {
    for (const char character : value) {
        output.push_back(static_cast<std::byte>(character));
    }
}

Bytes apci_header() {
    Bytes result;
    append_text(result, "APCI");
    append_u32(result, 0x101);
    return result;
}

void append_group_header(Bytes& output, std::uint16_t id, std::uint16_t records) {
    append_u16(output, id);
    append_u16(output, records);
}

void append_record(Bytes& output, std::string_view name, std::uint16_t id,
    std::uint16_t entries) {
    append_u16(output, id);
    append_u16(output, entries);
    check(name.size() <= 32, "APCI test record name is too long");
    append_text(output, name);
    output.resize(output.size() + 32 - name.size(), std::byte{0});
}

void append_entry(Bytes& output, std::uint32_t model, std::uint32_t raw_count) {
    append_u32(output, model);
    append_u32(output, raw_count);
}

void append_mid(Bytes& output, std::uint32_t header) {
    append_u32(output, header);
}

void append_item(Bytes& output, std::uint32_t x, std::uint32_t y,
    const Bytes& blob = {}) {
    append_u32(output, x);
    append_u32(output, y);
    output.insert(output.end(), blob.begin(), blob.end());
}

template <typename Callback>
void expect_error(Callback&& callback, const std::string& message,
    std::string_view text = {}) {
    try {
        std::forward<Callback>(callback)();
    } catch (const dhsh::FormatError& error) {
        if (!text.empty()) {
            check(std::string_view(error.what()).find(text) != std::string_view::npos,
                message + ": unclear error: " + error.what());
        }
        return;
    } catch (const std::exception& error) {
        throw std::runtime_error(message + ": wrong exception: " + error.what());
    }
    throw std::runtime_error(message + ": no exception");
}

void test_apci_empty_and_twenty_group_boundary() {
    const dhsh::ApciFile empty = dhsh::ApciFile::parse(apci_header());
    check(empty.version == 0x101 && empty.groups.empty() && empty.consumed == 8,
        "empty APCI header was not consumed exactly");

    Bytes twenty = apci_header();
    for (std::uint16_t group = 0; group < 20; ++group) {
        append_group_header(twenty, group, 0);
    }
    const dhsh::ApciFile parsed = dhsh::ApciFile::parse(twenty);
    check(parsed.groups.size() == 20 && parsed.consumed == twenty.size(),
        "20 empty APCI groups must be accepted");
    check(parsed.groups.front().group_id == 0 && parsed.groups.back().group_id == 19,
        "APCI group IDs changed");

    Bytes twenty_one = twenty;
    append_group_header(twenty_one, 20, 0);
    expect_error([&] { static_cast<void>(dhsh::ApciFile::parse(twenty_one)); },
        "21st APCI group must remain unconsumed", "trailing");
}

void test_apci_entry_mid_and_item_layout() {
    Bytes data = apci_header();
    append_group_header(data, 7, 1);
    append_record(data, "player\\body", 42, 1);
    append_entry(data, 200, 0xabcd0001U);
    append_mid(data, 0x00021234U);
    append_item(data, 0, 0);
    append_item(data, 0x01000003U, 0x00010000U, bytes({9}));
    const dhsh::ApciFile parsed = dhsh::ApciFile::parse(data);
    check(parsed.consumed == data.size() && parsed.groups.size() == 1,
        "8-byte APCI entry layout was not consumed exactly");
    const auto& record = parsed.groups[0].records[0];
    check(record.record_id == 42 && record.name == "player\\body",
        "36-byte APCI record fields changed");
    const auto& entry = record.entries[0];
    check(entry.model_id == 200 && entry.raw_count == 0xabcd0001U
            && entry.mids.size() == 1,
        "APCI mid count must come from entry.b low 16 bits");
    const auto& mid = entry.mids[0];
    check(mid.action_id() == 0x1234 && mid.direction_count() == 2
            && mid.items.size() == 2,
        "APCI action ID must come from mid.header low 16 bits");
    check(mid.items[0].blob_size == 0 && mid.items[1].blob_size == 1,
        "APCI item blob sizes changed");
    check(data[mid.items[1].blob_offset] == std::byte{9},
        "APCI blob offset does not address the stored byte");
}

void test_apci_blob_size_rules_and_u16_truncation() {
    Bytes data = apci_header();
    append_group_header(data, 0, 1);
    append_record(data, "player\\body", 0, 1);
    append_entry(data, 200, 1);
    append_mid(data, 5U << 16);
    append_item(data, 0x00000000U, 0, bytes({}));
    append_item(data, 0x03000002U, 0x00010000U, bytes({1, 2, 3}));
    append_item(data, 0x03000002U, 0, bytes({1, 2, 3, 4, 5, 6}));
    append_item(data, 0x02008000U, 0, bytes({}));
    append_item(data, 0x02008001U, 0, bytes({0x5a, 0xa5}));
    const dhsh::ApciFile parsed = dhsh::ApciFile::parse(data);
    const auto& items = parsed.groups[0].records[0].entries[0].mids[0].items;
    check(items.size() == 5 && parsed.consumed == data.size(),
        "APCI blob truncation changed file consumption");
    check(items[0].blob_size == 0, "zero-width APCI item must not carry a blob");
    check(items[1].blob_size == 3, "one-dimensional APCI blob must use cell size");
    check(items[2].blob_size == 6, "matrix APCI blob must use cell size times rows");
    check(items[3].blob_size == 0 && items[4].blob_size == 2,
        "APCI matrix blob length must be truncated to 16 bits");
    check(data[items[4].blob_offset] == std::byte{0x5a}
            && data[items[4].blob_offset + 1] == std::byte{0xa5},
        "truncated APCI blob consumed the wrong bytes");
}

void test_apci_rejects_malformed_or_unconsumed_input() {
    expect_error([] { static_cast<void>(dhsh::ApciFile::parse(Bytes{})); },
        "empty input");
    Bytes bad = apci_header();
    bad[0] = std::byte{'X'};
    expect_error([&] { static_cast<void>(dhsh::ApciFile::parse(bad)); },
        "bad APCI magic", "APCI");
    bad = apci_header();
    bad[4] = std::byte{2};
    expect_error([&] { static_cast<void>(dhsh::ApciFile::parse(bad)); },
        "bad APCI version", "version");
    bad = apci_header();
    append_group_header(bad, 0, 1);
    expect_error([&] { static_cast<void>(dhsh::ApciFile::parse(bad)); },
        "truncated APCI record", "record");
    bad = apci_header();
    append_group_header(bad, 0, 1);
    append_record(bad, "player\\body", 0, 1);
    append_u32(bad, 200);
    expect_error([&] { static_cast<void>(dhsh::ApciFile::parse(bad)); },
        "truncated APCI entry", "entry");
    bad = apci_header();
    append_group_header(bad, 0, 1);
    append_record(bad, "player\\body", 0, 1);
    append_entry(bad, 200, 1);
    expect_error([&] { static_cast<void>(dhsh::ApciFile::parse(bad)); },
        "truncated APCI mid", "mid");
    append_mid(bad, 1U << 16);
    expect_error([&] { static_cast<void>(dhsh::ApciFile::parse(bad)); },
        "truncated APCI item", "item");
    append_item(bad, 0x01000002U, 0x00010000U, bytes({}));
    expect_error([&] { static_cast<void>(dhsh::ApciFile::parse(bad)); },
        "truncated APCI blob", "blob");
    bad = apci_header();
    bad.push_back(std::byte{0});
    expect_error([&] { static_cast<void>(dhsh::ApciFile::parse(bad)); },
        "truncated trailing APCI group header", "group header");
}

std::uint16_t rgb565(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    return static_cast<std::uint16_t>((red >> 3) << 11)
        | static_cast<std::uint16_t>((green >> 2) << 5)
        | static_cast<std::uint16_t>(blue >> 3);
}

Bytes make_frame(std::int16_t x, std::int16_t y, std::uint8_t red,
    std::uint8_t green, std::uint8_t blue) {
    Bytes result = bytes({'A', 'P'});
    append_u16(result, 24);
    append_u16(result, static_cast<std::uint16_t>(x));
    append_u16(result, static_cast<std::uint16_t>(y));
    append_u16(result, 1);
    append_u16(result, 1);
    append_u16(result, 0);
    result.push_back(std::byte{1});
    result.push_back(std::byte{0});
    append_u32(result, 2);
    append_u32(result, 0);
    append_u16(result, rgb565(red, green, blue));
    return result;
}

Bytes make_apg(const std::vector<Bytes>& frames) {
    Bytes result = bytes({'A', 'G'});
    append_u16(result, static_cast<std::uint16_t>(8 + frames.size() * 8));
    append_u16(result, static_cast<std::uint16_t>(frames.size()));
    append_u16(result, 0);
    for (const Bytes& frame : frames) {
        append_u16(result, 0);
        append_u16(result, 0);
        append_u32(result, static_cast<std::uint32_t>(frame.size()));
    }
    for (const Bytes& frame : frames) {
        result.insert(result.end(), frame.begin(), frame.end());
    }
    return result;
}

Bytes make_pif(const std::vector<std::array<std::uint16_t, 14>>& rows) {
    Bytes result = bytes({'P', 'I', 'F', 0});
    append_u32(result, 3);
    append_u16(result, 0);
    append_u16(result, 28);
    append_u16(result, static_cast<std::uint16_t>(rows.size()));
    append_u16(result, 2);
    append_u32(result, 0);
    for (const auto& row : rows) {
        for (const std::uint16_t value : row) {
            append_u16(result, value);
        }
    }
    return result;
}

std::array<std::uint16_t, 14> pif_row(std::uint16_t anchor_x,
    std::uint16_t anchor_y, std::uint16_t delay) {
    std::array<std::uint16_t, 14> row{};
    row[2] = anchor_x;
    row[3] = anchor_y;
    row[5] = delay;
    return row;
}

Bytes make_avatar_apci(std::uint8_t body_key, std::uint8_t head_key) {
    Bytes data = apci_header();
    append_group_header(data, 0, 2);
    const std::array parts = {
        std::pair<std::string_view, std::uint32_t>{"player\\body", 200},
        std::pair<std::string_view, std::uint32_t>{"player\\head", 105},
    };
    const std::array keys = {body_key, head_key};
    for (std::size_t index = 0; index < parts.size(); ++index) {
        append_record(data, parts[index].first, static_cast<std::uint16_t>(index), 1);
        append_entry(data, parts[index].second, 1);
        append_mid(data, (8U << 16) | 10U);
        for (std::uint32_t direction = 0; direction < 8; ++direction) {
            append_item(data, 0x01000001U, 0x00010000U, bytes({keys[index]}));
        }
    }
    return data;
}

class MemoryResources final : public dhsh::ResourceProvider {
public:
    void add(std::string path, Bytes data) {
        files_.emplace(std::move(path), std::move(data));
    }

    Bytes bytes(std::string_view path) override {
        const auto found = files_.find(std::string(path));
        if (found == files_.end()) {
            throw dhsh::FormatError("missing test resource: " + std::string(path));
        }
        return found->second;
    }

    bool exists(std::string_view path) const override {
        return files_.find(std::string(path)) != files_.end();
    }

private:
    std::unordered_map<std::string, Bytes> files_;
};

std::uint8_t channel(const dhsh::Image& image, std::size_t pixel, std::size_t offset) {
    return std::to_integer<std::uint8_t>(image.rgba[pixel * 4 + offset]);
}

dhsh::AvatarCatalog make_catalog(std::uint8_t body_key, std::uint8_t head_key) {
    return dhsh::AvatarCatalog(make_avatar_apci(body_key, head_key));
}

void test_avatar_default_preset_and_missing_part_error() {
    const dhsh::AvatarPreset preset = dhsh::AvatarCatalog::default_preset();
    check(preset.pak == "player.pak" && preset.parts.size() == 2
            && preset.parts[0].part == "body" && preset.parts[0].model == 200
            && preset.parts[1].part == "head" && preset.parts[1].model == 105,
        "M1 preset must explicitly select the real body/200 and head/105 resources");
    MemoryResources resources;
    const dhsh::AvatarCatalog catalog = make_catalog(6, 11);
    dhsh::Avatar avatar(resources, catalog);
    expect_error([&] { static_cast<void>(avatar.assemble(preset, 10, 0)); },
        "missing body must not be silently omitted", "body/200/10/0.apg");
    resources.add("body/200/10/0.apg", make_apg({make_frame(0, 0, 255, 0, 0)}));
    expect_error([&] { static_cast<void>(avatar.assemble(preset, 10, 0)); },
        "missing head must not be silently omitted", "head/105/10/0.apg");
}

Bytes nz_store(const Bytes& payload) {
    Bytes stored = bytes({'N', 'Z'});
    append_u16(stored, 0);
    append_u32(stored, static_cast<std::uint32_t>(payload.size()));
    append_u32(stored, static_cast<std::uint32_t>(payload.size()));
    stored.insert(stored.end(), payload.begin(), payload.end());
    return stored;
}

void add_part(MemoryResources& resources, std::string_view part, std::uint32_t model,
    std::uint32_t direction, const Bytes& apg, const Bytes* pif = nullptr) {
    const std::string base = std::string(part) + "/" + std::to_string(model) + "/10";
    resources.add(base + "/" + std::to_string(direction) + ".apg", nz_store(apg));
    if (pif != nullptr) {
        resources.add(base + "/perinfo.dat", nz_store(*pif));
    }
}

void test_avatar_layer_keys_are_ascending_and_stable() {
    {
        MemoryResources resources;
        add_part(resources, "body", 200, 0, make_apg({make_frame(0, 0, 255, 0, 0)}));
        add_part(resources, "head", 105, 0, make_apg({make_frame(0, 0, 0, 0, 255)}));
        const dhsh::AvatarCatalog catalog = make_catalog(6, 11);
        dhsh::Avatar avatar(resources, catalog);
        const auto result = avatar.assemble(dhsh::AvatarCatalog::default_preset(), 10, 0);
        check(result.frames.size() == 1 && result.frames[0].image.width == 640
                && result.frames[0].image.height == 480,
            "avatar canvas or frame count changed");
        const std::size_t pixel = (30U * 640U + 160U);
        check(channel(result.frames[0].image, pixel, 0) == 0
                && channel(result.frames[0].image, pixel, 2) == 255,
            "smaller APCI layer key must be drawn first");
    }
    {
        MemoryResources resources;
        add_part(resources, "body", 200, 0, make_apg({make_frame(0, 0, 255, 0, 0)}));
        add_part(resources, "head", 105, 0, make_apg({make_frame(0, 0, 0, 0, 255)}));
        const dhsh::AvatarCatalog catalog = make_catalog(20, 4);
        dhsh::Avatar avatar(resources, catalog);
        const auto result = avatar.assemble(dhsh::AvatarCatalog::default_preset(), 10, 0);
        check(channel(result.frames[0].image, 30U * 640U + 160U, 0) == 255
                && channel(result.frames[0].image, 30U * 640U + 160U, 2) == 0,
            "larger APCI layer key must be drawn last");
    }
    {
        MemoryResources resources;
        add_part(resources, "body", 200, 0, make_apg({make_frame(0, 0, 255, 0, 0)}));
        add_part(resources, "head", 105, 0, make_apg({make_frame(0, 0, 0, 0, 255)}));
        const dhsh::AvatarCatalog catalog = make_catalog(9, 9);
        dhsh::Avatar avatar(resources, catalog);
        const auto result = avatar.assemble(dhsh::AvatarCatalog::default_preset(), 10, 0);
        check(channel(result.frames[0].image, 30U * 640U + 160U, 2) == 255,
            "equal layer keys must preserve explicit preset order");
    }
}

void test_avatar_direction_fallback_anchor_delay_and_frame_cycling() {
    MemoryResources resources;
    const Bytes body_apg = make_apg({
        make_frame(-3, 7, 255, 0, 0),
        make_frame(1, 2, 0, 255, 0),
    });
    const Bytes head_apg = make_apg({make_frame(0, 0, 0, 0, 255)});
    std::vector<std::array<std::uint16_t, 14>> rows(8);
    rows[3] = pif_row(160, 210, 120);
    const Bytes pif = make_pif(rows);
    add_part(resources, "body", 200, 3, body_apg, &pif);
    add_part(resources, "head", 105, 3, head_apg, &pif);
    add_part(resources, "body", 200, 7, body_apg, &pif);
    add_part(resources, "head", 105, 7, head_apg, &pif);
    const dhsh::AvatarCatalog catalog = make_catalog(6, 11);
    dhsh::Avatar avatar(resources, catalog);
    const auto result = avatar.assemble(dhsh::AvatarCatalog::default_preset(), 10, 0);
    check(result.direction == 3, "missing direction must use viewer fallback order 3,7,0,...");
    check(result.frames.size() == 2 && result.frames[0].duration_ms == 120
            && result.frames[1].duration_ms == 120,
        "PIF direction-row delay or short-part frame cycling changed");
    const std::size_t first = static_cast<std::size_t>(240 - 210 + 7) * 640
        + static_cast<std::size_t>(320 - 160 - 3);
    check(channel(result.frames[0].image, first, 0) == 255,
        "avatar anchor must combine PIF f2/f3 with signed APG frame offsets");
    check(result.frames[1].image.rgba != result.frames[0].image.rgba,
        "avatar did not advance the two-frame body while cycling the head");
}

void test_avatar_composed_rgba_budget() {
    MemoryResources resources;
    add_part(resources, "body", 200, 0, make_apg({
        make_frame(0, 0, 255, 0, 0),
        make_frame(1, 2, 0, 255, 0),
    }));
    add_part(resources, "head", 105, 0, make_apg({make_frame(0, 0, 0, 0, 255)}));
    const dhsh::AvatarCatalog catalog = make_catalog(6, 11);
    dhsh::Avatar avatar(resources, catalog);
    constexpr std::size_t frame_bytes = 640U * 480U * 4U;
    const dhsh::AvatarDecodeOptions exact{2U * frame_bytes};
    const auto result = avatar.assemble(dhsh::AvatarCatalog::default_preset(), 10, 0, exact);
    check(result.frames.size() == 2, "exact composed RGBA budget must be accepted");
    const dhsh::AvatarDecodeOptions too_small{2U * frame_bytes - 1U};
    expect_error([&] {
        static_cast<void>(avatar.assemble(
            dhsh::AvatarCatalog::default_preset(), 10, 0, too_small));
    }, "composed RGBA total budget", "composed RGBA budget");
}

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open real avatar fixture");
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("cannot determine real avatar fixture size");
    }
    Bytes data(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!data.empty()) {
        input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (!input) {
        throw std::runtime_error("cannot read real avatar fixture");
    }
    return data;
}

void fnv_byte(std::uint64_t& hash, std::uint8_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
}

void fnv_u16(std::uint64_t& hash, std::uint16_t value) {
    fnv_byte(hash, static_cast<std::uint8_t>(value & 0xffU));
    fnv_byte(hash, static_cast<std::uint8_t>(value >> 8));
}

void fnv_u32(std::uint64_t& hash, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        fnv_byte(hash, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

std::uint64_t fnv1a(const dhsh::Image& image) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const std::byte value : image.rgba) {
        fnv_byte(hash, std::to_integer<std::uint8_t>(value));
    }
    return hash;
}

std::uint64_t layout_hash(const dhsh::AvatarFrame& frame) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto& layer : frame.layers) {
        fnv_u32(hash, static_cast<std::uint32_t>(layer.part.size()));
        for (const char value : layer.part) {
            fnv_byte(hash, static_cast<std::uint8_t>(value));
        }
        fnv_u32(hash, layer.model);
        fnv_u32(hash, static_cast<std::uint32_t>(layer.source_frame));
        fnv_u16(hash, layer.layer_key);
        fnv_u32(hash, static_cast<std::uint32_t>(layer.origin_x));
        fnv_u32(hash, static_cast<std::uint32_t>(layer.origin_y));
    }
    return hash;
}

int real_resource_main(const std::filesystem::path& shdata) {
    dhsh::PakResourceProvider resources(shdata, {"player.pak"});
    const dhsh::AvatarCatalog catalog(read_file(shdata / "perinfo.pak"));
    dhsh::Avatar avatar(resources, catalog);
    const dhsh::AvatarPreset preset = dhsh::AvatarCatalog::default_preset();
    for (const std::uint32_t action : {10U, 30U}) {
        for (std::uint32_t direction = 0; direction < 8; ++direction) {
            const auto result = avatar.assemble(preset, action, direction);
            check(result.direction == direction && !result.frames.empty(),
                "real preset is missing an idle/walk direction");
            for (std::size_t index = 0; index < result.frames.size(); ++index) {
                const auto& frame = result.frames[index];
                check(frame.image.width == 640 && frame.image.height == 480
                        && frame.duration_ms > 0,
                    "real avatar frame dimensions or delay are invalid");
                std::cout << "REAL action=" << action << " direction=" << direction
                    << " frame=" << index << " delay=" << frame.duration_ms
                    << " fnv=" << std::hex << fnv1a(frame.image)
                    << " layout=" << layout_hash(frame) << std::dec << '\n';
            }
        }
    }
    return 0;
}

int dump_frame_main(const std::filesystem::path& shdata,
    const std::filesystem::path& output_path) {
    dhsh::PakResourceProvider resources(shdata, {"player.pak"});
    const dhsh::AvatarCatalog catalog(read_file(shdata / "perinfo.pak"));
    dhsh::Avatar avatar(resources, catalog);
    const auto result = avatar.assemble(dhsh::AvatarCatalog::default_preset(), 10, 0);
    const auto& rgba = result.frames.front().image.rgba;
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(rgba.data()),
        static_cast<std::streamsize>(rgba.size()));
    return output ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::string_view(argv[1]) == "--dump") {
        try {
            return dump_frame_main(argv[2], argv[3]);
        } catch (const std::exception& error) {
            std::cerr << "avatar dump error: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 3 && std::string_view(argv[1]) == "--real") {
        try {
            return real_resource_main(argv[2]);
        } catch (const std::exception& error) {
            std::cerr << "avatar real-resource error: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc != 1) {
        return 2;
    }

    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"APCI empty file and 20-group boundary", test_apci_empty_and_twenty_group_boundary},
        {"APCI entry, mid action, and item layout", test_apci_entry_mid_and_item_layout},
        {"APCI blob rules and 16-bit truncation", test_apci_blob_size_rules_and_u16_truncation},
        {"APCI malformed input and exact consumption", test_apci_rejects_malformed_or_unconsumed_input},
        {"Avatar explicit preset and missing parts", test_avatar_default_preset_and_missing_part_error},
        {"Avatar ascending and stable layer keys", test_avatar_layer_keys_are_ascending_and_stable},
        {"Avatar fallback, anchors, delays, and frame cycling", test_avatar_direction_fallback_anchor_delay_and_frame_cycling},
        {"Avatar composed RGBA total budget", test_avatar_composed_rgba_budget},
    };

    bool passed = true;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
            passed = false;
        }
    }
    return passed ? 0 : 1;
}
