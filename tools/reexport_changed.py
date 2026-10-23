#!/usr/bin/env python
"""Re-export leaves listed as changed in a PAK-update diff manifest.

The diff format is `manifests/batches/pak-update-2026-10-21.json`
(produced by comparing the old and rebuilt pak_inventory.json):
    {"size_changed": [{"pak", "path", "old_size", "new_size"}, ...],
     "added": [...], "removed": [...]}

Only `size_changed` entries with existing NZ wrappers are re-exported by
default.  The previous output (if any) is copied to --backup-root first.
Output layout matches the existing corpora:
    NZ 0x0A -> <out-0a>/<pak stem>/<path>
    NZ 0x0B -> <out-0b>/<pak stem>/<path>
    NZ 0x00 -> <out-0>/<pak stem>/<path>
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from dhsh.nz import HEADER_SIZE, decompress, parse_header  # noqa: E402
from dhsh.pak import safe_relative_path  # noqa: E402

DEFAULT_SOURCE = r"D:\software\dhsh\shdata"


def decode_entry(raw: bytes) -> tuple[bytes, int, int]:
    hdr = parse_header(raw)
    if hdr is None:
        return raw, -1, len(raw)
    if hdr.stored:
        return raw[HEADER_SIZE:], hdr.kind, hdr.size1
    return decompress(hdr.kind, raw[HEADER_SIZE:], hdr.size1), hdr.kind, hdr.size1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", default=DEFAULT_SOURCE)
    ap.add_argument("--inventory", default=os.path.join(ROOT, "pak_inventory.json"))
    ap.add_argument("--old-inventory", default=None,
                    help="optional old inventory; its NZ kind chooses the "
                         "existing corpus root (keeps updated leaves in place)")
    ap.add_argument("--diff", required=True)
    ap.add_argument("--out-0a", default=os.path.join(ROOT, "extracted", "decoded"))
    ap.add_argument("--out-0b", default=os.path.join(ROOT, "extracted", "decoded_0b"))
    ap.add_argument("--out-0", default=os.path.join(ROOT, "extracted", "easy"))
    ap.add_argument("--backup-root", default=os.path.join(
        ROOT, "extracted", "_rebaseline_old"))
    ap.add_argument("--manifest", default=None)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    inv = json.load(open(args.inventory, encoding="utf-8"))
    old_inv = (json.load(open(args.old_inventory, encoding="utf-8"))
               if args.old_inventory else {})
    diff = json.load(open(args.diff, encoding="utf-8"))
    changed = diff.get("size_changed", [])
    rows = []
    errors = []

    def nz_kind(head_hex: str) -> int:
        if len(head_hex) < 4 or head_hex[:4].lower() != "4e5a":
            return -1
        return int(head_hex[6:8] + head_hex[4:6], 16)

    for item in changed:
        pak, path = item["pak"], item["path"]
        entry = next((e for e in inv.get(pak, []) if e["path"] == path), None)
        if entry is None:
            # config.pak changed path case; match case-insensitively
            entry = next((e for e in inv.get(pak, [])
                          if e["path"].lower() == path.lower()), None)
        if entry is None:
            errors.append((pak, path, "not in inventory"))
            continue
        stem = os.path.splitext(pak)[0]
        source = os.path.join(args.source, pak)
        with open(source, "rb") as fh:
            fh.seek(entry["offset"])
            raw = fh.read(entry["size"])
        try:
            payload, kind, size1 = decode_entry(raw)
            if len(payload) != size1:
                raise ValueError(f"decoded {len(payload)} != size1 {size1}")
        except Exception as exc:  # noqa: BLE001
            errors.append((pak, path, f"{type(exc).__name__}: {exc}"))
            continue
        old_entry = next((e for e in old_inv.get(pak, [])
                          if e["path"].lower() == entry["path"].lower()), None)
        old_kind = nz_kind(old_entry.get("head", "")) if old_entry else kind
        layout_kind = old_kind if old_kind in (0, 0x0A, 0x0B) else kind
        out_root = {0x0A: args.out_0a, 0x0B: args.out_0b, 0: args.out_0}.get(
            layout_kind, args.out_0)
        out = os.path.join(out_root, stem, safe_relative_path(entry["path"]))
        sha = hashlib.sha256(payload).hexdigest()
        backup = ""
        if os.path.exists(out):
            backup = os.path.join(args.backup_root, stem,
                                  safe_relative_path(entry["path"]))
            if not args.dry_run:
                os.makedirs(os.path.dirname(backup), exist_ok=True)
                if not os.path.exists(backup):
                    shutil.copy2(out, backup)
        if not args.dry_run:
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "wb") as fh:
                fh.write(payload)
        rows.append({
            "pak": pak, "path": entry["path"], "nz": hex(kind),
            "old_nz": hex(old_kind), "size1": size1, "stored": entry["size"],
            "old_size": item.get("old_size", ""), "out": out,
            "sha256": sha, "backup": backup, "error": "",
        })
        print(f"{pak}: {entry['path']} -> {out} ({len(payload)} B)")
    if args.manifest and not args.dry_run:
        os.makedirs(os.path.dirname(os.path.abspath(args.manifest)),
                    exist_ok=True)
        fields = ["pak", "path", "nz", "old_nz", "size1", "stored",
                  "old_size", "out", "sha256", "backup", "error"]
        with open(args.manifest, "w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=fields)
            w.writeheader()
            w.writerows(rows)
    print(f"re-exported {len(rows)}/{len(changed)}, {len(errors)} errors")
    for e in errors:
        print("  error:", e)
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
