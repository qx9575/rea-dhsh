#!/usr/bin/env python3
"""Open the native client, sample its frame log, close it, and check exit 0."""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
from pathlib import Path
import subprocess
import sys
import tempfile
import time


class EarlyWindowExit(RuntimeError):
    pass


class FrameProgressError(RuntimeError):
    pass


class MinimizedWindowError(RuntimeError):
    pass


def sample_running_window(process, window: int, sample_seconds: float, user32,
                          frame_count) -> None:
    deadline = time.monotonic() + max(0.0, sample_seconds)
    initial_frames = frame_count()
    latest_frames = initial_frames
    while True:
        returncode = process.poll()
        if returncode is not None:
            raise EarlyWindowExit(
                f"native client exited during sampling with {returncode}")
        if not user32.IsWindow(window):
            raise EarlyWindowExit("native client window disappeared during sampling")
        if user32.IsIconic(window):
            raise MinimizedWindowError("native client was minimized during sampling")
        latest_frames = frame_count()
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
            break
        time.sleep(min(0.1, remaining))
    if latest_frames <= initial_frames:
        raise FrameProgressError(
            "native client produced no new Frame stats during sampling: "
            f"count={latest_frames}")
    if not user32.PostMessageW(window, 0x0010, 0, 0):
        raise ctypes.WinError(ctypes.get_last_error())


def diagnostics_error(stdout: str) -> str | None:
    frame_lines = [line for line in stdout.splitlines()
                   if line.startswith("Frame stats samples=120 ")]
    if "Startup total ms=" not in stdout or not frame_lines:
        return "native client produced no complete startup/frame diagnostics"
    if any("frame_to_present_avg_ms=" not in line
           or "frame_to_present_max_ms=" not in line for line in frame_lines):
        return "native client produced no frame-to-present wall-time diagnostics"
    over_lines = [line for line in stdout.splitlines() if " OVER" in line]
    if over_lines:
        return "native client exceeded a diagnostic budget: " + over_lines[0]
    return None


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
    user32.IsWindow.argtypes = [wintypes.HWND]
    user32.IsWindow.restype = wintypes.BOOL
    user32.IsIconic.argtypes = [wintypes.HWND]
    user32.IsIconic.restype = wintypes.BOOL

    stdout_sink = tempfile.NamedTemporaryFile(
        prefix="dhsh-window-stdout-", suffix=".log", delete=False)
    stderr_sink = tempfile.NamedTemporaryFile(
        prefix="dhsh-window-stderr-", suffix=".log", delete=False)
    stdout_path = Path(stdout_sink.name)
    stderr_path = Path(stderr_sink.name)
    process = subprocess.Popen(
        [args.client, "--shdata", args.shdata, "--map", args.map,
         "--cache", args.cache],
        stdout=stdout_sink,
        stderr=stderr_sink,
    )
    window = None

    def log_bytes(path: Path) -> bytes:
        try:
            return path.read_bytes()
        except FileNotFoundError:
            return b""

    def frame_count() -> int:
        return log_bytes(stdout_path).count(b"Frame stats samples=120 ")

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
    early_exit = False
    try:
        while time.monotonic() < deadline and process.poll() is None:
            find_window()
            if window is not None:
                break
            time.sleep(0.1)
        if window is None and process.poll() is None:
            timed_out = True
        elif window is not None:
            sample_running_window(
                process, window, args.sample_seconds, user32, frame_count)
        process.wait(timeout=20)
    except (EarlyWindowExit, FrameProgressError, MinimizedWindowError,
            subprocess.TimeoutExpired, OSError) as error:
        control_error = True
        early_exit = isinstance(error, EarlyWindowExit)
        timed_out = isinstance(error, subprocess.TimeoutExpired)
        if process.poll() is None:
            process.kill()
        process.wait()
        print(f"window smoke control error: {error}", file=sys.stderr)

    stdout_sink.close()
    stderr_sink.close()
    stdout = log_bytes(stdout_path).decode("utf-8", errors="replace")
    stderr = log_bytes(stderr_path).decode("utf-8", errors="replace")
    stdout_path.unlink(missing_ok=True)
    stderr_path.unlink(missing_ok=True)

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
    if early_exit:
        return 1
    if control_error:
        return 1
    diagnostic_error = diagnostics_error(stdout)
    if diagnostic_error is not None:
        print(diagnostic_error, file=sys.stderr)
        return 1
    print(f"WINDOW_SMOKE PASS exit={process.returncode} hwnd={window}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
