"""Compare native paper-doll output with the independent Python viewer."""
from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

import PIL  # noqa: F401

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

from kimi_viewer.avatar import AvatarSource
from kimi_viewer.source import Entry, PakSource, _live_rows

FNV_BASIS = 14695981039346656037
FNV_PRIME = 1099511628211
REAL_LINE = re.compile(
    r"REAL action=(\d+) direction=(\d+) frame=(\d+) delay=(\d+) "
    r"fnv=([0-9a-f]+) layout=([0-9a-f]+)")


def fnv_update(value: int, data: bytes) -> int:
    for byte in data:
        value ^= byte
        value = (value * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


def layout_hash(order) -> int:
    value = FNV_BASIS
    for key, part, model, anim, index in order:
        encoded = part.encode("utf-8")
        value = fnv_update(value, struct.pack("<I", len(encoded)) + encoded)
        value = fnv_update(value, struct.pack("<I", model))
        value = fnv_update(value, struct.pack("<I", index))
        value = fnv_update(value, struct.pack("<H", key))
        ox, oy = anim.offsets[index]
        origin_x = 320 - anim.anchor[0] + ox
        origin_y = 240 - anim.anchor[1] + oy
        value = fnv_update(value, struct.pack("<ii", origin_x, origin_y))
    return value


def native_observations(executable: Path, shdata: Path):
    completed = subprocess.run(
        [str(executable), "--real", str(shdata)],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
        check=False)
    if completed.returncode != 0:
        print(completed.stdout, end="")
        print(completed.stderr, end="", file=sys.stderr)
        raise RuntimeError(f"native avatar export failed with {completed.returncode}")
    observed = {}
    for line in completed.stdout.splitlines():
        match = REAL_LINE.fullmatch(line)
        if match is None:
            raise AssertionError(f"unexpected native output line: {line!r}")
        action, direction, frame, delay, rgba, layout = match.groups()
        key = (int(action), int(direction), int(frame))
        assert key not in observed, f"duplicate native frame: {key}"
        observed[key] = (int(delay), rgba, layout)
    return observed


def reference_source(shdata: Path) -> AvatarSource:
    entries = [Entry(*row) for row in _live_rows("player.pak", str(shdata / "player.pak"))]
    perinfo = shdata / "perinfo.pak"
    entries.append(Entry("perinfo.pak", "perinfo.pak", "perinfo.pak", ".pak",
                         0, perinfo.stat().st_size, -1))
    return AvatarSource(PakSource(entries, str(shdata)))


def reference_observations(avatar: AvatarSource):
    observed = {}
    for action in (10, 30):
        for direction in range(8):
            body = avatar.part_anim("player.pak", "body", 200, action, direction)
            head = avatar.part_anim("player.pak", "head", 105, action, direction)
            assert body is not None and head is not None
            frames, delay = avatar.assemble(
                "player.pak", {"body": 200, "head": 105}, action, direction)
            assert frames and delay > 0
            total = max(len(body.frames), len(head.frames))
            assert len(frames) == total
            for index, frame in enumerate(frames):
                order = []
                for part, model, anim in (("body", 200, body), ("head", 105, head)):
                    source_index = index % len(anim.frames)
                    key = (anim.keys[min(source_index, len(anim.keys) - 1)]
                           if anim.keys else 99)
                    order.append((key, part, model, anim, source_index))
                order.sort(key=lambda item: (item[0], item[1]))
                rgba = fnv_update(FNV_BASIS, frame.convert("RGBA").tobytes())
                observed[(action, direction, index)] = (
                    delay, f"{rgba:x}", f"{layout_hash(order):x}")
    return observed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--shdata", type=Path, required=True)
    args = parser.parse_args()
    for name in ("player.pak", "perinfo.pak"):
        if not (args.shdata / name).is_file():
            raise FileNotFoundError(args.shdata / name)

    actual = native_observations(args.native, args.shdata)
    expected = reference_observations(reference_source(args.shdata))
    assert len(expected) == 152, f"unexpected reference frame count: {len(expected)}"
    missing = sorted(expected.keys() - actual.keys())
    extra = sorted(actual.keys() - expected.keys())
    assert not missing and not extra, f"frame keys differ: missing={missing}, extra={extra}"
    mismatches = [(key, expected[key], actual[key]) for key in sorted(expected)
                  if expected[key] != actual[key]]
    if mismatches:
        for key, reference, native in mismatches[:8]:
            print(f"MISMATCH action={key[0]} direction={key[1]} frame={key[2]} "
                  f"python(delay, rgba, layout)={reference} native={native}")
        raise AssertionError(f"{len(mismatches)} avatar frames differ")
    print(f"PASS: {len(expected)} frames across 16 sequences; "
          "frame counts, delays, anchors, layer order, and RGBA match")


if __name__ == "__main__":
    main()
