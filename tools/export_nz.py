#!/usr/bin/env python
"""Parallel NZ leaf exporter driven by pak_inventory.json.

Usage (from the repository root):
    python tools/export_nz.py --types 0 --exts .apg --strip-nz \
        --out extracted/decoded_t0 --manifest manifests/batches/nzt0-full.csv --jobs 8

The inventory already stores each leaf's absolute PAK offset and NZ header, so
this tool avoids rescanning every PAK entry.  Workers are grouped by PAK and
chunked; type 0 is a direct payload copy, 0x0A/0x0B go through nz.decompress.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import sys
import time
from collections import Counter
from multiprocessing import Pool

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from dhsh.nz import HEADER_SIZE, decompress, parse_header  # noqa: E402
from dhsh.pak import safe_relative_path  # noqa: E402

DEFAULT_SOURCE = os.environ.get("DHSH_SOURCE", r"D:\software\dhsh\shdata")
DEFAULT_INVENTORY = os.path.join(ROOT, "pak_inventory.json")


def load_leaves(inventory: str, types: set[int], exts: set[str] | None,
                paks: tuple[str, ...] | None) -> list[dict]:
    inv = json.load(open(inventory, encoding="utf-8"))
    out: list[dict] = []
    for pak, entries in inv.items():
        stem = os.path.splitext(pak)[0]
        if paks and not stem.startswith(paks):
            continue
        for e in entries:
            head = e.get("head") or ""
            if len(head) < HEADER_SIZE * 2:
                continue
            raw = bytes.fromhex(head[: HEADER_SIZE * 2])
            if raw[:2] != b"NZ":
                continue
            kind, size1, size2 = (raw[2] | raw[3] << 8,
                                  int.from_bytes(raw[4:8], "little"),
                                  int.from_bytes(raw[8:12], "little"))
            if kind not in types:
                continue
            ext = os.path.splitext(e["name"])[1].lower() or "<none>"
            if exts is not None and ext not in exts:
                continue
            resolved = e["size"] if e["size"] >= HEADER_SIZE else HEADER_SIZE + size2
            out.append({
                "pak": pak, "name": e["name"], "path": e["path"],
                "offset": e["offset"], "resolved": resolved,
                "kind": kind, "size1": size1, "size2": size2, "ext": ext,
            })
    return out


def _safe(rel: str) -> str:
    return safe_relative_path(rel)


def export_chunk(task: tuple[str, list[dict], str, bool, bool, bool]) -> dict:
    pak, leaves, out_root, strip_nz, decomp, do_hash = task
    source = os.path.join(DEFAULT_SOURCE, pak)
    stem = os.path.splitext(pak)[0]
    out_dir = os.path.join(out_root, stem)
    rows: list[dict] = []
    counters: Counter = Counter()
    failures: list[tuple[str, str]] = []
    fh = open(source, "rb")
    try:
        for leaf in leaves:
            counters["selected"] += 1
            try:
                fh.seek(leaf["offset"])
                data = fh.read(leaf["resolved"])
                nz = parse_header(data)
                if nz is None:
                    raise ValueError("not NZ")
                payload = data[HEADER_SIZE:HEADER_SIZE + nz.size2]
                if decomp:
                    stored = decompress(nz.kind, payload, nz.size1)
                elif strip_nz:
                    stored = payload
                else:
                    stored = data
                rel = _safe(leaf["path"])
                dest = os.path.join(out_dir, rel)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, "wb") as out:
                    out.write(stored)
                counters["written"] += 1
                counters["bytes"] += len(stored)
                rows.append({
                    "pak": pak, "path": leaf["path"], "ext": leaf["ext"],
                    "nz_type": f"0x{leaf['kind']:04x}",
                    "size1": leaf["size1"], "size2": leaf["size2"],
                    "resolved": leaf["resolved"], "stored_size": len(stored),
                    "out_path": dest,
                    "sha256": hashlib.sha256(stored).hexdigest() if do_hash else "",
                    "decoded": 1 if decomp else 0,
                })
            except Exception as exc:  # noqa: BLE001
                failures.append((leaf["path"], f"{type(exc).__name__}: {exc}"))
    finally:
        fh.close()
    return {"rows": rows, "counters": dict(counters), "failures": failures}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", default=DEFAULT_SOURCE)
    ap.add_argument("--inventory", default=DEFAULT_INVENTORY)
    ap.add_argument("--out", required=True)
    ap.add_argument("--types", nargs="*", default=["0"], help="NZ types, e.g. 0 0x0a 0x0b")
    ap.add_argument("--exts", nargs="*", help="extensions, e.g. .apg .a16")
    ap.add_argument("--pak", nargs="*", help="top-level PAK stem prefixes")
    ap.add_argument("--limit", type=int, help="maximum selected leaves")
    ap.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 4))
    ap.add_argument("--chunk", type=int, default=500)
    ap.add_argument("--strip-nz", action="store_true")
    ap.add_argument("--decompress", action="store_true")
    ap.add_argument("--hash", action="store_true")
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--overwrite", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()

    types = {int(v, 0) for v in args.types}
    exts = None
    if args.exts:
        exts = {e.lower() if e.startswith(".") else "." + e.lower() for e in args.exts}
    paks = tuple(args.pak) if args.pak else None
    leaves = load_leaves(args.inventory, types, exts, paks)
    if args.limit:
        leaves = leaves[: args.limit]
    if not leaves:
        print("no matching leaves", file=sys.stderr)
        return 2
    if not (args.pak or args.exts or args.dry_run or args.all):
        print("refusing unfiltered export; add --pak/--exts filters or --all", file=sys.stderr)
        return 2
    total_bytes = sum(l["resolved"] for l in leaves)
    print(f"selected {len(leaves)} leaves, {total_bytes/1048576:.1f} MiB stored", flush=True)
    if args.dry_run:
        print("dry-run only")
        return 0

    # group by PAK, then chunk
    grouped: dict[str, list[dict]] = {}
    for leaf in leaves:
        grouped.setdefault(leaf["pak"], []).append(leaf)
    tasks = []
    for pak, items in sorted(grouped.items()):
        # honour existing files without overwrite: skip them in the parent
        if not args.overwrite:
            kept = []
            stem = os.path.splitext(pak)[0]
            for leaf in items:
                dest = os.path.join(args.out, stem, _safe(leaf["path"]))
                if not os.path.exists(dest):
                    kept.append(leaf)
            items = kept
        for i in range(0, len(items), args.chunk):
            tasks.append((pak, items[i:i + args.chunk], args.out,
                          args.strip_nz, args.decompress, args.hash))
    if not tasks:
        print("nothing to write (all destinations exist)")
        return 0

    started = time.perf_counter()
    totals: Counter = Counter()
    rows: list[dict] = []
    failures: list[tuple[str, str]] = []
    done = 0
    with Pool(processes=args.jobs) as pool:
        for res in pool.imap_unordered(export_chunk, tasks):
            totals.update(res["counters"])
            rows.extend(res["rows"])
            failures.extend(res["failures"])
            done += res["counters"].get("selected", 0)
            print(f"  {done}/{len(leaves)} leaves, {totals['written']} written, "
                  f"{totals['bytes']/1048576:.1f} MiB, {len(failures)} failed", flush=True)

    elapsed = time.perf_counter() - started
    rows.sort(key=lambda r: (r["pak"], r["path"]))
    os.makedirs(os.path.dirname(os.path.abspath(args.manifest)), exist_ok=True)
    with open(args.manifest, "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()) if rows else
                           ["pak", "path", "ext", "nz_type", "size1", "size2",
                            "resolved", "stored_size", "out_path", "sha256", "decoded"])
        w.writeheader()
        w.writerows(rows)
    print(f"selected {len(leaves)}, wrote {totals['written']}, failed {len(failures)}, "
          f"{totals['bytes']/1048576:.1f} MiB in {elapsed:.1f}s")
    print("wrote", args.manifest)
    for f in failures[:20]:
        print("FAIL", f)
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
