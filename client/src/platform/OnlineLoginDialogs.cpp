#include "platform/OnlineLoginDialogs.h"

#include <cstdio>
#include <utility>
#include <vector>

namespace dhsh {

// Defined below; used by validateOnlineGraphKeyDialog for its pixel
// assertion.
bool probeOnlineGraphKeyDialogPixel(std::uint8_t* r, std::uint8_t* g,
    std::uint8_t* b, const char* evidenceBmpPath);

namespace {

// ---- role-selection dialog ----

constexpr int kIdRoleList = 201;

constexpr int kRoleDialogWidth = 252;
constexpr int kRoleDialogHeight = 172;
constexpr int kRoleListX = 8;
constexpr int kRoleListY = 22;
constexpr int kRoleListWidth = kRoleDialogWidth - 2 * kRoleListX;
constexpr int kRoleListHeight = 116;
constexpr int kRoleButtonWidth = 50;
constexpr int kRoleButtonHeight = 14;
constexpr int kRoleButtonY = kRoleListY + kRoleListHeight + 8;

// ---- graph-key dialog ----

constexpr int kIdGraphImage = 301;
constexpr int kIdGraphKeyEdit = 302;

constexpr int kGraphDialogWidth = 320;
constexpr int kGraphImageX = 12;
constexpr int kGraphImageY = 10;
// Sized from the decoded image at kGraphKeyDisplayScale in WM_INITDIALOG.
constexpr int kGraphEditWidth = 200;
constexpr int kGraphEditHeight = 14;
constexpr int kGraphButtonWidth = 50;
constexpr int kGraphButtonHeight = 14;

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

void appendListBox(DialogTemplate* t, int x, int y, int width, int height,
    int id) {
    t->align();
    t->u32(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_BORDER
        | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT);
    t->u32(0);
    t->i16(static_cast<std::int16_t>(x));
    t->i16(static_cast<std::int16_t>(y));
    t->i16(static_cast<std::int16_t>(width));
    t->i16(static_cast<std::int16_t>(height));
    t->u16(static_cast<std::uint16_t>(id));
    t->atomClass(0x0083);  // "LISTBOX"
    t->text(L"");
    t->u16(0);
}

void appendEtchedFrame(DialogTemplate* t, int x, int y, int width, int height,
    int id) {
    t->align();
    t->u32(WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME);
    t->u32(0);
    t->i16(static_cast<std::int16_t>(x));
    t->i16(static_cast<std::int16_t>(y));
    t->i16(static_cast<std::int16_t>(width));
    t->i16(static_cast<std::int16_t>(height));
    t->u16(static_cast<std::uint16_t>(id));
    t->atomClass(0x0082);  // "STATIC"
    t->text(L"");
    t->u16(0);
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

// Fills the list box from `roles` and returns false (after a sanitized
// message) when the list is empty or any name fails the exact GBK -> UTF-16
// conversion. Item data carries the role id for the OK path.
bool fillRoleList(HWND dialog, const std::vector<RoleRecord>& roles) {
    if (roles.empty()) {
        MessageBoxW(dialog, L"没有可选择的角色。", L"在线聊天登录",
            MB_ICONWARNING | MB_OK);
        return false;
    }
    const HWND list = GetDlgItem(dialog, kIdRoleList);
    for (const RoleRecord& role : roles) {
        std::wstring name;
        // Displaying a record under a wrong name is worse than refusing the
        // dialog, so a conversion failure fails the whole list.
        if (!roleNameGbkToWide(role.nameGbk, &name)) {
            MessageBoxW(dialog,
                L"角色名编码错误，无法显示角色列表。", L"在线聊天登录",
                MB_ICONWARNING | MB_OK);
            return false;
        }
        const std::wstring row =
            name + L"（ID " + std::to_wstring(role.id) + L"）";
        const LRESULT index = SendMessageW(list, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(row.c_str()));
        if (index == LB_ERR || index == LB_ERRSPACE) {
            MessageBoxW(dialog,
                L"角色列表显示失败。", L"在线聊天登录", MB_ICONWARNING | MB_OK);
            return false;
        }
        SendMessageW(list, LB_SETITEMDATA,
            static_cast<WPARAM>(index), static_cast<LPARAM>(role.id));
    }
    return true;
}

struct RoleDialogData {
    const std::vector<RoleRecord>* roles = nullptr;
    std::uint32_t* out = nullptr;
};

INT_PTR CALLBACK onlineRoleDialogProc(
    HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dialog, DWLP_USER, static_cast<LONG_PTR>(lParam));
        // OK stays disabled until an explicit LBN_SELCHANGE: no automatic
        // first-role default.
        EnableWindow(GetDlgItem(dialog, IDOK), FALSE);
        const auto* data = reinterpret_cast<const RoleDialogData*>(lParam);
        if (data == nullptr || data->roles == nullptr
            || !fillRoleList(dialog, *data->roles)) {
            EndDialog(dialog, 2);  // nothing selectable: refuse, not cancel
        }
        return TRUE;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (id == kIdRoleList && code == LBN_SELCHANGE) {
            const LRESULT selected = SendDlgItemMessageW(dialog, kIdRoleList,
                LB_GETCURSEL, 0, 0);
            EnableWindow(GetDlgItem(dialog, IDOK), selected != LB_ERR);
            return TRUE;
        }
        if (id == IDOK) {
            const LRESULT selected = SendDlgItemMessageW(dialog, kIdRoleList,
                LB_GETCURSEL, 0, 0);
            if (selected == LB_ERR) {
                return TRUE;  // nothing explicitly selected
            }
            const LRESULT roleId = SendDlgItemMessageW(dialog, kIdRoleList,
                LB_GETITEMDATA, static_cast<WPARAM>(selected), 0);
            // DWLP_USER carries the whole RoleDialogData struct (stored in
            // WM_INITDIALOG); the id goes through data->out, never through a
            // mis-cast of the struct pointer itself.
            auto* data = reinterpret_cast<RoleDialogData*>(
                GetWindowLongPtrW(dialog, DWLP_USER));
            if (data != nullptr && data->out != nullptr) {
                *data->out = static_cast<std::uint32_t>(roleId);
            }
            EndDialog(dialog, 1);
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(dialog, 0);
            return TRUE;
        }
        return FALSE;
    }
    default:
        return FALSE;
    }
}

