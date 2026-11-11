#include "scene/PakManObjectProvider.h"

#include "formats/A16.h"
#include "formats/Pif.h"
#include "resource/NzDecoder.h"
#include "resource/PakResourceProvider.h"
#include "resource/ResourceError.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dhsh {
namespace {

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

} // namespace

PakManObjectProvider::PakManObjectProvider(ResourceProvider& resources)
    : resources_(resources) {
}

std::optional<MapObjectFrame> PakManObjectProvider::load_first_frame(
    const ManEntry& entry) {
    const SharedSceneAnimation animation = load_animation(entry);
    if (animation == nullptr || animation->empty()) {
        return std::nullopt;
    }
    return animation->front();
}

SharedSceneAnimation PakManObjectProvider::load_animation(const ManEntry& entry) {
    const std::string key =
        PakResourceProvider::normalize_logical_path(entry.path);
    const auto found = cache_.find(key);
    if (found != cache_.end()) {
        return found->second;
    }
    SharedSceneAnimation animation = load(entry.path);
    cache_.emplace(key, animation);
    return animation;
}

std::optional<std::string> PakManObjectProvider::numbered_a16(
    std::string_view directory, std::int32_t index) {
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
    return std::nullopt;
}

MapObjectFrame PakManObjectProvider::load_frame(const std::string& asset) {
    std::vector<std::byte> decoded;
    try {
        decoded = NzDecoder::decode(resources_.bytes(asset));
    } catch (const std::exception& error) {
        throw ResourceError("cannot decode MAN frame '" + asset
            + "': " + error.what());
    }
    const auto [offset_x, offset_y] = a16_offsets(decoded);
    MapObjectFrame frame;
    try {
        frame.image = A16Decoder::decode(decoded);
    } catch (const std::exception& error) {
        throw ResourceError("cannot decode A16 frame '" + asset
            + "': " + error.what());
    }
    frame.offset_x = offset_x;
    frame.offset_y = offset_y;
    return frame;
}

void PakManObjectProvider::load_delays(
    std::string_view directory, SceneAnimation& frames) {
    try {
        const std::string asset = std::string(directory) + "/perinfo.dat";
        if (!resources_.exists(asset)) {
            return;
        }
        const Pif pif = Pif::parse(NzDecoder::decode(resources_.bytes(asset)));
        if (pif.rows.size() != frames.size()) {
            return;
        }
        for (std::size_t index = 0; index < frames.size(); ++index) {
            const std::uint32_t delay = pif.rows[index][5];
            frames[index].duration_ms = delay == 0 ? 80U : delay;
        }
    } catch (const std::exception&) {
    }
}

SharedSceneAnimation PakManObjectProvider::load(std::string_view man_path) {
    for (const std::string& directory : asset_variants(man_path)) {
        std::optional<Pif> pif;
        try {
            const std::string metadata = directory + "/perinfo.dat";
            if (resources_.exists(metadata)) {
                pif = Pif::parse(NzDecoder::decode(resources_.bytes(metadata)));
            }
        } catch (const std::exception&) {
            pif.reset();
        }
        const std::size_t frame_limit = pif.has_value()
            && !pif->rows.empty() && pif->rows.size() <= 1000
            ? pif->rows.size()
            : 1000;
        SceneAnimation frames;
        for (std::size_t index = 0; index < frame_limit; ++index) {
            const std::optional<std::string> asset = numbered_a16(
                directory, static_cast<std::int32_t>(index));
            if (asset.has_value()) {
                frames.push_back(load_frame(*asset));
            }
        }
        if (frames.empty()) {
            continue;
        }
        if (pif.has_value()) {
            if (pif->rows.size() == frames.size()) {
                for (std::size_t index = 0; index < frames.size(); ++index) {
                    const std::uint32_t delay = pif->rows[index][5];
                    frames[index].duration_ms = delay == 0 ? 80U : delay;
                }
            }
        } else {
            load_delays(directory, frames);
        }
        return std::make_shared<SceneAnimation>(std::move(frames));
    }
    return nullptr;
}

} // namespace dhsh
