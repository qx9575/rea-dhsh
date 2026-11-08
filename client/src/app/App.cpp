#include "app/App.h"

#include "avatar/AvatarCatalog.h"
#include "platform/D3D11Presenter.h"
#include "platform/Win32Window.h"
#include "render/SoftwareRenderer.h"
#include "resource/CachedImageProvider.h"
#include "resource/PakResourceProvider.h"
#include "resource/ResourceError.h"
#include "scene/LocalEntityFactory.h"
#include "scene/MapScene.h"
#include "scene/PakManObjectProvider.h"

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
    std::string map = "YingTianFu";
    bool help = false;
};

void print_usage(std::ostream& output) {
    output << "usage: dhsh_client --shdata <directory> [--map <name>]"
              " [--cache <directory>]\n"
              "  --map defaults to YingTianFu; the Task 12 local spawn preset"
              " currently supports only YingTianFu\n"
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
        if (argument != "--shdata" && argument != "--map" && argument != "--cache") {
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
        } else {
            options.cache = value;
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

void InputState::handle_event(const WindowEvent& event) {
    if (event.type == WindowEvent::Type::input_cancelled) {
        dragging_ = false;
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
        return;
    }
    if (event.type == WindowEvent::Type::left_button_up) {
        dragging_ = false;
    } else if (event.type == WindowEvent::Type::left_button_down
        && event.logical_position.has_value()) {
        dragging_ = true;
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
    try {
        const Options options = parse_options(argc, argv);
        if (options.help) {
            print_usage(std::cout);
            return 0;
        }

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
            options.shdata, {"config.pak", "map.pak", "player.pak"},
            &pak_diagnostics);
        CacheResourceProvider cache_resources(options.cache);
        CachedImageProvider images(cache_resources);
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
        const AvatarCatalog catalog(read_file(options.shdata / "perinfo.pak"));
        scene.set_player(make_local_character(resources, catalog, spawn));
        log_startup("avatar", avatar_budget_ms);
        add_local_npc_if_available(scene, options.shdata, std::cerr);
        log_startup("npc", npc_budget_ms);
        std::cout << "Loaded " << scene.name() << ": background="
                  << scene.background().width << 'x' << scene.background().height
                  << ", MAN objects=" << scene.objects().size()
                  << ", skipped=" << scene.skipped_objects().size()
                  << ", player=" << spawn.position.x << ',' << spawn.position.y
                  << ", NPCs=" << scene.npcs().size() << '\n';

        double camera_x = 0.0;
        double camera_y = 0.0;
        double zoom = 1.0;
        double animation_elapsed_ms = 0.0;
        InputState input;
        double drag_anchor_x = 0.0;
        double drag_anchor_y = 0.0;
        bool dirty = true;
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
                input.handle_event(event);
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
                if (event.type == WindowEvent::Type::left_button_down) {
                    const auto [map_x, map_y] = map_under(point);
                    drag_anchor_x = map_x;
                    drag_anchor_y = map_y;
                } else if (event.type == WindowEvent::Type::mouse_move
                    && input.dragging()) {
                    camera_x = drag_anchor_x - point.x / zoom;
                    camera_y = drag_anchor_y - point.y / zoom;
                    clamp_camera();
                    dirty = true;
                } else if (event.type == WindowEvent::Type::mouse_wheel
                    && event.wheel_delta != 0) {
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
        log_startup("window_presenter", presenter_budget_ms);
        std::cout << "Startup total ms=" << std::fixed << std::setprecision(2)
                  << diagnostic_ms(startup_start) << '\n';

        SoftwareRenderer renderer(logical_width(), logical_height());
        const Image black = opaque_black(logical_width(), logical_height());
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
        StageStats frame_stats;
        SceneRenderDiagnostics render_diagnostics;

        while (true) {
            const auto frame_cpu_start = DiagnosticClock::now();
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    return static_cast<int>(message.wParam);
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            LARGE_INTEGER current{};
            if (!QueryPerformanceCounter(&current)) {
                throw std::runtime_error("QueryPerformanceCounter failed while sampling the frame clock");
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
            advance_stats.add(diagnostic_ms(advance_start));

            const double horizontal = input.horizontal();
            const double vertical = input.vertical();
            if (horizontal != 0.0 || vertical != 0.0) {
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
                dirty = false;
            }
            const auto present_start = DiagnosticClock::now();
            presenter->present(renderer.target(), window.viewport());
            present_stats.add(diagnostic_ms(present_start));
            frame_stats.add(diagnostic_ms(frame_cpu_start));

            if (frame_stats.samples == 120) {
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
                          << " cpu_avg_ms=" << frame_stats.average()
                          << " cpu_max_ms=" << frame_stats.max_ms << '\n'
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
                frame_stats.reset();
            }

            std::wostringstream status;
            status << L"dhsh_client - " << wide_map << L" | camera="
                   << static_cast<std::int32_t>(camera_x) << L','
                   << static_cast<std::int32_t>(camera_y) << L" | zoom="
                   << std::fixed << std::setprecision(2) << zoom
                   << L" | frame=" << std::setprecision(2) << elapsed_ms << L" ms";
            window.set_title(status.str());
        }
    } catch (const std::exception& error) {
        std::cerr << "dhsh_client: " << error.what() << '\n';
        print_usage(std::cerr);
        return 1;
    }
}

} // namespace dhsh
