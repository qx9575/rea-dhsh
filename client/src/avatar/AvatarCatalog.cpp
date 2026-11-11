#include "avatar/AvatarCatalog.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace dhsh {
namespace {

std::uint8_t byte_at(std::span<const std::byte> data, std::size_t offset) {
    return std::to_integer<std::uint8_t>(data[offset]);
}

std::uint16_t read_u16(std::span<const std::byte> data, std::size_t offset) {
    return static_cast<std::uint16_t>(byte_at(data, offset))
        | static_cast<std::uint16_t>(byte_at(data, offset + 1)) << 8;
}

std::string family_name(std::string_view pak) {
    const std::size_t separator = pak.find_last_of("/\\");
    if (separator != std::string_view::npos) {
        pak.remove_prefix(separator + 1);
    }
    const std::size_t dot = pak.find_last_of('.');
    if (dot != std::string_view::npos) {
        pak = pak.substr(0, dot);
    }
    return std::string(pak);
}

} // namespace

AvatarCatalog::AvatarCatalog(std::vector<std::byte> apci_data)
    : data_(std::move(apci_data)), file_(ApciFile::parse(data_)) {
}

AvatarPreset AvatarCatalog::default_preset() {
    return AvatarPreset{"player.pak", {{"body", 200}, {"head", 105}}};
}

std::vector<std::uint16_t> AvatarCatalog::layer_keys(std::string_view pak,
    std::string_view part, std::uint32_t model, std::uint32_t action,
    std::size_t direction, std::size_t frame_count) const {
    const std::string record_name = family_name(pak) + "\\" + std::string(part);
    const ApciItem* selected = nullptr;
    for (const ApciGroup& group : file_.groups) {
        for (const ApciRecord& record : group.records) {
            if (record.name != record_name) {
                continue;
            }
            for (const ApciEntry& entry : record.entries) {
                if (entry.model_id != model) {
                    continue;
                }
                for (const ApciMid& mid : entry.mids) {
                    if (mid.action_id() == action && direction < mid.items.size()) {
                        selected = &mid.items[direction];
                        break;
                    }
                }
            }
        }
    }
    if (selected == nullptr || selected->blob_size == 0) {
        return {};
    }

    std::vector<std::uint16_t> result;
    result.reserve(frame_count);
    if (selected->cell_size() == 1) {
        for (std::size_t frame = 0; frame < frame_count; ++frame) {
            const std::size_t index = selected->layout() == 1
                ? 0
                : std::min(frame, selected->blob_size - 1);
            result.push_back(byte_at(data_, selected->blob_offset + index));
        }
    } else if (selected->cell_size() == 12) {
        const std::size_t cells = selected->blob_size / 12;
        for (std::size_t frame = 0; frame < frame_count && cells != 0; ++frame) {
            const std::size_t cell = std::min(frame, cells - 1);
            result.push_back(read_u16(data_, selected->blob_offset + cell * 12 + 4));
        }
    }
    return result;
}

} // namespace dhsh
