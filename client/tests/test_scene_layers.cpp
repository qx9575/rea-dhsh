#include "animation/AnimationClip.h"
#include "formats/MapFormats.h"
#include "formats/Pif.h"
#include "render/Image.h"
#include "resource/ResourceError.h"
#include "scene/CoverMask.h"
#include "scene/SceneObject.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

void set_u32(Bytes& output, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output[offset + shift / 8] = static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

dhsh::Image image(std::initializer_list<unsigned> rgba) {
    check(rgba.size() == 4, "test image must contain one pixel");
    dhsh::Image result;
    result.width = 1;
    result.height = 1;
    for (const unsigned value : rgba) {
        result.rgba.push_back(static_cast<std::byte>(value));
    }
    return result;
}

dhsh::MapObjectFrame frame(unsigned red, unsigned green, unsigned blue) {
    dhsh::MapObjectFrame result;
    result.image = image({red, green, blue, 255});
    return result;
}

dhsh::ManEntry entry(std::string path, std::int32_t y,
    std::int32_t f48, std::int32_t f58 = 0) {
    dhsh::ManEntry result;
    result.path = std::move(path);
    result.y = y;
    result.f48 = f48;
    result.f58 = f58;
    return result;
}

void test_man_depth_formula_drives_stable_ordering() {
    std::vector<dhsh::SceneObject> objects;
    objects.emplace_back(entry("first", 10, 5, 256), frame(1, 2, 3));
    objects.emplace_back(entry("second", 18, 0), frame(4, 5, 6));
    objects.emplace_back(entry("third", 18, 0), frame(7, 8, 9));

    check(objects[0].sort_y(4) == 19, "MAN sortY must include (f58*arg)>>8");
    dhsh::SceneObject::sort_by_depth(objects, 0);
    check(objects[0].entry.path == "first" && objects[1].entry.path == "second"
            && objects[2].entry.path == "third",
        "MAN objects were not sorted by their initial depth");

    dhsh::SceneObject::sort_by_depth(objects, 4);
    check(objects[0].entry.path == "second" && objects[1].entry.path == "third"
            && objects[2].entry.path == "first",
        "MAN slope argument did not affect draw order");
    dhsh::SceneObject::sort_by_depth(objects, 4);
    check(objects[0].entry.path == "second" && objects[1].entry.path == "third",
        "equal-depth MAN objects did not retain their stable order");

    dhsh::ManEntry negative = entry("negative", -20, -7, -129);
    check(dhsh::SceneObject(negative, frame(0, 0, 0)).sort_y(3) == -29,
        "MAN depth must use a signed arithmetic shift");
}

Bytes make_frame(std::int16_t x, std::int16_t y, unsigned color) {
    Bytes result = bytes({'A', 'P'});
    append_u16(result, 24);
    append_u16(result, static_cast<std::uint16_t>(x));
    append_u16(result, static_cast<std::uint16_t>(y));
    append_u16(result, 1);
    append_u16(result, 1);
    append_u16(result, 0);
    result.push_back(std::byte{0});
    result.push_back(std::byte{0});
    append_u32(result, 2);
    append_u32(result, 0);
    append_u16(result, static_cast<std::uint16_t>(color));
    return result;
}

Bytes make_apg(const Bytes& first, const Bytes& second) {
    Bytes result = bytes({'A', 'G'});
    append_u16(result, 24);
    append_u16(result, 2);
    append_u16(result, 0);
    append_u16(result, 0);
    append_u16(result, 0);
    append_u32(result, static_cast<std::uint32_t>(first.size()));
    append_u16(result, 1);
    append_u16(result, 0);
    append_u32(result, static_cast<std::uint32_t>(second.size()));
    result.insert(result.end(), first.begin(), first.end());
    result.insert(result.end(), second.begin(), second.end());
    return result;
}

Bytes make_pif() {
    Bytes result = bytes({'P', 'I', 'F', 0});
    append_u32(result, 3);
    append_u16(result, 0);
    append_u16(result, 28);
    append_u16(result, 2);
    append_u16(result, 2);
    append_u32(result, 0);
    for (const std::uint16_t delay : {80, 120}) {
        for (std::size_t column = 0; column < 14; ++column) {
            append_u16(result, column == 5 ? delay : 0);
        }
    }
    return result;
}

void test_scene_object_uses_pif_delays_and_current_anchors() {
    const Bytes apg = make_apg(make_frame(-3, 7, 0xf800), make_frame(12, -9, 0x001f));
    const dhsh::Pif pif = dhsh::Pif::parse(make_pif());
    dhsh::ManEntry animated = entry("animated", 200, 0);
    animated.x = 100;
    dhsh::SceneObject object(animated,
        dhsh::AnimationClip::decode(apg, nullptr, &pif));

    check(object.current_frame_index() == 0 && object.x == 97 && object.y == 207,
        "initial APG frame anchor was not applied to the MAN position");
    check(!object.advance(0) && object.current_frame_index() == 0,
        "advance(0) changed the initial animation frame");
    check(!object.advance(79) && object.current_frame_index() == 0,
        "PIF's 80ms first frame ended early");
    check(object.advance(1) && object.current_frame_index() == 1
            && object.x == 112 && object.y == 191,
        "PIF's 80ms first frame did not advance to the second anchor");
    check(!object.advance(119) && object.current_frame_index() == 1,
        "PIF's 120ms second frame ended early");
    check(object.advance(1) && object.current_frame_index() == 0
            && object.x == 97 && object.y == 207,
        "PIF animation did not wrap to the first frame");

    dhsh::SceneObject skipping(animated,
        dhsh::AnimationClip::decode(apg, nullptr, &pif));
    check(skipping.advance(281) && skipping.current_frame_index() == 1,
        "multi-cycle advance did not preserve the frame remainder");
    check(!skipping.advance(0) && skipping.current_frame_index() == 1,
        "advance(0) changed a later animation frame");
    check(!skipping.advance(400) && skipping.current_frame_index() == 1,
        "whole animation cycles changed the current frame");
    check(skipping.advance(119) && skipping.current_frame_index() == 0,
        "long-running animation did not wrap at the correct remainder");

    dhsh::SceneObject large(animated,
        dhsh::AnimationClip::decode(apg, nullptr, &pif));
    check(large.advance(std::numeric_limits<std::uint32_t>::max())
            && large.current_frame_index() == 1,
        "UINT32_MAX advance overflowed the scene animation timeline");
}

void test_scene_object_handles_empty_frames_and_clip() {
    dhsh::ManEntry animated = entry("empty", 200, 0);
    animated.x = 100;
    dhsh::SceneObject from_frames(animated, dhsh::SceneAnimation{});
    Bytes empty_apg = bytes({'A', 'G'});
    append_u16(empty_apg, 8);
    append_u16(empty_apg, 0);
    append_u16(empty_apg, 0);
    dhsh::SceneObject from_clip(animated, dhsh::AnimationClip::decode(empty_apg));
    for (dhsh::SceneObject* object : {&from_frames, &from_clip}) {
        check(object->animation() != nullptr && object->animation()->empty(),
            "empty scene animation was not retained");
        check(object->current_frame_index() == 0 && object->frame.image.rgba.empty()
                && object->x == 100 && object->y == 200,
            "empty scene animation did not use a safe empty frame");
        check(!object->advance(0)
                && !object->advance(std::numeric_limits<std::uint32_t>::max()),
            "empty scene animation changed during advance");
    }
}

Bytes make_core() {
    const std::vector<std::uint16_t> runs = {
        0x0001, 0x8002,
        0x8001, 0x0002,
    };
    Bytes result = bytes({'M', 'S', 'K', 0});
    append_u32(result, 5);
    append_u32(result, 0);
    append_u32(result, 0);
    append_u32(result, 40);
    append_u32(result, 28);
    append_u32(result, 1);
    append_u32(result, 68);
    append_u32(result, 24);
    append_u32(result, 1);
    append_u32(result, 3);
    append_u32(result, 2);
    append_u32(result, 7);
    append_u32(result, 1);
    append_u32(result, 92);
    append_u32(result, 8);
    append_u32(result, 128);
    append_u32(result, 0);
    append_u32(result, 10);
    append_u32(result, 20);
    append_u32(result, 1);
    append_u32(result, 4);
    append_u32(result, 2);
    for (const std::uint16_t run : runs) {
        append_u16(result, run);
    }
    set_u32(result, 8, static_cast<std::uint32_t>(result.size()));
    return result;
}

dhsh::Image character() {
    dhsh::Image result;
    result.width = 3;
    result.height = 2;
    result.rgba = bytes({
        80, 72, 64, 255, 88, 81, 73, 128, 96, 90, 82, 255,
        104, 99, 91, 255, 112, 108, 100, 255, 120, 117, 109, 255,
    });
    return result;
}

Bytes packed16_darkened_rgb(unsigned red, unsigned green, unsigned blue) {
    const std::uint16_t pixel = static_cast<std::uint16_t>(
        (((red * 31U + 127U) / 255U) << 11)
        | (((green * 63U + 127U) / 255U) << 5)
        | ((blue * 31U + 127U) / 255U));
    const std::int16_t signed_pixel = static_cast<std::int16_t>(pixel);
    const std::uint16_t darkened = static_cast<std::uint16_t>(
        (signed_pixel >> 3) & 0x1f1f);
    return bytes({
        (((darkened >> 11) & 0x1fU) * 255U) / 31U,
        (((darkened >> 5) & 0x3fU) * 255U) / 63U,
        (darkened & 0x1fU) * 255U / 31U,
    });
}

void test_core_depth_and_rle_ghost_darkening() {
    const dhsh::Core core = dhsh::Core::parse(make_core());
    check(core.sort_y(0) == 22, "core sortY fixture does not match the documented formula");
    dhsh::CoverMask mask(core, 0);
    check(mask.active(), "static core flag=1 must be enabled in M1");
    check(mask.covers(12, 27), "core pixel below the strict depth boundary was skipped");
    check(!mask.covers(12, 28), "core depth comparison must be y < boundary, not <=");

    dhsh::Image actual = character();
    const dhsh::Image original = actual;
    check(mask.apply(actual, 10, 20, 10, 20), "covered character pixels were not darkened");
    check(packed16_darkened_rgb(80, 72, 64) != bytes({10, 9, 8}),
        "packed16 reference accidentally reproduced the old RGBA shift");
    const dhsh::Image expected = [&] {
        dhsh::Image value = character();
        for (const std::size_t pixel : {1, 2, 3}) {
            const std::size_t offset = pixel * 4;
            const Bytes darkened = packed16_darkened_rgb(
                std::to_integer<unsigned>(original.rgba[offset]),
                std::to_integer<unsigned>(original.rgba[offset + 1]),
                std::to_integer<unsigned>(original.rgba[offset + 2]));
            std::copy_n(darkened.begin(), 3, value.rgba.begin() + offset);
        }
        return value;
    }();
    check(actual.rgba == expected.rgba,
        "ApplyMask must match packed16 signed shift/mask semantics and preserve alpha");
    for (const std::size_t pixel : {0, 4, 5}) {
        const std::size_t offset = pixel * 4;
        check(std::equal(actual.rgba.begin() + offset,
                actual.rgba.begin() + offset + 4, original.rgba.begin() + offset),
            "an uncovered character pixel was changed");
    }

    dhsh::Image behind = character();
    check(!mask.apply(behind, 12, 28, 12, 28) && behind.rgba == original.rgba,
        "a character at or behind the core ground line was darkened");
    dhsh::Image disjoint = character();
    check(!mask.apply(disjoint, 120, 27, 120, 27) && disjoint.rgba == original.rgba,
        "a depth-passing but disjoint sprite was changed");
}

void test_actor_depth_is_separate_from_sprite_origin() {
    const dhsh::Core core = dhsh::Core::parse(make_core());
    dhsh::CoverMask mask(core, 0);
    dhsh::Image actual = character();
    check(mask.apply(actual, 10, 26, 9, 19),
        "a depth-passing actor with an anchored sprite did not intersect the mask");
    const Bytes expected = bytes({
        80, 72, 64, 255, 88, 81, 73, 128, 96, 90, 82, 255,
        104, 99, 91, 255, 112, 108, 100, 255, 8, 226, 172, 255,
    });
    if (actual.rgba != expected) {
        for (const std::byte value : actual.rgba) {
            std::cerr << std::to_integer<unsigned>(value) << ' ';
        }
        std::cerr << '\n';
    }
    check(actual.rgba == expected,
        "bitmap intersection must use left/top while depth uses the actor's logical x/y");

    dhsh::Image at_boundary = character();
    check(!mask.apply(at_boundary, 10, 27, 9, 19)
            && at_boundary.rgba == character().rgba,
        "the sprite origin must not replace the actor's logical depth coordinate");
}

void test_cover_mask_rejects_extreme_image_dimensions() {
    const dhsh::Core core = dhsh::Core::parse(make_core());
    dhsh::CoverMask mask(core, 0);
    dhsh::Image oversized;
    oversized.width = 0x80000000U;
    oversized.height = 0x80000000U;
    try {
        (void)mask.apply(oversized, 10, 20, 0, 0);
    } catch (const dhsh::FormatError&) {
        return;
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "extreme image dimensions produced the wrong exception: " + std::string(error.what()));
    }
    throw std::runtime_error("extreme image dimensions bypassed RGBA size validation");
}

