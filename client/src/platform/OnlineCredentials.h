#pragma once

#include "network/GameSession.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace dhsh {

// In-memory online-chat credentials collected by the modal dialog. All user
// text lives only in process memory: nothing is read from credential files,
// nothing is accepted on the command line, and no field is written to logs.
// 2026-11-24: no role name / role code here anymore -- the role is chosen at
// runtime from the server-published 0x2000 records (promptOnlineRole) and
// the old 0x1002 "role code" slot carries the graph-key input.
struct OnlineCredentials {
    std::string account;        // UTF-8
    std::string password;       // UTF-8; masked in the dialog, never logged
    std::string zoneName;       // UTF-8
    std::string versionToken;   // ASCII; default "801111111,2-6-2,9-11,"
    std::string securityCode;   // ASCII; exactly 6 alphanumeric, default "000000"

    void clear() noexcept;  // best-effort in-memory zeroing of every field
};

// Byte limits of the evidenced wire slots (see GameSession::start). The
// dialog caps wide characters at these numbers: a wchar count never exceeds
// its GBK byte count, so the cap is always inside the wire slot.
inline constexpr std::size_t kMaxAccountGbkBytes = 51;
inline constexpr std::size_t kMaxPasswordGbkBytes = 20;
inline constexpr std::size_t kMaxZoneGbkBytes = 25;
inline constexpr std::size_t kMaxVersionBytes = 35;

// Fixed system facts feeding the 0x1500 environment report. The Win32
// collector runs on the UI thread at startup; unit tests inject synthetic
// values instead of touching the real machine.
struct SystemInfoSnapshot {
    std::string windowsVersion;  // UTF-8
    std::uint64_t memoryBytes = 0;
    unsigned cpuCount = 0;
    std::string cpuName;  // UTF-8
    // An explicit generic fallback when no reliable GPU name source answers;
    // a real model name is never fabricated.
    std::string gpuName;
};

// Builds the 0x1500 environment report CSV (GBK) whose text starts at wire
// +0x0e. The evidenced field set is account / local time / Windows version /
// memory / CPU / GPU (docs/formats/net-protocol.md); this replication emits
// one CSV row in exactly that order, with the CPU count and name as two
// columns. Returns false with a sanitized error (field names only, never
// values) when a Unicode field cannot be represented in GBK or a required
// field is empty; the caller must not start the online session then.
bool buildEnvironmentReportGbk(const OnlineCredentials& credentials,
    const SystemInfoSnapshot& system, const std::string& localTimeUtf8,
    std::string* reportGbk, std::string* error);

// Formats six raw adapter bytes as the lowercase "aa:bb:cc:dd:ee:ff" string
// that the login-blob MAC slot carries.
bool formatMacAddress(const std::uint8_t bytes[6], std::string* out);

// First hardware MAC of the machine via GetAdaptersInfo (iphlpapi). Returns
// false with a diagnostic when no usable adapter exists.
bool readFirstMacAddress(std::string* out, std::string* error);

// Collects the real system snapshot (registry values, GlobalMemoryStatusEx,
// GetSystemInfo). GPU falls back to an explicit generic name on any failure.
SystemInfoSnapshot collectSystemInfo();

// Validates the credentials and assembles the wire-facing SessionCredentials:
// account/password/zone convert UTF-8 -> GBK (conversion failure or slot
// overflow is an error), version/security must be printable ASCII inside
// the evidenced sizes, macAddress must be the formatted hex string, and the
// environment report must already be GBK. Errors name the offending field
// only; no credential value is copied into `error`. Role material is not
// part of this contract anymore: the session takes the role from the
// runtime selectRole() choice and the graph key from submitGraphKey().
bool buildSessionCredentials(const OnlineCredentials& credentials,
    const std::string& macAddress, const std::string& environmentReportGbk,
    SessionCredentials* out, std::string* error);

// Formats the current local time as "YYYY-MM-DD HH:MM:SS" (UTF-8).
std::string currentLocalTimeUtf8();

// Modal Win32 credentials dialog parented to `owner` (nullable). Returns
// false when the user cancels; `out` is only filled on success. Interactive
// only -- exercised by live acceptance, never by unit tests.
bool promptOnlineCredentials(HWND owner, OnlineCredentials* out);

// Non-interactive structural check of the dialog template: creates the
// dialog modelessly (running its WM_INITDIALOG defaults/limit setup) and
// destroys it without showing anything to a user. Lets unit tests catch a
// malformed hand-packed DLGTEMPLATE without any interaction.
bool validateOnlineCredentialsDialog();

} // namespace dhsh
