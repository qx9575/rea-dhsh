#!/usr/bin/env python
"""Check CTI palette coverage for AI frames in an extracted APG corpus.

AI blocks store 8-bit indices; the palette is a nearby ``clrtb.cti`` found by
walking up the logical directory.  This tool parses every APG under
``extracted/decoded`` and, for files containing AI frames, resolves a CTI from
``extracted/decoded`` (0x0A CTI) or ``extracted/images-proprietary`` (type 0
CTI), first within the same PAK then across any PAK (matching probe_apg).

Usage (from the repository root):
    python tools/check_apg_cti.py --jobs 8 --out manifests/batches/apg-cti.json
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
sys.path.insert(0, os.path.join(ROOT, "tools"))

from dhsh.formats.apg import iter_frames  # noqa: E402

DECODED_ROOT = os.path.join(ROOT, "extracted", "decoded")
DECODED_0B_ROOT = os.path.join(ROOT, "extracted", "decoded_0b")
PROP_ROOT = os.path.join(ROOT, "extracted", "images-proprietary")

_INDEX = None
_SCAN_ROOT = None


def build_cti_index(roots) -> tuple[dict, dict]:
    """Index CTI files by (pak, logical dir) and by logical dir, in root order."""
    by_pak: dict[tuple[str, str], list[str]] = defaultdict(list)
    by_dir: dict[str, list[str]] = defaultdict(list)
    for root in roots:
        for base, _dirs, files in os.walk(root):
            for name in files:
                if not name.lower().endswith(".cti"):
                    continue
                path = os.path.join(base, name)
                rel = os.path.relpath(path, root)
                parts = rel.split(os.sep)
                pak = parts[0]
                logical = os.path.dirname(os.path.join(*parts[1:])) if len(parts) > 1 else ""
                logical = "" if logical == "." else logical
                by_pak[(pak, logical)].append(path)
                by_dir[logical].append(path)
    return by_pak, by_dir


def cti_index():
    """Return the default (0x0A decoded + type-0) CTI index."""
    global _INDEX
    if _INDEX is None:
        _INDEX = build_cti_index((DECODED_ROOT, PROP_ROOT))
    return _INDEX


def _init_worker(root: str, index) -> None:
    global _SCAN_ROOT, _INDEX
    _SCAN_ROOT = root
    _INDEX = index


def resolve_cti(pak: str, logical: str) -> tuple[str | None, str]:
    by_pak, by_dir = cti_index()

    def lookup(key_dir: str) -> tuple[str | None, str]:
        d = key_dir
        while True:
            cands = by_pak.get((pak, d))
            if cands:
                return cands[0], "same-pak"
            if not d:
                break
            d = os.path.dirname(d)
            if d == ".":
                d = ""
        d = key_dir
        while True:
            cands = by_dir.get(d)
            if cands:
                return cands[0], "cross-pak"
            if not d:
                break
            d = os.path.dirname(d)
            if d == ".":
                d = ""
        return None, "missing"

    found, source = lookup(logical)
    if found:
        return found, source
    # Some left-cuff animations (e.g. Lcuff/521) ship without a palette; the
    # matching right-cuff/cloth palette is byte-identical on all checked IDs.
    parts = logical.split(os.sep)
    if parts and parts[0] == "Lcuff":
        alt = os.path.join("Rcuff", *parts[1:])
        found, _source = lookup(alt)
        if found:
            return found, "sibling-palette"
    return None, "missing"


def scan_chunk(paths: list[str]) -> dict:
    files = 0
    ai_files = 0
    ai_frames = 0
    ok_files = 0
    missing_files = 0
    ok_frames = 0
    missing_frames = 0
    sources: Counter = Counter()
    missing: Counter = Counter()
    errors: list[tuple[str, str]] = []

    for path in paths:
        files += 1
        try:
            with open(path, "rb") as fh:
                data = fh.read()
            _ag, frames = iter_frames(data)
        except Exception as exc:  # noqa: BLE001
            errors.append((path, f"{type(exc).__name__}: {exc}"))
            continue
        ai = [f for f in frames if f.header.magic == b"AI"]
        if not ai:
            continue
        ai_files += 1
        ai_frames += len(ai)
        rel = os.path.relpath(path, _SCAN_ROOT or DECODED_ROOT)
        parts = rel.split(os.sep)
        pak = parts[0]
        logical = os.path.dirname(os.path.join(*parts[1:])) if len(parts) > 1 else ""
        logical = "" if logical == "." else logical
        cti, source = resolve_cti(pak, logical)
        sources[source] += 1
        if cti:
            ok_files += 1
            ok_frames += len(ai)
        else:
            missing_files += 1
            missing_frames += len(ai)
            missing[(pak, logical)] += 1
    return {
        "files": files,
        "ai_files": ai_files,
        "ai_frames": ai_frames,
        "ok_files": ok_files,
        "missing_files": missing_files,
        "ok_frames": ok_frames,
        "missing_frames": missing_frames,
        "sources": dict(sources),
        "missing": {f"{p}:{d}" if d else p: n for (p, d), n in missing.items()},
        "errors": errors,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=DECODED_ROOT)
    ap.add_argument("--cti-roots", nargs="*", help="CTI roots in preference order")
    ap.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 4))
    ap.add_argument("--chunk", type=int, default=200)
    ap.add_argument("--out", default=os.path.join(ROOT, "manifests", "batches", "apg-cti.json"))
    args = ap.parse_args()

    paths = []
    for base, _dirs, files in os.walk(args.root):
        for name in files:
            if name.lower().endswith(".apg"):
                paths.append(os.path.join(base, name))
    paths.sort()
    print(f"checking CTI for {len(paths)} APG files with {args.jobs} workers", flush=True)

    chunks = [paths[i : i + args.chunk] for i in range(0, len(paths), args.chunk)]
    started = time.perf_counter()
    totals: Counter = Counter()
    sources: Counter = Counter()
    missing: Counter = Counter()
    errors: list[tuple[str, str]] = []
    done = 0
    index = build_cti_index(args.cti_roots or [args.root, DECODED_ROOT, PROP_ROOT])
    with Pool(processes=args.jobs, initializer=_init_worker,
              initargs=(args.root, index)) as pool:
        for res in pool.imap_unordered(scan_chunk, chunks):
            done += res["files"]
            for key in ("files", "ai_files", "ai_frames", "ok_files",
                        "missing_files", "ok_frames", "missing_frames"):
                totals[key] += res[key]
            sources.update(res["sources"])
            missing.update(res["missing"])
            errors.extend(res["errors"])
            print(f"  {done}/{len(paths)} files, AI {totals['ai_files']} files/"
                  f"{totals['ai_frames']} frames, missing {totals['missing_files']} files",
                  flush=True)

    elapsed = time.perf_counter() - started
    report = {
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "root": args.root,
        "files": totals["files"],
        "ai_files": totals["ai_files"],
        "ai_frames": totals["ai_frames"],
        "ok_files": totals["ok_files"],
        "missing_files": totals["missing_files"],
        "ok_frames": totals["ok_frames"],
        "missing_frames": totals["missing_frames"],
        "sources": dict(sources),
        "missing": dict(missing.most_common(200)),
        "error_count": len(errors),
        "errors": errors[:100],
        "elapsed_seconds": round(elapsed, 1),
    }
    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=1)
    print(f"AI files {totals['ai_files']}, frames {totals['ai_frames']}; "
          f"resolved {totals['ok_files']} files / {totals['ok_frames']} frames; "
          f"missing {totals['missing_files']} files / {totals['missing_frames']} frames")
    print("sources:", report["sources"])
    print("top missing:", list(report["missing"].items())[:20])
    print("wrote", out)
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
