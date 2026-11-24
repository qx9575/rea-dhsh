#include "platform/OnlineCredentials.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <utility>
#include <vector>

namespace dhsh {
namespace {

// Best-effort in-memory zeroing of a string's byte buffer before release.
void zeroString(std::string* value) noexcept {
    if (value == nullptr) {
        return;
    }
    if (!value->empty()) {
        volatile std::uint8_t* bytes =
            reinterpret_cast<volatile std::uint8_t*>(value->data());
        for (std::size_t index = 0; index < value->size(); ++index) {
            bytes[index] = 0;
        }
    }
    value->clear();
}

// Strict UTF-8 -> GBK(CP936) conversion. Fails (returns false with an empty
// output) on invalid UTF-8 or on any character GBK cannot represent, so a
// wrong byte sequence never reaches the wire.
bool utf8ToGbk(std::string_view utf8, std::string* gbk) {
    gbk->clear();
    if (utf8.empty()) {
        return true;
    }
    const int sourceLength = static_cast<int>(utf8.size());
    const int wideCount = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8.data(), sourceLength, nullptr, 0);
    if (wideCount <= 0) {
        return false;
    }
    std::wstring wide(static_cast<std::size_t>(wideCount), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
            sourceLength, wide.data(), wideCount) != wideCount) {
        return false;
    }
    BOOL usedDefault = FALSE;
    const int gbkCount = WideCharToMultiByte(936,
        WC_NO_BEST_FIT_CHARS, wide.data(), wideCount,
        nullptr, 0, "?", &usedDefault);
    if (gbkCount <= 0) {
        return false;
    }
    gbk->assign(static_cast<std::size_t>(gbkCount), '\0');
    usedDefault = FALSE;
    if (WideCharToMultiByte(936, WC_NO_BEST_FIT_CHARS, wide.data(), wideCount,
            gbk->data(), gbkCount, "?", &usedDefault) != gbkCount) {
        gbk->clear();
        return false;
    }
    if (usedDefault) {
        gbk->clear();
        return false;
    }
    return true;
}

bool wideToUtf8(std::wstring_view wide, std::string* utf8) {
    utf8->clear();
    if (wide.empty()) {
        return true;
    }
    const int sourceLength = static_cast<int>(wide.size());
    const int utf8Count = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
        sourceLength, nullptr, 0, nullptr, nullptr);
    if (utf8Count <= 0) {
        return false;
    }
    utf8->assign(static_cast<std::size_t>(utf8Count), '\0');
    return WideCharToMultiByte(CP_UTF8, 0, wide.data(), sourceLength,
        utf8->data(), utf8Count, nullptr, nullptr) == utf8Count;
}

// CSV shape guard: commas/CR/LF inside provider text would split a field, so
// they are replaced with spaces. GBK trail bytes never hold these values
// (trail range 0x40..0xfe), so a byte scan is safe.
std::string sanitizeCsvField(std::string value) {
    for (char& character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == ',' || byte == '\r' || byte == '\n') {
            character = ' ';
        }
    }
    return value;
}

bool isPrintableAscii(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(),
        [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte >= 0x21 && byte <= 0x7e;
        });
}

bool isLowerHexDigit(char character) {
    return (character >= '0' && character <= '9')
        || (character >= 'a' && character <= 'f');
}

bool isFormattedMac(const std::string& mac) {
    if (mac.size() != 17) {
        return false;
    }
    for (std::size_t index = 0; index < mac.size(); ++index) {
        if ((index + 1) % 3 == 0) {
            if (mac[index] != ':') {
                return false;
            }
        } else if (!isLowerHexDigit(mac[index])) {
            return false;
        }
    }
    return true;
}

// ---- Win32 system-info collectors (live only; tests inject fixed data) ----

