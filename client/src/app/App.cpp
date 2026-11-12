#include "app/App.h"

#include "audio/AudioPlayer.h"
#include "avatar/AvatarCatalog.h"
#include "diagnostics/Log.h"
#include "diagnostics/StartupReport.h"
#include "formats/A16.h"
#include "formats/Pif.h"
#include "input/MapInput.h"
#include "navigation/NavGrid.h"
#include "navigation/PathFinder.h"
#include "platform/D3D11Presenter.h"
#include "platform/Win32Window.h"
#include "render/ClickStar.h"
#include "render/SoftwareRenderer.h"
#include "resource/CachedImageProvider.h"
#include "resource/NzDecoder.h"
#include "resource/PakResourceProvider.h"
#include "resource/ResourceError.h"
#include "scene/LocalEntityFactory.h"
#include "scene/MapScene.h"
#include "scene/PakManObjectProvider.h"
#include "script/FsLuaHost.h"
#include "script/ScpParser.h"
#include "ui/MessageBus.h"
#include "ui/ScpWindow.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dhsh {
namespace {

using DiagnosticClock = std::chrono::steady_clock;

constexpr double resource_budget_ms = 2000.0;
constexpr double scene_budget_ms = 10000.0;
constexpr double avatar_budget_ms = 10000.0;
constexpr double npc_budget_ms = 2000.0;
constexpr double presenter_budget_ms = 2000.0;
constexpr double render_budget_ms = 1000.0 / 60.0;

double diagnostic_ms(DiagnosticClock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        DiagnosticClock::now() - start).count();
}

struct StageStats {
    double total_ms = 0.0;
    double max_ms = 0.0;
    std::uint64_t samples = 0;

    void add(double value) {
        total_ms += value;
        max_ms = std::max(max_ms, value);
        ++samples;
    }

    double average() const {
        return samples == 0 ? 0.0 : total_ms / samples;
    }

    void reset() {
        total_ms = 0.0;
        max_ms = 0.0;
        samples = 0;
    }
};

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw ResourceError("cannot open cached image: " + path.string());
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw ResourceError("cannot determine cached image size: " + path.string());
    }
    std::vector<std::byte> data(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!data.empty()) {
        input.read(reinterpret_cast<char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    }
    if (!input) {
        throw ResourceError("cannot read cached image: " + path.string());
    }
    return data;
}

void write_ppm(const std::filesystem::path& path, const Image& image) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw ResourceError("cannot create capture: " + path.string());
    }
    output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (std::size_t offset = 0; offset < image.rgba.size(); offset += 4) {
        output.put(static_cast<char>(std::to_integer<unsigned char>(image.rgba[offset])));
        output.put(static_cast<char>(std::to_integer<unsigned char>(image.rgba[offset + 1])));
        output.put(static_cast<char>(std::to_integer<unsigned char>(image.rgba[offset + 2])));
    }
    if (!output) {
        throw ResourceError("cannot write capture: " + path.string());
    }
}

class CacheResourceProvider final : public ResourceProvider {
public:
    explicit CacheResourceProvider(std::filesystem::path root)
        : root_(std::move(root)) {
    }

    std::vector<std::byte> bytes(std::string_view logical_path) override {
        const auto file = resolve(logical_path);
        if (!file.has_value()) {
            throw ResourceError("cached image not found under '" + root_.string()
                + "': " + std::string(logical_path)
                + " (expected the same path with a .png extension)");
        }
        return read_file(*file);
    }

    bool exists(std::string_view logical_path) const override {
        return resolve(logical_path).has_value();
    }

private:
    std::optional<std::filesystem::path> resolve(std::string_view logical_path) const {
        const std::string relative =
            PakResourceProvider::normalize_logical_path(logical_path);
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

struct Options {
    std::filesystem::path shdata;
    std::filesystem::path cache = DHSH_DEFAULT_CACHE_DIR;
    std::filesystem::path capture;
    std::filesystem::path log;
    std::string map = "YingTianFu";
    bool acceptance = false;
    bool help = false;
};

void print_usage(std::ostream& output) {
    output << "usage: dhsh_client --shdata <directory> [--map <name>]"
              " [--cache <directory>] [--log <file>] [--capture <ppm>]"
              " [--acceptance]\n"
              "  --map defaults to YingTianFu; the Task 12 local spawn preset"
              " currently supports only YingTianFu\n"
              "  --acceptance loads the real bulletin2 SCP; F8 toggles its window\n"
              "  --cache defaults to " << DHSH_DEFAULT_CACHE_DIR << '\n';
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            options.help = true;
            continue;
        }
        if (argument == "--acceptance") {
            options.acceptance = true;
            continue;
        }
        if (argument != "--shdata" && argument != "--map" && argument != "--cache"
            && argument != "--log" && argument != "--capture") {
            throw std::invalid_argument("unknown argument: " + std::string(argument));
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value after " + std::string(argument));
        }
        const std::string value(argv[++index]);
        if (value.empty()) {
            throw std::invalid_argument("empty value after " + std::string(argument));
        }
        if (argument == "--shdata") {
            options.shdata = value;
        } else if (argument == "--map") {
            options.map = value;
        } else if (argument == "--cache") {
            options.cache = value;
        } else if (argument == "--log") {
            options.log = value;
        } else {
            options.capture = value;
        }
    }
    if (!options.help && options.shdata.empty()) {
        throw std::invalid_argument("--shdata is required");
    }
    if (!options.help && options.map != "YingTianFu") {
        throw std::invalid_argument(
            "--map currently supports only YingTianFu with the confirmed local spawn");
    }
    return options;
}