void buildRoleDialogTemplate(DialogTemplate* t) {
    t->u32(DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    t->u32(0);
    t->u16(4);  // label + listbox + 2 buttons
    t->i16(0);
    t->i16(0);
    t->i16(kRoleDialogWidth);
    t->i16(kRoleDialogHeight);
    t->u16(0);  // menu
    t->u16(0);  // window class
    t->text(L"在线聊天 - 选择角色");
    t->u16(9);  // point size
    t->text(L"MS Shell Dlg");
    appendStatic(t, kRoleListX, 6, kRoleListWidth, 12, 200,
        L"该账号有以下角色，请明确选择要登录的角色：");
    appendListBox(t, kRoleListX, kRoleListY, kRoleListWidth, kRoleListHeight,
        kIdRoleList);
    appendButton(t, kRoleDialogWidth - 2 * kRoleButtonWidth - 16, kRoleButtonY,
        kRoleButtonWidth, kRoleButtonHeight, IDOK, L"确定", true);
    appendButton(t, kRoleDialogWidth - kRoleButtonWidth - 8, kRoleButtonY,
        kRoleButtonWidth, kRoleButtonHeight, IDCANCEL, L"取消", false);
}

// ---- graph-key dialog ----

// Distinctive SYNTH image color for the pixel probe: far from any dialog
// background, so a false-green render can never match by accident.
constexpr std::uint8_t kProbeRed = 7;
constexpr std::uint8_t kProbeGreen = 200;
constexpr std::uint8_t kProbeBlue = 89;

struct GraphKeyDialogData {
    const GraphKeyImage* image = nullptr;
    std::string* out = nullptr;
};

bool graphKeyImageWellFormed(const GraphKeyImage* image) {
    return image != nullptr && image->width > 0 && image->height > 0
        && image->rgba.size()
            >= static_cast<std::size_t>(image->width)
                * static_cast<std::size_t>(image->height) * 4u;
}

void paintGraphKeyImage(HWND dialog, HDC dc) {
    auto* data = reinterpret_cast<const GraphKeyDialogData*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    if (data == nullptr || !graphKeyImageWellFormed(data->image)) {
        return;
    }
    const GraphKeyImage& image = *data->image;
    const HWND picture = GetDlgItem(dialog, kIdGraphImage);
    if (picture == nullptr) {
        return;
    }
    RECT pictureRect{};
    GetWindowRect(picture, &pictureRect);
    MapWindowPoints(HWND_DESKTOP, dialog,
        reinterpret_cast<POINT*>(&pictureRect), 2);
    // Inset by the etched frame border.
    const int inset = 2;
    const int destX = pictureRect.left + inset;
    const int destY = pictureRect.top + inset;
    const int destWidth =
        (pictureRect.right - pictureRect.left) - 2 * inset;
    const int destHeight =
        (pictureRect.bottom - pictureRect.top) - 2 * inset;
    if (destWidth <= 0 || destHeight <= 0) {
        return;
    }
    // The decoded pixels are R,G,B,A bytes per pixel. With BI_BITFIELDS on
    // a little-endian target the byte order maps to masks R=0x000000FF,
    // G=0x0000FF00, B=0x00FF0000; a top-down bitmap uses a negative height.
    // BITMAPV4HEADER carries the masks inline -- a plain BITMAPINFO has only
    // one bmiColors DWORD, so writing three masks there is a stack overrun.
    BITMAPV4HEADER header{};
    header.bV4Size = sizeof(header);
    header.bV4Width = image.width;
    header.bV4Height = -static_cast<LONG>(image.height);
    header.bV4Planes = 1;
    header.bV4BitCount = 32;
    header.bV4V4Compression = BI_BITFIELDS;
    header.bV4RedMask = 0x000000ffu;
    header.bV4GreenMask = 0x0000ff00u;
    header.bV4BlueMask = 0x00ff0000u;
    StretchDIBits(dc, destX, destY, destWidth, destHeight, 0, 0, image.width,
        image.height, image.rgba.data(),
        reinterpret_cast<const BITMAPINFO*>(&header), DIB_RGB_COLORS, SRCCOPY);
}

INT_PTR CALLBACK onlineGraphKeyDialogProc(
    HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        auto* data = reinterpret_cast<const GraphKeyDialogData*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, static_cast<LONG_PTR>(lParam));
        // Size the picture control from the decoded image so the template
        // geometry always matches what WM_PAINT draws.
        if (data != nullptr && graphKeyImageWellFormed(data->image)) {
            const int width =
                static_cast<int>(data->image->width) * kGraphKeyDisplayScale;
            const int height =
                static_cast<int>(data->image->height) * kGraphKeyDisplayScale;
            SetWindowPos(GetDlgItem(dialog, kIdGraphImage), nullptr,
                kGraphImageX, kGraphImageY, width, height,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        SendDlgItemMessageW(dialog, kIdGraphKeyEdit, EM_SETLIMITTEXT,
            kMaxGraphKeyChars, 0);
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(dialog, &paint);
        paintGraphKeyImage(dialog, dc);
        EndPaint(dialog, &paint);
        return TRUE;
    }
    case WM_PRINTCLIENT:
        // PrintWindow-based capture (used by the unit-test pixel probe)
        // renders client content through WM_PRINTCLIENT instead of
        // WM_PAINT; draw the same decoded image.
        paintGraphKeyImage(dialog, reinterpret_cast<HDC>(wParam));
        return TRUE;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == IDOK) {
            auto* data = reinterpret_cast<const GraphKeyDialogData*>(
                GetWindowLongPtrW(dialog, DWLP_USER));
            wchar_t buffer[kMaxGraphKeyChars + 2] = {};
            if (GetDlgItemTextW(dialog, kIdGraphKeyEdit, buffer,
                    static_cast<int>(sizeof(buffer) / sizeof(buffer[0])))
                <= 0) {
                // Empty input is refused: the 0x1002 graph-key slot must not
                // go out blank.
                MessageBoxW(dialog, L"请输入图形验证码。",
                    L"在线聊天登录", MB_ICONWARNING | MB_OK);
                return TRUE;
            }
            if (data != nullptr && data->out != nullptr) {
                wideToUtf8(buffer, data->out);
            }
            EndDialog(dialog, 1);
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(dialog, 0);
            return TRUE;
        }
        return FALSE;
    }
    default:
        return FALSE;
    }
}

