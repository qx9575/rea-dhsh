#include "avatar/Avatar.h"
#include "avatar/AvatarCatalog.h"
#include "formats/Pif.h"
#include "render/Image.h"
#include "render/SoftwareRenderer.h"
#include "resource/CachedImageProvider.h"
#include "resource/NzDecoder.h"
#include "resource/PakResourceProvider.h"
#include "resource/ResourceProvider.h"
#include "scene/Character.h"
#include "scene/Entity.h"
#include "scene/MapScene.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <optional>
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

dhsh::Image pixel(unsigned red, unsigned green, unsigned blue) {
    dhsh::Image result;
    result.width = 1;
    result.height = 1;
    result.rgba = {
        static_cast<std::byte>(red),
        static_cast<std::byte>(green),
        static_cast<std::byte>(blue),
        std::byte{0xff},
    };
    return result;
}

dhsh::AnimationClip clip(
    std::int16_t anchor_x,
    std::int16_t anchor_y,
    const std::vector<dhsh::Image>& images) {
    std::vector<dhsh::AnimationFrame> frames;
    for (const dhsh::Image& image : images) {
        frames.push_back(dhsh::AnimationFrame{image, anchor_x, anchor_y, 80});
    }
    return dhsh::AnimationClip(std::move(frames));
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

void append_text(Bytes& output, std::string_view text) {
    for (const char value : text) {
        output.push_back(static_cast<std::byte>(value));
    }
}

void append_fixed(Bytes& output, std::string_view text, std::size_t size) {
    append_text(output, text);
    output.resize(output.size() + size - text.size(), std::byte{0});
}

Bytes nz(const Bytes& payload) {
    Bytes result;
    append_text(result, "NZ");
    result.push_back(std::byte{0});
    result.push_back(std::byte{0});
    append_u32(result, static_cast<std::uint32_t>(payload.size()));
    append_u32(result, static_cast<std::uint32_t>(payload.size()));
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

class MemoryResources final : public dhsh::ResourceProvider {
public:
    void add(std::string path, Bytes data) {
        files.emplace(std::move(path), std::move(data));
    }

    Bytes bytes(std::string_view path) override {
        return files.at(std::string(path));
    }

    bool exists(std::string_view path) const override {
        return files.find(std::string(path)) != files.end();
    }

private:
    std::unordered_map<std::string, Bytes> files;
};

class GreenManProvider final : public dhsh::ManObjectProvider {
public:
    std::optional<dhsh::MapObjectFrame> load_first_frame(
        const dhsh::ManEntry&) override {
        return dhsh::MapObjectFrame{pixel(0, 200, 0), 0, 0, 80};
    }
};

Bytes make_cel() {
    Bytes result;
    append_text(result, "CEL");
    result.push_back(std::byte{0});
    append_u32(result, 2);
    append_u32(result, 27);
    append_u32(result, 0);
    append_u32(result, 4);
    append_u32(result, 3);
    result.resize(27, std::byte{0});
    return result;
}

Bytes make_man() {
    Bytes result;
    append_text(result, "MAN");
    result.push_back(std::byte{0});
    append_u32(result, 4);
    append_u32(result, 0);
    append_u32(result, 0);
    append_u32(result, 28);
    append_u32(result, 100);
    append_u32(result, 2);
    for (const std::int32_t x : {1, 2}) {
        append_fixed(result, "green", 64);
        for (const std::int32_t value : {x, 20, 0, 0, 0, 0, 0, 0, 0}) {
            append_u32(result, static_cast<std::uint32_t>(value));
        }
    }
    set_u32(result, 8, static_cast<std::uint32_t>(result.size()));
    return result;
}

Bytes make_mlk() {
    Bytes result;
    append_text(result, "lnk");
    result.push_back(std::byte{0});
    append_u32(result, 3);
    append_u32(result, 128);
    append_u32(result, 1);
    append_u32(result, 1);
    append_u32(result, 17);
    append_fixed(result, "YTF_YaMen.map", 64);
    for (const std::int32_t value : {0, 0, 0, 0, 0, 6960, 2128, 1, 1, 0}) {
        append_u32(result, static_cast<std::uint32_t>(value));
    }
    return result;
}

void add_map(MemoryResources& resources) {
    Bytes ini;
    append_text(ini,
        "[BASE]\nWIDTH=64\nHEIGHT=48\nBACKGROUND=bg\nCELLFILE=cel\n"
        "[EXT]\nANIFILE=man\n");
    resources.add("map/test.map", nz(ini));
    resources.add("map/test.mlk", nz(make_mlk()));
    resources.add("bg", Bytes{std::byte{0}});
    resources.add("cel", nz(make_cel()));
    resources.add("man", nz(make_man()));
}

void check_color(const dhsh::Image& image, std::uint32_t x, std::uint32_t y,
    unsigned red, unsigned green, unsigned blue, const std::string& message) {
    const std::size_t offset = (static_cast<std::size_t>(y) * image.width + x) * 4;
    check(image.rgba[offset] == static_cast<std::byte>(red)
            && image.rgba[offset + 1] == static_cast<std::byte>(green)
            && image.rgba[offset + 2] == static_cast<std::byte>(blue),
        message);
}

void test_idle_walk_transitions_select_the_state_animation() {
    dhsh::Character character({40, 48});
    character.set_animation(dhsh::EntityState::idle, dhsh::Direction::down,
        clip(0, 0, {pixel(10, 0, 0)}));
    character.set_animation(dhsh::EntityState::idle, dhsh::Direction::right,
        clip(0, 0, {pixel(20, 0, 0)}));
    character.set_animation(dhsh::EntityState::walk, dhsh::Direction::right,
        clip(0, 0, {pixel(30, 0, 0)}));

    check(character.state() == dhsh::EntityState::idle
            && character.direction() == dhsh::Direction::down,
        "a character must start idle without inventing a movement direction");
    check(character.current_frame() != nullptr
            && character.current_frame()->image.rgba[0] == std::byte{10},
        "the idle animation was not selected initially");

    character.move_by(1, 0);
    check(character.position().x == 41 && character.position().y == 48
            && character.state() == dhsh::EntityState::walk
            && character.direction() == dhsh::Direction::right,
        "a real position delta must enter Walk and select its direction");
    check(character.current_frame() != nullptr
            && character.current_frame()->image.rgba[0] == std::byte{30},
        "Walk did not select the walking animation");

    character.stop();
    check(character.position().x == 41 && character.position().y == 48
            && character.state() == dhsh::EntityState::idle
            && character.direction() == dhsh::Direction::right,
        "stopping must preserve position and facing while returning to Idle");
    check(character.current_frame() != nullptr
            && character.current_frame()->image.rgba[0] == std::byte{20},
        "stopping did not restore the idle animation for the current direction");
}

void test_eight_direction_vectors_match_action_and_mlk_directions() {
    struct Case {
        std::int32_t x;
        std::int32_t y;
        dhsh::Direction direction;
    };
    constexpr std::array cases = {
        Case{-9, 0, dhsh::Direction::left},
        Case{-9, 4, dhsh::Direction::left_down},
        Case{0, 4, dhsh::Direction::down},
        Case{9, 4, dhsh::Direction::right_down},
        Case{9, 0, dhsh::Direction::right},
        Case{9, -4, dhsh::Direction::right_up},
        Case{0, -4, dhsh::Direction::up},
        Case{-9, -4, dhsh::Direction::left_up},
    };
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto& expected = cases[index];
        check(dhsh::direction_from_vector(expected.x, expected.y) == expected.direction,
            "an eight-way movement vector selected the wrong action direction");
        check(dhsh::direction_from_mlk(static_cast<std::int32_t>(index))
                == expected.direction,
            "MLK extra0 does not match the confirmed action direction encoding");
    }
    check(!dhsh::direction_from_vector(0, 0).has_value(),
        "a zero vector must not invent a direction");
    check(!dhsh::direction_from_mlk(-1).has_value()
            && !dhsh::direction_from_mlk(8).has_value(),
        "invalid MLK facing values must be rejected");
}

void test_zero_movement_does_not_enter_walk() {
    dhsh::Character character({120, 160}, dhsh::Direction::left_up);
    character.move_by(0, 0);
    check(character.position().x == 120 && character.position().y == 160
            && character.direction() == dhsh::Direction::left_up
            && character.state() == dhsh::EntityState::idle,
        "a zero delta incorrectly switched an idle character to Walk");
    character.set_position({120, 160});
    check(character.state() == dhsh::EntityState::idle
            && character.direction() == dhsh::Direction::left_up,
        "setting an unchanged position altered state or facing");
}

void test_npc_only_advances_local_animation() {
    dhsh::Entity npc({300, 320}, dhsh::Direction::right_down);
    npc.set_animation(dhsh::EntityState::idle, dhsh::Direction::right_down,
        clip(0, 0, {pixel(1, 2, 3), pixel(4, 5, 6)}));

    check(!npc.advance(79) && npc.current_frame_index() == 0,
        "NPC local animation advanced before its frame delay");
    check(npc.advance(1) && npc.current_frame_index() == 1,
        "NPC local animation did not advance to its second frame");
    check(npc.position().x == 300 && npc.position().y == 320
            && npc.direction() == dhsh::Direction::right_down
            && npc.state() == dhsh::EntityState::idle,
        "local NPC animation changed position, facing, or movement state");
    check(npc.current_frame() != nullptr
            && npc.current_frame()->image.rgba[0] == std::byte{4},
        "NPC animation did not expose its current local frame");
}

void test_mlk_spawn_is_cell_center_with_confirmed_facing() {
    dhsh::MlkLink link;
    link.dest_x = 6960;
    link.dest_y = 2128;
    link.extra0 = 1;
    const dhsh::EntityPosition spawn = dhsh::spawn_from_mlk(link);
    check(spawn.x == 6968 && spawn.y == 2136
            && spawn.x / 16 == 435 && spawn.y / 16 == 133,
        "MLK spawn must be the destination cell center, not its top-left");
    check(dhsh::direction_from_mlk(link.extra0) == dhsh::Direction::left_down,
        "the confirmed YingTianFu return facing was not preserved");
}

void test_frame_origin_combines_logical_position_and_current_anchor() {
    dhsh::Character character({6968, 2136}, dhsh::Direction::left_down);
    character.set_animation(dhsh::EntityState::idle, dhsh::Direction::left_down,
        clip(-320, -240, {pixel(1, 1, 1)}));
    check(character.frame_origin().x == 6648 && character.frame_origin().y == 1896,
        "avatar canvas origin must use its centered anchor and logical feet position");

    dhsh::Entity npc({300, 320}, dhsh::Direction::down);
    npc.set_animation(dhsh::EntityState::idle, dhsh::Direction::down,
        clip(-3, 7, {pixel(1, 1, 1), pixel(1, 1, 1)}));
    npc.advance(80);
    check(npc.frame_origin().x == 297 && npc.frame_origin().y == 327,
        "NPC sprite origin must use the current APG anchor");
}

void test_map_scene_mixes_man_and_entities_by_depth_and_anchor() {
    MemoryResources resources;
    add_map(resources);
    dhsh::CachedImageProvider images(resources, [](std::span<const std::byte>) {
        dhsh::Image background;
        background.width = 64;
        background.height = 48;
        background.rgba.assign(64U * 48U * 4U, std::byte{0});
        for (std::size_t alpha = 3; alpha < background.rgba.size(); alpha += 4) {
            background.rgba[alpha] = std::byte{0xff};
        }
        return background;
    });
    GreenManProvider provider;
    dhsh::MapScene scene = dhsh::MapScene::load(resources, images, "test", {&provider});
    check(scene.mlk().links.size() == 1
            && scene.mlk().links.front().dest_x == 6960,
        "MapScene did not load the MAP sibling MLK");

    dhsh::Character player({1, 10}, dhsh::Direction::down);
    player.set_animation(dhsh::EntityState::idle, dhsh::Direction::down,
        clip(0, 10, {pixel(200, 0, 0)}));
    scene.set_player(std::move(player));

    dhsh::Entity npc({2, 30}, dhsh::Direction::down);
    npc.set_animation(dhsh::EntityState::idle, dhsh::Direction::down,
        clip(0, -10, {pixel(0, 0, 200), pixel(200, 200, 0)}));
    scene.add_npc(std::move(npc));

    dhsh::SoftwareRenderer renderer(64, 48);
    scene.render(renderer, 0, 0);
    check_color(renderer.target(), 1, 20, 0, 200, 0,
        "a player behind a MAN ground line must be drawn before that object");
    check_color(renderer.target(), 2, 20, 0, 0, 200,
        "an NPC in front of a MAN ground line must be drawn after that object");

    check(scene.advance(80), "MapScene did not advance the local NPC animation");
    check(scene.player() != nullptr
            && scene.player()->state() == dhsh::EntityState::idle
            && scene.npcs().size() == 1 && scene.npcs().front().current_frame_index() == 1,
        "MapScene animation advance changed entity state or skipped the NPC");
    scene.render(renderer, 0, 0);
    check_color(renderer.target(), 2, 20, 200, 200, 0,
        "MapScene did not render the NPC's current anchored frame");
}

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    check(input.good(), "cannot open real entity resource: " + path.string());
    const auto end = input.tellg();
    check(end >= 0, "cannot size real entity resource: " + path.string());
    Bytes data(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!data.empty()) {
        input.read(reinterpret_cast<char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    }
    check(input.good(), "cannot read real entity resource: " + path.string());
    return data;
}

class CacheResources final : public dhsh::ResourceProvider {
public:
    explicit CacheResources(std::filesystem::path root) : root_(std::move(root)) {
    }

    Bytes bytes(std::string_view path) override {
        const auto resolved = resolve(path);
        check(resolved.has_value(), "cached image not found: " + std::string(path));
        return read_file(*resolved);
    }

    bool exists(std::string_view path) const override {
        return resolve(path).has_value();
    }

private:
    std::optional<std::filesystem::path> resolve(std::string_view path) const {
        const std::string relative = dhsh::PakResourceProvider::normalize_logical_path(path);
        if (relative.empty() || relative == ".." || relative.rfind("../", 0) == 0
            || relative.find("/../") != std::string::npos || relative.find(':') != std::string::npos) {
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

dhsh::AnimationClip avatar_clip(dhsh::AvatarAnimation animation) {
    std::vector<dhsh::AnimationFrame> frames;
    frames.reserve(animation.frames.size());
    for (dhsh::AvatarFrame& source : animation.frames) {
        frames.push_back(dhsh::AnimationFrame{
            std::move(source.image), -320, -240, source.duration_ms});
    }
    return dhsh::AnimationClip(std::move(frames));
}

void write_ppm(const std::filesystem::path& path, const dhsh::Image& image) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (std::size_t offset = 0; offset < image.rgba.size(); offset += 4) {
        output.write(reinterpret_cast<const char*>(image.rgba.data() + offset), 3);
    }
    check(output.good(), "cannot write entity scene PPM: " + path.string());
}

int real_main(const std::filesystem::path& shdata,
    const std::filesystem::path& cache,
    const std::optional<std::filesystem::path>& screenshot) {
    dhsh::PakResourceProvider resources(
        shdata, {"config.pak", "map.pak", "player.pak", "npc.pak"});
    CacheResources cache_resources(cache);
    dhsh::CachedImageProvider images(cache_resources);
    dhsh::MapScene scene = dhsh::MapScene::load(resources, images, "YingTianFu");

    const dhsh::MlkLink* spawn_link = nullptr;
    for (const dhsh::MlkLink& link : scene.mlk().links) {
        if (dhsh::PakResourceProvider::normalize_logical_path(link.target)
            == "ytf_yamen.map") {
            spawn_link = &link;
            break;
        }
    }
    check(spawn_link != nullptr, "YingTianFu MLK has no confirmed YTF_YaMen return spawn");
    const dhsh::EntityPosition spawn = dhsh::spawn_from_mlk(*spawn_link);
    const auto facing = dhsh::direction_from_mlk(spawn_link->extra0);
    check(spawn.x == 6968 && spawn.y == 2136
            && facing == dhsh::Direction::left_down
            && !scene.cel().blocked(spawn.x / 16, spawn.y / 16),
        "the confirmed MLK spawn is not a walkable cell center with facing left-down");

    const dhsh::AvatarCatalog catalog(read_file(shdata / "perinfo.pak"));
    const dhsh::Avatar avatar(resources, catalog);
    const dhsh::AvatarPreset preset = dhsh::AvatarCatalog::default_preset();
    dhsh::Character player(spawn, *facing);
    for (const auto [action, state] : {
             std::pair<std::uint32_t, dhsh::EntityState>{10, dhsh::EntityState::idle},
             {30, dhsh::EntityState::walk}}) {
        dhsh::AvatarAnimation assembled = avatar.assemble(preset, action, 1);
        check(assembled.direction == 1 && !assembled.frames.empty(),
            "real avatar Idle/Walk assembly returned no left-down animation");
        player.set_animation(state, *facing, avatar_clip(std::move(assembled)));
    }
    check(player.frame_origin().x == spawn.x - 320
            && player.frame_origin().y == spawn.y - 240,
        "real avatar canvas anchor is not centered on the MLK spawn");
    scene.set_player(std::move(player));

    constexpr dhsh::EntityPosition npc_position{7000, 2152};
    check(!scene.cel().blocked(npc_position.x / 16, npc_position.y / 16),
        "the explicit M1 NPC preset is not on a walkable cell");
    dhsh::Entity npc(npc_position, dhsh::Direction::right_down);
    constexpr std::string_view npc_path = "10009/10/3.apg";
    npc.set_animation(dhsh::EntityState::idle, dhsh::Direction::right_down,
        dhsh::AnimationClip::decode(dhsh::NzDecoder::decode(resources.bytes(npc_path))));
    check(npc.current_frame() != nullptr,
        "real NPC APG did not produce a local animation frame");
    const dhsh::EntityPosition npc_origin = npc.frame_origin();
    check(npc_origin.x == npc_position.x + npc.current_frame()->anchor_x
            && npc_origin.y == npc_position.y + npc.current_frame()->anchor_y,
        "real NPC APG anchor was not applied to its logical position");
    scene.add_npc(std::move(npc));

    dhsh::SoftwareRenderer renderer(800, 600);
    scene.render(renderer, spawn.x - 400, spawn.y - 300);
    if (screenshot.has_value()) {
        write_ppm(*screenshot, renderer.target());
    }
    std::cout << "REAL spawn=" << spawn.x << ',' << spawn.y
              << " facing=" << spawn_link->extra0
              << " player_origin=" << scene.player()->frame_origin().x << ','
              << scene.player()->frame_origin().y
              << " npc=" << npc_path << " npc_position=" << npc_position.x << ','
              << npc_position.y << " npc_origin=" << npc_origin.x << ',' << npc_origin.y
              << " npc_frames=" << scene.npcs().front().current_frame_index() + 1
              << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if ((argc == 4 || argc == 5) && std::string_view(argv[1]) == "--real") {
        try {
            const std::optional<std::filesystem::path> screenshot = argc == 5
                ? std::optional<std::filesystem::path>(argv[4])
                : std::nullopt;
            return real_main(argv[2], argv[3], screenshot);
        } catch (const std::exception& error) {
            std::cerr << "real entity verification failed: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc != 1) {
        return 2;
    }
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests = {
        {"Idle/Walk state animations", test_idle_walk_transitions_select_the_state_animation},
        {"eight-way vectors and MLK directions", test_eight_direction_vectors_match_action_and_mlk_directions},
        {"zero movement remains idle", test_zero_movement_does_not_enter_walk},
        {"NPC local animation only", test_npc_only_advances_local_animation},
        {"MLK spawn cell center and facing", test_mlk_spawn_is_cell_center_with_confirmed_facing},
        {"entity frame anchors", test_frame_origin_combines_logical_position_and_current_anchor},
        {"MapScene MAN/entity depth and anchors", test_map_scene_mixes_man_and_entities_by_depth_and_anchor},
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