std::wstring registryString(
    HKEY root, const wchar_t* subKey, const wchar_t* valueName) {
    HKEY key = nullptr;
    std::wstring value;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ | KEY_WOW64_64KEY, &key)
            == ERROR_SUCCESS) {
        wchar_t buffer[256] = {};
        DWORD byteCount = sizeof(buffer);
        DWORD type = 0;
        if (RegQueryValueExW(key, valueName, nullptr, &type,
                reinterpret_cast<LPBYTE>(buffer), &byteCount) == ERROR_SUCCESS
            && (type == REG_SZ || type == REG_EXPAND_SZ)) {
            buffer[(sizeof(buffer) / sizeof(buffer[0])) - 1] = L'\0';
            value.assign(buffer);
        }
        RegCloseKey(key);
    }
    return value;
}

void trimWide(std::wstring* value) {
    const auto notSpace = [](wchar_t character) {
        return character != L' ' && character != L'\t';
    };
    const auto begin = std::find_if(value->begin(), value->end(), notSpace);
    const auto end = std::find_if(value->rbegin(), value->rend(), notSpace).base();
    value->assign(begin < end ? std::wstring(begin, end) : std::wstring());
}

// ---- Modal credentials dialog (interactive only) ----

// Field ids; labels/defaults follow the task spec: zone defaults to the
// evidenced 超梦幻, version to 801111111,2-6-2,9-11,, security to 000000.
constexpr int kIdAccount = 101;
constexpr int kIdPassword = 102;
constexpr int kIdZone = 103;
constexpr int kIdVersion = 104;
constexpr int kIdSecurity = 105;
constexpr int kIdRoleName = 106;
constexpr int kIdRoleCode = 107;

constexpr int kDialogWidth = 286;
constexpr int kLabelX = 8;
constexpr int kLabelWidth = 80;
constexpr int kEditX = 92;
constexpr int kEditWidth = 186;
constexpr int kRowHeight = 16;
constexpr int kFirstRowY = 6;
constexpr int kRowCount = 7;
constexpr int kButtonY = kFirstRowY + kRowCount * kRowHeight;
constexpr int kButtonWidth = 50;
constexpr int kButtonHeight = 13;
constexpr int kDialogHeight = kButtonY + kButtonHeight + 8;

// Byte-builder for an in-memory DLGTEMPLATE. Every member is DWORD aligned
// exactly as DialogBoxIndirectParamW requires.
class DialogTemplate {
public:
    void align() {
        while (data_.size() % sizeof(std::uint32_t) != 0) {
            data_.push_back(0);
        }
    }

    void u16(std::uint16_t value) {
        data_.push_back(static_cast<std::uint8_t>(value & 0xffu));
        data_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
    }

    void i16(std::int16_t value) {
        u16(static_cast<std::uint16_t>(value));
    }

    void u32(std::uint32_t value) {
        u16(static_cast<std::uint16_t>(value & 0xffffu));
        u16(static_cast<std::uint16_t>((value >> 16) & 0xffffu));
    }

    void text(const wchar_t* value) {
        do {
            u16(static_cast<std::uint16_t>(*value));
        } while (*value++ != L'\0');
    }

    void atomClass(std::uint16_t atom) {
        u16(0xffffu);
        u16(atom);
    }

    const std::uint8_t* data() const { return data_.data(); }

private:
    std::vector<std::uint8_t> data_;
};

void appendStatic(DialogTemplate* t, int x, int y, int width, int height,
    int id, const wchar_t* caption) {
    t->align();
    t->u32(WS_CHILD | WS_VISIBLE | SS_LEFT);  // style
    t->u32(0);                                 // dwExtendedStyle
    t->i16(static_cast<std::int16_t>(x));
    t->i16(static_cast<std::int16_t>(y));
    t->i16(static_cast<std::int16_t>(width));
    t->i16(static_cast<std::int16_t>(height));
    t->u16(static_cast<std::uint16_t>(id));
    t->atomClass(0x0082);  // "STATIC"
    t->text(caption);
    t->u16(0);  // no extra data
}

