#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dhsh {

enum class MessageDirection : std::uint8_t {
    server_to_client = 0,
    client_to_server = 1,
};

struct DecodedMessage {
    std::uint32_t elapsed_ms = 0;
    std::uint16_t command = 0;
    MessageDirection direction = MessageDirection::server_to_client;
    std::vector<std::byte> payload;
};

} // namespace dhsh
