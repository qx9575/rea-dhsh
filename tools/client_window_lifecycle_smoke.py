#!/usr/bin/env python3
"""Verify minimize suspension, restore clock reset, and clean native exit."""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
from pathlib import Path
import re
import subprocess
import sys
import time


FRAME_MARKER = b"Frame stats samples=120 "
TITLE_FRAME = re.compile(r"\| frame=([0-9]+(?:\.[0-9]+)?) ms$")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", required=True)
    parser.add_argument("--shdata", required=True)
    parser.add_argument("--cache", required=True)
    parser.add_argument("--map", default="YingTianFu")
    parser.add_argument("--startup-timeout", type=float, default=120.0)
    parser.add_argument("--minimize-seconds", type=float, default=1.0)
    parser.add_argument("--log-dir", default="client/build")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if sys.platform != "win32":
        print("native lifecycle smoke requires Windows", file=sys.stderr)
        return 2
    if args.minimize_seconds < 1.0:
        print("--minimize-seconds must be at least 1.0", file=sys.stderr)
        return 2

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    enum_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.GetWindowThreadProcessId.argtypes = [
        wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.GetWindowThreadProcessId.restype = wintypes.DWORD
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.IsWindowVisible.restype = wintypes.BOOL
    user32.GetWindowTextW.argtypes = [
        wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user32.GetWindowTextW.restype = ctypes.c_int
    user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    user32.ShowWindow.restype = wintypes.BOOL
    user32.PostMessageW.argtypes = [
        wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.PostMessageW.restype = wintypes.BOOL

    log_dir = Path(args.log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)
    stdout_path = log_dir / "task12-minimize-stdout.log"
    stderr_path = log_dir / "task12-minimize-stderr.log"
    process = None
    window = None

    def frame_count() -> int:
        try:
            return stdout_path.read_bytes().count(FRAME_MARKER)
        except FileNotFoundError:
            return 0

    def title() -> str:
        buffer = ctypes.create_unicode_buffer(512)
        user32.GetWindowTextW(window, buffer, len(buffer))
        return buffer.value

    def find_window() -> None:
        nonlocal window
        pid = wintypes.DWORD(process.pid)

        def visit(hwnd: int, _lparam: int) -> bool:
            nonlocal window
            owner = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == pid.value and user32.IsWindowVisible(hwnd):
                buffer = ctypes.create_unicode_buffer(512)
                user32.GetWindowTextW(hwnd, buffer, len(buffer))
                if buffer.value.startswith("dhsh_client - "):
                    window = hwnd
                    return False
            return True

        user32.EnumWindows(enum_proc(visit), 0)

    def require_alive() -> None:
        if process.poll() is not None:
            raise RuntimeError(f"client exited early with {process.returncode}")

    try:
        with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
            process = subprocess.Popen(
                [args.client, "--shdata", args.shdata, "--map", args.map,
                 "--cache", args.cache],
                stdout=stdout, stderr=stderr)
            deadline = time.monotonic() + args.startup_timeout
            while time.monotonic() < deadline:
                require_alive()
                find_window()
                if window is not None and frame_count() > 0:
                    break
                time.sleep(0.1)
            if window is None or frame_count() == 0:
                raise RuntimeError("client did not produce a window and initial frame stats")

            user32.ShowWindow(window, 6)
            time.sleep(0.25)
            require_alive()
            minimized_count = frame_count()
            minimized_title = title()
            time.sleep(args.minimize_seconds - 0.25)
            require_alive()
            final_minimized_count = frame_count()
            final_minimized_title = title()
            minimized_delta = final_minimized_count - minimized_count
            title_still = final_minimized_title == minimized_title
            if minimized_delta != 0 or not title_still:
                raise RuntimeError(
                    "minimized client kept producing diagnostics or title updates: "
                    f"stats_delta={minimized_delta} title_still={title_still}")

            user32.ShowWindow(window, 9)
            restore_frame_ms = None
            restore_deadline = time.monotonic() + 5.0
            while time.monotonic() < restore_deadline:
                require_alive()
                restored_title = title()
                match = TITLE_FRAME.search(restored_title)
                if match and restored_title != final_minimized_title:
                    restore_frame_ms = float(match.group(1))
                    break
                time.sleep(0.02)
            if restore_frame_ms is None:
                raise RuntimeError("restored client did not resume title updates")
            if restore_frame_ms >= 750.0:
                raise RuntimeError(
                    f"restore elapsed time includes minimization: {restore_frame_ms:.2f} ms")

            resume_deadline = time.monotonic() + 15.0
            while frame_count() <= final_minimized_count:
                require_alive()
                if time.monotonic() >= resume_deadline:
                    raise RuntimeError("restored client did not resume frame diagnostics")
                time.sleep(0.1)

            if not user32.PostMessageW(window, 0x0010, 0, 0):
                raise ctypes.WinError(ctypes.get_last_error())
            if process.wait(timeout=20) != 0:
                raise RuntimeError(f"client close returned {process.returncode}")
            print(
                "WINDOW_MINIMIZE_PASS minimized_stats_delta=0 title_still=True "
                f"restore_frame_ms={restore_frame_ms:.2f} exit=0")
            return 0
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"WINDOW_MINIMIZE_FAIL {error}", file=sys.stderr)
        return 1
    finally:
        if process is not None and process.poll() is None:
            try:
                if window is not None:
                    user32.ShowWindow(window, 9)
                    user32.PostMessageW(window, 0x0010, 0, 0)
                    process.wait(timeout=10)
                else:
                    process.terminate()
                    process.wait(timeout=10)
            except (OSError, subprocess.TimeoutExpired):
                process.kill()
                process.wait()
        if stderr_path.exists():
            errors = stderr_path.read_text(encoding="utf-8", errors="replace")
            if errors:
                print(errors, end="", file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