void appendEdit(DialogTemplate* t, int x, int y, int width, int height,
    int id, const wchar_t* caption, bool password) {
    t->align();
    t->u32(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_LEFT
        | ES_AUTOHSCROLL | (password ? ES_PASSWORD : 0));
    t->u32(0);
    t->i16(static_cast<std::int16_t>(x));
    t->i16(static_cast<std::int16_t>(y));
    t->i16(static_cast<std::int16_t>(width));
    t->i16(static_cast<std::int16_t>(height));
    t->u16(static_cast<std::uint16_t>(id));
    t->atomClass(0x0081);  // "EDIT"
    t->text(caption);
    t->u16(0);
}

void appendButton(DialogTemplate* t, int x, int y, int width, int height,
    int id, const wchar_t* caption, bool defaultButton) {
    t->align();
    t->u32(WS_CHILD | WS_VISIBLE | WS_TABSTOP
        | (defaultButton ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON));
    t->u32(0);
    t->i16(static_cast<std::int16_t>(x));
    t->i16(static_cast<std::int16_t>(y));
    t->i16(static_cast<std::int16_t>(width));
    t->i16(static_cast<std::int16_t>(height));
    t->u16(static_cast<std::uint16_t>(id));
    t->atomClass(0x0080);  // "BUTTON"
    t->text(caption);
    t->u16(0);
}

bool readFieldUtf8(HWND dialog, int id, int limit, std::string* utf8) {
    wchar_t buffer[128] = {};
    if (GetDlgItemTextW(dialog, id, buffer,
            static_cast<int>(sizeof(buffer) / sizeof(buffer[0]))) > limit) {
        return false;
    }
    return wideToUtf8(buffer, utf8);
}

// Dialog-side validation is intentionally shallow (presence + security code
// shape); byte-level wire checks run in buildSessionCredentials so every
// rejection path names a field only, never a value.
bool collectDialogCredentials(HWND dialog, OnlineCredentials* out) {
    OnlineCredentials collected;
    std::string error;
    if (!readFieldUtf8(dialog, kIdAccount, static_cast<int>(kMaxAccountGbkBytes),
            &collected.account)
        || !readFieldUtf8(dialog, kIdPassword,
            static_cast<int>(kMaxPasswordGbkBytes), &collected.password)
        || !readFieldUtf8(dialog, kIdZone, static_cast<int>(kMaxZoneGbkBytes),
            &collected.zoneName)
        || !readFieldUtf8(dialog, kIdVersion, static_cast<int>(kMaxVersionBytes),
            &collected.versionToken)
        || !readFieldUtf8(dialog, kIdSecurity, 6, &collected.securityCode)
        || !readFieldUtf8(dialog, kIdRoleName,
            static_cast<int>(kMaxRoleNameGbkBytes), &collected.roleName)
        || !readFieldUtf8(dialog, kIdRoleCode,
            static_cast<int>(kMaxRoleCodeBytes), &collected.roleCode)) {
        error = "A field exceeds its input limit.";
    } else if (collected.account.empty() || collected.password.empty()
        || collected.zoneName.empty() || collected.roleName.empty()
        || collected.roleCode.empty()) {
        error = "Account, password, zone, role name, and role code are all "
                "required (no automatic role list is available).";
    } else if (collected.versionToken.empty()) {
        error = "The version token must not be empty.";
    } else if (collected.securityCode.size() != 6
        || !std::all_of(collected.securityCode.begin(),
            collected.securityCode.end(), [](char character) {
                return std::isalnum(static_cast<unsigned char>(character)) != 0;
            })) {
        error = "The security code must be exactly 6 letters or digits.";
    }
    if (!error.empty()) {
        MessageBoxW(dialog,
            L"账号、密码、区服、版本、角色名、角色代码均不能为空；安全码必须为6位字母或数字。",
            L"在线聊天登录", MB_ICONWARNING | MB_OK);
        collected.clear();
        return false;
    }
    *out = std::move(collected);
    return true;
}

