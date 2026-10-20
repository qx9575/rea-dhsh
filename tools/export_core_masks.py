#!/usr/bin/env python
"""Export MSK/core cover-mask records to PNG.

Each record is a run-length encoded 1-bit mask:
    u16 word = (covered << 15) | run_length
Runs are row-major, never cross a scanline, and each row sums to width.
Records are placed on the map by the placement table (x, y = mask top-left).

Usage (from the repository root):
    python tools/export_core_masks.py --sample 10 --seed 1 \
        --out extracted/_batches/core-mask-b0 \
        --manifest manifests/batches/core-mask-b0.csv
    python tools/export_core_masks.py --sample 50 --seed 2 ...
    python tools/export_core_masks.py --all --jobs 8 \
        --out extracted/core_masks \
        --manifest manifests/batches/core-mask-full.csv

Output layout: <out>/<corpus>/<relative-path-without-ext>/<record:04d>.png
White (255) = covered (bit 1), black = transparent.
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

from dhsh.formats.map import decode_mask, parse_msk  # noqa: E402

DEFAULT_ROOTS = (
    ("easy", "extracted/easy"),
    ("0a", "extracted/decoded"),
    ("0b", "extracted/decoded_0b"),
)


def iter_masks(root: str):
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if name.lower().endswith((".msk", ".core")):
                yield os.path.join(dirpath, name)


def export_chunk(task: tuple[list[tuple[str, str]], dict]) -> dict:
    items, cfg = task
    rows = []
    errors = []
    counters: Counter = Counter()
    from PIL import Image

    for label, path in items:
        counters["files"] += 1
        try:
            data = open(path, "rb").read()
            msk = parse_msk(data)
        except Exception as exc:  # noqa: BLE001
            errors.append((path, f"{type(exc).__name__}: {exc}"))
            continue
        if not msk.records:
            counters["empty_files"] += 1
            continue
        rel = os.path.relpath(path, cfg["root_by_label"][label])
        # keep the extension in the directory name: X.core/ and X.MSK/
        # are distinct containers (legacy MSK records carry BMP names).
        outdir = os.path.join(cfg["out_root"], label, rel)
        os.makedirs(outdir, exist_ok=True)
        for i, rec in enumerate(msk.records):
            counters["records"] += 1
            out = os.path.join(outdir, f"{i:04d}.png")
            row = {
                "corpus": label,
                "path": rel.replace(os.sep, "/"),
                "record": i,
                "name": rec.name,
                "width": rec.width,
                "height": rec.height,
                "pixels": "",
                "covered": "",
                "runs": "",
                "blob_bytes": rec.data_size,
                "bytes": "",
                "out": out,
                "error": "",
            }
            try:
                if not cfg["overwrite"] and os.path.exists(out):
                    size = os.path.getsize(out)
                    counters["skipped"] += 1
                else:
                    mask = decode_mask(msk, i)
                    row["covered"] = int(mask.sum())
                    img = Image.fromarray(mask * 255)
                    img.save(out, "PNG", compress_level=cfg["png_level"],
                             optimize=False)
                    size = os.path.getsize(out)
                    counters["written"] += 1
                row["pixels"] = rec.width * rec.height
                row["runs"] = rec.data_size // 2
                row["bytes"] = size
                counters["pixels"] += row["pixels"]
                counters["runs"] += row["runs"]
                counters["bytes"] += size
            except Exception as exc:  # noqa: BLE001
                row["error"] = f"{type(exc).__name__}: {exc}"
                errors.append((f"{path}#{i}", row["error"]))
            rows.append(row)
    return {"rows": rows, "counters": dict(counters), "errors": errors}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", action="append", default=None,
                    help="corpus as LABEL=PATH (repeatable)")
    ap.add_argument("--sample", type=int, default=0,
                    help="random sample of files (after --limit)")
    ap.add_argument("--limit", type=int, default=0,
                    help="max files per corpus root")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--png-level", type=int, default=6)
    ap.add_argument("--out", default=os.path.join(ROOT, "extracted", "core_masks"))
    ap.add_argument("--manifest", default=None)
    ap.add_argument("--overwrite", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()

    if not args.all and not args.sample and not args.limit and not args.dry_run:
        ap.error("refusing full corpus without --all; use --sample/--limit first")

    roots = []
    if args.root:
        for spec in args.root:
            if "=" not in spec:
                ap.error(f"--root expects LABEL=PATH, got {spec!r}")
            label, rel = spec.split("=", 1)
            roots.append((label, os.path.abspath(rel)))
    else:
        roots = [(label, os.path.join(ROOT, rel)) for label, rel in DEFAULT_ROOTS]

    items = []
    root_by_label = {}
    for label, root in roots:
        found = sorted(iter_masks(root))
        print(f"root {label}: {len(found)} MSK/core")
        if args.limit:
            found = found[: args.limit]
        root_by_label[label] = root
        items.extend((label, p) for p in found)

    if args.sample and args.sample < len(items):
        random.Random(args.seed).shuffle(items)
        items = items[: args.sample]

    if args.dry_run:
        print(f"would export records from {len(items)} MSK/core files")
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
    rows = []
    errors = []
    for part in parts:
        counters.update(part["counters"])
        rows.extend(part["rows"])
        errors.extend(part["errors"])
    elapsed = time.time() - started

    if args.manifest:
        os.makedirs(os.path.dirname(os.path.abspath(args.manifest)),
                    exist_ok=True)
        fields = ["corpus", "path", "record", "name", "width", "height",
                  "pixels", "covered", "runs", "blob_bytes", "bytes",
                  "out", "error"]
        with open(args.manifest, "w", newline="", encoding="utf-8") as fh:
            writer = csv.DictWriter(fh, fieldnames=fields)
            writer.writeheader()
            writer.writerows(sorted(rows, key=lambda r: (
                r["corpus"], r["path"], r["record"])))

    print(f"core masks -> PNG: {counters['records']} records from "
          f"{counters['files']} files ({counters['skipped']} existing, "
          f"{counters['empty_files']} empty), "
          f"{counters['pixels']:,} px, {counters['runs']:,} runs, "
          f"{counters['bytes'] / 1048576:.2f} MiB, "
          f"{len(errors)} errors, {elapsed:.1f}s")
    for path, why in errors[:20]:
        print("  error:", path, why)
    if args.manifest:
        print("manifest:", args.manifest)
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
