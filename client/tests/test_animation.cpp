#include "animation/AnimationClip.h"
#include "formats/Apg.h"
#include "formats/Cti.h"
#include "formats/Pif.h"
#include "render/Image.h"
#include "resource/ResourceError.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <limits>
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

Bytes make_frame(
    std::string_view magic,
    std::int16_t x,
    std::int16_t y,
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
    append_u16(result, static_cast<std::uint16_t>(x));
    append_u16(result, static_cast<std::uint16_t>(y));
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

Bytes make_apg(const std::vector<Bytes>& frames, const std::vector<unsigned>& padding = {}) {
    check(padding.empty() || padding.size() == frames.size(), "APG padding count mismatch");
    Bytes result = bytes({'A', 'G'});
    append_u16(result, static_cast<std::uint16_t>(8 + frames.size() * 8));
    append_u16(result, static_cast<std::uint16_t>(frames.size()));
    append_u16(result, 0);
    for (std::size_t index = 0; index < frames.size(); ++index) {
        append_u16(result, static_cast<std::uint16_t>(index));
        append_u16(result, 0);
        append_u32(result, static_cast<std::uint32_t>(frames[index].size()
            + (padding.empty() ? 0 : padding[index])));
    }
    for (std::size_t index = 0; index < frames.size(); ++index) {
        result.insert(result.end(), frames[index].begin(), frames[index].end());
        if (!padding.empty()) {
            result.insert(result.end(), padding[index], std::byte{0});
        }
    }
    return result;
}

Bytes make_pif(const std::vector<std::vector<std::uint16_t>>& rows, std::uint16_t stride) {
    check(stride == 24 || stride == 28, "unsupported synthetic PIF stride");
    Bytes result = bytes({'P', 'I', 'F', 0});
    append_u32(result, stride == 24 ? 2 : 3);
    append_u16(result, 0);
    append_u16(result, stride);
    append_u16(result, static_cast<std::uint16_t>(rows.size()));
    append_u16(result, 2);
    append_u32(result, 0);
    for (const auto& row : rows) {
        check(row.size() == stride / 2, "synthetic PIF row size mismatch");
        for (const std::uint16_t value : row) {
            append_u16(result, value);
        }
    }
    return result;
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

std::pair<Bytes, Bytes> two_frame_apg_and_pif() {
    const Bytes first = make_frame("AP", -3, 7, 2, 1, 0, 0,
        bytes({0x00, 0xf8, 0xe0, 0x07}), {});
    const Bytes second = make_frame("AP", 12, -9, 1, 1, 1, 0,
        bytes({0x1f, 0x00}), {});
    const Bytes apg = make_apg({first, second}, {3, 0});
    const Bytes pif = make_pif({
        {160, 160, 160, 210, 5, 0, 0, 0, 0, 0, 0, 0},
        {160, 160, 160, 210, 5, 120, 0, 0, 0, 0, 0, 0},
    }, 24);
    return {apg, pif};
}

void test_ag_delta_traversal_frame_boundaries_and_anchors() {
    const auto [apg, pif] = two_frame_apg_and_pif();
    const dhsh::ApgAnimation animation = dhsh::ApgDecoder::decode(apg);
    check(animation.header.frame_count == 2 && animation.header.entries.size() == 2,
        "AG frame count or index size");
    check(animation.header.entries[0].delta == 31 && animation.header.entries[1].delta == 26,
        "AG delta values changed");
    check(animation.frames.size() == 2 && animation.frames[0].offset == 24
            && animation.frames[1].offset == 55,
        "AG delta traversal produced wrong frame boundaries");
    check(animation.frames[0].anchor_x == -3 && animation.frames[0].anchor_y == 7
            && animation.frames[1].anchor_x == 12 && animation.frames[1].anchor_y == -9,
        "APG signed x/y anchors changed");
    check(animation.frames[0].image.rgba == bytes({
              255, 0, 0, 255, 0, 255, 0, 255,
          }) && animation.frames[1].image.rgba == bytes({0, 0, 255, 255}),
        "two-frame APG RGBA mismatch");
}

void test_ag_rejects_invalid_deltas_and_frame_extents() {
    const auto [apg, pif] = two_frame_apg_and_pif();
    expect_error([] { static_cast<void>(dhsh::ApgDecoder::decode(bytes({'A', 'G'}))); },
        "short AG header");
    Bytes bad_magic = apg;
    bad_magic[0] = std::byte{'X'};
    expect_error([&] { static_cast<void>(dhsh::ApgDecoder::decode(bad_magic)); },
        "bad AG magic", "APG");
    Bytes short_delta = apg;
    set_u32(short_delta, 12, 27);
    expect_error([&] { static_cast<void>(dhsh::ApgDecoder::decode(short_delta)); },
        "delta shorter than frame", "delta");
    Bytes zero_delta = apg;
    set_u32(zero_delta, 12, 0);
    expect_error([&] { static_cast<void>(dhsh::ApgDecoder::decode(zero_delta)); },
        "zero AG delta", "delta");
    Bytes truncated = apg;
    truncated.pop_back();
    expect_error([&] { static_cast<void>(dhsh::ApgDecoder::decode(truncated)); },
        "truncated final frame", "truncated");
}

void test_ai_frames_require_cti_and_decode_rle8_indices() {
    const Bytes ai = make_frame("AI", 4, -5, 4, 1, 2, 0,
        bytes({0x82, 0, 0x02, 1, 2}), {});
    const Bytes apg = make_apg({ai});
    expect_error([&] { static_cast<void>(dhsh::ApgDecoder::decode(apg)); },
        "AI without CTI", "palette");
    const dhsh::CtiPalette palette = dhsh::CtiPalette::parse(make_cti());
    const dhsh::ApgAnimation animation = dhsh::ApgDecoder::decode(apg, &palette);
    check(animation.frames[0].image.rgba == bytes({
              255, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255,
          }), "AI CTI RGBA mismatch");
    check(animation.frames[0].anchor_x == 4 && animation.frames[0].anchor_y == -5,
        "AI anchor mismatch");
}

void test_jpeg_frames_use_wic_and_apply_rle8_alpha() {
    const Bytes jpeg = make_frame("AP", 0, 0, 2, 1, 5, 2, jpeg_2x1(),
        bytes({0x82, 16}));
    const dhsh::ApgAnimation animation = dhsh::ApgDecoder::decode(make_apg({jpeg}));
    const dhsh::Image& image = animation.frames[0].image;
    check(image.width == 2 && image.height == 1 && image.rgba.size() == 8,
        "JPEG APG dimensions or packing");
    check(std::to_integer<unsigned>(image.rgba[0]) > 100
            && std::to_integer<unsigned>(image.rgba[4]) > 100,
        "JPEG APG did not use WIC pixel decoding");
    check(std::to_integer<unsigned>(image.rgba[3]) == 127
            && std::to_integer<unsigned>(image.rgba[7]) == 127,
        "JPEG APG alpha scaling");
}

void test_pif_normalizes_24_byte_rows_and_default_delay() {
    const auto [apg, data] = two_frame_apg_and_pif();
    const dhsh::Pif pif = dhsh::Pif::parse(data);
    check(pif.count == 3 && pif.height == 28 && pif.width == 2 && pif.data_offset == 20,
        "24-byte PIF header was not normalized");
    check(pif.rows.size() == 2 && pif.rows[0][5] == 0 && pif.rows[1][5] == 120,
        "PIF f5 delay changed");
    check(pif.rows[0][12] == 0 && pif.rows[0][13] == 0
            && pif.rows[1][12] == 0 && pif.rows[1][13] == 0,
        "24-byte PIF row tail was not zero filled");
    check(pif.rows[0][2] == 160 && pif.rows[0][3] == 210,
        "PIF normalized row layout changed");
}

void test_animation_clip_anchors_durations_and_multi_frame_advance() {
    const auto [apg, data] = two_frame_apg_and_pif();
    const dhsh::Pif pif = dhsh::Pif::parse(data);
    dhsh::AnimationClip clip = dhsh::AnimationClip::decode(apg, nullptr, &pif);
    check(clip.frames().size() == 2 && clip.current_frame_index() == 0,
        "animation initial frame");
    check(clip.frames()[0].duration_ms == 80 && clip.frames()[1].duration_ms == 120,
        "animation duration or zero-delay normalization");
    check(clip.frames()[0].anchor_x == -3 && clip.frames()[0].anchor_y == 7
            && clip.frames()[1].anchor_x == 12 && clip.frames()[1].anchor_y == -9,
        "animation anchors differ from APG");
    clip.advance(79);
    check(clip.current_frame_index() == 0, "animation advanced before 80ms");
    clip.advance(1);
    check(clip.current_frame_index() == 1, "animation did not advance at 80ms");
    clip.advance(119);
    check(clip.current_frame_index() == 1, "animation advanced before 120ms");
    clip.advance(1);
    check(clip.current_frame_index() == 0, "animation did not wrap");

    dhsh::AnimationClip skipping = dhsh::AnimationClip::decode(apg, nullptr, &pif);
    skipping.advance(281);
    check(skipping.current_frame_index() == 1, "advance did not preserve remainder across frames");
    skipping.advance(400);
    check(skipping.current_frame_index() == 1, "advance changed the multi-cycle remainder");
    skipping.advance(119);
    check(skipping.current_frame_index() == 0, "advance did not wrap after multiple cycles");
}

void test_animation_without_matching_pif_uses_80ms() {
    const auto [apg, data] = two_frame_apg_and_pif();
    dhsh::AnimationClip clip = dhsh::AnimationClip::decode(apg);
    check(clip.frames()[0].duration_ms == 80 && clip.frames()[1].duration_ms == 80,
        "default APG duration is not 80ms");
    const dhsh::Pif one_row = dhsh::Pif::parse(make_pif({
        {160, 160, 160, 210, 5, 200, 0, 0, 0, 0, 0, 0, 0, 0},
    }, 28));
    clip = dhsh::AnimationClip::decode(apg, nullptr, &one_row);
    check(clip.frames()[0].duration_ms == 80 && clip.frames()[1].duration_ms == 80,
        "non-matching PIF should not provide frame durations");
}

void test_pif_rejects_malformed_tables() {
    expect_error([] { static_cast<void>(dhsh::Pif::parse(bytes({'P', 'I', 'F', 0}))); },
        "short PIF header");
    Bytes bad_magic = make_pif({{0, 0, 0, 0, 0, 80, 0, 0, 0, 0, 0, 0, 0, 0}}, 28);
    bad_magic[0] = std::byte{'X'};
    expect_error([&] { static_cast<void>(dhsh::Pif::parse(bad_magic)); }, "bad PIF magic", "PIF");
    Bytes bad_count = bad_magic;
    bad_count[0] = std::byte{'P'};
    set_u32(bad_count, 4, 4);
    expect_error([&] { static_cast<void>(dhsh::Pif::parse(bad_count)); }, "PIF count above three");
    Bytes bad_stride = bad_count;
    set_u32(bad_stride, 4, 3);
    set_u16(bad_stride, 10, 26);
    expect_error([&] { static_cast<void>(dhsh::Pif::parse(bad_stride)); }, "unsupported PIF stride");
    Bytes truncated = bad_stride;
    set_u16(truncated, 10, 28);
    truncated.pop_back();
    expect_error([&] { static_cast<void>(dhsh::Pif::parse(truncated)); }, "truncated PIF rows", "truncated");
}

void test_apg_decoded_byte_budgets_and_dimension_preflight() {
    const auto [apg, pif] = two_frame_apg_and_pif();
    dhsh::ApgDecodeOptions options;
    options.max_frame_rgba_bytes = 8;
    options.max_animation_rgba_bytes = 12;
    const dhsh::ApgAnimation exact = dhsh::ApgDecoder::decode(apg, nullptr, options);
    check(exact.frames.size() == 2, "exact decoded-byte budgets must be accepted");

    options.max_frame_rgba_bytes = 7;
    expect_error([&] { static_cast<void>(dhsh::ApgDecoder::decode(apg, nullptr, options)); },
        "single-frame RGBA budget", "frame RGBA budget");
    options.max_frame_rgba_bytes = 8;
    options.max_animation_rgba_bytes = 11;
    expect_error([&] { static_cast<void>(dhsh::ApgDecoder::decode(apg, nullptr, options)); },
        "cumulative RGBA budget", "animation RGBA budget");
    expect_error([&] {
        static_cast<void>(dhsh::AnimationClip::decode(apg, nullptr, nullptr, options));
    }, "AnimationClip forwards RGBA budgets", "animation RGBA budget");

    const Bytes oversized_jpeg = make_apg({make_frame("AP", 0, 0, 32767, 32767, 5, 0,
        bytes({0xff, 0xd8, 0xff}), {})});
    expect_error([&] { static_cast<void>(dhsh::ApgDecoder::decode(oversized_jpeg)); },
        "JPEG dimensions must be checked before WIC", "dimensions");

    const Bytes nested_large_jpeg = make_apg({make_frame("AP", 0, 0, 1, 1, 5, 0, jpeg_2x1(), {})});
    dhsh::ApgDecodeOptions nested_options;
    nested_options.max_frame_rgba_bytes = 4;
    nested_options.max_animation_rgba_bytes = 4;
    expect_error([&] {
        static_cast<void>(dhsh::ApgDecoder::decode(nested_large_jpeg, nullptr, nested_options));
    }, "inner JPEG dimensions must obey the APG frame budget", "WIC decoded RGBA budget");
}

void test_empty_apg_and_zero_or_large_advance() {
    const Bytes empty = make_apg({});
    const dhsh::ApgAnimation animation = dhsh::ApgDecoder::decode(empty);
    check(animation.header.frame_count == 0 && animation.frames.empty(), "empty APG decode");
    dhsh::AnimationClip empty_clip = dhsh::AnimationClip::decode(empty);
    empty_clip.advance(0);
    empty_clip.advance(std::numeric_limits<std::uint32_t>::max());
    empty_clip.advance(65535);
    check(empty_clip.frames().empty() && empty_clip.current_frame_index() == 0,
        "empty AnimationClip advance");

    const auto [apg, data] = two_frame_apg_and_pif();
    const dhsh::Pif pif = dhsh::Pif::parse(data);
    dhsh::AnimationClip clip = dhsh::AnimationClip::decode(apg, nullptr, &pif);
    clip.advance(79);
    clip.advance(0);
    check(clip.current_frame_index() == 0, "advance(0) changed the frame or remainder");
    clip.advance(1);
    clip.advance(0);
    check(clip.current_frame_index() == 1, "advance(0) changed the second frame");

    dhsh::AnimationClip large = dhsh::AnimationClip::decode(apg, nullptr, &pif);
    large.advance(std::numeric_limits<std::uint32_t>::max());
    check(large.current_frame_index() == 1, "UINT32_MAX advance overflowed the timeline");
    large.advance(65535);
    check(large.current_frame_index() == 0, "65535ms advance produced the wrong remainder");
}

void test_public_animation_constructor_normalizes_zero_durations() {
    dhsh::AnimationClip clip({
        dhsh::AnimationFrame{dhsh::Image{}, 0, 0, 0},
        dhsh::AnimationFrame{dhsh::Image{}, 0, 0, 0},
    });
    check(clip.frames().size() == 2
            && clip.frames()[0].duration_ms == 80
            && clip.frames()[1].duration_ms == 80,
        "public AnimationClip construction left a zero-duration frame");
    clip.advance(79);
    check(clip.current_frame_index() == 0,
        "normalized first frame advanced before 80ms");
    clip.advance(1);
    check(clip.current_frame_index() == 1,
        "normalized first frame did not advance at 80ms");
    clip.advance(80);
    check(clip.current_frame_index() == 0,
        "normalized zero-duration animation did not wrap safely");
}

void test_apg_header_frame_stream_and_jpeg_dimension_regressions() {
    const auto [apg, pif] = two_frame_apg_and_pif();
    Bytes short_header = apg;
    set_u16(short_header, 2, 23);
    expect_error([&] { static_cast<void>(dhsh::ApgDecoder::decode(short_header)); },
        "AG header shorter than its index", "header");
    Bytes header_beyond = apg;
    set_u16(header_beyond, 2, 0xffff);
    expect_error([&] { static_cast<void>(dhsh::ApgDecoder::decode(header_beyond)); },
        "AG header beyond input", "header");
    Bytes bad_frame_magic = apg;
    bad_frame_magic[24] = std::byte{'X'};
    expect_error([&] { static_cast<void>(dhsh::ApgDecoder::decode(bad_frame_magic)); },
        "bad APG frame magic", "magic");
    Bytes bad_frame_header = apg;
    set_u16(bad_frame_header, 26, 23);
    expect_error([&] { static_cast<void>(dhsh::ApgDecoder::decode(bad_frame_header)); },
        "short APG frame header", "header");
    Bytes overflowing_stream = apg;
    set_u32(overflowing_stream, 40, 0xffffffffU);
    expect_error([&] { static_cast<void>(dhsh::ApgDecoder::decode(overflowing_stream)); },
        "overflowing APG stream extent", "delta");

    const Bytes wrong_jpeg_size = make_apg({make_frame("AP", 0, 0, 1, 1, 5, 0, jpeg_2x1(), {})});
    expect_error([&] { static_cast<void>(dhsh::ApgDecoder::decode(wrong_jpeg_size)); },
        "JPEG dimensions differ from APG header", "dimensions");
    const Bytes zero_width = make_apg({make_frame("AP", 0, 0, 0, 1, 1, 0, bytes({0, 0}), {})});
    expect_error([&] { static_cast<void>(dhsh::ApgDecoder::decode(zero_width)); },
        "zero APG width", "dimensions");
    const Bytes negative_width = make_apg({make_frame("AP", 0, 0, 0x8000, 1, 1, 0,
        bytes({0, 0}), {})});
    expect_error([&] { static_cast<void>(dhsh::ApgDecoder::decode(negative_width)); },
        "APG width follows A16 signed-positive contract", "dimensions");
}

void test_pif_data_offset_and_zero_width_regressions() {
    Bytes beyond = make_pif({{0, 0, 0, 0, 0, 80, 0, 0, 0, 0, 0, 0, 0, 0}}, 28);
    set_u16(beyond, 8, 49);
    expect_error([&] { static_cast<void>(dhsh::Pif::parse(beyond)); },
        "PIF data offset beyond input", "truncated");
    Bytes zero_width = make_pif({{0, 0, 0, 0, 0, 80, 0, 0, 0, 0, 0, 0, 0, 0}}, 28);
    set_u16(zero_width, 12, 0);
    expect_error([&] { static_cast<void>(dhsh::Pif::parse(zero_width)); },
        "zero PIF width", "dimensions");

    Bytes valid_offset = make_pif({{160, 160, 160, 210, 5, 120, 0, 0, 0, 0, 0, 0, 0, 0}}, 28);
    valid_offset.insert(valid_offset.begin() + 20, 4, std::byte{0x5a});
    set_u16(valid_offset, 8, 24);
    const dhsh::Pif shifted = dhsh::Pif::parse(valid_offset);
    check(shifted.data_offset == 24 && shifted.rows.size() == 1
            && shifted.rows[0][0] == 160 && shifted.rows[0][5] == 120,
        "legal nonzero PIF data offset changed the row layout");
}

void test_animation_65535ms_frame_duration() {
    const auto [apg, data] = two_frame_apg_and_pif();
    const dhsh::Pif pif = dhsh::Pif::parse(make_pif({
        {160, 160, 160, 210, 5, 65535, 0, 0, 0, 0, 0, 0, 0, 0},
        {160, 160, 160, 210, 5, 80, 0, 0, 0, 0, 0, 0, 0, 0},
    }, 28));
    dhsh::AnimationClip clip = dhsh::AnimationClip::decode(apg, nullptr, &pif);
    clip.advance(65534);
    check(clip.current_frame_index() == 0, "65535ms frame ended early");
    clip.advance(1);
    check(clip.current_frame_index() == 1, "65535ms frame did not end at its boundary");
    clip.advance(79);
    check(clip.current_frame_index() == 1, "80ms frame ended early after 65535ms frame");
    clip.advance(1);
    check(clip.current_frame_index() == 0, "65535ms and 80ms cycle did not wrap");
}

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open animation fixture");
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("cannot determine animation fixture size");
    }
    Bytes data(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!data.empty()) {
        input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (!input) {
        throw std::runtime_error("cannot read animation fixture");
    }
    return data;
}

void write_file(const std::filesystem::path& path, const Bytes& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create animation output");
    }
    if (!data.empty()) {
        output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (!output) {
        throw std::runtime_error("cannot write animation output");
    }
}

int fixture_main(int argc, char** argv) {
    if (argc < 4 || argc > 6 || std::string_view(argv[1]) != "--apg") {
        return 2;
    }
    const Bytes apg = read_file(argv[2]);
    const std::filesystem::path prefix = argv[3];
    dhsh::CtiPalette palette;
    const dhsh::CtiPalette* palette_pointer = nullptr;
    if (argc >= 5 && std::string_view(argv[4]) != "-") {
        palette = dhsh::CtiPalette::parse(read_file(argv[4]));
        palette_pointer = &palette;
    }
    dhsh::Pif pif;
    const dhsh::Pif* pif_pointer = nullptr;
    if (argc >= 6) {
        pif = dhsh::Pif::parse(read_file(argv[5]));
        pif_pointer = &pif;
    }
    const dhsh::AnimationClip clip = dhsh::AnimationClip::decode(apg, palette_pointer, pif_pointer);
    std::ofstream metadata(prefix.string() + ".meta", std::ios::trunc);
    metadata << clip.frames().size() << '\n';
    for (std::size_t index = 0; index < clip.frames().size(); ++index) {
        const auto& frame = clip.frames()[index];
        metadata << index << ' ' << frame.image.width << ' ' << frame.image.height << ' '
            << frame.anchor_x << ' ' << frame.anchor_y << ' ' << frame.duration_ms << '\n';
        write_file(prefix.string() + "." + std::to_string(index) + ".rgba", frame.image.rgba);
    }
    return metadata ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 1) {
        try {
            return fixture_main(argc, argv);
        } catch (const std::exception& error) {
            std::cerr << "animation fixture error: " << error.what() << '\n';
            return 1;
        }
    }

    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"AG delta traversal, frame boundaries, and anchors", test_ag_delta_traversal_frame_boundaries_and_anchors},
        {"AG invalid deltas and frame extents", test_ag_rejects_invalid_deltas_and_frame_extents},
        {"AI CTI requirement and RLE8 indices", test_ai_frames_require_cti_and_decode_rle8_indices},
        {"JPEG WIC decoding and RLE8 alpha", test_jpeg_frames_use_wic_and_apply_rle8_alpha},
        {"PIF 24-byte normalization and delays", test_pif_normalizes_24_byte_rows_and_default_delay},
        {"AnimationClip anchors, durations, and multi-frame advance", test_animation_clip_anchors_durations_and_multi_frame_advance},
        {"AnimationClip default durations", test_animation_without_matching_pif_uses_80ms},
        {"PIF malformed tables", test_pif_rejects_malformed_tables},
        {"APG decoded-byte budgets and dimension preflight", test_apg_decoded_byte_budgets_and_dimension_preflight},
        {"Empty APG and zero or large advance", test_empty_apg_and_zero_or_large_advance},
        {"public constructor zero-duration normalization", test_public_animation_constructor_normalizes_zero_durations},
        {"APG header, frame, stream, and JPEG dimension regressions", test_apg_header_frame_stream_and_jpeg_dimension_regressions},
        {"PIF data offset and zero width regressions", test_pif_data_offset_and_zero_width_regressions},
        {"AnimationClip 65535ms frame duration", test_animation_65535ms_frame_duration},
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
