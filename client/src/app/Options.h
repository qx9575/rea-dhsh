#pragma once

#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>

namespace dhsh {

// Command-line surface of dhsh_client. The default mode stays fully offline;
// the limited online chat mode is opt-in via the valueless --online flag and
// is mutually exclusive with --replay. --online claims online chat only --
// never a full online world.
struct Options {
    std::filesystem::path shdata;
    std::filesystem::path cache;
    std::filesystem::path capture;
    std::filesystem::path log;
    std::filesystem::path replay;
    std::string map = "YingTianFu";
    bool acceptance = false;
    bool help = false;
    bool online = false;  // --online: interactive credentials, live chat only
};

// Default plaintext login gateway, used only after an explicit --online.
// This is the currently evidenced endpoint; nothing here hardcodes accounts,
// passwords, or roles.
inline constexpr char kDefaultOnlineGatewayHost[] = "101.42.255.186";
inline constexpr std::uint16_t kDefaultOnlineGatewayPort = 8000;

void print_usage(std::ostream& output);
Options parse_options(int argc, char** argv);

} // namespace dhsh
