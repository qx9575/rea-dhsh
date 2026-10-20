#!/usr/bin/env python
"""Batch-export APG frames to PNG with parallel workers.

Usage (from the repository root):
    # sample: first 10 player APGs
    python tools/export_apg.py --pak player --limit 10 --out extracted/_batches/apg-b0

    # full 0x0A APG corpus (explicit --all required)
    python tools/export_apg.py --all --jobs 8 --png-level 3 \
        --out extracted/apg_frames --manifest manifests/batches/apg-frames.csv

Each APG is written to ``<out>/<pak>/<logical dir>/<apg stem>/fNNN.png``.  AI
frames resolve ``clrtb.cti`` along the logical directory (same PAK first, then
cross-PAK), matching probe_apg/check_apg_cti.  NumPy fast paths are used when
available; otherwise the reference Python decoder is used.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import sys
import time
from collections import Counter
from multiprocessing import Pool

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from dhsh.formats.apg import iter_frames, frame_to_rgba  # noqa: E402
from dhsh.formats.a16 import parse_cti  # noqa: E402
from check_apg_cti import (  # noqa: E402
    resolve_cti, cti_index, load_or_build_cti_cache,
)

try:
    from dhsh.formats.apg_fast import frame_to_rgba_fast, cti_rgba_lut
    FAST = True
except ImportError:  # pragma: no cover - environment dependent
    FAST = False

DECODED_ROOT = os.path.join(ROOT, "extracted", "decoded")


def _rel_parts(path: str, root: str) -> tuple[str, str]:
    rel = os.path.relpath(path, root)
    parts = rel.split(os.sep)
    pak = parts[0]
    logical = os.path.dirname(os.path.join(*parts[1:])) if len(parts) > 1 else ""
    logical = "" if logical == "." else logical
    return pak, logical


def _init_worker(index) -> None:
    """Share the parent-built CTI index with every worker."""
    import check_apg_cti as _c
    _c._INDEX = index


def export_chunk(task: tuple[list[str], str, int, bool, int, str]) -> dict:
    paths, out_root, png_level, overwrite, table, root = task
    rows: list[dict] = []
    cti_cache: dict[str, tuple[object, object]] = {}
    counters: Counter = Counter()
    errors: list[tuple[str, str]] = []

    for path in paths:
        counters["files"] += 1
        t0 = time.perf_counter()
        try:
            with open(path, "rb") as fh:
                data = fh.read()
            ag, frames = iter_frames(data)
        except Exception as exc:  # noqa: BLE001
            errors.append((path, f"{type(exc).__name__}: {exc}"))
            continue

        pak, logical = _rel_parts(path, root)
        ai_frames = sum(1 for fr in frames if fr.header.magic == b"AI")
        cti_path = None
        cti = None
        lut = None
        if ai_frames:
            cti_path, _src = resolve_cti(pak, logical)
            if cti_path:
                if cti_path not in cti_cache:
                    try:
                        cti = parse_cti(open(cti_path, "rb").read())
                        cti_cache[cti_path] = (
                            cti,
                            cti_rgba_lut(cti, table) if FAST else None,
                        )
                    except Exception as exc:  # noqa: BLE001
                        errors.append((path, f"CTI {cti_path}: {type(exc).__name__}: {exc}"))
                        cti_cache[cti_path] = (None, None)
                cti, lut = cti_cache[cti_path]
            else:
                errors.append((path, "AI frame without a resolvable clrtb.cti"))

        rel = os.path.relpath(path, root)
        stem = os.path.splitext(rel)[0]
        out_dir = os.path.join(out_root, stem)
        os.makedirs(out_dir, exist_ok=True)
        written = 0
        pixels = 0
        nbytes = 0
        for fr in frames:
            png = os.path.join(out_dir, f"f{fr.index:03d}.png")
            if not overwrite and os.path.exists(png):
                written += 1
                pixels += fr.header.width * fr.header.height
                nbytes += os.path.getsize(png)
                continue
            try:
                if FAST:
                    img = frame_to_rgba_fast(fr, cti=cti, table=table, lut=lut)
                else:
                    img = frame_to_rgba(fr, palette=cti, palette_table=table)
                img.save(png, "PNG", compress_level=png_level, optimize=False)
                written += 1
                pixels += img.width * img.height
                nbytes += os.path.getsize(png)
            except Exception as exc:  # noqa: BLE001
                errors.append((path, f"frame {fr.index}: {type(exc).__name__}: {exc}"))
        counters["frames"] += len(frames)
        counters["written"] += written
        counters["pixels"] += pixels
        counters["bytes"] += nbytes
        counters["ai_frames"] += ai_frames
        if written != len(frames):
            counters["partial"] += 1
        rows.append({
            "pak": pak,
            "path": rel.replace(os.sep, "/"),
            "frames": len(frames),
            "ai_frames": ai_frames,
            "cti": cti_path or "",
            "out_dir": out_dir,
            "written": written,
            "pixels": pixels,
            "bytes": nbytes,
            "elapsed": round(time.perf_counter() - t0, 3),
            "error": "",
        })
    return {"rows": rows, "counters": dict(counters), "errors": errors}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=DECODED_ROOT, help="decoded root containing *.apg")
    ap.add_argument("--out", default=os.path.join(ROOT, "extracted", "apg_frames"))
    ap.add_argument("--pak", nargs="*", help="top-level PAK prefixes, e.g. player npc magic ui")
    ap.add_argument("--match", help="substring filter on the relative path")
    ap.add_argument("--cti-roots", nargs="*", help="CTI roots in preference order")
    ap.add_argument("--cti-index-cache", default=os.path.join(ROOT, "manifests", "batches", "cti-index.json"))
    ap.add_argument("--limit", type=int, help="maximum APG files")
    ap.add_argument("--per-pak-limit", type=int, help="maximum APG files per top-level PAK")
    ap.add_argument("--sample", type=int, help="randomly sample this many files after filters")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 4))
    ap.add_argument("--chunk", type=int, default=50)
    ap.add_argument("--png-level", type=int, default=3)
    ap.add_argument("--palette-table", type=int, choices=(1, 2), default=1)
    ap.add_argument("--manifest", default=os.path.join(ROOT, "manifests", "batches", "apg-frames.csv"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--overwrite", action="store_true")
    ap.add_argument("--all", action="store_true", help="explicitly allow an unfiltered full export")
    args = ap.parse_args()

    paths = []
    for base, _dirs, files in os.walk(args.root):
        for name in files:
            if name.lower().endswith(".apg"):
                paths.append(os.path.join(base, name))
    paths.sort()
    if args.pak:
        prefixes = tuple(args.pak)
        paths = [p for p in paths if os.path.relpath(p, args.root).split(os.sep)[0].startswith(prefixes)]
    if args.match:
        paths = [p for p in paths if args.match.lower() in os.path.relpath(p, args.root).lower()]
    if args.per_pak_limit:
        per: Counter = Counter()
        kept = []
        for p in paths:
            pak = os.path.relpath(p, args.root).split(os.sep)[0]
            if per[pak] >= args.per_pak_limit:
                continue
            per[pak] += 1
            kept.append(p)
        paths = kept
    if args.limit:
        paths = paths[: args.limit]
    if args.sample and args.sample < len(paths):
        import random
        paths = random.Random(args.seed).sample(paths, args.sample)
        paths.sort()
    if not paths:
        print("no APG files selected", file=sys.stderr)
        return 2
    if not (args.pak or args.match or args.limit or args.per_pak_limit
            or args.sample or args.dry_run or args.all):
        print("refusing an unfiltered export; add --pak/--match/--limit or --all", file=sys.stderr)
        return 2

    print(f"selected {len(paths)} APG files, jobs={args.jobs}, fast={FAST}, "
          f"png_level={args.png_level}", flush=True)
    if args.dry_run:
        total_frames = total_pixels = 0
        for p in paths:
            try:
                _ag, frames = iter_frames(open(p, "rb").read())
                total_frames += len(frames)
                total_pixels += sum(fr.header.width * fr.header.height for fr in frames)
            except Exception as exc:  # noqa: BLE001
                print(f"  dry-run parse error {p}: {exc}", file=sys.stderr)
        print(f"dry-run: {total_frames} frames, {total_pixels/1e6:.1f} Mpx, "
              f"raw RGBA {total_pixels*4/1073741824:.2f} GiB")
        return 0

    chunks = [paths[i : i + args.chunk] for i in range(0, len(paths), args.chunk)]
    tasks = [(c, args.out, args.png_level, args.overwrite, args.palette_table, args.root)
             for c in chunks]
    print("building CTI index...", flush=True)
    index = (load_or_build_cti_cache(args.cti_index_cache, args.cti_roots)
             if args.cti_roots else cti_index())
    started = time.perf_counter()
    totals: Counter = Counter()
    rows: list[dict] = []
    errors: list[tuple[str, str]] = []
    done = 0
    with Pool(processes=args.jobs, initializer=_init_worker, initargs=(index,)) as pool:
        for res in pool.imap_unordered(export_chunk, tasks):
            done += len(res["rows"])
            totals.update(res["counters"])
            rows.extend(res["rows"])
            errors.extend(res["errors"])
            print(f"  {done}/{len(paths)} APGs, {totals['frames']} frames, "
                  f"{totals['bytes']/1048576:.1f} MiB, {len(errors)} errors", flush=True)

    elapsed = time.perf_counter() - started
    rows.sort(key=lambda r: r["path"])
    out = os.path.abspath(args.manifest)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()) if rows else
                           ["pak", "path", "frames", "ai_frames", "cti", "out_dir",
                            "written", "pixels", "bytes", "elapsed", "error"])
        w.writeheader()
        w.writerows(rows)
    print(f"APGs {totals['files']}, frames {totals['written']}/{totals['frames']} written, "
          f"{totals['bytes']/1048576:.1f} MiB, {totals['pixels']/1e6:.1f} Mpx, "
          f"{len(errors)} errors in {elapsed:.1f}s")
    print("wrote", out)
    for e in errors[:20]:
        print("ERROR", e)
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
