#!/usr/bin/env python
"""Validate pak_inventory.json against the source PAK files.

Checks every inventory leaf:
- absolute offset in range and the 16-byte head matches the PAK bytes;
- NZ header size consistency (entry size vs 12 + size2);
- path/name agreement and duplicate paths within a PAK.

Usage (from the repository root):
    python tools/validate_inventory.py --jobs 8 --out manifests/batches/inventory-validation.json
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time
from collections import Counter
from multiprocessing import Pool

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from dhsh.pak import safe_relative_path  # noqa: E402

DEFAULT_SOURCE = os.environ.get("DHSH_SOURCE", r"D:\software\dhsh\shdata")
DEFAULT_INVENTORY = os.path.join(ROOT, "pak_inventory.json")


def validate_pak(task: tuple[str, list[dict]]) -> dict:
    pak, entries = task
    path = os.path.join(DEFAULT_SOURCE, pak)
    size = os.path.getsize(path)
    counters: Counter = Counter()
    failures: list[tuple[str, str]] = []
    paths: Counter = Counter()
    with open(path, "rb") as fh:
        for e in entries:
            counters["entries"] += 1
            name = e.get("name", "")
            rel = e.get("path", "")
            if name == "UNREADABLE" or not rel:
                counters["unreadable"] += 1
                failures.append((rel or name, "unreadable/empty path"))
                continue
            paths[rel] += 1
            try:
                norm = safe_relative_path(rel)
            except Exception as exc:  # noqa: BLE001
                failures.append((rel, f"unsafe path: {exc}"))
                continue
            if not (norm.split("/")[-1] == name or norm.split("\\")[-1] == name):
                counters["name_mismatch"] += 1
                failures.append((rel, f"name mismatch: {name}"))
            offset = int(e.get("offset", -1))
            declared = int(e.get("size", -1))
            head = bytes.fromhex(e.get("head") or "")
            if offset < 0 or offset + max(declared, 0) > size:
                counters["range_bad"] += 1
                failures.append((rel, f"range offset={offset} size={declared} file={size}"))
                continue
            fh.seek(offset)
            actual = fh.read(16)
            if len(head) >= len(actual) and actual != head[:len(actual)]:
                counters["head_mismatch"] += 1
                failures.append((rel, f"head {actual.hex()} != {head[:len(actual)].hex()}"))
                continue
            if len(actual) >= 12 and actual[:2] == b"NZ":
                kind, s1, s2 = struct.unpack("<HII", actual[2:12])
                counters[f"type_0x{kind:04x}"] += 1
                expected = 12 + s2
                resolved = declared if declared >= 12 else expected
                if resolved != expected:
                    counters["size_mismatch"] += 1
                    failures.append((rel, f"size {resolved} != 12+size2 {expected}"))
                if offset + resolved > size:
                    counters["range_bad"] += 1
                    failures.append((rel, f"payload range offset={offset} resolved={resolved} file={size}"))
            else:
                counters["not_nz"] += 1
    dup = {p: n for p, n in paths.items() if n > 1}
    if dup:
        counters["duplicate_paths"] += len(dup)
        for p, n in list(dup.items())[:50]:
            failures.append((p, f"duplicate path x{n}"))
    return {"pak": pak, "counters": dict(counters), "failures": failures[:500],
            "failure_count": len(failures)}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", default=DEFAULT_SOURCE)
    ap.add_argument("--inventory", default=DEFAULT_INVENTORY)
    ap.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 4))
    ap.add_argument("--out", default=os.path.join(ROOT, "manifests", "batches", "inventory-validation.json"))
    args = ap.parse_args()

    inv = json.load(open(args.inventory, encoding="utf-8"))
    tasks = [(pak, ents) for pak, ents in sorted(inv.items())]
    print(f"validating {sum(len(v) for v in inv.values())} entries across {len(tasks)} PAKs",
          flush=True)
    started = time.perf_counter()
    totals: Counter = Counter()
    failures: list[tuple[str, str]] = []
    paks: list[dict] = []
    with Pool(processes=args.jobs) as pool:
        for res in pool.imap_unordered(validate_pak, tasks):
            totals.update(res["counters"])
            paks.append({"pak": res["pak"], "counters": res["counters"],
                         "failure_count": res["failure_count"]})
            for f in res["failures"]:
                failures.append((res["pak"], f[0], f[1]))
            print(f"  {res['pak']}: {res['counters'].get('entries',0)} entries, "
                  f"{res['failure_count']} failures", flush=True)
    elapsed = time.perf_counter() - started
    report = {
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "inventory": args.inventory,
        "source": args.source,
        "elapsed_seconds": round(elapsed, 1),
        "totals": dict(totals),
        "paks": paks,
        "failure_count": len(failures),
        "failures": failures[:500],
    }
    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=1)
    print(f"entries {totals['entries']}, failures {len(failures)} in {elapsed:.1f}s")
    print("totals:", dict(totals))
    print("wrote", out)
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