INT_PTR CALLBACK onlineCredentialsDialogProc(
    HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dialog, DWLP_USER, static_cast<LONG_PTR>(lParam));
        SetDlgItemTextW(dialog, kIdZone, L"超梦幻");
        SetDlgItemTextW(dialog, kIdVersion, L"801111111,2-6-2,9-11,");
        SetDlgItemTextW(dialog, kIdSecurity, L"000000");
        const struct {
            int id;
            int limit;
        } limits[] = {
            {kIdAccount, static_cast<int>(kMaxAccountGbkBytes)},
            {kIdPassword, static_cast<int>(kMaxPasswordGbkBytes)},
            {kIdZone, static_cast<int>(kMaxZoneGbkBytes)},
            {kIdVersion, static_cast<int>(kMaxVersionBytes)},
            {kIdSecurity, 6},
            {kIdRoleName, static_cast<int>(kMaxRoleNameGbkBytes)},
            {kIdRoleCode, static_cast<int>(kMaxRoleCodeBytes)},
        };
        for (const auto& field : limits) {
            SendDlgItemMessageW(dialog, field.id, EM_SETLIMITTEXT, field.limit, 0);
        }
        return TRUE;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == IDOK || id == IDCANCEL) {
            auto* out = reinterpret_cast<OnlineCredentials*>(
                GetWindowLongPtrW(dialog, DWLP_USER));
            if (id == IDOK && out != nullptr
                && collectDialogCredentials(dialog, out)) {
                EndDialog(dialog, 1);
            } else if (id == IDCANCEL) {
                EndDialog(dialog, 0);
            }
            return TRUE;
        }
        return FALSE;
    }
    default:
        return FALSE;
    }
}

void appendCredentialsDialogControls(DialogTemplate* t) {
    const struct {
        const wchar_t* label;
        int id;
        bool password;
    } rows[] = {
        {L"账号(&A)：", kIdAccount, false},
        {L"密码(&P)：", kIdPassword, true},
        {L"区服(&Z)：", kIdZone, false},
        {L"版本(&V)：", kIdVersion, false},
        {L"安全码(&S)：", kIdSecurity, false},
        {L"角色名(&R)：", kIdRoleName, false},
        {L"角色代码(&C)：", kIdRoleCode, false},
    };
    for (int index = 0; index < kRowCount; ++index) {
        const int rowY = kFirstRowY + index * kRowHeight;
        appendStatic(t, kLabelX, rowY, kLabelWidth, kRowHeight - 4,
            1000 + index, rows[index].label);
        appendEdit(t, kEditX, rowY - 1, kEditWidth, kRowHeight - 3,
            rows[index].id, L"", rows[index].password);
    }
    appendButton(t, kEditX + kEditWidth - 2 * kButtonWidth - 6, kButtonY,
        kButtonWidth, kButtonHeight, IDOK, L"确定", true);
    appendButton(t, kEditX + kEditWidth - kButtonWidth, kButtonY,
        kButtonWidth, kButtonHeight, IDCANCEL, L"取消", false);
}

// Builds the in-memory DLGTEMPLATE for the credentials dialog.
void buildCredentialsDialogTemplate(DialogTemplate* t) {
    t->u32(DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    t->u32(0);
    t->u16(16);  // 7 labels + 7 edits + 2 buttons
    t->i16(0);
    t->i16(0);
    t->i16(kDialogWidth);
    t->i16(kDialogHeight);
    t->u16(0);  // menu
    t->u16(0);  // window class
    t->text(L"在线聊天登录（仅在线聊天，非完整世界）");
    t->u16(9);  // point size
    t->text(L"MS Shell Dlg");
    appendCredentialsDialogControls(t);
}

} // namespace

void OnlineCredentials::clear() noexcept {
    zeroString(&account);
    zeroString(&password);
    zeroString(&zoneName);
    zeroString(&versionToken);
    zeroString(&securityCode);
    zeroString(&roleName);
    zeroString(&roleCode);
}

