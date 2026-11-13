#include "network/NpcReplayDriver.h"

#include <ostream>
#include <utility>

namespace dhsh {

NpcReplayDriver::NpcReplayDriver(const ProtocolReplay& replay, MapScene& scene,
    ResourceProvider& resources, std::ostream* diagnostics, ChatModel* chat_model)
    : replay_(replay), scene_(scene), resources_(resources),
      diagnostics_(diagnostics), chat_model_(chat_model) {
}

bool NpcReplayDriver::advance_to(std::uint32_t elapsed_ms) {
    bool changed = false;
    const std::vector<DecodedMessage>& messages = replay_.messages();
    while (next_message_ < messages.size()
        && messages[next_message_].elapsed_ms <= elapsed_ms) {
        const DecodedMessage& message = messages[next_message_];
        if (chat_model_ != nullptr) {
            if (auto chat = chat_decoder_.decode(message); chat.has_value()) {
                chat_model_->append(std::move(*chat));
                ++chat_count_;
                changed = true;
                ++next_message_;
                continue;
            }
        }
        changed = apply(decoder_.decode(message)) || changed;
        ++next_message_;
    }
    return changed;
}

bool NpcReplayDriver::apply(const NpcSyncEvent& event) {
    if (std::holds_alternative<std::monostate>(event)) {
        ++ignored_count_;
        return false;
    }
    if (const auto* upsert = std::get_if<ObjectUpsert>(&event)) {
        if (!objects_.upsert(upsert->object)) {
            return false;
        }
        const WorldObject* object = objects_.find(upsert->object.object_id);
        if (object == nullptr) {
            return false;
        }
        const bool changed = scene_.upsert_network_npc(
            *object, resources_, diagnostics_);
        if (changed) {
            ++upsert_count_;
            if (diagnostics_ != nullptr) {
                *diagnostics_ << "NPC replay upsert id=" << object->object_id
                              << " shape=" << object->shape.value_or(0)
                              << " position=" << object->position.x << ','
                              << object->position.y << " direction="
                              << static_cast<unsigned>(object->direction)
                              << " revision=" << object->revision << '\n';
            }
        }
        return changed;
    }

    const auto& remove = std::get<ObjectRemove>(event);
    if (!objects_.remove(remove.object_id)) {
        return false;
    }
    const bool changed = scene_.remove_network_npc(remove.object_id);
    if (changed) {
        ++removal_count_;
        if (diagnostics_ != nullptr) {
            *diagnostics_ << "NPC replay remove id=" << remove.object_id << '\n';
        }
    }
    return changed;
}

bool NpcReplayDriver::finished() const noexcept {
    return next_message_ == replay_.messages().size();
}

} // namespace dhsh