int graphKeyDialogHeight() {
    // Layout below the picture control: input row + buttons.
    return kGraphImageY + 32 * kGraphKeyDisplayScale + 8 + kGraphEditHeight
        + 10 + kGraphButtonHeight + 12;
}

void buildGraphKeyDialogTemplate(DialogTemplate* t) {
    t->u32(DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    t->u32(0);
    t->u16(5);  // prompt + picture frame + edit + 2 buttons
    t->i16(0);
    t->i16(0);
    t->i16(kGraphDialogWidth);
    t->i16(graphKeyDialogHeight());
    t->u16(0);  // menu
    t->u16(0);  // window class
    t->text(L"在线聊天 - 图形验证码");
    t->u16(9);  // point size
    t->text(L"MS Shell Dlg");
    const int editY = kGraphImageY + 32 * kGraphKeyDisplayScale + 8;
    appendStatic(t, kGraphImageX, editY + 1, 60, 12, 303,
        L"验证码(&C)：");
    // The etched frame marks the image region; WM_PAINT draws the decoded
    // pixels inside it (inset by the border).
    appendEtchedFrame(t, kGraphImageX, kGraphImageY,
        96 * kGraphKeyDisplayScale, 32 * kGraphKeyDisplayScale, kIdGraphImage);
    appendEdit(t, kGraphImageX + 64, editY, kGraphEditWidth, kGraphEditHeight,
        kIdGraphKeyEdit, L"", false);
    appendButton(t,
        kGraphDialogWidth - 2 * kGraphButtonWidth - 16,
        editY + kGraphEditHeight + 10, kGraphButtonWidth, kGraphButtonHeight,
        IDOK, L"确定", true);
    appendButton(t,
        kGraphDialogWidth - kGraphButtonWidth - 8,
        editY + kGraphEditHeight + 10, kGraphButtonWidth, kGraphButtonHeight,
        IDCANCEL, L"取消", false);
}

// Writes a bottom-up 32bpp BI_RGB BMP (B,G,R,A byte order, `bits` exactly as
// a positive-height DIB section holds them). The 14-byte file header is
// written field by field because BITMAPFILEHEADER's padded sizeof is 16.
bool saveBmp32(const std::uint8_t* bits, int width, int height,
    const char* path) {
    if (bits == nullptr || width <= 0 || height <= 0 || path == nullptr) {
        return false;
    }
    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || file == nullptr) {
        return false;
    }
    auto byte = [&](std::uint8_t value) {
        std::fputc(static_cast<int>(value), file);
    };
    auto u16le = [&](std::uint16_t value) {
        byte(static_cast<std::uint8_t>(value & 0xffu));
        byte(static_cast<std::uint8_t>((value >> 8) & 0xffu));
    };
    auto u32le = [&](std::uint32_t value) {
        u16le(static_cast<std::uint16_t>(value & 0xffffu));
        u16le(static_cast<std::uint16_t>((value >> 16) & 0xffffu));
    };
    byte('B');
    byte('M');
    const std::uint32_t pixelBytes =
        static_cast<std::uint32_t>(width) * static_cast<std::uint32_t>(height)
        * 4u;
    const std::uint32_t headerBytes = 14u + sizeof(BITMAPINFOHEADER);
    u32le(headerBytes + pixelBytes);   // bfSize
    u32le(0);                          // bfReserved1/2
    u32le(headerBytes);                // bfOffBits
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = height;  // positive: bottom-up, same order as `bits`
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = pixelBytes;
    std::fwrite(&info, 1, sizeof(info), file);
    std::fwrite(bits, 1, pixelBytes, file);
    std::fclose(file);
    return true;
}

} // namespace

