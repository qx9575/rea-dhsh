#pragma once

#include "formats/MapFormats.h"
#include "render/Image.h"
#include "resource/CachedImageProvider.h"
#include "resource/ResourceError.h"
#include "resource/ResourceProvider.h"
#include "scene/Character.h"
#include "scene/CoverMask.h"
#include "scene/Entity.h"
#include "scene/SceneObject.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dhsh {

class SoftwareRenderer;

struct SceneRenderDiagnostics {
    double background_ms = 0.0;
    double setup_ms = 0.0;
    double man_ms = 0.0;
    double entity_mask_ms = 0.0;
    double entity_draw_ms = 0.0;
    std::uint64_t cover_tests = 0;
    std::uint64_t covering_masks = 0;
    std::uint64_t sprite_copies = 0;
};

struct SceneLoadDiagnostics {
    double map_ms = 0.0;
    double mlk_ms = 0.0;
    double background_ms = 0.0;
    double cel_ms = 0.0;
    double man_ms = 0.0;
    double core_ms = 0.0;
    double cover_masks_ms = 0.0;
    double objects_ms = 0.0;
};

class ManObjectProvider {
public:
    virtual ~ManObjectProvider() = default;
    virtual std::optional<MapObjectFrame> load_first_frame(const ManEntry& entry) = 0;
    virtual SharedSceneAnimation load_animation(const ManEntry& entry) {
        (void)entry;
        return nullptr;
    }
};

struct SkippedSceneObject {
    std::string path;
    std::string reason;
};

// Animated view of one map: MAP dimensions, background, ground CEL, MAN table
// and core cover-mask table. The referenced CachedImageProvider owns the
// decoded background and must outlive this scene, which is the normal
// load-then-render scope used by the client and tests.
class MapScene {
public:
    static MapScene load(ResourceProvider& resources,
        CachedImageProvider& images,
        std::string_view name,
        std::initializer_list<ManObjectProvider*> object_providers = {},
        SceneLoadDiagnostics* diagnostics = nullptr);

    const std::string& name() const noexcept {
        return name_;
    }

    std::uint32_t width() const noexcept {
        return width_;
    }

    std::uint32_t height() const noexcept {
        return height_;
    }

    // Never null for a scene returned by load(); a default-constructed scene
    // cannot exist outside the class and throws here instead of dereferencing.
    const Image& background() const {
        if (background_ == nullptr) {
            throw ResourceError(
                "map scene has no background; construct it with MapScene::load");
        }
        return *background_;
    }

    // Computed once while loading the immutable cached background.
    bool background_opaque() const noexcept {
        return background_opaque_;
    }

    const Man& man() const noexcept {
        return man_;
    }

    const Mlk& mlk() const noexcept {
        return mlk_;
    }

    const Cel& cel() const noexcept {
        return cel_;
    }

    const std::optional<Core>& core() const noexcept {
        return core_;
    }

    const std::vector<SceneObject>& objects() const noexcept {
        return objects_;
    }

    const std::vector<SkippedSceneObject>& skipped_objects() const noexcept {
        return skipped_objects_;
    }

    std::vector<CoverMask>& cover_masks() noexcept {
        return cover_masks_;
    }

    const std::vector<CoverMask>& cover_masks() const noexcept {
        return cover_masks_;
    }

    void set_player(Character player);
    Character* player() noexcept;
    const Character* player() const noexcept;
    void add_npc(Entity npc);
    const std::vector<Entity>& npcs() const noexcept;

    bool advance(std::uint32_t milliseconds);

    // Draws the background and depth-sorted MAN/entity frames. The camera is
    // the top-left map coordinate shown at viewport (0, 0).
    void render(SoftwareRenderer& renderer,
        std::int32_t camera_x,
        std::int32_t camera_y,
        SceneRenderDiagnostics* diagnostics = nullptr) const;

private:
    struct OcclusionKey {
        EntityPosition position;
        EntityPosition origin;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint64_t animation_revision = 0;
        std::vector<bool> mask_active;

        bool operator==(const OcclusionKey&) const = default;
    };

    struct MaskedFrame {
        EntityState state = EntityState::idle;
        Direction direction = Direction::down;
        std::size_t frame = 0;
        EntityPosition origin;
        Image image;
    };

    struct EntityOcclusionCache {
        bool candidates_valid = false;
        OcclusionKey key;
        std::vector<std::size_t> candidates;
        std::vector<MaskedFrame> frames;
    };

    // Only the load() factory may construct a scene; a default-constructed
    // scene has no validated background or dimensions.
    MapScene() = default;

    void draw_entity(const Entity& entity, EntityOcclusionCache& cache,
        const std::vector<bool>& mask_active, SoftwareRenderer& renderer,
        std::int32_t camera_x, std::int32_t camera_y,
        SceneRenderDiagnostics* diagnostics) const;

    std::string name_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    const Image* background_ = nullptr;
    bool background_opaque_ = false;
    Man man_;
    Mlk mlk_;
    Cel cel_;
    std::optional<Core> core_;
    std::vector<SceneObject> objects_;
    std::vector<CoverMask> cover_masks_;
    std::vector<SkippedSceneObject> skipped_objects_;
    std::optional<Character> player_;
    std::vector<Entity> npcs_;
    mutable EntityOcclusionCache player_occlusion_;
    mutable std::vector<EntityOcclusionCache> npc_occlusion_;
};

} // namespace dhsh
