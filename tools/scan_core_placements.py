#!/usr/bin/env python
"""Scan MSK/core placement tables across the decoded map corpora.

For every .core/.msk file, parse the container and aggregate the placement
fields (record/x/y/flag/group/frame).  Read-only; reports which maps carry
non-static placements (flag != 1, group != 0, frame != 0) and writes the
per-file table to manifests/batches/core-placement-scan.json.

    python tools/scan_core_placements.py
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from dhsh.formats.map import parse_msk  # noqa: E402

DEFAULT_ROOTS = [
    "extracted/decoded/map", "extracted/decoded/map1",
    "extracted/decoded_0b/map", "extracted/decoded_0b/map1",
    "extracted/easy/map", "extracted/easy/map1",
]
DEFAULT_OUT = "manifests/batches/core-placement-scan.json"


def iter_containers(roots):
    for rel in roots:
        base = os.path.join(ROOT, rel)
        if not os.path.isdir(base):
            continue
        for dirpath, _dirnames, filenames in os.walk(base):
            for name in filenames:
                if name.lower().endswith((".core", ".msk")):
                    yield os.path.join(dirpath, name)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--roots", nargs="*", default=DEFAULT_ROOTS)
    ap.add_argument("--out", default=DEFAULT_OUT)
    args = ap.parse_args()

    flag = collections.Counter()
    group = collections.Counter()
    frame = collections.Counter()
    files = []
    errors = []
    anomalies = []

    for path in iter_containers(args.roots):
        rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
        try:
            msk = parse_msk(open(path, "rb").read())
        except Exception as exc:  # noqa: BLE001
            errors.append({"path": rel, "error": f"{type(exc).__name__}: {exc}"})
            continue
        n = len(msk.placements)
        flag_not_1 = sum(1 for p in msk.placements if p.flag != 1)
        groups = sorted({p.group for p in msk.placements if p.group})
        frames = sorted({p.frame for p in msk.placements if p.frame})
        for p in msk.placements:
            flag[p.flag] += 1
            group[p.group] += 1
            frame[p.frame] += 1
        entry = {"path": rel, "placements": n, "flag_not_1": flag_not_1,
                 "groups": groups, "frames": frames}
        files.append(entry)
        if flag_not_1 or groups or frames:
            anomalies.append(entry)

    result = {
        "roots": args.roots,
        "file_count": len(files) + len(errors),
        "parsed": len(files),
        "placement_count": sum(f["placements"] for f in files),
        "flag": {str(k): v for k, v in sorted(flag.items())},
        "group": {str(k): v for k, v in sorted(group.items())},
        "frame": {str(k): v for k, v in sorted(frame.items())},
        "anomaly_files": anomalies,
        "errors": errors,
        "files": files,
    }
    out = os.path.join(ROOT, args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(result, fh, ensure_ascii=False, indent=1)

    print(f"files={result['file_count']} parsed={result['parsed']} "
          f"placements={result['placement_count']} errors={len(errors)}")
    print(f"flag={result['flag']}")
    print(f"group nonzero={sum(v for k, v in group.items() if k)} "
          f"frame nonzero={sum(v for k, v in frame.items() if k)}")
    print(f"non-static files={len(anomalies)}")
    for a in anomalies:
        print(f"  {a['path']}: placements={a['placements']} "
              f"flag!=1={a['flag_not_1']} groups={a['groups']} "
              f"frames={a['frames']}")
    print("saved:", out)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