Image scale_nearest(const Image& source, std::uint32_t width, std::uint32_t height) {
    if (source.width == 0 || source.height == 0
        || source.rgba.size()
            != static_cast<std::size_t>(source.width) * source.height * 4) {
        throw std::invalid_argument("cannot scale an invalid source framebuffer");
    }
    Image result;
    result.width = width;
    result.height = height;
    result.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint32_t source_y =
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(y)
                * source.height / height);
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t source_x =
                static_cast<std::uint32_t>(static_cast<std::uint64_t>(x)
                    * source.width / width);
            const std::size_t source_offset =
                (static_cast<std::size_t>(source_y) * source.width + source_x) * 4;
            const std::size_t destination_offset =
                (static_cast<std::size_t>(y) * width + x) * 4;
            std::copy_n(source.rgba.begin() + source_offset, 4,
                result.rgba.begin() + destination_offset);
        }
    }
    return result;
}

Image opaque_black(std::uint32_t width, std::uint32_t height) {
    Image result;
    result.width = width;
    result.height = height;
    result.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t offset = 3; offset < result.rgba.size(); offset += 4) {
        result.rgba[offset] = std::byte{0xff};
    }
    return result;
}

struct CursorFrame {
    Image image;
    std::uint32_t delay_ms = 120;
};

struct CursorAnimation {
    std::vector<CursorFrame> frames;
    std::int32_t hotspot_x = 0;
    std::int32_t hotspot_y = 0;
    std::size_t frame = 0;
    double elapsed_ms = 0.0;
};

CursorAnimation load_cursor(PakResourceProvider& resources) {
    CursorAnimation animation;
    const Pif perinfo = Pif::parse(NzDecoder::decode(
        resources.bytes("cursor/00000/perinfo.dat")));
    if (perinfo.rows.empty()) {
        throw ResourceError("cursor perinfo has no frames");
    }
    animation.hotspot_x = static_cast<std::int32_t>(perinfo.rows.front()[0]);
    animation.hotspot_y = static_cast<std::int32_t>(perinfo.rows.front()[1]);
    for (std::size_t index = 0; index < perinfo.rows.size(); ++index) {
        std::ostringstream path;
        path << "cursor/00000/" << std::setw(5) << std::setfill('0')
             << index << ".a16";
        Image image = A16Decoder::decode(NzDecoder::decode(
            resources.bytes(path.str())));
        if (image.width == 0 || image.height == 0) {
            throw ResourceError("cursor frame is empty: " + path.str());
        }
        const std::uint32_t delay = perinfo.rows[index][5] == 0
            ? 120
            : perinfo.rows[index][5];
        animation.frames.push_back(CursorFrame{std::move(image), delay});
    }
    return animation;
}

struct ClickEffect {
    double map_x = 0.0;
    double map_y = 0.0;
    double age_ms = 0.0;
};

constexpr double click_effect_lifetime_ms = 700.0;

std::string_view path_status_name(PathStatus status) {
    switch (status) {
    case PathStatus::success:
        return "success";
    case PathStatus::already_at_goal:
        return "already_at_goal";
    case PathStatus::unreachable:
        return "unreachable";
    case PathStatus::start_out_of_bounds:
        return "start_out_of_bounds";
    case PathStatus::goal_out_of_bounds:
        return "goal_out_of_bounds";
    case PathStatus::start_blocked:
        return "start_blocked";
    case PathStatus::goal_blocked:
        return "goal_blocked";
    }
    return "unknown";
}

} // namespace

double frame_elapsed_ms(
    std::int64_t previous, std::int64_t current, std::int64_t frequency) {
    if (frequency <= 0 || current <= previous) {
        return 0.0;
    }
    const long double elapsed =
        (static_cast<long double>(current) - static_cast<long double>(previous))
        * 1000.0L / static_cast<long double>(frequency);
    const double result = static_cast<double>(elapsed);
    return std::isfinite(result) && result >= 0.0 ? result : 0.0;
}

double approach_camera_coordinate(
    double current, double target, double elapsed_ms) noexcept {
    if (!std::isfinite(current) || !std::isfinite(target)
        || !std::isfinite(elapsed_ms) || elapsed_ms <= 0.0) {
        return current;
    }
    const double difference = target - current;
    if (std::abs(difference) <= 0.01) {
        return target;
    }
    const double weight = 1.0 - std::exp(-elapsed_ms / 120.0);
    return current + difference * weight;
}

