#pragma once

#include "avatar/AvatarCatalog.h"
#include "render/Image.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dhsh {

class ResourceProvider;

struct AvatarFrame {
    Image image;
    std::uint32_t duration_ms = 80;
};

struct AvatarAnimation {
    std::vector<AvatarFrame> frames;
    std::uint32_t direction = 0;
};

class Avatar {
public:
    Avatar(ResourceProvider& resources, const AvatarCatalog& catalog);

    AvatarAnimation assemble(const AvatarPreset& preset, std::uint32_t action,
        std::uint32_t requested_direction) const;

private:
    ResourceProvider& resources_;
    const AvatarCatalog& catalog_;
};

} // namespace dhsh
