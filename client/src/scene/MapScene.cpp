#include "scene/MapScene.h"

#include "render/SoftwareRenderer.h"
#include "resource/NzDecoder.h"
#include "resource/ResourceError.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace dhsh {
namespace {

char ascii_lower(char value) {
    const auto byte = static_cast<unsigned char>(value);
    if (byte >= 'A' && byte <= 'Z') {
        return static_cast<char>(byte - 'A' + 'a');
    }
    return value;
}

// Logical paths in the archives are case-insensitive and use either
// separator. Leading, trailing, and repeated separators are removed.
std::string normalize_path(std::string_view path) {
    std::string normalized;
    for (const char value : path) {
        if (value == '/' || value == '\\') {
            if (!normalized.empty() && normalized.back() != '/') {
                normalized.push_back('/');
            }
            continue;
        }
        normalized.push_back(ascii_lower(value));
    }
    while (!normalized.empty() && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
}

// Literal path first, then the map1 sibling and the stripped form, matching
// tools/map_preview.py and tools/kimi_viewer/source.py. Duplicates are removed.
std::vector<std::string> path_variants(std::string_view path) {
    const std::string normalized = normalize_path(path);
    std::vector<std::string> variants;
    if (normalized.empty()) {
        return variants;
    }
    variants.push_back(normalized);
    const auto add = [&](const std::string& candidate) {
        if (std::find(variants.begin(), variants.end(), candidate) == variants.end()) {
            variants.push_back(candidate);
        }
    };
    if (normalized.rfind("map/", 0) == 0) {
        const std::string rest = normalized.substr(4);
        add("map1/" + rest);
        add(rest);
    } else if (normalized.rfind("map1/", 0) == 0) {
        const std::string rest = normalized.substr(5);
        add("map/" + rest);
        add(rest);
    }
    return variants;
}

std::optional<std::string> find_existing_path(
    ResourceProvider& resources,
    std::string_view logical_path) {
    for (const std::string& candidate : path_variants(logical_path)) {
        if (resources.exists(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::vector<std::byte> read_nz_resource(
    ResourceProvider& resources,
    const std::string& logical_path) {
    std::vector<std::byte> stored;
    try {
        stored = resources.bytes(logical_path);
    } catch (const std::exception& error) {
        throw ResourceError(
            "cannot read resource '" + logical_path + "': " + error.what());
    }
    try {
        return NzDecoder::decode(stored);
    } catch (const std::exception& error) {
        throw ResourceError(
            "cannot decode NZ resource '" + logical_path + "': " + error.what());
    }
}

std::string replace_extension(std::string_view path, std::string_view extension) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos
        || (slash != std::string_view::npos && dot < slash)) {
        return std::string(path) + std::string(extension);
    }
    return std::string(path.substr(0, dot)) + std::string(extension);
}

std::string ini_value(
    const MapIni& ini,
    std::string_view section,
    std::string_view key) {
    const auto section_it = ini.find(std::string(section));
    if (section_it == ini.end()) {
        return {};
    }
    const auto value_it = section_it->second.find(std::string(key));
    if (value_it == section_it->second.end()) {
        return {};
    }
    return value_it->second;
}

std::uint32_t parse_dimension(
    const std::string& text,
    std::string_view field,
    const std::string& map_path) {
    if (text.empty()) {
        throw ResourceError(
            "MAP file '" + map_path + "' has no " + std::string(field));
    }
    std::uint32_t value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0) {
        throw ResourceError(
            "MAP file '" + map_path + "' has an invalid " + std::string(field)
            + ": '" + text + "'");
    }
    return value;
}

std::uint32_t cell_count(std::uint32_t pixels, std::string_view field) {
    const std::uint64_t cells =
        (static_cast<std::uint64_t>(pixels) + 15U) / 16U;
    if (cells > std::numeric_limits<std::uint32_t>::max()) {
        throw ResourceError("MAP " + std::string(field) + " is too large");
    }
    return static_cast<std::uint32_t>(cells);
}

std::int32_t clamp_i32(std::int64_t value) {
    if (value < std::numeric_limits<std::int32_t>::min()) {
        return std::numeric_limits<std::int32_t>::min();
    }
    if (value > std::numeric_limits<std::int32_t>::max()) {
        return std::numeric_limits<std::int32_t>::max();
    }
    return static_cast<std::int32_t>(value);
}

std::int32_t clamp_subtract(std::int32_t left, std::int32_t right) {
    return clamp_i32(
        static_cast<std::int64_t>(left) - static_cast<std::int64_t>(right));
}

std::int32_t clamp_negate(std::int32_t value) {
    return clamp_i32(-static_cast<std::int64_t>(value));
}

} // namespace

MapScene MapScene::load(
    ResourceProvider& resources,
    CachedImageProvider& images,
    std::string_view name,
    std::initializer_list<ManObjectProvider*> object_providers) {
    MapScene scene;
    scene.name_ = std::string(name);
    if (scene.name_.empty()) {
        throw ResourceError("map name is empty");
    }

    const std::vector<std::string> map_candidates = {
        "map/" + scene.name_ + ".map",
        "config/map/" + scene.name_ + ".map",
        scene.name_ + ".map",
    };
    std::optional<std::string> map_path;
    for (const std::string& candidate : map_candidates) {
        if (resources.exists(candidate)) {
            map_path = candidate;
            break;
        }
    }
    if (!map_path.has_value()) {
        throw ResourceError(
            "required MAP file not found: " + map_candidates.front()
            + " (also tried config/map and the bare map name)");
    }

    MapIni ini;
    try {
        ini = parse_map_ini(read_nz_resource(resources, *map_path));
    } catch (const ResourceError&) {
        throw;
    } catch (const std::exception& error) {
        throw ResourceError(
            "cannot parse MAP file '" + *map_path + "': " + error.what());
    }

    scene.width_ = parse_dimension(
        ini_value(ini, "base", "WIDTH"), "WIDTH", *map_path);
    scene.height_ = parse_dimension(
        ini_value(ini, "base", "HEIGHT"), "HEIGHT", *map_path);

    const std::string background_path = ini_value(ini, "base", "BACKGROUND");
    if (background_path.empty()) {
        throw ResourceError(
            "MAP file '" + *map_path + "' has no BACKGROUND path");
    }
    std::string background_error;
    for (const std::string& candidate : path_variants(background_path)) {
        try {
            scene.background_ = &images.get(candidate);
            break;
        } catch (const std::exception& error) {
            if (background_error.empty()) {
                background_error = error.what();
            }
        }
    }
    if (scene.background_ == nullptr) {
        throw ResourceError(
            "required map background not found: " + background_path
            + (background_error.empty() ? "" : " (" + background_error + ")"));
    }

    const std::string cel_path = ini_value(ini, "base", "CELLFILE");
    if (cel_path.empty()) {
        throw ResourceError(
            "MAP file '" + *map_path + "' has no CELLFILE path");
    }
    const std::optional<std::string> resolved_cel =
        find_existing_path(resources, cel_path);
    if (!resolved_cel.has_value()) {
        throw ResourceError("required ground CEL not found: " + cel_path);
    }
    try {
        scene.cel_ = Cel::parse(read_nz_resource(resources, *resolved_cel))
                         .for_map(cell_count(scene.width_, "WIDTH"),
                             cell_count(scene.height_, "HEIGHT"));
    } catch (const ResourceError&) {
        throw;
    } catch (const std::exception& error) {
        throw ResourceError(
            "cannot load ground CEL '" + *resolved_cel + "': " + error.what());
    }

    const std::string man_path = ini_value(ini, "ext", "ANIFILE");
    if (!man_path.empty()) {
        const std::optional<std::string> resolved_man =
            find_existing_path(resources, man_path);
        if (resolved_man.has_value()) {
            try {
                scene.man_ = Man::parse(read_nz_resource(resources, *resolved_man));
            } catch (const ResourceError&) {
                throw;
            } catch (const std::exception& error) {
                throw ResourceError(
                    "cannot load MAN file '" + *resolved_man + "': "
                    + error.what());
            }
        }
    }

    const std::string mask_path = ini_value(ini, "base", "MASKFILE");
    if (!mask_path.empty()) {
        // The map loader rewrites MASKFILE's extension to .core; the legacy
        // .MSK files are not loaded by the current client.
        const std::string core_path = replace_extension(mask_path, ".core");
        std::optional<std::string> resolved_core =
            find_existing_path(resources, core_path);
        if (!resolved_core.has_value()
            && normalize_path(core_path) != normalize_path(mask_path)) {
            resolved_core = find_existing_path(resources, mask_path);
        }
        if (resolved_core.has_value()) {
            try {
                scene.core_ = Core::parse(
                    read_nz_resource(resources, *resolved_core));
            } catch (const ResourceError&) {
                throw;
            } catch (const std::exception& error) {
                throw ResourceError(
                    "cannot load core mask '" + *resolved_core + "': "
                    + error.what());
            }
        }
    }

    if (scene.core_.has_value()) {
        scene.cover_masks_.reserve(scene.core_->placements.size());
        for (std::size_t index = 0; index < scene.core_->placements.size(); ++index) {
            scene.cover_masks_.emplace_back(*scene.core_, index);
        }
    }

    for (const ManEntry& entry : scene.man_.entries) {
        bool loaded = false;
        std::string reason;
        if (object_providers.size() == 0) {
            reason = "no MAN object provider is available";
        }
        for (ManObjectProvider* provider : object_providers) {
            if (provider == nullptr) {
                continue;
            }
            try {
                SharedSceneAnimation animation = provider->load_animation(entry);
                if (animation != nullptr && !animation->empty()) {
                    scene.objects_.emplace_back(entry, std::move(animation));
                    loaded = true;
                    break;
                }
                std::optional<MapObjectFrame> frame =
                    provider->load_first_frame(entry);
                if (frame.has_value()) {
                    scene.objects_.emplace_back(entry, std::move(*frame));
                    loaded = true;
                    break;
                }
                if (reason.empty()) {
                    reason = "no provider returned a MAN frame";
                }
            } catch (const std::exception& error) {
                reason = "MAN object provider failed: " + std::string(error.what());
            }
        }
        if (!loaded) {
            if (reason.empty()) {
                reason = "no provider returned a MAN frame";
            }
            scene.skipped_objects_.push_back(
                SkippedSceneObject{entry.path, std::move(reason)});
        }
    }
    SceneObject::sort_by_depth(scene.objects_);

    return scene;
}

bool MapScene::advance(std::uint32_t milliseconds) {
    bool changed = false;
    for (SceneObject& object : objects_) {
        changed = object.advance(milliseconds) || changed;
    }
    return changed;
}

void MapScene::render(
    SoftwareRenderer& renderer,
    std::int32_t camera_x,
    std::int32_t camera_y) const {
    if (background_ != nullptr) {
        renderer.draw(*background_, clamp_negate(camera_x), clamp_negate(camera_y));
    }
    for (const SceneObject& object : objects_) {
        renderer.draw(object.frame.image,
            clamp_subtract(object.x, camera_x),
            clamp_subtract(object.y, camera_y));
    }
}

} // namespace dhsh
