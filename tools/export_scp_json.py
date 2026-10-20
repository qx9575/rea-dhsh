#!/usr/bin/env python
"""Export parsed SCP UI scripts to JSON.

Usage (from the repository root):
    python tools/export_scp_json.py --sample 10 --seed 1 \
        --out extracted/_batches/scp-json-b0 --manifest manifests/batches/scp-json-b0.csv

    python tools/export_scp_json.py --all --jobs 8 \
        --out extracted/scp_json --manifest manifests/batches/scp-json-full.csv

JSON contains encoding, #include list, preprocessor directives, script blocks,
root properties, the WINDOW/FIXEDPART tree (kind/type/line/props/children),
free text lines and parser warnings.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import random
import sys
import time
from collections import Counter
from multiprocessing import Pool

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from dhsh.formats.scp import parse_scp, scp_to_dict, iter_nodes  # noqa: E402

DEFAULT_ROOTS = (
    ("easy", "extracted/easy"),
    ("0a", "extracted/decoded"),
    ("0b", "extracted/decoded_0b"),
)


def iter_scp(root: str):
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if name.lower().endswith(".scp"):
                yield os.path.join(dirpath, name)


def export_chunk(task: tuple[list[tuple[str, str]], dict]) -> dict:
    items, cfg = task
    rows = []
    errors = []
    counters: Counter = Counter()
    for label, path in items:
        counters["files"] += 1
        try:
            data = open(path, "rb").read()
            scp = parse_scp(data)
            doc = scp_to_dict(scp)
            rel = os.path.relpath(path, cfg["root_by_label"][label])
            out = os.path.join(cfg["out_root"], label,
                               os.path.splitext(rel)[0] + ".json")
            os.makedirs(os.path.dirname(out), exist_ok=True)
            doc["path"] = rel.replace(os.sep, "/")
            doc["corpus"] = label
            with open(out, "w", encoding="utf-8") as fh:
                json.dump(doc, fh, ensure_ascii=False, indent=1)
            size = os.path.getsize(out)
        except Exception as exc:  # noqa: BLE001
            errors.append((path, f"{type(exc).__name__}: {exc}"))
            continue
        nodes = sum(1 for _ in iter_nodes(scp.windows))
        counters["written"] += 1
        counters["nodes"] += nodes
        counters["includes"] += len(scp.includes)
        counters["warnings"] += len(scp.warnings)
        counters["bytes"] += size
        rows.append({
            "corpus": label,
            "path": rel.replace(os.sep, "/"),
            "encoding": scp.encoding,
            "includes": len(scp.includes),
            "windows": nodes,
            "scripts": len(scp.scripts),
            "free_text": len(scp.free_text),
            "warnings": len(scp.warnings),
            "bytes": size,
            "out": out,
            "error": "",
        })
    return {"rows": rows, "counters": dict(counters), "errors": errors}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", action="append", default=None,
                    help="corpus as LABEL=PATH (repeatable)")
    ap.add_argument("--sample", type=int, default=0)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--out", default=os.path.join(ROOT, "extracted", "scp_json"))
    ap.add_argument("--manifest", default=None)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()

    if not args.all and not args.sample and not args.limit and not args.dry_run:
        ap.error("refusing full corpus without --all; use --sample/--limit first")

    roots = []
    if args.root:
        for spec in args.root:
            if "=" not in spec:
                ap.error(f"--root expects LABEL=PATH, got {spec!r}")
            label, rel = spec.split("=", 1)
            roots.append((label, os.path.abspath(rel)))
    else:
        roots = [(label, os.path.join(ROOT, rel)) for label, rel in DEFAULT_ROOTS]

    items = []
    root_by_label = {}
    for label, root in roots:
        found = sorted(iter_scp(root))
        print(f"root {label}: {len(found)} SCP")
        if args.limit:
            found = found[: args.limit]
        root_by_label[label] = root
        items.extend((label, p) for p in found)

    if args.sample and args.sample < len(items):
        random.Random(args.seed).shuffle(items)
        items = items[: args.sample]

    if args.dry_run:
        print(f"would export {len(items)} SCP files")
        return 0

    cfg = {
        "out_root": args.out,
        "root_by_label": root_by_label,
    }
    started = time.time()
    nchunks = max(1, args.jobs * 4)
    chunks = [(items[i::nchunks], cfg) for i in range(nchunks)]
    chunks = [c for c in chunks if c[0]]
    with Pool(args.jobs) as pool:
        parts = pool.map(export_chunk, chunks)

    counters: Counter = Counter()
    rows = []
    errors = []
    for part in parts:
        counters.update(part["counters"])
        rows.extend(part["rows"])
        errors.extend(part["errors"])
    elapsed = time.time() - started

    if args.manifest:
        os.makedirs(os.path.dirname(os.path.abspath(args.manifest)), exist_ok=True)
        fields = ["corpus", "path", "encoding", "includes", "windows", "scripts",
                  "free_text", "warnings", "bytes", "out", "error"]
        with open(args.manifest, "w", newline="", encoding="utf-8") as fh:
            writer = csv.DictWriter(fh, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    print(f"SCP -> JSON: {counters['written']}/{counters['files']} files, "
          f"{counters['nodes']} blocks, {counters['includes']} includes, "
          f"{counters['warnings']} warnings, {counters['bytes'] / 1048576:.2f} MiB, "
          f"{len(errors)} errors, {elapsed:.1f}s")
    for path, why in errors[:20]:
        print("  error:", path, why)
    if args.manifest:
        print("manifest:", args.manifest)
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