bool roleNameGbkToWide(std::string_view gbk, std::wstring* wide) {
    if (wide == nullptr) {
        return false;
    }
    wide->clear();
    if (gbk.empty()) {
        return false;
    }
    const int sourceLength = static_cast<int>(gbk.size());
    const int wideCount = MultiByteToWideChar(936, MB_ERR_INVALID_CHARS,
        gbk.data(), sourceLength, nullptr, 0);
    if (wideCount <= 0) {
        return false;
    }
    wide->assign(static_cast<std::size_t>(wideCount), L'\0');
    return MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, gbk.data(),
        sourceLength, wide->data(), wideCount) == wideCount;
}

bool promptOnlineRole(HWND owner, const std::vector<RoleRecord>& roles,
                      std::uint32_t* selectedRoleId) {
    if (selectedRoleId == nullptr || roles.empty()) {
        return false;
    }
    DialogTemplate t;
    buildRoleDialogTemplate(&t);
    std::uint32_t chosen = 0;
    RoleDialogData data{&roles, &chosen};
    const INT_PTR result = DialogBoxIndirectParamW(
        GetModuleHandleW(nullptr),
        reinterpret_cast<LPCDLGTEMPLATEW>(const_cast<std::uint8_t*>(t.data())),
        owner, onlineRoleDialogProc, reinterpret_cast<LPARAM>(&data));
    if (result != 1) {
        return false;  // cancel or dialog failure
    }
    *selectedRoleId = chosen;
    return true;
}

