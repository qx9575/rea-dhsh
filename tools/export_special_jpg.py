#!/usr/bin/env python
"""Export private FF D8 FF C8 JPEGs (510 files) to PNG.

Usage (from the repository root):
    python tools/export_special_jpg.py --sample 10 --seed 1 \
        --out extracted/_batches/special-jpg-b0 \
        --manifest manifests/batches/special-jpg-b0.csv
    python tools/export_special_jpg.py --sample 50 --seed 2 ...
    python tools/export_special_jpg.py --all --jobs 8 \
        --out extracted/special_jpg_png \
        --manifest manifests/batches/special-jpg-full.csv

Only files starting with FF D8 FF C8 are selected.  Output mirrors the
source-relative path under --out with a .png extension.  Existing PNGs are
skipped, so batches can be resumed.
"""
from __future__ import annotations

import argparse
import csv
import os
import random
import sys
import time
from multiprocessing import Pool

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

DEFAULT_ROOT = os.path.join(ROOT, "extracted", "images-standard")
MAGIC = b"\xff\xd8\xff\xc8"


def iter_private(root: str):
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if not name.lower().endswith(".jpg"):
                continue
            path = os.path.join(dirpath, name)
            try:
                with open(path, "rb") as fh:
                    if fh.read(4) != MAGIC:
                        continue
            except OSError:
                continue
            yield path


def export_one(task: tuple[str, str, str, int]) -> dict:
    src, dst, rel, png_level = task
    row = {"path": rel.replace("\\", "/"), "version": "", "width": "",
           "height": "", "pixels": "", "in_bytes": "", "out_bytes": "",
           "elapsed_s": "", "out": dst, "error": ""}
    started = time.time()
    try:
        from dhsh.formats import special_jpg as sj
        from PIL import Image

        row["in_bytes"] = os.path.getsize(src)
        jpg = sj.parse(open(src, "rb").read())
        img = sj.decode(jpg, fast=True)
        if img.shape[:2] != (jpg.height, jpg.width):
            raise ValueError(f"shape {img.shape} != {jpg.width}x{jpg.height}")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        Image.fromarray(img).save(dst, compress_level=png_level)
        row["version"] = jpg.version
        row["width"] = jpg.width
        row["height"] = jpg.height
        row["pixels"] = jpg.width * jpg.height
        row["out_bytes"] = os.path.getsize(dst)
    except Exception as exc:  # noqa: BLE001
        row["error"] = f"{type(exc).__name__}: {exc}"
    row["elapsed_s"] = round(time.time() - started, 3)
    return row


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--out", default=os.path.join(ROOT, "extracted",
                                                  "special_jpg_png"))
    ap.add_argument("--manifest", default=None)
    ap.add_argument("--sample", type=int, default=0)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--png-level", type=int, default=1,
                    help="zlib level for PNG (0 fastest, 6 smallest)")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if not args.all and not args.sample and not args.limit and not args.dry_run:
        ap.error("refusing full corpus without --all; use --sample/--limit first")

    files = sorted(iter_private(args.root))
    print(f"root {args.root}: {len(files)} private JPEG")
    if args.limit:
        files = files[: args.limit]
    if args.sample and args.sample < len(files):
        random.Random(args.seed).shuffle(files)
        files = files[: args.sample]

    tasks = []
    skipped = 0
    for src in files:
        rel = os.path.relpath(src, args.root)
        dst = os.path.join(args.out, os.path.splitext(rel)[0] + ".png")
        if os.path.exists(dst):
            skipped += 1
            continue
        tasks.append((src, dst, rel, args.png_level))

    if args.dry_run:
        print(f"would export {len(tasks)} PNG ({skipped} existing would be skipped)")
        return 0

    started = time.time()
    rows = []
    if tasks:
        with Pool(args.jobs) as pool:
            for i, row in enumerate(pool.imap_unordered(export_one, tasks), 1):
                rows.append(row)
                if i % 25 == 0 or i == len(tasks):
                    print(f"  {i}/{len(tasks)} "
                          f"{'ERR ' + row['path'] if row['error'] else ''}")
    elapsed = time.time() - started

    errors = [r for r in rows if r["error"]]
    pixels = sum(int(r["pixels"]) for r in rows if r["pixels"])
    out_bytes = sum(int(r["out_bytes"]) for r in rows if r["out_bytes"])
    ok = [r for r in rows if not r["error"]]

    if args.manifest:
        os.makedirs(os.path.dirname(os.path.abspath(args.manifest)), exist_ok=True)
        fields = ["path", "version", "width", "height", "pixels", "in_bytes",
                  "out_bytes", "elapsed_s", "out", "error"]
        with open(args.manifest, "w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=fields)
            w.writeheader()
            w.writerows(sorted(rows, key=lambda r: r["path"]))

    print(f"special JPEG -> PNG: {len(ok)}/{len(tasks)} exported, "
          f"{skipped} skipped, {len(errors)} errors, "
          f"{pixels} px, {out_bytes / 1048576:.1f} MiB, "
          f"{elapsed:.1f}s ({args.jobs} jobs)")
    for r in errors[:10]:
        print("  error:", r["path"], r["error"])
    if args.manifest:
        print("manifest:", args.manifest)
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
