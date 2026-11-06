#include "avatar/Avatar.h"

#include "formats/Apg.h"
#include "formats/Cti.h"
#include "formats/Pif.h"
#include "resource/NzDecoder.h"
#include "resource/ResourceError.h"
#include "resource/ResourceProvider.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dhsh {
namespace {

constexpr std::uint32_t canvas_width = 640;
constexpr std::uint32_t canvas_height = 480;
constexpr std::array<std::uint32_t, 8> direction_fallback = {3, 7, 0, 1, 2, 4, 5, 6};

struct PartFrame {
    Image image;
    std::int16_t x = 0;
    std::int16_t y = 0;
};

struct PartAnimation {
    std::vector<PartFrame> frames;
    std::int32_t anchor_x = 160;
    std::int32_t anchor_y = 210;
    std::uint32_t delay_ms = 80;
    std::vector<std::uint16_t> keys;
};

std::string part_path(const AvatarPart& part, std::uint32_t action,
    std::uint32_t direction) {
    return part.part + "/" + std::to_string(part.model) + "/"
        + std::to_string(action) + "/" + std::to_string(direction) + ".apg";
}

std::string metadata_path(const AvatarPart& part, std::uint32_t action) {
    return part.part + "/" + std::to_string(part.model) + "/"
        + std::to_string(action) + "/perinfo.dat";
}

std::string palette_path(const AvatarPart& part) {
    return part.part + "/" + std::to_string(part.model) + "/clrtb.cti";
}

bool any_part_exists(ResourceProvider& resources, const AvatarPreset& preset,
    std::uint32_t action, std::uint32_t direction) {
    return std::any_of(preset.parts.begin(), preset.parts.end(),
        [&](const AvatarPart& part) {
            return resources.exists(part_path(part, action, direction));
        });
}

std::uint32_t resolve_direction(ResourceProvider& resources, const AvatarPreset& preset,
    std::uint32_t action, std::uint32_t requested) {
    if (any_part_exists(resources, preset, action, requested)) {
        return requested;
    }
    for (const std::uint32_t direction : direction_fallback) {
        if (direction != requested && any_part_exists(resources, preset, action, direction)) {
            return direction;
        }
    }
    throw FormatError("avatar part resource not found: "
        + part_path(preset.parts.front(), action, requested));
}

std::vector<std::byte> decoded_resource(ResourceProvider& resources,
    const std::string& path) {
    std::vector<std::byte> stored = resources.bytes(path);
    if (stored.size() >= 2 && stored[0] == std::byte{'N'} && stored[1] == std::byte{'Z'}) {
        return NzDecoder::decode(stored);
    }
    return stored;
}

PartAnimation load_part(ResourceProvider& resources, const AvatarCatalog& catalog,
    const AvatarPreset& preset, const AvatarPart& part, std::uint32_t action,
    std::uint32_t direction) {
    const std::string path = part_path(part, action, direction);
    if (!resources.exists(path)) {
        throw FormatError("avatar part resource not found: " + path);
    }

    std::optional<CtiPalette> palette;
    const std::string cti_path = palette_path(part);
    if (resources.exists(cti_path)) {
        palette = CtiPalette::parse(decoded_resource(resources, cti_path));
    }
    ApgAnimation decoded = ApgDecoder::decode(
        decoded_resource(resources, path), palette ? &*palette : nullptr);

    PartAnimation result;
    result.frames.reserve(decoded.frames.size());
    for (ApgFrame& frame : decoded.frames) {
        result.frames.push_back(PartFrame{
            std::move(frame.image), frame.anchor_x, frame.anchor_y});
    }
    if (result.frames.empty()) {
        throw FormatError("avatar part has no frames: " + path);
    }

    const std::string pif_path = metadata_path(part, action);
    if (resources.exists(pif_path)) {
        try {
            const Pif pif = Pif::parse(decoded_resource(resources, pif_path));
            if (direction < pif.rows.size()) {
                const auto& row = pif.rows[direction];
                result.anchor_x = row[2];
                result.anchor_y = row[3];
                result.delay_ms = std::max<std::uint32_t>(1, row[5]);
            }
        } catch (const FormatError&) {
            result.anchor_x = 160;
            result.anchor_y = 210;
            result.delay_ms = 80;
        }
    }
    result.keys = catalog.layer_keys(preset.pak, part.part, part.model, action,
        direction, result.frames.size());
    return result;
}

unsigned channel(std::byte value) {
    return std::to_integer<unsigned>(value);
}

std::byte to_byte(unsigned value) {
    return static_cast<std::byte>(value & 0xffU);
}

unsigned shift_for_div255(unsigned value) {
    return ((value >> 8) + value) >> 8;
}

void blend_pixel(Image& target, std::size_t destination, const Image& source,
    std::size_t source_index) {
    const unsigned source_alpha = channel(source.rgba[source_index + 3]);
    if (source_alpha == 0) {
        return;
    }
    constexpr unsigned precision_bits = 7;
    constexpr unsigned precision_scale = 1U << precision_bits;
    const unsigned destination_alpha = channel(target.rgba[destination + 3]);
    const unsigned output_alpha_255 = source_alpha * 255U
        + destination_alpha * (255U - source_alpha);
    const unsigned source_coefficient = source_alpha * 255U * 255U * precision_scale
        / output_alpha_255;
    const unsigned destination_coefficient = 255U * precision_scale - source_coefficient;
    for (std::size_t component = 0; component < 3; ++component) {
        const unsigned blended = channel(source.rgba[source_index + component])
                * source_coefficient
            + channel(target.rgba[destination + component]) * destination_coefficient;
        target.rgba[destination + component] = to_byte(
            shift_for_div255(blended + (0x80U << precision_bits)) >> precision_bits);
    }
    target.rgba[destination + 3] = to_byte(shift_for_div255(output_alpha_255 + 0x80U));
}

void draw(Image& target, const Image& source, std::int32_t x, std::int32_t y) {
    const std::int64_t left = std::max<std::int64_t>(0, x);
    const std::int64_t top = std::max<std::int64_t>(0, y);
    const std::int64_t right = std::min<std::int64_t>(
        canvas_width, static_cast<std::int64_t>(x) + source.width);
    const std::int64_t bottom = std::min<std::int64_t>(
        canvas_height, static_cast<std::int64_t>(y) + source.height);
    for (std::int64_t destination_y = top; destination_y < bottom; ++destination_y) {
        for (std::int64_t destination_x = left; destination_x < right; ++destination_x) {
            const std::size_t source_x = static_cast<std::size_t>(destination_x - x);
            const std::size_t source_y = static_cast<std::size_t>(destination_y - y);
            blend_pixel(target,
                (static_cast<std::size_t>(destination_y) * canvas_width
                    + static_cast<std::size_t>(destination_x)) * 4,
                source, (source_y * source.width + source_x) * 4);
        }
    }
}

} // namespace

