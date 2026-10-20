#!/usr/bin/env python
"""Scan an extracted APG corpus and report structure/mode coverage.

Usage (from the repository root):
    python tools/scan_apg.py --root extracted/decoded --jobs 8 \
        --out manifests/batches/apg-0a-scan.json

The scanner reads every ``*.apg`` under ``--root``, parses the AG header and
all AP/AI block headers, and aggregates frame counts, compression/alpha modes,
AG/index flags and parse errors.  Pixels are not decoded.
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

from dhsh.formats.apg import iter_frames  # noqa: E402


def scan_chunk(paths: list[str]) -> dict:
    files = 0
    files_ok = 0
    total_bytes = 0
    frames = 0
    pixels = 0
    max_frame_pixels = 0
    ag_flags: Counter = Counter()
    ag_reserved: Counter = Counter()
    flag_ai: Counter = Counter()
    idx_flags: Counter = Counter()
    idx_bad: Counter = Counter()
    magics: Counter = Counter()
    comps: Counter = Counter()
    alphas: Counter = Counter()
    combos: Counter = Counter()
    block_hdr: Counter = Counter()
    frame_counts: Counter = Counter()
    errors: list[tuple[str, str]] = []

    for path in paths:
        files += 1
        try:
            with open(path, "rb") as fh:
                data = fh.read()
        except OSError as exc:
            errors.append((path, f"read: {exc}"))
            continue
        total_bytes += len(data)
        try:
            ag, frs = iter_frames(data)
        except Exception as exc:  # noqa: BLE001 - report every failure
            errors.append((path, f"parse: {type(exc).__name__}: {exc}"))
            continue
        files_ok += 1
        ag_flags[ag.flags] += 1
        ag_reserved[ag.reserved] += 1
        frame_counts[ag.frame_count] += 1
        for _frame_id, flag, _off in ag.index:
            idx_flags[flag] += 1
        if len(frs) != ag.frame_count:
            errors.append((path, f"frame count {len(frs)} != {ag.frame_count}"))
        has_ai = any(fr.header.magic == b"AI" for fr in frs)
        flag_ai[(ag.flags, has_ai)] += 1
        for fr in frs:
            frames += 1
            h = fr.header
            px = h.width * h.height
            pixels += px
            if px > max_frame_pixels:
                max_frame_pixels = px
            magics[h.magic.decode("latin1")] += 1
            comps[h.compression] += 1
            alphas[h.alpha_mode] += 1
            combos[(h.magic.decode("latin1"), h.compression, h.alpha_mode)] += 1
            block_hdr[h.header_size] += 1
    return {
        "files": files,
        "files_ok": files_ok,
        "bytes": total_bytes,
        "frames": frames,
        "pixels": pixels,
        "max_frame_pixels": max_frame_pixels,
        "ag_flags": dict(ag_flags),
        "ag_reserved": dict(ag_reserved),
        "flag_ai": {f"flags{flag}/ai{int(has_ai)}": n for (flag, has_ai), n in flag_ai.items()},
        "frame_counts": dict(frame_counts),
        "index_flags": dict(idx_flags),
        "magics": dict(magics),
        "compressions": dict(comps),
        "alpha_modes": dict(alphas),
        "combos": {f"{m}/{c}/{a}": n for (m, c, a), n in combos.items()},
        "block_headers": dict(block_hdr),
        "errors": errors,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.join(ROOT, "extracted", "decoded"))
    ap.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 4))
    ap.add_argument("--chunk", type=int, default=200)
    ap.add_argument("--out", default=os.path.join(ROOT, "manifests", "batches", "apg-scan.json"))
    args = ap.parse_args()

    paths = []
    for base, _dirs, files in os.walk(args.root):
        for name in files:
            if name.lower().endswith(".apg"):
                paths.append(os.path.join(base, name))
    paths.sort()
    print(f"scanning {len(paths)} APG files under {args.root} with {args.jobs} workers", flush=True)

    chunks = [paths[i : i + args.chunk] for i in range(0, len(paths), args.chunk)]
    started = time.perf_counter()
    totals: Counter = Counter()
    nested: dict[str, Counter] = {
        "ag_flags": Counter(), "ag_reserved": Counter(), "flag_ai": Counter(),
        "frame_counts": Counter(), "index_flags": Counter(), "magics": Counter(),
        "compressions": Counter(), "alpha_modes": Counter(), "combos": Counter(),
        "block_headers": Counter(),
    }
    errors: list[tuple[str, str]] = []
    done = 0
    with Pool(processes=args.jobs) as pool:
        for res in pool.imap_unordered(scan_chunk, chunks):
            done += res["files"]
            for key in ("files", "files_ok", "bytes", "frames", "pixels"):
                totals[key] += res[key]
            if res["max_frame_pixels"] > totals["max_frame_pixels"]:
                totals["max_frame_pixels"] = res["max_frame_pixels"]
            for key in nested:
                nested[key].update(res[key])
            errors.extend(res["errors"])
            print(f"  {done}/{len(paths)} files, {totals['frames']} frames, "
                  f"{len(errors)} errors", flush=True)

    elapsed = time.perf_counter() - started
    report = {
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "root": args.root,
        "files": totals["files"],
        "files_ok": totals["files_ok"],
        "bytes": totals["bytes"],
        "frames": totals["frames"],
        "pixels": totals["pixels"],
        "max_frame_pixels": totals["max_frame_pixels"],
        "elapsed_seconds": round(elapsed, 1),
        "ag_flags": dict(nested["ag_flags"]),
        "ag_reserved": dict(nested["ag_reserved"]),
        "flag_ai": dict(nested["flag_ai"]),
        "frame_counts": dict(sorted(nested["frame_counts"].items())),
        "index_flags": dict(nested["index_flags"]),
        "magics": dict(nested["magics"]),
        "compressions": dict(nested["compressions"]),
        "alpha_modes": dict(nested["alpha_modes"]),
        "combos": dict(nested["combos"]),
        "block_headers": dict(nested["block_headers"]),
        "errors": errors[:200],
        "error_count": len(errors),
    }
    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=1)
    print(f"{totals['files_ok']}/{totals['files']} files parsed, "
          f"{totals['frames']} frames, {len(errors)} errors in {elapsed:.1f}s")
    print("magics:", report["magics"])
    print("compressions:", report["compressions"])
    print("alpha_modes:", report["alpha_modes"])
    print("combos:", report["combos"])
    print("wrote", out)
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
