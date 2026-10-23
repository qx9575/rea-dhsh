#!/usr/bin/env python
"""Scan MAN animation placements and MLK map links (read-only).

MAN: every MAP INI's `[EXT] ANIFILE` points at a `.MAN` table.  Each
100-byte record names an animation directory (`map\\animate\\...`) plus
its map pixel position.  This tool parses all MAN tables, resolves the
directories across the exported corpora, and counts frame files.

MLK: every `config/map/<map>.mlk` holds 108-byte link records.  This tool
parses them, verifies the reverse-link pairing (`paired` equals the
target-side record's `value`), and reports edges whose target map is not
present in the corpus.

Outputs JSON (and optionally an edges CSV).  Never decodes pixels and
never runs the game.

Usage (from the repository root):
    python tools/scan_map_anim.py --limit 5 --out /tmp/map-anim-b0.json
    python tools/scan_map_anim.py \
        --out manifests/batches/map-anim-scan.json \
        --edges-csv manifests/batches/map-anim-edges.csv
"""
from __future__ import annotations

import argparse
import csv
import glob
import hashlib
import json
import os
import sys
import time
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from dhsh.formats.map import (  # noqa: E402
    parse_man_entries, parse_map_ini, parse_mlk_links,
)

DEFAULT_ROOTS = (
    "extracted/decoded",
    "extracted/decoded_0b",
    "extracted/easy",
)
INI_DIR = "extracted/easy/config/map"
FRAME_EXTS = (".a16", ".apg", ".tga")


def norm(path: str) -> str:
    return path.replace("\\", "/").lstrip("/")


def path_variants(rel: str) -> list[str]:
    """Candidate paths for a map-side relative path.

    Some INIs name `map\\Foo\\Foo.MAN` while the exported corpus keeps the
    file under `map1/Foo/Foo.MAN` (and animation dirs may live under
    `map1/animate`).  Try the literal path, the map1 sibling, then the
    stripped `animate/...` form.
    """
    rel = norm(rel)
    out = [rel]
    if rel.startswith("map/"):
        out.append("map1/" + rel[4:])
        out.append(rel[4:])
    return out


def resolve_file(rel: str, exts, roots) -> str | None:
    """Resolve a map-relative file path across the decoded corpora."""
    for root in roots:
        for cand in path_variants(rel):
            path = os.path.join(root, cand)
            if os.path.isfile(path):
                return path
            for ext in exts:
                alt = os.path.splitext(path)[0] + ext
                if os.path.isfile(alt):
                    return alt
    return None


def resolve_dir(rel: str, roots) -> str | None:
    for root in roots:
        for cand in path_variants(rel):
            path = os.path.join(root, cand)
            if os.path.isdir(path):
                return path
    return None


def load_maps(roots):
    """Return [(name, ini, ext_section)] sorted by INI file name."""
    out = []
    for path in sorted(glob.glob(os.path.join(ROOT, INI_DIR, "*.map"))):
        try:
            ini = parse_map_ini(open(path, "rb").read())
        except Exception as exc:  # noqa: BLE001
            out.append((os.path.splitext(os.path.basename(path))[0],
                        path, None, f"{type(exc).__name__}: {exc}"))
            continue
        out.append((os.path.splitext(os.path.basename(path))[0],
                    path, ini.get("ext", {}), None))
    return out


