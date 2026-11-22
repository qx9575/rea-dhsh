#!/usr/bin/env python3
"""Task 8 interactive acceptance for the rebuilt detached chat window.

Drives the real dhsh_client (read-only shdata) through the evidenced
interaction flow: open -> filter -> scroll -> pause -> edit -> switch/hide ->
reopen, verifies window geometry/styles/log evidence, and captures client
area screenshots under .rea for the same-size visual comparison against the
original captures. The original game is never attached, injected or closed.
"""

from __future__ import annotations

import ctypes
import json
import struct
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORKSPACE_ROOT = ROOT.parent.parent
CLIENT_EXE = ROOT / "client/build/Release/dhsh_client.exe"
SHDATA = "D:/software/dhsh2/shdata"
BASE_CACHE = WORKSPACE_ROOT / "client/build/map-cache"
SOURCE_REPLAY = ROOT / ".rea/task6_sanitized_chat.dhr1"
SPACED_REPLAY = ROOT / ".rea/task8_chat_spaced.dhr1"
CLIENT_LOG = ROOT / "client/build/task8-acceptance.log"
OUT_JSON = ROOT / ".rea/task8_acceptance.json"

MAIN_CLASS = "dhsh_client_window"
CHAT_CLASS = "dhsh_scp_chat_window"

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
kernel32 = ctypes.windll.kernel32

WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_MOUSEWHEEL = 0x020A
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_CLOSE = 0x0010
VK_RETURN = 0x0D
WS_CAPTION = 0x00C00000
WS_SYSMENU = 0x00080000
WS_THICKFRAME = 0x00040000
WS_EX_WINDOWEDGE = 0x00000100
RDW_INVALIDATE = 0x0001
RDW_UPDATENOW = 0x0100
SRC_COPY = 0x00CC0020

results = []


def record(name: str, ok: bool, detail: str = "") -> None:
    results.append({"check": name, "ok": bool(ok), "detail": detail})
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" -- {detail}" if detail else ""))


def fail(message: str) -> None:
    print(f"[FATAL] {message}")
    sys.exit(2)


def find_window(class_name: str) -> int | None:
    handle = user32.FindWindowExW(None, None, class_name, None)
    return handle or None


def class_window_count(class_name: str) -> int:
    count = 0
    enum_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def callback(handle, _param):
        nonlocal count
        buffer = ctypes.create_unicode_buffer(128)
        if user32.GetClassNameW(handle, buffer, 128) > 0 and buffer.value == class_name:
            count += 1
        return True

    user32.EnumWindows(enum_proc(callback), 0)
    return count


def client_size(handle: int) -> tuple[int, int]:
    rect = wintypes.RECT()
    user32.GetClientRect(handle, ctypes.byref(rect))
    return rect.right - rect.left, rect.bottom - rect.top


def frame_size(handle: int) -> tuple[int, int]:
    rect = wintypes.RECT()
    user32.GetWindowRect(handle, ctypes.byref(rect))
    return rect.right - rect.left, rect.bottom - rect.top


def window_title(handle: int) -> str:
    buffer = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(handle, buffer, 256)
    return buffer.value


def click(handle: int, x: int, y: int) -> None:
    user32.PostMessageW(handle, WM_LBUTTONDOWN, 0x0001, (y << 16) | (x & 0xFFFF))
    user32.PostMessageW(handle, WM_LBUTTONUP, 0, (y << 16) | (x & 0xFFFF))


def client_to_screen(handle: int, x: int, y: int) -> tuple[int, int]:
    point = wintypes.POINT(x, y)
    user32.ClientToScreen(handle, ctypes.byref(point))
    return point.x, point.y


def wheel(handle: int, x: int, y: int, delta: int) -> None:
    screen_x, screen_y = client_to_screen(handle, x, y)
    user32.PostMessageW(handle, WM_MOUSEWHEEL,
                        (delta & 0xFFFF) << 16, (screen_y << 16) | (screen_x & 0xFFFF))


def key(handle: int, virtual_key: int) -> None:
    user32.PostMessageW(handle, WM_KEYDOWN, virtual_key, 0)
    user32.PostMessageW(handle, WM_KEYUP, virtual_key, 0)


