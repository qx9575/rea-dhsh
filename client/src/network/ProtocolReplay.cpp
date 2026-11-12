#include "network/ProtocolReplay.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace dhsh {
namespace {

constexpr std::uint32_t max_payload_size = 8000;
constexpr std::uint16_t login_command = 0x4004;
constexpr std::uint64_t magic_size = 4;
constexpr std::uint64_t record_header_size = 12;

std::uint16_t read_u16(const unsigned char* bytes) {
    return static_cast<std::uint16_t>(bytes[0])
        | (static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t read_u32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

[[noreturn]] void record_error(std::size_t index, const std::string& message) {
    throw std::runtime_error("record " + std::to_string(index) + ": " + message);
}

void read_exact(
    std::ifstream& input,
    char* destination,
    std::streamsize size,
    std::size_t record_index,
    const char* description) {
    input.read(destination, size);
    if (!input) {
        record_error(record_index, std::string("truncated ") + description);
    }
}

} // namespace

ProtocolReplay ProtocolReplay::load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open replay: " + path.string());
    }
    const std::streampos end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("cannot determine replay size: " + path.string());
    }
    const auto file_size = static_cast<std::uint64_t>(end);
    input.seekg(0, std::ios::beg);
    if (!input) {
        throw std::runtime_error("cannot seek replay: " + path.string());
    }

    std::array<unsigned char, magic_size> magic{};
    input.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
    if (!input || magic != std::array<unsigned char, magic_size>{'D', 'H', 'R', '1'}) {
        throw std::runtime_error("invalid DHR1 magic");
    }

    ProtocolReplay replay;
    std::uint64_t offset = magic_size;
    std::uint32_t previous_elapsed = 0;
    while (offset < file_size) {
        const std::size_t index = replay.messages_.size();
        if (file_size - offset < record_header_size) {
            record_error(index, "truncated record header");
        }

        std::array<unsigned char, record_header_size> header{};
        read_exact(
            input,
            reinterpret_cast<char*>(header.data()),
            static_cast<std::streamsize>(header.size()),
            index,
            "record header");
        offset += record_header_size;

        const std::uint32_t elapsed_ms = read_u32(header.data());
        const std::uint16_t command = read_u16(header.data() + 4);
        const std::uint8_t direction_value = header[6];
        const std::uint8_t flags = header[7];
        const std::uint32_t payload_size = read_u32(header.data() + 8);

        if (command == login_command) {
            record_error(index, "login command 0x4004 is forbidden");
        }
        if (payload_size > max_payload_size) {
            record_error(index, "payload exceeds 8000 bytes");
        }
        if (payload_size > file_size - offset) {
            record_error(index, "truncated payload");
        }
        if (flags != 0) {
            record_error(index, "nonzero reserved flags");
        }
        MessageDirection direction;
        if (direction_value == static_cast<std::uint8_t>(MessageDirection::server_to_client)) {
            direction = MessageDirection::server_to_client;
        } else if (direction_value == static_cast<std::uint8_t>(MessageDirection::client_to_server)) {
            direction = MessageDirection::client_to_server;
        } else {
            record_error(index, "invalid direction " + std::to_string(direction_value));
        }
        if (!replay.messages_.empty() && elapsed_ms < previous_elapsed) {
            record_error(index, "timestamp is not monotonic");
        }

        DecodedMessage message;
        message.elapsed_ms = elapsed_ms;
        message.command = command;
        message.direction = direction;
        message.payload.resize(payload_size);
        if (payload_size != 0) {
            read_exact(
                input,
                reinterpret_cast<char*>(message.payload.data()),
                static_cast<std::streamsize>(payload_size),
                index,
                "payload");
        }
        offset += payload_size;
        replay.messages_.push_back(std::move(message));
        previous_elapsed = elapsed_ms;
    }
    return replay;
}

const std::vector<DecodedMessage>& ProtocolReplay::messages() const noexcept {
    return messages_;
}

} // namespace dhsh
