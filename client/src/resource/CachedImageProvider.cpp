#include "resource/CachedImageProvider.h"

#include "platform/WicImage.h"
#include "resource/ResourceError.h"

#include <cstdint>
#include <exception>
#include <limits>
#include <string>
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

// Matches PakResourceProvider's logical-path normalization: ASCII lowercase,
// either separator accepted, repeated and edge separators removed.
std::string normalize_key(std::string_view path) {
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

std::size_t checked_rgba_bytes(std::uint32_t width, std::uint32_t height) {
    const std::uint64_t row = static_cast<std::uint64_t>(width) * 4;
    if (row != 0
        && static_cast<std::uint64_t>(height)
            > std::numeric_limits<std::uint64_t>::max() / row) {
        throw ResourceError("decoded image dimensions overflow");
    }
    const std::uint64_t total = row * height;
    if (total > std::numeric_limits<std::size_t>::max()) {
        throw ResourceError("decoded image is too large");
    }
    return static_cast<std::size_t>(total);
}

} // namespace

CachedImageProvider::CachedImageProvider(
    ResourceProvider& resources,
    Decoder decoder)
    : resources_(resources), decoder_(std::move(decoder)) {
    if (!decoder_) {
        throw ResourceError("image decoder is empty");
    }
}

CachedImageProvider::CachedImageProvider(ResourceProvider& resources)
    : CachedImageProvider(
        resources,
        [](std::span<const std::byte> data) { return WicImage::decode(data); }) {
}

const Image& CachedImageProvider::get(std::string_view logical_path) {
    const std::string key = normalize_key(logical_path);
    if (key.empty()) {
        throw ResourceError("empty image resource path");
    }

    const auto found = cache_.find(key);
    if (found != cache_.end()) {
        return found->second;
    }

    std::vector<std::byte> data;
    try {
        data = resources_.bytes(key);
    } catch (const std::exception& error) {
        throw ResourceError(
            "cannot read image resource '" + key + "': " + error.what());
    }

    Image image;
    try {
        image = decoder_(data);
    } catch (const std::exception& error) {
        throw ResourceError(
            "cannot decode image resource '" + key + "': " + error.what());
    }

    if (image.rgba.size() != checked_rgba_bytes(image.width, image.height)) {
        throw ResourceError(
            "decoded image resource '" + key
            + "' has a buffer that does not match its dimensions");
    }

    // unordered_map keeps element references stable across rehash, and this
    // container never erases, so callers may retain the returned reference.
    const auto inserted = cache_.emplace(key, std::move(image));
    return inserted.first->second;
}

bool CachedImageProvider::cached(std::string_view logical_path) const {
    return cache_.find(normalize_key(logical_path)) != cache_.end();
}

} // namespace dhsh