bool buildEnvironmentReportGbk(const OnlineCredentials& credentials,
    const SystemInfoSnapshot& system, const std::string& localTimeUtf8,
    std::string* reportGbk, std::string* error) {
    if (reportGbk == nullptr || error == nullptr) {
        return false;
    }
    reportGbk->clear();
    error->clear();
    std::string accountGbk;
    if (credentials.account.empty()) {
        *error = "environment report: account is empty";
        return false;
    }
    if (!utf8ToGbk(credentials.account, &accountGbk)) {
        *error = "environment report: account is not representable in GBK";
        return false;
    }
    if (localTimeUtf8.empty()) {
        *error = "environment report: local time is empty";
        return false;
    }
    // The documented field set is account/time/Windows/memory/CPU/GPU; the
    // provider texts fall back to explicit generic names instead of ever
    // inventing a real model or version. Every text field is validated as
    // UTF-8 and converted to GBK here -- mixing encodings would corrupt the
    // report -- and an unrepresentable character fails the whole build.
    const struct {
        const char* name;
        std::string_view fallback;
        std::string_view utf8;
    } fields[] = {
        {"local time", {}, localTimeUtf8},
        {"Windows version", "Windows (unknown version)", system.windowsVersion},
        {"CPU name", "Generic CPU", system.cpuName},
        {"GPU name", "Generic Display Adapter", system.gpuName},
    };
    std::string timeGbk;
    std::string windowsGbk;
    std::string cpuNameGbk;
    std::string gpuNameGbk;
    std::string* const gbkSlots[] = {
        &timeGbk, &windowsGbk, &cpuNameGbk, &gpuNameGbk};
    for (std::size_t index = 0; index < std::size(fields); ++index) {
        std::string_view utf8 = fields[index].utf8;
        if (utf8.empty()) {
            utf8 = fields[index].fallback;
        }
        if (utf8.empty()) {
            *error = std::string("environment report: ") + fields[index].name
                + " is empty";
            return false;
        }
        if (!utf8ToGbk(utf8, gbkSlots[index])) {
            *error = std::string("environment report: ") + fields[index].name
                + " is not representable in GBK";
            return false;
        }
    }
    const std::string memoryMb =
        std::to_string(system.memoryBytes / (1024ull * 1024ull));
    std::ostringstream csv;
    csv << accountGbk << ',' << sanitizeCsvField(timeGbk) << ','
        << sanitizeCsvField(windowsGbk) << ',' << memoryMb << ','
        << system.cpuCount << ',' << sanitizeCsvField(cpuNameGbk) << ','
        << sanitizeCsvField(gpuNameGbk);
    *reportGbk = csv.str();
    return true;
}

bool formatMacAddress(const std::uint8_t bytes[6], std::string* out) {
    if (bytes == nullptr || out == nullptr) {
        return false;
    }
    char text[18] = {};
    std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x",
        static_cast<unsigned>(bytes[0]), static_cast<unsigned>(bytes[1]),
        static_cast<unsigned>(bytes[2]), static_cast<unsigned>(bytes[3]),
        static_cast<unsigned>(bytes[4]), static_cast<unsigned>(bytes[5]));
    *out = text;
    return true;
}

bool readFirstMacAddress(std::string* out, std::string* error) {
    if (out == nullptr || error == nullptr) {
        return false;
    }
    out->clear();
    error->clear();
    ULONG bufferLength = 16384;
    std::vector<std::uint8_t> buffer(bufferLength);
    DWORD result = GetAdaptersInfo(
        reinterpret_cast<PIP_ADAPTER_INFO>(buffer.data()), &bufferLength);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufferLength);
        result = GetAdaptersInfo(
            reinterpret_cast<PIP_ADAPTER_INFO>(buffer.data()), &bufferLength);
    }
    if (result != NO_ERROR) {
        *error = "cannot enumerate network adapters (GetAdaptersInfo failed)";
        return false;
    }
    for (auto* adapter = reinterpret_cast<PIP_ADAPTER_INFO>(buffer.data());
         adapter != nullptr; adapter = adapter->Next) {
        if (adapter->AddressLength < 6) {
            continue;
        }
        const bool allZero = std::all_of(adapter->Address, adapter->Address + 6,
            [](std::uint8_t byte) { return byte == 0; });
        if (allZero) {
            continue;  // a zeroed hardware address says nothing about the host
        }
        return formatMacAddress(adapter->Address, out);
    }
    *error = "no network adapter with a hardware MAC address was found";
    return false;
}

