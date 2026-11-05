#include "formats/A16.h"
#include "formats/Cti.h"
#include "platform/WicImage.h"
#include "render/Image.h"
#include "resource/ResourceError.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <span>
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

void append_u16(Bytes& output, std::uint16_t value) {
    output.push_back(static_cast<std::byte>(value & 0xffU));
    output.push_back(static_cast<std::byte>(value >> 8));
}

void append_u32(Bytes& output, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void set_u16(Bytes& output, std::size_t offset, std::uint16_t value) {
    output[offset] = static_cast<std::byte>(value & 0xffU);
    output[offset + 1] = static_cast<std::byte>(value >> 8);
}

void set_u32(Bytes& output, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output[offset + shift / 8] = static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

Bytes make_a16(
    std::string_view magic,
    std::uint16_t width,
    std::uint16_t height,
    unsigned compression,
    unsigned alpha_mode,
    const Bytes& stream1,
    const Bytes& stream2) {
    Bytes result;
    for (const char value : magic) {
        result.push_back(static_cast<std::byte>(value));
    }
    append_u16(result, 24);
    append_u16(result, 0);
    append_u16(result, 0);
    append_u16(result, width);
    append_u16(result, height);
    append_u16(result, 0);
    result.push_back(static_cast<std::byte>(compression));
    result.push_back(static_cast<std::byte>(alpha_mode));
    append_u32(result, static_cast<std::uint32_t>(stream1.size()));
    append_u32(result, static_cast<std::uint32_t>(stream2.size()));
    result.insert(result.end(), stream1.begin(), stream1.end());
    result.insert(result.end(), stream2.begin(), stream2.end());
    return result;
}

Bytes rgba(std::initializer_list<unsigned> values) {
    return bytes(values);
}

void expect_rgba(const dhsh::Image& image, unsigned width, unsigned height, const Bytes& expected,
    const std::string& message) {
    check(image.width == width && image.height == height, message + ": wrong dimensions");
    check(image.rgba == expected, message + ": wrong RGBA bytes");
    check(image.rgba.size() == static_cast<std::size_t>(width) * height * 4,
        message + ": pixels are not tightly packed RGBA8");
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

void expect_a16_error(const Bytes& input, const std::string& message,
    std::string_view text = {}) {
    expect_error([&] { static_cast<void>(dhsh::A16Decoder::decode(input)); }, message, text);
}

void test_raw_rgb565_and_alpha_zero() {
    const Bytes stream = bytes({
        0x00, 0x00, 0xff, 0xff, 0x00, 0xf8, 0xe0, 0x07,
    });
    expect_rgba(dhsh::A16Decoder::decode(make_a16("AP", 2, 2, 0, 0, stream, {})),
        2, 2,
        rgba({
            0, 0, 0, 255, 255, 255, 255, 255,
            255, 0, 0, 255, 0, 255, 0, 255,
        }),
        "raw RGB565");
}

void test_solid_and_short_raw_streams_match_reference_padding() {
    expect_rgba(dhsh::A16Decoder::decode(make_a16("AP", 3, 1, 1, 0, bytes({0x1f, 0}), {})),
        3, 1, rgba({0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255}), "solid RGB565");
    expect_rgba(dhsh::A16Decoder::decode(make_a16("AP", 2, 1, 0, 0, bytes({0x00, 0xf8, 0xff}), {})),
        2, 1, rgba({255, 0, 0, 255, 0, 0, 0, 255}), "short raw padding");
    expect_rgba(dhsh::A16Decoder::decode(make_a16("AP", 1, 1, 1, 0, bytes({0x1f}), {})),
        1, 1, rgba({0, 0, 0, 255}), "short solid padding");
}

void test_rle16_repeat_literal_and_padding() {
    const Bytes stream = bytes({0x82, 0x00, 0xf8, 0x02, 0xe0, 0x07, 0x1f, 0x00});
    expect_rgba(dhsh::A16Decoder::decode(make_a16("AP", 4, 1, 2, 0, stream, {})),
        4, 1,
        rgba({
            255, 0, 0, 255, 255, 0, 0, 255,
            0, 255, 0, 255, 0, 0, 255, 255,
        }),
        "RLE16 repeat and literal");
    expect_rgba(dhsh::A16Decoder::decode(make_a16("AP", 2, 1, 2, 0, bytes({0x82, 0x00}), {})),
        2, 1, rgba({0, 0, 0, 255, 0, 0, 0, 255}), "truncated RLE16 padding");
}

void test_rle8_alpha_scaling_and_padding() {
    const Bytes pixels = bytes({0x00, 0xf8, 0xe0, 0x07, 0x1f, 0x00, 0xff, 0xff});
    const Bytes alpha = bytes({0x82, 32, 0x02, 0, 16});
    expect_rgba(dhsh::A16Decoder::decode(make_a16("AP", 4, 1, 0, 2, pixels, alpha)),
        4, 1,
        rgba({
            255, 0, 0, 255, 0, 255, 0, 255,
            0, 0, 255, 0, 255, 255, 255, 127,
        }),
        "RLE8 alpha scaling");
    expect_rgba(dhsh::A16Decoder::decode(make_a16("AP", 1, 1, 1, 2,
                      bytes({0xff, 0xff}), bytes({0x81, 33}))),
        1, 1, rgba({255, 255, 255, 255}), "alpha saturation");
    expect_rgba(dhsh::A16Decoder::decode(make_a16("AP", 2, 1, 1, 2,
                      bytes({0xff, 0xff}), bytes({0x81, 0}))),
        2, 1, rgba({255, 255, 255, 0, 255, 255, 255, 255}), "short alpha padding");
}

void test_alpha_only_layer_is_white_silhouette() {
    expect_rgba(dhsh::A16Decoder::decode(make_a16("AL", 2, 1, 0, 2, {}, bytes({0x82, 16}))),
        2, 1, rgba({255, 255, 255, 127, 255, 255, 255, 127}), "alpha-only layer");
}

void test_a16_rejects_bad_headers_truncation_and_modes() {
    expect_a16_error(bytes({'A', 'P'}), "short header");
    expect_a16_error(make_a16("XX", 1, 1, 0, 0, {}, {}), "bad magic", "A16");
    Bytes bad_header = make_a16("AP", 1, 1, 0, 0, {}, {});
    set_u16(bad_header, 2, 23);
    expect_a16_error(bad_header, "short declared header");
    set_u16(bad_header, 2, 25);
    expect_a16_error(bad_header, "header beyond input");
    expect_a16_error(make_a16("AP", 1, 1, 3, 0, {}, {}), "unsupported compression", "compression 3");
    expect_a16_error(make_a16("AP", 1, 1, 0, 1, {}, {}), "unsupported alpha", "alpha mode 1");
    Bytes truncated = make_a16("AP", 1, 1, 0, 0, bytes({0, 0}), {});
    truncated.pop_back();
    expect_a16_error(truncated, "truncated streams", "truncated");
    Bytes overflowing = make_a16("AP", 1, 1, 0, 0, {}, {});
    set_u32(overflowing, 16, 0xffffffffU);
    set_u32(overflowing, 20, 2);
    expect_a16_error(overflowing, "overflowing stream extent");
}

void test_a16_rejects_invalid_dimensions_and_output_overflow() {
    expect_a16_error(make_a16("AP", 0, 1, 1, 0, bytes({0, 0}), {}), "zero width", "dimensions");
    expect_a16_error(make_a16("AP", 1, 0, 1, 0, bytes({0, 0}), {}), "zero height", "dimensions");
    expect_a16_error(make_a16("AP", 0xffffU, 1, 1, 0, bytes({0, 0}), {}),
        "negative width", "dimensions");
    expect_a16_error(make_a16("AP", 32767, 32767, 1, 0, bytes({0, 0}), {}),
        "RGBA size overflow", "dimensions");
}

Bytes make_cti() {
    Bytes result = bytes({'C', 'T'});
    append_u16(result, 20);
    append_u16(result, 3);
    append_u16(result, 16);
    append_u32(result, 0);
    append_u32(result, 6);
    append_u32(result, 12);
    for (const std::uint16_t color : {0xf800U, 0x07e0U, 0x001fU}) {
        append_u16(result, color);
    }
    for (const std::uint32_t color : {0x80ff0000U, 0x4000ff00U, 0xff0000ffU}) {
        append_u32(result, color);
    }
    return result;
}

void test_cti_parses_both_tables() {
    const dhsh::CtiPalette palette = dhsh::CtiPalette::parse(make_cti());
    check(palette.header.header_size == 20 && palette.header.count == 3
            && palette.header.bpp == 16 && palette.header.format == 0
            && palette.header.table1_size == 6 && palette.header.table2_size == 12,
        "CTI header fields changed");
    check(palette.table1 == std::vector<std::uint16_t>({0xf800, 0x07e0, 0x001f}),
        "CTI table1 changed");
    check(palette.table2 == std::vector<std::uint32_t>({0x80ff0000, 0x4000ff00, 0xff0000ff}),
        "CTI table2 changed");
}

void test_cti_indexed_table1_and_table2_conversion() {
    const dhsh::CtiPalette palette = dhsh::CtiPalette::parse(make_cti());
    const Bytes indices = bytes({0, 1, 2});
    expect_rgba(palette.indexed_image(indices, 3, 1, dhsh::CtiTable::table1), 3, 1,
        rgba({255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255}), "CTI table1");
    expect_rgba(palette.indexed_image(indices, 3, 1, dhsh::CtiTable::table2), 3, 1,
        rgba({255, 0, 0, 128, 0, 255, 0, 64, 0, 0, 255, 255}), "CTI table2");
    expect_rgba(palette.indexed_image(indices, 3, 1, dhsh::CtiTable::table2, bytes({0, 16, 32})),
        3, 1, rgba({255, 0, 0, 0, 0, 255, 0, 127, 0, 0, 255, 255}),
        "CTI alpha override");
}

void test_ai_rle8_indices_require_palette_and_use_table1() {
    const dhsh::CtiPalette palette = dhsh::CtiPalette::parse(make_cti());
    const Bytes input = make_a16("AI", 4, 1, 0, 0,
        bytes({0x82, 0, 0x02, 1, 2}), {});
    expect_error([&] { static_cast<void>(dhsh::A16Decoder::decode(input)); },
        "AI without palette", "palette");
    expect_error([&] { static_cast<void>(dhsh::A16Decoder::decode(input, nullptr)); },
        "AI with null palette", "palette");
    expect_rgba(dhsh::A16Decoder::decode(input, &palette), 4, 1,
        rgba({255, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255}),
        "AI RLE8 indexed table1");
}

void test_ai_short_indices_fail_and_short_alpha_pads() {
    const dhsh::CtiPalette palette = dhsh::CtiPalette::parse(make_cti());
    expect_error([&] {
        static_cast<void>(dhsh::A16Decoder::decode(
            make_a16("AI", 3, 1, 0, 0, bytes({0x81, 1}), {}), &palette));
    }, "AI short index output", "index");
    expect_error([&] {
        static_cast<void>(dhsh::A16Decoder::decode(
            make_a16("AI", 2, 1, 0, 0, {}, {}), &palette));
    }, "AI empty index output", "index");
    expect_rgba(dhsh::A16Decoder::decode(make_a16("AI", 3, 1, 0, 2,
                      bytes({0x83, 1}), bytes({0x81, 16})), &palette),
        3, 1, rgba({0, 255, 0, 127, 0, 255, 0, 255, 0, 255, 0, 255}),
        "AI short alpha padding and single scaling");
}

void test_cti_rejects_malformed_tables_and_indices() {
    expect_error([] { static_cast<void>(dhsh::CtiPalette::parse(bytes({'C', 'T'}))); },
        "short CTI header");
    Bytes bad_magic = make_cti();
    bad_magic[0] = std::byte{'X'};
    expect_error([&] { static_cast<void>(dhsh::CtiPalette::parse(bad_magic)); }, "bad CTI magic", "CTI");
    Bytes short_header = make_cti();
    set_u16(short_header, 2, 19);
    expect_error([&] { static_cast<void>(dhsh::CtiPalette::parse(short_header)); }, "short CTI declaration");
    Bytes truncated = make_cti();
    truncated.pop_back();
    expect_error([&] { static_cast<void>(dhsh::CtiPalette::parse(truncated)); }, "truncated CTI", "truncated");
    Bytes overflowing = make_cti();
    set_u32(overflowing, 12, 0xffffffffU);
    expect_error([&] { static_cast<void>(dhsh::CtiPalette::parse(overflowing)); }, "overflowing CTI extent");
    Bytes small_table = make_cti();
    set_u32(small_table, 12, 2);
    expect_error([&] { static_cast<void>(dhsh::CtiPalette::parse(small_table)); }, "short CTI table1");
    const dhsh::CtiPalette palette = dhsh::CtiPalette::parse(make_cti());
    expect_error([&] { static_cast<void>(palette.indexed_image(bytes({3}), 1, 1)); },
        "out-of-range CTI index", "index");
    expect_error([&] { static_cast<void>(palette.indexed_image(bytes({0}), 2, 1)); },
        "short CTI indices", "index");
    expect_error([&] {
        static_cast<void>(palette.indexed_image(bytes({0}), 1, 1,
            static_cast<dhsh::CtiTable>(3)));
    }, "unsupported CTI table", "table");
}

Bytes bmp_2x1() {
    Bytes result = bytes({'B', 'M'});
    append_u32(result, 62);
    append_u32(result, 0);
    append_u32(result, 54);
    append_u32(result, 40);
    append_u32(result, 2);
    append_u32(result, 1);
    append_u16(result, 1);
    append_u16(result, 24);
    append_u32(result, 0);
    append_u32(result, 8);
    append_u32(result, 0);
    append_u32(result, 0);
    append_u32(result, 0);
    append_u32(result, 0);
    const Bytes pixels = bytes({0, 0, 255, 0, 255, 0, 0, 0});
    result.insert(result.end(), pixels.begin(), pixels.end());
    return result;
}

Bytes png_2x1() {
    return bytes({
        137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82,
        0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 244, 34, 127, 138,
        0, 0, 0, 17, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 208,
        192, 240, 159, 225, 63, 0, 14, 125, 3, 126, 234, 246, 147, 192,
        0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130,
    });
}

Bytes jpeg_2x1() {
    return bytes({
        255, 216, 255, 224, 0, 16, 74, 70, 73, 70, 0, 1, 1, 0, 0, 1,
        0, 1, 0, 0, 255, 219, 0, 67, 0, 8, 6, 6, 7, 6, 5, 8,
        7, 7, 7, 9, 9, 8, 10, 12, 20, 13, 12, 11, 11, 12, 25, 18,
        19, 15, 20, 29, 26, 31, 30, 29, 26, 28, 28, 32, 36, 46, 39, 32,
        34, 44, 35, 28, 28, 40, 55, 41, 44, 48, 49, 52, 52, 52, 31, 39,
        57, 61, 56, 50, 60, 46, 51, 52, 50, 255, 219, 0, 67, 1, 8, 9,
        9, 12, 11, 12, 24, 13, 13, 24, 50, 33, 28, 33, 50, 50, 50, 50,
        50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50,
        50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50,
        50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 255, 192,
        0, 17, 8, 0, 1, 0, 2, 3, 1, 34, 0, 2, 17, 1, 3, 17,
        1, 255, 196, 0, 21, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 4, 255, 196, 0, 27, 16, 1, 0, 0,
        7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2,
        4, 5, 53, 115, 178, 255, 196, 0, 20, 1, 1, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 255, 196, 0, 29, 17,
        0, 1, 2, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        2, 0, 1, 3, 5, 50, 52, 113, 114, 178, 255, 218, 0, 12, 3, 1,
        0, 2, 17, 3, 17, 0, 63, 0, 182, 221, 140, 164, 211, 39, 48, 0,
        92, 117, 62, 93, 17, 204, 175, 163, 238, 125, 154, 255, 217,
    });
}

void test_wic_bmp_and_png_are_rgba() {
    expect_rgba(dhsh::WicImage::decode(bmp_2x1()), 2, 1,
        rgba({255, 0, 0, 255, 0, 255, 0, 255}), "WIC BMP");
    expect_rgba(dhsh::WicImage::decode(png_2x1()), 2, 1,
        rgba({255, 0, 0, 128, 0, 255, 0, 255}), "WIC PNG alpha");
}

void test_wic_jpeg_decodes_and_repeated_calls_balance_com() {
    const dhsh::Image first = dhsh::WicImage::decode(jpeg_2x1());
    check(first.width == 2 && first.height == 1 && first.rgba.size() == 8,
        "WIC JPEG dimensions or packing");
    const auto red = std::to_integer<unsigned>(first.rgba[0]);
    const auto green = std::to_integer<unsigned>(first.rgba[5]);
    check(red > 100 && green > 100, "WIC JPEG color conversion");
    check(std::to_integer<unsigned>(first.rgba[3]) == 255
            && std::to_integer<unsigned>(first.rgba[7]) == 255,
        "WIC JPEG alpha");
    const dhsh::Image second = dhsh::WicImage::decode(png_2x1());
    check(second.width == 2 && second.height == 1, "repeated WIC decode after COM cleanup");
}

void test_wic_rejects_gif_before_decoding() {
    const Bytes gif = bytes({
        71, 73, 70, 56, 57, 97, 1, 0, 1, 0, 128, 0, 0, 0, 0, 0,
        255, 255, 255, 33, 249, 4, 1, 0, 0, 0, 0, 44, 0, 0, 0, 0,
        1, 0, 1, 0, 0, 2, 2, 68, 1, 0, 59,
    });
    expect_error([&] { static_cast<void>(dhsh::WicImage::decode(gif)); },
        "GIF must be outside the approved WIC formats", "unsupported");
}

void test_wic_rejects_tga_and_garbage() {
    Bytes tga(18, std::byte{0});
    tga[2] = std::byte{2};
    tga[12] = std::byte{1};
    tga[14] = std::byte{1};
    tga[16] = std::byte{24};
    const Bytes pixel = bytes({0, 0, 0});
    tga.insert(tga.end(), pixel.begin(), pixel.end());
    expect_error([&] { static_cast<void>(dhsh::WicImage::decode(tga)); },
        "unsupported TGA", "WIC");
    expect_error([&] { static_cast<void>(dhsh::WicImage::decode(bytes({1, 2, 3}))); },
        "garbage WIC input", "WIC");
}

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open image fixture");
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("cannot determine image fixture size");
    }
    Bytes data(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!data.empty()) {
        input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (!input) {
        throw std::runtime_error("cannot read image fixture");
    }
    return data;
}

void write_file(const std::filesystem::path& path, const Bytes& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create decoded image");
    }
    if (!data.empty()) {
        output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (!output) {
        throw std::runtime_error("cannot write decoded image");
    }
}

int fixture_main(const std::vector<std::string>& args) {
    const Bytes input = read_file(args[2]);
    dhsh::Image image;
    if (args[1] == "--a16") {
        image = dhsh::A16Decoder::decode(input);
    } else if (args[1] == "--cti1" || args[1] == "--cti2") {
        const dhsh::CtiPalette palette = dhsh::CtiPalette::parse(input);
        Bytes indices(palette.header.count);
        for (std::size_t index = 0; index < indices.size(); ++index) {
            indices[index] = static_cast<std::byte>(index);
        }
        image = palette.indexed_image(indices, palette.header.count, 1,
            args[1] == "--cti2" ? dhsh::CtiTable::table2 : dhsh::CtiTable::table1);
    } else if (args[1] == "--wic") {
        image = dhsh::WicImage::decode(input);
    } else {
        return 2;
    }
    write_file(args[3], image.rgba);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4) {
        try {
            return fixture_main({argv, argv + argc});
        } catch (const std::exception& error) {
            std::cerr << "image error: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc != 1) {
        return 2;
    }

    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"raw RGB565 and alpha zero", test_raw_rgb565_and_alpha_zero},
        {"solid and short raw padding", test_solid_and_short_raw_streams_match_reference_padding},
        {"RLE16 repeat, literal, and padding", test_rle16_repeat_literal_and_padding},
        {"RLE8 alpha scaling and padding", test_rle8_alpha_scaling_and_padding},
        {"alpha-only white silhouette", test_alpha_only_layer_is_white_silhouette},
        {"A16 malformed headers, truncation, and modes", test_a16_rejects_bad_headers_truncation_and_modes},
        {"A16 invalid dimensions and overflow", test_a16_rejects_invalid_dimensions_and_output_overflow},
        {"CTI parses both tables", test_cti_parses_both_tables},
        {"CTI indexed color conversion", test_cti_indexed_table1_and_table2_conversion},
        {"AI RLE8 indices require CTI and use table1", test_ai_rle8_indices_require_palette_and_use_table1},
        {"AI short indices fail and short alpha pads", test_ai_short_indices_fail_and_short_alpha_pads},
        {"CTI malformed tables and indices", test_cti_rejects_malformed_tables_and_indices},
        {"WIC BMP and PNG", test_wic_bmp_and_png_are_rgba},
        {"WIC JPEG and COM lifetime", test_wic_jpeg_decodes_and_repeated_calls_balance_com},
        {"WIC rejects GIF before decoding", test_wic_rejects_gif_before_decoding},
        {"WIC rejects TGA and garbage", test_wic_rejects_tga_and_garbage},
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
