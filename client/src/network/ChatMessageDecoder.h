#pragma once

#include "chat/ChatModel.h"
#include "network/DecodedMessage.h"

#include <optional>

namespace dhsh {

class ChatMessageDecoder {
public:
    std::optional<ChatMessage> decode(const DecodedMessage& message) const;
};

} // namespace dhsh
