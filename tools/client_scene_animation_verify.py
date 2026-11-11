#!/usr/bin/env python
"""Verify native MAN animation frames against direct PAK decoding."""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np

import client_scene_verify as scene
from dhsh.formats.a16 import decode_a16_rgba, parse_header
from dhsh.formats.pif import parse_pif


def load_lantern_reference(shdata: Path):
    index = {}
    scene.index_pak(shdata / "config.pak", index)
    scene.index_pak(shdata / "map.pak", index)
    ini, _ = scene.load_map(index, "YingTianFu")
    man_path = ini.get("ext", {}).get("ANIFILE", "").strip()
    entries = ()
    for candidate in scene.path_variants(man_path):
        found = index.get(scene.normalize_logical_path(candidate))
        if found is not None:
            pak_path, entry = found
            entries = scene.parse_man_entries(scene.read_nz_entry(pak_path, entry))
            break
    lantern = next(
        (entry for entry in entries if "denglong1" in entry.path.lower()), None)
    if lantern is None:
        raise ValueError("YingTianFu MAN has no denglong1 entry")

    found_frames = []
    directory = ""
    for variant in scene.animation_dir_variants(lantern.path):
        prefix = scene.normalize_logical_path(variant) + "/"
        indexed = []
        for key, value in index.items():
            if not key.startswith(prefix) or "/" in key[len(prefix):]:
                continue
            name = key[len(prefix):]
            stem, extension = name.rsplit(".", 1)
            if extension.lower() != "a16" or not stem.isdigit():
                continue
            indexed.append((int(stem), len(stem), stem, value))
        if indexed:
            indexed.sort(key=lambda item: item[:3])
            found_frames = indexed
            directory = variant
            break
    if not found_frames:
        raise ValueError("lantern has no direct-PAK A16 frames")

    delays = [80] * len(found_frames)
    pif_found = index.get(scene.normalize_logical_path(directory + "/perinfo.dat"))
    if pif_found is not None:
        pak_path, entry = pif_found
        pif = parse_pif(scene.read_nz_entry(pak_path, entry))
        if len(pif.rows) == len(found_frames):
            delays = [row[5] or 80 for row in pif.rows]

    frames = []
    for _number, _width, _stem, (pak_path, entry) in found_frames:
        data = scene.read_nz_entry(pak_path, entry)
        header = parse_header(data)
        rgba = np.array(decode_a16_rgba(data), dtype=np.uint8)
        frames.append((header, rgba, entry.path))
    return frames, delays


def verify(prefix: Path, frames, delays) -> None:
    lines = prefix.with_suffix(".meta").read_text(encoding="utf-8").splitlines()
    fields = [line.split() for line in lines if line.strip()]
    if not fields or fields[0] != ["frames", str(len(frames))]:
        raise ValueError("native/reference frame counts differ")
    for index, (header, rgba, path) in enumerate(frames):
        actual = fields[index + 1]
        expected = [
            "frame", str(index), str(header.x), str(header.y),
            str(delays[index]), str(rgba.shape[1]), str(rgba.shape[0]),
        ]
        if actual != expected:
            raise ValueError(
                f"frame {index} metadata mismatch: {actual} != {expected} ({path})")
    tail = fields[len(frames) + 1:]
    if tail != [["before", "0"], ["after", "1"], ["shared", "1"]]:
        raise ValueError(f"advance/cache markers differ: {tail}")

    for label, index in (("before", 0), ("after", 1)):
        actual = prefix.with_suffix(f".{label}.rgba").read_bytes()
        expected = np.ascontiguousarray(frames[index][1]).tobytes()
        if actual != expected:
            raise ValueError(
                f"{label} frame RGBA differs from direct PAK frame {index}")
        print(f"{label}: frame={index} rgba_bytes={len(actual)} exact=1")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--shdata", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    args = parser.parse_args()
    try:
        frames, delays = load_lantern_reference(args.shdata)
        verify(args.prefix, frames, delays)
        print(f"PASS: {len(frames)} MAN frames, anchors/delays/metadata/shared cache exact")
        return 0
    except Exception as error:
        print(f"FAIL: {type(error).__name__}: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
