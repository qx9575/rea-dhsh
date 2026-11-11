#pragma once

#include "render/Image.h"
#include "resource/ResourceProvider.h"

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace dhsh {

// Decodes a resource once per normalized logical path and keeps the decoded
// image alive for the lifetime of the provider. Failed reads and failed
// decodes are never cached, so a repaired resource is retried on the next
// request. References returned by get() remain stable: the cache only inserts
// into a node-based container and never erases.
class CachedImageProvider {
public:
    using Decoder = std::function<Image(std::span<const std::byte>)>;

    CachedImageProvider(ResourceProvider& resources, Decoder decoder);

    // Uses WicImage::decode (PNG/JPEG/BMP).
    explicit CachedImageProvider(ResourceProvider& resources);

    const Image& get(std::string_view logical_path);
    bool cached(std::string_view logical_path) const;

private:
    ResourceProvider& resources_;
    Decoder decoder_;
    std::unordered_map<std::string, Image> cache_;
};

} // namespace dhsh