void test_placement_index_is_not_a_record_index() {
    Bytes data = make_core();
    data.insert(data.begin() + 92, 24, std::byte{0});
    set_u32(data, 8, static_cast<std::uint32_t>(data.size()));
    set_u32(data, 36, 2);
    set_u32(data, 56, 116);
    set_u32(data, 96, 10);
    set_u32(data, 100, 20);
    set_u32(data, 104, 1);
    const dhsh::Core core = dhsh::Core::parse(data);
    dhsh::CoverMask second(core, 1);
    dhsh::Image actual = character();
    check(second.apply(actual, 10, 20, 10, 20),
        "a placement must decode its referenced record, not the record at the placement index");
}

void test_group_frame_masks_stay_inactive_until_selected() {
    Bytes data = make_core();
    set_u32(data, 80, 0);
    const dhsh::Core core = dhsh::Core::parse(data);
    dhsh::CoverMask mask(core, 0);
    check(!mask.active() && mask.group() == 4 && mask.frame() == 2,
        "group/frame state was invented for a static-only scene");
    dhsh::Image actual = character();
    check(!mask.apply(actual, 10, 20, 10, 20) && actual.rgba == character().rgba,
        "an unselected group/frame mask darkened the character");
    mask.set_selected(true);
    check(mask.active() && mask.apply(actual, 10, 20, 10, 20),
        "the explicit group/frame selection interface did not activate the mask");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests = {
        {"MAN depth formula and stable ordering", test_man_depth_formula_drives_stable_ordering},
        {"SceneObject PIF delays and anchors", test_scene_object_uses_pif_delays_and_current_anchors},
        {"SceneObject empty animations", test_scene_object_handles_empty_frames_and_clip},
        {"core depth and RLE ghost darkening", test_core_depth_and_rle_ghost_darkening},
        {"actor depth and anchored sprite origin", test_actor_depth_is_separate_from_sprite_origin},
        {"extreme image dimensions", test_cover_mask_rejects_extreme_image_dimensions},
        {"core placement and record indices", test_placement_index_is_not_a_record_index},
        {"core group/frame selection", test_group_frame_masks_stay_inactive_until_selected},
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
