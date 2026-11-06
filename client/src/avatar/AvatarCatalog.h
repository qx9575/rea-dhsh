#pragma once

#include "formats/Apci.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dhsh {

struct AvatarPart {
    std::string part;
    std::uint32_t model = 0;
};

struct AvatarPreset {
    std::string pak;
    std::vector<AvatarPart> parts;
};

class AvatarCatalog {
public:
    explicit AvatarCatalog(std::vector<std::byte> apci_data);

    static AvatarPreset default_preset();

    std::vector<std::uint16_t> layer_keys(std::string_view pak,
        std::string_view part, std::uint32_t model, std::uint32_t action,
        std::size_t direction, std::size_t frame_count) const;

private:
    std::vector<std::byte> data_;
    ApciFile file_;
};

} // namespace dhsh
