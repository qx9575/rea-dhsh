#!/usr/bin/env python
"""Export CEL collision bitmasks to PNG.

Usage (from the repository root):
    python tools/export_map_cel.py --sample 10 --seed 1 \
        --out extracted/_batches/map-cel-b0 --manifest manifests/batches/map-cel-b0.csv

    python tools/export_map_cel.py --all --jobs 8 \
        --out extracted/map_cel_png --manifest manifests/batches/map-cel-full.csv

White (255) = blocked (bit 1), black (0) = walkable.  CEL dimensions are
cells (typically map pixels / 16).

By default exports the ground collision layer (first bitmap when flags & 1).
Use ``--cel-layer 0`` for the auxiliary second bitmap.
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

from dhsh.formats.map import parse_cel, cel_to_image  # noqa: E402

DEFAULT_ROOTS = (
    ("easy", "extracted/easy"),
    ("0a", "extracted/decoded"),
    ("0b", "extracted/decoded_0b"),
)


def iter_cel(root: str):
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if name.lower().endswith(".cel"):
                yield os.path.join(dirpath, name)


def export_chunk(task: tuple[list[tuple[str, str]], dict]) -> dict:
    items, cfg = task
    rows = []
    errors = []
    counters: Counter = Counter()
    for label, path in items:
        counters["files"] += 1
        try:
            data = open(path, "rb").read()
            cel = parse_cel(data, cfg["cel_layer"])
            img = cel_to_image(data, cfg["cel_layer"])
            rel = os.path.relpath(path, cfg["root_by_label"][label])
            out = os.path.join(cfg["out_root"], label,
                               os.path.splitext(rel)[0] + ".png")
            os.makedirs(os.path.dirname(out), exist_ok=True)
            if cfg["overwrite"] or not os.path.exists(out):
                img.save(out, "PNG", compress_level=cfg["png_level"], optimize=False)
            blocked = sum(bin(b).count("1") for b in cel.bits)
            cells = cel.width * cel.height
            size = os.path.getsize(out)
        except Exception as exc:  # noqa: BLE001
            errors.append((path, f"{type(exc).__name__}: {exc}"))
            continue
        counters["written"] += 1
        counters["cells"] += cells
        counters["bytes"] += size
        rows.append({
            "corpus": label,
            "path": rel.replace(os.sep, "/"),
            "width": cel.width,
            "height": cel.height,
            "flags": cel.flags,
            "layer": cfg["cel_layer"],
            "blocked_ratio": round(blocked / cells, 4) if cells else 0,
            "bytes": size,
            "out": out,
            "error": "",
        })
    return {"rows": rows, "counters": dict(counters), "errors": errors}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", action="append", default=None,
                    help="corpus as LABEL=PATH (repeatable)")
    ap.add_argument("--sample", type=int, default=0)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--png-level", type=int, default=3)
    ap.add_argument("--cel-layer", type=int, choices=(0, 1), default=1,
                    help="1=ground (first bitmap), 0=auxiliary (second)")
    ap.add_argument("--out", default=os.path.join(ROOT, "extracted", "map_cel_png"))
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
        found = sorted(iter_cel(root))
        print(f"root {label}: {len(found)} CEL")
        if args.limit:
            found = found[: args.limit]
        root_by_label[label] = root
        items.extend((label, p) for p in found)

    if args.sample and args.sample < len(items):
        random.Random(args.seed).shuffle(items)
        items = items[: args.sample]

    if args.dry_run:
        print(f"would export {len(items)} CEL masks")
        return 0

    cfg = {
        "out_root": args.out,
        "png_level": args.png_level,
        "overwrite": args.overwrite,
        "cel_layer": args.cel_layer,
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
        os.makedirs(os.path.dirname(os.path.abspath(args.manifest)), exist_ok=True)
        fields = ["corpus", "path", "width", "height", "flags", "layer",
                  "blocked_ratio", "bytes", "out", "error"]
        with open(args.manifest, "w", newline="", encoding="utf-8") as fh:
            writer = csv.DictWriter(fh, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    print(f"CEL -> PNG: {counters['written']}/{counters['files']} masks, "
          f"{counters['cells']:,} cells, {counters['bytes'] / 1048576:.2f} MiB, "
          f"{len(errors)} errors, {elapsed:.1f}s")
    for path, why in errors[:20]:
        print("  error:", path, why)
    if args.manifest:
        print("manifest:", args.manifest)
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
