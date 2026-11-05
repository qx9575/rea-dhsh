#pragma once

#include "render/Image.h"

#include <cstddef>
#include <span>

namespace dhsh {

class WicImage {
public:
    static Image decode(std::span<const std::byte> data);
};

} // namespace dhsh
