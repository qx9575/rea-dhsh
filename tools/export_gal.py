#!/usr/bin/env python
"""Export `.gal` mapframe channels (AL alpha-only A16 images) to PNG.

The `.gal` referenced by `[mapframe] CHANNEL` uses the A16 header with the
`AL` magic and `stream1_size == 0`: there is no colour stream, only an RLE8
alpha mask in stream2.  The runtime leaves the pixel buffer null and uses
the alpha channel (e.g. FuXianDao's cloud/fog overlay).

This tool writes two PNGs per input:
  <out>/<corpus>/<rel>.png       white RGB + decoded alpha
  <out>/<corpus>/<rel>.mask.png  grayscale alpha (0..255)

Never runs the game.

Usage:
    python tools/export_gal.py --dry-run
    python tools/export_gal.py --out extracted/gal_png \
        --manifest manifests/batches/gal-scan.csv
"""
from __future__ import annotations

import argparse
import csv
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from PIL import Image  # noqa: E402

from dhsh.formats.a16 import decode_a16_rgba, parse_header  # noqa: E402

DEFAULT_ROOTS = (
    ("easy", os.path.join("extracted", "easy")),
    ("0a", os.path.join("extracted", "decoded")),
    ("0b", os.path.join("extracted", "decoded_0b")),
)


def iter_gal(root: str):
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if name.lower().endswith(".gal"):
                yield os.path.join(dirpath, name)


def export_one(label: str, root: str, path: str, out_root: str,
               overwrite: bool):
    t0 = time.perf_counter()
    rel = os.path.relpath(path, ROOT)
    rel_root = os.path.relpath(path, root)
    row = {
        "corpus": label, "path": rel.replace(os.sep, "/"),
        "width": "", "height": "", "compression": "", "alpha_mode": "",
        "stream1": "", "stream2": "", "opaque_pct": "", "transparent_pct": "",
        "rgba_out": "", "mask_out": "", "elapsed": "", "error": "",
    }
    try:
        data = open(path, "rb").read()
        hdr = parse_header(data)
        row.update({
            "width": hdr.width, "height": hdr.height,
            "compression": hdr.compression, "alpha_mode": hdr.alpha_mode,
            "stream1": hdr.stream1_size, "stream2": hdr.stream2_size,
        })
        if hdr.stream1_size != 0:
            raise ValueError(
                f"expected an alpha-only AL layer (stream1=0), got "
                f"{hdr.stream1_size} bytes")
        img = decode_a16_rgba(data)
        alpha = img.getchannel("A")
        hist = alpha.histogram()
        total = hdr.width * hdr.height
        opaque = sum(hist[255:])
        transparent = hist[0]
        row["opaque_pct"] = round(opaque / total * 100, 2)
        row["transparent_pct"] = round(transparent / total * 100, 2)
        rgba_out = os.path.join(
            out_root, label, os.path.splitext(rel_root)[0] + ".png")
        mask_out = os.path.join(
            out_root, label, os.path.splitext(rel_root)[0] + ".mask.png")
        os.makedirs(os.path.dirname(rgba_out), exist_ok=True)
        if overwrite or not os.path.exists(rgba_out):
            img.save(rgba_out, "PNG", compress_level=6)
        if overwrite or not os.path.exists(mask_out):
            alpha.save(mask_out, "PNG", compress_level=6)
        row["rgba_out"] = os.path.relpath(rgba_out, ROOT).replace(
            os.sep, "/")
        row["mask_out"] = os.path.relpath(mask_out, ROOT).replace(
            os.sep, "/")
    except Exception as exc:  # noqa: BLE001
        row["error"] = f"{type(exc).__name__}: {exc}"
    row["elapsed"] = round(time.perf_counter() - t0, 2)
    return row


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", action="append", default=None,
                    help="scan root (repeatable); defaults to easy/decoded/"
                         "decoded_0b")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--out", default=os.path.join(ROOT, "extracted",
                                                  "gal_png"))
    ap.add_argument("--manifest", default=os.path.join(
        ROOT, "manifests", "batches", "gal-scan.csv"))
    ap.add_argument("--overwrite", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    roots = args.root or [r[1] for r in DEFAULT_ROOTS]
    labels = {os.path.join(ROOT, r): l for l, r in DEFAULT_ROOTS}
    items = []
    for root in roots:
        root = os.path.join(ROOT, root) if not os.path.isabs(root) else root
        if not os.path.isdir(root):
            print("missing root", root)
            continue
        label = labels.get(root, os.path.basename(root))
        for path in iter_gal(root):
            items.append((label, root, path))
    if args.limit:
        items = items[:args.limit]
    print(f"found {len(items)} .gal file(s)")
    for label, _root, path in items:
        print(f"  {label}  {os.path.relpath(path, ROOT)}")
    if args.dry_run:
        return 0

    os.makedirs(os.path.dirname(args.manifest), exist_ok=True)
    rows = [export_one(label, root, path, args.out, args.overwrite)
            for label, root, path in items]
    with open(args.manifest, "w", newline="", encoding="utf-8-sig") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    errors = sum(1 for r in rows if r["error"])
    print(f"done: {len(rows)} files, {errors} errors; manifest={args.manifest}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
