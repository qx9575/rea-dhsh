#!/usr/bin/env python3
"""Diff leaf inventories of PAKF3001 archives between two client installs.

Usage:
    python tools/diff_pak.py --old D:/software/dhsh/shdata \
        --new D:/software/dhsh2/shdata --paks ui,config,800config \
        --out manifests/batches/pak-diff.json
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dhsh.pak import iter_pak  # noqa: E402


def list_entries(path: Path) -> dict[str, dict[str, int]]:
    out: dict[str, dict[str, int]] = {}
    for e in iter_pak(path):
        out[e.path] = {"size": e.size, "kind": e.kind, "depth": e.depth}
    return out


def diff_one(old_path: Path, new_path: Path) -> dict:
    old = list_entries(old_path) if old_path.exists() else {}
    new = list_entries(new_path) if new_path.exists() else {}
    old_names = set(old)
    new_names = set(new)
    added = sorted(new_names - old_names)
    removed = sorted(old_names - new_names)
    changed = []
    for name in sorted(old_names & new_names):
        if old[name]["size"] != new[name]["size"]:
            changed.append({"path": name, "old_size": old[name]["size"],
                            "new_size": new[name]["size"]})
    return {
        "old_path": str(old_path),
        "new_path": str(new_path),
        "old_exists": old_path.exists(),
        "new_exists": new_path.exists(),
        "old_count": len(old),
        "new_count": len(new),
        "added": [{"path": n, **new[n]} for n in added],
        "removed": [{"path": n, **old[n]} for n in removed],
        "changed_size": changed,
        "cursor_old": {n: old[n] for n in sorted(old) if n.lower().startswith("cursor/") or "/cursor/" in n.lower()},
        "cursor_new": {n: new[n] for n in sorted(new) if n.lower().startswith("cursor/") or "/cursor/" in n.lower()},
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--paks", default="ui,config,800config")
    ap.add_argument("--out")
    args = ap.parse_args()
    old_dir = Path(args.old)
    new_dir = Path(args.new)
    result = {}
    for stem in [p.strip() for p in args.paks.split(",") if p.strip()]:
        name = stem if stem.endswith(".pak") else stem + ".pak"
        result[name] = diff_one(old_dir / name, new_dir / name)
        r = result[name]
        print(f"{name}: old={r['old_count']} new={r['new_count']} "
              f"added={len(r['added'])} removed={len(r['removed'])} changed_size={len(r['changed_size'])} "
              f"cursor_old={len(r['cursor_old'])} cursor_new={len(r['cursor_new'])}")
    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(json.dumps(result, ensure_ascii=False, indent=1), encoding="utf-8")
        print(f"wrote {args.out}")
    else:
        print(json.dumps(result, ensure_ascii=False, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
