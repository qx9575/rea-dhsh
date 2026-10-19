#!/usr/bin/env python
"""Validate every NZ type 0x0A leaf with a per-PAK process pool.

Usage (from the repository root):
    python tools/validate_0a.py --jobs 8 --out manifests/batches/nz0a-full.json

Each PAK is validated by one worker: decode the payload, require the output
length to equal the NZ header ``size1``, and run the same extension checks as
``probe_0a.py``.  Nothing is written to disk except the JSON report and the
log the caller redirects.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from collections import Counter, defaultdict
from multiprocessing import Pool

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
sys.path.insert(0, TOOLS)

import probe_0a  # noqa: E402
from dhsh.nz import parse_header, decompress  # noqa: E402


def validate_pak(task: tuple[str, list[dict]]) -> dict:
    pak, leaves = task
    handles = {pak: open(os.path.join(probe_0a.SOURCE, pak), "rb")}
    by_ext: Counter = Counter()
    by_ext_ok: Counter = Counter()
    failures: list[dict] = []
    out_bytes = 0
    decoded = 0
    try:
        for leaf in leaves:
            rec = dict(leaf)
            by_ext[leaf["ext"]] += 1
            try:
                head, payload = probe_0a.read_leaf(handles, leaf)
                nz = parse_header(head)
                if nz is None:
                    raise ValueError("not NZ")
                data = decompress(nz.kind, payload, nz.size1)
                if len(data) != nz.size1:
                    raise ValueError(f"length {len(data)} != {nz.size1}")
                ok, note = probe_0a.check_output(leaf["ext"], data)
                if not ok:
                    raise ValueError(note)
            except Exception as exc:  # noqa: BLE001 - report every failure
                rec["error"] = f"{type(exc).__name__}: {exc}"
                failures.append(rec)
                continue
            decoded += 1
            out_bytes += len(data)
            by_ext_ok[leaf["ext"]] += 1
    finally:
        for fh in handles.values():
            fh.close()
    return {
        "pak": pak,
        "total": len(leaves),
        "decoded": decoded,
        "failed": len(failures),
        "output_bytes": out_bytes,
        "by_ext": dict(by_ext),
        "by_ext_ok": dict(by_ext_ok),
        "failures": failures,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 4))
    ap.add_argument("--out", default=os.path.join(ROOT, "manifests", "batches", "nz0a-full.json"))
    args = ap.parse_args()

    leaves = probe_0a.load_leaves({0x0A})
    grouped: dict[str, list[dict]] = defaultdict(list)
    for leaf in leaves:
        grouped[leaf["pak"]].append(leaf)
    tasks = [(pak, grouped[pak]) for pak in sorted(grouped)]
    print(f"validating {len(leaves)} leaves across {len(tasks)} PAKs with {args.jobs} workers",
          flush=True)

    started = time.time()
    by_ext: Counter = Counter()
    by_ext_ok: Counter = Counter()
    failures: list[dict] = []
    decoded = 0
    out_bytes = 0
    done_leaves = 0
    done_paks = 0

    with Pool(processes=args.jobs) as pool:
        for result in pool.imap_unordered(validate_pak, tasks):
            done_paks += 1
            done_leaves += result["total"]
            decoded += result["decoded"]
            out_bytes += result["output_bytes"]
            by_ext.update(result["by_ext"])
            by_ext_ok.update(result["by_ext_ok"])
            failures.extend(result["failures"])
            print(f"[{done_paks}/{len(tasks)}] {result['pak']}: "
                  f"{result['decoded']}/{result['total']}, {result['failed']} failed; "
                  f"total {done_leaves}/{len(leaves)}", flush=True)

    elapsed = time.time() - started
    report = {
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "batch": "full-parallel",
        "algorithm": "NZ 0x0A: 16-bit LZW (Game.exe VA 0x8a6f60)",
        "types": [0x0A],
        "requested": len(leaves),
        "decoded": decoded,
        "failed": len(failures),
        "output_bytes": out_bytes,
        "elapsed_seconds": round(elapsed, 1),
        "jobs": args.jobs,
        "by_ext": {
            ext: {"total": by_ext[ext], "ok": by_ext_ok[ext]}
            for ext in sorted(by_ext)
        },
        "failures": failures[:200],
    }
    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=1)
    print(f"\n{decoded}/{len(leaves)} decoded, {len(failures)} failed, "
          f"{out_bytes / 1048576:.1f} MiB output in {elapsed:.1f}s", flush=True)
    print("by ext:", {k: f"{v['ok']}/{v['total']}" for k, v in report["by_ext"].items()})
    print("wrote", out)
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