SystemInfoSnapshot collectSystemInfo() {
    SystemInfoSnapshot system;
    const std::wstring productName = registryString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
    if (productName.empty()
        || !wideToUtf8(productName, &system.windowsVersion)) {
        system.windowsVersion = "Windows (unknown version)";
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory) != 0) {
        system.memoryBytes = memory.ullTotalPhys;
    }
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    system.cpuCount = info.dwNumberOfProcessors;
    std::wstring cpuName = registryString(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"ProcessorNameString");
    trimWide(&cpuName);
    if (cpuName.empty()
        || !wideToUtf8(cpuName, &system.cpuName)) {
        system.cpuName = "Generic CPU";
    }
    const std::wstring gpuName = registryString(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Class"
        L"\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000",
        L"DriverDesc");
    if (gpuName.empty() || !wideToUtf8(gpuName, &system.gpuName)) {
        // No reliable GPU name source answered: use the explicit generic
        // fallback instead of guessing a model.
        system.gpuName = "Generic Display Adapter";
    }
    return system;
}

bool buildSessionCredentials(const OnlineCredentials& credentials,
    const std::string& macAddress, const std::string& environmentReportGbk,
    SessionCredentials* out, std::string* error) {
    if (out == nullptr || error == nullptr) {
        return false;
    }
    *out = SessionCredentials{};
    error->clear();

    std::string accountGbk;
    if (credentials.account.empty()) {
        *error = "account is empty";
        return false;
    }
    if (!utf8ToGbk(credentials.account, &accountGbk)) {
        *error = "account is not representable in GBK";
        return false;
    }
    if (accountGbk.size() > kMaxAccountGbkBytes) {
        *error = "account exceeds its wire slot";
        return false;
    }
    // The same account bytes are embedded verbatim in the 0x1500 environment
    // report CSV; a comma or line break would split its column there and
    // desynchronize the 0x1000/0x1500 identity. Fail closed instead of
    // silently rewriting the account (GBK encodes ASCII verbatim, so a byte
    // scan on the converted form is exact).
    if (accountGbk.find(',') != std::string::npos
        || accountGbk.find('\r') != std::string::npos
        || accountGbk.find('\n') != std::string::npos) {
        *error = "account contains a comma or line break, which the "
                 "environment report CSV cannot carry";
        return false;
    }

    std::string passwordGbk;
    if (credentials.password.empty()) {
        *error = "password is empty";
        return false;
    }
    if (!utf8ToGbk(credentials.password, &passwordGbk)) {
        *error = "password is not representable in GBK";
        return false;
    }
    if (passwordGbk.size() > kMaxPasswordGbkBytes) {
        *error = "password exceeds its wire slot";
        return false;
    }

    std::string zoneGbk;
    if (credentials.zoneName.empty()) {
        *error = "zone name is empty";
        return false;
    }
    if (!utf8ToGbk(credentials.zoneName, &zoneGbk)) {
        *error = "zone name is not representable in GBK";
        return false;
    }
    if (zoneGbk.size() > kMaxZoneGbkBytes) {
        *error = "zone name exceeds its wire slot";
        return false;
    }

    if (!isPrintableAscii(credentials.versionToken)
        || credentials.versionToken.size() > kMaxVersionBytes) {
        *error = "version token is empty or not printable ASCII within its wire slot";
        return false;
    }
    if (credentials.securityCode.size() != 6
        || !std::all_of(credentials.securityCode.begin(),
            credentials.securityCode.end(), [](char character) {
                return std::isalnum(static_cast<unsigned char>(character)) != 0;
            })) {
        *error = "security code must be exactly 6 alphanumeric characters";
        return false;
    }

    std::string roleNameGbk;
    if (credentials.roleName.empty()) {
        *error = "role name is empty (no automatic role list is available)";
        return false;
    }
    if (!utf8ToGbk(credentials.roleName, &roleNameGbk)) {
        *error = "role name is not representable in GBK";
        return false;
    }
    if (roleNameGbk.size() > kMaxRoleNameGbkBytes
        || roleNameGbk.find('\0') != std::string::npos) {
        *error = "role name exceeds its wire slot";
        return false;
    }
    if (!isPrintableAscii(credentials.roleCode)
        || credentials.roleCode.size() > kMaxRoleCodeBytes) {
        *error = "role code is empty or not printable ASCII within its wire slot";
        return false;
    }
    if (!isFormattedMac(macAddress)) {
        *error = "MAC address is not in aa:bb:cc:dd:ee:ff form";
        return false;
    }
    if (environmentReportGbk.empty()) {
        *error = "environment report is empty";
        return false;
    }

    out->account = std::move(accountGbk);
    out->password = std::move(passwordGbk);
    out->zoneNameGbk = std::move(zoneGbk);
    out->versionToken = credentials.versionToken;
    out->securityCode = credentials.securityCode;
    out->macAddress = macAddress;
    out->environmentReportGbk = environmentReportGbk;
    out->roleNameGbk = std::move(roleNameGbk);
    out->roleCode = credentials.roleCode;
    return true;
}

