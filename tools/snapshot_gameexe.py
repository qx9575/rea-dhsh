#!/usr/bin/env python
"""Snapshot Game.exe and report drift across runs.

The client's protector/launcher may rewrite Game.exe after a run, so the
on-disk SHA-256 is not a stable analysis baseline.  This tool:
  1. hashes the live Game.exe;
  2. stores each new SHA under extracted/gameexe-snapshots/Game-<sha>.exe;
  3. compares a new snapshot with the previous one (byte diff, changed 4 KiB
     pages, key analysis-address bytes) and appends a JSON record.

Usage:
    python tools/snapshot_gameexe.py
    python tools/snapshot_gameexe.py --source D:/software/dhsh/shdata/Game.exe \
        --dir extracted/gameexe-snapshots \
        --record manifests/batches/gameexe-snapshots.json
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SOURCE = r"D:\software\dhsh\shdata\Game.exe"
DEFAULT_DIR = os.path.join(ROOT, "extracted", "gameexe-snapshots")
DEFAULT_RECORD = os.path.join(ROOT, "manifests", "batches",
                              "gameexe-snapshots.json")

# VAs used by the current mask/APG analysis (image base 0x400000).
KEY_VAS = {
    "mask_parser": 0x4272D0,
    "mask_loader": 0x4278C0,
    "apply_mask": 0x4266D0,
    "depth_test": 0x426756,
    "group_frame": 0x426980,
    "mask_darken": 0x4264B0,
}


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_record(path: str) -> dict:
    if os.path.exists(path):
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    return {"snapshots": []}


def save_record(path: str, rec: dict) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(rec, fh, ensure_ascii=False, indent=1)


def compare(old: bytes, new: bytes) -> dict:
    n = min(len(old), len(new))
    diff = sum(1 for i in range(n) if old[i] != new[i])
    pages = set()
    for i in range(0, n, 4096):
        if old[i:i + 4096] != new[i:i + 4096]:
            pages.add(i // 4096)
    keys = {}
    for name, va in KEY_VAS.items():
        off = va - 0x400000
        a = old[off:off + 16] if 0 <= off < len(old) else b""
        b = new[off:off + 16] if 0 <= off < len(new) else b""
        keys[name] = {"va": hex(va), "old": a.hex(), "new": b.hex(),
                      "same": a == b}
    return {
        "old_size": len(old),
        "new_size": len(new),
        "different_bytes": diff,
        "changed_4k_pages": len(pages),
        "key_bytes": keys,
        "identical": old == new,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", default=DEFAULT_SOURCE)
    ap.add_argument("--dir", default=DEFAULT_DIR)
    ap.add_argument("--record", default=DEFAULT_RECORD)
    ap.add_argument("--force", action="store_true",
                    help="snapshot even if this SHA is already recorded")
    args = ap.parse_args()

    if not os.path.exists(args.source):
        print("source not found:", args.source)
        return 1
    data = open(args.source, "rb").read()
    sha = sha256_bytes(data)
    mtime = os.path.getmtime(args.source)
    os.makedirs(args.dir, exist_ok=True)
    snap = os.path.join(args.dir, f"Game-{sha}.exe")
    rec = load_record(args.record)
    known = {s["sha256"]: s for s in rec.get("snapshots", [])}
    if sha not in known or args.force:
        if not os.path.exists(snap):
            shutil.copy2(args.source, snap)
        entry = {
            "sha256": sha,
            "size": len(data),
            "mtime": time.strftime("%Y-%m-%dT%H:%M:%S",
                                   time.localtime(mtime)),
            "snapshot": os.path.relpath(snap, ROOT).replace(os.sep, "/"),
            "first_seen": time.strftime("%Y-%m-%dT%H:%M:%S"),
        }
        prev = None
        if rec.get("snapshots"):
            prev = rec["snapshots"][-1]
        rec.setdefault("snapshots", []).append(entry)
        save_record(args.record, rec)
        print("new snapshot:", sha, "->", entry["snapshot"])
        if prev and os.path.exists(os.path.join(ROOT, prev["snapshot"])):
            old = open(os.path.join(ROOT, prev["snapshot"]), "rb").read()
            diff = compare(old, data)
            print("compared with previous", prev["sha256"][:16])
            print("  sizes", diff["old_size"], "->", diff["new_size"],
                  "different bytes", diff["different_bytes"],
                  "changed 4K pages", diff["changed_4k_pages"])
            for name, k in diff["key_bytes"].items():
                print(f"  {name:14} {k['va']} same={k['same']}")
            entry["comparison"] = diff
            save_record(args.record, rec)
    else:
        print("already recorded:", sha)
    print("snapshot dir:", args.dir)
    print("record:", args.record)
    return 0


if __name__ == "__main__":
    sys.exit(main())