def scan_man(maps, roots, limit=0):
    details = []
    totals = Counter()
    missing_dirs = Counter()
    dir_frames = {}
    dir_state = {}
    frames_by_ext = Counter()
    errors = []

    if limit:
        maps = maps[:limit]
    for name, ini_path, ext, ini_err in maps:
        totals["maps"] += 1
        if ini_err:
            errors.append({"map": name, "stage": "ini", "error": ini_err})
            continue
        anifile = (ext or {}).get("ANIFILE", "")
        if not anifile:
            totals["maps_without_anifile"] += 1
            continue
        totals["maps_with_anifile"] += 1
        man_path = resolve_file(anifile, (".MAN", ".man"), roots)
        if not man_path:
            totals["man_missing"] += 1
            details.append({
                "map": name, "anifile": anifile, "man": "",
                "entries": 0, "unique": 0, "dirs": 0, "resolved": 0,
                "frames": 0, "missing": 0,
            })
            continue
        try:
            entries = parse_man_entries(open(man_path, "rb").read())
        except Exception as exc:  # noqa: BLE001
            totals["man_bad"] += 1
            errors.append({"map": name, "man": os.path.relpath(
                man_path, ROOT).replace(os.sep, "/"),
                "error": f"{type(exc).__name__}: {exc}"})
            continue

        uniq = list(dict.fromkeys(e.path for e in entries))
        resolved = 0
        missing = 0
        frames = 0
        for rel in uniq:
            directory = resolve_dir(rel, roots)
            if not directory:
                missing += 1
                missing_dirs[rel] += 1
                continue
            resolved += 1
            if directory not in dir_frames:
                files = os.listdir(directory)
                count = sum(
                    1 for f in files
                    if os.path.splitext(f)[1].lower() in FRAME_EXTS)
                dir_frames[directory] = count
                for f in files:
                    ext2 = os.path.splitext(f)[1].lower()
                    if ext2 in FRAME_EXTS:
                        frames_by_ext[ext2] += 1
                dir_state[directory] = count > 0
            frames += dir_frames[directory]
        totals["entries"] += len(entries)
        totals["unique_entries"] += len(uniq)
        totals["dir_refs"] += resolved
        totals["frames"] += frames
        details.append({
            "map": name,
            "anifile": anifile,
            "man": os.path.relpath(man_path, ROOT).replace(os.sep, "/"),
            "entries": len(entries),
            "unique": len(uniq),
            "dirs": len(uniq),
            "resolved": resolved,
            "frames": frames,
            "missing": missing,
        })

    totals["unique_dirs"] = len(dir_frames)
    totals["dirs_with_frames"] = sum(1 for v in dir_state.values() if v)
    totals["dirs_without_frames"] = sum(
        1 for v in dir_state.values() if not v)
    totals["frame_files"] = sum(dir_frames.values())
    return {
        "totals": dict(totals),
        "frame_exts": dict(frames_by_ext),
        "missing_dirs": [
            {"path": p, "refs": n} for p, n in missing_dirs.most_common()
        ],
        "maps": details,
        "errors": errors,
    }


def collect_mlk(roots):
    """Return {stem: (path, bytes)} preferring the first root; flag dupes."""
    by = {}
    duplicates = []
    for root in roots:
        for path in sorted(glob.glob(os.path.join(root, "config", "map",
                                                  "*.mlk"))):
            stem = os.path.splitext(os.path.basename(path))[0]
            data = open(path, "rb").read()
            digest = hashlib.sha256(data).hexdigest()
            if stem not in by:
                by[stem] = (path, data, digest)
            elif by[stem][2] != digest:
                duplicates.append({
                    "map": stem,
                    "path": os.path.relpath(path, ROOT).replace(os.sep, "/"),
                    "first": os.path.relpath(by[stem][0], ROOT).replace(
                        os.sep, "/"),
                })
    return by, duplicates


def scan_mlk(roots, map_names):
    by, duplicates = collect_mlk(roots)
    links_by_map = {}
    errors = []
    for stem, (path, data, _digest) in sorted(by.items()):
        try:
            links_by_map[stem] = parse_mlk_links(data)
        except Exception as exc:  # noqa: BLE001
            errors.append({
                "map": stem,
                "mlk": os.path.relpath(path, ROOT).replace(os.sep, "/"),
                "error": f"{type(exc).__name__}: {exc}",
            })

    def stem_of(target: str) -> str:
        return target[:-4] if target.lower().endswith(".map") else target

    edges = []
    for source, links in sorted(links_by_map.items()):
        for link in links:
            target = stem_of(link.target)
            reverse = links_by_map.get(target, ())
            back = [r for r in reverse
                    if stem_of(r.target).lower() == source.lower()]
            has_reverse = bool(back)
            pair_ok = any(r.value == link.paired and
                          r.paired == link.value for r in back)
            edges.append({
                "source": source,
                "value": link.value,
                "target": link.target,
                "target_stem": target,
                "target_exists": target.lower() in map_names,
                "paired": link.paired,
                "rect": [link.x, link.y, link.w, link.h],
                "dest": [link.dest_x, link.dest_y],
                "extra": [link.extra0, link.extra1, link.extra2],
                "adjusted": link.adjusted,
                "has_reverse": has_reverse,
                "pair_ok": pair_ok,
            })

    missing_targets = [e for e in edges if not e["target_exists"]]
    unpaired = [e for e in edges if e["has_reverse"] and not e["pair_ok"]]
    no_reverse = [e for e in edges if not e["has_reverse"]]
    sequential = 0
    for links in links_by_map.values():
        if [l.value for l in links] == list(range(len(links))):
            sequential += 1

    return {
        "files": len(links_by_map),
        "records": len(edges),
        "value_sequential_files": sequential,
        "files_without_ini": sorted(
            s for s in links_by_map if s.lower() not in map_names),
        "missing_targets": missing_targets,
        "no_reverse": no_reverse,
        "pair_mismatch": unpaired,
        "paired_ok": sum(1 for e in edges if e["pair_ok"]),
        "adjusted_records": sum(1 for e in edges if e["adjusted"]),
        "duplicate_files": duplicates,
        "edges": edges,
        "errors": errors,
    }


