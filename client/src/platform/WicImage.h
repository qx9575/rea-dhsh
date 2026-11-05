#pragma once

#include "render/Image.h"

#include <cstddef>
#include <limits>
#include <span>

namespace dhsh {

struct WicDecodeOptions {
    std::size_t max_rgba_bytes = std::numeric_limits<std::size_t>::max();
};

class WicImage {
public:
    static Image decode(std::span<const std::byte> data, WicDecodeOptions options = {});
};

} // namespace dhsh
