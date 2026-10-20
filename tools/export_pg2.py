#!/usr/bin/env python
"""Batch-export PG2 frames to PNG with parallel workers.

Usage (from the repository root):
    python tools/export_pg2.py --sample 10 --seed 1 \
        --out extracted/_batches/pg2-b0 --manifest manifests/batches/pg2-b0.csv

    python tools/export_pg2.py --all --jobs 8 \
        --out extracted/pg2_png --manifest manifests/batches/pg2-full.csv

Each PG2 frame payload is an AP block (24-byte A16 header + RLE streams), so
the NumPy A16 decoder is reused.  Output layout:
``<out>/<corpus>/<logical path>/<pg2 stem>/fNNN.png``.
"""
from __future__ import annotations

import argparse
import csv
import os
import random
import sys
import time
from collections import Counter
from multiprocessing import Pool

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from dhsh.formats.pg2 import iter_frames, frame_block  # noqa: E402

try:
    from dhsh.formats.a16_fast import a16_to_rgba_fast
    FAST = True
except ImportError:  # pragma: no cover - environment dependent
    FAST = False

DEFAULT_ROOTS = (
    ("t0", os.path.join("extracted", "misc")),
    ("0a", os.path.join("extracted", "decoded")),
    ("0b", os.path.join("extracted", "decoded_0b")),
)


def iter_pg2(root: str):
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if name.lower().endswith(".pg2"):
                yield os.path.join(dirpath, name)


def export_chunk(task: tuple[list[tuple[str, str]], dict]) -> dict:
    items, cfg = task
    out_root = cfg["out_root"]
    png_level = cfg["png_level"]
    overwrite = cfg["overwrite"]
    rows: list[dict] = []
    counters: Counter = Counter()
    errors: list[tuple[str, str]] = []

    for label, path in items:
        counters["files"] += 1
        t0 = time.perf_counter()
        try:
            with open(path, "rb") as fh:
                data = fh.read()
            _hdr, frames = iter_frames(data)
        except Exception as exc:  # noqa: BLE001
            errors.append((path, f"{type(exc).__name__}: {exc}"))
            continue

        rel = os.path.relpath(path, cfg["root_by_label"][label])
        out_dir = os.path.join(out_root, label, os.path.splitext(rel)[0])
        os.makedirs(out_dir, exist_ok=True)
        written = 0
        pixels = 0
        nbytes = 0
        for frame in frames:
            png = os.path.join(out_dir, f"f{frame.index:03d}.png")
            try:
                if not overwrite and os.path.exists(png):
                    written += 1
                    nbytes += os.path.getsize(png)
                    pixels += frame.width * frame.height
                    continue
                block = frame_block(data, frame)
                if FAST:
                    img = a16_to_rgba_fast(block)
                else:
                    from dhsh.formats.a16 import decode_a16_rgba
                    img = decode_a16_rgba(block)
                img.save(png, "PNG", compress_level=png_level, optimize=False)
                written += 1
                pixels += img.width * img.height
                nbytes += os.path.getsize(png)
            except Exception as exc:  # noqa: BLE001
                errors.append((path, f"frame {frame.index}: {type(exc).__name__}: {exc}"))
        counters["frames"] += len(frames)
        counters["written"] += written
        counters["pixels"] += pixels
        counters["bytes"] += nbytes
        if written != len(frames):
            counters["partial"] += 1
        rows.append({
            "corpus": label,
            "path": rel.replace(os.sep, "/"),
            "frames": len(frames),
            "written": written,
            "pixels": pixels,
            "bytes": nbytes,
            "out_dir": out_dir,
            "elapsed": round(time.perf_counter() - t0, 3),
            "error": "",
        })
    return {"rows": rows, "counters": dict(counters), "errors": errors}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", action="append", default=None,
                    help="corpus as LABEL=PATH (repeatable); defaults to t0/0a/0b")
    ap.add_argument("--sample", type=int, default=0)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--png-level", type=int, default=3)
    ap.add_argument("--out", default=os.path.join(ROOT, "extracted", "pg2_png"))
    ap.add_argument("--manifest", default=None)
    ap.add_argument("--overwrite", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()

    if not args.all and not args.sample and not args.limit and not args.dry_run:
        ap.error("refusing full corpus without --all; use --sample/--limit first")

    roots: list[tuple[str, str]] = []
    if args.root:
        for spec in args.root:
            if "=" not in spec:
                ap.error(f"--root expects LABEL=PATH, got {spec!r}")
            label, rel = spec.split("=", 1)
            roots.append((label, os.path.abspath(rel)))
    else:
        roots = [(label, os.path.join(ROOT, rel)) for label, rel in DEFAULT_ROOTS]

    items: list[tuple[str, str]] = []
    root_by_label: dict[str, str] = {}
    for label, root in roots:
        if not os.path.isdir(root):
            ap.error(f"root not found: {root}")
        found = sorted(iter_pg2(root))
        print(f"root {label}: {root} ({len(found)} PG2)")
        if args.limit:
            found = found[: args.limit]
        root_by_label[label] = root
        items.extend((label, path) for path in found)

    if args.sample and args.sample < len(items):
        random.Random(args.seed).shuffle(items)
        items = items[: args.sample]

    if args.dry_run:
        print(f"would convert {len(items)} files")
        return 0

    cfg = {
        "out_root": args.out,
        "png_level": args.png_level,
        "overwrite": args.overwrite,
        "root_by_label": root_by_label,
    }
    started = time.time()
    nchunks = max(1, args.jobs * 2)
    chunks = [(items[i::nchunks], cfg) for i in range(nchunks)]
    chunks = [c for c in chunks if c[0]]
    with Pool(args.jobs) as pool:
        parts = pool.map(export_chunk, chunks)

    counters: Counter = Counter()
    rows: list[dict] = []
    errors: list[tuple[str, str]] = []
    for part in parts:
        counters.update(part["counters"])
        rows.extend(part["rows"])
        errors.extend(part["errors"])
    elapsed = time.time() - started

    if args.manifest:
        os.makedirs(os.path.dirname(os.path.abspath(args.manifest)), exist_ok=True)
        fields = ["corpus", "path", "frames", "written", "pixels", "bytes",
                  "out_dir", "elapsed", "error"]
        with open(args.manifest, "w", newline="", encoding="utf-8") as fh:
            writer = csv.DictWriter(fh, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    print(f"PG2 -> PNG: {counters['written']}/{counters['frames']} frames, "
          f"{counters['pixels']:,} px, {counters['bytes'] / 1048576:.1f} MiB out, "
          f"{len(errors)} errors, {elapsed:.1f}s")
    for path, why in errors[:20]:
        print("  error:", path, why)
    if args.manifest:
        print("manifest:", args.manifest)
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
