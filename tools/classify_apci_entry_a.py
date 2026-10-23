#!/usr/bin/env python
"""Classify APCI `entry.a` values by record name.

`entry.a` is an animation/action id for some record families, but a large
share of entries fall outside the four ranges accepted by `FUN_007ed6d0`.
This script groups the exported items.csv by record name and reports how
many unique entries fall inside/outside those ranges, plus the dominant
value bands.  It is analysis-only (no game execution).

Usage:
    python tools/classify_apci_entry_a.py \
        --out manifests/batches/apci-entry-a-families.csv
"""
from __future__ import annotations

import argparse
import csv
import os
import sys
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ITEMS = os.path.join(ROOT, "extracted", "apci", "perinfo",
                             "items.csv")
RANGES = ((100, 9999), (40000, 49999), (61500, 64999),
          (101500, 103511))


def in_ranges(value: int) -> bool:
    return any(lo <= value <= hi for lo, hi in RANGES)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--items", default=DEFAULT_ITEMS)
    ap.add_argument("--out", default=os.path.join(
        ROOT, "manifests", "batches", "apci-entry-a-families.csv"))
    args = ap.parse_args()
    if not os.path.exists(args.items):
        print("missing items.csv:", args.items)
        return 2

    # record -> entry index -> entry.a (unique entry, not per mid/item)
    entries = defaultdict(dict)
    item_counts = Counter()
    with open(args.items, encoding="utf-8-sig", newline="") as fh:
        for row in csv.DictReader(fh):
            name = row["name"]
            idx = int(row["entry"])
            value = int(row["entry_a"])
            entries[name].setdefault(idx, value)
            item_counts[name] += 1

    rows = []
    for name, by_entry in entries.items():
        values = list(by_entry.values())
        inside = sum(1 for v in values if in_ranges(v))
        outside = len(values) - inside
        bands = Counter()
        for v in values:
            # coarse value bands for the report
            if v < 100:
                band = "<100"
            elif v <= 9999:
                band = "100-9999"
            elif v <= 19999:
                band = "10000-19999"
            elif v <= 29999:
                band = "20000-29999"
            elif v <= 39999:
                band = "30000-39999"
            elif v <= 49999:
                band = "40000-49999"
            elif v <= 59999:
                band = "50000-59999"
            elif v <= 64999:
                band = "60000-64999"
            elif v <= 99999:
                band = "65000-99999"
            elif v <= 103511:
                band = "101500-103511"
            else:
                band = ">=103512"
            bands[band] += 1
        rows.append({
            "name": name,
            "entries": len(values),
            "items": item_counts[name],
            "in_range": inside,
            "out_range": outside,
            "out_pct": round(outside / len(values) * 100, 2) if values else 0,
            "min": min(values) if values else "",
            "max": max(values) if values else "",
            "top_bands": ";".join(f"{k}:{v}" for k, v in
                                  bands.most_common(5)),
        })
    rows.sort(key=lambda r: (-r["entries"], r["name"]))
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", newline="", encoding="utf-8-sig") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    total_entries = sum(r["entries"] for r in rows)
    total_out = sum(r["out_range"] for r in rows)
    print(f"records {len(rows)}, unique entries {total_entries}, "
          f"outside ranges {total_out} "
          f"({total_out / total_entries * 100:.2f}%)")
    print(f"manifest: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
