#!/usr/bin/env python3
"""Open the native client, sample its frame log, close it, and check exit 0."""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import subprocess
import sys
import time


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", required=True)
    parser.add_argument("--shdata", required=True)
    parser.add_argument("--cache", required=True)
    parser.add_argument("--map", default="YingTianFu")
    parser.add_argument("--startup-timeout", type=float, default=120.0)
    parser.add_argument("--sample-seconds", type=float, default=5.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if sys.platform != "win32":
        print("native window smoke requires Windows", file=sys.stderr)
        return 2

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    enum_proc = ctypes.WINFUNCTYPE(
        wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.GetWindowThreadProcessId.argtypes = [
        wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.GetWindowThreadProcessId.restype = wintypes.DWORD
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.IsWindowVisible.restype = wintypes.BOOL
    user32.PostMessageW.argtypes = [
        wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.PostMessageW.restype = wintypes.BOOL

    process = subprocess.Popen(
        [args.client, "--shdata", args.shdata, "--map", args.map,
         "--cache", args.cache],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    window = None

    def find_window() -> None:
        nonlocal window
        pid = wintypes.DWORD(process.pid)

        def visit(hwnd: int, _lparam: int) -> bool:
            nonlocal window
            owner = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == pid.value and user32.IsWindowVisible(hwnd):
                window = hwnd
                return False
            return True

        user32.EnumWindows(enum_proc(visit), 0)

    deadline = time.monotonic() + args.startup_timeout
    timed_out = False
    control_error = False
    try:
        while time.monotonic() < deadline and process.poll() is None:
            find_window()
            if window is not None:
                break
            time.sleep(0.1)
        if window is None and process.poll() is None:
            timed_out = True
        elif window is not None:
            time.sleep(max(0.0, args.sample_seconds))
            if process.poll() is None and not user32.PostMessageW(window, 0x0010, 0, 0):
                raise ctypes.WinError(ctypes.get_last_error())
        stdout, stderr = process.communicate(timeout=20)
    except (subprocess.TimeoutExpired, OSError) as error:
        control_error = True
        timed_out = isinstance(error, subprocess.TimeoutExpired)
        if process.poll() is None:
            process.kill()
        stdout, stderr = process.communicate()
        print(f"window smoke control error: {error}", file=sys.stderr)

    if stdout:
        print(stdout, end="")
    if stderr:
        print(stderr, end="", file=sys.stderr)
    if timed_out:
        print("native client did not open and close within the time budget", file=sys.stderr)
        return 1
    if process.returncode != 0:
        print(f"native client exited with {process.returncode}", file=sys.stderr)
        return 1
    if window is None:
        print("native client exited before creating a visible window", file=sys.stderr)
        return 1
    if control_error:
        return 1
    frame_lines = [line for line in stdout.splitlines()
                   if line.startswith("Frame stats samples=120 ")]
    if "Startup total ms=" not in stdout or not frame_lines:
        print("native client produced no complete startup/frame diagnostics", file=sys.stderr)
        return 1
    if any("render_budget_ms=16.67 OVER" in line for line in frame_lines):
        print("native client exceeded the CPU composition budget", file=sys.stderr)
        return 1
    print(f"WINDOW_SMOKE PASS exit={process.returncode} hwnd={window}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