bool promptOnlineGraphKey(HWND owner, const GraphKeyImage& image,
                          std::string* graphKey) {
    if (graphKey == nullptr || !graphKeyImageWellFormed(&image)) {
        return false;
    }
    DialogTemplate t;
    buildGraphKeyDialogTemplate(&t);
    GraphKeyDialogData data{&image, graphKey};
    const INT_PTR result = DialogBoxIndirectParamW(
        GetModuleHandleW(nullptr),
        reinterpret_cast<LPCDLGTEMPLATEW>(const_cast<std::uint8_t*>(t.data())),
        owner, onlineGraphKeyDialogProc, reinterpret_cast<LPARAM>(&data));
    if (result != 1) {
        graphKey->clear();  // cancel or dialog failure must leave no residue
        return false;
    }
    return true;
}

bool validateOnlineRoleDialog() {
    // Fixed SYNTH list: one GBK Chinese name (exact bytes for "角色") plus a
    // second role, so the check covers multi-role display and the exact
    // GBK -> UTF-16 path at once.
    const std::vector<RoleRecord> roles = {
        RoleRecord{0x85b, std::string("\xBD\xC7\xC9\xAB", 4)},
        RoleRecord{42, "SynthTwo"},
    };
    DialogTemplate t;
    buildRoleDialogTemplate(&t);
    std::uint32_t ignored = 0;
    RoleDialogData data{&roles, &ignored};
    HWND dialog = CreateDialogIndirectParamW(
        GetModuleHandleW(nullptr),
        reinterpret_cast<LPCDLGTEMPLATEW>(const_cast<std::uint8_t*>(t.data())),
        nullptr, onlineRoleDialogProc, reinterpret_cast<LPARAM>(&data));
    if (dialog == nullptr) {
        return false;
    }
    bool ok = true;
    const HWND list = GetDlgItem(dialog, kIdRoleList);
    ok = ok && list != nullptr;
    if (list != nullptr) {
        ok = ok && SendMessageW(list, LB_GETCOUNT, 0, 0) == 2;
        ok = ok && SendMessageW(list, LB_GETCURSEL, 0, 0) == LB_ERR;
        ok = ok && SendMessageW(list, LB_GETITEMDATA, 0, 0) == 0x85b;
        ok = ok && SendMessageW(list, LB_GETITEMDATA, 1, 0) == 42;
        // The visible row must be the exact UTF-16 of the GBK name plus the
        // id suffix -- a wrong-codepage name would fail this comparison.
        std::wstring expected;
        ok = ok && roleNameGbkToWide(roles[0].nameGbk, &expected);
        expected += L"（ID " + std::to_wstring(roles[0].id) + L"）";
        const LRESULT length = SendMessageW(list, LB_GETTEXTLEN, 0, 0);
        ok = ok && length == static_cast<LRESULT>(expected.size());
        if (ok && length > 0) {
            std::wstring actual(static_cast<std::size_t>(length), L'\0');
            SendMessageW(list, LB_GETTEXT, 0,
                reinterpret_cast<LPARAM>(actual.data()));
            ok = ok && actual == expected;
        }
        // Explicit selection is what enables OK: drive the same notification
        // the user's click sends and watch the button follow, then deselect
        // and watch it return to disabled.
        const HWND okButton = GetDlgItem(dialog, IDOK);
        ok = ok && !IsWindowEnabled(okButton);
        SendMessageW(list, LB_SETCURSEL, 1, 0);
        SendMessageW(dialog, WM_COMMAND,
            MAKEWPARAM(kIdRoleList, LBN_SELCHANGE),
            reinterpret_cast<LPARAM>(list));
        ok = ok && IsWindowEnabled(okButton);
        SendMessageW(list, LB_SETCURSEL, -1, 0);
        SendMessageW(dialog, WM_COMMAND,
            MAKEWPARAM(kIdRoleList, LBN_SELCHANGE),
            reinterpret_cast<LPARAM>(list));
        ok = ok && !IsWindowEnabled(okButton);
        // Drive the actual OK path after an explicit choice: the proc must
        // write the SELECTED item's id through RoleDialogData::out (second
        // role -> 42 -- never 0 and never the first item's 0x85b) and must
        // leave the dialog-data struct itself intact (a mis-cast would
        // clobber the roles pointer and leave the output slot at 0).
        SendMessageW(list, LB_SETCURSEL, 1, 0);
        SendMessageW(dialog, WM_COMMAND,
            MAKEWPARAM(kIdRoleList, LBN_SELCHANGE),
            reinterpret_cast<LPARAM>(list));
        SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDOK, 0), 0);
        ok = ok && ignored == 42;
        ok = ok && data.roles == &roles;
    }
    // The OK drive ends the dialog; only destroy when it is still a window.
    if (IsWindow(dialog)) {
        DestroyWindow(dialog);
    }
    return ok;
}

