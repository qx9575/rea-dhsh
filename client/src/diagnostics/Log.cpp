#include "diagnostics/Log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace dhsh {
namespace {

char ascii_lower(char value) {
    const auto byte = static_cast<unsigned char>(value);
    if (byte >= 'A' && byte <= 'Z') {
        return static_cast<char>(byte - 'A' + 'a');
    }
    return value;
}

bool key_character(char value) {
    const auto byte = static_cast<unsigned char>(value);
    return std::isalnum(byte) != 0 || value == '_';
}

} // namespace

std::string redact_sensitive(std::string_view message) {
    constexpr std::array keys{
        std::string_view("account"),
        std::string_view("username"),
        std::string_view("password"),
        std::string_view("passwd"),
        std::string_view("token"),
        std::string_view("secret"),
        std::string_view("credential"),
        std::string_view("authorization"),
        std::string_view("cookie"),
    };

    std::string lowered;
    lowered.reserve(message.size());
    std::transform(message.begin(), message.end(), std::back_inserter(lowered),
        [](char value) { return ascii_lower(value); });

    std::string result;
    std::size_t copied_until = 0;
    std::size_t search_from = 0;
    while (search_from < message.size()) {
        std::size_t key_position = std::string_view::npos;
        std::size_t value_position = std::string_view::npos;
        for (const std::string_view key : keys) {
            std::size_t position = lowered.find(key, search_from);
            while (position != std::string::npos) {
                const bool left_boundary = position == 0
                    || !key_character(message[position - 1]);
                std::size_t separator = position + key.size();
                while (separator < message.size() && message[separator] == ' ') {
                    ++separator;
                }
                if (left_boundary && separator < message.size()
                    && (message[separator] == '=' || message[separator] == ':')) {
                    if (position < key_position) {
                        key_position = position;
                        value_position = separator + 1;
                        while (value_position < message.size()
                            && message[value_position] == ' ') {
                            ++value_position;
                        }
                    }
                    break;
                }
                position = lowered.find(key, position + 1);
            }
        }
        if (key_position == std::string_view::npos) {
            break;
        }

        result.append(message.substr(copied_until, value_position - copied_until));
        result.append("<redacted>");
        std::size_t value_end = value_position;
        while (value_end < message.size() && message[value_end] != '\n'
            && message[value_end] != '\r' && message[value_end] != ' '
            && message[value_end] != '\t' && message[value_end] != ';'
            && message[value_end] != ',' && message[value_end] != '&') {
            ++value_end;
        }
        copied_until = value_end;
        search_from = value_end;
    }
    result.append(message.substr(copied_until));
    return result;
}

Log::Log(std::filesystem::path path)
    : path_(std::move(path)) {
    if (path_.empty()) {
        return;
    }

    const auto parent = path_.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) {
            throw std::runtime_error("cannot create log directory '"
                + parent.string() + "': " + error.message());
        }
    }
    file_.open(path_, std::ios::out | std::ios::trunc);
    if (!file_) {
        throw std::runtime_error("cannot open log file: " + path_.string());
    }
}

void Log::info(std::string_view message) {
    write(std::cout, message);
}

void Log::error(std::string_view message) {
    write(std::cerr, message);
}

void Log::write(std::ostream& console, std::string_view message) {
    const std::string safe = redact_sensitive(message);
    std::size_t line_start = 0;
    while (true) {
        const std::size_t line_end = safe.find('\n', line_start);
        const std::string_view line(safe.data() + line_start,
            (line_end == std::string::npos ? safe.size() : line_end) - line_start);
        console << line << '\n';
        console.flush();
        if (file_.is_open()) {
            file_ << line << '\n';
            file_.flush();
            if (!file_) {
                throw std::runtime_error("cannot write log file: " + path_.string());
            }
        }
        if (line_end == std::string::npos) {
            break;
        }
        line_start = line_end + 1;
    }
}

} // namespace dhsh
