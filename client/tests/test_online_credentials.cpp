// Non-interactive unit tests for the --online option surface and the online
// credentials/environment-report builders. Everything here is SYNTH: fixed
// credential strings and a fixed SystemInfoSnapshot provider; no real
// credentials are read, no machine fact is collected, and the modal dialog
// (promptOnlineCredentials) plus the real Win32 collectors stay live-only.
#include "app/Options.h"
#include "platform/OnlineCredentials.h"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& name) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}

std::string gbkToUtf8(const std::string& gbk) {
    if (gbk.empty()) {
        return {};
    }
    const int wideCount = MultiByteToWideChar(
        936, 0, gbk.data(), static_cast<int>(gbk.size()), nullptr, 0);
    if (wideCount <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(wideCount), L'\0');
    MultiByteToWideChar(936, 0, gbk.data(), static_cast<int>(gbk.size()),
        wide.data(), wideCount);
    const int utf8Count = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
        wideCount, nullptr, 0, nullptr, nullptr);
    if (utf8Count <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(utf8Count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideCount,
        utf8.data(), utf8Count, nullptr, nullptr);
    return utf8;
}

std::vector<std::string> splitCsv(const std::string& row) {
    std::vector<std::string> fields;
    std::string field;
    std::istringstream input(row);
    while (std::getline(input, field, ',')) {
        fields.push_back(field);
    }
    return fields;
}

dhsh::OnlineCredentials synthCredentials() {
    dhsh::OnlineCredentials credentials;
    credentials.account = "tester01";
    credentials.password = "s3cret!pass";
    credentials.zoneName = "SynthZone";
    credentials.versionToken = "801111111,2-6-2,9-11,";
    credentials.securityCode = "000000";
    credentials.roleName = "SynthRole";
    credentials.roleCode = "1234567";
    return credentials;
}

dhsh::SystemInfoSnapshot synthSystem() {
    dhsh::SystemInfoSnapshot system;
    system.windowsVersion = "Windows 10 Synth";
    system.memoryBytes = 16ull * 1024ull * 1024ull * 1024ull;
    system.cpuCount = 8;
    system.cpuName = "Synth CPU 3000";
    system.gpuName = "Generic Display Adapter";
    return system;
}

void runOptionsTests() {
    using dhsh::Options;

    {
        const char* argv[] = {"dhsh_client", "--shdata", "shdata"};
        const Options options = dhsh::parse_options(3, const_cast<char**>(argv));
        expect(!options.online, "online defaults to false");
        expect(options.replay.empty(), "replay defaults to empty");
        expect(options.map == "YingTianFu", "map default unchanged");
    }
    {
        const char* argv[] = {
            "dhsh_client", "--shdata", "shdata", "--online"};
        const Options options = dhsh::parse_options(4, const_cast<char**>(argv));
        expect(options.online, "--online sets online");
    }
    bool threw = false;
    try {
        const char* argv[] = {
            "dhsh_client", "--shdata", "shdata", "--online", "--replay", "r.dhr1"};
        (void)dhsh::parse_options(6, const_cast<char**>(argv));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "--online --replay is rejected");

    threw = false;
    try {
        const char* argv[] = {
            "dhsh_client", "--shdata", "shdata", "--replay", "r.dhr1", "--online"};
        (void)dhsh::parse_options(6, const_cast<char**>(argv));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "--replay --online is rejected (order independent)");

    threw = false;
    try {
        const char* argv[] = {"dhsh_client", "--online"};
        (void)dhsh::parse_options(2, const_cast<char**>(argv));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "--online still requires --shdata");

    {
        const char* argv[] = {"dhsh_client", "--help"};
        const Options options = dhsh::parse_options(2, const_cast<char**>(argv));
        expect(options.help, "--help unaffected by online mode");
    }
    {
        std::ostringstream usage;
        dhsh::print_usage(usage);
        expect(usage.str().find("--online") != std::string::npos,
            "usage documents --online");
        expect(usage.str().find("101.42.255.186") == std::string::npos,
            "usage does not print the gateway default (not a credential)");
    }
    expect(std::string(dhsh::kDefaultOnlineGatewayHost) == "101.42.255.186",
        "default gateway host is the evidenced endpoint");
    expect(dhsh::kDefaultOnlineGatewayPort == 8000,
        "default gateway port is the evidenced endpoint");
}

