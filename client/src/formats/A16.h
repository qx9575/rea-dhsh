#pragma once

#include "render/Image.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace dhsh {

class A16Decoder {
public:
    static Image decode(std::span<const std::byte> data);
};

} // namespace dhsh
