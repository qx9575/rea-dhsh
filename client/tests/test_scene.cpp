#include "formats/MapFormats.h"
#include "render/Image.h"
#include "render/SoftwareRenderer.h"
#include "resource/CachedImageProvider.h"
#include "resource/PakResourceProvider.h"
#include "resource/ResourceError.h"
#include "resource/ResourceProvider.h"
#include "scene/MapScene.h"
#include "scene/PakManObjectProvider.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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

std::string ascii_lower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char value : text) {
        result.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(value))));
    }
    return result;
}

bool contains_ci(std::string_view text, std::string_view fragment) {
    return ascii_lower(text).find(ascii_lower(fragment)) != std::string::npos;
}

// Mirrors PakResourceProvider's logical path normalization so the in-memory
// fake behaves like the real archive provider used by the client.
std::string normalize_logical_path(std::string_view path) {
    std::string result;
    result.reserve(path.size());
    for (const char value : path) {
        const unsigned char lowered = static_cast<unsigned char>(
            std::tolower(static_cast<unsigned char>(value)));
        if (value == '\\' || value == '/') {
            if (!result.empty() && result.back() != '/') {
                result.push_back('/');
            }
        } else {
            result.push_back(static_cast<char>(lowered));
        }
    }
    while (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    return result;
}

// Literal path, map1 sibling, and the stripped "map/" form, matching
// tools/map_preview.py. Only the first existing variant is served.
std::vector<std::string> asset_variants(std::string_view path) {
    const std::string normalized = normalize_logical_path(path);
    std::vector<std::string> result = {normalized};
    if (normalized.rfind("map/", 0) == 0) {
        const std::string rest = normalized.substr(4);
        result.push_back("map1/" + rest);
        result.push_back(rest);
    }
    return result;
}

Bytes nz_wrapped(const Bytes& payload) {
    check(payload.size() <= 0xffffffffU, "NZ test payload is too large");
    Bytes result = bytes({'N', 'Z', 0, 0});
    append_u32(result, static_cast<std::uint32_t>(payload.size()));
    append_u32(result, static_cast<std::uint32_t>(payload.size()));
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

// Tiny in-memory image format used by the fake decoder:
//   byte 0 = width, byte 1 = height, then width*height RGBA bytes.
Bytes image_payload(
    std::uint32_t width,
    std::uint32_t height,
    const std::vector<unsigned>& rgba) {
    check(width <= 0xffU && height <= 0xffU, "test image dimensions must fit one byte");
    check(rgba.size() == static_cast<std::size_t>(width) * height * 4,
        "test image pixel count mismatch");
    Bytes result;
    result.reserve(2 + rgba.size());
    result.push_back(static_cast<std::byte>(width));
    result.push_back(static_cast<std::byte>(height));
    for (const unsigned value : rgba) {
        check(value <= 0xffU, "test image channel is out of range");
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

dhsh::Image decode_test_image(std::span<const std::byte> data) {
    if (data.size() < 2) {
        throw std::runtime_error("test image payload is truncated");
    }
    dhsh::Image image;
    image.width = std::to_integer<std::uint32_t>(data[0]);
    image.height = std::to_integer<std::uint32_t>(data[1]);
    const std::size_t expected = 2
        + static_cast<std::size_t>(image.width) * image.height * 4;
    if (data.size() != expected) {
        throw std::runtime_error("test image payload size does not match its header");
    }
    image.rgba.assign(data.begin() + 2, data.end());
    return image;
}

dhsh::Image make_test_image(
    std::uint32_t width,
    std::uint32_t height,
    const std::vector<unsigned>& rgba) {
    const Bytes payload = image_payload(width, height, rgba);
    return decode_test_image(std::span<const std::byte>(payload));
}

std::vector<unsigned> solid_pixels(
    std::uint32_t width,
    std::uint32_t height,
    unsigned red,
    unsigned green,
    unsigned blue,
    unsigned alpha = 255) {
    std::vector<unsigned> rgba(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t index = 0; index < rgba.size(); index += 4) {
        rgba[index] = red;
        rgba[index + 1] = green;
        rgba[index + 2] = blue;
        rgba[index + 3] = alpha;
    }
    return rgba;
}

void check_pixel(
    const dhsh::Image& image,
    std::uint32_t x,
    std::uint32_t y,
    std::initializer_list<unsigned> expected,
    const std::string& message) {
    check(x < image.width && y < image.height, message + ": pixel is outside the image");
    const std::size_t offset = (static_cast<std::size_t>(y) * image.width + x) * 4;
    check(offset + 4 <= image.rgba.size(), message + ": image data is truncated");
    const std::array<unsigned, 4> actual = {
        std::to_integer<unsigned>(image.rgba[offset]),
        std::to_integer<unsigned>(image.rgba[offset + 1]),
        std::to_integer<unsigned>(image.rgba[offset + 2]),
        std::to_integer<unsigned>(image.rgba[offset + 3]),
    };
    std::size_t channel = 0;
    for (const unsigned value : expected) {
        check(channel < actual.size(), message + ": too many expected channels");
        if (actual[channel] != value) {
            throw std::runtime_error(message + ": channel " + std::to_string(channel)
                + " expected " + std::to_string(value) + " but got "
                + std::to_string(actual[channel]));
        }
        ++channel;
    }
    check(channel == actual.size(), message + ": expected all four RGBA channels");
}

class FakeResourceProvider final : public dhsh::ResourceProvider {
public:
    std::unordered_map<std::string, Bytes> files;
    std::size_t byte_calls = 0;
    mutable std::size_t exists_calls = 0;
    std::vector<std::string> requests;

    std::vector<std::byte> bytes(std::string_view logical_path) override {
        ++byte_calls;
        const std::string key = normalize_logical_path(logical_path);
        requests.push_back(key);
        const auto found = files.find(key);
        if (found == files.end()) {
            throw dhsh::ResourceError("missing resource: " + std::string(logical_path));
        }
        return found->second;
    }

    bool exists(std::string_view logical_path) const override {
        ++exists_calls;
        return files.find(normalize_logical_path(logical_path)) != files.end();
    }

    void put(std::string_view logical_path, Bytes data) {
        files[normalize_logical_path(logical_path)] = std::move(data);
    }

    void erase(std::string_view logical_path) {
        files.erase(normalize_logical_path(logical_path));
    }
};

void erase_asset(FakeResourceProvider& resources, std::string_view path) {
    for (const std::string& variant : asset_variants(path)) {
        resources.files.erase(variant);
    }
}

class FakeManObjectProvider final : public dhsh::ManObjectProvider {
public:
    std::unordered_map<std::string, dhsh::MapObjectFrame> frames;
    std::size_t calls = 0;

    std::optional<dhsh::MapObjectFrame> load_first_frame(
        const dhsh::ManEntry& entry) override {
        ++calls;
        const auto found = frames.find(entry.path);
        if (found == frames.end()) {
            return std::nullopt;
        }
        return found->second;
    }
};

template <typename Callback>
void expect_resource_error(
    Callback&& callback,
    const std::string& message,
    std::string_view fragment = {}) {
    try {
        std::forward<Callback>(callback)();
    } catch (const dhsh::ResourceError& error) {
        if (!fragment.empty() && !contains_ci(error.what(), fragment)) {
            throw std::runtime_error(message + ": error does not mention '"
                + std::string(fragment) + "': " + error.what());
        }
        return;
    } catch (const std::exception& error) {
        throw std::runtime_error(message + ": wrong exception: " + error.what());
    }
    throw std::runtime_error(message + ": no exception");
}

Bytes background_payload() {
    return image_payload(4, 3, {
        10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 255, 40, 0, 0, 255,
        50, 0, 0, 255, 60, 0, 0, 255, 70, 0, 0, 255, 80, 0, 0, 255,
        90, 0, 0, 255, 100, 0, 0, 255, 110, 0, 0, 255, 120, 0, 0, 255,
    });
}

constexpr std::string_view background_asset = "map\\YingTianFu\\YingTianFu.jpg";
constexpr std::string_view cel_asset = "map\\YingTianFu\\YingTianFu.CEL";
constexpr std::string_view present_object_path = "map\\animate\\YingTianFu\\present\\001";
constexpr std::string_view missing_object_path = "map\\animate\\YingTianFu\\missing\\001";

Bytes make_map_ini() {
    return text_bytes(
        "[BASE]\r\n"
        "WIDTH=64\r\n"
        "HEIGHT=48\r\n"
        "BACKGROUND=map\\YingTianFu\\YingTianFu.jpg\r\n"
        "CELLFILE=map\\YingTianFu\\YingTianFu.CEL\r\n"
        "MASKFILE=map\\YingTianFu\\YingTianFu.core\r\n"
        "[EXT]\r\n"
        "ANIFILE=map\\YingTianFu\\YingTianFu.MAN\r\n");
}

Bytes make_cel() {
    const Bytes ground = bytes({0x80, 0x40, 0x00});
    const Bytes auxiliary = bytes({0xff, 0xff, 0xff});
    Bytes result = bytes({'C', 'E', 'L', 0});
    append_u32(result, 2);
    append_u32(result, 0);
    append_u32(result, 1);
    append_u32(result, 4);
    append_u32(result, 3);
    result.insert(result.end(), ground.begin(), ground.end());
    result.insert(result.end(), auxiliary.begin(), auxiliary.end());
    set_u32(result, 8, static_cast<std::uint32_t>(result.size()));
    return result;
}

Bytes make_man() {
    Bytes result = bytes({'M', 'A', 'N', 0});
    append_u32(result, 4);
    append_u32(result, 0);
    append_u32(result, 0);
    append_u32(result, 28);
    append_u32(result, 100);
    append_u32(result, 2);
    const auto entry = [&](std::string_view path, std::int32_t x, std::int32_t y) {
        append_fixed_string(result, path, 64);
        for (const std::int32_t value : {x, y, 0, 0, 0, 0, 0, 0, 0}) {
            append_u32(result, static_cast<std::uint32_t>(value));
        }
    };
    entry(present_object_path, 1, 1);
    entry(missing_object_path, 30, 40);
    set_u32(result, 8, static_cast<std::uint32_t>(result.size()));
    return result;
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

void add_valid_map(FakeResourceProvider& resources, const Bytes& background) {
    resources.put("map/YingTianFu.map", nz_wrapped(make_map_ini()));
    resources.put(background_asset, background);
    resources.put(cel_asset, nz_wrapped(make_cel()));
    resources.put("map/YingTianFu/YingTianFu.MAN", nz_wrapped(make_man()));
    resources.put("map/YingTianFu/YingTianFu.core", nz_wrapped(make_core()));
}

dhsh::Image solid_image(
    std::uint32_t width,
    std::uint32_t height,
    unsigned red,
    unsigned green,
    unsigned blue,
    unsigned alpha = 255) {
    return make_test_image(width, height,
        solid_pixels(width, height, red, green, blue, alpha));
}

void fill_black(dhsh::SoftwareRenderer& renderer) {
    check(renderer.draw(solid_image(renderer.width(), renderer.height(), 0, 0, 0),
              0, 0),
        "black viewport fill failed");
}

void test_renderer_clips_background_and_sprites_to_viewport() {
    // draw() returns true only when at least one destination pixel changes.
    dhsh::SoftwareRenderer renderer(4, 3);
    check(renderer.target().width == 4 && renderer.target().height == 3,
        "renderer framebuffer dimensions changed");
    check(renderer.draw(solid_image(4, 3, 0, 0, 0), 0, 0), "opaque background draw failed");

    const dhsh::Image background = make_test_image(2, 2, {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 255,
    });
    check(renderer.draw(background, -1, -1), "top-left clipped background was not drawn");
    check_pixel(renderer.target(), 0, 0, {255, 255, 255, 255},
        "top-left clipped source pixel changed");
    check_pixel(renderer.target(), 1, 0, {0, 0, 0, 255},
        "clipped background wrote outside its source rectangle");

    const dhsh::Image sprite = solid_image(2, 2, 255, 255, 0);
    check(renderer.draw(sprite, 3, 2), "bottom-right clipped sprite was not drawn");
    check_pixel(renderer.target(), 3, 2, {255, 255, 0, 255},
        "bottom-right clipped source pixel changed");

    check(renderer.draw(make_test_image(2, 1, {
              1, 2, 3, 255, 4, 5, 6, 255,
          }), -1, 2),
        "left-clipped sprite was not drawn");
    check_pixel(renderer.target(), 0, 2, {4, 5, 6, 255},
        "left-clipped source column changed");
}

void test_renderer_is_safe_outside_the_viewport() {
    dhsh::SoftwareRenderer renderer(4, 3);
    fill_black(renderer);
    const dhsh::Image sprite = solid_image(2, 2, 255, 255, 0);
    const dhsh::Image before = renderer.target();

    check(!renderer.draw(sprite, -2, 0), "fully left sprite reported a draw");
    check(!renderer.draw(sprite, 4, 0), "fully right sprite reported a draw");
    check(!renderer.draw(sprite, 0, 3), "fully below sprite reported a draw");
    check(!renderer.draw(sprite,
              std::numeric_limits<std::int32_t>::min(),
              std::numeric_limits<std::int32_t>::min()),
        "INT32_MIN placement reported a draw");
    check(!renderer.draw(sprite, std::numeric_limits<std::int32_t>::max(), 0),
        "INT32_MAX placement reported a draw");
    check(!renderer.draw(dhsh::Image{}, 0, 0), "empty image reported a draw");
    check(renderer.target().rgba == before.rgba,
        "out-of-viewport draws changed the framebuffer");
}

void test_renderer_blends_source_alpha_and_preserves_transparent_pixels() {
    // RGB = (src * alpha + dst * (255 - alpha) + 127) / 255, matching
    // map_preview.py's blend_sprite. Alpha 0 leaves the destination untouched.
    dhsh::SoftwareRenderer renderer(2, 1);
    check(renderer.draw(make_test_image(2, 1, {
              10, 20, 30, 255, 100, 110, 120, 255,
          }), 0, 0),
        "opaque destination draw failed");
    check(renderer.draw(make_test_image(2, 1, {
              200, 100, 50, 128, 255, 0, 0, 0,
          }), 0, 0),
        "alpha sprite draw failed");
    check_pixel(renderer.target(), 0, 0, {105, 60, 40, 255},
        "source-over alpha blend changed");
    check_pixel(renderer.target(), 1, 0, {100, 110, 120, 255},
        "fully transparent source pixel changed the destination");

    check(renderer.draw(make_test_image(1, 1, {1, 2, 3, 255}), 1, 0),
        "opaque single-pixel sprite draw failed");
    check_pixel(renderer.target(), 1, 0, {1, 2, 3, 255},
        "opaque source pixel did not replace the destination");

    check(!renderer.draw(make_test_image(1, 1, {9, 9, 9, 0}), 0, 0),
        "fully transparent draw reported a change");
    check_pixel(renderer.target(), 0, 0, {105, 60, 40, 255},
        "fully transparent draw changed the destination");
}

void test_cached_image_provider_reuses_decoded_images_by_logical_path() {
    FakeResourceProvider resources;
    resources.put("map/one", image_payload(2, 1, {
        1, 2, 3, 4, 5, 6, 7, 8,
    }));
    resources.put("map/two", image_payload(1, 1, {9, 10, 11, 12}));

    std::size_t decode_calls = 0;
    dhsh::CachedImageProvider images(resources,
        [&](std::span<const std::byte> data) {
            ++decode_calls;
            return decode_test_image(data);
        });

    check(!images.cached("map/one"), "provider reported an unrequested path as cached");
    const dhsh::Image& first = images.get("map/one");
    const dhsh::Image& second = images.get("map/one");
    check(&first == &second, "repeated logical path did not return the cached image");
    check(resources.byte_calls == 1, "repeated logical path re-read the resource");
    check(decode_calls == 1, "repeated logical path re-decoded the image");
    check(images.cached("map/one"), "successfully decoded path was not cached");
    check(first.width == 2 && first.height == 1 && first.rgba.size() == 8,
        "cached image dimensions or packing changed");

    const dhsh::Image& other = images.get("map/two");
    check(other.width == 1 && other.height == 1
            && std::to_integer<unsigned>(other.rgba[3]) == 12,
        "second logical path decoded the wrong image");
    check(resources.byte_calls == 2 && decode_calls == 2,
        "distinct logical paths did not decode independently");
}

void test_cached_image_provider_does_not_cache_failures() {
    FakeResourceProvider resources;
    resources.put("map/bad", bytes({0xff, 0xff}));

    std::size_t decode_calls = 0;
    dhsh::CachedImageProvider images(resources,
        [&](std::span<const std::byte> data) {
            ++decode_calls;
            return decode_test_image(data);
        });

    expect_resource_error([&] { (void)images.get("map/missing"); },
        "missing resource did not throw", "map/missing");
    check(!images.cached("map/missing"), "missing path was cached");
    const std::size_t calls_after_missing = resources.byte_calls;
    expect_resource_error([&] { (void)images.get("map/missing"); },
        "missing resource retry did not throw", "map/missing");
    check(resources.byte_calls == calls_after_missing + 1,
        "missing resource was negatively cached");

    expect_resource_error([&] { (void)images.get("map/bad"); },
        "decode failure did not throw", "map/bad");
    check(!images.cached("map/bad"), "failed decode was cached");
    check(decode_calls == 1, "failed decode did not attempt the decoder exactly once");

    resources.put("map/bad", image_payload(1, 1, {1, 2, 3, 255}));
    const dhsh::Image& recovered = images.get("map/bad");
    check(recovered.width == 1 && recovered.height == 1
            && std::to_integer<unsigned>(recovered.rgba[0]) == 1,
        "failed decode was not retried after the resource was fixed");
    check(images.cached("map/bad"), "successful retry was not cached");
}

void test_map_scene_loads_required_assets_and_skips_missing_man_object() {
    FakeResourceProvider resources;
    add_valid_map(resources, background_payload());

    FakeManObjectProvider objects;
    dhsh::MapObjectFrame present;
    present.image = solid_image(2, 2, 200, 100, 50);
    present.offset_x = 1;
    present.offset_y = 1;
    objects.frames.emplace(std::string(present_object_path), std::move(present));

    dhsh::CachedImageProvider images(resources, decode_test_image);
    const dhsh::MapScene scene = dhsh::MapScene::load(
        resources, images, "YingTianFu", {&objects});

    check(scene.name() == "YingTianFu", "scene map name changed");
    check(scene.width() == 64 && scene.height() == 48,
        "scene logical dimensions did not come from the MAP INI");
    check(scene.background().width == 4 && scene.background().height == 3,
        "scene background was not decoded through CachedImageProvider");
    check(scene.man().entries.size() == 2, "scene MAN table was not loaded");
    check(scene.cel().width == 4 && scene.cel().height == 3,
        "scene ground CEL was not loaded or fitted to the map");
    check(scene.core().has_value() && scene.core()->placements.size() == 1,
        "scene core mask table was not loaded");
    check(scene.objects().size() == 1, "missing MAN object was not skipped");
    check(std::string_view(scene.objects()[0].entry.path) == present_object_path,
        "scene kept the wrong MAN object");
    check(scene.objects()[0].x == 2 && scene.objects()[0].y == 2,
        "scene object placement dropped the MAN or frame offset");
    check(scene.skipped_objects().size() == 1,
        "scene did not record exactly one skipped MAN object");
    check(std::string_view(scene.skipped_objects()[0].path) == missing_object_path,
        "scene recorded the wrong skipped MAN path");
    check(!scene.skipped_objects()[0].reason.empty(),
        "skipped MAN object has no diagnostic reason");
    check(objects.calls == 2, "scene did not attempt every MAN object exactly once");
}

void test_map_scene_rejects_missing_required_assets() {
    {
        FakeResourceProvider resources;
        add_valid_map(resources, background_payload());
        resources.erase("map/YingTianFu.map");
        dhsh::CachedImageProvider images(resources, decode_test_image);
        expect_resource_error([&] {
            (void)dhsh::MapScene::load(resources, images, "YingTianFu");
        }, "missing MAP was not fatal", "YingTianFu.map");
    }
    {
        FakeResourceProvider resources;
        add_valid_map(resources, background_payload());
        erase_asset(resources, background_asset);
        dhsh::CachedImageProvider images(resources, decode_test_image);
        expect_resource_error([&] {
            (void)dhsh::MapScene::load(resources, images, "YingTianFu");
        }, "missing background was not fatal", "YingTianFu.jpg");
    }
    {
        FakeResourceProvider resources;
        add_valid_map(resources, background_payload());
        erase_asset(resources, cel_asset);
        dhsh::CachedImageProvider images(resources, decode_test_image);
        expect_resource_error([&] {
            (void)dhsh::MapScene::load(resources, images, "YingTianFu");
        }, "missing ground CEL was not fatal", "YingTianFu.CEL");
    }
}

void test_map_scene_renders_background_and_man_frames_through_the_camera() {
    FakeResourceProvider resources;
    add_valid_map(resources, background_payload());

    FakeManObjectProvider objects;
    dhsh::MapObjectFrame present;
    present.image = solid_image(2, 2, 200, 100, 50);
    present.offset_x = 1;
    present.offset_y = 1;
    objects.frames.emplace(std::string(present_object_path), std::move(present));

    dhsh::CachedImageProvider images(resources, decode_test_image);
    const dhsh::MapScene scene = dhsh::MapScene::load(
        resources, images, "YingTianFu", {&objects});

    dhsh::SoftwareRenderer origin(4, 3);
    fill_black(origin);
    scene.render(origin, 0, 0);
    check_pixel(origin.target(), 0, 0, {10, 0, 0, 255},
        "scene background origin changed");
    check_pixel(origin.target(), 3, 1, {80, 0, 0, 255},
        "scene background row stride changed");
    check_pixel(origin.target(), 2, 2, {200, 100, 50, 255},
        "scene MAN frame was not drawn at its MAN plus frame offset");
    check_pixel(origin.target(), 3, 2, {200, 100, 50, 255},
        "scene MAN frame clipping changed");

    dhsh::SoftwareRenderer shifted(4, 3);
    fill_black(shifted);
    scene.render(shifted, 1, 1);
    check_pixel(shifted.target(), 0, 0, {60, 0, 0, 255},
        "camera offset did not move the background");
    check_pixel(shifted.target(), 0, 1, {100, 0, 0, 255},
        "camera offset did not move the background row");
    check_pixel(shifted.target(), 1, 1, {200, 100, 50, 255},
        "camera offset did not move the MAN frame");
    check_pixel(shifted.target(), 2, 1, {200, 100, 50, 255},
        "camera offset clipped the MAN frame incorrectly");
    check_pixel(shifted.target(), 3, 2, {0, 0, 0, 255},
        "camera viewport leaked outside the background");

    dhsh::SoftwareRenderer negative(4, 3);
    fill_black(negative);
    scene.render(negative, -1, -1);
    check_pixel(negative.target(), 0, 0, {0, 0, 0, 255},
        "negative camera painted outside the background");
    check_pixel(negative.target(), 1, 1, {10, 0, 0, 255},
        "negative camera did not place the background origin");
}

void test_renderer_unchanged_opaque_draw_returns_false() {
    dhsh::SoftwareRenderer renderer(2, 1);
    const dhsh::Image source = make_test_image(2, 1, {
        10, 20, 30, 255, 40, 50, 60, 255,
    });
    check(renderer.draw(source, 0, 0), "initial opaque draw did not report a change");
    const dhsh::Image before = renderer.target();
    check(!renderer.draw(source, 0, 0),
        "identical opaque row copy reported a change");
    check(renderer.target().rgba == before.rgba,
        "identical opaque row copy changed the framebuffer");
}

void test_renderer_verified_opaque_draw_matches_regular_draw() {
    const dhsh::Image source = make_test_image(2, 2, {
        10, 20, 30, 255, 40, 50, 60, 255,
        70, 80, 90, 255, 100, 110, 120, 255,
    });
    dhsh::SoftwareRenderer regular(3, 2);
    dhsh::SoftwareRenderer verified(3, 2);
    check(regular.draw(source, 1, 0), "regular clipped opaque draw failed");
    check(verified.draw_opaque(source, 1, 0),
        "verified clipped opaque draw failed");
    check(verified.target().rgba == regular.target().rgba,
        "verified opaque fast path changed clipped pixels");
    const dhsh::Image before = verified.target();
    check(!verified.draw_opaque(source, 1, 0),
        "unchanged verified opaque draw reported a change");
    check(verified.target().rgba == before.rgba,
        "unchanged verified opaque draw changed the framebuffer");
}

void test_renderer_mixed_alpha_row_and_unchanged_blend() {
    dhsh::SoftwareRenderer renderer(3, 1);
    fill_black(renderer);
    const dhsh::Image source = make_test_image(3, 1, {
        200, 100, 50, 255, 100, 100, 100, 128, 9, 9, 9, 0,
    });
    check(renderer.draw(source, 0, 0), "mixed-alpha row was not drawn");
    check_pixel(renderer.target(), 0, 0, {200, 100, 50, 255},
        "opaque pixel in a mixed-alpha row was blended instead of copied");
    check_pixel(renderer.target(), 1, 0, {50, 50, 50, 255},
        "semi-transparent pixel in a mixed-alpha row changed blend math");
    check_pixel(renderer.target(), 2, 0, {0, 0, 0, 255},
        "transparent pixel in a mixed-alpha row changed the destination");
    check(renderer.draw(source, 0, 0),
        "repeated semi-transparent blending did not advance toward the source RGB");

    dhsh::SoftwareRenderer unchanged(1, 1);
    check(unchanged.draw(solid_image(1, 1, 50, 50, 50), 0, 0),
        "unchanged-blend fixture setup failed");
    check(!unchanged.draw(make_test_image(1, 1, {50, 50, 50, 128}), 0, 0),
        "semi-transparent source equal to the destination reported a change");
}

void test_map_scene_caches_background_opacity() {
    FakeResourceProvider opaque_resources;
    add_valid_map(opaque_resources, background_payload());
    dhsh::CachedImageProvider opaque_images(opaque_resources, decode_test_image);
    const dhsh::MapScene opaque_scene = dhsh::MapScene::load(
        opaque_resources, opaque_images, "YingTianFu");
    check(opaque_scene.background_opaque(),
        "fully opaque map background was not classified as opaque");

    Bytes translucent = background_payload();
    translucent.back() = std::byte{128};
    FakeResourceProvider translucent_resources;
    add_valid_map(translucent_resources, translucent);
    dhsh::CachedImageProvider translucent_images(
        translucent_resources, decode_test_image);
    const dhsh::MapScene translucent_scene = dhsh::MapScene::load(
        translucent_resources, translucent_images, "YingTianFu");
    check(!translucent_scene.background_opaque(),
        "translucent map background was incorrectly classified as opaque");
}

void test_map_scene_entity_masks_match_undiagnostic_rendering() {
    FakeResourceProvider resources;
    add_valid_map(resources, image_payload(64, 48,
        solid_pixels(64, 48, 0, 0, 0)));
    FakeManObjectProvider objects;
    dhsh::MapObjectFrame present;
    present.image = solid_image(2, 2, 200, 100, 50);
    present.offset_x = 1;
    present.offset_y = 1;
    objects.frames.emplace(std::string(present_object_path), std::move(present));
    dhsh::CachedImageProvider images(resources, decode_test_image);
    dhsh::MapScene scene = dhsh::MapScene::load(
        resources, images, "YingTianFu", {&objects});

    dhsh::Character player({10, 20}, dhsh::Direction::down);
    player.set_animation(dhsh::EntityState::idle, dhsh::Direction::down,
        dhsh::AnimationClip({dhsh::AnimationFrame{
            solid_image(3, 2, 80, 80, 80), 0, 0, 80}}));
    scene.set_player(std::move(player));
    dhsh::Entity npc({12, 28}, dhsh::Direction::down);
    npc.set_animation(dhsh::EntityState::idle, dhsh::Direction::down,
        dhsh::AnimationClip({dhsh::AnimationFrame{
            solid_image(1, 1, 9, 9, 9), 0, 0, 80}}));
    scene.add_npc(std::move(npc));
    dhsh::Entity non_intersecting({11, 20}, dhsh::Direction::down);
    non_intersecting.set_animation(dhsh::EntityState::idle, dhsh::Direction::down,
        dhsh::AnimationClip({dhsh::AnimationFrame{
            solid_image(1, 1, 120, 120, 120), 30, 10, 80}}));
    scene.add_npc(std::move(non_intersecting));

    dhsh::SoftwareRenderer plain(64, 48);
    dhsh::SoftwareRenderer measured(64, 48);
    dhsh::SceneRenderDiagnostics diagnostics;
    scene.render(measured, 0, 0, &diagnostics);
    scene.render(plain, 0, 0);
    check(measured.target().rgba == plain.target().rgba,
        "render diagnostics changed scene pixels");
    check(diagnostics.cover_tests == 3 && diagnostics.covering_masks == 2
            && diagnostics.sprite_copies == 1,
        "covered sprite with a disjoint mask rectangle triggered a copy: tests="
            + std::to_string(diagnostics.cover_tests) + " covering="
            + std::to_string(diagnostics.covering_masks) + " copies="
            + std::to_string(diagnostics.sprite_copies));
    dhsh::SceneRenderDiagnostics cached;
    scene.render(measured, 0, 0, &cached);
    check(cached.cover_tests == 0 && cached.covering_masks == 0
            && cached.sprite_copies == 0,
        "unchanged entities repeated depth tests or sprite copies: tests="
            + std::to_string(cached.cover_tests) + " covering="
            + std::to_string(cached.covering_masks) + " copies="
            + std::to_string(cached.sprite_copies));
    check(measured.target().rgba == plain.target().rgba,
        "cached entity occlusion changed scene pixels");
    check_pixel(measured.target(), 10, 20, {80, 80, 80, 255},
        "uncovered player pixel was darkened");
    check_pixel(measured.target(), 11, 20, {10, 10, 10, 255},
        "first covered player pixel was not darkened through MapScene");
    check_pixel(measured.target(), 12, 20, {10, 10, 10, 255},
        "second covered player pixel was not darkened through MapScene");
    check_pixel(measured.target(), 10, 21, {10, 10, 10, 255},
        "covered pixel on the second sprite row was not darkened");
    check_pixel(measured.target(), 11, 21, {80, 80, 80, 255},
        "uncovered second-row player pixel was darkened");
    check_pixel(measured.target(), 12, 28, {9, 9, 9, 255},
        "NPC behind the strict core boundary was darkened");
    check_pixel(measured.target(), 41, 30, {120, 120, 120, 255},
        "disjoint cover mask changed the non-intersecting NPC pixel");
    dhsh::Image empty;
    check(!scene.cover_masks().front().apply(empty, 11, 20, 0, 0),
        "empty sprite changed pixels or reached invalid mask arithmetic");
}

void test_entity_occlusion_cache_tracks_frames_and_position() {
    FakeResourceProvider resources;
    add_valid_map(resources, image_payload(64, 48,
        solid_pixels(64, 48, 0, 0, 0)));
    dhsh::CachedImageProvider images(resources, decode_test_image);
    dhsh::MapScene scene = dhsh::MapScene::load(resources, images, "YingTianFu");
    dhsh::Character player({10, 20}, dhsh::Direction::down);
    player.set_animation(dhsh::EntityState::idle, dhsh::Direction::down,
        dhsh::AnimationClip({
            dhsh::AnimationFrame{solid_image(3, 2, 80, 80, 80), 0, 0, 80},
            dhsh::AnimationFrame{solid_image(3, 2, 96, 96, 96), 0, 0, 80},
        }));
    scene.set_player(std::move(player));
    dhsh::SoftwareRenderer renderer(64, 48);

    dhsh::SceneRenderDiagnostics first;
    scene.render(renderer, 0, 0, &first);
    check(first.cover_tests == 1 && first.sprite_copies == 1,
        "initial entity occlusion was not computed exactly once");
    dhsh::SceneRenderDiagnostics repeated;
    scene.render(renderer, 0, 0, &repeated);
    check(repeated.cover_tests == 0 && repeated.sprite_copies == 0,
        "an unchanged entity frame repeated occlusion work");

    check(scene.advance(80), "two-frame entity did not advance");
    dhsh::SceneRenderDiagnostics next_frame;
    scene.render(renderer, 0, 0, &next_frame);
    check(next_frame.cover_tests == 0 && next_frame.sprite_copies == 1,
        "a new frame recomputed depth candidates or was not copied exactly once");
    check_pixel(renderer.target(), 11, 20, {12, 12, 12, 255},
        "the new frame did not receive the cached candidate mask");

    check(scene.advance(80), "two-frame entity did not wrap");
    dhsh::SceneRenderDiagnostics wrapped;
    scene.render(renderer, 0, 0, &wrapped);
    check(wrapped.cover_tests == 0 && wrapped.sprite_copies == 0,
        "a previously masked animation frame was copied again");

    scene.player()->set_position({10, 21});
    dhsh::SceneRenderDiagnostics moved;
    scene.render(renderer, 0, 0, &moved);
    check(moved.cover_tests == 1 && moved.sprite_copies == 1,
        "moving an entity did not invalidate its occlusion cache");
}

void test_entity_occlusion_cache_tracks_group_selection() {
    FakeResourceProvider resources;
    add_valid_map(resources, image_payload(64, 48,
        solid_pixels(64, 48, 0, 0, 0)));
    Bytes group_core = make_core();
    set_u32(group_core, 80, 0);
    resources.put("map/YingTianFu/YingTianFu.core", nz_wrapped(group_core));
    dhsh::CachedImageProvider images(resources, decode_test_image);
    dhsh::MapScene scene = dhsh::MapScene::load(resources, images, "YingTianFu");
    dhsh::Character player({10, 20}, dhsh::Direction::down);
    player.set_animation(dhsh::EntityState::idle, dhsh::Direction::down,
        dhsh::AnimationClip({dhsh::AnimationFrame{
            solid_image(3, 2, 80, 80, 80), 0, 0, 80}}));
    scene.set_player(std::move(player));
    dhsh::SoftwareRenderer renderer(64, 48);

    dhsh::SceneRenderDiagnostics inactive;
    scene.render(renderer, 0, 0, &inactive);
    check(inactive.cover_tests == 1 && inactive.covering_masks == 0
            && inactive.sprite_copies == 0,
        "inactive group mask setup was not retained");
    scene.cover_masks().front().set_selected(true);
    dhsh::SceneRenderDiagnostics selected;
    scene.render(renderer, 0, 0, &selected);
    check(selected.cover_tests == 1 && selected.covering_masks == 1
            && selected.sprite_copies == 1,
        "group activation did not invalidate cached mask candidates");
    check_pixel(renderer.target(), 11, 20, {10, 10, 10, 255},
        "group-activated mask did not darken the entity");
    scene.cover_masks().front().set_selected(false);
    dhsh::SceneRenderDiagnostics deselected;
    scene.render(renderer, 0, 0, &deselected);
    check(deselected.cover_tests == 1 && deselected.covering_masks == 0
            && deselected.sprite_copies == 0,
        "group deactivation did not invalidate cached mask candidates");
    check_pixel(renderer.target(), 11, 20, {80, 80, 80, 255},
        "group-deactivated mask remained applied to the entity");
}

Bytes synthetic_a16() {
    Bytes result = bytes({'A', 'P'});
    append_u16(result, 24);
    append_u16(result, 0);
    append_u16(result, 0);
    append_u16(result, 1);
    append_u16(result, 1);
    append_u16(result, 0);
    result.push_back(std::byte{0});
    result.push_back(std::byte{0});
    append_u32(result, 2);
    append_u32(result, 0);
    append_u16(result, 0xf800);
    return result;
}

Bytes synthetic_pif(const std::vector<std::uint16_t>& delays) {
    Bytes result = bytes({'P', 'I', 'F', 0});
    append_u32(result, 3);
    append_u16(result, 0);
    append_u16(result, 28);
    append_u16(result, static_cast<std::uint16_t>(delays.size()));
    append_u16(result, 2);
    append_u32(result, 0);
    for (const std::uint16_t delay : delays) {
        for (std::size_t field = 0; field < 14; ++field) {
            append_u16(result, field == 5 ? delay : 0);
        }
    }
    return result;
}

void test_man_pif_bounds_frame_probes_and_preserves_fallback() {
    FakeResourceProvider resources;
    resources.put("map/animate/test/perinfo.dat", nz_wrapped(synthetic_pif({80, 120})));
    resources.put("map/animate/test/0.a16", nz_wrapped(synthetic_a16()));
    resources.put("map/animate/test/00001.a16", nz_wrapped(synthetic_a16()));
    dhsh::PakManObjectProvider objects(resources);
    dhsh::ManEntry entry;
    entry.path = "map/animate/test";
    const auto animation = objects.load_animation(entry);
    check(animation != nullptr && animation->size() == 2
            && animation->at(0).duration_ms == 80
            && animation->at(1).duration_ms == 120,
        "PIF-guided MAN loading changed frames or delays");
    check(resources.exists_calls <= 7,
        "PIF frame count did not bound MAN resource probes");

    resources.erase("map/animate/test/perinfo.dat");
    resources.exists_calls = 0;
    dhsh::PakManObjectProvider fallback_objects(resources);
    const auto fallback = fallback_objects.load_animation(entry);
    check(fallback != nullptr && fallback->size() == 2,
        "MAN loading without PIF no longer used the numeric fallback");
    check(resources.exists_calls <= 5002,
        "numeric fallback exceeded its 1000-frame probe bound");

    resources.put("map/animate/test/perinfo.dat", nz_wrapped(bytes({1, 2})));
    dhsh::PakManObjectProvider corrupt_objects(resources);
    const auto corrupt = corrupt_objects.load_animation(entry);
    check(corrupt != nullptr && corrupt->size() == 2
            && corrupt->at(0).duration_ms == 80
            && corrupt->at(1).duration_ms == 80,
        "corrupt PIF metadata did not fall back to default frame delays");

    resources.put("map/animate/test/perinfo.dat",
        nz_wrapped(synthetic_pif({10, 20, 30})));
    dhsh::PakManObjectProvider mismatch_objects(resources);
    const auto mismatch = mismatch_objects.load_animation(entry);
    check(mismatch != nullptr && mismatch->size() == 2
            && mismatch->at(0).duration_ms == 80
            && mismatch->at(1).duration_ms == 80,
        "PIF/frame-count mismatch incorrectly applied partial delays");

    resources.put("map/animate/test/perinfo.dat",
        nz_wrapped(synthetic_pif(std::vector<std::uint16_t>(1001, 80))));
    resources.exists_calls = 0;
    dhsh::PakManObjectProvider oversized_objects(resources);
    const auto oversized = oversized_objects.load_animation(entry);
    check(oversized != nullptr && oversized->size() == 2
            && resources.exists_calls <= 5001,
        "oversized PIF metadata exceeded the 1000-frame safety cap");
}

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open file: " + path.string());
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("cannot determine file size: " + path.string());
    }
    Bytes data(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!data.empty()) {
        input.read(reinterpret_cast<char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    }
    if (!input) {
        throw std::runtime_error("cannot read file: " + path.string());
    }
    return data;
}

// Test-side cache provider for the real YingTianFu entry. The Python cache
// tool exports the MAP BACKGROUND under its logical PAK path with a .png
// suffix, e.g. <cache>/map/YingTianFu/YingTianFu_10Year.png. No version or
// SHA is appended.
class CacheResourceProvider final : public dhsh::ResourceProvider {
public:
    explicit CacheResourceProvider(std::filesystem::path root)
        : root_(std::move(root)) {
    }

    std::vector<std::byte> bytes(std::string_view logical_path) override {
        const auto file = resolve(logical_path);
        if (!file.has_value()) {
            throw dhsh::ResourceError(
                "cached image not found: " + std::string(logical_path));
        }
        return read_file(*file);
    }

    bool exists(std::string_view logical_path) const override {
        return resolve(logical_path).has_value();
    }

private:
    std::optional<std::filesystem::path> resolve(std::string_view logical_path) const {
        const std::string relative = normalize_logical_path(logical_path);
        if (relative.empty() || relative == ".." || relative.rfind("../", 0) == 0
            || relative.find(':') != std::string::npos) {
            return std::nullopt;
        }
        std::error_code error;
        std::filesystem::path candidate = root_ / std::filesystem::path(relative);
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
        candidate.replace_extension(".png");
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
        return std::nullopt;
    }

    std::filesystem::path root_;
};

void write_ppm(const std::filesystem::path& path, const dhsh::Image& image) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create viewport image: " + path.string());
    }
    output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (std::size_t offset = 0; offset + 4 <= image.rgba.size(); offset += 4) {
        output.put(static_cast<char>(std::to_integer<unsigned>(image.rgba[offset])));
        output.put(static_cast<char>(std::to_integer<unsigned>(image.rgba[offset + 1])));
        output.put(static_cast<char>(std::to_integer<unsigned>(image.rgba[offset + 2])));
    }
    if (!output) {
        throw std::runtime_error("cannot write viewport image: " + path.string());
    }
}

std::int32_t parse_int32(std::string_view text, const char* name) {
    std::int32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error(std::string("invalid ") + name + ": " + std::string(text));
    }
    return value;
}

void render_ying_tian_fu(
    const std::filesystem::path& shdata,
    const std::filesystem::path& cache,
    const std::filesystem::path& output,
    std::int32_t camera_x,
    std::int32_t camera_y) {
    dhsh::PakResourceProvider resources(shdata, {"config.pak", "map.pak"});
    CacheResourceProvider cache_resources(cache);
    dhsh::CachedImageProvider images(cache_resources);
    dhsh::PakManObjectProvider objects(resources);
    const dhsh::MapScene scene = dhsh::MapScene::load(
        resources, images, "YingTianFu", {&objects});
    std::cout << "YingTianFu MAN objects=" << scene.objects().size()
              << " skipped=" << scene.skipped_objects().size() << '\n';
    if (scene.objects().size() != 103 || !scene.skipped_objects().empty()) {
        for (const auto& skipped : scene.skipped_objects()) {
            std::cout << "SKIP " << skipped.path << ": " << skipped.reason << '\n';
        }
    }
    check(scene.objects().size() == 103,
        "every real YingTianFu MAN entry has a decodable A16 first frame in map.pak");
    check(scene.skipped_objects().empty(),
        "no real YingTianFu MAN entry should be skipped when reading the complete PAK");

    dhsh::SoftwareRenderer renderer(800, 600);
    scene.render(renderer, camera_x, camera_y);
    write_ppm(output, renderer.target());

    std::cout << "YingTianFu static viewport " << renderer.width() << 'x'
              << renderer.height() << " camera=(" << camera_x << ',' << camera_y
              << ") map=" << scene.width() << 'x' << scene.height()
              << " background=" << scene.background().width << 'x'
              << scene.background().height
              << " man=" << scene.man().entries.size()
              << " objects=" << scene.objects().size()
              << " skipped=" << scene.skipped_objects().size()
              << " output=" << output.string() << '\n';
}

void write_bytes(const std::filesystem::path& path, const Bytes& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create animation verification output");
    }
    if (!data.empty()) {
        output.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    }
    if (!output) {
        throw std::runtime_error("cannot write animation verification output");
    }
}

