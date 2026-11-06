#!/usr/bin/env python
"""Verify the native static scene viewport against a direct PAK reference.

The reference is built without the C++ client:

* MAP from ``config.pak`` / ``map.pak`` (same logical path rules),
* BACKGROUND PNG from the cache written by ``tools/client_map_cache.py``,
* MAN first frames decoded numerically from the real A16 entries,
* the same source-over blend and placement rules as ``SoftwareRenderer``.

The actual image is the PPM written by ``dhsh_test_scene
--render-ying-tian-fu``.  Every RGB channel must match exactly; a mismatch
returns a nonzero exit code, which makes the CTest verification fail.

The script imports ``dhsh`` from this file's own ``tools`` directory, so it
works inside a worktree as long as the repository tools are present.  It does
not hardcode any build or worktree output path.

Example:

    python tools/client_scene_verify.py \
        --shdata D:/software/dhsh/shdata \
        --cache client/cache/map \
        --actual client/build/tests/ying_tian_fu_800x600.ppm \
        --reference client/build/tests/scene-reference/YingTianFu-reference.png \
        --diff client/build/tests/scene-reference/YingTianFu-diff.png
"""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

TOOLS_ROOT = Path(__file__).resolve().parent
if not (TOOLS_ROOT / "dhsh" / "pak.py").is_file():
    raise SystemExit(
        f"FAIL: cannot find the repository tools/dhsh package next to "
        f"{Path(__file__).resolve()}")
sys.path.insert(0, str(TOOLS_ROOT))

try:
    import numpy as np
    from PIL import Image
except ImportError as error:  # pragma: no cover - environment diagnostic
    raise SystemExit(
        f"FAIL: client_scene_verify.py requires numpy and Pillow: {error}")

from dhsh import nz  # noqa: E402
from dhsh.formats.a16 import (  # noqa: E402
    decode_a16_rgba,
    parse_header as parse_a16_header,
)
from dhsh.formats.map import (  # noqa: E402
    parse_man_entries,
    parse_map_ini,
)
from dhsh.pak import iter_pak  # noqa: E402

INT32_MIN = -(2 ** 31)
INT32_MAX = 2 ** 31 - 1
DEFAULT_MAP = "YingTianFu"
DEFAULT_WIDTH = 800
DEFAULT_HEIGHT = 600
DEFAULT_CAMERA_X = 0
DEFAULT_CAMERA_Y = 0
MAX_FRAME_INDEX = 999
MAX_STEM_WIDTH = 5


def normalize_logical_path(path: str) -> str:
    """Match PakResourceProvider: lowercase, '/', empty separators dropped."""
    parts = [part for part in path.replace("\\", "/").split("/") if part]
    return "/".join(part.lower() for part in parts)


def path_variants(path: str) -> list[str]:
    """Match MapScene::path_variants: literal, map1 sibling, then stripped."""
    normalized = normalize_logical_path(path)
    if not normalized:
        return []
    variants = [normalized]
    if normalized.startswith("map/"):
        rest = normalized[4:]
        for candidate in ("map1/" + rest, rest):
            if candidate not in variants:
                variants.append(candidate)
    elif normalized.startswith("map1/"):
        rest = normalized[5:]
        for candidate in ("map/" + rest, rest):
            if candidate not in variants:
                variants.append(candidate)
    return variants


def animation_dir_variants(path: str) -> list[str]:
    """Match the test's PakManObjectProvider asset_variants (map/ only)."""
    normalized = normalize_logical_path(path)
    if not normalized:
        return []
    variants = [normalized]
    if normalized.startswith("map/"):
        rest = normalized[4:]
        for candidate in ("map1/" + rest, rest):
            if candidate not in variants:
                variants.append(candidate)
    return variants


