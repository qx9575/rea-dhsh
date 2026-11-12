#include "network/ProtocolReplay.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void append_u16(Bytes& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xffU));
    bytes.push_back(static_cast<std::byte>((value >> 8) & 0xffU));
}

void append_u32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_text(Bytes& bytes, const std::string& text) {
    for (const char value : text) {
        bytes.push_back(static_cast<std::byte>(value));
    }
}

void append_record(
    Bytes& bytes,
    std::uint32_t elapsed_ms,
    std::uint16_t command,
    std::uint8_t direction,
    const std::string& payload,
    std::uint8_t flags = 0) {
    append_u32(bytes, elapsed_ms);
    append_u16(bytes, command);
    bytes.push_back(static_cast<std::byte>(direction));
    bytes.push_back(static_cast<std::byte>(flags));
    append_u32(bytes, static_cast<std::uint32_t>(payload.size()));
    append_text(bytes, payload);
}

Bytes golden_replay() {
    Bytes bytes;
    append_text(bytes, "DHR1");
    append_record(bytes, 0, 0x303f, 0, "chat");
    append_record(bytes, 20, 0x301e, 0, "object");
    check(bytes.size() == 38, "incorrect golden replay size");
    return bytes;
}

void write_file(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create replay fixture");
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("cannot write replay fixture");
    }
}

class TempReplay {
public:
    explicit TempReplay(const Bytes& bytes) {
        static unsigned sequence = 0;
        path_ = std::filesystem::temp_directory_path()
            / ("dhsh_test_protocol_replay_" + std::to_string(++sequence) + ".dhr");
        write_file(path_, bytes);
    }

    TempReplay(const TempReplay&) = delete;
    TempReplay& operator=(const TempReplay&) = delete;

    ~TempReplay() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::string payload_text(const std::vector<std::byte>& payload) {
    std::string text;
    for (const std::byte value : payload) {
        text.push_back(static_cast<char>(value));
    }
    return text;
}

void expect_reject(const Bytes& bytes, const std::string& diagnostic) {
    const TempReplay file(bytes);
    try {
        const auto replay = dhsh::ProtocolReplay::load(file.path());
        (void)replay;
    } catch (const std::exception& error) {
        check(
            std::string(error.what()).find(diagnostic) != std::string::npos,
            "wrong diagnostic for " + diagnostic + ": " + error.what());
        return;
    }
    throw std::runtime_error("expected rejection containing: " + diagnostic);
}

void test_two_records_match_python_golden() {
    const TempReplay file(golden_replay());
    const auto replay = dhsh::ProtocolReplay::load(file.path());
    const auto& messages = replay.messages();
    check(messages.size() == 2, "incorrect record count");
    check(messages[0].elapsed_ms == 0 && messages[0].command == 0x303f, "incorrect first header");
    check(messages[0].direction == dhsh::MessageDirection::server_to_client, "incorrect first direction");
    check(payload_text(messages[0].payload) == "chat", "incorrect first payload");
    check(messages[1].elapsed_ms == 20 && messages[1].command == 0x301e, "incorrect second header");
    check(messages[1].direction == dhsh::MessageDirection::server_to_client, "incorrect second direction");
    check(payload_text(messages[1].payload) == "object", "incorrect second payload");
}

void test_empty_equal_time_c2s_and_maximum_payload() {
    Bytes bytes;
    append_text(bytes, "DHR1");
    {
        const TempReplay file(bytes);
        check(dhsh::ProtocolReplay::load(file.path()).messages().empty(), "empty replay rejected");
    }
    append_record(bytes, 0, 0x303f, 0, "");
    append_record(bytes, 0, 0x301e, 1, "binary");
    append_record(bytes, 0xffffffffU, 0x303f, 0, std::string(8000, 'x'));
    const TempReplay file(bytes);
    const auto replay = dhsh::ProtocolReplay::load(file.path());
    const auto& messages = replay.messages();
    check(messages.size() == 3, "valid boundary records missing");
    check(messages[0].payload.empty(), "empty payload changed");
    check(messages[1].direction == dhsh::MessageDirection::client_to_server, "c2s direction rejected");
    check(messages[2].payload.size() == 8000, "maximum payload rejected");
}

void test_bad_magic_and_truncation() {
    for (const std::string magic : {"", "DHR", "DHR0"}) {
        Bytes bytes;
        append_text(bytes, magic);
        expect_reject(bytes, "magic");
    }
    Bytes header;
    append_text(header, "DHR1");
    header.resize(15, std::byte{0});
    expect_reject(header, "record 0");
    expect_reject(header, "truncated");

    Bytes truncated = golden_replay();
    truncated.pop_back();
    expect_reject(truncated, "record 1");
    expect_reject(truncated, "truncated");

    Bytes trailing = golden_replay();
    trailing.push_back(std::byte{0});
    expect_reject(trailing, "record 2");
    expect_reject(trailing, "truncated");
}

void test_oversized_payload_is_rejected_before_allocation() {
    Bytes bytes;
    append_text(bytes, "DHR1");
    append_u32(bytes, 0);
    append_u16(bytes, 0x303f);
    bytes.push_back(std::byte{0});
    bytes.push_back(std::byte{0});
    append_u32(bytes, 8001);
    expect_reject(bytes, "record 0");
    expect_reject(bytes, "payload");
}

void test_login_flags_direction_and_non_monotonic_time() {
    Bytes login = golden_replay();
    login[8] = std::byte{0x04};
    login[9] = std::byte{0x40};
    expect_reject(login, "record 0");
    expect_reject(login, "0x4004");

    Bytes flags = golden_replay();
    flags[11] = std::byte{1};
    expect_reject(flags, "record 0");
    expect_reject(flags, "flags");

    Bytes direction = golden_replay();
    direction[10] = std::byte{2};
    expect_reject(direction, "record 0");
    expect_reject(direction, "direction");

    Bytes time = golden_replay();
    time[4] = std::byte{20};
    time[20] = std::byte{19};
    expect_reject(time, "record 1");
    expect_reject(time, "timestamp");
}

} // namespace

int main() {
    try {
        test_two_records_match_python_golden();
        test_empty_equal_time_c2s_and_maximum_payload();
        test_bad_magic_and_truncation();
        test_oversized_payload_is_rejected_before_allocation();
        test_login_flags_direction_and_non_monotonic_time();
        std::cout << "protocol_replay tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "protocol_replay test failure: " << error.what() << '\n';
        return 1;
    }
}
