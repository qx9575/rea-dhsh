#include "formats/MapFormats.h"
#include "resource/ResourceError.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
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

Bytes text_bytes(std::string_view text) {
    Bytes result;
    result.reserve(text.size());
    for (const char value : text) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

void append_text(Bytes& output, std::string_view text) {
    const Bytes input = text_bytes(text);
    output.insert(output.end(), input.begin(), input.end());
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

void set_u32(Bytes& output, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output[offset + shift / 8] = static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

void append_fixed_string(Bytes& output, std::string_view value, std::size_t size) {
    check(value.size() <= size, "fixed test string is too long");
    append_text(output, value);
    output.resize(output.size() + size - value.size(), std::byte{0});
}

template <typename Callback>
void expect_error(Callback&& callback, const std::string& message) {
    try {
        std::forward<Callback>(callback)();
    } catch (const dhsh::FormatError&) {
        return;
    } catch (const std::exception& error) {
        throw std::runtime_error(message + ": wrong exception: " + error.what());
    }
    throw std::runtime_error(message + ": no exception");
}

void test_map_ini_gb18030_sections_and_keys() {
    Bytes input = text_bytes("; comment\r\n[ Base ]\r\nwidth = 3072\r\nNAME=");
    const Bytes name = bytes({0xd3, 0xa6, 0xcc, 0xec, 0xb8, 0xae});
    input.insert(input.end(), name.begin(), name.end());
    append_text(input, "\r\n[ext]\nCellFile=map\\YingTianFu\\YingTianFu.CEL\n");

    const dhsh::MapIni map = dhsh::parse_map_ini(input);
    check(map.size() == 2, "MAP section count changed");
    check(map.at("base").at("WIDTH") == "3072", "MAP key was not normalized");
    check(map.at("base").at("NAME") == "\xe5\xba\x94\xe5\xa4\xa9\xe5\xba\x9c",
        "MAP GB18030 text was not decoded to UTF-8");
    check(map.at("ext").at("CELLFILE") == "map\\YingTianFu\\YingTianFu.CEL",
        "MAP value or mixed-case key changed");
}

void test_map_ini_utf8_bom_and_duplicate_keys() {
    Bytes input = bytes({0xef, 0xbb, 0xbf});
    append_text(input, "[BASE]\nMUSIC=1\n# ignored\nmusic = 19015\n");
    const dhsh::MapIni map = dhsh::parse_map_ini(input);
    check(map.at("base").at("MUSIC") == "19015", "MAP UTF-8 BOM or duplicate key changed");
}

Bytes make_cel(
    std::uint32_t width = 9,
    std::uint32_t height = 2,
    std::uint32_t flags = 1,
    const Bytes& ground = bytes({0x80, 0x40, 0x00, 0x80}),
    const Bytes& auxiliary = bytes({0xff, 0xff, 0xff, 0xff})) {
    Bytes result = bytes({'C', 'E', 'L', 0});
    append_u32(result, 2);
    append_u32(result, 0);
    append_u32(result, flags);
    append_u32(result, width);
    append_u32(result, height);
    result.insert(result.end(), ground.begin(), ground.end());
    if ((flags & 1U) != 0) {
        result.insert(result.end(), auxiliary.begin(), auxiliary.end());
    }
    set_u32(result, 8, static_cast<std::uint32_t>(result.size()));
    return result;
}

void test_cel_uses_msb_first_ground_layer_and_blocked_boundaries() {
    const dhsh::Cel cel = dhsh::Cel::parse(make_cel());
    check(cel.width == 9 && cel.height == 2 && cel.flags == 1, "CEL header changed");
    check(cel.bits.size() == 4, "CEL did not select exactly one ground bitmap");
    check(cel.blocked(0, 0) && !cel.blocked(1, 0) && !cel.blocked(8, 0) && cel.blocked(9, 0),
        "CEL MSB-first bits or first ground layer changed");
    check(!cel.blocked(0, 1) && cel.blocked(8, 1), "CEL row stride changed");
    check(cel.blocked(-1, 0) && cel.blocked(0, -1) && cel.blocked(9, 0) && cel.blocked(0, 2),
        "CEL out-of-bounds cells must be blocked");
}

void test_cel_map_fit_copies_top_left_crops_and_pads_blocked() {
    const dhsh::Cel file = dhsh::Cel::parse(make_cel());
    const dhsh::Cel padded = file.for_map(11, 3);
    check(padded.width == 11 && padded.height == 3, "CEL map dimensions changed");
    check(padded.blocked(0, 0) && !padded.blocked(1, 0) && padded.blocked(9, 0),
        "CEL top-left copy changed");
    check(padded.blocked(10, 0) && padded.blocked(0, 2) && padded.blocked(10, 2),
        "CEL right/bottom padding must be blocked");

    const dhsh::Cel cropped = file.for_map(8, 1);
    check(cropped.width == 8 && cropped.height == 1 && cropped.blocked(0, 0) && !cropped.blocked(7, 0),
        "CEL crop is not the top-left map region");
    expect_error([&] { static_cast<void>(file.for_map(0, 1)); }, "CEL zero map width");
    expect_error([&] { static_cast<void>(file.for_map(1, 0)); }, "CEL zero map height");
}

void test_cel_rejects_malformed_headers_and_extents() {
    expect_error([] { static_cast<void>(dhsh::Cel::parse(bytes({'C', 'E', 'L'}))); }, "short CEL");
    Bytes input = make_cel();
    input[0] = std::byte{'X'};
    expect_error([&] { static_cast<void>(dhsh::Cel::parse(input)); }, "bad CEL magic");
    input = make_cel();
    set_u32(input, 4, 3);
    expect_error([&] { static_cast<void>(dhsh::Cel::parse(input)); }, "bad CEL version");
    input = make_cel();
    set_u32(input, 8, static_cast<std::uint32_t>(input.size() - 1));
    expect_error([&] { static_cast<void>(dhsh::Cel::parse(input)); }, "bad CEL total size");
    input = make_cel();
    input.pop_back();
    set_u32(input, 8, static_cast<std::uint32_t>(input.size()));
    expect_error([&] { static_cast<void>(dhsh::Cel::parse(input)); }, "truncated CEL auxiliary layer");
    input = make_cel(9, 2, 0, bytes({0x80, 0x40, 0x00}));
    expect_error([&] { static_cast<void>(dhsh::Cel::parse(input)); }, "truncated CEL ground layer");
    expect_error([] { static_cast<void>(dhsh::Cel::parse(make_cel(0, 2, 0, {}))); }, "zero CEL width");
    expect_error([] { static_cast<void>(dhsh::Cel::parse(make_cel(1, 0, 0, {}))); }, "zero CEL height");
    expect_error([] {
        static_cast<void>(dhsh::Cel::parse(make_cel(0xffffffffU, 0xffffffffU, 0, {})));
    }, "overflowing CEL dimensions");
}

Bytes make_man(std::uint32_t entry_size = 100) {
    check(entry_size >= 100, "invalid MAN test entry size");
    Bytes result = bytes({'M', 'A', 'N', 0});
    append_u32(result, 4);
    append_u32(result, 0);
    append_u32(result, 0);
    append_u32(result, 28);
    append_u32(result, entry_size);
    append_u32(result, 1);
    append_fixed_string(result, "map\\animate\\YingTianFu\\duchang\\denglong1", 64);
    for (const std::int32_t value : {2628, 421, -730, 1, 0, 7, -25770, 3000, 5000}) {
        append_u32(result, static_cast<std::uint32_t>(value));
    }
    result.resize(28 + entry_size, std::byte{0});
    set_u32(result, 8, static_cast<std::uint32_t>(result.size()));
    return result;
}

void test_man_preserves_100_byte_records_and_top_left_coordinates() {
    const dhsh::Man man = dhsh::Man::parse(make_man());
    check(man.data_offset == 28 && man.entry_size == 100 && man.entries.size() == 1,
        "MAN table header or count changed");
    const dhsh::ManEntry& entry = man.entries.front();
    check(entry.path == "map\\animate\\YingTianFu\\duchang\\denglong1", "MAN path changed");
    check(entry.x == 2628 && entry.y == 421, "MAN x/y are not the top-left pixel position");
    check(entry.f48 == -730 && entry.f4c == 1 && entry.f50 == 0 && entry.f54 == 7
            && entry.f58 == -25770 && entry.f5c == 3000 && entry.f60 == 5000,
        "MAN trailing fields were not preserved verbatim");

    const dhsh::Man padded = dhsh::Man::parse(make_man(104));
    check(padded.entries.size() == 1 && padded.entries[0].x == 2628 && padded.entries[0].f60 == 5000,
        "MAN padded record stride changed");
}

void test_man_rejects_malformed_headers_and_extents() {
    expect_error([] { static_cast<void>(dhsh::Man::parse(bytes({'M', 'A', 'N'}))); }, "short MAN");
    Bytes input = make_man();
    input[0] = std::byte{'X'};
    expect_error([&] { static_cast<void>(dhsh::Man::parse(input)); }, "bad MAN magic");
    input = make_man();
    set_u32(input, 4, 5);
    expect_error([&] { static_cast<void>(dhsh::Man::parse(input)); }, "bad MAN version");
    input = make_man();
    set_u32(input, 8, 0);
    expect_error([&] { static_cast<void>(dhsh::Man::parse(input)); }, "bad MAN total size");
    input = make_man();
    set_u32(input, 20, 99);
    expect_error([&] { static_cast<void>(dhsh::Man::parse(input)); }, "short MAN entry size");
    input = make_man();
    set_u32(input, 16, 0xffffffffU);
    expect_error([&] { static_cast<void>(dhsh::Man::parse(input)); }, "overflowing MAN data offset");
    input = make_man();
    set_u32(input, 24, 2);
    expect_error([&] { static_cast<void>(dhsh::Man::parse(input)); }, "truncated MAN entries");
    input = make_man();
    set_u32(input, 20, 0xffffffffU);
    set_u32(input, 24, 2);
    expect_error([&] { static_cast<void>(dhsh::Man::parse(input)); }, "overflowing MAN entry extent");
}

Bytes make_mlk() {
    Bytes result = bytes({'l', 'n', 'k', 0});
    append_u32(result, 3);
    append_u32(result, 128);
    append_u32(result, 1);
    append_u32(result, 1);
    append_u32(result, 7);
    append_fixed_string(result, "YTF_YaMen", 64);
    for (const std::int32_t value : {9, 80, 96, 0, 0, 512, 544, 5, 11, 12}) {
        append_u32(result, static_cast<std::uint32_t>(value));
    }
    check(result.size() == 128, "invalid MLK test record size");
    return result;
}

void test_mlk_preserves_spawn_cell_facing_and_adjusts_source_rectangle() {
    const dhsh::Mlk mlk = dhsh::Mlk::parse(make_mlk());
    check(mlk.links.size() == 1, "MLK record count changed");
    const dhsh::MlkLink& link = mlk.links.front();
    check(link.value == 7 && link.target == "YTF_YaMen" && link.paired == 9,
        "MLK identity or target changed");
    check(link.x == 48 && link.y == 64 && link.w == 32 && link.h == 32 && link.adjusted,
        "MLK zero-size source rectangle adjustment changed");
    check(link.dest_x / 16 == 32 && link.dest_y / 16 == 34 && link.extra0 == 5,
        "MLK destination spawn cell or facing changed");
    check(link.extra1 == 11 && link.extra2 == 12, "MLK extra fields changed");
}

void test_mlk_rejects_malformed_headers_and_record_table() {
    expect_error([] { static_cast<void>(dhsh::Mlk::parse(bytes({'l', 'n', 'k'}))); }, "short MLK");
    Bytes input = make_mlk();
    input[0] = std::byte{'X'};
    expect_error([&] { static_cast<void>(dhsh::Mlk::parse(input)); }, "bad MLK magic");
    input = make_mlk();
    set_u32(input, 4, 4);
    expect_error([&] { static_cast<void>(dhsh::Mlk::parse(input)); }, "bad MLK version");
    input = make_mlk();
    set_u32(input, 8, 127);
    expect_error([&] { static_cast<void>(dhsh::Mlk::parse(input)); }, "bad MLK total size");
    input = make_mlk();
    set_u32(input, 16, 2);
    expect_error([&] { static_cast<void>(dhsh::Mlk::parse(input)); }, "MLK count does not match table");
    input = make_mlk();
    set_u32(input, 16, 0xffffffffU);
    expect_error([&] { static_cast<void>(dhsh::Mlk::parse(input)); }, "overflowing MLK table");
}

Bytes make_core(
    const std::vector<std::uint16_t>& runs = {0x0001, 0x8002, 0x8003},
    std::uint32_t width = 3,
    std::uint32_t height = 2,
    std::uint32_t record_size = 28,
    std::string_view name = {}) {
    check(record_size >= 28 && name.size() <= record_size - 28, "invalid core test record");
    Bytes result = bytes({'M', 'S', 'K', 0});
    append_u32(result, 5);
    append_u32(result, 0);
    append_u32(result, 0);
    append_u32(result, 40);
    append_u32(result, record_size);
    append_u32(result, 1);
    append_u32(result, 40 + record_size);
    append_u32(result, 24);
    append_u32(result, 1);
    append_fixed_string(result, name, record_size - 28);
    append_u32(result, width);
    append_u32(result, height);
    append_u32(result, static_cast<std::uint32_t>(-10));
    append_u32(result, 1);
    const std::size_t blob_offset_field = result.size();
    append_u32(result, 0);
    const std::size_t blob_size_field = result.size();
    append_u32(result, 0);
    append_u32(result, 256);
    append_u32(result, 0);
    append_u32(result, static_cast<std::uint32_t>(-2));
    append_u32(result, 100);
    append_u32(result, 1);
    append_u32(result, 0);
    append_u32(result, 0);
    set_u32(result, blob_offset_field, static_cast<std::uint32_t>(result.size()));
    set_u32(result, blob_size_field, static_cast<std::uint32_t>(runs.size() * 2));
    for (const std::uint16_t run : runs) {
        append_u16(result, run);
    }
    set_u32(result, 8, static_cast<std::uint32_t>(result.size()));
    return result;
}

void test_core_records_placements_rle_and_sort_y() {
    const dhsh::Core core = dhsh::Core::parse(make_core());
    check(core.records.size() == 1 && core.placements.size() == 1 && core.placement_size == 24,
        "core record or placement count changed");
    const dhsh::CoreRecord& record = core.records.front();
    check(record.name.empty() && record.width == 3 && record.height == 2 && record.base_y == -10
            && record.kind == 1 && record.data_offset == 92 && record.data_size == 6 && record.slope == 256,
        "core record fields changed");
    const dhsh::CorePlacement& placement = core.placements.front();
    check(placement.record == 0 && placement.x == -2 && placement.y == 100 && placement.flag == 1
            && placement.group == 0 && placement.frame == 0,
        "core 24-byte placement fields changed");
    check(core.sort_y(0) == 92, "core sortY formula changed");
    const dhsh::CoreMask mask = core.decode_mask(0);
    check(mask.width == 3 && mask.height == 2
            && mask.pixels == std::vector<std::uint8_t>({0, 1, 1, 1, 1, 1}),
        "core row-local RLE mask changed");

    const dhsh::Core legacy = dhsh::Core::parse(make_core({0x0003, 0x8003}, 3, 2, 284, "cover\\00001.bmp"));
    check(legacy.records[0].name == "cover\\00001.bmp" && legacy.decode_mask(0).pixels.size() == 6,
        "legacy MSK record name or stride changed");
}

void test_core_rejects_malformed_container_and_placement_references() {
    expect_error([] { static_cast<void>(dhsh::Core::parse(bytes({'M', 'S', 'K'}))); }, "short core");
    Bytes input = make_core();
    input[0] = std::byte{'X'};
    expect_error([&] { static_cast<void>(dhsh::Core::parse(input)); }, "bad core magic");
    input = make_core();
    set_u32(input, 4, 4);
    expect_error([&] { static_cast<void>(dhsh::Core::parse(input)); }, "bad core version");
    input = make_core();
    set_u32(input, 8, 0);
    expect_error([&] { static_cast<void>(dhsh::Core::parse(input)); }, "bad core total size");
    input = make_core();
    set_u32(input, 16, 39);
    expect_error([&] { static_cast<void>(dhsh::Core::parse(input)); }, "bad core header size");
    input = make_core();
    set_u32(input, 20, 27);
    expect_error([&] { static_cast<void>(dhsh::Core::parse(input)); }, "short core record size");
    input = make_core();
    set_u32(input, 28, 72);
    expect_error([&] { static_cast<void>(dhsh::Core::parse(input)); }, "core data offset mismatch");
    input = make_core();
    set_u32(input, 32, 20);
    expect_error([&] { static_cast<void>(dhsh::Core::parse(input)); }, "bad core placement size");
    input = make_core();
    set_u32(input, 36, 2);
    expect_error([&] { static_cast<void>(dhsh::Core::parse(input)); }, "truncated core placements");
    input = make_core();
    set_u32(input, 56, 0);
    expect_error([&] { static_cast<void>(dhsh::Core::parse(input)); }, "core blob overlaps tables");
    input = make_core();
    set_u32(input, 60, 0xffffffffU);
    expect_error([&] { static_cast<void>(dhsh::Core::parse(input)); }, "core blob overruns file");
    input = make_core();
    set_u32(input, 68, 1);
    expect_error([&] { static_cast<void>(dhsh::Core::parse(input)); }, "core placement record reference");
}

void test_core_rle_rejects_cross_row_short_extra_zero_and_odd_runs() {
    const std::vector<std::pair<std::string, Bytes>> invalid = {
        {"cross-row run", make_core({0x0004})},
        {"short row", make_core({0x0003, 0x8002})},
        {"extra pixels", make_core({0x0003, 0x8003, 0x0001})},
        {"zero run", make_core({0x0000, 0x0003, 0x8003})},
    };
    for (const auto& [name, input] : invalid) {
        const dhsh::Core core = dhsh::Core::parse(input);
        expect_error([&] { static_cast<void>(core.decode_mask(0)); }, "core " + name);
    }

    Bytes odd = make_core();
    odd.push_back(std::byte{0});
    set_u32(odd, 8, static_cast<std::uint32_t>(odd.size()));
    set_u32(odd, 60, 7);
    expect_error([&] { static_cast<void>(dhsh::Core::parse(odd).decode_mask(0)); }, "odd core blob");

    expect_error([] { static_cast<void>(dhsh::Core::parse(make_core({}, 0, 2)).decode_mask(0)); },
        "zero core mask width");
    expect_error([] { static_cast<void>(dhsh::Core::parse(make_core({}, 3, 0)).decode_mask(0)); },
        "zero core mask height");
    expect_error([] {
        static_cast<void>(dhsh::Core::parse(make_core({}, 0xffffffffU, 2)).decode_mask(0));
    }, "overflowing core mask dimensions");

    const dhsh::Core core = dhsh::Core::parse(make_core());
    expect_error([&] { static_cast<void>(core.decode_mask(1)); }, "core mask index");
    expect_error([&] { static_cast<void>(core.sort_y(1)); }, "core placement index");
}

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    check(static_cast<bool>(input), "cannot open real resource: " + path.string());
    const std::streampos end = input.tellg();
    check(end >= 0, "cannot size real resource: " + path.string());
    Bytes result(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!result.empty()) {
        input.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
    }
    check(static_cast<bool>(input), "cannot read real resource: " + path.string());
    return result;
}

void verify_ying_tian_fu(const std::filesystem::path& root) {
    const dhsh::Man man = dhsh::Man::parse(read_file(root / "YingTianFu.MAN"));
    check(man.entries.size() == 103, "YingTianFu MAN count must match manifest 103");
    bool found_lantern = false;
    for (const dhsh::ManEntry& entry : man.entries) {
        std::string path = entry.path;
        for (char& value : path) {
            value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
        }
        if (path.find("yingtianfu\\duchang\\denglong1") != std::string::npos) {
            check(entry.x == 2628 && entry.y == 421, "YingTianFu lantern top-left coordinate changed");
            found_lantern = true;
        }
    }
    check(found_lantern, "YingTianFu lantern MAN entry is missing");

    const dhsh::Cel cel = dhsh::Cel::parse(read_file(root / "YingTianFu.CEL"));
    check(cel.width == 488 && cel.height == 360, "YingTianFu CEL dimensions changed");
    check(!cel.blocked(128, 125) && !cel.blocked(135, 123) && cel.blocked(-1, 0),
        "YingTianFu known ground cells do not match cel-layer-runtime.json");

    const dhsh::Core core = dhsh::Core::parse(read_file(root / "YingTianFu.core"));
    check(core.placements.size() == 563, "YingTianFu core placement count must match manifest 563");
    std::cout << "YingTianFu MAN=" << man.entries.size()
              << " CEL=" << cel.width << 'x' << cel.height
              << " cell(128,125)=" << cel.blocked(128, 125)
              << " cell(135,123)=" << cel.blocked(135, 123)
              << " core_records=" << core.records.size()
              << " core_placements=" << core.placements.size()
              << " sortY[0]=" << core.sort_y(0) << '\n';
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        try {
            verify_ying_tian_fu(argv[1]);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL: YingTianFu real resources: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc != 1) {
        return 2;
    }

    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"MAP GB18030 sections and keys", test_map_ini_gb18030_sections_and_keys},
        {"MAP UTF-8 BOM and duplicate keys", test_map_ini_utf8_bom_and_duplicate_keys},
        {"CEL ground bits and blocked boundaries", test_cel_uses_msb_first_ground_layer_and_blocked_boundaries},
        {"CEL top-left map fit", test_cel_map_fit_copies_top_left_crops_and_pads_blocked},
        {"CEL malformed inputs", test_cel_rejects_malformed_headers_and_extents},
        {"MAN complete records and coordinates", test_man_preserves_100_byte_records_and_top_left_coordinates},
        {"MAN malformed inputs", test_man_rejects_malformed_headers_and_extents},
        {"MLK spawn and facing", test_mlk_preserves_spawn_cell_facing_and_adjusts_source_rectangle},
        {"MLK malformed inputs", test_mlk_rejects_malformed_headers_and_record_table},
        {"core records placements RLE and sortY", test_core_records_placements_rle_and_sort_y},
        {"core malformed container", test_core_rejects_malformed_container_and_placement_references},
        {"core strict row-local RLE", test_core_rle_rejects_cross_row_short_extra_zero_and_odd_runs},
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