def index_pak(pak_path: Path, index: dict) -> int:
    """Add one archive's entries; the first mounted occurrence wins."""
    count = 0
    for entry in iter_pak(pak_path):
        key = normalize_logical_path(entry.path)
        if key:
            index.setdefault(key, (pak_path, entry))
            count += 1
    return count


def read_nz_entry(pak_path: Path, entry) -> bytes:
    """Read and strictly decode one NZ leaf, matching NzDecoder."""
    with open(pak_path, "rb") as source:
        source.seek(entry.offset)
        stored = source.read(entry.size)
    if len(stored) != entry.size:
        raise ValueError(
            f"truncated PAK entry {entry.path!r}: "
            f"expected {entry.size} bytes, read {len(stored)}")
    header = nz.parse_header(stored)
    if header is None:
        raise ValueError(f"PAK entry {entry.path!r} is not NZ-wrapped")
    if len(stored) != nz.HEADER_SIZE + header.size2:
        raise ValueError(
            f"PAK entry {entry.path!r} NZ size2 does not match the entry")
    if header.kind == 0 and header.size1 != header.size2:
        raise ValueError(
            f"PAK entry {entry.path!r} stored NZ size1 != size2")
    payload = stored[nz.HEADER_SIZE:]
    decoded = nz.decompress(header.kind, payload, header.size1)
    if len(decoded) != header.size1:
        raise ValueError(
            f"PAK entry {entry.path!r} NZ size mismatch: "
            f"{len(decoded)} != {header.size1}")
    return decoded


def load_map(index: dict, name: str) -> tuple[dict, str]:
    """Parse the same MAP candidates MapScene::load tries, in order."""
    candidates = [
        f"map/{name}.map",
        f"config/map/{name}.map",
        f"{name}.map",
    ]
    for candidate in candidates:
        found = index.get(normalize_logical_path(candidate))
        if found is not None:
            pak_path, entry = found
            return parse_map_ini(read_nz_entry(pak_path, entry)), candidate
    raise ValueError(
        f"MAP entry not found for {name!r}; tried {', '.join(candidates)}")


def index_cache(root: Path) -> dict[str, Path]:
    """Case-insensitive relative-path index over the background cache."""
    index: dict[str, Path] = {}
    for path in sorted(root.rglob("*")):
        if path.is_file():
            key = path.relative_to(root).as_posix().lower()
            index.setdefault(key, path)
    return index


def find_cached_background(cache_index: dict[str, Path],
                           logical_path: str) -> Path:
    """Match CacheResourceProvider: exact normalized path, then .png."""
    for candidate in path_variants(logical_path):
        exact = cache_index.get(candidate)
        if exact is not None:
            return exact
        stem, _extension = os.path.splitext(candidate)
        image = cache_index.get(stem + ".png")
        if image is not None:
            return image
    raise ValueError(
        f"cached background PNG not found for {logical_path!r}; "
        "run tools/client_map_cache.py first")


def first_a16(index: dict, directory: str):
    """Mirror PakManObjectProvider::first_a16 probe order exactly."""
    if not directory:
        return None
    for variant in animation_dir_variants(directory):
        for frame_index in range(MAX_FRAME_INDEX + 1):
            stem = str(frame_index)
            for width in range(1, MAX_STEM_WIDTH + 1):
                if width < len(stem):
                    continue
                name = ("0" * (width - len(stem))) + stem + ".a16"
                found = index.get(normalize_logical_path(
                    variant + "/" + name))
                if found is not None:
                    return found
    return None


def load_first_frame(index: dict, cache: dict, directory: str):
    """Return (RGBA array, x offset, y offset, asset path) or None."""
    key = normalize_logical_path(directory)
    if key in cache:
        return cache[key]
    found = first_a16(index, directory)
    frame = None
    if found is not None:
        pak_path, entry = found
        data = read_nz_entry(pak_path, entry)
        header = parse_a16_header(data)
        rgba = np.array(decode_a16_rgba(data))
        if rgba.ndim != 3 or rgba.shape[2] != 4:
            raise ValueError(
                f"A16 frame {entry.path!r} is not RGBA")
        if rgba.shape[0] == 0 or rgba.shape[1] == 0:
            raise ValueError(
                f"A16 frame {entry.path!r} has zero dimensions")
        frame = (rgba, header.x, header.y, entry.path)
    cache[key] = frame
    return frame