def enter_key(handle: int) -> None:
    # The physical Enter sequence: WM_KEYDOWN, TranslateMessage's
    # WM_CHAR(0x0D), then WM_KEYUP. Posting only the key down misses the
    # CR the real input stack delivers afterwards.
    user32.PostMessageW(handle, WM_KEYDOWN, VK_RETURN, 0)
    user32.PostMessageW(handle, 0x0102, 0x0D, 0)  # WM_CHAR CR
    user32.PostMessageW(handle, WM_KEYUP, VK_RETURN, 0)


def type_text(handle: int, text: str) -> None:
    for char in text:
        user32.PostMessageW(handle, 0x0102, ord(char), 0)  # WM_CHAR


def force_paint(handle: int) -> None:
    # Synchronous WM_PAINT (the same mechanism the scp_ui unit tests use);
    # cross-process RedrawWindow did not reach the window's update region.
    user32.SendMessageW(handle, 0x000F, 0, 0)


def bring_to_top(handle: int) -> None:
    # The screen BitBlt capture needs the window physically unobscured; a
    # shown-again chat window may sit behind the main window.
    user32.SetWindowPos(handle, 0, 0, 0, 0, 0,
                        0x0001 | 0x0002 | 0x0010 | 0x0004)  # NOMOVE|NOSIZE|SHOWWINDOW|NOACTIVATE


