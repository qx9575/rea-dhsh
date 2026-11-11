#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace dhsh {

enum class ResourceOutcome {
    success,
    cache,
    skipped,
    failure,
};

struct ResourceCounts {
    std::uint64_t success = 0;
    std::uint64_t cache = 0;
    std::uint64_t skipped = 0;
    std::uint64_t failure = 0;
};

class StartupReport {
public:
    StartupReport(std::filesystem::path shdata, std::string map);

    void record(ResourceOutcome outcome, std::size_t count = 1) noexcept;
    void set_fatal_error(std::string error);

    const ResourceCounts& resources() const noexcept {
        return resources_;
    }

    int exit_code() const noexcept {
        return fatal_error_.has_value() ? 1 : 0;
    }

    std::string format() const;

private:
    std::filesystem::path shdata_;
    std::string map_;
    ResourceCounts resources_;
    std::optional<std::string> fatal_error_;
};

} // namespace dhsh
