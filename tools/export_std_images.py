#!/usr/bin/env python
"""Batch-convert standard image formats (TGA/BMP/PSD) to PNG.

Usage (from the repository root):
    python tools/export_std_images.py --sample 40 --seed 1 \
        --out extracted/_batches/std-b0 --manifest manifests/batches/std-b0.csv

    python tools/export_std_images.py --all --jobs 8 \
        --out extracted/std_png --manifest manifests/batches/std-full.csv

Each file is written to ``<out>/<corpus>/<relative path>.png``.  Pillow opens
all three formats; alpha channels are preserved (RGBA whenever the source has
an alpha band or transparency metadata, otherwise RGB).
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

DEFAULT_ROOTS = (
    ("std", os.path.join("extracted", "images-standard")),
    ("0a", os.path.join("extracted", "decoded")),
    ("0b", os.path.join("extracted", "decoded_0b")),
)
EXTS = (".tga", ".bmp", ".psd")


def iter_images(root: str, exts=EXTS):
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if os.path.splitext(name)[1].lower() in exts:
                yield os.path.join(dirpath, name)


def mode_of(path: str) -> tuple[str, int]:
    """Cheap grouping key for stratified sampling.

    TGA carries an image-type byte at offset 2; BMP/PSD group by extension.
    """
    ext = os.path.splitext(path)[1].lower()
    if ext == ".tga":
        try:
            with open(path, "rb") as fh:
                head = fh.read(18)
            return (ext, head[2] if len(head) >= 3 else -1)
        except OSError:
            return (ext, -1)
    return (ext, 0)


def stratified_sample(items: list[tuple[str, str]], n: int, seed: int):
    rng = random.Random(seed)
    groups: dict[tuple[str, int], list[tuple[str, str]]] = {}
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
    by_ext: Counter = Counter()
    errors: list[tuple[str, str]] = []

    from PIL import Image

    for label, path in items:
        counters["files"] += 1
        ext = os.path.splitext(path)[1].lower()
        t0 = time.perf_counter()
        try:
            size_in = os.path.getsize(path)
            with Image.open(path) as img:
                img.load()
                has_alpha = "A" in img.getbands() or "transparency" in img.info
                conv = "RGBA" if has_alpha else "RGB"
                img = img.convert(conv)
                width, height = img.size
                rel = os.path.relpath(path, cfg["root_by_label"][label])
                out = os.path.join(
                    out_root, label, os.path.splitext(rel)[0] + ".png"
                )
                if not overwrite and os.path.exists(out):
                    size_out = os.path.getsize(out)
                    counters["written"] += 1
                    counters["bytes_out"] += size_out
                    by_ext[ext] += 1
                    rows.append({
                        "corpus": label,
                        "path": rel.replace(os.sep, "/"),
                        "ext": ext,
                        "mode": conv,
                        "width": width,
                        "height": height,
                        "bytes_in": size_in,
                        "bytes_out": size_out,
                        "out": out,
                        "elapsed": round(time.perf_counter() - t0, 3),
                        "error": "",
                    })
                    continue
                os.makedirs(os.path.dirname(out), exist_ok=True)
                img.save(out, "PNG", compress_level=png_level, optimize=False)
                size_out = os.path.getsize(out)
        except Exception as exc:  # noqa: BLE001
            errors.append((path, f"{type(exc).__name__}: {exc}"))
            continue
        counters["written"] += 1
        counters["pixels"] += width * height
        counters["bytes_out"] += size_out
        by_ext[ext] += 1
        rows.append({
            "corpus": label,
            "path": rel.replace(os.sep, "/"),
            "ext": ext,
            "mode": conv,
            "width": width,
            "height": height,
            "bytes_in": size_in,
            "bytes_out": size_out,
            "out": out,
            "elapsed": round(time.perf_counter() - t0, 3),
            "error": "",
        })
    return {
        "rows": rows,
        "counters": dict(counters),
        "by_ext": dict(by_ext),
        "errors": errors,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", action="append", default=None,
                    help="corpus as LABEL=PATH (repeatable); defaults to std/0a/0b")
    ap.add_argument("--sample", type=int, default=0,
                    help="stratified sample size across all roots")
    ap.add_argument("--limit", type=int, default=0,
                    help="max files per root (after sampling)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--png-level", type=int, default=3)
    ap.add_argument("--out", default=os.path.join(ROOT, "extracted", "std_png"))
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
        found = sorted(iter_images(root))
        print(f"root {label}: {root} ({len(found)} images)")
        if args.limit:
            found = found[: args.limit]
        root_by_label[label] = root
        items.extend((label, path) for path in found)

    if args.sample and args.sample < len(items):
        items = stratified_sample(items, args.sample, args.seed)

    if args.dry_run:
        exts: Counter = Counter(os.path.splitext(p)[1].lower() for _l, p in items)
        print(f"would convert {len(items)} files: {dict(exts)}")
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
    by_ext: Counter = Counter()
    rows: list[dict] = []
    errors: list[tuple[str, str]] = []
    for part in parts:
        counters.update(part["counters"])
        by_ext.update(part["by_ext"])
        rows.extend(part["rows"])
        errors.extend(part["errors"])
    elapsed = time.time() - started

    if args.manifest:
        os.makedirs(os.path.dirname(os.path.abspath(args.manifest)), exist_ok=True)
        fields = ["corpus", "path", "ext", "mode", "width", "height",
                  "bytes_in", "bytes_out", "out", "elapsed", "error"]
        with open(args.manifest, "w", newline="", encoding="utf-8") as fh:
            writer = csv.DictWriter(fh, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    print(f"std images -> PNG: {counters['written']}/{counters['files']} files, "
          f"{counters['bytes_out'] / 1048576:.1f} MiB out, "
          f"{len(errors)} errors, {elapsed:.1f}s")
    print("by ext", dict(sorted(by_ext.items())))
    for path, why in errors[:20]:
        print("  error:", path, why)
    if args.manifest:
        print("manifest:", args.manifest)
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