void verify_ying_tian_fu_animation(
    const std::filesystem::path& shdata,
    const std::filesystem::path& cache,
    const std::filesystem::path& prefix) {
    dhsh::PakResourceProvider resources(shdata, {"config.pak", "map.pak"});
    CacheResourceProvider cache_resources(cache);
    dhsh::CachedImageProvider images(cache_resources);
    dhsh::PakManObjectProvider objects(resources);
    dhsh::MapScene scene = dhsh::MapScene::load(
        resources, images, "YingTianFu", {&objects});

    const dhsh::SceneObject* lantern = nullptr;
    for (const dhsh::SceneObject& object : scene.objects()) {
        if (contains_ci(object.entry.path, "denglong1")) {
            lantern = &object;
            break;
        }
    }
    check(lantern != nullptr, "YingTianFu lantern MAN object was not loaded");
    const auto animation = objects.load_animation(lantern->entry);
    const auto repeated = objects.load_animation(lantern->entry);
    check(animation != nullptr && animation == repeated,
        "repeated MAN paths did not share the cached immutable animation");
    check(lantern->animation() == animation,
        "SceneObject copied the provider animation instead of sharing it");
    check(animation->size() > 1, "lantern fixture is not multi-frame");
    check(lantern->current_frame_index() == 0
            && lantern->frame.image.rgba == animation->at(0).image.rgba,
        "lantern did not start on its first production frame");
    check(!scene.advance(0), "zero-duration scene advance changed an animation frame");

    write_bytes(prefix.string() + ".before.rgba", lantern->frame.image.rgba);
    const std::uint32_t first_duration = animation->at(0).duration_ms;
    check(scene.advance(first_duration), "scene did not advance the lantern animation");
    check(lantern->current_frame_index() == 1
            && lantern->frame.image.rgba == animation->at(1).image.rgba,
        "lantern did not advance to its second production frame");
    write_bytes(prefix.string() + ".after.rgba", lantern->frame.image.rgba);

    std::ofstream metadata(prefix.string() + ".meta", std::ios::trunc);
    metadata << "frames " << animation->size() << '\n';
    for (std::size_t index = 0; index < animation->size(); ++index) {
        const dhsh::MapObjectFrame& frame = animation->at(index);
        metadata << "frame " << index << ' ' << frame.offset_x << ' '
                 << frame.offset_y << ' ' << frame.duration_ms << ' '
                 << frame.image.width << ' ' << frame.image.height << '\n';
    }
    metadata << "before 0\nafter 1\nshared 1\n";
    if (!metadata) {
        throw std::runtime_error("cannot write animation verification metadata");
    }
    std::cout << "YingTianFu lantern frames=" << animation->size()
              << " first_duration=" << first_duration
              << " shared_cache=1 output=" << prefix.string() << '\n';
}

