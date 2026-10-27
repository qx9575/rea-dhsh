#!/usr/bin/env python3
"""Export kimi_viewer's sourced model-name coverage from the current PAK data."""
from __future__ import annotations

import argparse
import csv
import hashlib
import os
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from kimi_viewer.avatar import (  # noqa: E402
    PARTS,
    all_npc_models,
    all_part_models,
    load_model_names,
)
from kimi_viewer.source import PakSource, load_inventory  # noqa: E402

CATEGORIES = tuple(part for part, _label in PARTS) + ("npc",)
FIELDS = ("category", "model_id", "pak", "names", "sources", "status")


def coverage_rows(src) -> list[dict]:
    catalog = load_model_names(src)
    rows = []
    for category in CATEGORIES:
        available = (all_npc_models(src) if category == "npc"
                     else all_part_models(src, category))
        paks = {}
        for pak, model in available:
            paks.setdefault(model, set()).add(pak)
        assert set(paks) == catalog._available[category]
        for model in sorted(paks):
            info = catalog.query(category, model)
            status = "confirmed" if info.confirmed else "pending"
            assert bool(info.names) == info.confirmed
            rows.append({
                "category": category,
                "model_id": model,
                "pak": " | ".join(sorted(paks[model])),
                "names": " | ".join(info.names),
                "sources": " | ".join(info.sources),
                "status": status,
            })
    return rows


def write_coverage(rows, output: str) -> str:
    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    with open(output, "w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=FIELDS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    with open(output, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def main() -> int:
    data_dir = os.path.join(os.getcwd(), "kimi_viewer_data")
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--shdata", default=r"D:\software\dhsh\shdata",
                    help="PAK directory")
    ap.add_argument("--inventory", default=os.path.join(
        data_dir, "pak_inventory.json"), help="PAK inventory JSON")
    ap.add_argument("--cache-dir", default=data_dir,
                    help="compiled inventory cache directory")
    ap.add_argument("--output", default=os.path.join(
        os.getcwd(), "manifests", "kimi_viewer_model_coverage.csv"),
                    help="coverage CSV output")
    args = ap.parse_args()

    entries, stale = load_inventory(args.inventory, args.cache_dir, args.shdata)
    if stale:
        print(f"rebuilt stale PAK indexes: {', '.join(stale)}")
    src = PakSource(entries, args.shdata)
    try:
        rows = coverage_rows(src)
    finally:
        src.close()
    digest = write_coverage(rows, args.output)

    counts = Counter((row["category"], row["status"]) for row in rows)
    for category in CATEGORIES:
        confirmed = counts[category, "confirmed"]
        pending = counts[category, "pending"]
        print(f"{category}: confirmed={confirmed} pending={pending} "
              f"total={confirmed + pending}")
    print(f"total={len(rows)} rows={len(rows) + 1} sha256={digest}")
    print(args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