def clamp_i32(value: int) -> int:
    return max(INT32_MIN, min(INT32_MAX, value))


def draw_rgba(canvas: np.ndarray, rgba: np.ndarray, x: int, y: int) -> bool:
    """Source-over blend identical to SoftwareRenderer::draw."""
    canvas_height, canvas_width = canvas.shape[:2]
    source_height, source_width = rgba.shape[:2]
    x0 = max(0, x)
    y0 = max(0, y)
    x1 = min(canvas_width, x + source_width)
    y1 = min(canvas_height, y + source_height)
    if x1 <= x0 or y1 <= y0:
        return False
    source = rgba[y0 - y:y1 - y, x0 - x:x1 - x]
    alpha = source[:, :, 3].astype(np.uint16)[:, :, None]
    if not alpha.any():
        return False
    target = canvas[y0:y1, x0:x1].astype(np.uint16)
    canvas[y0:y1, x0:x1] = (
        (source[:, :, :3].astype(np.uint16) * alpha
         + target * (255 - alpha) + 127) // 255
    ).astype(np.uint8)
    return True


def compose_reference(args, index: dict, cache_index: dict[str, Path]):
    """Build the expected viewport from the MAP, cache and real MAN frames."""
    ini, map_candidate = load_map(index, args.map)
    background_path = ini.get("base", {}).get("BACKGROUND", "").strip()
    if not background_path:
        raise ValueError(f"MAP {args.map!r} has no BACKGROUND")

    background_file = find_cached_background(cache_index, background_path)
    with Image.open(background_file) as image:
        background = np.array(image.convert("RGBA"))

    canvas = np.zeros((args.height, args.width, 3), dtype=np.uint8)
    draw_rgba(canvas, background,
              clamp_i32(-args.camera_x), clamp_i32(-args.camera_y))

    man_path = ini.get("ext", {}).get("ANIFILE", "").strip()
    entries = ()
    man_candidate = None
    if man_path:
        for candidate in path_variants(man_path):
            found = index.get(candidate)
            if found is not None:
                pak_path, entry = found
                entries = parse_man_entries(read_nz_entry(pak_path, entry))
                man_candidate = candidate
                break
        if man_candidate is None:
            print(f"warning: MAN not found: {man_path}", file=sys.stderr)

    frames: dict[str, object] = {}
    resolved = 0
    drawn = 0
    for entry in entries:
        frame = load_first_frame(index, frames, entry.path)
        if frame is None:
            continue
        resolved += 1
        rgba, offset_x, offset_y, _asset = frame
        object_x = clamp_i32(entry.x + offset_x)
        object_y = clamp_i32(entry.y + offset_y)
        if draw_rgba(canvas, rgba,
                     clamp_i32(object_x - args.camera_x),
                     clamp_i32(object_y - args.camera_y)):
            drawn += 1

    stats = {
        "map": map_candidate,
        "background": background_path,
        "background_file": str(background_file),
        "man": man_candidate or "",
        "man_entries": len(entries),
        "frames": resolved,
        "drawn": drawn,
    }
    return canvas, stats