void runEnvironmentReportTests() {
    using dhsh::OnlineCredentials;
    using dhsh::SystemInfoSnapshot;

    {
        OnlineCredentials credentials = synthCredentials();
        SystemInfoSnapshot system = synthSystem();
        std::string report;
        std::string error;
        expect(dhsh::buildEnvironmentReportGbk(credentials, system,
                "2026-11-23 12:00:00", &report, &error),
            "environment report builds for SYNTH input");
        expect(error.empty(), "environment report success carries no error");
        const std::string decoded = gbkToUtf8(report);
        const std::vector<std::string> fields = splitCsv(decoded);
        expect(fields.size() == 7, "environment report has 7 CSV fields");
        if (fields.size() == 7) {
            expect(fields[0] == "tester01", "field 0 is the account");
            expect(fields[1] == "2026-11-23 12:00:00", "field 1 is local time");
            expect(fields[2] == "Windows 10 Synth", "field 2 is Windows version");
            expect(fields[3] == "16384", "field 3 is memory in MB");
            expect(fields[4] == "8", "field 4 is the CPU count");
            expect(fields[5] == "Synth CPU 3000", "field 5 is the CPU name");
            expect(fields[6] == "Generic Display Adapter", "field 6 is the GPU");
        }
    }
    {
        // Chinese account/zone text must round-trip through GBK so the wire
        // payload carries the original characters.
        OnlineCredentials credentials = synthCredentials();
        credentials.account = "测试账号";
        SystemInfoSnapshot system = synthSystem();
        system.cpuName = "合成处理器";
        std::string report;
        std::string error;
        expect(dhsh::buildEnvironmentReportGbk(credentials, system,
                "2026-11-23 12:00:00", &report, &error),
            "environment report builds with Chinese account");
        const std::string decoded = gbkToUtf8(report);
        expect(decoded.rfind("测试账号,", 0) == 0,
            "Chinese account survives the GBK round trip");
        expect(decoded.find("合成处理器") != std::string::npos,
            "Chinese CPU name survives the GBK round trip");
    }
    {
        // U+1F600 has no GBK mapping (unlike hiragana, which GBK includes):
        // the build must fail closed with a field-naming error instead of
        // emitting a replacement character.
        OnlineCredentials credentials = synthCredentials();
        credentials.account = "user\xF0\x9F\x98\x80";
        SystemInfoSnapshot system = synthSystem();
        std::string report;
        std::string error;
        expect(!dhsh::buildEnvironmentReportGbk(credentials, system,
                "2026-11-23 12:00:00", &report, &error),
            "unrepresentable account fails the environment report");
        expect(!error.empty(), "failure reports a diagnostic");
        expect(report.empty(), "failure leaves the report empty");
        expect(error.find("あ") == std::string::npos,
            "error never copies the offending value");
    }
    {
        OnlineCredentials credentials = synthCredentials();
        credentials.account.clear();
        SystemInfoSnapshot system = synthSystem();
        std::string report;
        std::string error;
        expect(!dhsh::buildEnvironmentReportGbk(credentials, system,
                "2026-11-23 12:00:00", &report, &error),
            "empty account fails the environment report");
    }
    {
        // Commas inside provider text would corrupt the CSV; the builder
        // must neutralize them while keeping the 7-column shape.
        OnlineCredentials credentials = synthCredentials();
        SystemInfoSnapshot system = synthSystem();
        system.cpuName = "Synth,CPU";
        system.windowsVersion = "Windows,10";
        std::string report;
        std::string error;
        expect(dhsh::buildEnvironmentReportGbk(credentials, system,
                "2026-11-23 12:00:00", &report, &error),
            "environment report sanitizes commas");
        expect(splitCsv(gbkToUtf8(report)).size() == 7,
            "sanitized report keeps 7 CSV fields");
    }
}

