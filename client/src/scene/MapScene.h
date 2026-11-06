#pragma once

#include "formats/MapFormats.h"
#include "render/Image.h"
#include "resource/CachedImageProvider.h"
#include "resource/ResourceError.h"
#include "resource/ResourceProvider.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dhsh {

class SoftwareRenderer;

struct MapObjectFrame {
    Image image;
    std::int32_t offset_x = 0;
    std::int32_t offset_y = 0;
};

// Supplies the first rendered frame for one MAN record. Missing frames are not
// fatal: MapScene records the skipped path and continues with the other
// records.
class ManObjectProvider {
public:
    virtual ~ManObjectProvider() = default;
    virtual std::optional<MapObjectFrame> load_first_frame(const ManEntry& entry) = 0;
};

struct SceneObject {
    ManEntry entry;
    std::int32_t x = 0;
    std::int32_t y = 0;
    MapObjectFrame frame;
};

struct SkippedSceneObject {
    std::string path;
    std::string reason;
};

// Static view of one map: MAP dimensions, background, ground CEL, MAN table
// and core cover-mask table. The referenced CachedImageProvider owns the
// decoded background and must outlive this scene, which is the normal
// load-then-render scope used by the client and tests.
class MapScene {
public:
    static MapScene load(ResourceProvider& resources,
        CachedImageProvider& images,
        std::string_view name,
        std::initializer_list<ManObjectProvider*> object_providers = {});

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

    const Man& man() const noexcept {
        return man_;
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

    // Draws the background and the loaded MAN object frames. The camera is the
    // top-left map coordinate shown at viewport (0, 0).
    void render(SoftwareRenderer& renderer,
        std::int32_t camera_x,
        std::int32_t camera_y) const;

private:
    // Only the load() factory may construct a scene; a default-constructed
    // scene has no validated background or dimensions.
    MapScene() = default;

    std::string name_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    const Image* background_ = nullptr;
    Man man_;
    Cel cel_;
    std::optional<Core> core_;
    std::vector<SceneObject> objects_;
    std::vector<SkippedSceneObject> skipped_objects_;
};

} // namespace dhsh
