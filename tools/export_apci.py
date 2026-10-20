#!/usr/bin/env python
"""Export an APCI container (perinfo.pak) to CSV/JSON manifests.

Usage (from the repository root):
    python tools/export_apci.py --limit-items 1000 \
        --out extracted/_batches/apci-b0 --manifest manifests/batches/apci-b0.json
    python tools/export_apci.py --all \
        --out extracted/apci/perinfo --manifest manifests/batches/apci-perinfo.json

Outputs:
    items.csv     one row per item: group/record/entry/mid/item, x, y,
                  width, bytes-per-pixel, flags, blob offset/size
    blobs.bin     concatenated item blobs (offsets refer to this file)
    records.csv   per-record aggregates (full runs)
    manifest JSON totals, group/record names and container statistics

The input is opened read-only; nothing is written outside the workspace.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from dhsh.formats.apci import (  # noqa: E402
    APCIFormatError, blob_size, iter_items, parse_header, record_summary,
)

DEFAULT_INPUT = r"D:\software\dhsh\shdata\perinfo.pak"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--input", default=DEFAULT_INPUT)
    ap.add_argument("--out", default=os.path.join(ROOT, "extracted", "apci", "perinfo"))
    ap.add_argument("--manifest", default=os.path.join(
        ROOT, "manifests", "batches", "apci-perinfo.json"))
    ap.add_argument("--limit-items", type=int, default=0,
                    help="stop after N items (smoke batch); 0 = all")
    ap.add_argument("--all", action="store_true", help="explicit full export")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if not args.all and not args.limit_items and not args.dry_run:
        ap.error("refusing full export without --all or --limit-items")

    data = open(args.input, "rb").read()
    header = parse_header(data)
    sha = hashlib.sha256(data).hexdigest()
    print(f"input {args.input}: {len(data)} bytes, version 0x{header.version:x}, "
          f"sha256 {sha[:16]}…")

    if args.dry_run:
        # Header-level preview only: first group id/count.
        import struct
        gid, count = struct.unpack_from("<HH", data, 8)
        print(f"dry run: first group id={gid} records={count}; no output written")
        return 0

    os.makedirs(args.out, exist_ok=True)
    items_path = os.path.join(args.out, "items.csv")
    blobs_path = os.path.join(args.out, "blobs.bin")
    started = time.time()
    items = blobs = blob_bytes = 0
    groups: dict[int, dict] = {}
    error = None

    with open(items_path, "w", newline="", encoding="utf-8") as items_fh, \
            open(blobs_path, "wb") as blobs_fh:
        writer = csv.writer(items_fh)
        writer.writerow(["group", "group_id", "record", "record_id", "name",
                         "entry", "entry_a", "mid", "mid_header", "item",
                         "x", "y", "width", "bpp", "flags",
                         "blob_offset", "blob_size"])
        try:
            for (g, gid, r, rid, name, s, a, b, m, hdr, i, x, y,
                 off, size) in iter_items(data):
                if args.limit_items and items >= args.limit_items:
                    break
                wrote = size
                blob_off = blob_bytes
                if size:
                    blobs_fh.write(data[off:off + size])
                    blob_bytes += size
                    blobs += 1
                writer.writerow([g, gid, r, rid, name, s, a, m, hdr, i,
                                 x, y, x & 0xFFFF, (x >> 24) & 0xFF,
                                 (y >> 16) & 0xFF, blob_off, wrote])
                items += 1
                ginfo = groups.setdefault(g, {"group_id": gid, "records": {}})
                rinfo = ginfo["records"].setdefault(r, {
                    "record_id": rid, "name": name, "items": 0, "blob_bytes": 0})
                rinfo["items"] += 1
                rinfo["blob_bytes"] += size
        except APCIFormatError as exc:
            error = str(exc)

    partial = bool(args.limit_items and items >= args.limit_items)
    summary = None
    if not partial and error is None:
        summary = record_summary(data)
        rec_path = os.path.join(args.out, "records.csv")
        with open(rec_path, "w", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            w.writerow(["group", "group_id", "record", "record_id", "name",
                        "sub_count", "entries", "mids", "items", "blob_count",
                        "blob_bytes"])
            for row in summary:
                w.writerow([row["group"], row["group_id"], row["record"],
                            row["record_id"], row["name"], row["sub_count"],
                            row["entries"], row["mids"], row["items"],
                            row["blob_count"], row["blob_bytes"]])
        print("records:", rec_path)

    elapsed = time.time() - started
    manifest = {
        "input": args.input,
        "sha256": sha,
        "size": len(data),
        "version": header.version,
        "partial": partial,
        "items": items,
        "blobs": blobs,
        "blob_bytes": blob_bytes,
        "groups": [
            {"group": g, "group_id": info["group_id"],
             "records": [
                 {"record": r, **rec} for r, rec in sorted(info["records"].items())
             ]}
            for g, info in sorted(groups.items())
        ],
        "records_summary": summary,
        "error": error,
        "outputs": {"items": items_path, "blobs": blobs_path},
        "elapsed_s": round(elapsed, 2),
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.manifest)), exist_ok=True)
    with open(args.manifest, "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, ensure_ascii=False, indent=1)

    print(f"APCI: {items} items, {blobs} blobs, {blob_bytes} blob bytes, "
          f"{len(groups)} groups, partial={partial}, errors={1 if error else 0}, "
          f"{elapsed:.1f}s")
    if error:
        print("  error:", error)
    print("manifest:", args.manifest)
    return 0 if error is None else 1


if __name__ == "__main__":
    sys.exit(main())