def load_actual(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        return np.array(image.convert("RGB"))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare the native static scene PPM with a direct PAK "
                    "reference; every RGB channel must match exactly.")
    parser.add_argument("--shdata", type=Path, required=True,
                        help="read-only shdata directory containing "
                             "config.pak and map.pak")
    parser.add_argument("--cache", type=Path, required=True,
                        help="background cache written by client_map_cache.py")
    parser.add_argument("--actual", type=Path, required=True,
                        help="PPM written by dhsh_test_scene")
    parser.add_argument("--reference", type=Path, default=None,
                        help="optional path for the reference PNG")
    parser.add_argument("--diff", type=Path, default=None,
                        help="optional path for the amplified diff PNG")
    parser.add_argument("--map", default=DEFAULT_MAP,
                        help=f"map stem (default: {DEFAULT_MAP})")
    parser.add_argument("--width", type=int, default=DEFAULT_WIDTH,
                        help=f"viewport width (default: {DEFAULT_WIDTH})")
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT,
                        help=f"viewport height (default: {DEFAULT_HEIGHT})")
    parser.add_argument("--camera-x", type=int, default=DEFAULT_CAMERA_X,
                        help=f"camera X (default: {DEFAULT_CAMERA_X})")
    parser.add_argument("--camera-y", type=int, default=DEFAULT_CAMERA_Y,
                        help=f"camera Y (default: {DEFAULT_CAMERA_Y})")
    args = parser.parse_args()

    if args.width <= 0 or args.height <= 0:
        parser.error("--width and --height must be positive")
    for name, value in (("--camera-x", args.camera_x),
                        ("--camera-y", args.camera_y)):
        if not INT32_MIN <= value <= INT32_MAX:
            parser.error(f"{name} must fit a signed 32-bit integer")

    try:
        config_pak = args.shdata / "config.pak"
        map_pak = args.shdata / "map.pak"
        for pak_path in (config_pak, map_pak):
            if not pak_path.is_file():
                raise ValueError(f"missing PAK: {pak_path}")
        if not args.cache.is_dir():
            raise ValueError(f"cache directory not found: {args.cache}")

        index: dict = {}
        print(f"indexing {config_pak} ...")
        config_count = index_pak(config_pak, index)
        print(f"indexing {map_pak} ...")
        map_count = index_pak(map_pak, index)
        print(f"indexed {config_count} + {map_count} PAK entries")

        cache_index = index_cache(args.cache)
        print(f"indexed {len(cache_index)} cache file(s) under {args.cache}")

        reference, stats = compose_reference(args, index, cache_index)
        actual = load_actual(args.actual)
        if actual.shape != reference.shape:
            raise ValueError(
                f"actual shape {actual.shape} != reference shape "
                f"{reference.shape}")
        print(
            f"map={stats['map']} background={stats['background']} "
            f"cache={stats['background_file']} man={stats['man']} "
            f"man_entries={stats['man_entries']} frames={stats['frames']} "
            f"drawn={stats['drawn']}")

        difference = np.abs(
            actual.astype(np.int32) - reference.astype(np.int32))
        total_pixels = actual.shape[0] * actual.shape[1]
        exact_pixels = int(np.count_nonzero(np.all(difference == 0, axis=2)))
        differing_channels = int(np.count_nonzero(difference))
        max_delta = int(difference.max()) if difference.size else 0
        print(f"compare: exact_pixels={exact_pixels}/{total_pixels} "
              f"differing_channels={differing_channels} "
              f"max_channel_delta={max_delta}")

        if args.reference is not None:
            args.reference.parent.mkdir(parents=True, exist_ok=True)
            Image.fromarray(reference).save(args.reference)
            print(f"reference: {args.reference}")
        if args.diff is not None:
            args.diff.parent.mkdir(parents=True, exist_ok=True)
            Image.fromarray(
                np.clip(difference * 4, 0, 255).astype(np.uint8)
            ).save(args.diff)
            print(f"diff: {args.diff}")

        if not np.array_equal(actual, reference):
            print("FAIL: actual pixels differ from the direct PAK reference",
                  file=sys.stderr)
            return 1
        print("PASS: actual pixels exactly match the direct PAK reference")
        return 0
    except Exception as error:  # noqa: BLE001 - report a clear CTest failure
        print(f"FAIL: {type(error).__name__}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
