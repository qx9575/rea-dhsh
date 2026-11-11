#!/usr/bin/env python3
"""Drive the real native window through the offline M1 acceptance flow."""

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

from client_window_smoke import diagnostics_error


TITLE = re.compile(
    r"camera=(-?\d+),(-?\d+) \| zoom=([0-9.]+) \| player=(-?\d+),(-?\d+) "
    r"state=(Idle|Walk) nav=(\w+) goal=(-?\d+),(-?\d+)")


class WindowDriver:
    def __init__(self, process, log_path: Path):
        self.process = process
        self.log_path = log_path
        self.window = None
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
        self.user32.IsWindow.argtypes = [wintypes.HWND]
        self.user32.IsWindow.restype = wintypes.BOOL
        self.user32.GetWindowTextW.argtypes = [
            wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
        self.user32.GetWindowTextW.restype = ctypes.c_int
        self.user32.SendMessageW.argtypes = [
            wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        self.user32.SendMessageW.restype = ctypes.c_ssize_t
        self.user32.PostMessageW.argtypes = [
            wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        self.user32.PostMessageW.restype = wintypes.BOOL
        self.user32.GetClientRect.argtypes = [
            wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
        self.user32.GetClientRect.restype = wintypes.BOOL
        self.user32.ClientToScreen.argtypes = [
            wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
        self.user32.ClientToScreen.restype = wintypes.BOOL
        self.user32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
        self.user32.SetCursorPos.restype = wintypes.BOOL
        self.user32.SetForegroundWindow.argtypes = [wintypes.HWND]
        self.user32.SetForegroundWindow.restype = wintypes.BOOL

    @staticmethod
    def lparam(x: int, y: int) -> int:
        return ((y & 0xFFFF) << 16) | (x & 0xFFFF)

    def find_window(self) -> None:
        def visit(hwnd, _):
            owner = wintypes.DWORD()
            self.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == self.process.pid and self.user32.IsWindowVisible(hwnd):
                text = ctypes.create_unicode_buffer(1024)
                self.user32.GetWindowTextW(hwnd, text, len(text))
                if text.value.startswith("dhsh_client - "):
                    self.window = hwnd
                    return False
            return True

        callback = self.enum_proc(visit)
        self.user32.EnumWindows(callback, 0)

    def wait_window(self, timeout: float = 120.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(f"client exited early with {self.process.returncode}")
            self.find_window()
            if self.window is not None and self.log_text().find("Startup report") >= 0:
                self.user32.SetForegroundWindow(self.window)
                return
            time.sleep(0.1)
        raise RuntimeError("native client window/startup report timed out")

    def log_text(self) -> str:
        try:
            return self.log_path.read_text(encoding="utf-8", errors="replace")
        except FileNotFoundError:
            return ""

    def wait_log(self, expected: str, timeout: float = 10.0, count: int = 1) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.log_text().count(expected) >= count:
                return
            if self.process.poll() is not None:
                raise RuntimeError("client exited while waiting for diagnostic")
            time.sleep(0.05)
        raise RuntimeError(f"diagnostic timed out: {expected!r} count={count}")

    def title(self) -> dict:
        buffer = ctypes.create_unicode_buffer(1024)
        self.user32.GetWindowTextW(self.window, buffer, len(buffer))
        match = TITLE.search(buffer.value)
        if match is None:
            raise RuntimeError(f"unrecognized client title: {buffer.value!r}")
        values = match.groups()
        return {
            "camera": (int(values[0]), int(values[1])),
            "zoom": float(values[2]),
            "player": (int(values[3]), int(values[4])),
            "state": values[5],
            "nav": values[6],
            "goal": (int(values[7]), int(values[8])),
        }

    def wait_title(self, predicate, timeout: float = 10.0) -> dict:
        deadline = time.monotonic() + timeout
        latest = None
        while time.monotonic() < deadline:
            latest = self.title()
            if predicate(latest):
                return latest
            time.sleep(0.04)
        raise RuntimeError(f"title condition timed out: {latest}")

    def logical_to_client(self, x: int, y: int) -> tuple[int, int]:
        rect = wintypes.RECT()
        if not self.user32.GetClientRect(self.window, ctypes.byref(rect)):
            raise ctypes.WinError(ctypes.get_last_error())
        width = rect.right - rect.left
        height = rect.bottom - rect.top
        if width * 600 > height * 800:
            logical_height = height
            logical_width = height * 800 // 600
            offset_x, offset_y = (width - logical_width) // 2, 0
        else:
            logical_width = width
            logical_height = width * 600 // 800
            offset_x, offset_y = 0, (height - logical_height) // 2
        return (offset_x + round(x * logical_width / 800),
                offset_y + round(y * logical_height / 600))

    def mouse(self, message: int, x: int, y: int, wparam: int = 0) -> None:
        self.user32.SendMessageW(
            self.window, message, wparam, self.lparam(x, y))

    def position_cursor(self, client: tuple[int, int]) -> None:
        point = wintypes.POINT(*client)
        if not self.user32.ClientToScreen(self.window, ctypes.byref(point)):
            raise ctypes.WinError(ctypes.get_last_error())
        self.user32.SetCursorPos(point.x, point.y)

    def click_logical(self, x: int, y: int) -> None:
        client = self.logical_to_client(x, y)
        self.position_cursor(client)
        self.mouse(0x0200, *client)
        time.sleep(0.08)
        self.mouse(0x0201, *client)
        time.sleep(0.04)
        self.mouse(0x0202, *client)
        time.sleep(0.12)

    def key(self, key: int) -> None:
        self.user32.SendMessageW(self.window, 0x0100, key, 0)
        self.user32.SendMessageW(self.window, 0x0101, key, 0)
        time.sleep(0.12)

    def click_cell(self, cell: tuple[int, int]) -> None:
        current = self.title()
        logical = tuple(round((cell[index] * 16 + 8 - current["camera"][index])
                              * current["zoom"]) for index in (0, 1))
        if not (0 <= logical[0] < 800 and 0 <= logical[1] < 600):
            raise RuntimeError(f"target {cell} is not visible: {logical} {current}")
        self.click_logical(*logical)

    def close(self) -> None:
        if self.window is not None:
            self.user32.PostMessageW(self.window, 0x0010, 0, 0)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", required=True)
    parser.add_argument("--shdata", required=True)
    parser.add_argument("--cache", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--capture", required=True)
    parser.add_argument("--result", required=True)
    parser.add_argument("--configuration", default="Release")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.configuration != "Release":
        print(f"SKIP: M1 App acceptance is Release-only, got {args.configuration}")
        return 77
    if sys.platform != "win32":
        print("M1 App acceptance requires Windows", file=sys.stderr)
        return 2
    log_path = Path(args.log).resolve()
    capture_path = Path(args.capture).resolve()
    result_path = Path(args.result).resolve()
    for path in (log_path, capture_path, result_path):
        path.unlink(missing_ok=True)
    result = {"real_pak": True, "map": "YingTianFu"}
    process = None
    driver = None
    stdout_path = result_path.with_suffix(".stdout.log")
    stderr_path = result_path.with_suffix(".stderr.log")
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        try:
            process = subprocess.Popen([
                args.client, "--shdata", args.shdata, "--map", "YingTianFu",
                "--cache", args.cache, "--log", str(log_path),
                "--capture", str(capture_path), "--acceptance"],
                stdout=stdout, stderr=stderr)
            driver = WindowDriver(process, log_path)
            driver.wait_window()
            initial = driver.title()
            if initial["player"] != (6968, 2136):
                raise RuntimeError(f"wrong App spawn: {initial}")
            result["spawn"] = initial["player"]

            driver.click_logical(160, 115)
            driver.wait_log("SCP Lua file=ui/login/bulletin2.scp calls=1")
            lua_text = driver.log_text()
            if "policy_visible=true bulletin_visible=false license_visible=false" not in lua_text:
                raise RuntimeError("App Lua callback did not apply the original window state")
            time.sleep(0.3)
            if driver.title()["player"] != initial["player"]:
                raise RuntimeError("SCP callback click moved the map player")
            result["lua_callback"] = "OnShowPolicy"
            result["ui_click_preserved_player"] = True

            driver.key(0x77)
            driver.wait_log("SCP acceptance bulletin=hidden")
            driver.key(0x77)
            driver.wait_log("SCP acceptance bulletin=shown registered_callbacks=3", count=2)
            driver.click_logical(160, 115)
            driver.wait_log("policy_visible=true bulletin_visible=false license_visible=false",
                count=2)
            time.sleep(0.3)
            if driver.title()["player"] != initial["player"]:
                raise RuntimeError("reopened SCP callback click moved the map player")
            result["reopen_callback"] = "OnShowPolicy"
            driver.key(0x77)
            driver.wait_log("SCP acceptance bulletin=hidden", count=2)
            driver.click_cell((425, 118))
            started = time.monotonic()
            positions = set()
            states = set()
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                current = driver.title()
                positions.add(current["player"])
                states.add(current["state"])
                if current["player"] == (6808, 1896) and current["state"] == "Idle" \
                        and current["nav"] == "already_at_goal":
                    break
                time.sleep(0.04)
            else:
                raise RuntimeError(f"App reachable click did not arrive: {driver.title()}")
            if "Walk" not in states or len(positions) < 3:
                raise RuntimeError("App did not show continuous Walk movement")
            result["reachable"] = {
                "position": [6808, 1896], "positions": len(positions),
                "seconds": round(time.monotonic() - started, 3)}

            before = driver.title()
            driver.click_cell((426, 118))
            blocked = driver.wait_title(lambda current: current["nav"] == "goal_blocked")
            if blocked["player"] != before["player"] or blocked["state"] != "Idle":
                raise RuntimeError("App blocked click moved the player")
            result["blocked"] = "goal_blocked"

            before_drag = driver.title()
            down = driver.logical_to_client(400, 500)
            moved = driver.logical_to_client(600, 500)
            driver.position_cursor(down)
            driver.mouse(0x0200, *down)
            driver.mouse(0x0204, *down, 0x0002)
            time.sleep(0.05)
            driver.position_cursor(moved)
            driver.mouse(0x0200, *moved, 0x0002)
            time.sleep(0.12)
            driver.mouse(0x0205, *moved)
            expected_camera = (before_drag["camera"][0] - 200, before_drag["camera"][1])
            after_drag = driver.wait_title(
                lambda current: current["camera"] == expected_camera)
            if after_drag["player"] != before_drag["player"]:
                raise RuntimeError("camera drag moved the player")
            result["camera_drag"] = after_drag["camera"]

            before_unreachable = driver.title()
            driver.click_cell((395, 114))
            unreachable = driver.wait_title(
                lambda current: current["nav"] == "unreachable")
            if unreachable["player"] != before_unreachable["player"] \
                    or unreachable["state"] != "Idle":
                raise RuntimeError("App unreachable click moved the player")
            result["unreachable"] = {"cell": [395, 114], "status": "unreachable"}

            driver.click_cell((435, 133))
            driver.wait_title(lambda current: current["state"] == "Walk")
            before_moving_drag = driver.title()
            down = driver.logical_to_client(500, 300)
            moved = driver.logical_to_client(300, 300)
            driver.position_cursor(down)
            driver.mouse(0x0204, *down, 0x0002)
            time.sleep(0.05)
            after_down = driver.title()
            driver.position_cursor(moved)
            driver.mouse(0x0200, *moved, 0x0002)
            time.sleep(0.15)
            during_moving_drag = driver.title()
            if during_moving_drag["player"] == before_moving_drag["player"]:
                raise RuntimeError("right camera drag paused path movement")
            if during_moving_drag["camera"] != (
                    after_down["camera"][0] + 200, after_down["camera"][1]):
                raise RuntimeError(
                    "moving right drag did not preserve its map anchor: "
                    f"down={after_down} during={during_moving_drag}")
            driver.mouse(0x0205, *moved)
            released = driver.title()
            if released["camera"] != during_moving_drag["camera"]:
                raise RuntimeError("right button release jumped the camera")

            driver.click_cell((435, 133))
            resumed = driver.title()
            def follow_error(sample):
                return (abs(sample["camera"][0] - (sample["player"][0] - 400))
                        + abs(sample["camera"][1] - (sample["player"][1] - 300)))
            initial_error = follow_error(resumed)
            if initial_error <= 1:
                raise RuntimeError("new path immediately snapped the camera to the player")
            time.sleep(0.12)
            later = driver.title()
            if follow_error(later) >= initial_error:
                raise RuntimeError("camera did not smoothly approach the moving player")
            result["moving_right_drag"] = {
                "player_continued": True, "release_stable": True,
                "follow_error_before": initial_error,
                "follow_error_after": follow_error(later)}
            converged = driver.wait_title(
                lambda current: current["state"] == "Idle"
                and current["player"] == (6968, 2136) and follow_error(current) <= 1)
            result["follow_converged"] = converged["camera"]

            if not capture_path.is_file():
                raise RuntimeError("App did not produce its native first-frame capture")
            driver.close()
            if process.wait(timeout=20) != 0:
                raise RuntimeError(f"App close returned {process.returncode}")
            result["exit"] = 0
        except Exception as error:
            result["error"] = str(error)
            print(f"M1_APP_ACCEPTANCE FAIL {error}", file=sys.stderr)
            if process is not None and process.poll() is None:
                if driver is not None:
                    driver.close()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            result_path.write_text(json.dumps(result, indent=2), encoding="utf-8")
            return 1
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.wait()

    stdout = stdout_path.read_text(encoding="utf-8", errors="replace")
    stderr = stderr_path.read_text(encoding="utf-8", errors="replace")
    diagnostic = diagnostics_error(stdout)
    if diagnostic is not None:
        result["error"] = diagnostic
        result_path.write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(f"M1_APP_ACCEPTANCE FAIL {diagnostic}", file=sys.stderr)
        return 1
    if stderr:
        result["stderr"] = stderr
        result_path.write_text(json.dumps(result, indent=2), encoding="utf-8")
        print("M1_APP_ACCEPTANCE FAIL nonempty stderr", file=sys.stderr)
        return 1
    result["startup"] = next(
        line for line in stdout.splitlines() if line.startswith("Startup total ms="))
    result["frame_windows"] = stdout.count("Frame stats samples=120 ")
    result["diagnostic_budgets"] = "PASS"
    result_path.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print("M1_APP_ACCEPTANCE PASS " + json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