std::string currentLocalTimeUtf8() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    char text[32] = {};
    if (localtime_s(&local, &now) != 0
        || std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local) == 0) {
        return {};
    }
    return text;
}

bool promptOnlineCredentials(HWND owner, OnlineCredentials* out) {
    if (out == nullptr) {
        return false;
    }
    DialogTemplate t;
    buildCredentialsDialogTemplate(&t);
    const INT_PTR result = DialogBoxIndirectParamW(
        GetModuleHandleW(nullptr),
        reinterpret_cast<LPCDLGTEMPLATEW>(const_cast<std::uint8_t*>(t.data())),
        owner, onlineCredentialsDialogProc, reinterpret_cast<LPARAM>(out));
    if (result != 1) {
        out->clear();  // cancel or dialog failure must leave no residue
        return false;
    }
    return true;
}

bool validateOnlineCredentialsDialog() {
    DialogTemplate t;
    buildCredentialsDialogTemplate(&t);
    OnlineCredentials ignored{};
    HWND dialog = CreateDialogIndirectParamW(
        GetModuleHandleW(nullptr),
        reinterpret_cast<LPCDLGTEMPLATEW>(const_cast<std::uint8_t*>(t.data())),
        nullptr, onlineCredentialsDialogProc,
        reinterpret_cast<LPARAM>(&ignored));
    if (dialog == nullptr) {
        return false;
    }
    // WM_INITDIALOG ran: the dialog-side defaults must actually be in place.
    wchar_t zone[64] = {};
    GetDlgItemTextW(dialog, kIdZone, zone,
        static_cast<int>(sizeof(zone) / sizeof(zone[0])));
    const bool zoneDefaultOk = std::wstring(zone) == L"超梦幻";
    wchar_t version[64] = {};
    GetDlgItemTextW(dialog, kIdVersion, version,
        static_cast<int>(sizeof(version) / sizeof(version[0])));
    const bool versionDefaultOk =
        std::wstring(version) == L"801111111,2-6-2,9-11,";
    wchar_t security[16] = {};
    GetDlgItemTextW(dialog, kIdSecurity, security,
        static_cast<int>(sizeof(security) / sizeof(security[0])));
    const bool securityDefaultOk = std::wstring(security) == L"000000";
    DestroyWindow(dialog);
    return zoneDefaultOk && versionDefaultOk && securityDefaultOk;
}

} // namespace dhsh