def write_edges_csv(path, edges):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fields = ["source", "value", "target", "target_exists", "paired",
              "rect_x", "rect_y", "rect_w", "rect_h", "dest_x", "dest_y",
              "extra0", "extra1", "extra2", "adjusted", "has_reverse",
              "pair_ok"]
    scalar = [f for f in fields if f not in (
        "rect_x", "rect_y", "rect_w", "rect_h", "dest_x", "dest_y",
        "extra0", "extra1", "extra2")]
    with open(path, "w", newline="", encoding="utf-8-sig") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields)
        writer.writeheader()
        for e in edges:
            row = {k: e[k] for k in scalar}
            row.update({
                "rect_x": e["rect"][0], "rect_y": e["rect"][1],
                "rect_w": e["rect"][2], "rect_h": e["rect"][3],
                "dest_x": e["dest"][0], "dest_y": e["dest"][1],
                "extra0": e["extra"][0], "extra1": e["extra"][1],
                "extra2": e["extra"][2],
            })
            writer.writerow(row)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", action="append", default=None,
                    help="corpus root (repeatable); defaults to "
                         "decoded/decoded_0b/easy")
    ap.add_argument("--limit", type=int, default=0,
                    help="only scan the first N map INIs (MAN section)")
    ap.add_argument("--map", action="append", default=None,
                    help="map stem to scan (repeatable; MAN section)")
    ap.add_argument("--out", default=os.path.join(
        ROOT, "manifests", "batches", "map-anim-scan.json"))
    ap.add_argument("--edges-csv", default="")
    args = ap.parse_args()

    roots = [os.path.join(ROOT, r) for r in (args.root or DEFAULT_ROOTS)]
    roots = [r for r in roots if os.path.isdir(r)]
    if not roots:
        print("no corpus roots found")
        return 2
    started = time.time()
    maps = load_maps(roots)
    if args.map:
        wanted = {m.lower() for m in args.map}
        maps = [m for m in maps if m[0].lower() in wanted]
        missing = wanted - {m[0].lower() for m in maps}
        if missing:
            print("map(s) not found:", ", ".join(sorted(missing)))
    man = scan_man(maps, roots, args.limit)
    map_names = {name.lower() for name, _p, _e, _err in load_maps(roots)}
    mlk = scan_mlk(roots, map_names)
    report = {
        "tool": "tools/scan_map_anim.py",
        "roots": [os.path.relpath(r, ROOT).replace(os.sep, "/")
                  for r in roots],
        "man": man,
        "mlk": mlk,
        "elapsed_s": round(time.time() - started, 2),
    }
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=2)
    if args.edges_csv:
        write_edges_csv(args.edges_csv, mlk["edges"])

    t = man["totals"]
    print(f"MAN: {t.get('maps_with_anifile', 0)} maps, "
          f"{t.get('entries', 0)} entries, "
          f"{t.get('unique_entries', 0)} unique/map, "
          f"{t.get('unique_dirs', 0)} dirs "
          f"({t.get('dirs_without_frames', 0)} without frames), "
          f"{t.get('frame_files', 0)} frame files, "
          f"{len(man['missing_dirs'])} missing dirs")
    print(f"MLK: {mlk['files']} files, {mlk['records']} records, "
          f"{mlk['paired_ok']} paired "
          f"({len(mlk['pair_mismatch'])} mismatches), "
          f"{len(mlk['missing_targets'])} missing targets, "
          f"{len(mlk['no_reverse'])} without reverse")
    errs = man["errors"] + mlk["errors"]
    print(f"errors: {len(errs)}; report={args.out}")
    if args.edges_csv:
        print(f"edges csv: {args.edges_csv}")
    return 0 if not errs else 1


if __name__ == "__main__":
    sys.exit(main())
