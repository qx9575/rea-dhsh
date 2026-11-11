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
#include "scene/LocalEntityFactory.h"
#include "scene/MapScene.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <limits>
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
        files.insert_or_assign(std::move(path), std::move(data));
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

struct MlkFixture {
    std::string target = "YTF_YaMen.map";
    std::int32_t dest_x = 0;
    std::int32_t dest_y = 0;
    std::int32_t extra0 = 1;
};

Bytes make_mlk(const std::vector<MlkFixture>& links = {
        MlkFixture{"YTF_YaMen.map", 6960, 2128, 1}}) {
    Bytes result;
    append_text(result, "lnk");
    result.push_back(std::byte{0});
    append_u32(result, 3);
    append_u32(result, 128);
    append_u32(result, 1);
    append_u32(result, static_cast<std::uint32_t>(links.size()));
    for (const MlkFixture& link : links) {
        append_u32(result, 0);
        append_fixed(result, link.target, 64);
        for (const std::int32_t value : {
                0, 0, 0, 0, 0, link.dest_x, link.dest_y, link.extra0, 1, 0}) {
            append_u32(result, static_cast<std::uint32_t>(value));
        }
    }
    set_u32(result, 8, static_cast<std::uint32_t>(result.size()));
    return result;
}

void add_map(MemoryResources& resources, const Bytes& mlk = make_mlk()) {
    Bytes ini;
    append_text(ini,
        "[BASE]\nWIDTH=64\nHEIGHT=48\nBACKGROUND=bg\nCELLFILE=cel\n"
        "[EXT]\nANIFILE=man\n");
    resources.add("map/test.map", nz(ini));
    resources.add("map/test.mlk", nz(mlk));
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

void test_saturated_movement_uses_the_actual_displacement_direction() {
    dhsh::Character character(
        {std::numeric_limits<std::int32_t>::max(), 10}, dhsh::Direction::down);
    character.move_by(1, -1);
    check(character.position().x == std::numeric_limits<std::int32_t>::max()
            && character.position().y == 9
            && character.direction() == dhsh::Direction::up
            && character.state() == dhsh::EntityState::walk,
        "saturated X movement retained the requested diagonal instead of actual Up");

    character.set_position({std::numeric_limits<std::int32_t>::min(), 0});
    character.stop();
    character.move_by(-1, 0);
    check(character.position().x == std::numeric_limits<std::int32_t>::min()
            && character.position().y == 0
            && character.direction() == dhsh::Direction::up
            && character.state() == dhsh::EntityState::idle,
        "a fully saturated move changed position, facing, or state");
}

void test_negative_pixel_coordinates_use_floor_cell_division() {
    check(dhsh::cell_from_pixel(0) == 0 && dhsh::cell_from_pixel(15) == 0
            && dhsh::cell_from_pixel(16) == 1
            && dhsh::cell_from_pixel(-1) == -1
            && dhsh::cell_from_pixel(-16) == -1
            && dhsh::cell_from_pixel(-17) == -2
            && dhsh::cell_from_pixel(std::numeric_limits<std::int32_t>::min())
                == -134217728
            && dhsh::cell_from_pixel(std::numeric_limits<std::int32_t>::max())
                == 134217727,
        "pixel-to-cell conversion truncated a negative coordinate toward zero");
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

void test_eight_direction_animations_load_lazily_and_stay_bounded() {
    dhsh::Character character({120, 160}, dhsh::Direction::left_down);
    std::size_t loads = 0;
    character.set_animation_loader(
        [&](dhsh::EntityState state, dhsh::Direction direction) {
            ++loads;
            const unsigned red = (state == dhsh::EntityState::idle ? 10U : 30U)
                + static_cast<unsigned>(direction);
            return clip(0, 0, {pixel(red, 0, 0)});
        });

    struct Movement {
        std::int32_t x;
        std::int32_t y;
        dhsh::Direction direction;
    };
    constexpr std::array movements = {
        Movement{-1, 0, dhsh::Direction::left},
        Movement{-1, 1, dhsh::Direction::left_down},
        Movement{0, 1, dhsh::Direction::down},
        Movement{1, 1, dhsh::Direction::right_down},
        Movement{1, 0, dhsh::Direction::right},
        Movement{1, -1, dhsh::Direction::right_up},
        Movement{0, -1, dhsh::Direction::up},
        Movement{-1, -1, dhsh::Direction::left_up},
    };
    const auto visit_all = [&] {
        for (const Movement& movement : movements) {
            character.move_by(movement.x, movement.y);
            check(character.direction() == movement.direction
                    && character.current_frame() != nullptr,
                "turning selected an empty Walk animation");
            const unsigned walk_red = 30U + static_cast<unsigned>(movement.direction);
            check(character.current_frame()->image.rgba[0] == static_cast<std::byte>(walk_red),
                "turning loaded the wrong Walk direction");
            character.stop();
            check(character.current_frame() != nullptr,
                "stopping after a turn selected an empty Idle animation");
            const unsigned idle_red = 10U + static_cast<unsigned>(movement.direction);
            check(character.current_frame()->image.rgba[0] == static_cast<std::byte>(idle_red),
                "stopping loaded the wrong Idle direction");
        }
    };
    visit_all();
    check(loads == 16,
        "eight directions must cache at most the sixteen Idle/Walk clips");
    visit_all();
    check(loads == 16, "revisiting a direction reloaded an already cached clip");
}

void test_failed_lazy_load_preserves_entity_state() {
    dhsh::Character character({120, 160}, dhsh::Direction::left_down);
    character.set_animation(dhsh::EntityState::idle, dhsh::Direction::left_down,
        clip(0, 0, {pixel(10, 0, 0)}));
    character.set_animation_loader(
        [&](dhsh::EntityState, dhsh::Direction) -> dhsh::AnimationClip {
            throw std::runtime_error("intentional animation load failure");
        });

    bool move_threw = false;
    try {
        character.move_by(1, 0);
    } catch (const std::runtime_error&) {
        move_threw = true;
    }
    check(move_threw, "a missing lazy Walk animation did not report its error");
    check(character.position() == dhsh::EntityPosition{120, 160}
            && character.direction() == dhsh::Direction::left_down
            && character.state() == dhsh::EntityState::idle
            && character.current_frame() != nullptr
            && character.current_frame()->image.rgba[0] == std::byte{10},
        "failed Walk loading left a moved entity without its previous valid frame");

    character.set_animation(dhsh::EntityState::walk, dhsh::Direction::right,
        clip(0, 0, {pixel(30, 0, 0)}));
    character.move_by(1, 0);
    bool stop_threw = false;
    try {
        character.stop();
    } catch (const std::runtime_error&) {
        stop_threw = true;
    }
    check(stop_threw, "a missing lazy Idle animation did not report its error");
    check(character.position() == dhsh::EntityPosition{121, 160}
            && character.direction() == dhsh::Direction::right
            && character.state() == dhsh::EntityState::walk
            && character.current_frame() != nullptr
            && character.current_frame()->image.rgba[0] == std::byte{30},
        "failed Idle loading discarded the previous valid Walk state");
}

void check_spawn_rejected(const Bytes& mlk, const Bytes* cel,
    std::string_view expected_error) {
    MemoryResources resources;
    add_map(resources, mlk);
    if (cel != nullptr) {
        resources.add("cel", nz(*cel));
    }
    dhsh::CachedImageProvider images(resources, [](std::span<const std::byte>) {
        dhsh::Image background;
        background.width = 64;
        background.height = 48;
        background.rgba.assign(64U * 48U * 4U, std::byte{0xff});
        return background;
    });
    GreenManProvider provider;
    dhsh::MapScene scene = dhsh::MapScene::load(resources, images, "test", {&provider});
    try {
        static_cast<void>(dhsh::find_local_spawn(scene));
    } catch (const std::exception& error) {
        check(std::string_view(error.what()).find(expected_error)
                != std::string_view::npos,
            "confirmed spawn rejected for an unexpected reason: "
                + std::string(error.what()));
        return;
    }
    throw std::runtime_error("invalid confirmed spawn was accepted or fell back");
}

void test_invalid_confirmed_spawn_does_not_fall_back_to_another_link() {
    {
        MemoryResources resources;
        add_map(resources, make_mlk({MlkFixture{"YTF_YaMen.map", 0, 0, 1}}));
        dhsh::CachedImageProvider images(resources, [](std::span<const std::byte>) {
            dhsh::Image background;
            background.width = 64;
            background.height = 48;
            background.rgba.assign(64U * 48U * 4U, std::byte{0xff});
            return background;
        });
        GreenManProvider provider;
        dhsh::MapScene scene = dhsh::MapScene::load(
            resources, images, "test", {&provider});
        const dhsh::LocalSpawn spawn = dhsh::find_local_spawn(scene);
        check(spawn.position == dhsh::EntityPosition{8, 8}
                && spawn.direction == dhsh::Direction::left_down,
            "walkable-cell spawn fixture did not isolate validation branches");
    }

    check_spawn_rejected(make_mlk({
        MlkFixture{"YTF_YaMen.map", 0, 0, 8},
        MlkFixture{"AnotherGate.map", 0, 0, 1},
    }), nullptr, "invalid facing");
    Bytes blocked_cel = make_cel();
    blocked_cel[24] = std::byte{0x80};
    check_spawn_rejected(make_mlk({
        MlkFixture{"YTF_YaMen.map", 0, 0, 1},
        MlkFixture{"AnotherGate.map", 16, 0, 1},
    }), &blocked_cel, "not on a walkable cell");
    check_spawn_rejected(make_mlk({
        MlkFixture{"AnotherGate.map", 0, 0, 1},
    }), nullptr, "no confirmed YTF_YaMen.map spawn");
}

void test_missing_npc_pak_is_nonfatal() {
    MemoryResources resources;
    add_map(resources);
    dhsh::CachedImageProvider images(resources, [](std::span<const std::byte>) {
        dhsh::Image background;
        background.width = 64;
        background.height = 48;
        background.rgba.assign(64U * 48U * 4U, std::byte{0xff});
        return background;
    });
    GreenManProvider provider;
    dhsh::MapScene scene = dhsh::MapScene::load(resources, images, "test", {&provider});
    check(!dhsh::add_local_npc_if_available(
              scene, "test-fixtures/missing-npc-pak", std::cerr),
        "a missing NPC PAK unexpectedly reported success");
    check(scene.npcs().empty(), "a failed NPC load left a partial entity in the scene");
}

void test_bad_npc_resource_is_nonfatal_and_preserves_existing_npcs() {
    MemoryResources resources;
    add_map(resources);
    dhsh::CachedImageProvider images(resources, [](std::span<const std::byte>) {
        dhsh::Image background;
        background.width = 64;
        background.height = 48;
        background.rgba.assign(64U * 48U * 4U, std::byte{0xff});
        return background;
    });
    GreenManProvider provider;
    dhsh::MapScene scene = dhsh::MapScene::load(resources, images, "test", {&provider});
    dhsh::Entity existing({300, 320}, dhsh::Direction::right_down);
    existing.set_animation(dhsh::EntityState::idle, dhsh::Direction::right_down,
        clip(0, 0, {pixel(7, 8, 9)}));
    scene.add_npc(std::move(existing));

    MemoryResources bad_npc;
    bad_npc.add("10009/10/3.apg", Bytes{std::byte{1}, std::byte{2}});
    check(!dhsh::add_local_npc_if_available(scene, bad_npc, std::cerr),
        "a corrupt NPC animation unexpectedly reported success");
    check(scene.npcs().size() == 1
            && scene.npcs().front().position() == dhsh::EntityPosition{300, 320}
            && scene.npcs().front().current_frame() != nullptr
            && scene.npcs().front().current_frame()->image.rgba[0] == std::byte{7},
        "a failed NPC decode polluted the existing scene entities");
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
            || relative.find("/../") != std::string::npos
            || relative.ends_with("/..") || relative.find(':') != std::string::npos) {
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
    output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (std::size_t offset = 0; offset < image.rgba.size(); offset += 4) {
        output.write(reinterpret_cast<const char*>(image.rgba.data() + offset), 3);
    }
    check(output.good(), "cannot write entity scene PPM: " + path.string());
}

std::uint64_t rgb_fnv1a64(const dhsh::Image& image) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t offset = 0; offset < image.rgba.size(); offset += 4) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            hash ^= std::to_integer<unsigned>(image.rgba[offset + channel]);
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

int real_main(const std::filesystem::path& shdata,
    const std::filesystem::path& cache,
    const std::optional<std::filesystem::path>& screenshot) {
    dhsh::PakResourceProvider resources(
        shdata, {"config.pak", "map.pak", "player.pak"});
    CacheResources cache_resources(cache);
    dhsh::CachedImageProvider images(cache_resources);
    dhsh::MapScene scene = dhsh::MapScene::load(resources, images, "YingTianFu");

    const dhsh::LocalSpawn spawn = dhsh::find_local_spawn(scene);
    check(spawn.position.x == 6968 && spawn.position.y == 2136
            && spawn.direction == dhsh::Direction::left_down,
        "the confirmed MLK spawn changed position or facing");
    const dhsh::AvatarCatalog catalog(read_file(shdata / "perinfo.pak"));
    scene.set_player(dhsh::make_local_character(resources, catalog, spawn));
    check(scene.player()->frame_origin().x == spawn.position.x - 320
            && scene.player()->frame_origin().y == spawn.position.y - 240,
        "real avatar canvas anchor is not centered on the MLK spawn");

    constexpr dhsh::EntityPosition npc_position{7000, 2152};
    check(dhsh::add_local_npc_if_available(scene, shdata, std::cerr)
            && scene.npcs().size() == 1,
        "the explicit local NPC preset was not loaded through production logic");
    const dhsh::Entity& npc = scene.npcs().front();
    check(npc.position() == npc_position && npc.current_frame() != nullptr,
        "real NPC APG did not produce the expected local animation");
    const dhsh::EntityPosition npc_origin = npc.frame_origin();
    check(npc_origin.x == npc_position.x + npc.current_frame()->anchor_x
            && npc_origin.y == npc_position.y + npc.current_frame()->anchor_y,
        "real NPC APG anchor was not applied to its logical position");

    dhsh::SoftwareRenderer renderer(800, 600);
    scene.render(renderer, spawn.position.x - 400, spawn.position.y - 300);
    check(rgb_fnv1a64(renderer.target()) == 0xfc5b7bc888bd4c91ULL,
        "final production entity viewport pixels differ from the verified baseline");
    if (screenshot.has_value()) {
        write_ppm(*screenshot, renderer.target());
    }

    struct Movement {
        std::int32_t x;
        std::int32_t y;
        dhsh::Direction direction;
    };
    constexpr std::array movements = {
        Movement{-1, 0, dhsh::Direction::left},
        Movement{-1, 1, dhsh::Direction::left_down},
        Movement{0, 1, dhsh::Direction::down},
        Movement{1, 1, dhsh::Direction::right_down},
        Movement{1, 0, dhsh::Direction::right},
        Movement{1, -1, dhsh::Direction::right_up},
        Movement{0, -1, dhsh::Direction::up},
        Movement{-1, -1, dhsh::Direction::left_up},
    };
    for (const Movement& movement : movements) {
        scene.player()->move_by(movement.x, movement.y);
        check(scene.player()->direction() == movement.direction
                && scene.player()->current_frame() != nullptr,
            "real avatar Walk disappeared after turning");
        scene.player()->stop();
        check(scene.player()->current_frame() != nullptr,
            "real avatar Idle disappeared after turning");
    }

    std::cout << "REAL spawn=" << spawn.position.x << ',' << spawn.position.y
              << " facing=" << static_cast<unsigned>(spawn.direction)
              << " player_origin=" << spawn.position.x - 320 << ','
              << spawn.position.y - 240
              << " npc_position=" << npc_position.x << ',' << npc_position.y
              << " npc_origin=" << npc_origin.x << ',' << npc_origin.y
              << " npc_frames=" << npc.current_frame_count()
              << " viewport_rgb_fnv1a64=fc5b7bc888bd4c91"
              << " eight_direction_turns=8\n";
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
        {"saturated movement actual direction", test_saturated_movement_uses_the_actual_displacement_direction},
        {"negative pixel floor cell division", test_negative_pixel_coordinates_use_floor_cell_division},
        {"NPC local animation only", test_npc_only_advances_local_animation},
        {"MLK spawn cell center and facing", test_mlk_spawn_is_cell_center_with_confirmed_facing},
        {"entity frame anchors", test_frame_origin_combines_logical_position_and_current_anchor},
        {"MapScene MAN/entity depth and anchors", test_map_scene_mixes_man_and_entities_by_depth_and_anchor},
        {"lazy bounded eight-direction animation cache", test_eight_direction_animations_load_lazily_and_stay_bounded},
        {"failed lazy load preserves entity state", test_failed_lazy_load_preserves_entity_state},
        {"invalid confirmed spawn has no fallback", test_invalid_confirmed_spawn_does_not_fall_back_to_another_link},
        {"missing NPC PAK is nonfatal", test_missing_npc_pak_is_nonfatal},
        {"bad NPC resource preserves existing entities", test_bad_npc_resource_is_nonfatal_and_preserves_existing_npcs},
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
