#include "app/App.h"
#include "diagnostics/StartupReport.h"
#include "diagnostics/Log.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void check_contains(
    std::string_view text, std::string_view expected, const std::string& message) {
    check(text.find(expected) != std::string_view::npos,
        message + ": missing '" + std::string(expected) + "' in:\n" + std::string(text));
}

class TempDir {
public:
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("dhsh_test_startup_report_" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "cannot read test log");
    return std::string(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void test_report_contains_startup_state_and_real_counts() {
    dhsh::StartupReport report("D:/readonly/shdata", "YingTianFu");
    report.record(dhsh::ResourceOutcome::success, 3);
    report.record(dhsh::ResourceOutcome::cache, 2);
    report.record(dhsh::ResourceOutcome::skipped, 1);
    report.record(dhsh::ResourceOutcome::failure, 4);

    const std::string text = report.format();
    check_contains(text, "shdata=D:/readonly/shdata", "shdata path");
    check_contains(text, "map=YingTianFu", "map name");
    check_contains(text, "resources success=3 cache=2 skipped=1 failure=4",
        "resource counts");
    check_contains(text, "fatal_error=none", "empty fatal error");
    check(report.exit_code() == 0, "ordinary resource failures must not be fatal");
}

void test_core_failure_has_nonzero_exit_and_diagnostic() {
    dhsh::StartupReport report("D:/readonly/shdata", "YingTianFu");
    report.record(dhsh::ResourceOutcome::failure);
    check(report.exit_code() == 0, "ordinary failure changed the exit code");

    report.set_fatal_error("core map unavailable");
    check(report.exit_code() != 0, "core failure did not change the exit code");
    check_contains(report.format(), "fatal_error=core map unavailable",
        "fatal diagnostic");
}

void test_log_flushes_each_line_and_redacts_credentials() {
    TempDir dir;
    const auto path = dir.path() / "startup.log";
    {
        dhsh::Log log(path);
        log.info("account=alice password=hunter2 token=network-secret");
        log.info("ordinary resource skipped");

        const std::string text = read_text(path);
        check(text.find("alice") == std::string::npos, "account value was logged");
        check(text.find("hunter2") == std::string::npos, "password value was logged");
        check(text.find("network-secret") == std::string::npos,
            "network credential was logged");
        check_contains(text, "account=<redacted>", "account redaction");
        check_contains(text, "password=<redacted>", "password redaction");
        check_contains(text, "token=<redacted>", "token redaction");
        check_contains(text, "ordinary resource skipped", "flushed second line");
    }

    bool rejected = false;
    try {
        dhsh::Log invalid(dir.path());
    } catch (const std::exception& error) {
        rejected = true;
        check_contains(error.what(), "cannot open log file", "log open diagnostic");
    }
    check(rejected, "opening a directory as a log unexpectedly succeeded");
}

void test_redaction_covers_headers_json_compound_keys_and_tabs() {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"Authorization: Bearer abc123", "abc123"},
        {"Cookie: sess=1; theme=dark", "theme=dark"},
        {"password=\tsecret", "secret"},
        {"password\t=\tsecret", "secret"},
        {"access_token=abc123", "abc123"},
        {"client_secret=def", "def"},
        {"{\"password\":\"hunter2\"}", "hunter2"},
    };
    for (const auto& [message, secret] : cases) {
        const std::string safe = dhsh::redact_sensitive(message);
        check(safe.find(secret) == std::string::npos,
            "credential value survived redaction: " + message + " -> " + safe);
        check_contains(safe, "<redacted>", "redacted credential marker");
    }
}

void test_app_reports_core_failure_and_default_map() {
    TempDir dir;
    const auto missing = dir.path() / "missing-shdata";
    const std::string shdata = missing.generic_string();
    char program[] = "dhsh_client";
    char shdata_option[] = "--shdata";
    std::vector<char> shdata_value(shdata.begin(), shdata.end());
    shdata_value.push_back('\0');
    char* arguments[] = {program, shdata_option, shdata_value.data()};

    std::ostringstream standard_output;
    std::ostringstream standard_error;
    auto* old_output = std::cout.rdbuf(standard_output.rdbuf());
    auto* old_error = std::cerr.rdbuf(standard_error.rdbuf());
    int exit_code = 0;
    try {
        exit_code = dhsh::App{}.run(3, arguments);
    } catch (...) {
        std::cout.rdbuf(old_output);
        std::cerr.rdbuf(old_error);
        throw;
    }
    std::cout.rdbuf(old_output);
    std::cerr.rdbuf(old_error);

    check(exit_code != 0, "missing core resources returned success");
    const std::string diagnostics = standard_output.str() + standard_error.str();
    check_contains(diagnostics, "shdata=" + shdata, "application shdata report");
    check_contains(diagnostics, "map=YingTianFu", "application default map report");
    check(diagnostics.find("fatal_error=none") == std::string::npos,
        "core failure was reported as nonfatal");
    check_contains(diagnostics, "fatal_error=", "application fatal report");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"report contains startup state and counts", test_report_contains_startup_state_and_real_counts},
        {"core failure is fatal and ordinary failure is not", test_core_failure_has_nonzero_exit_and_diagnostic},
        {"log flushes and redacts credentials", test_log_flushes_each_line_and_redacts_credentials},
        {"headers JSON compound keys and tabs are redacted", test_redaction_covers_headers_json_compound_keys_and_tabs},
        {"app reports core failure and default map", test_app_reports_core_failure_and_default_map},
    };

    bool passed = true;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
            passed = false;
        }
    }
    return passed ? 0 : 1;
}
