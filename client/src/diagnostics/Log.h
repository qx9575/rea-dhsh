#pragma once

#include <filesystem>
#include <fstream>
#include <iosfwd>
#include <string>
#include <string_view>

namespace dhsh {

std::string redact_sensitive(std::string_view message);

class Log {
public:
    explicit Log(std::filesystem::path path = {});

    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    void info(std::string_view message);
    void error(std::string_view message);

private:
    void write(std::ostream& console, std::string_view message);

    std::filesystem::path path_;
    std::ofstream file_;
};

} // namespace dhsh
