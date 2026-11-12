#include "scene/WorldObjectStore.h"

namespace dhsh {
namespace {

bool same_visual_state(const WorldObject& left, const WorldObject& right) {
    return left.kind == right.kind
        && left.shape == right.shape
        && left.name == right.name
        && left.position == right.position
        && left.direction == right.direction;
}

} // namespace

bool WorldObjectStore::upsert(const WorldObject& object) {
    if (object.kind == WorldObjectKind::npc
        && (!object.shape.has_value() || *object.shape == 0)) {
        return false;
    }

    const auto found = objects_.find(object.object_id);
    if (found == objects_.end()) {
        WorldObject stored = object;
        stored.revision = 1;
        objects_.emplace(stored.object_id, std::move(stored));
        return true;
    }

    WorldObject& stored = found->second;
    const bool visual_changed = !same_visual_state(stored, object);
    const std::uint64_t next_revision = stored.revision + (visual_changed ? 1 : 0);
    stored = object;
    stored.revision = next_revision;
    return visual_changed;
}

bool WorldObjectStore::remove(std::uint32_t object_id) noexcept {
    return objects_.erase(object_id) != 0;
}

const WorldObject* WorldObjectStore::find(std::uint32_t object_id) const noexcept {
    const auto found = objects_.find(object_id);
    return found == objects_.end() ? nullptr : &found->second;
}

std::size_t WorldObjectStore::size() const noexcept {
    return objects_.size();
}

} // namespace dhsh