bool validateOnlineGraphKeyDialog() {
    // Fixed SYNTH image at the evidenced 96x32 shape.
    GraphKeyImage image;
    image.width = 96;
    image.height = 32;
    image.rgba.assign(96u * 32u * 4u, 255);
    DialogTemplate t;
    buildGraphKeyDialogTemplate(&t);
    std::string submitted;
    GraphKeyDialogData data{&image, &submitted};
    HWND dialog = CreateDialogIndirectParamW(
        GetModuleHandleW(nullptr),
        reinterpret_cast<LPCDLGTEMPLATEW>(const_cast<std::uint8_t*>(t.data())),
        nullptr, onlineGraphKeyDialogProc, reinterpret_cast<LPARAM>(&data));
    if (dialog == nullptr) {
        return false;
    }
    bool ok = true;
    const HWND picture = GetDlgItem(dialog, kIdGraphImage);
    ok = ok && picture != nullptr;
    if (picture != nullptr) {
        RECT rect{};
        GetWindowRect(picture, &rect);
        ok = ok
            && (rect.right - rect.left)
                == 96 * kGraphKeyDisplayScale
            && (rect.bottom - rect.top) == 32 * kGraphKeyDisplayScale;
    }
    // WM_INITDIALOG applied the evidenced 7-character cap.
    ok = ok && SendDlgItemMessageW(dialog, kIdGraphKeyEdit, EM_GETLIMITTEXT, 0,
               0) == kMaxGraphKeyChars;
    wchar_t text[kMaxGraphKeyChars + 2] = {};
    ok = ok && GetDlgItemTextW(dialog, kIdGraphKeyEdit, text,
               static_cast<int>(sizeof(text) / sizeof(text[0]))) == 0;
    // Drive the actual OK path: the typed text must arrive in the output
    // string exactly, proving the GraphKeyDialogData round trip is intact.
    SetDlgItemTextW(dialog, kIdGraphKeyEdit, L"aB3");
    SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDOK, 0), 0);
    ok = ok && submitted == "aB3";
    if (IsWindow(dialog)) {
        DestroyWindow(dialog);
    }
    // Geometry cannot prove the decoded image is actually visible: a child
    // control painting its background after the parent's WM_PAINT would
    // overpaint it while every size check still passes. Read back the
    // rendered center pixel.
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    ok = ok && probeOnlineGraphKeyDialogPixel(&r, &g, &b, nullptr);
    ok = ok && r == kProbeRed && g == kProbeGreen && b == kProbeBlue;
    return ok;
}

