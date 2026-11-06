#include "app/App.h"

#include "formats/A16.h"
#include "platform/D3D11Presenter.h"
#include "platform/Win32Window.h"
#include "render/SoftwareRenderer.h"
#include "resource/CachedImageProvider.h"
#include "resource/NzDecoder.h"
#include "resource/PakResourceProvider.h"
#include "resource/ResourceError.h"
#include "scene/MapScene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dhsh {
namespace {

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

std::vector<std::string> asset_variants(std::string_view path) {
    const std::string normalized =
        PakResourceProvider::normalize_logical_path(path);
    std::vector<std::string> result;
    if (normalized.empty()) {
        return result;
    }
    result.push_back(normalized);
    if (normalized.rfind("map/", 0) == 0) {
        const std::string rest = normalized.substr(4);
        result.push_back("map1/" + rest);
        result.push_back(rest);
    }
    return result;
}

std::pair<std::int32_t, std::int32_t> a16_offsets(std::span<const std::byte> data) {
    if (data.size() < 8) {
        throw ResourceError("A16 frame header is truncated");
    }
    const auto read_i16 = [&](std::size_t offset) {
        const auto value = static_cast<std::uint16_t>(
            std::to_integer<unsigned>(data[offset])
            | (std::to_integer<unsigned>(data[offset + 1]) << 8));
        return static_cast<std::int16_t>(value);
    };
    return {read_i16(4), read_i16(6)};
}

class PakManObjectProvider final : public ManObjectProvider {
public:
    explicit PakManObjectProvider(ResourceProvider& resources)
        : resources_(resources) {
    }

    std::optional<MapObjectFrame> load_first_frame(const ManEntry& entry) override {
        const std::string key =
            PakResourceProvider::normalize_logical_path(entry.path);
        const auto found = cache_.find(key);
        if (found != cache_.end()) {
            return found->second;
        }
        std::optional<MapObjectFrame> frame = load(entry.path);
        cache_.emplace(key, frame);
        return frame;
    }

private:
    std::optional<std::string> first_a16(std::string_view directory) {
        for (std::int32_t index = 0; index <= 999; ++index) {
            const std::string stem = std::to_string(index);
            for (std::size_t width = stem.size(); width <= 5; ++width) {
                std::string name(width - stem.size(), '0');
                name += stem;
                name += ".a16";
                std::string candidate(directory);
                candidate.push_back('/');
                candidate += name;
                if (resources_.exists(candidate)) {
                    return candidate;
                }
            }
        }
        return std::nullopt;
    }

    std::optional<MapObjectFrame> load(std::string_view man_path) {
        for (const std::string& directory : asset_variants(man_path)) {
            const std::optional<std::string> asset = first_a16(directory);
            if (!asset.has_value()) {
                continue;
            }
            std::vector<std::byte> decoded;
            try {
                decoded = NzDecoder::decode(resources_.bytes(*asset));
            } catch (const std::exception& error) {
                throw ResourceError("cannot decode MAN frame '" + *asset
                    + "': " + error.what());
            }
            const auto [offset_x, offset_y] = a16_offsets(decoded);
            MapObjectFrame frame;
            try {
                frame.image = A16Decoder::decode(decoded);
            } catch (const std::exception& error) {
                throw ResourceError("cannot decode A16 frame '" + *asset
                    + "': " + error.what());
            }
            frame.offset_x = offset_x;
            frame.offset_y = offset_y;
            return frame;
        }
        return std::nullopt;
    }

    ResourceProvider& resources_;
    std::unordered_map<std::string, std::optional<MapObjectFrame>> cache_;
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
              "  --map defaults to YingTianFu\n"
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

        PakResourceProvider resources(options.shdata, {"config.pak", "map.pak"});
        CacheResourceProvider cache_resources(options.cache);
        CachedImageProvider images(cache_resources);
        PakManObjectProvider objects(resources);
        const MapScene scene = MapScene::load(
            resources, images, options.map, {&objects});
        std::cout << "Loaded " << scene.name() << ": background="
                  << scene.background().width << 'x' << scene.background().height
                  << ", MAN objects=" << scene.objects().size()
                  << ", skipped=" << scene.skipped_objects().size() << '\n';

        double camera_x = 0.0;
        double camera_y = 0.0;
        double zoom = 1.0;
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

        while (true) {
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

            const double horizontal = input.horizontal();
            const double vertical = input.vertical();
            if (horizontal != 0.0 || vertical != 0.0) {
                camera_x += horizontal * 500.0 * elapsed_ms / 1000.0;
                camera_y += vertical * 500.0 * elapsed_ms / 1000.0;
                clamp_camera();
                dirty = true;
            }

            if (dirty) {
                renderer.draw(black, 0, 0);
                const auto render_x = static_cast<std::int32_t>(std::floor(camera_x));
                const auto render_y = static_cast<std::int32_t>(std::floor(camera_y));
                if (zoom == 1.0) {
                    scene.render(renderer, render_x, render_y);
                } else {
                    const auto source_width = std::max(1U, static_cast<std::uint32_t>(
                        logical_width() / zoom));
                    const auto source_height = std::max(1U, static_cast<std::uint32_t>(
                        logical_height() / zoom));
                    SoftwareRenderer source(source_width, source_height);
                    scene.render(source, render_x, render_y);
                    const Image scaled = scale_nearest(
                        source.target(), logical_width(), logical_height());
                    renderer.draw(scaled, 0, 0);
                }
                dirty = false;
            }
            presenter->present(renderer.target(), window.viewport());

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