int animation_main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: dhsh_test_scene --verify-ying-tian-fu-animation"
                     " <shdata> <cache> <output-prefix>\n";
        return 2;
    }
    try {
        verify_ying_tian_fu_animation(argv[2], argv[3], argv[4]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: YingTianFu animation: " << error.what() << '\n';
        return 1;
    }
}

int real_main(int argc, char** argv) {
    if (argc != 5 && argc != 7) {
        std::cerr
            << "usage: dhsh_test_scene --render-ying-tian-fu <shdata> <cache> <output.ppm>"
               " [camera-x camera-y]\n"
            << "  <cache> must contain the background PNG exported by"
               " tools/client_map_cache.py for the MAP BACKGROUND path,"
               " for example:\n"
            << "  <cache>/map/YingTianFu/YingTianFu_10Year.png\n";
        return 2;
    }
    try {
        std::int32_t camera_x = 0;
        std::int32_t camera_y = 0;
        if (argc == 7) {
            camera_x = parse_int32(argv[5], "camera-x");
            camera_y = parse_int32(argv[6], "camera-y");
        }
        render_ying_tian_fu(argv[2], argv[3], argv[4], camera_x, camera_y);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: YingTianFu static viewport: " << error.what() << '\n';
        return 1;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "--render-ying-tian-fu") {
        return real_main(argc, argv);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "--verify-ying-tian-fu-animation") {
        return animation_main(argc, argv);
    }
    if (argc != 1) {
        return 2;
    }

    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"renderer viewport clipping", test_renderer_clips_background_and_sprites_to_viewport},
        {"renderer out-of-viewport safety", test_renderer_is_safe_outside_the_viewport},
        {"renderer source alpha blending", test_renderer_blends_source_alpha_and_preserves_transparent_pixels},
        {"renderer unchanged opaque draw", test_renderer_unchanged_opaque_draw_returns_false},
        {"renderer verified opaque draw", test_renderer_verified_opaque_draw_matches_regular_draw},
        {"renderer mixed alpha row", test_renderer_mixed_alpha_row_and_unchanged_blend},
        {"cached image provider logical path cache", test_cached_image_provider_reuses_decoded_images_by_logical_path},
        {"cached image provider failure behavior", test_cached_image_provider_does_not_cache_failures},
        {"map scene required assets and MAN skips", test_map_scene_loads_required_assets_and_skips_missing_man_object},
        {"map scene missing required assets", test_map_scene_rejects_missing_required_assets},
        {"map scene camera rendering", test_map_scene_renders_background_and_man_frames_through_the_camera},
        {"map scene cached background opacity", test_map_scene_caches_background_opacity},
        {"map scene entity mask diagnostics", test_map_scene_entity_masks_match_undiagnostic_rendering},
        {"entity occlusion frame and position cache", test_entity_occlusion_cache_tracks_frames_and_position},
        {"entity occlusion group selection cache", test_entity_occlusion_cache_tracks_group_selection},
        {"PIF-bounded MAN probes and fallback", test_man_pif_bounds_frame_probes_and_preserves_fallback},
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
