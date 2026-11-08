#!/usr/bin/env python3
from __future__ import annotations

import inspect
import time
import unittest

import client_window_smoke as smoke


VALID_WALL_DIAGNOSTICS = (
    "Startup total ms=12.00\n"
    "Frame stats samples=120 advance_avg_ms=0.01 render_samples=50 "
    "render_avg_ms=10.00 render_max_ms=12.00 render_budget_ms=16.67 PASS "
    "present_avg_ms=8.00 frame_to_present_avg_ms=17.00 "
    "frame_to_present_max_ms=20.00\n"
)


class FakeProcess:
    def __init__(self, exit_after_polls: int | None = None) -> None:
        self.exit_after_polls = exit_after_polls
        self.polls = 0

    def poll(self) -> int | None:
        self.polls += 1
        if self.exit_after_polls is not None and self.polls >= self.exit_after_polls:
            return 0
        return None


class FakeUser32:
    def __init__(self, iconic: bool = False) -> None:
        self.close_messages: list[int] = []
        self.iconic = iconic

    def IsWindow(self, _window: int) -> bool:
        return True

    def IsIconic(self, _window: int) -> bool:
        return self.iconic

    def PostMessageW(self, window: int, message: int, wparam: int, lparam: int) -> bool:
        self.assert_close(message, wparam, lparam)
        self.close_messages.append(window)
        return True

    @staticmethod
    def assert_close(message: int, wparam: int, lparam: int) -> None:
        if (message, wparam, lparam) != (0x0010, 0, 0):
            raise AssertionError("unexpected window message")


class WindowSamplingTests(unittest.TestCase):
    def require_progress_api(self) -> None:
        self.assertTrue(hasattr(smoke, "sample_running_window"),
                        "missing bounded live-window sampling helper")
        parameters = inspect.signature(smoke.sample_running_window).parameters
        self.assertIn("frame_count", parameters,
                      "sampling must observe increasing Frame stats counts")

    def test_clean_exit_during_sampling_is_rejected(self) -> None:
        self.require_progress_api()
        process = FakeProcess(exit_after_polls=3)
        user32 = FakeUser32()
        with self.assertRaises(smoke.EarlyWindowExit):
            smoke.sample_running_window(
                process, 42, 1.0, user32, frame_count=lambda: 1)
        self.assertEqual(user32.close_messages, [])
        self.assertGreaterEqual(process.polls, 3)

    def test_live_window_is_sampled_for_full_duration_then_closed(self) -> None:
        self.require_progress_api()
        process = FakeProcess()
        user32 = FakeUser32()
        counts = iter([1, 1, 2])
        started = time.monotonic()
        smoke.sample_running_window(
            process, 42, 0.20, user32, frame_count=lambda: next(counts, 2))
        elapsed = time.monotonic() - started
        self.assertGreaterEqual(elapsed, 0.19)
        self.assertGreaterEqual(process.polls, 2)
        self.assertEqual(user32.close_messages, [42])

    def test_live_window_without_frame_progress_is_rejected(self) -> None:
        self.require_progress_api()
        self.assertTrue(hasattr(smoke, "FrameProgressError"),
                        "missing explicit stalled-frame result")
        process = FakeProcess()
        user32 = FakeUser32()
        with self.assertRaises(smoke.FrameProgressError):
            smoke.sample_running_window(
                process, 42, 0.05, user32, frame_count=lambda: 3)
        self.assertEqual(user32.close_messages, [])

    def test_minimized_window_cannot_pass_sampling(self) -> None:
        self.require_progress_api()
        self.assertTrue(hasattr(smoke, "MinimizedWindowError"),
                        "missing explicit minimized-window result")
        process = FakeProcess()
        user32 = FakeUser32(iconic=True)
        counts = iter([1, 2])
        with self.assertRaises(smoke.MinimizedWindowError):
            smoke.sample_running_window(
                process, 42, 0.20, user32, frame_count=lambda: next(counts, 2))
        self.assertEqual(user32.close_messages, [])


class DiagnosticContractTests(unittest.TestCase):
    def test_wall_time_field_is_required_and_cpu_name_is_rejected(self) -> None:
        self.assertTrue(hasattr(smoke, "diagnostics_error"),
                        "missing diagnostic validation helper")
        self.assertIsNone(smoke.diagnostics_error(VALID_WALL_DIAGNOSTICS))
        old_cpu_name = VALID_WALL_DIAGNOSTICS.replace(
            "frame_to_present_avg_ms", "cpu_avg_ms").replace(
            "frame_to_present_max_ms", "cpu_max_ms")
        self.assertIsNotNone(smoke.diagnostics_error(old_cpu_name))

    def test_render_over_is_still_rejected(self) -> None:
        self.assertTrue(hasattr(smoke, "diagnostics_error"),
                        "missing diagnostic validation helper")
        over = VALID_WALL_DIAGNOSTICS.replace(
            "render_budget_ms=16.67 PASS", "render_budget_ms=16.67 OVER")
        self.assertIsNotNone(smoke.diagnostics_error(over))


if __name__ == "__main__":
    unittest.main()
