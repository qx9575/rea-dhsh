#pragma once

#include "network/GameSession.h"
#include "network/GraphKeyDecoder.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dhsh {

// Win32 login-stage dialogs for the --online role/graph-key interaction
// (2026-11-24 design, docs/plans/2026-11-24-online-role-graphkey-design.md).
// Both are modal, main-thread-only APIs fed by OnlineChatWorker events; the
// worker keeps owning the socket in the background. No SCP/Lua UI is
// embedded here. Nothing entered or displayed by these dialogs is ever
// written to logs.

// Edit-box cap of the graph-key input, evidenced by config/ui/login/
// graphkey.scp's 7-character limit; the same bound is the 0x1002 +0x22 wire
// slot. The dialog enforces it with EM_SETLIMITTEXT and empty input is
// refused, so a zero-length graph key can never be submitted.
inline constexpr int kMaxGraphKeyChars = 7;

// The decoded 0x2005 image is 96x32 (docs/formats/net-protocol.md); it is
// drawn scaled by this factor so the characters stay readable.
inline constexpr int kGraphKeyDisplayScale = 3;

// Exact GBK (CP936) -> UTF-16 conversion of one role-record name; the role
// dialog renders exactly this text. Fails (returns false, clears `wide`) on
// empty input or bytes CP936 cannot decode, so a corrupt record can never be
// displayed under a wrong name.
bool roleNameGbkToWide(std::string_view gbk, std::wstring* wide);

// Modal role-selection dialog parented to `owner` (nullable). `roles` is
// the accumulated 0x2000 display list the worker published (never empty in
// practice; an empty list refuses the dialog). Every role shows as
// "name（ID n）" in server order; nothing is pre-selected and OK stays
// disabled until the user explicitly picks a row, so there is no automatic
// first-role default. Returns false when the user cancels or the dialog
// cannot be built; `selectedRoleId` is only written on success and is then
// passed to OnlineChatWorker::selectRole.
bool promptOnlineRole(HWND owner, const std::vector<RoleRecord>& roles,
                      std::uint32_t* selectedRoleId);

// Modal graph-key dialog parented to `owner` (nullable). Draws the decoded
// 0x2005 RGBA image (width*height*4 bytes) scaled by kGraphKeyDisplayScale
// and accepts at most kMaxGraphKeyChars of input; empty input keeps the
// dialog open with a prompt. Returns false when the user cancels;
// `graphKey` only holds the confirmed text on success and is handed straight
// to OnlineChatWorker::submitGraphKey -- it is never logged.
bool promptOnlineGraphKey(HWND owner, const GraphKeyImage& image,
                          std::string* graphKey);

// Non-interactive structural checks of the hand-packed dialog templates:
// each creates the dialog modelessly (running its WM_INITDIALOG setup) with
// fixed SYNTH data and destroys it without showing anything to a user, so
// unit tests can catch a malformed DLGTEMPLATE or a broken limit without any
// interaction. validateOnlineRoleDialog uses a two-role list (one GBK
// Chinese name) and additionally simulates one explicit list-box selection
// to prove it is what enables OK. validateOnlineGraphKeyDialog additionally
// renders the dialog offscreen and reads back the picture-area center pixel,
// proving the decoded image is really visible and not overpainted by a child
// control (geometry alone cannot catch that).
bool validateOnlineRoleDialog();
bool validateOnlineGraphKeyDialog();

// Pixel-level probe backing the graph-key validator. Renders the dialog
// offscreen with a fixed SYNTH image whose center RGB is (7, 200, 89),
// forces a synchronous paint of the parent and its children, and reads back
// the picture-area center pixel exactly where WM_PAINT draws the decoded
// 0x2005 image. Returns false only when the offscreen render failed; the out
// pixels are always filled. When `evidenceBmpPath` is non-null the captured
// picture area is saved there as a 32bpp BMP for manual inspection.
bool probeOnlineGraphKeyDialogPixel(std::uint8_t* r, std::uint8_t* g,
    std::uint8_t* b, const char* evidenceBmpPath);

} // namespace dhsh