void InputState::handle_event(const WindowEvent& event) {
    if (event.type == WindowEvent::Type::input_cancelled) {
        move_left_ = false;
        move_right_ = false;
        move_up_ = false;
        move_down_ = false;
        return;
    }
    if (event.type == WindowEvent::Type::key_down
        || event.type == WindowEvent::Type::key_up) {
        const bool pressed = event.type == WindowEvent::Type::key_down;
        if (event.key == VK_LEFT || event.key == 'A') {
            move_left_ = pressed;
        } else if (event.key == VK_RIGHT || event.key == 'D') {
            move_right_ = pressed;
        } else if (event.key == VK_UP || event.key == 'W') {
            move_up_ = pressed;
        } else if (event.key == VK_DOWN || event.key == 'S') {
            move_down_ = pressed;
        }
    }
}

double InputState::horizontal() const noexcept {
    return static_cast<double>(move_right_) - static_cast<double>(move_left_);
}

double InputState::vertical() const noexcept {
    return static_cast<double>(move_down_) - static_cast<double>(move_up_);
}

int App::logical_width() const noexcept {
    return 800;
}

int App::logical_height() const noexcept {
    return 600;
}

int App::run(int argc, char** argv) {
    std::optional<StartupReport> startup_report;
    std::optional<Log> log;
    try {
        Options options = parse_options(argc, argv);
        if (options.help) {
            print_usage(std::cout);
            return 0;
        }
        startup_report.emplace(options.shdata, options.map);
        log.emplace(options.log);

        const auto startup_start = DiagnosticClock::now();
        auto stage_start = startup_start;
        const auto log_startup = [&](std::string_view name, double budget_ms) {
            const double elapsed = diagnostic_ms(stage_start);
            stage_start = DiagnosticClock::now();
            std::cout << "Startup " << name << " ms=" << std::fixed
                      << std::setprecision(2) << elapsed << " budget_ms=" << budget_ms
                      << (elapsed <= budget_ms ? " PASS" : " OVER") << '\n';
        };

        std::vector<PakMountDiagnostics> pak_diagnostics;
        PakResourceProvider resources(
            options.shdata,
            {"800config.pak", "config.pak", "ui.pak", "map.pak"},
            &pak_diagnostics);
        startup_report->record(ResourceOutcome::success, pak_diagnostics.size());
        CacheResourceProvider cache_resources(options.cache);
        CachedImageProvider images(cache_resources);
        CursorAnimation cursor_animation;
        try {
            cursor_animation = load_cursor(resources);
            std::cout << "Cursor frames=" << cursor_animation.frames.size()
                      << " hotspot=" << cursor_animation.hotspot_x << ','
                      << cursor_animation.hotspot_y << '\n';
        } catch (const std::exception& error) {
            std::cerr << "Cursor load failed: " << error.what() << '\n';
        }
        log_startup("resources", resource_budget_ms);
        for (const PakMountDiagnostics& diagnostic : pak_diagnostics) {
            std::cout << "PAK " << diagnostic.pak << " open_ms=" << std::fixed
                      << std::setprecision(2) << diagnostic.open_ms
                      << " index_ms=" << diagnostic.index_ms
                      << " entries=" << diagnostic.entries << '\n';
        }
        PakManObjectProvider objects(resources);
        SceneLoadDiagnostics scene_diagnostics;
        MapScene scene = MapScene::load(
            resources, images, options.map, {&objects}, &scene_diagnostics);
        startup_report->record(ResourceOutcome::success);
        startup_report->record(ResourceOutcome::cache);
        startup_report->record(ResourceOutcome::success, scene.objects().size());
        startup_report->record(ResourceOutcome::skipped, scene.skipped_objects().size());
        const LocalSpawn spawn = find_local_spawn(scene);
        const bool background_opaque = scene.background_opaque();
        log_startup("scene", scene_budget_ms);
        std::cout << "Scene load map_ms=" << std::fixed << std::setprecision(2)
                  << scene_diagnostics.map_ms << " mlk_ms=" << scene_diagnostics.mlk_ms
                  << " background_ms=" << scene_diagnostics.background_ms
                  << " cel_ms=" << scene_diagnostics.cel_ms
                  << " man_ms=" << scene_diagnostics.man_ms
                  << " core_ms=" << scene_diagnostics.core_ms
                  << " cover_masks_ms=" << scene_diagnostics.cover_masks_ms
                  << " objects_ms=" << scene_diagnostics.objects_ms << '\n';
        std::vector<PakMountDiagnostics> player_diagnostics;
        PakResourceProvider player_resources(
            options.shdata, {"player.pak"}, &player_diagnostics);
        startup_report->record(ResourceOutcome::success, player_diagnostics.size());
        for (const PakMountDiagnostics& diagnostic : player_diagnostics) {
            std::cout << "PAK " << diagnostic.pak << " open_ms=" << std::fixed
                      << std::setprecision(2) << diagnostic.open_ms
                      << " index_ms=" << diagnostic.index_ms
                      << " entries=" << diagnostic.entries << '\n';
        }
        const AvatarCatalog catalog(read_file(options.shdata / "perinfo.pak"));
        scene.set_player(make_local_character(player_resources, catalog, spawn));
        startup_report->record(ResourceOutcome::success);
        log_startup("avatar", avatar_budget_ms);
        std::ostringstream npc_diagnostics;
        const bool npc_loaded = add_local_npc_if_available(
            scene, options.shdata, npc_diagnostics);
        startup_report->record(npc_loaded
                ? ResourceOutcome::success
                : ResourceOutcome::failure);
        if (!npc_loaded) {
            startup_report->record(ResourceOutcome::skipped);
        }
        std::cerr << npc_diagnostics.str();
        log_startup("npc", npc_budget_ms);
        std::cout << "Loaded " << scene.name() << ": background="
                  << scene.background().width << 'x' << scene.background().height
                  << ", MAN objects=" << scene.objects().size()
                  << ", skipped=" << scene.skipped_objects().size()
                  << ", player=" << spawn.position.x << ',' << spawn.position.y
                  << ", NPCs=" << scene.npcs().size() << '\n';

        const MapMusicSelection music = select_map_music(scene.map_ini());
        std::optional<AudioPlaybackWorker> audio_worker;
        bool audio_result_logged = false;
        try {
            audio_worker.emplace(music, options.shdata);
            startup_report->record(ResourceOutcome::success);
        } catch (const std::exception& error) {
            startup_report->record(ResourceOutcome::failure);
            std::cout << "Audio playing=false diagnostic=audio task unavailable: "
                      << error.what() << '\n';
        }
        log_startup("audio_dispatch", resource_budget_ms);

        MessageBus scp_messages;
        std::optional<PakResourceProvider> acceptance_resources;
        std::size_t logged_scp_api_diagnostics = 0;
        std::vector<std::unique_ptr<ScpWindow>> scp_windows;
        for (const std::string_view path : {std::string_view("ui/main/bar.scp"),
                 std::string_view("ui/main/fucnbtn_shortcut.scp")}) {
            try {
                ScpLoadDiagnostics load_profile;
                ScpDocument document = ScpParser::load(
                    resources, std::string(path), {}, &load_profile);
                ScpWindowProfile window_profile;
                auto scp_window = std::make_unique<ScpWindow>(std::move(document),
                    resources, scp_messages, ScpWindow::ImageDecoder{},
                    ScpWindowOptions{}, &window_profile);
                std::cout << "SCP " << path << " widgets="
                          << scp_window->widgets().size() << " diagnostics="
                          << scp_window->diagnostics().size() << '\n';
                std::cout << std::fixed << std::setprecision(2)
                          << "SCP profile file=" << path
                          << " files=" << load_profile.files
                          << " includes=" << load_profile.includes
                          << " resource_read_ms=" << load_profile.resource_read_ms
                          << " nz_decode_ms=" << load_profile.nz_decode_ms
                          << " parse_ms=" << load_profile.parse_ms
                          << " include_resolve_ms=" << load_profile.include_resolve_ms
                          << " layout_ms=" << window_profile.layout_ms
                          << " widget_setup_ms=" << window_profile.widget_setup_ms
                          << " image_read_ms=" << window_profile.resource_read_ms
                          << " image_nz_ms=" << window_profile.nz_decode_ms
                          << " wic_ms=" << window_profile.wic_decode_ms
                          << " proprietary_decode_ms=" << window_profile.proprietary_decode_ms
                          << " lua_ms=" << window_profile.lua_ms
                          << " images=" << window_profile.images << '\n';
                for (const std::string& diagnostic : scp_window->diagnostics()) {
                    std::cout << diagnostic << '\n';
                }
                scp_windows.push_back(std::move(scp_window));
                startup_report->record(ResourceOutcome::success);
            } catch (const std::exception& error) {
                startup_report->record(ResourceOutcome::failure);
                std::cerr << "SCP load failed file=" << path << ": "
                          << error.what() << '\n';
            }
        }
        constexpr std::string_view acceptance_bulletin_path = "ui/login/bulletin2.scp";
        std::size_t acceptance_lua_calls = 0;
        const auto load_acceptance_bulletin = [&]() {
            if (!acceptance_resources.has_value()) {
                throw std::logic_error("acceptance bulletin provider is unavailable");
            }
            ScpDocument document = ScpParser::load(
                *acceptance_resources, std::string(acceptance_bulletin_path));
            if (document.scripts.size() != 1 || document.windows.empty()) {
                throw ResourceError(
                    "acceptance bulletin did not load the full config.pak SCP source");
            }
            auto bulletin = std::make_unique<ScpWindow>(
                std::move(document), *acceptance_resources, scp_messages,
                ScpWindow::ImageDecoder{});
            if (bulletin->widgets().empty() || bulletin->lua_host() == nullptr
                || bulletin->lua_host()->registered_callback_count() != 3) {
                throw ResourceError(
                    "acceptance bulletin did not register its three original Lua callbacks");
            }
            return bulletin;
        };
        if (options.acceptance) {
            acceptance_resources.emplace(
                options.shdata, std::vector<std::string>{"config.pak"});
            scp_windows.push_back(load_acceptance_bulletin());
            startup_report->record(ResourceOutcome::success);
            log->info("SCP acceptance bulletin=shown registered_callbacks=3");
        }
        log_startup("scp_ui", resource_budget_ms);

        double camera_x = 0.0;
        double camera_y = 0.0;
        double zoom = 1.0;
        double animation_elapsed_ms = 0.0;
        InputState input;
        const NavGrid nav_grid(scene.cel());
        // M1 tuning choice only; this is not claimed to be the original speed.
        PathMovement movement(nav_grid, MovementConfig{120.0});
        MapInput map_input(scp_input_regions(scp_windows));
        double drag_anchor_x = 0.0;
        double drag_anchor_y = 0.0;
        bool camera_follow = false;
        bool dirty = true;
        std::optional<LogicalPoint> mouse_position;
        std::vector<ClickEffect> click_effects;
        std::unique_ptr<D3D11Presenter> presenter;

        const double map_width = static_cast<double>(
            std::max(scene.width(), scene.background().width));
        const double map_height = static_cast<double>(
            std::max(scene.height(), scene.background().height));
        const auto clamp_camera = [&]() {
            const double visible_width = logical_width() / zoom;
            const double visible_height = logical_height() / zoom;
            camera_x = std::clamp(camera_x, 0.0,
                std::max(0.0, map_width - visible_width));
            camera_y = std::clamp(camera_y, 0.0,
                std::max(0.0, map_height - visible_height));
        };
        camera_x = static_cast<double>(spawn.position.x) - logical_width() / 2.0;
        camera_y = static_cast<double>(spawn.position.y) - logical_height() / 2.0;
        clamp_camera();
        const auto map_under = [&](const LogicalPoint& point) {
            return std::pair{camera_x + point.x / zoom, camera_y + point.y / zoom};
        };

        Win32Window window(L"dhsh_client", logical_width(), logical_height(),
            [&](const WindowEvent& event) {
                if (options.acceptance && event.type == WindowEvent::Type::key_down
                    && event.key == VK_F8) {
                    const auto bulletin = std::find_if(scp_windows.begin(), scp_windows.end(),
                        [](const auto& candidate) {
                            return candidate != nullptr
                                && candidate->document().source_file == acceptance_bulletin_path;
                        });
                    if (bulletin != scp_windows.end()) {
                        scp_windows.erase(bulletin);
                        acceptance_lua_calls = 0;
                        log->info("SCP acceptance bulletin=hidden");
                    } else {
                        scp_windows.push_back(load_acceptance_bulletin());
                        log->info("SCP acceptance bulletin=shown registered_callbacks=3");
                    }
                    map_input.set_ui_regions(scp_input_regions(scp_windows));
                    dirty = true;
                    return;
                }
                input.handle_event(event);
                if (event.logical_position.has_value()) {
                    mouse_position = event.logical_position;
                    if (event.type == WindowEvent::Type::mouse_move
                        && !cursor_animation.frames.empty()) {
                        dirty = true;
                    }
                }
                const bool ui_consumed = handle_scp_event(scp_windows, event);
                if (ui_consumed) {
                    dirty = true;
                }
                if (options.acceptance) {
                    const auto bulletin = std::find_if(scp_windows.begin(), scp_windows.end(),
                        [](const auto& candidate) {
                            return candidate != nullptr
                                && candidate->document().source_file == acceptance_bulletin_path;
                        });
                    if (bulletin != scp_windows.end()
                        && (*bulletin)->lua_host()->callback_count() != acceptance_lua_calls) {
                        acceptance_lua_calls = (*bulletin)->lua_host()->callback_count();
                        const auto* policy = (*bulletin)->find_widget("Bulletin_win06");
                        const auto* panel = (*bulletin)->find_widget("Bulletin_win00");
                        const auto* license = (*bulletin)->find_widget("Bulletin_win03");
                        log->info(std::string("SCP Lua file=")
                            + std::string(acceptance_bulletin_path) + " calls="
                            + std::to_string(acceptance_lua_calls) + " policy_visible="
                            + (policy != nullptr && policy->visible() ? "true" : "false")
                            + " bulletin_visible="
                            + (panel != nullptr && panel->visible() ? "true" : "false")
                            + " license_visible="
                            + (license != nullptr && license->visible() ? "true" : "false"));
                    }
                }
                while (logged_scp_api_diagnostics < scp_messages.diagnostics().size()) {
                    std::cout << scp_messages.diagnostics()[logged_scp_api_diagnostics++]
                              << '\n';
                }
                map_input.set_ui_regions(scp_input_regions(scp_windows));
                const std::optional<MapPoint> click = map_input.handle_event(
                    event, MapInputView{camera_x, camera_y, zoom}, ui_consumed);
                if (!ui_consumed && click.has_value() && scene.player() != nullptr) {
                    const NavCell goal = nav_grid.cell_at_pixel({click->x, click->y});
                    const PathStatus status = movement.set_destination(
                        *scene.player(), goal);
                    camera_follow = status == PathStatus::success;
                    if (nav_grid.walkable(goal)) {
                        click_effects.push_back(ClickEffect{
                            static_cast<double>(click->x),
                            static_cast<double>(click->y),
                            0.0});
                    }
                    dirty = true;
                    std::cout << "Navigation click map=" << click->x << ',' << click->y
                              << " cell=" << goal.x << ',' << goal.y
                              << " status=" << path_status_name(status)
                              << " player=" << scene.player()->position().x << ','
                              << scene.player()->position().y << '\n';
                }
                if (event.type == WindowEvent::Type::resized) {
                    if (presenter) {
                        presenter->resize(event.width, event.height);
                    }
                    dirty = true;
                    return;
                }
                if (!event.logical_position.has_value()) {
                    return;
                }
                const LogicalPoint point = *event.logical_position;
                if (!ui_consumed && event.type == WindowEvent::Type::right_button_down
                    && map_input.camera_dragging()) {
                    camera_follow = false;
                    const auto [map_x, map_y] = map_under(point);
                    drag_anchor_x = map_x;
                    drag_anchor_y = map_y;
                } else if (event.type == WindowEvent::Type::mouse_move
                    && map_input.camera_dragging()) {
                    camera_follow = false;
                    camera_x = drag_anchor_x - point.x / zoom;
                    camera_y = drag_anchor_y - point.y / zoom;
                    clamp_camera();
                    dirty = true;
                } else if (!ui_consumed && event.type == WindowEvent::Type::mouse_wheel
                    && event.wheel_delta != 0) {
                    camera_follow = false;
                    const auto [map_x, map_y] = map_under(point);
                    const double steps =
                        static_cast<double>(event.wheel_delta) / WHEEL_DELTA;
                    zoom = std::clamp(zoom * std::pow(1.25, steps), 0.5, 3.0);
                    camera_x = map_x - point.x / zoom;
                    camera_y = map_y - point.y / zoom;
                    clamp_camera();
                    dirty = true;
                }
            });
        presenter = std::make_unique<D3D11Presenter>(
            window.handle(), logical_width(), logical_height());
        window.set_cursor_hidden(!cursor_animation.frames.empty());
        log_startup("window_presenter", presenter_budget_ms);
        std::cout << "Startup total ms=" << std::fixed << std::setprecision(2)
                  << diagnostic_ms(startup_start) << '\n';
        log->info(startup_report->format());

        SoftwareRenderer renderer(logical_width(), logical_height());
        const Image black = opaque_black(logical_width(), logical_height());
        Image click_star_image;
        click_star_image.width = kClickStarWidth;
        click_star_image.height = kClickStarHeight;
        click_star_image.rgba.resize(kClickStarRgba.size());
        for (std::size_t index = 0; index < kClickStarRgba.size(); ++index) {
            click_star_image.rgba[index] =
                static_cast<std::byte>(kClickStarRgba[index]);
        }
        const std::wstring wide_map(options.map.begin(), options.map.end());
        LARGE_INTEGER frequency{};
        LARGE_INTEGER previous{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
            throw std::runtime_error("QueryPerformanceFrequency returned an invalid frequency");
        }
        if (!QueryPerformanceCounter(&previous)) {
            throw std::runtime_error("QueryPerformanceCounter failed to initialize the frame clock");
        }
        StageStats advance_stats;
        StageStats clear_stats;
        StageStats background_stats;
        StageStats setup_stats;
        StageStats man_stats;
        StageStats entity_mask_stats;
        StageStats entity_draw_stats;
        StageStats cover_test_stats;
        StageStats covering_mask_stats;
        StageStats sprite_copy_stats;
        StageStats render_stats;
        StageStats present_stats;
        StageStats frame_to_present_stats;
        SceneRenderDiagnostics render_diagnostics;
        bool client_suspended = false;

        while (true) {
            if (audio_worker.has_value() && !audio_result_logged) {
                AudioPlaybackResult result;
                if (audio_worker->try_result(result)) {
                    std::cout << "Audio playing=" << (result.playing ? "true" : "false");
                    if (!result.diagnostic.empty()) {
                        std::cout << " diagnostic=" << result.diagnostic;
                    }
                    std::cout << '\n';
                    audio_result_logged = true;
                }
            }
            const auto frame_to_present_start = DiagnosticClock::now();
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    if (audio_worker.has_value()) {
                        audio_worker->request_stop();
                    }
                    window.rethrow_event_error();
                    return static_cast<int>(message.wParam);
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            if (window.client_width() == 0 || window.client_height() == 0) {
                client_suspended = true;
                Sleep(16);
                continue;
            }
            LARGE_INTEGER current{};
            if (!QueryPerformanceCounter(&current)) {
                throw std::runtime_error("QueryPerformanceCounter failed while sampling the frame clock");
            }
            if (client_suspended) {
                previous = current;
                animation_elapsed_ms = 0.0;
                client_suspended = false;
            }
            const double elapsed_ms = frame_elapsed_ms(
                previous.QuadPart, current.QuadPart, frequency.QuadPart);
            previous = current;
            const auto advance_start = DiagnosticClock::now();
            animation_elapsed_ms += elapsed_ms;
            if (animation_elapsed_ms >= 1.0) {
                const auto whole_ms = static_cast<std::uint32_t>(std::min(
                    animation_elapsed_ms,
                    static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
                animation_elapsed_ms -= whole_ms;
                if (scene.advance(whole_ms)) {
                    dirty = true;
                }
            }
            if (!cursor_animation.frames.empty()) {
                cursor_animation.elapsed_ms += elapsed_ms;
                CursorFrame& frame = cursor_animation.frames[cursor_animation.frame];
                while (cursor_animation.elapsed_ms
                    >= std::max<std::uint32_t>(1, frame.delay_ms)) {
                    cursor_animation.elapsed_ms -= std::max<std::uint32_t>(
                        1, frame.delay_ms);
                    cursor_animation.frame = (cursor_animation.frame + 1)
                        % cursor_animation.frames.size();
                    frame = cursor_animation.frames[cursor_animation.frame];
                    dirty = true;
                }
            }
            if (!click_effects.empty()) {
                for (ClickEffect& effect : click_effects) {
                    effect.age_ms += elapsed_ms;
                }
                click_effects.erase(std::remove_if(click_effects.begin(),
                    click_effects.end(), [](const ClickEffect& effect) {
                        return effect.age_ms >= click_effect_lifetime_ms;
                    }), click_effects.end());
                dirty = true;
            }
            if (scene.player() != nullptr) {
                if (movement.advance(*scene.player(), elapsed_ms)) {
                    dirty = true;
                }
                if (camera_follow) {
                    const double old_camera_x = camera_x;
                    const double old_camera_y = camera_y;
                    const double target_x = std::clamp(
                        scene.player()->position().x - logical_width() / (2.0 * zoom),
                        0.0, std::max(0.0, map_width - logical_width() / zoom));
                    const double target_y = std::clamp(
                        scene.player()->position().y - logical_height() / (2.0 * zoom),
                        0.0, std::max(0.0, map_height - logical_height() / zoom));
                    camera_x = approach_camera_coordinate(
                        camera_x, target_x, elapsed_ms);
                    camera_y = approach_camera_coordinate(
                        camera_y, target_y, elapsed_ms);
                    dirty = dirty || old_camera_x != camera_x || old_camera_y != camera_y;
                }
            }
            advance_stats.add(diagnostic_ms(advance_start));

            const double horizontal = input.horizontal();
            const double vertical = input.vertical();
            if (horizontal != 0.0 || vertical != 0.0) {
                camera_follow = false;
                camera_x += horizontal * 500.0 * elapsed_ms / 1000.0;
                camera_y += vertical * 500.0 * elapsed_ms / 1000.0;
                clamp_camera();
                dirty = true;
            }

            const auto render_start = DiagnosticClock::now();
            if (dirty) {
                const auto render_x = static_cast<std::int32_t>(std::floor(camera_x));
                const auto render_y = static_cast<std::int32_t>(std::floor(camera_y));
                const bool background_covers_viewport = zoom == 1.0 && background_opaque
                    && render_x >= 0 && render_y >= 0
                    && static_cast<std::int64_t>(render_x) + logical_width()
                        <= scene.background().width
                    && static_cast<std::int64_t>(render_y) + logical_height()
                        <= scene.background().height;
                const auto clear_start = DiagnosticClock::now();
                if (!background_covers_viewport) {
                    renderer.draw(black, 0, 0);
                }
                clear_stats.add(diagnostic_ms(clear_start));
                if (zoom == 1.0) {
                    scene.render(renderer, render_x, render_y, &render_diagnostics);
                } else {
                    const auto source_width = std::max(1U, static_cast<std::uint32_t>(
                        logical_width() / zoom));
                    const auto source_height = std::max(1U, static_cast<std::uint32_t>(
                        logical_height() / zoom));
                    SoftwareRenderer source(source_width, source_height);
                    scene.render(source, render_x, render_y, &render_diagnostics);
                    const Image scaled = scale_nearest(
                        source.target(), logical_width(), logical_height());
                    renderer.draw(scaled, 0, 0);
                }
                for (const auto& scp_window : scp_windows) {
                    if (scp_window != nullptr) {
                        scp_window->render(renderer);
                    }
                }
                for (const ClickEffect& effect : click_effects) {
                    const double t = std::clamp(
                        effect.age_ms / click_effect_lifetime_ms, 0.0, 1.0);
                    const double expansion = 1.0 - (1.0 - t) * (1.0 - t);
                    const double center_x = (effect.map_x - camera_x) * zoom;
                    const double center_y = (effect.map_y - camera_y) * zoom;
                    const double radius_x = (10.0 + 20.0 * expansion) * zoom;
                    const double radius_y = (6.0 + 10.0 * expansion) * zoom;
                    const auto alpha = static_cast<std::uint8_t>(std::clamp(
                        230.0 * (1.0 - t), 0.0, 255.0));
                    renderer.draw_ellipse_ring(
                        center_x, center_y, radius_x, radius_y, alpha);
                    const double star_t = std::clamp(
                        effect.age_ms / 250.0, 0.0, 1.0);
                    const auto star_alpha = static_cast<unsigned>(std::clamp(
                        255.0 * (1.0 - star_t), 0.0, 255.0));
                    if (star_alpha != 0) {
                        Image star = click_star_image;
                        if (zoom != 1.0) {
                            star = scale_nearest(click_star_image,
                                std::max(1U, static_cast<std::uint32_t>(
                                    std::lround(kClickStarWidth * zoom))),
                                std::max(1U, static_cast<std::uint32_t>(
                                    std::lround(kClickStarHeight * zoom))));
                        }
                        for (std::size_t index = 3; index < star.rgba.size();
                             index += 4) {
                            const unsigned source_alpha =
                                std::to_integer<unsigned>(star.rgba[index]);
                            star.rgba[index] = static_cast<std::byte>(
                                (source_alpha * star_alpha) / 255U);
                        }
                        renderer.draw(star,
                            static_cast<std::int32_t>(std::lround(
                                center_x - static_cast<double>(star.width) / 2.0)),
                            static_cast<std::int32_t>(std::lround(
                                center_y - static_cast<double>(star.height) / 2.0)));
                    }
                }
                if (!cursor_animation.frames.empty() && mouse_position.has_value()) {
                    const CursorFrame& frame =
                        cursor_animation.frames[cursor_animation.frame];
                    renderer.draw(frame.image,
                        mouse_position->x - cursor_animation.hotspot_x,
                        mouse_position->y - cursor_animation.hotspot_y);
                }
                background_stats.add(render_diagnostics.background_ms);
                setup_stats.add(render_diagnostics.setup_ms);
                man_stats.add(render_diagnostics.man_ms);
                entity_mask_stats.add(render_diagnostics.entity_mask_ms);
                entity_draw_stats.add(render_diagnostics.entity_draw_ms);
                cover_test_stats.add(static_cast<double>(render_diagnostics.cover_tests));
                covering_mask_stats.add(
                    static_cast<double>(render_diagnostics.covering_masks));
                sprite_copy_stats.add(
                    static_cast<double>(render_diagnostics.sprite_copies));
                render_stats.add(diagnostic_ms(render_start));
                if (!options.capture.empty()) {
                    write_ppm(options.capture, renderer.target());
                    std::cout << "Capture " << options.capture << '\n';
                    options.capture.clear();
                }
                dirty = false;
            }
            const auto present_start = DiagnosticClock::now();
            presenter->present(renderer.target(), window.viewport());
            present_stats.add(diagnostic_ms(present_start));
            frame_to_present_stats.add(diagnostic_ms(frame_to_present_start));

            if (frame_to_present_stats.samples == 120) {
                std::cout << std::fixed << std::setprecision(2)
                          << "Frame stats samples=120 advance_avg_ms="
                          << advance_stats.average() << " render_samples="
                          << render_stats.samples << " render_avg_ms="
                          << render_stats.average() << " render_max_ms="
                          << render_stats.max_ms << " render_budget_ms="
                          << render_budget_ms
                          << (render_stats.samples > 0
                                      && render_stats.average() <= render_budget_ms
                                  ? " PASS" : " OVER")
                          << " present_avg_ms=" << present_stats.average()
                          << " frame_to_present_avg_ms=" << frame_to_present_stats.average()
                          << " frame_to_present_max_ms=" << frame_to_present_stats.max_ms << '\n'
                          << "Scene stats clear_avg_ms=" << clear_stats.average()
                          << " background_avg_ms=" << background_stats.average()
                          << " setup_avg_ms=" << setup_stats.average()
                          << " man_avg_ms=" << man_stats.average()
                          << " entity_mask_avg_ms=" << entity_mask_stats.average()
                          << " entity_draw_avg_ms=" << entity_draw_stats.average()
                          << " cover_tests_avg=" << cover_test_stats.average()
                          << " covering_masks_avg=" << covering_mask_stats.average()
                          << " sprite_copies_avg=" << sprite_copy_stats.average()
                          << '\n';
                advance_stats.reset();
                clear_stats.reset();
                background_stats.reset();
                setup_stats.reset();
                man_stats.reset();
                entity_mask_stats.reset();
                entity_draw_stats.reset();
                cover_test_stats.reset();
                covering_mask_stats.reset();
                sprite_copy_stats.reset();
                render_stats.reset();
                present_stats.reset();
                frame_to_present_stats.reset();
            }

            const std::string_view nav_name = path_status_name(movement.status());
            const std::wstring wide_nav(nav_name.begin(), nav_name.end());
            std::wostringstream status;
            status << L"dhsh_client - " << wide_map << L" | camera="
                   << static_cast<std::int32_t>(camera_x) << L','
                   << static_cast<std::int32_t>(camera_y) << L" | zoom="
                   << std::fixed << std::setprecision(2) << zoom;
            if (scene.player() != nullptr) {
                status << L" | player=" << scene.player()->position().x << L','
                       << scene.player()->position().y << L" state="
                       << (scene.player()->state() == EntityState::walk ? L"Walk" : L"Idle")
                       << L" nav=" << wide_nav << L" goal=" << movement.goal().x
                       << L',' << movement.goal().y;
            }
            status << L" | frame=" << std::setprecision(2) << elapsed_ms << L" ms";
            window.set_title(status.str());
        }
    } catch (const std::exception& error) {
        if (startup_report.has_value()) {
            startup_report->set_fatal_error(error.what());
            bool report_logged = false;
            if (log.has_value()) {
                try {
                    log->error(startup_report->format());
                    report_logged = true;
                } catch (const std::exception& log_error) {
                    std::cerr << "dhsh_client: " << log_error.what() << '\n';
                }
            }
            if (!report_logged) {
                std::cerr << startup_report->format() << '\n';
            }
        }
        std::cerr << "dhsh_client: " << redact_sensitive(error.what()) << '\n';
        print_usage(std::cerr);
        return 1;
    }
}

} // namespace dhsh