Avatar::Avatar(ResourceProvider& resources, const AvatarCatalog& catalog)
    : resources_(resources), catalog_(catalog) {
}

AvatarAnimation Avatar::assemble(const AvatarPreset& preset, std::uint32_t action,
    std::uint32_t requested_direction, AvatarDecodeOptions options) const {
    if (preset.parts.empty()) {
        throw FormatError("avatar preset has no parts");
    }
    AvatarAnimation result;
    result.direction = resolve_direction(resources_, preset, action, requested_direction);

    std::vector<PartAnimation> animations;
    animations.reserve(preset.parts.size());
    for (const AvatarPart& part : preset.parts) {
        animations.push_back(load_part(resources_, catalog_, preset, part, action,
            result.direction));
    }
    std::size_t frame_count = 0;
    for (const PartAnimation& animation : animations) {
        frame_count = std::max(frame_count, animation.frames.size());
    }
    if (frame_count > result.frames.max_size()) {
        throw FormatError("avatar frame count exceeds the container limit");
    }
    constexpr std::uint64_t row_bytes = static_cast<std::uint64_t>(canvas_width) * 4U;
    if (row_bytes > std::numeric_limits<std::uint64_t>::max() / canvas_height) {
        throw FormatError("avatar canvas RGBA dimensions overflow uint64_t");
    }
    const std::uint64_t rgba_bytes_64 = row_bytes * canvas_height;
    if (rgba_bytes_64 > std::numeric_limits<std::size_t>::max()
        || rgba_bytes_64 > std::vector<std::byte>().max_size()) {
        throw FormatError("avatar canvas RGBA dimensions overflow the allocation limit");
    }
    const std::size_t rgba_bytes = static_cast<std::size_t>(rgba_bytes_64);
    if (frame_count > std::numeric_limits<std::size_t>::max() / rgba_bytes) {
        throw FormatError("avatar composed RGBA size overflows size_t");
    }
    if (frame_count > options.max_composed_rgba_bytes / rgba_bytes) {
        throw FormatError("avatar composed RGBA budget exceeded");
    }

    struct DrawPart {
        std::uint16_t key;
        const PartAnimation* animation;
        const AvatarPart* part;
        std::size_t frame;
    };
    result.frames.reserve(frame_count);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        std::vector<DrawPart> order;
        order.reserve(animations.size());
        for (std::size_t part_index = 0; part_index < animations.size(); ++part_index) {
            const PartAnimation& animation = animations[part_index];
            const std::size_t index = frame % animation.frames.size();
            const std::uint16_t key = animation.keys.empty() ? 99 : animation.keys[index];
            order.push_back(DrawPart{key, &animation, &preset.parts[part_index], index});
        }
        std::stable_sort(order.begin(), order.end(),
            [](const DrawPart& left, const DrawPart& right) { return left.key < right.key; });

        Image canvas;
        canvas.width = canvas_width;
        canvas.height = canvas_height;
        canvas.rgba.assign(rgba_bytes, std::byte{0});
        std::vector<AvatarLayer> layers;
        layers.reserve(order.size());
        for (const DrawPart& draw_part : order) {
            const PartAnimation& animation = *draw_part.animation;
            const PartFrame& part_frame = animation.frames[draw_part.frame];
            const std::int32_t origin_x = static_cast<std::int32_t>(canvas_width / 2)
                - animation.anchor_x + part_frame.x;
            const std::int32_t origin_y = static_cast<std::int32_t>(canvas_height / 2)
                - animation.anchor_y + part_frame.y;
            draw(canvas, part_frame.image, origin_x, origin_y);
            layers.push_back(AvatarLayer{draw_part.part->part, draw_part.part->model,
                draw_part.frame, draw_part.key, origin_x, origin_y});
        }
        result.frames.push_back(AvatarFrame{
            std::move(canvas), animations.front().delay_ms, std::move(layers)});
    }
    return result;
}

} // namespace dhsh