void runMacTests() {
    const std::uint8_t bytes[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    std::string formatted;
    expect(dhsh::formatMacAddress(bytes, &formatted), "MAC formats");
    expect(formatted == "aa:bb:cc:dd:ee:ff", "MAC is lowercase colon hex");
    const std::uint8_t zeros[6] = {0, 0, 0, 0, 0, 0};
    expect(dhsh::formatMacAddress(zeros, &formatted), "zero MAC formats");
    expect(formatted == "00:00:00:00:00:00", "zero MAC is all zeros");
    // The modal dialog itself is interactive (live acceptance), but its
    // hand-packed template can be validated without any user interaction.
    expect(dhsh::validateOnlineCredentialsDialog(),
        "credentials dialog template builds and applies its defaults");
}

void runSessionCredentialsTests() {
    using dhsh::OnlineCredentials;

    {
        OnlineCredentials credentials = synthCredentials();
        dhsh::SessionCredentials session;
        std::string error;
        expect(dhsh::buildSessionCredentials(credentials,
                "aa:bb:cc:dd:ee:ff", "acct,time", &session, &error),
            "session credentials build for SYNTH input");
        expect(error.empty(), "session build success carries no error");
        expect(session.account == "tester01", "account passes through (ASCII)");
        expect(session.password == "s3cret!pass", "password passes through (ASCII)");
        expect(session.zoneNameGbk == "SynthZone", "zone passes through (ASCII)");
        expect(session.versionToken == "801111111,2-6-2,9-11,",
            "version token passes through");
        expect(session.securityCode == "000000", "security code passes through");
        expect(session.macAddress == "aa:bb:cc:dd:ee:ff", "MAC passes through");
        expect(session.environmentReportGbk == "acct,time",
            "environment report passes through untouched");
        expect(session.roleNameGbk == "SynthRole", "role name passes through (ASCII)");
        expect(session.roleCode == "1234567", "role code passes through");
    }
    {
        // Zone and role names are user text: they must convert UTF-8 -> GBK
        // and round-trip exactly.
        OnlineCredentials credentials = synthCredentials();
        credentials.zoneName = "超梦幻";
        credentials.roleName = "测试角色";
        dhsh::SessionCredentials session;
        std::string error;
        expect(dhsh::buildSessionCredentials(credentials,
                "aa:bb:cc:dd:ee:ff", "r", &session, &error),
            "Chinese zone/role names convert");
        expect(gbkToUtf8(session.zoneNameGbk) == "超梦幻",
            "zone name survives the GBK round trip");
        expect(gbkToUtf8(session.roleNameGbk) == "测试角色",
            "role name survives the GBK round trip");
    }
    {
        // Field validation must fail closed and never echo values back.
        struct Case {
            const char* name;
            void (*mutate)(OnlineCredentials*);
        };
        const Case cases[] = {
            {"empty account", [](OnlineCredentials* c) { c->account.clear(); }},
            // The account is embedded verbatim in the 0x1500 CSV: a comma or
            // line break would split its column, so the build must reject
            // the credential instead of silently rewriting it.
            {"comma in account", [](OnlineCredentials* c) { c->account = "a,b"; }},
            {"CR in account", [](OnlineCredentials* c) { c->account = "a\rb"; }},
            {"LF in account", [](OnlineCredentials* c) { c->account = "a\nb"; }},
            {"oversized account (52 GBK bytes)",
                [](OnlineCredentials* c) { c->account = "一二三四五六七八九十壹二三四五六七八九十壹二三四五六"; }},
            {"empty password", [](OnlineCredentials* c) { c->password.clear(); }},
            {"empty zone", [](OnlineCredentials* c) { c->zoneName.clear(); }},
            {"empty version", [](OnlineCredentials* c) { c->versionToken.clear(); }},
            {"short security code", [](OnlineCredentials* c) { c->securityCode = "00000"; }},
            {"non-alphanumeric security code",
                [](OnlineCredentials* c) { c->securityCode = "00000!"; }},
            {"empty role name (no automatic list)",
                [](OnlineCredentials* c) { c->roleName.clear(); }},
            {"empty role code", [](OnlineCredentials* c) { c->roleCode.clear(); }},
            {"oversized role code",
                [](OnlineCredentials* c) { c->roleCode = "12345678"; }},
            {"spaced role code",
                [](OnlineCredentials* c) { c->roleCode = "12 4567"; }},
            {"oversized password",
                [](OnlineCredentials* c) { c->password.assign(21, 'p'); }},
            {"oversized role name",
                [](OnlineCredentials* c) { c->roleName = "一二三四五六七八九十壹"; }},
            {"oversized zone",
                [](OnlineCredentials* c) { c->zoneName = "一二三四五六七八九十壹二三"; }},
            {"oversized version",
                [](OnlineCredentials* c) { c->versionToken.assign(36, 'v'); }},
            {"unrepresentable password",
                [](OnlineCredentials* c) { c->password = "pass\xF0\x9F\x98\x80word"; }},
        };
        for (const Case& testCase : cases) {
            OnlineCredentials credentials = synthCredentials();
            testCase.mutate(&credentials);
            dhsh::SessionCredentials session;
            std::string error;
            expect(!dhsh::buildSessionCredentials(credentials,
                    "aa:bb:cc:dd:ee:ff", "r", &session, &error),
                std::string("rejected: ") + testCase.name);
            expect(!error.empty(),
                std::string("rejection names a field: ") + testCase.name);
            const bool echoSafe =
                (credentials.account.empty()
                    || error.find(credentials.account) == std::string::npos)
                && (credentials.password.empty()
                    || error.find(credentials.password) == std::string::npos)
                && (credentials.roleName.empty()
                    || error.find(credentials.roleName) == std::string::npos)
                && (credentials.zoneName.empty()
                    || error.find(credentials.zoneName) == std::string::npos);
            expect(echoSafe,
                std::string("rejection never echoes credential values: ")
                    + testCase.name);
        }
        // Boundary: the evidenced 51-byte account slot is inclusive. A 51
        // GBK-byte account must build, and its bytes must reach the session
        // (the same bytes feed 0x1000 and the 0x1500 CSV) verbatim.
        {
            OnlineCredentials credentials = synthCredentials();
            credentials.account = "一二三四五六七八九十壹二三四五六七八九十壹二三四五x";  // 25*2+1 = 51 GBK bytes
            dhsh::SessionCredentials session;
            std::string error;
            expect(dhsh::buildSessionCredentials(credentials,
                    "aa:bb:cc:dd:ee:ff", "r", &session, &error),
                "51-byte account accepted at the wire slot boundary");
            expect(session.account.size() == 51,
                "boundary account fills the slot exactly");
            expect(gbkToUtf8(session.account) == credentials.account,
                "boundary account bytes pass through unmodified");
        }
    }
    {
        OnlineCredentials credentials = synthCredentials();
        dhsh::SessionCredentials session;
        std::string error;
        expect(!dhsh::buildSessionCredentials(credentials,
                "aa:bb:cc:dd:ee", "r", &session, &error),
            "short MAC rejected");
        expect(!dhsh::buildSessionCredentials(credentials,
                "aa:bb:cc:dd:ee:gg", "r", &session, &error),
            "non-hex MAC rejected");
        expect(!dhsh::buildSessionCredentials(credentials,
                "aa:bb:cc:dd:ee:ff", "", &session, &error),
            "empty environment report rejected");
        expect(!dhsh::buildSessionCredentials(credentials,
                "AA:BB:CC:DD:EE:FF", "", &session, &error),
            "uppercase MAC rejected (wire format is lowercase)");
    }
}

} // namespace

int main() {
    runOptionsTests();
    runEnvironmentReportTests();
    runMacTests();
    runSessionCredentialsTests();
    if (failures != 0) {        std::cerr << failures << " online credential test(s) failed\n";
        return 1;
    }
    std::cout << "online credential tests passed\n";
    return 0;
}
