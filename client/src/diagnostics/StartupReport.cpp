#include "diagnostics/StartupReport.h"

#include "diagnostics/Log.h"

#include <sstream>
#include <utility>

namespace dhsh {

StartupReport::StartupReport(std::filesystem::path shdata, std::string map)
    : shdata_(std::move(shdata)), map_(std::move(map)) {
}

void StartupReport::record(ResourceOutcome outcome, std::size_t count) noexcept {
    std::uint64_t* destination = nullptr;
    switch (outcome) {
    case ResourceOutcome::success:
        destination = &resources_.success;
        break;
    case ResourceOutcome::cache:
        destination = &resources_.cache;
        break;
    case ResourceOutcome::skipped:
        destination = &resources_.skipped;
        break;
    case ResourceOutcome::failure:
        destination = &resources_.failure;
        break;
    }
    if (destination != nullptr) {
        *destination += static_cast<std::uint64_t>(count);
    }
}

void StartupReport::set_fatal_error(std::string error) {
    fatal_error_ = std::move(error);
}

std::string StartupReport::format() const {
    std::ostringstream output;
    output << "Startup report\n"
           << "shdata=" << shdata_.generic_string() << '\n'
           << "map=" << map_ << '\n'
           << "resources success=" << resources_.success
           << " cache=" << resources_.cache
           << " skipped=" << resources_.skipped
           << " failure=" << resources_.failure << '\n'
           << "fatal_error="
           << redact_sensitive(fatal_error_.value_or("none")) << '\n'
           << "exit_code=" << exit_code();
    return output.str();
}

} // namespace dhsh
