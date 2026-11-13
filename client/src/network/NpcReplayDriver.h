#pragma once

#include "network/ChatMessageDecoder.h"
#include "network/NpcSyncDecoder.h"
#include "network/ProtocolReplay.h"
#include "scene/MapScene.h"
#include "scene/WorldObjectStore.h"

#include <cstddef>
#include <cstdint>
#include <iosfwd>

namespace dhsh {

// Applies DHR1 records on their recorded virtual clock. The replay, scene, and
// resource provider must outlive this driver. Unknown messages stay ignored;
// ObjectRemove is routed only when a decoder explicitly produces that event.
class NpcReplayDriver {
public:
    NpcReplayDriver(const ProtocolReplay& replay, MapScene& scene,
        ResourceProvider& resources, std::ostream* diagnostics = nullptr,
        ChatModel* chat_model = nullptr);

    bool advance_to(std::uint32_t elapsed_ms);
    bool apply(const NpcSyncEvent& event);

    std::size_t next_message_index() const noexcept {
        return next_message_;
    }
    bool finished() const noexcept;
    std::size_t upsert_count() const noexcept {
        return upsert_count_;
    }
    std::size_t removal_count() const noexcept {
        return removal_count_;
    }
    std::size_t ignored_count() const noexcept {
        return ignored_count_;
    }
    std::size_t chat_count() const noexcept {
        return chat_count_;
    }

private:
    const ProtocolReplay& replay_;
    MapScene& scene_;
    ResourceProvider& resources_;
    std::ostream* diagnostics_;
    ChatModel* chat_model_;
    WorldObjectStore objects_;
    NpcSyncDecoder decoder_;
    ChatMessageDecoder chat_decoder_;
    std::size_t next_message_ = 0;
    std::size_t upsert_count_ = 0;
    std::size_t removal_count_ = 0;
    std::size_t ignored_count_ = 0;
    std::size_t chat_count_ = 0;
};

} // namespace dhsh
