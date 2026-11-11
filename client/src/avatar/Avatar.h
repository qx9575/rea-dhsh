#pragma once

#include "avatar/AvatarCatalog.h"
#include "render/Image.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dhsh {

class ResourceProvider;

struct AvatarDecodeOptions {
    std::size_t max_composed_rgba_bytes = 256U * 1024U * 1024U;
};

struct AvatarLayer {
    std::string part;
    std::uint32_t model = 0;
    std::size_t source_frame = 0;
    std::uint16_t layer_key = 99;
    std::int32_t origin_x = 0;
    std::int32_t origin_y = 0;
};

struct AvatarFrame {
    Image image;
    std::uint32_t duration_ms = 80;
    std::vector<AvatarLayer> layers;
};

struct AvatarAnimation {
    std::vector<AvatarFrame> frames;
    std::uint32_t direction = 0;
};

class Avatar {
public:
    Avatar(ResourceProvider& resources, const AvatarCatalog& catalog);

    AvatarAnimation assemble(const AvatarPreset& preset, std::uint32_t action,
        std::uint32_t requested_direction, AvatarDecodeOptions options = {}) const;

private:
    ResourceProvider& resources_;
    const AvatarCatalog& catalog_;
};

} // namespace dhsh
