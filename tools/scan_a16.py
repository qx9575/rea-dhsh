#!/usr/bin/env python
"""Scan A16 files across exported corpora and report mode coverage.

Usage (from the repository root):
    python tools/scan_a16.py --jobs 8 \
        --out manifests/batches/a16-scan.json

The scanner reads only the 24-byte A16 header and the file size for every
``*.a16`` under the given roots.  It aggregates magic, compression and alpha
modes, checks ``header_size + stream1_size + stream2_size == file size`` and
keeps a few sample paths for every unsupported mode.  Pixels are not decoded.
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

DEFAULT_ROOTS = {
    "t0": "extracted/images-proprietary",
    "0a": "extracted/decoded",
    "0b": "extracted/decoded_0b",
}

MAGICS = (b"AP", b"AL", b"AI")
# compression modes implemented in tools/dhsh/formats/a16.py
SUPPORTED_COMP = {0, 1, 2}
SUPPORTED_ALPHA = {0, 2}


def iter_a16(root: str):
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if name.lower().endswith(".a16"):
                yield os.path.join(dirpath, name)


def scan_one(path: str) -> dict | None:
    try:
        with open(path, "rb") as fh:
            head = fh.read(24)
            size = os.fstat(fh.fileno()).st_size
    except OSError as exc:
        return {"path": path, "error": f"read: {exc}"}
    if len(head) < 24:
        return {"path": path, "error": f"short header ({len(head)} bytes)"}
    magic = head[:2]
    header_size = struct.unpack_from("<H", head, 2)[0]
    width, height = struct.unpack_from("<HH", head, 8)
    flags = struct.unpack_from("<H", head, 12)[0]
    compression = head[14]
    alpha_mode = head[15]
    s1, s2 = struct.unpack_from("<II", head, 16)
    error = None
    if magic not in MAGICS:
        error = f"bad magic {magic!r}"
    elif header_size < 24:
        error = f"header_size {header_size} < 24"
    elif header_size + s1 + s2 != size:
        error = f"size {size} != header {header_size}+{s1}+{s2}"
    elif width == 0 or height == 0:
        error = f"empty image {width}x{height}"
    return {
        "path": path,
        "magic": magic.decode("latin1"),
        "header_size": header_size,
        "width": width,
        "height": height,
        "flags": flags,
        "compression": compression,
        "alpha_mode": alpha_mode,
        "s1": s1,
        "s2": s2,
        "size": size,
        "error": error,
    }


def scan_chunk(paths: list[str]) -> dict:
    files = 0
    total_bytes = 0
    magics: Counter = Counter()
    comps: Counter = Counter()
    alphas: Counter = Counter()
    combos: Counter = Counter()
    flags: Counter = Counter()
    header_sizes: Counter = Counter()
    sizes: Counter = Counter()
    pixels = 0
    max_pixels = 0
    errors: list[tuple[str, str]] = []
    unknown_comp: dict[int, list[str]] = {}
    unknown_alpha: dict[int, list[str]] = {}
    empty_s2: Counter = Counter()

    for path in paths:
        files += 1
        rec = scan_one(path)
        if rec is None:
            continue
        if rec.get("error"):
            errors.append((rec["path"], rec["error"]))
            continue
        total_bytes += rec["size"]
        magics[rec["magic"]] += 1
        comp = rec["compression"]
        alpha = rec["alpha_mode"]
        comps[comp] += 1
        alphas[alpha] += 1
        combos[(comp, alpha)] += 1
        flags[rec["flags"]] += 1
        header_sizes[rec["header_size"]] += 1
        sizes[(rec["width"], rec["height"])] += 1
        px = rec["width"] * rec["height"]
        pixels += px
        max_pixels = max(max_pixels, px)
        if rec["s2"] == 0:
            empty_s2[alpha] += 1
        if comp not in SUPPORTED_COMP:
            unknown_comp.setdefault(comp, []).append(path)
        if alpha not in SUPPORTED_ALPHA:
            unknown_alpha.setdefault(alpha, []).append(path)

    return {
        "files": files,
        "bytes": total_bytes,
        "pixels": pixels,
        "max_pixels": max_pixels,
        "magics": magics,
        "compressions": comps,
        "alpha_modes": alphas,
        "combos": combos,
        "flags": flags,
        "header_sizes": header_sizes,
        "sizes": sizes,
        "empty_s2": empty_s2,
        "errors": errors,
        "unknown_comp": {k: v[:10] for k, v in unknown_comp.items()},
        "unknown_alpha": {k: v[:10] for k, v in unknown_alpha.items()},
    }


def merge(parts: list[dict]) -> dict:
    out = {
        "files": 0,
        "bytes": 0,
        "pixels": 0,
        "max_pixels": 0,
        "magics": Counter(),
        "compressions": Counter(),
        "alpha_modes": Counter(),
        "combos": Counter(),
        "flags": Counter(),
        "header_sizes": Counter(),
        "sizes": Counter(),
        "empty_s2": Counter(),
        "errors": [],
        "unknown_comp": {},
        "unknown_alpha": {},
    }
    for part in parts:
        out["files"] += part["files"]
        out["bytes"] += part["bytes"]
        out["pixels"] += part["pixels"]
        out["max_pixels"] = max(out["max_pixels"], part["max_pixels"])
        for key in ("magics", "compressions", "alpha_modes", "combos", "flags",
                    "header_sizes", "sizes", "empty_s2"):
            out[key].update(part[key])
        out["errors"].extend(part["errors"])
        for key in ("unknown_comp", "unknown_alpha"):
            for mode, paths in part[key].items():
                bucket = out[key].setdefault(mode, [])
                if len(bucket) < 10:
                    bucket.extend(paths[: 10 - len(bucket)])
    return out


def to_json(agg: dict) -> dict:
    return {
        "files": agg["files"],
        "bytes": agg["bytes"],
        "pixels": agg["pixels"],
        "max_pixels": agg["max_pixels"],
        "magics": dict(agg["magics"]),
        "compressions": {str(k): v for k, v in sorted(agg["compressions"].items())},
        "alpha_modes": {str(k): v for k, v in sorted(agg["alpha_modes"].items())},
        "combos": {f"{c}/{a}": v for (c, a), v in sorted(agg["combos"].items())},
        "flags": dict(agg["flags"]),
        "header_sizes": dict(agg["header_sizes"]),
        "empty_s2": {str(k): v for k, v in sorted(agg["empty_s2"].items())},
        "sizes_top20": [
            {"width": w, "height": h, "count": n}
            for (w, h), n in agg["sizes"].most_common(20)
        ],
        "distinct_sizes": len(agg["sizes"]),
        "errors": [{"path": p, "error": e} for p, e in agg["errors"]],
        "unknown_comp": agg["unknown_comp"],
        "unknown_alpha": agg["unknown_alpha"],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", action="append", default=None,
                        help="root directory (repeatable); default scans the three corpora")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--out", default=os.path.join(
        ROOT, "manifests", "batches", "a16-scan.json"))
    args = parser.parse_args()

    roots = args.root or [os.path.join(ROOT, rel) for rel in DEFAULT_ROOTS.values()]
    started = time.time()
    all_paths: list[str] = []
    for root in roots:
        found = list(iter_a16(root))
        print(f"root {root}: {len(found)} A16")
        all_paths.extend(found)

    chunks = [all_paths[i:: max(1, args.jobs * 4)] for i in range(args.jobs * 4)]
    chunks = [c for c in chunks if c]
    with Pool(args.jobs) as pool:
        parts = pool.map(scan_chunk, chunks)
    agg = merge(parts)

    report = to_json(agg)
    report["roots"] = roots
    report["elapsed_s"] = round(time.time() - started, 2)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=2)

    print(f"files {report['files']}, bytes {report['bytes']:,}, "
          f"pixels {report['pixels']:,}, errors {len(report['errors'])}")
    print("magics", report["magics"])
    print("compressions", report["compressions"])
    print("alpha_modes", report["alpha_modes"])
    print("unknown comp", {k: len(v) for k, v in report["unknown_comp"].items()})
    print("unknown alpha", {k: len(v) for k, v in report["unknown_alpha"].items()})
    print("report:", args.out)


if __name__ == "__main__":
    sys.exit(main())
