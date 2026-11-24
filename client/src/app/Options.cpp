#include "app/Options.h"

#include <stdexcept>
#include <string_view>

namespace dhsh {

void print_usage(std::ostream& output) {
    output << "usage: dhsh_client --shdata <directory> [--map <name>]"
              " [--cache <directory>] [--log <file>] [--capture <ppm>]"
              " [--replay <DHR1>] [--acceptance] [--online]\n"
              "  --map defaults to YingTianFu; the Task 12 local spawn preset"
              " currently supports only YingTianFu\n"
              "  --replay drives networked NPCs and chat on the DHR1 recorded clock\n"
              "  --acceptance loads the real bulletin2 SCP; F8 toggles its window\n"
              "  --online starts the limited online chat mode: an interactive"
              " credentials dialog, then live gateway chat only. It is not a"
              " full online world (no NPC sync, movement, or world state) and"
              " cannot be combined with --replay\n"
              "  --cache defaults to " << DHSH_DEFAULT_CACHE_DIR << '\n';
}

Options parse_options(int argc, char** argv) {
    Options options;
    options.cache = DHSH_DEFAULT_CACHE_DIR;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            options.help = true;
            continue;
        }
        if (argument == "--acceptance") {
            options.acceptance = true;
            continue;
        }
        if (argument == "--online") {
            options.online = true;
            continue;
        }
        if (argument != "--shdata" && argument != "--map" && argument != "--cache"
            && argument != "--log" && argument != "--capture"
            && argument != "--replay") {
            throw std::invalid_argument("unknown argument: " + std::string(argument));
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value after " + std::string(argument));
        }
        const std::string value(argv[++index]);
        if (value.empty()) {
            throw std::invalid_argument("empty value after " + std::string(argument));
        }
        if (argument == "--shdata") {
            options.shdata = value;
        } else if (argument == "--map") {
            options.map = value;
        } else if (argument == "--cache") {
            options.cache = value;
        } else if (argument == "--log") {
            options.log = value;
        } else if (argument == "--capture") {
            options.capture = value;
        } else {
            options.replay = value;
        }
    }
    if (options.online && !options.replay.empty()) {
        throw std::invalid_argument(
            "--online cannot be combined with --replay");
    }
    if (!options.help && options.shdata.empty()) {
        throw std::invalid_argument("--shdata is required");
    }
    if (!options.help && options.map != "YingTianFu") {
        throw std::invalid_argument(
            "--map currently supports only YingTianFu with the confirmed local spawn");
    }
    return options;
}

} // namespace dhsh
