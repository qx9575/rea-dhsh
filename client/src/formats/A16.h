#pragma once

#include "render/Image.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace dhsh {

struct CtiPalette;

class A16Decoder {
public:
    static Image decode(std::span<const std::byte> data);
    static Image decode(std::span<const std::byte> data, const CtiPalette* palette);
};

} // namespace dhsh