def capture_client_fast(handle: int):
    from PIL import Image
    import ctypes as ct
    width, height = client_size(handle)
    # Read the window's own DC (the surface our WM_PAINT draws into). Unlike
    # a screen BitBlt this stays correct when the window is occluded and
    # when the interactive display session is unavailable.
    window_dc = user32.GetWindowDC(handle)
    mem_dc = gdi32.CreateCompatibleDC(window_dc)
    bitmap = gdi32.CreateCompatibleBitmap(window_dc, width, height)
    gdi32.SelectObject(mem_dc, bitmap)
    rect = wintypes.RECT()
    user32.GetWindowRect(handle, ct.byref(rect))
    origin = wintypes.POINT(0, 0)
    user32.ClientToScreen(handle, ct.byref(origin))
    gdi32.BitBlt(mem_dc, 0, 0, width, height, window_dc,
                 origin.x - rect.left, origin.y - rect.top, SRC_COPY)

    class BITMAPINFOHEADER(ct.Structure):
        _fields_ = [
            ("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG),
            ("biHeight", wintypes.LONG), ("biPlanes", wintypes.WORD),
            ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
            ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
            ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
            ("biClrImportant", wintypes.DWORD)]
    header = BITMAPINFOHEADER()
    header.biSize = ct.sizeof(BITMAPINFOHEADER)
    header.biWidth = width
    header.biHeight = -height
    header.biPlanes = 1
    header.biBitCount = 32
    header.biCompression = 0
    buffer = ct.create_string_buffer(width * height * 4)
    gdi32.GetDIBits(mem_dc, bitmap, 0, height, buffer, ct.byref(header), 0)
    gdi32.DeleteObject(bitmap)
    gdi32.DeleteDC(mem_dc)
    user32.ReleaseDC(handle, window_dc)
    # GetDIBits 32-bit BI_RGB gives BGRA rows.
    image = Image.frombuffer("RGBA", (width, height), buffer.raw, "raw", "BGRA", width * 4, 1)
    return image


def crop_region(image, box):
    return image.crop(box)


def region_equal(a, b) -> bool:
    return list(a.getdata()) == list(b.getdata())


def capture_ready(handle):
    bring_to_top(handle)
    force_paint(handle)
    return capture_client_fast(handle)


def list_text_pixels(image) -> int:
    # Channel-max threshold: luminance alone misses red channel text
    # (the world span renders red, ~76 in gray) and blue/green spans.
    rgb = image.convert("RGB")
    box = rgb.crop((1, 23, 545, 519))
    return sum(1 for r, g, b in box.getdata() if max(r, g, b) > 100)


def capture_with_text(handle, timeout_s: float = 15.0):
    """Captures the client area once it renders list content; the window
    repaints asynchronously on the client's frame clock."""
    deadline = time.time() + timeout_s
    shot = None
    while time.time() < deadline:
        bring_to_top(handle)
        force_paint(handle)
        shot = capture_client_fast(handle)
        if list_text_pixels(shot) > 50:
            return shot
        time.sleep(0.4)
    return shot


def build_spaced_replay() -> None:
    data = SOURCE_REPLAY.read_bytes()
    if data[:4] != b"DHR1":
        fail("source replay missing DHR1 magic")
    records = []
    offset = 4
    while offset < len(data):
        elapsed, command, direction, flags, size = struct.unpack_from("<IHBBI", data, offset)
        payload = data[offset + 12: offset + 12 + size]
        records.append((command, direction, flags, payload))
        offset += 12 + size
    # 12 cycles x 4 messages every 900 ms: enough rows to scroll and to see
    # auto-roll pause/resume live, well under the 200-message safeguard.
    out = bytearray(b"DHR1")
    elapsed = 0
    for cycle in range(12):
        for command, direction, flags, payload in records:
            out += struct.pack("<IHBBI", elapsed, command, direction, flags, len(payload))
            out += payload
            elapsed += 900
    SPACED_REPLAY.write_bytes(bytes(out))


def wait_for(condition, timeout_s: float, poll_s: float = 0.1, what: str = "condition"):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if condition():
            return True
        time.sleep(poll_s)
    return False


def main() -> int:
    build_spaced_replay()
    if not CLIENT_EXE.exists():
        fail(f"client executable missing: {CLIENT_EXE}")

    stdout_path = ROOT / ".rea/task8_acceptance_client.log"
    stdout_file = open(stdout_path, "w", encoding="utf-8", errors="replace")
    process = subprocess.Popen(
        [str(CLIENT_EXE), "--shdata", SHDATA, "--cache", str(BASE_CACHE),
         "--replay", str(SPACED_REPLAY), "--log", str(CLIENT_LOG)],
        stdout=stdout_file, stderr=subprocess.STDOUT)

    start = time.time()
    try:
        if not wait_for(lambda: find_window(MAIN_CLASS), 60.0):
            fail("main window did not appear")
        record("main window appeared", True, f"{time.time() - start:.1f}s")
        main = find_window(MAIN_CLASS)
        record("no chat window before open", class_window_count(CHAT_CLASS) == 0)

        # open: the HUD chat_switch button sits at client (9,474).
        click(main, 9, 474)
        if not wait_for(lambda: class_window_count(CHAT_CLASS) == 1, 10.0):
            fail("chat window did not open")
        chat = find_window(CHAT_CLASS)
        record("chat_switch opened the chat window", True)

        cw, ch = client_size(chat)
        fw, fh = frame_size(chat)
        record("client area is 549x590", (cw, ch) == (549, 590), f"got {cw}x{ch}")
        record("frame is 565x629", (fw, fh) == (565, 629), f"got {fw}x{fh}")
        style = user32.GetWindowLongW(chat, -16)  # GWL_STYLE
        ex_style = user32.GetWindowLongW(chat, -20)  # GWL_EXSTYLE
        record("no WS_SYSMENU (no X button)",
               (style & WS_SYSMENU) == 0 and (style & WS_CAPTION) != 0
               and (style & WS_THICKFRAME) != 0,
               f"style=0x{style & 0xFFFFFFFF:08x}")
        record("WS_EX_WINDOWEDGE kept", (ex_style & WS_EX_WINDOWEDGE) != 0,
               f"exstyle=0x{ex_style & 0xFFFFFFFF:08x}")
        record("title is the original one", window_title(chat) == "聊天窗口",
               window_title(chat))

        time.sleep(1.5)  # let the first replay cycles render
        shot_open = capture_ready(chat)
        shot_open.save(ROOT / ".rea/task8_client_open.png")

        # filter: world strip button at (105,12), then back to all at (10,12).
        click(chat, 105, 12)
        time.sleep(0.6)
        capture_ready(chat).save(ROOT / ".rea/task8_client_world_filter.png")
        click(chat, 10, 12)
        time.sleep(0.4)

        # scroll: wheel up over the message list.
        for _ in range(6):
            wheel(chat, 100, 100, 120)
        time.sleep(0.6)
        capture_ready(chat).save(ROOT / ".rea/task8_client_scrolled.png")

        # pause through the stop button, then keep appending via replay.
        click(chat, 35, 551)
        time.sleep(0.5)
        paused_shot = capture_ready(chat)
        paused_shot.save(ROOT / ".rea/task8_client_paused.png")
        # The already-visible top band must not move while paused; rows only
        # ever append below, so the filled top region is the stable witness.
        paused_view = crop_region(paused_shot, (0, 23, 545, 300))

        # Let several replay messages arrive while paused (~3.6s = 4 messages).
        time.sleep(3.6)
        after_shot = capture_ready(chat)
        after_shot.save(ROOT / ".rea/task8_client_paused_after.png")
        after_view = crop_region(after_shot, (0, 23, 545, 300))
        record("paused append did not jump the view", region_equal(paused_view, after_view))

        # resume: stop2 occupies the stop button spot while paused.
        click(chat, 35, 551)
        time.sleep(0.6)
        capture_ready(chat).save(ROOT / ".rea/task8_client_resumed.png")

        # attached mode (input row) via chatmain_switch at (12,551).
        click(chat, 12, 551)
        time.sleep(0.8)
        record("chatmain_switch reported to host",
               wait_for(lambda: "SCP chatmain switch=attached-toggle" in stdout_path.read_text(encoding="utf-8", errors="replace"), 5.0))

        # edit: click the input row edit at (100,545), type, screenshot, send.
        click(chat, 100, 545)
        type_text(chat, "TASK8_ACCEPT_01")
        time.sleep(0.5)
        edit_shot = capture_ready(chat)
        edit_shot.save(ROOT / ".rea/task8_client_edit.png")
        # The input row must actually render: light chatinputbg strip,
        # the typed text on it, and the channel combo button.
        bg = edit_shot.crop((0, 534, 360, 562)).convert("RGB")
        light = sum(1 for p in bg.getdata() if min(p) > 120)
        record("attached input bar background renders", light > 1000,
               f"light_px={light}")
        edit_area = edit_shot.crop((56, 538, 356, 554)).convert("RGB")
        dark = sum(1 for p in edit_area.getdata() if max(p) < 80)
        record("typed text renders in the input edit", dark > 20,
               f"dark_px={dark}")
        combo_area = edit_shot.crop((16, 532, 56, 570)).convert("RGB")
        combo_px = sum(1 for p in combo_area.getdata()
                       if max(p) - min(p) > 25 or min(p) > 120)
        record("channel combo renders", combo_px > 50, f"combo_px={combo_px}")
        enter_key(chat)
        time.sleep(0.8)
        stdout_text = stdout_path.read_text(encoding="utf-8", errors="replace")
        record("typed text reached the single sink without fake success",
               "Chat send suppressed" in stdout_text and "bytes=15" in stdout_text)

        # detach again (chatmain_switch sits at (12,523) while attached).
        click(chat, 12, 523)
        time.sleep(0.6)

        # WM_CLOSE must not close the window.
        user32.SendMessageW(chat, WM_CLOSE, 0, 0)
        time.sleep(0.4)
        record("WM_CLOSE is not a close path",
               class_window_count(CHAT_CLASS) == 1 and find_window(CHAT_CLASS) == chat)

        # hide via the main HUD chat_switch, then reopen.
        click(main, 9, 474)
        time.sleep(0.6)
        record("chat_switch hid the detached window",
               not user32.IsWindowVisible(chat))
        capture_ready(main).save(ROOT / ".rea/task8_client_embedded_main.png")
        click(main, 9, 474)
        time.sleep(0.6)
        record("chat_switch reopened the detached window",
               bool(user32.IsWindowVisible(chat)))
        reopened_shot = capture_with_text(chat)
        reopened_shot.save(ROOT / ".rea/task8_client_reopened.png")
        record("reopened window shows the shared history",
               list_text_pixels(reopened_shot) > 50,
               f"text_pixels={list_text_pixels(reopened_shot)}")
        record("exactly one chat window throughout",
               class_window_count(CHAT_CLASS) == 1)

        # The client exits by itself once the replay finishes.
        exited = process.wait(timeout=90) == 0
        record("client exited cleanly after replay", exited,
               f"returncode={process.returncode}")
    finally:
        if process.poll() is None:
            process.terminate()
        stdout_file.close()

    client_out = stdout_path.read_text(encoding="utf-8", errors="replace")
    record("client processed the chat replay",
           "chats=48" in client_out, "chats=48" if "chats=48" in client_out else client_out[-200:])
    record("app logged the new chat window",
           "SCP chatmain window=new" in CLIENT_LOG.read_text(encoding="utf-8", errors="replace"))

    passed = sum(1 for item in results if item["ok"])
    summary = {"passed": passed, "failed": len(results) - passed, "checks": results}
    OUT_JSON.write_text(json.dumps(summary, ensure_ascii=True, indent=1))
    print(f"SUMMARY passed={passed} failed={len(results) - passed}")
    print(f"details: {OUT_JSON}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
