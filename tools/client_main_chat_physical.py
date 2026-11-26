#!/usr/bin/env python3
"""Offline physical-input acceptance for the real seven-window main chat UI."""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import re
import subprocess
import sys
import time
import traceback

from PIL import Image, ImageGrab

from client_window_smoke import diagnostics_error


TITLE = re.compile(
    r"camera=(-?\d+),(-?\d+) \| zoom=([0-9.]+) \| player=(-?\d+),(-?\d+) "
    r"state=(Idle|Walk) nav=(\w+) goal=(-?\d+),(-?\d+)")


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
        ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.c_size_t),
    ]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", wintypes.LONG), ("dy", wintypes.LONG),
        ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t),
    ]


class INPUTUNION(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("union", INPUTUNION)]


class PhysicalWindow:
    def __init__(self, process: subprocess.Popen, output: Path):
        self.process = process
        self.output = output
        self.hwnd = None
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.enum_proc = ctypes.WINFUNCTYPE(
            wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        self.user32.EnumWindows.argtypes = [self.enum_proc, wintypes.LPARAM]
        self.user32.EnumWindows.restype = wintypes.BOOL
        self.user32.GetWindowThreadProcessId.argtypes = [
            wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        self.user32.GetWindowThreadProcessId.restype = wintypes.DWORD
        self.user32.IsWindowVisible.argtypes = [wintypes.HWND]
        self.user32.IsWindowVisible.restype = wintypes.BOOL
        self.user32.GetWindowTextW.argtypes = [
            wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
        self.user32.GetWindowTextW.restype = ctypes.c_int
        self.user32.GetClientRect.argtypes = [
            wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
        self.user32.GetClientRect.restype = wintypes.BOOL
        self.user32.ClientToScreen.argtypes = [
            wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
        self.user32.ClientToScreen.restype = wintypes.BOOL
        self.user32.SetForegroundWindow.argtypes = [wintypes.HWND]
        self.user32.SetForegroundWindow.restype = wintypes.BOOL
        self.user32.BringWindowToTop.argtypes = [wintypes.HWND]
        self.user32.BringWindowToTop.restype = wintypes.BOOL
        self.user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
        self.user32.ShowWindow.restype = wintypes.BOOL
        self.user32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
        self.user32.SetCursorPos.restype = wintypes.BOOL
        self.user32.GetForegroundWindow.restype = wintypes.HWND
        self.user32.PostMessageW.argtypes = [
            wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        self.user32.PostMessageW.restype = wintypes.BOOL
        self.imm32 = ctypes.WinDLL("imm32")
        self.imm32.ImmAssociateContext.argtypes = [wintypes.HWND, wintypes.HANDLE]
        self.imm32.ImmAssociateContext.restype = wintypes.HANDLE
        self.imm32.ImmGetOpenStatus.argtypes = [wintypes.HANDLE]
        self.imm32.ImmGetOpenStatus.restype = wintypes.BOOL
        self.imm32.ImmSetOpenStatus.argtypes = [wintypes.HANDLE, wintypes.BOOL]
        self.imm32.ImmSetOpenStatus.restype = wintypes.BOOL
        self.user32.GetKeyboardLayout.argtypes = [wintypes.DWORD]
        self.user32.GetKeyboardLayout.restype = wintypes.HANDLE
        self.user32.LoadKeyboardLayoutW.argtypes = [wintypes.LPCWSTR, wintypes.UINT]
        self.user32.LoadKeyboardLayoutW.restype = wintypes.HANDLE
        self.user32.UnloadKeyboardLayout.argtypes = [wintypes.HANDLE]
        self.user32.UnloadKeyboardLayout.restype = wintypes.BOOL
        self.ime_context = None
        self.ime_open = None
        self.original_keyboard_layout = None
        self.english_keyboard_layout = None

    def disable_ime_for_client(self) -> None:
        # Close the test window's saved context as well as detaching it: with
        # the host Chinese IME, detaching alone still produces VK_PROCESSKEY.
        self.ime_context = self.imm32.ImmAssociateContext(self.hwnd, None)
        if self.ime_context is not None:
            self.ime_open = bool(self.imm32.ImmGetOpenStatus(self.ime_context))
            self.imm32.ImmSetOpenStatus(self.ime_context, False)

    def use_english_keyboard(self) -> None:
        thread = self.user32.GetWindowThreadProcessId(self.hwnd, None)
        self.original_keyboard_layout = self.user32.GetKeyboardLayout(thread)
        self.english_keyboard_layout = self.user32.LoadKeyboardLayoutW("00000409", 0)
        if self.english_keyboard_layout is None:
            raise ctypes.WinError(ctypes.get_last_error())
        self.ensure_english_keyboard()

    def ensure_english_keyboard(self) -> None:
        if self.english_keyboard_layout is None:
            return
        thread = self.user32.GetWindowThreadProcessId(self.hwnd, None)
        if self.user32.GetKeyboardLayout(thread) == self.english_keyboard_layout:
            return
        # This changes only the client thread's keyboard layout. It is setup,
        # not simulated key input; every measured key still uses the OS queue.
        self.user32.PostMessageW(self.hwnd, 0x0050, 0, self.english_keyboard_layout)
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            if self.user32.GetKeyboardLayout(thread) == self.english_keyboard_layout:
                return
            time.sleep(0.05)
        raise RuntimeError("client thread did not switch to the en-US keyboard layout")

    def english_keyboard_active(self) -> bool:
        thread = self.user32.GetWindowThreadProcessId(self.hwnd, None)
        return self.english_keyboard_layout is not None and (
            self.user32.GetKeyboardLayout(thread) == self.english_keyboard_layout)

    def restore_ime(self) -> None:
        if self.hwnd is not None and self.english_keyboard_layout is not None:
            thread = self.user32.GetWindowThreadProcessId(self.hwnd, None)
            self.user32.PostMessageW(
                self.hwnd, 0x0050, 0, self.original_keyboard_layout)
            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline:
                if self.user32.GetKeyboardLayout(thread) == self.original_keyboard_layout:
                    break
                time.sleep(0.05)
            self.user32.UnloadKeyboardLayout(self.english_keyboard_layout)
            self.english_keyboard_layout = None
            self.original_keyboard_layout = None
        if self.hwnd is not None and self.ime_context is not None:
            self.imm32.ImmAssociateContext(self.hwnd, self.ime_context)
            self.imm32.ImmSetOpenStatus(self.ime_context, self.ime_open)
            self.ime_context = None
            self.ime_open = None

    def find(self) -> None:
        def visit(hwnd, _):
            pid = wintypes.DWORD()
            self.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == self.process.pid and self.user32.IsWindowVisible(hwnd):
                text = ctypes.create_unicode_buffer(1024)
                self.user32.GetWindowTextW(hwnd, text, len(text))
                if text.value.startswith("dhsh_client - "):
                    self.hwnd = hwnd
                    return False
            return True

        callback = self.enum_proc(visit)
        self.user32.EnumWindows(callback, 0)

    def wait(self, timeout: float = 120.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(f"client exited early with {self.process.returncode}")
            self.find()
            if self.hwnd is not None:
                try:
                    self.title()
                    self.user32.ShowWindow(self.hwnd, 9)
                    self.user32.BringWindowToTop(self.hwnd)
                    self.user32.SetForegroundWindow(self.hwnd)
                    time.sleep(0.4)
                    return
                except RuntimeError:
                    pass
            time.sleep(0.1)
        raise RuntimeError("real native window did not become ready")

    def title(self) -> dict:
        text = ctypes.create_unicode_buffer(1024)
        self.user32.GetWindowTextW(self.hwnd, text, len(text))
        match = TITLE.search(text.value)
        if match is None:
            raise RuntimeError(f"unrecognized client title: {text.value!r}")
        values = match.groups()
        return {
            "camera": (int(values[0]), int(values[1])),
            "zoom": float(values[2]),
            "player": (int(values[3]), int(values[4])),
            "state": values[5],
            "nav": values[6],
            "goal": (int(values[7]), int(values[8])),
        }

    def client_point(self, logical_x: int, logical_y: int) -> wintypes.POINT:
        rect = wintypes.RECT()
        if not self.user32.GetClientRect(self.hwnd, ctypes.byref(rect)):
            raise ctypes.WinError(ctypes.get_last_error())
        width, height = rect.right - rect.left, rect.bottom - rect.top
        if width * 600 > height * 800:
            logical_height = height
            logical_width = height * 800 // 600
            offset_x, offset_y = (width - logical_width) // 2, 0
        else:
            logical_width = width
            logical_height = width * 600 // 800
            offset_x, offset_y = 0, (height - logical_height) // 2
        point = wintypes.POINT(
            offset_x + round(logical_x * logical_width / 800),
            offset_y + round(logical_y * logical_height / 600))
        if not self.user32.ClientToScreen(self.hwnd, ctypes.byref(point)):
            raise ctypes.WinError(ctypes.get_last_error())
        return point

    def focus(self) -> None:
        self.user32.ShowWindow(self.hwnd, 9)
        self.user32.BringWindowToTop(self.hwnd)
        self.user32.SetForegroundWindow(self.hwnd)
        time.sleep(0.15)
        self.ensure_english_keyboard()

    def click(self, x: int, y: int) -> None:
        self.focus()
        point = self.client_point(x, y)
        # SetCursorPos can transiently return FALSE with ERROR_SUCCESS while
        # foreground ownership changes. Retry that harness-side race briefly;
        # never reinterpret the ambiguous zero error as a product failure.
        for attempt in range(10):
            if self.user32.SetCursorPos(point.x, point.y):
                break
            last_error = ctypes.get_last_error()
            time.sleep(0.05)
        else:
            raise ctypes.WinError(last_error)
        time.sleep(0.12)
        self.user32.mouse_event(0x0002, 0, 0, 0, 0)
        time.sleep(0.07)
        self.user32.mouse_event(0x0004, 0, 0, 0, 0)
        time.sleep(0.25)

    def send_key(self, key: int, released: bool) -> None:
        release_flag = 0x0002 if released else 0
        if key == 0xA0:
            key_input = KEYBDINPUT(0, 0x2A, 0x0008 | release_flag, 0, 0)
        elif key == 0x27:
            key_input = KEYBDINPUT(key, 0x4D, 0x0001 | release_flag, 0, 0)
        else:
            self.user32.keybd_event(key, 0, release_flag, 0)
            return
        event = INPUT(1, INPUTUNION(ki=key_input))
        if self.user32.SendInput(1, ctypes.byref(event), ctypes.sizeof(event)) != 1:
            raise ctypes.WinError(ctypes.get_last_error())

    def key_down(self, key: int) -> None:
        self.send_key(key, released=False)

    def key_up(self, key: int) -> None:
        self.send_key(key, released=True)

    def tap(self, key: int) -> None:
        self.focus()
        self.key_down(key)
        time.sleep(0.06)
        self.key_up(key)
        time.sleep(0.18)

    def type_ascii(self, text: str) -> None:
        self.focus()
        # SendInput(KEYEVENTF_UNICODE) uses the same system input queue as the
        # physical keyboard but bypasses the host IME's ambiguous composition
        # state. It is not SendMessageW and still exercises Win32Window text.
        for char in text:
            encoded = char.encode("utf-16-le")
            for offset in range(0, len(encoded), 2):
                scan = int.from_bytes(encoded[offset:offset + 2], "little")
                key_input = KEYBDINPUT(0, scan, 0x0004, 0, 0)
                event = INPUT(1, INPUTUNION(ki=key_input))
                if self.user32.SendInput(1, ctypes.byref(event), ctypes.sizeof(event)) != 1:
                    raise ctypes.WinError(ctypes.get_last_error())
                key_input = KEYBDINPUT(0, scan, 0x0004 | 0x0002, 0, 0)
                event = INPUT(1, INPUTUNION(ki=key_input))
                if self.user32.SendInput(1, ctypes.byref(event), ctypes.sizeof(event)) != 1:
                    raise ctypes.WinError(ctypes.get_last_error())
                time.sleep(0.07)

    def enter(self) -> None:
        self.tap(0x0D)

    def capture(self, name: str) -> dict:
        time.sleep(0.25)
        top_left = self.client_point(0, 0)
        bottom_right = self.client_point(800, 600)
        box = (top_left.x, top_left.y, bottom_right.x, bottom_right.y)
        image = ImageGrab.grab(bbox=box, all_screens=True).convert("RGB")
        path = self.output / name
        image.save(path)
        # Metrics are diagnostic evidence, not OCR or a substitute for the
        # checked-in screenshots. Regions use native logical 800x600 geometry.
        resized = image.resize((800, 600), Image.Resampling.NEAREST)
        regions = {
            "selected_label": (23, 585, 55, 600),
            "dropdown_frame": (19, 420, 59, 582),
            "dropdown_text": (23, 422, 49, 584),
            "input_text": (56, 586, 356, 600),
        }
        metrics = {}
        for label, region in regions.items():
            crop = resized.crop(region)
            pixels = list(crop.getdata())
            metrics[label] = {
                "near_white": sum(min(p) >= 190 and max(p) - min(p) <= 45 for p in pixels),
                "non_blue_ink": sum(r >= 150 and g >= 150 and b < 140 for r, g, b in pixels),
            }
            crop.resize((crop.width * 4, crop.height * 4), Image.Resampling.NEAREST).save(
                self.output / f"{Path(name).stem}_{label}.png")
        return {"path": str(path), "metrics": metrics}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", required=True)
    parser.add_argument("--shdata", required=True)
    parser.add_argument("--cache", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--result", required=True)
    parser.add_argument("--output-dir", required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if sys.platform != "win32":
        raise SystemExit("physical acceptance requires Windows")
    ctypes.WinDLL("user32").SetProcessDPIAware()
    log_path = Path(args.log).resolve()
    result_path = Path(args.result).resolve()
    output = Path(args.output_dir).resolve()
    output.mkdir(parents=True, exist_ok=True)
    log_path.unlink(missing_ok=True)
    result_path.unlink(missing_ok=True)
    stdout_path = result_path.with_suffix(".stdout.log")
    stderr_path = result_path.with_suffix(".stderr.log")
    result = {
        "mode": "offline_physical_input",
        "online": False,
        "input_api": "SetForegroundWindow/SetCursorPos/mouse_event/keybd_event/SendInput",
        "checks": {},
        "camera_samples": {},
        "screenshots": {},
    }
    checks = result["checks"]
    process = None
    window = None
    stage = "not_started"

    def text() -> str:
        chunks = []
        for path in (log_path, stdout_path):
            try:
                chunks.append(path.read_text(encoding="utf-8", errors="replace"))
            except FileNotFoundError:
                pass
        return "\n".join(chunks)

    def wait_count(expected: str, count: int, timeout: float = 4.0) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if text().count(expected) >= count:
                return True
            if process.poll() is not None:
                return False
            time.sleep(0.05)
        return text().count(expected) >= count

    def stationary(before: dict, after: dict) -> bool:
        return before["camera"] == after["camera"]

    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        try:
            # No --online, --replay, credentials, or login flow is used.
            stage = "launch_client"
            process = subprocess.Popen([
                args.client, "--shdata", args.shdata, "--map", "YingTianFu",
                "--cache", args.cache, "--log", str(log_path)],
                stdout=stdout, stderr=stderr)
            window = PhysicalWindow(process, output)
            stage = "wait_for_window"
            window.wait()
            # A physical click on an empty part of the bottom bar activates
            # the real window without map navigation or focusing input_mes.
            stage = "initial_activation_click"
            window.click(700, 590)
            stage = "disable_ime"
            window.disable_ime_for_client()
            stage = "load_english_keyboard"
            window.use_english_keyboard()
            checks["english_keyboard_layout"] = True
            stage = "verify_foreground"
            window.focus()
            checks["real_foreground_window"] = window.user32.GetForegroundWindow() == window.hwnd
            if not checks["real_foreground_window"]:
                raise RuntimeError("could not keep the test client in the foreground")
            stage = "capture_initial"
            result["screenshots"]["initial"] = window.capture("01_initial.png")

            # Non-letter global keyboard contract at full App message order.
            for name, keys in {
                "plain_right": [0x27],
                "plain_down": [0x28],
            }.items():
                window.focus()
                before = window.title()
                for key in keys:
                    window.key_down(key)
                time.sleep(0.35)
                after = window.title()
                for key in reversed(keys):
                    window.key_up(key)
                time.sleep(0.15)
                checks[f"no_move_{name}"] = stationary(before, after)
                result["camera_samples"][name] = {"before": before["camera"], "after": after["camera"]}

            window.click(700, 590)
            before = window.title()
            window.key_down(0xA0)
            time.sleep(0.15)
            shift_before_right = bool(window.user32.GetAsyncKeyState(0xA0) & 0x8000)
            window.key_down(0x27)
            time.sleep(0.35)
            moving = window.title()
            checks["english_layout_during_shift_right"] = window.english_keyboard_active()
            result["physical_key_state"] = {
                "foreground_during_shift_right": window.user32.GetForegroundWindow() == window.hwnd,
                "english_layout_during_shift_right": checks["english_layout_during_shift_right"],
                "left_shift_down_before_right": shift_before_right,
                "shift_down_during_shift_right": bool(window.user32.GetAsyncKeyState(0x10) & 0x8000),
                "left_shift_down_during_shift_right": bool(window.user32.GetAsyncKeyState(0xA0) & 0x8000),
                "right_down_during_shift_right": bool(window.user32.GetAsyncKeyState(0x27) & 0x8000),
            }
            checks["shift_right_moves"] = moving["camera"][0] > before["camera"][0]
            window.key_up(0x27)
            time.sleep(0.15)
            released_arrow = window.title()
            time.sleep(0.35)
            checks["arrow_release_stops"] = stationary(released_arrow, window.title())
            window.key_down(0x27)
            time.sleep(0.2)
            window.key_up(0xA0)
            time.sleep(0.15)
            released_shift = window.title()
            time.sleep(0.35)
            checks["shift_release_stops"] = stationary(released_shift, window.title())
            window.key_up(0x27)
            result["camera_samples"]["shift_right"] = {
                "before": before["camera"], "moving": moving["camera"],
                "released_arrow": released_arrow["camera"],
                "released_shift": released_shift["camera"],
                "final": window.title()["camera"],
            }

            # Real keyboard focus and text entry, followed by direct Enter.
            window.click(100, 590)
            window.type_ascii("a")
            result["screenshots"]["typed"] = window.capture("02_input_typed.png")
            window.enter()
            checks["current_send"] = wait_count(
                "Chat send suppressed: replay has no live 0x4050 channel=1 bytes=1", 1)

            window.click(35, 590)
            result["screenshots"]["channels"] = window.capture("03_channels.png")
            window.click(30, 511)
            result["screenshots"]["world"] = window.capture("04_world_selected.png")
            window.type_ascii("b")
            window.enter()
            checks["world_direct_enter"] = wait_count(
                "Chat send suppressed: replay has no live 0x4050 channel=5 bytes=1", 1)

            window.click(35, 590)
            result["screenshots"]["channels_reopened"] = window.capture("04b_channels_reopened.png")
            window.click(30, 430)
            result["screenshots"]["current"] = window.capture("04c_current_selected.png")
            window.type_ascii("c")
            window.enter()
            checks["current_direct_enter"] = wait_count(
                "Chat send suppressed: replay has no live 0x4050 channel=1 bytes=1", 2)
            before = window.title()
            window.key_down(ord("D"))
            time.sleep(0.35)
            checks["chat_d_does_not_move"] = stationary(before, window.title())
            window.key_up(ord("D"))
            window.tap(0x08)  # clear the D text without another send
            window.enter()
            time.sleep(0.3)
            checks["empty_enter_suppressed"] = text().count(
                "Chat send suppressed: replay has no live 0x4050 channel=1 bytes=1") == 2

            # Letter probes run after chat text samples so their WM_CHAR output
            # cannot contaminate the next physical edit assertion.
            window.click(700, 590)
            for name, keys in {
                "plain_d": [ord("D")],
                "plain_a": [ord("A")],
                "shift_d": [0x10, ord("D")],
                "shift_a": [0x10, ord("A")],
            }.items():
                window.focus()
                before = window.title()
                for key in keys:
                    window.key_down(key)
                time.sleep(0.35)
                after = window.title()
                for key in reversed(keys):
                    window.key_up(key)
                time.sleep(0.15)
                checks[f"no_move_{name}"] = stationary(before, after)
                result["camera_samples"][name] = {"before": before["camera"], "after": after["camera"]}

            result["screenshots"]["final"] = window.capture("05_final.png")
            result["exit"] = None
        except Exception as error:
            result["error"] = str(error)
            result["failed_stage"] = stage
            result["traceback"] = traceback.format_exc()
        finally:
            # Never leave a physically held key or a client process behind.
            if window is not None:
                for key in (0x27, 0x28, ord("A"), ord("D"), 0x10, 0xA0):
                    try:
                        window.key_up(key)
                    except OSError as error:
                        result.setdefault("cleanup_errors", []).append(
                            f"key_up {key:#x}: {error}")
                try:
                    window.restore_ime()
                except OSError as error:
                    result.setdefault("cleanup_errors", []).append(
                        f"restore input locale: {error}")
            if process is not None and process.poll() is None:
                if window is not None and window.hwnd is not None:
                    window.user32.PostMessageW(window.hwnd, 0x0010, 0, 0)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            if process is not None:
                result["exit"] = process.returncode

    stdout_text = stdout_path.read_text(encoding="utf-8", errors="replace")
    stderr_text = stderr_path.read_text(encoding="utf-8", errors="replace")
    diagnostic = diagnostics_error(stdout_text)
    if diagnostic is not None:
        checks["diagnostic_budgets"] = False
        result["error"] = diagnostic
    else:
        checks["diagnostic_budgets"] = True
    checks["empty_stderr"] = not stderr_text
    checks["clean_exit"] = result.get("exit") == 0
    result["pass"] = all(checks.values()) and "error" not in result
    result_path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(("MAIN_CHAT_PHYSICAL PASS " if result["pass"] else "MAIN_CHAT_PHYSICAL FAIL ")
          + json.dumps({"checks": checks, "error": result.get("error")}, ensure_ascii=False))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
