#!/usr/bin/env python
"""Batch-convert standalone A16 images to PNG with parallel workers.

Usage (from the repository root):
    # sample 30 files across the three corpora
    python tools/export_a16.py --sample 30 --seed 1 \
        --out extracted/_batches/a16-b0 --manifest manifests/batches/a16-b0.csv

    # full corpus (29,716 files; explicit --all required)
    python tools/export_a16.py --all --jobs 8 --png-level 3 \
        --out extracted/a16_png --manifest manifests/batches/a16-full.csv

Each file is written to ``<out>/<corpus>/<relative path>.png``.  The NumPy
fast path in ``dhsh.formats.a16_fast`` is byte-identical to the reference
decoder for compression 0/1/2/5 and alpha 0/2 (the whole scanned corpus).
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

from dhsh.formats.a16 import parse_header  # noqa: E402

try:
    from dhsh.formats.a16_fast import a16_to_rgba_fast
    FAST = True
except ImportError:  # pragma: no cover - environment dependent
    FAST = False

DEFAULT_ROOTS = (
    ("t0", os.path.join("extracted", "images-proprietary")),
    ("0a", os.path.join("extracted", "decoded")),
    ("0b", os.path.join("extracted", "decoded_0b")),
)


def iter_a16(root: str):
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if name.lower().endswith(".a16"):
                yield os.path.join(dirpath, name)


def mode_of(path: str) -> tuple[int, int]:
    """Read only the header mode fields; unreadable files get (-1, -1)."""
    try:
        with open(path, "rb") as fh:
            head = fh.read(16)
    except OSError:
        return (-1, -1)
    if len(head) < 16:
        return (-1, -1)
    return (head[14], head[15])


def stratified_sample(items: list[tuple[str, str]], n: int, seed: int):
    """Round-robin by (compression, alpha_mode) so rare modes are covered."""
    rng = random.Random(seed)
    groups: dict[tuple[int, int], list[tuple[str, str]]] = {}
    for item in items:
        groups.setdefault(mode_of(item[1]), []).append(item)
    keys = sorted(groups)
    for bucket in groups.values():
        rng.shuffle(bucket)
    picked: list[tuple[str, str]] = []
    i = 0
    while len(picked) < n and any(groups[k] for k in keys):
        key = keys[i % len(keys)]
        i += 1
        if groups[key]:
            picked.append(groups[key].pop())
    return picked


def export_chunk(task: tuple[list[tuple[str, str]], dict]) -> dict:
    items, cfg = task
    out_root = cfg["out_root"]
    png_level = cfg["png_level"]
    overwrite = cfg["overwrite"]
    rows: list[dict] = []
    counters: Counter = Counter()
    comps: Counter = Counter()
    alphas: Counter = Counter()
    errors: list[tuple[str, str]] = []

    for label, path in items:
        counters["files"] += 1
        t0 = time.perf_counter()
        try:
            with open(path, "rb") as fh:
                data = fh.read()
            hdr = parse_header(data)
        except Exception as exc:  # noqa: BLE001
            errors.append((path, f"{type(exc).__name__}: {exc}"))
            continue
        comps[hdr.compression] += 1
        alphas[hdr.alpha_mode] += 1

        rel = os.path.relpath(path, cfg["root_by_label"][label])
        out = os.path.join(out_root, label, os.path.splitext(rel)[0] + ".png")
        counters["pixels"] += hdr.width * hdr.height
        counters["bytes_in"] += len(data)
        if not overwrite and os.path.exists(out):
            size_out = os.path.getsize(out)
            counters["written"] += 1
            counters["bytes_out"] += size_out
            rows.append({
                "corpus": label,
                "path": rel.replace(os.sep, "/"),
                "width": hdr.width,
                "height": hdr.height,
                "compression": hdr.compression,
                "alpha_mode": hdr.alpha_mode,
                "bytes_in": len(data),
                "bytes_out": size_out,
                "out": out,
                "elapsed": round(time.perf_counter() - t0, 3),
                "error": "",
            })
            continue
        try:
            os.makedirs(os.path.dirname(out), exist_ok=True)
            if FAST:
                img = a16_to_rgba_fast(data)
            else:
                from dhsh.formats.a16 import decode_a16, rgb565_to_rgba
                from PIL import Image
                dec = decode_a16(data)
                img = Image.frombytes(
                    "RGBA", (hdr.width, hdr.height),
                    rgb565_to_rgba(dec.pixels, dec.alpha),
                )
            img.save(out, "PNG", compress_level=png_level, optimize=False)
            size_out = os.path.getsize(out)
            counters["written"] += 1
            counters["bytes_out"] += size_out
        except Exception as exc:  # noqa: BLE001
            errors.append((path, f"{type(exc).__name__}: {exc}"))
            continue
        rows.append({
            "corpus": label,
            "path": rel.replace(os.sep, "/"),
            "width": hdr.width,
            "height": hdr.height,
            "compression": hdr.compression,
            "alpha_mode": hdr.alpha_mode,
            "bytes_in": len(data),
            "bytes_out": size_out,
            "out": out,
            "elapsed": round(time.perf_counter() - t0, 3),
            "error": "",
        })
    return {
        "rows": rows,
        "counters": dict(counters),
        "comps": dict(comps),
        "alphas": dict(alphas),
        "errors": errors,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", action="append", default=None,
                    help="corpus as LABEL=PATH (repeatable); defaults to t0/0a/0b")
    ap.add_argument("--sample", type=int, default=0,
                    help="random sample size across all roots")
    ap.add_argument("--limit", type=int, default=0,
                    help="max files per root (after sampling)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--png-level", type=int, default=3)
    ap.add_argument("--out", default=os.path.join(ROOT, "extracted", "a16_png"))
    ap.add_argument("--manifest", default=None)
    ap.add_argument("--overwrite", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--all", action="store_true",
                    help="required to convert the full corpus without a limit")
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
        found = sorted(iter_a16(root))
        print(f"root {label}: {root} ({len(found)} A16)")
        if args.limit:
            found = found[: args.limit]
        root_by_label[label] = root
        items.extend((label, path) for path in found)

    if args.sample and args.sample < len(items):
        items = stratified_sample(items, args.sample, args.seed)

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
    nchunks = max(1, args.jobs * 4)
    chunks = [(items[i::nchunks], cfg) for i in range(nchunks)]
    chunks = [c for c in chunks if c[0]]
    with Pool(args.jobs) as pool:
        parts = pool.map(export_chunk, chunks)

    counters: Counter = Counter()
    comps: Counter = Counter()
    alphas: Counter = Counter()
    rows: list[dict] = []
    errors: list[tuple[str, str]] = []
    for part in parts:
        counters.update(part["counters"])
        comps.update(part["comps"])
        alphas.update(part["alphas"])
        rows.extend(part["rows"])
        errors.extend(part["errors"])
    elapsed = time.time() - started

    if args.manifest:
        os.makedirs(os.path.dirname(os.path.abspath(args.manifest)), exist_ok=True)
        with open(args.manifest, "w", newline="", encoding="utf-8") as fh:
            writer = csv.DictWriter(fh, fieldnames=list(rows[0].keys()) if rows else
                                    ["corpus", "path", "width", "height", "compression",
                                     "alpha_mode", "bytes_in", "bytes_out", "out",
                                     "elapsed", "error"])
            writer.writeheader()
            writer.writerows(rows)

    files = counters["files"]
    written = counters["written"]
    print(f"A16 -> PNG: {written}/{files} files, {counters['pixels']:,} px, "
          f"{counters['bytes_out'] / 1048576:.1f} MiB out, "
          f"{len(errors)} errors, {elapsed:.1f}s")
    print("compressions", dict(sorted(comps.items())))
    print("alpha_modes", dict(sorted(alphas.items())))
    for path, why in errors[:20]:
        print("  error:", path, why)
    if args.manifest:
        print("manifest:", args.manifest)
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