bool probeOnlineGraphKeyDialogPixel(std::uint8_t* r, std::uint8_t* g,
    std::uint8_t* b, const char* evidenceBmpPath) {
    if (r == nullptr || g == nullptr || b == nullptr) {
        return false;
    }
    *r = 0;
    *g = 0;
    *b = 0;
    // Fixed SYNTH image in the distinctive probe color.
    GraphKeyImage image;
    image.width = 96;
    image.height = 32;
    image.rgba.resize(96u * 32u * 4u);
    for (std::size_t index = 0; index < 96u * 32u; ++index) {
        image.rgba[index * 4u] = kProbeRed;
        image.rgba[index * 4u + 1u] = kProbeGreen;
        image.rgba[index * 4u + 2u] = kProbeBlue;
        image.rgba[index * 4u + 3u] = 255;
    }
    DialogTemplate t;
    buildGraphKeyDialogTemplate(&t);
    GraphKeyDialogData data{&image, nullptr};
    HWND dialog = CreateDialogIndirectParamW(
        GetModuleHandleW(nullptr),
        reinterpret_cast<LPCDLGTEMPLATEW>(const_cast<std::uint8_t*>(t.data())),
        nullptr, onlineGraphKeyDialogProc, reinterpret_cast<LPARAM>(&data));
    if (dialog == nullptr) {
        return false;
    }
    // Show far offscreen (no flash) and force a synchronous paint. The
    // capture itself uses PrintWindow, whose default WM_PRINT handler runs
    // in the real screen order (non-client, erase, client via
    // WM_PRINTCLIENT, then children) -- so the result shows exactly what a
    // user sees, including any child overpainting of the parent-drawn
    // image. (A never-composed offscreen window exposes no readable bits to
    // screen-DC or window-DC BitBlt, and PW_RENDERFULLCONTENT snapshots the
    // empty redirection surface -- both were tried and rejected.)
    SetWindowPos(dialog, nullptr, -2000, -2000, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RedrawWindow(dialog, nullptr, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    RECT clientRect{};
    GetClientRect(dialog, &clientRect);
    const int width = clientRect.right - clientRect.left;
    const int height = clientRect.bottom - clientRect.top;
    const HWND picture = GetDlgItem(dialog, kIdGraphImage);
    RECT pictureRect{};
    GetWindowRect(picture, &pictureRect);
    MapWindowPoints(HWND_DESKTOP, dialog,
        reinterpret_cast<POINT*>(&pictureRect), 2);
    if (width <= 0 || height <= 0) {
        DestroyWindow(dialog);
        return false;
    }
    // Render the whole client area into a 32bpp DIB as evidence.
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height;  // positive: bottom-up rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC memory = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits,
        nullptr, 0);
    const bool ready = memory != nullptr && dib != nullptr
        && bits != nullptr;
    bool rendered = false;
    if (ready) {
        HBITMAP previous = static_cast<HBITMAP>(SelectObject(memory, dib));
        // Classic WM_PRINT-based rendering: the default WM_PRINT handler
        // recurses into the standard child controls in screen order, so the
        // capture shows exactly what a user sees.
        if (PrintWindow(dialog, memory, 0) != 0) {
            rendered = true;
            // Bottom-up B,G,R,A rows: the picture-area center pixel the
            // user sees.
            const int centerX = pictureRect.left
                + (pictureRect.right - pictureRect.left) / 2;
            const int centerY = pictureRect.top
                + (pictureRect.bottom - pictureRect.top) / 2;
            const std::uint8_t* row =
                static_cast<const std::uint8_t*>(bits)
                + static_cast<std::size_t>(height - 1 - centerY)
                    * static_cast<std::size_t>(width) * 4u;
            *b = row[centerX * 4];
            *g = row[centerX * 4 + 1];
            *r = row[centerX * 4 + 2];
            if (evidenceBmpPath != nullptr) {
                saveBmp32(static_cast<const std::uint8_t*>(bits), width,
                    height, evidenceBmpPath);
            }
        }
        SelectObject(memory, previous);
    }
    if (dib != nullptr) {
        DeleteObject(dib);
    }
    if (memory != nullptr) {
        DeleteDC(memory);
    }
    DestroyWindow(dialog);
    return rendered;
}

} // namespace dhsh
