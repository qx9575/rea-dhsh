#include "avatar/AvatarCatalog.h"
#include "input/MapInput.h"
#include "navigation/NavGrid.h"
#include "navigation/PathFinder.h"
#include "render/SoftwareRenderer.h"
#include "resource/CachedImageProvider.h"
#include "resource/NzDecoder.h"
#include "resource/PakResourceProvider.h"
#include "scene/LocalEntityFactory.h"
#include "scene/MapScene.h"
#include "scene/PakManObjectProvider.h"
#include "script/FsLuaHost.h"
#include "script/ScpParser.h"
#include "ui/MessageBus.h"
#include "ui/ScpWindow.h"
#include "ui/Widget.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
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

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    check(static_cast<bool>(input), "cannot open real resource: " + path.string());
    const auto end = input.tellg();
    check(end >= 0, "cannot size real resource: " + path.string());
    Bytes data(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!data.empty()) {
        input.read(reinterpret_cast<char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    }
    check(static_cast<bool>(input), "cannot read real resource: " + path.string());
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

dhsh::WindowEvent mouse_event(
    dhsh::WindowEvent::Type type, dhsh::LogicalPoint point) {
    dhsh::WindowEvent event;
    event.type = type;
    event.logical_position = point;
    return event;
}

void verify_real_lua_callback(const std::filesystem::path& shdata) {
    dhsh::PakResourceProvider resources(shdata, {"config.pak"});
    const std::string path = "ui/login/bulletin2.scp";
    Bytes source = resources.bytes(path);
    check(source.size() >= 2 && source[0] == std::byte{'N'}
            && source[1] == std::byte{'Z'},
        "real bulletin2.scp was not its original NZ resource");
    source = dhsh::NzDecoder::decode(source);

    dhsh::MessageBus messages;
    {
        dhsh::ScpWindow window(
            dhsh::ScpParser::parse(source, path), resources, messages);
        check(window.lua_host() != nullptr
                && window.lua_host()->registered_callback_count() == 3,
            "real SCP did not register its three original Lua callbacks");
        dhsh::Widget* policy = window.find_widget("Bulletin_win06");
        dhsh::Widget* bulletin = window.find_widget("Bulletin_win00");
        dhsh::Widget* license = window.find_widget("Bulletin_win03");
        dhsh::Widget* policy_button = window.find_widget("Bulletin_main_title06");
        dhsh::Widget* bulletin_button = window.find_widget("Bulletin_main_title05");
        dhsh::Widget* license_button = window.find_widget("Bulletin_main_title03");
        check(policy != nullptr && bulletin != nullptr && license != nullptr
                && policy_button != nullptr && bulletin_button != nullptr
                && license_button != nullptr,
            "real SCP callback controls are missing");
        const bool initial_state = !policy->visible() && bulletin->visible()
            && !license->visible() && policy_button->enabled()
            && !bulletin_button->enabled() && license_button->enabled();
        check(initial_state,
            "real SCP initial state differs: policy_visible="
            + std::to_string(policy->visible()) + " bulletin_visible="
            + std::to_string(bulletin->visible()) + " license_visible="
            + std::to_string(license->visible()) + " policy_enabled="
            + std::to_string(policy_button->enabled()) + " bulletin_enabled="
            + std::to_string(bulletin_button->enabled()) + " license_enabled="
            + std::to_string(license_button->enabled()));

        using Type = dhsh::WindowEvent::Type;
        check(window.handle_event(mouse_event(Type::left_button_down, {160, 115}))
                && window.handle_event(mouse_event(Type::left_button_up, {160, 115})),
            "real SCP policy button did not consume the click");
        check(window.lua_host()->callback_count() == 1,
            "real SCP click did not invoke the original Lua callback exactly once");
        check(policy->visible() && !bulletin->visible() && !license->visible()
                && !policy_button->enabled() && bulletin_button->enabled()
                && license_button->enabled(),
            "original OnShowPolicy did not change the real SCP window state");
    }
    check(!messages.dispatch({"Bulletin_main_title06", "FSM_BTN_MSG", 1,
              "OnShowPolicy", path}),
        "destroyed SCP window left a live Lua message subscription");
    std::cout << "M1 lua PASS source=" << path
              << " registered=3 calls=1 cleanup=unsubscribed\n";
}

int run_real_acceptance(const std::filesystem::path& shdata,
    const std::filesystem::path& cache) {
    dhsh::PakResourceProvider resources(
        shdata, {"config.pak", "map.pak", "player.pak"});
    CacheResources cache_resources(cache);
    dhsh::CachedImageProvider images(cache_resources);
    dhsh::PakManObjectProvider object_provider(resources);
    dhsh::MapScene scene = dhsh::MapScene::load(
        resources, images, "YingTianFu", {&object_provider});
    check(scene.name() == "YingTianFu"
            && scene.background().width == 7808 && scene.background().height == 5760
            && scene.objects().size() == 103 && scene.skipped_objects().empty()
            && scene.core().has_value() && !scene.cover_masks().empty(),
        "real YingTianFu scene, MAN objects, or core masks did not load");
    const bool depth_sorted = std::is_sorted(scene.objects().begin(), scene.objects().end(),
        [](const dhsh::SceneObject& left, const dhsh::SceneObject& right) {
            return left.sort_y(0) < right.sort_y(0);
        });
    check(depth_sorted, "real MAN objects are not ordered by their production depth key");
    std::cout << "M1 scene PASS background=7808x5760 objects="
              << scene.objects().size() << " masks=" << scene.cover_masks().size()
              << " depth_sorted=true\n";

    const dhsh::LocalSpawn spawn = dhsh::find_local_spawn(scene);
    check(spawn.position == dhsh::EntityPosition{6968, 2136}
            && spawn.direction == dhsh::Direction::left_down,
        "confirmed MLK spawn position or facing changed");
    const dhsh::AvatarCatalog catalog(read_file(shdata / "perinfo.pak"));
    scene.set_player(dhsh::make_local_character(resources, catalog, spawn));
    check(scene.player() != nullptr && scene.player()->current_frame() != nullptr,
        "real local character has no spawn animation");
    check(dhsh::add_local_npc_if_available(scene, shdata, std::cerr)
            && scene.npcs().size() == 1
            && scene.npcs().front().current_frame() != nullptr,
        "real local NPC preset did not load");
    dhsh::SoftwareRenderer renderer(800, 600);
    dhsh::SceneRenderDiagnostics initial_render;
    scene.render(renderer, spawn.position.x - 400, spawn.position.y - 300,
        &initial_render);
    check(renderer.target().rgba.size() == 800U * 600U * 4U
            && initial_render.cover_tests > 0,
        "real scene viewport or entity/core interaction was not rendered");
    std::cout << "M1 spawn PASS position=6968,2136 facing=left_down"
              << " npc=10009/10/3.apg cover_tests=" << initial_render.cover_tests << '\n';

    const dhsh::NavGrid grid(scene.cel());
    const std::int32_t camera_x = spawn.position.x - 400;
    const std::int32_t camera_y = spawn.position.y - 300;
    dhsh::MapInput map_input({});
    const dhsh::MapInputView input_view{
        static_cast<double>(camera_x), static_cast<double>(camera_y), 1.0};
    const auto click_cell = [&](dhsh::NavCell cell) {
        const auto center = grid.cell_center(cell);
        const dhsh::LogicalPoint point{center.x - camera_x, center.y - camera_y};
        check(point.x >= 0 && point.x < 800 && point.y >= 0 && point.y < 600,
            "real click target is outside the initial viewport");
        using Type = dhsh::WindowEvent::Type;
        map_input.handle_event(mouse_event(Type::left_button_down, point),
            input_view, false);
        const auto click = map_input.handle_event(
            mouse_event(Type::left_button_up, point), input_view, false);
        check(click.has_value()
                && grid.cell_at_pixel({click->x, click->y}) == cell,
            "MapInput did not deliver the intended real-map click target");
        return *click;
    };

    dhsh::PathMovement movement(grid, dhsh::MovementConfig{120.0});
    std::optional<dhsh::NavCell> blocked_goal;
    for (std::int32_t y = 0; y < static_cast<std::int32_t>(grid.height())
            && !blocked_goal.has_value(); ++y) {
        for (std::int32_t x = 0; x < static_cast<std::int32_t>(grid.width()); ++x) {
            const auto center = grid.cell_center({x, y});
            const std::int32_t screen_x = center.x - camera_x;
            const std::int32_t screen_y = center.y - camera_y;
            if (!grid.walkable({x, y}) && screen_x >= 0 && screen_x < 800
                && screen_y >= 0 && screen_y < 600) {
                blocked_goal = dhsh::NavCell{x, y};
                break;
            }
        }
    }
    check(blocked_goal.has_value(), "real CEL has no visible blocked click target");
    const dhsh::MapPoint blocked_click = click_cell(*blocked_goal);
    check(movement.set_destination(*scene.player(),
              grid.cell_at_pixel({blocked_click.x, blocked_click.y}))
            == dhsh::PathStatus::goal_blocked && !movement.moving()
            && scene.player()->position() == spawn.position,
        "blocked real-map click moved the player or entered Walk");

    const dhsh::NavCell goal = grid.cell_at_pixel({6808, 1896});
    const dhsh::MapPoint reachable_click = click_cell(goal);
    check(movement.set_destination(*scene.player(),
              grid.cell_at_pixel({reachable_click.x, reachable_click.y}))
            == dhsh::PathStatus::success && movement.moving(),
        "known reachable real-map click did not produce a path");
    std::size_t movement_samples = 0;
    std::size_t blocked_samples = 0;
    std::size_t walk_frame_changes = 0;
    while (movement.moving()) {
        check(movement_samples < 1000, "real-map path did not finish within 50 seconds");
        const bool walking = scene.player()->state() == dhsh::EntityState::walk;
        const std::size_t walk_frame = scene.player()->current_frame_index();
        movement.advance(*scene.player(), 50.0);
        scene.advance(50);
        ++movement_samples;
        if (walking && scene.player()->current_frame_index() != walk_frame) {
            ++walk_frame_changes;
        }
        const auto position = scene.player()->position();
        if (!grid.walkable(grid.cell_at_pixel({position.x, position.y}))) {
            ++blocked_samples;
        }
    }
    const auto target = grid.cell_center(goal);
    check(scene.player()->position() == dhsh::EntityPosition{target.x, target.y}
            && scene.player()->state() == dhsh::EntityState::idle
            && movement_samples > 1 && blocked_samples == 0 && walk_frame_changes > 0,
        "reachable click did not arrive, stay walkable, animate Walk, and return to Idle");

    const dhsh::NavCell start = grid.cell_at_pixel({spawn.position.x, spawn.position.y});
    std::vector<bool> reached(
        static_cast<std::size_t>(grid.width()) * grid.height(), false);
    std::deque<dhsh::NavCell> pending{start};
    reached[static_cast<std::size_t>(start.y) * grid.width() + start.x] = true;
    while (!pending.empty()) {
        const dhsh::NavCell current = pending.front();
        pending.pop_front();
        for (const dhsh::NavCell delta : dhsh::PathFinder::neighbor_order()) {
            const dhsh::NavCell next{current.x + delta.x, current.y + delta.y};
            if (!grid.walkable(next)) {
                continue;
            }
            if (delta.x != 0 && delta.y != 0
                && (!grid.walkable({current.x + delta.x, current.y})
                    || !grid.walkable({current.x, current.y + delta.y}))) {
                continue;
            }
            const std::size_t index =
                static_cast<std::size_t>(next.y) * grid.width() + next.x;
            if (!reached[index]) {
                reached[index] = true;
                pending.push_back(next);
            }
        }
    }
    std::optional<dhsh::NavCell> unreachable_goal;
    int nearest_distance = -1;
    for (std::int32_t y = 0; y < static_cast<std::int32_t>(grid.height()); ++y) {
        for (std::int32_t x = 0; x < static_cast<std::int32_t>(grid.width()); ++x) {
            const dhsh::NavCell cell{x, y};
            if (!grid.walkable(cell)
                || reached[static_cast<std::size_t>(y) * grid.width() + x]) {
                continue;
            }
            const int distance = abs(x - start.x) + abs(y - start.y);
            if (nearest_distance < 0 || distance < nearest_distance) {
                nearest_distance = distance;
                unreachable_goal = cell;
            }
        }
    }
    check(unreachable_goal.has_value(),
        "real CEL has no walkable target disconnected from the spawn component");
    const auto unreachable_center = grid.cell_center(*unreachable_goal);
    dhsh::MapInput disconnected_input({});
    const dhsh::MapInputView disconnected_view{
        static_cast<double>(unreachable_center.x - 400),
        static_cast<double>(unreachable_center.y - 300), 1.0};
    using Type = dhsh::WindowEvent::Type;
    disconnected_input.handle_event(
        mouse_event(Type::left_button_down, {400, 300}), disconnected_view, false);
    const auto disconnected_click = disconnected_input.handle_event(
        mouse_event(Type::left_button_up, {400, 300}), disconnected_view, false);
    check(disconnected_click.has_value()
            && grid.cell_at_pixel({disconnected_click->x, disconnected_click->y})
                == *unreachable_goal,
        "MapInput did not deliver the disconnected real-map target");
    const auto before_unreachable = scene.player()->position();
    check(movement.set_destination(*scene.player(), *unreachable_goal)
            == dhsh::PathStatus::unreachable && !movement.moving()
            && scene.player()->position() == before_unreachable
            && scene.player()->state() == dhsh::EntityState::idle,
        "walkable disconnected click did not report unreachable without moving");
    std::cout << "M1 navigation PASS blocked=goal_blocked unreachable="
              << unreachable_goal->x << ',' << unreachable_goal->y
              << " target=6808,1896 samples=" << movement_samples
              << " blocked_samples=" << blocked_samples
              << " walk_frame_changes=" << walk_frame_changes << '\n';

    struct Turn {
        std::int32_t x;
        std::int32_t y;
        dhsh::Direction direction;
    };
    constexpr std::array turns = {
        Turn{-1, 0, dhsh::Direction::left},
        Turn{-1, 1, dhsh::Direction::left_down},
        Turn{0, 1, dhsh::Direction::down},
        Turn{1, 1, dhsh::Direction::right_down},
        Turn{1, 0, dhsh::Direction::right},
        Turn{1, -1, dhsh::Direction::right_up},
        Turn{0, -1, dhsh::Direction::up},
        Turn{-1, -1, dhsh::Direction::left_up},
    };
    std::size_t walk_playbacks = 0;
    std::size_t idle_playbacks = 0;
    for (const Turn& turn : turns) {
        scene.player()->move_by(turn.x, turn.y);
        check(scene.player()->direction() == turn.direction
                && scene.player()->state() == dhsh::EntityState::walk
                && scene.player()->current_frame() != nullptr
                && scene.player()->current_frame_count() > 1,
            "real avatar Walk animation missing after an eight-way turn");
        const std::size_t walk_frame = scene.player()->current_frame_index();
        scene.advance(std::max(80U, scene.player()->current_frame()->duration_ms));
        if (scene.player()->current_frame_index() != walk_frame) {
            ++walk_playbacks;
        }
        scene.player()->stop();
        check(scene.player()->current_frame() != nullptr
                && scene.player()->current_frame_count() > 1
                && scene.player()->state() == dhsh::EntityState::idle,
            "real avatar Idle animation missing after an eight-way turn");
        const std::size_t idle_frame = scene.player()->current_frame_index();
        scene.advance(std::max(80U, scene.player()->current_frame()->duration_ms));
        if (scene.player()->current_frame_index() != idle_frame) {
            ++idle_playbacks;
        }
    }
    check(walk_playbacks == turns.size() && idle_playbacks == turns.size(),
        "a real eight-way Walk or Idle clip did not advance its frame");
    std::cout << "M1 eight_directions PASS turns=" << turns.size()
              << " walk_playbacks=" << walk_playbacks
              << " idle_playbacks=" << idle_playbacks << '\n';

    const std::size_t npc_frame = scene.npcs().front().current_frame_index();
    std::vector<std::size_t> man_frames;
    man_frames.reserve(scene.objects().size());
    for (const dhsh::SceneObject& object : scene.objects()) {
        man_frames.push_back(object.current_frame_index());
    }
    std::size_t man_frame_changes = 0;
    bool npc_advanced = false;
    for (int step = 0; step < 20; ++step) {
        scene.advance(80);
        npc_advanced = npc_advanced
            || scene.npcs().front().current_frame_index() != npc_frame;
        for (std::size_t index = 0; index < scene.objects().size(); ++index) {
            if (scene.objects()[index].current_frame_index() != man_frames[index]) {
                ++man_frame_changes;
                man_frames[index] = scene.objects()[index].current_frame_index();
            }
        }
    }
    check(npc_advanced && man_frame_changes > 0,
        "real NPC or MAN animation did not advance");
    std::cout << "M1 animation PASS npc_advanced=true man_frame_changes="
              << man_frame_changes << '\n';

    auto occlusion_at = [&](dhsh::EntityPosition position)
        -> std::optional<std::pair<std::size_t, dhsh::EntityPosition>> {
        const dhsh::AnimationFrame* frame = scene.player()->current_frame();
        check(frame != nullptr, "occlusion probe lost the real avatar frame");
        const std::int32_t left = position.x + frame->anchor_x;
        const std::int32_t top = position.y + frame->anchor_y;
        for (std::size_t index = 0; index < scene.cover_masks().size(); ++index) {
            const dhsh::CoverMask& mask = scene.cover_masks()[index];
            if (!mask.active() || !mask.intersects_sprite(frame->image, left, top)) {
                continue;
            }
            dhsh::Image masked = frame->image;
            if (!mask.apply(masked, position.x, position.y, left, top)) {
                continue;
            }
            for (std::size_t offset = 0; offset < masked.rgba.size(); offset += 4) {
                const bool opaque_faded = frame->image.rgba[offset + 3] == std::byte{0xff}
                    && masked.rgba[offset + 3] != std::byte{0xff}
                    && masked.rgba[offset] == frame->image.rgba[offset]
                    && masked.rgba[offset + 1] == frame->image.rgba[offset + 1]
                    && masked.rgba[offset + 2] == frame->image.rgba[offset + 2];
                if (opaque_faded) {
                    return std::pair{index, position};
                }
            }
        }
        return std::nullopt;
    };
    auto occlusion = occlusion_at(spawn.position);
    if (!occlusion.has_value()) {
        for (std::int32_t dy = -48; dy <= 48 && !occlusion.has_value(); ++dy) {
            for (std::int32_t dx = -48; dx <= 48; ++dx) {
                const dhsh::NavCell cell{spawn.position.x / 16 + dx,
                    spawn.position.y / 16 + dy};
                if (grid.walkable(cell)) {
                    const auto center = grid.cell_center(cell);
                    occlusion = occlusion_at({center.x, center.y});
                    if (occlusion.has_value()) {
                        break;
                    }
                }
            }
        }
    }
    check(occlusion.has_value(),
        "no real core mask alpha-faded the real avatar near the confirmed spawn");
    scene.player()->set_position(occlusion->second);
    const dhsh::AnimationFrame* occluded_frame = scene.player()->current_frame();
    const std::int32_t frame_left = occlusion->second.x + occluded_frame->anchor_x;
    const std::int32_t frame_top = occlusion->second.y + occluded_frame->anchor_y;
    dhsh::Image expected = occluded_frame->image;
    for (const dhsh::CoverMask& mask : scene.cover_masks()) {
        if (mask.active()) {
            mask.apply(expected, occlusion->second.x, occlusion->second.y,
                frame_left, frame_top);
        }
    }
    std::optional<std::pair<std::uint32_t, std::uint32_t>> changed_pixel;
    for (std::uint32_t y = 0; y < expected.height && !changed_pixel.has_value(); ++y) {
        for (std::uint32_t x = 0; x < expected.width; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * expected.width + x) * 4;
            if (occluded_frame->image.rgba[offset + 3] != std::byte{0xff}
                || expected.rgba[offset + 3] == occluded_frame->image.rgba[offset + 3]
                || expected.rgba[offset] != occluded_frame->image.rgba[offset]
                || expected.rgba[offset + 1] != occluded_frame->image.rgba[offset + 1]
                || expected.rgba[offset + 2] != occluded_frame->image.rgba[offset + 2]) {
                continue;
            }
            const std::int32_t world_x = frame_left + static_cast<std::int32_t>(x);
            const std::int32_t world_y = frame_top + static_cast<std::int32_t>(y);
            bool overdrawn = false;
            for (const dhsh::SceneObject& object : scene.objects()) {
                const auto& image = object.frame.image;
                if (object.sort_y(0) > occlusion->second.y
                    && world_x >= object.x && world_y >= object.y
                    && world_x < object.x + static_cast<std::int32_t>(image.width)
                    && world_y < object.y + static_cast<std::int32_t>(image.height)) {
                    const std::size_t overlay =
                        (static_cast<std::size_t>(world_y - object.y) * image.width
                            + (world_x - object.x)) * 4 + 3;
                    if (image.rgba[overlay] != std::byte{0}) {
                        overdrawn = true;
                        break;
                    }
                }
            }
            for (const dhsh::Entity& npc : scene.npcs()) {
                const dhsh::EntityPosition origin = npc.frame_origin();
                const auto& image = npc.current_frame()->image;
                if (npc.position().y >= occlusion->second.y
                    && world_x >= origin.x && world_y >= origin.y
                    && world_x < origin.x + static_cast<std::int32_t>(image.width)
                    && world_y < origin.y + static_cast<std::int32_t>(image.height)) {
                    const std::size_t overlay =
                        (static_cast<std::size_t>(world_y - origin.y) * image.width
                            + (world_x - origin.x)) * 4 + 3;
                    if (image.rgba[overlay] != std::byte{0}) {
                        overdrawn = true;
                    }
                }
            }
            if (!overdrawn) {
                changed_pixel = std::pair{x, y};
                break;
            }
        }
    }
    check(changed_pixel.has_value(),
        "real core probe has no opaque alpha-faded pixel visible in the final draw order");
    const std::int32_t occlusion_camera_x = occlusion->second.x - 400;
    const std::int32_t occlusion_camera_y = occlusion->second.y - 300;
    const std::size_t expected_offset =
        (static_cast<std::size_t>(changed_pixel->second) * expected.width
            + changed_pixel->first) * 4;
    const std::int32_t viewport_x =
        frame_left + static_cast<std::int32_t>(changed_pixel->first) - occlusion_camera_x;
    const std::int32_t viewport_y =
        frame_top + static_cast<std::int32_t>(changed_pixel->second) - occlusion_camera_y;
    check(viewport_x >= 0 && viewport_x < 800 && viewport_y >= 0 && viewport_y < 600,
        "real core changed pixel is outside the verification viewport");
    const std::size_t actual_offset =
        (static_cast<std::size_t>(viewport_y) * 800 + viewport_x) * 4;

    // Capture the destination under the faded sprite by moving the player off
    // screen for one frame with the same camera; earlier-sorted objects and the
    // background stay identical for the already-filtered visible pixel.
    const dhsh::EntityPosition occluded_position = occlusion->second;
    scene.player()->set_position(
        {occluded_position.x + 1000000, occluded_position.y + 1000000});
    dhsh::SoftwareRenderer background_renderer(800, 600);
    scene.render(background_renderer, occlusion_camera_x, occlusion_camera_y);
    scene.player()->set_position(occluded_position);

    dhsh::SceneRenderDiagnostics occlusion_render;
    scene.render(renderer, occlusion_camera_x, occlusion_camera_y, &occlusion_render);
    const auto blend = [](unsigned source, unsigned destination, unsigned alpha) {
        return (source * alpha + destination * (255U - alpha) + 127U) / 255U;
    };
    const unsigned faded_alpha = std::to_integer<unsigned>(
        expected.rgba[expected_offset + 3]);
    const std::byte* destination =
        background_renderer.target().rgba.data() + actual_offset;
    unsigned expected_rgb[3];
    for (std::size_t channel = 0; channel < 3; ++channel) {
        expected_rgb[channel] = blend(
            std::to_integer<unsigned>(occluded_frame->image.rgba[expected_offset + channel]),
            std::to_integer<unsigned>(destination[channel]), faded_alpha);
    }
    bool matches = true;
    bool changed = false;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const unsigned actual =
            std::to_integer<unsigned>(renderer.target().rgba[actual_offset + channel]);
        matches = matches && actual == expected_rgb[channel];
        changed = changed
            || actual != std::to_integer<unsigned>(
                occluded_frame->image.rgba[expected_offset + channel]);
    }
    check(occlusion_render.covering_masks > 0 && occlusion_render.sprite_copies > 0
            && matches && changed,
        "MapScene::render did not apply the expected real core alpha fade");
    std::cout << "M1 occlusion PASS mask=" << occlusion->first
              << " position=" << occlusion->second.x << ',' << occlusion->second.y
              << " pixel=" << viewport_x << ',' << viewport_y
              << " alpha=" << faded_alpha
              << " covering_masks=" << occlusion_render.covering_masks
              << " sprite_copies=" << occlusion_render.sprite_copies << '\n';

    verify_real_lua_callback(shdata);
    std::cout << "M1 cleanup PASS lua_unsubscribed=true\n";
    std::cout << "M1_SUBSYSTEM PASS real_pak=true map=YingTianFu\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        std::cout << "SKIP: real M1 acceptance requires --real <shdata> <cache>\n";
        return 77;
    }
    if (argc != 4 || std::string_view(argv[1]) != "--real") {
        std::cerr << "usage: dhsh_test_m1_acceptance [--real <shdata> <cache>]\n";
        return 2;
    }
    try {
        return run_real_acceptance(argv[2], argv[3]);
    } catch (const std::exception& error) {
        std::cerr << "M1_ACCEPTANCE FAIL: " << error.what() << '\n';
        return 1;
    }
}
