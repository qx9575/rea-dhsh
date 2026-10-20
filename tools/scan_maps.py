#!/usr/bin/env python
"""Scan map side files (MAP/CEL/MAN/MSK/core/MLK) and report coverage.

Usage (from the repository root):
    python tools/scan_maps.py --jobs 8 --out manifests/batches/map-scan.json
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from collections import Counter
from multiprocessing import Pool

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from dhsh.formats.map import (  # noqa: E402
    parse_cel, parse_man, parse_msk, parse_mlk, parse_map_ini,
)

DEFAULT_ROOTS = (
    "extracted/easy",
    "extracted/decoded",
    "extracted/decoded_0b",
)
EXTS = {".map", ".cel", ".man", ".msk", ".mlk"}


def iter_files(root: str):
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            ext = os.path.splitext(name)[1].lower()
            if ext in EXTS or (ext == ".core"):
                yield ext, os.path.join(dirpath, name)


def scan_chunk(task: tuple[str, list[tuple[str, str]]]) -> dict:
    label, items = task
    out = {
        "label": label,
        "counts": Counter(),
        "cel_flags": Counter(),
        "cel_sizes": Counter(),
        "man_entries": Counter(),
        "msk_records": Counter(),
        "core_records": Counter(),
        "mlk_records": Counter(),
        "map_dims": Counter(),
        "errors": [],
        "samples": {},
    }
    for ext, path in items:
        out["counts"][ext] += 1
        try:
            data = open(path, "rb").read()
            if ext == ".cel":
                cel = parse_cel(data)
                out["cel_flags"][cel.flags] += 1
                out["cel_sizes"][(cel.width, cel.height)] += 1
                out["samples"].setdefault(".cel", path)
            elif ext == ".man":
                man = parse_man(data)
                out["man_entries"][len(man.entries)] += 1
                out["samples"].setdefault(".man", path)
            elif ext == ".msk":
                msk = parse_msk(data)
                out["msk_records"][msk.record_size] += 1
                out["samples"].setdefault(".msk", path)
            elif ext == ".core":
                core = parse_msk(data)
                out["core_records"][core.record_size] += 1
                out["samples"].setdefault(".core", path)
            elif ext == ".mlk":
                mlk = parse_mlk(data)
                out["mlk_records"][len(mlk.records)] += 1
                out["samples"].setdefault(".mlk", path)
            elif ext == ".map":
                ini = parse_map_ini(data)
                base = ini.get("base", {})
                if "WIDTH" in base and "HEIGHT" in base:
                    out["map_dims"][(int(base["WIDTH"]), int(base["HEIGHT"]))] += 1
                out["samples"].setdefault(".map", path)
        except Exception as exc:  # noqa: BLE001
            out["errors"].append((path, f"{type(exc).__name__}: {exc}"))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", action="append", default=None,
                    help="scan root (repeatable); defaults to easy/decoded/decoded_0b")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--out", default=os.path.join(
        ROOT, "manifests", "batches", "map-scan.json"))
    args = ap.parse_args()

    roots = args.root or [os.path.join(ROOT, r) for r in DEFAULT_ROOTS]
    started = time.time()
    tasks = []
    for root in roots:
        if not os.path.isdir(root):
            print("missing root", root)
            continue
        items = list(iter_files(root))
        print(f"root {root}: {len(items)} map files")
        if items:
            tasks.append((os.path.relpath(root, ROOT), items))
    with Pool(args.jobs) as pool:
        parts = pool.map(scan_chunk, tasks)

    total_counts: Counter = Counter()
    total_errors: list[tuple[str, str]] = []
    sections = []
    for part in parts:
        total_counts.update(part["counts"])
        total_errors.extend(part["errors"])
        sections.append({
            "root": part["label"],
            "counts": dict(part["counts"]),
            "cel_flags": {str(k): v for k, v in part["cel_flags"].items()},
            "cel_sizes_top10": [
                {"width": w, "height": h, "count": n}
                for (w, h), n in part["cel_sizes"].most_common(10)
            ],
            "man_entries": {str(k): v for k, v in sorted(part["man_entries"].items())},
            "msk_record_sizes": {str(k): v for k, v in part["msk_records"].items()},
            "core_record_sizes": {str(k): v for k, v in part["core_records"].items()},
            "mlk_records": {str(k): v for k, v in sorted(part["mlk_records"].items())},
            "map_dims_top10": [
                {"width": w, "height": h, "count": n}
                for (w, h), n in part["map_dims"].most_common(10)
            ],
            "samples": part["samples"],
        })
    report = {
        "roots": roots,
        "counts": dict(total_counts),
        "errors": [{"path": p, "error": e} for p, e in total_errors],
        "sections": sections,
        "elapsed_s": round(time.time() - started, 2),
    }
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=2)
    print("total", report["counts"], "errors", len(report["errors"]))
    for path, err in total_errors[:20]:
        print("  error:", path, err)
    print("report:", args.out)
    return 0 if not total_errors else 1


if __name__ == "__main__":
    sys.exit(main())
