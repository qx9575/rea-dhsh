#!/usr/bin/env python
"""Scan SCP UI scripts: encodings, includes, blocks, keys and warnings.

Usage (from the repository root):
    python tools/scan_scp.py --jobs 8 --out manifests/batches/scp-scan.json
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

from dhsh.formats.scp import parse_scp, iter_nodes  # noqa: E402

DEFAULT_ROOTS = ("extracted/easy", "extracted/decoded", "extracted/decoded_0b")


def iter_scp(root: str):
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if name.lower().endswith(".scp"):
                yield os.path.join(dirpath, name)


def scan_chunk(task: tuple[str, list[str]]) -> dict:
    label, paths = task
    out = {
        "label": label,
        "files": 0,
        "lines": 0,
        "encodings": Counter(),
        "includes": 0,
        "nodes": 0,
        "kinds": Counter(),
        "types": Counter(),
        "keys": Counter(),
        "depth": Counter(),
        "warnings": [],
        "window_names": [],
    }
    for path in paths:
        out["files"] += 1
        try:
            data = open(path, "rb").read()
            scp = parse_scp(data)
        except Exception as exc:  # noqa: BLE001
            out["warnings"].append((path, f"parse: {type(exc).__name__}: {exc}"))
            continue
        out["encodings"][scp.encoding] += 1
        out["includes"] += len(scp.includes)
        out["lines"] += data.count(b"\n") + 1
        for key, _value in scp.props:
            out["keys"][key] += 1
        max_depth = 0

        def walk(nodes, depth):
            nonlocal max_depth
            max_depth = max(max_depth, depth)
            for node in nodes:
                out["nodes"] += 1
                out["kinds"][node.kind] += 1
                out["types"][node.type or "?"] += 1
                for key, _value in node.props:
                    out["keys"][key] += 1
                    if key == "NAME" and len(out["window_names"]) < 50:
                        out["window_names"].append(_value.strip('"'))
                walk(node.children, depth + 1)

        walk(scp.windows, 1)
        out["depth"][max_depth] += 1
        for warn in scp.warnings:
            if len(out["warnings"]) < 200:
                out["warnings"].append((path, warn))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", action="append", default=None)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--out", default=os.path.join(
        ROOT, "manifests", "batches", "scp-scan.json"))
    args = ap.parse_args()

    roots = args.root or [os.path.join(ROOT, r) for r in DEFAULT_ROOTS]
    started = time.time()
    tasks = []
    for root in roots:
        if not os.path.isdir(root):
            continue
        found = list(iter_scp(root))
        print(f"root {root}: {len(found)} SCP")
        if found:
            tasks.append((os.path.relpath(root, ROOT), found))
    with Pool(args.jobs) as pool:
        parts = pool.map(scan_chunk, tasks)

    total = {
        "files": 0, "lines": 0, "includes": 0, "nodes": 0,
        "encodings": Counter(), "kinds": Counter(), "types": Counter(),
        "keys": Counter(), "depth": Counter(), "warnings": [],
        "window_names": [],
    }
    for part in parts:
        for key in ("files", "lines", "includes", "nodes"):
            total[key] += part[key]
        for key in ("encodings", "kinds", "types", "keys", "depth"):
            total[key].update(part[key])
        total["warnings"].extend(part["warnings"])
        total["window_names"].extend(part["window_names"])

    report = {
        "roots": roots,
        "files": total["files"],
        "lines": total["lines"],
        "includes": total["includes"],
        "nodes": total["nodes"],
        "encodings": dict(total["encodings"]),
        "kinds": dict(total["kinds"]),
        "types": {k: v for k, v in total["types"].most_common(50)},
        "keys": {k: v for k, v in total["keys"].most_common(60)},
        "depth": {str(k): v for k, v in sorted(total["depth"].items())},
        "warnings_count": len(total["warnings"]),
        "warnings_sample": [
            {"path": p, "warning": w} for p, w in total["warnings"][:100]
        ],
        "window_names_sample": total["window_names"][:50],
        "elapsed_s": round(time.time() - started, 2),
    }
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=2)
    print(f"SCP: {report['files']} files, {report['nodes']} blocks, "
          f"{report['includes']} includes, {report['warnings_count']} warnings")
    print("encodings", report["encodings"])
    print("kinds", report["kinds"])
    print("types", report["types"])
    print("report:", args.out)
    return 0 if report["warnings_count"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
