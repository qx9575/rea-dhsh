#!/usr/bin/env python
"""Build the SCP ``#include`` graph across exported corpora.

Usage (from the repository root):
    python tools/scp_include_graph.py --dry-run
    python tools/scp_include_graph.py --out manifests/batches/scp-include-graph.json

Corpora:
    easy  extracted/easy              (NZ type 0 leaves)
    0a    extracted/decoded           (NZ 0x0A leaves)
    0b    extracted/decoded_0b        (NZ 0x0B leaves)

Include paths are written relative to the UI ``config`` root.  The 800x600
overlay tree stores the same logical tree under ``800config/`` and drops the
``config`` segment (``config\\UI\\x.scp`` -> ``800config/ui/x.scp``), so each
include is tried as written and through that alias.  Resolution prefers the
including file's own corpus, then 0a, 0b, easy; all matching copies are
reported because the corpora overlap.

Output JSON:
    stats      counts by resolution status, target extension, depth/cycles
    missing    includes with no file in any corpus
    nodes      per SCP: includes (target/status/resolved copies) and
               included_by reverse edges, plus transitive closure for SCPs
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from dhsh.formats.scp import parse_scp  # noqa: E402

DEFAULT_ROOTS = (
    ("easy", "extracted/easy"),
    ("0a", "extracted/decoded"),
    ("0b", "extracted/decoded_0b"),
)
CORPUS_ORDER = ("0a", "0b", "easy")


def norm_path(path: str) -> str:
    """Normalise a relative path for lookup: forward slashes, lowercase."""
    p = path.strip().replace("\\", "/")
    while p.startswith("./"):
        p = p[2:]
    return p.lstrip("/").lower()


def path_aliases(norm: str) -> list[str]:
    """Return lookup candidates for include path *norm* (already normalised)."""
    out = [norm]
    if norm.startswith("config/"):
        # 800config overlay drops the leading ``config`` segment.
        out.append("800config/" + norm[len("config/"):])
    return out


def walk_files(root: str):
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace("\\", "/")
            yield rel, full


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", action="append", default=None,
                    help="corpus as LABEL=PATH (repeatable)")
    ap.add_argument("--out", default=os.path.join(
        ROOT, "manifests", "batches", "scp-include-graph.json"))
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if args.root:
        roots = []
        for spec in args.root:
            if "=" not in spec:
                ap.error(f"--root expects LABEL=PATH, got {spec!r}")
            label, rel = spec.split("=", 1)
            roots.append((label, os.path.abspath(rel)))
    else:
        roots = [(label, os.path.join(ROOT, rel)) for label, rel in DEFAULT_ROOTS]

    # Per-corpus file index: lowercased relative path -> actual path.
    corpus_index: dict[str, dict[str, list[str]]] = {}
    for label, root in roots:
        index: dict[str, list[str]] = {}
        for rel, _full in walk_files(root):
            index.setdefault(norm_path(rel), []).append(rel)
        corpus_index[label] = index
        print(f"root {label}: {len(index)} logical paths")

    # Global logical index: norm path -> [(corpus, actual path), ...]
    global_index: dict[str, list[tuple[str, str]]] = {}
    for label, index in corpus_index.items():
        for norm, paths in index.items():
            for p in paths:
                global_index.setdefault(norm, []).append((label, p))

    def resolve(corpus: str, include: str) -> tuple[str, list[dict], bool]:
        norm = norm_path(include)
        candidates = path_aliases(norm)
        # Includes written as ``config/...`` first mean the base tree; the
        # 800config overlay alias is only a fallback, otherwise an overlay
        # script would resolve its own base include back to itself.
        for alias, cand in enumerate(candidates):
            hits = corpus_index.get(corpus, {}).get(cand)
            if hits:
                return "same", [{"corpus": corpus, "path": p} for p in hits], alias > 0
            found: list[dict] = []
            for label in CORPUS_ORDER:
                for hit_corpus, path in global_index.get(cand, []):
                    if hit_corpus == label:
                        entry = {"corpus": label, "path": path}
                        if entry not in found:
                            found.append(entry)
            if found:
                return "cross", found, alias > 0
        return "missing", [], False

    nodes: dict[str, dict] = {}
    edges: list[dict] = []
    missing: list[dict] = []
    stats: Counter = Counter()
    target_kinds: Counter = Counter()
    include_counts = Counter()
    dup_counts = 0

    for label, root in roots:
        for rel, full in walk_files(root):
            if not rel.lower().endswith(".scp"):
                continue
            node_id = f"{label}:{rel}"
            stats["files"] += 1
            try:
                scp = parse_scp(open(full, "rb").read())
            except Exception as exc:  # noqa: BLE001
                stats["errors"] += 1
                nodes[node_id] = {"corpus": label, "path": rel,
                                  "error": f"{type(exc).__name__}: {exc}",
                                  "includes": [], "included_by": [], "closure": []}
                continue
            resolved_includes = []
            seen_includes = Counter()
            for include in scp.includes:
                seen_includes[include] += 1
                if seen_includes[include] > 1:
                    dup_counts += 1
                status, resolved, via_alias = resolve(label, include)
                ext = os.path.splitext(norm_path(include))[1] or "(none)"
                target_kinds[ext] += 1
                include_counts[include] += 1
                stats[f"edges_{status}"] += 1
                if via_alias:
                    stats["edges_via_alias"] += 1
                stats["edges"] += 1
                entry = {
                    "target": include,
                    "status": status,
                    "resolved": resolved,
                }
                if via_alias:
                    entry["via_alias"] = True
                if seen_includes[include] > 1:
                    entry["duplicate"] = seen_includes[include]
                resolved_includes.append(entry)
                if status == "missing":
                    missing.append({"corpus": label, "path": rel, "include": include})
                for hit in resolved:
                    edges.append({
                        "from": node_id,
                        "from_path": rel,
                        "target": include,
                        "to": f"{hit['corpus']}:{hit['path']}",
                        "to_corpus": hit["corpus"],
                        "to_path": hit["path"],
                        "status": status,
                        "via_alias": via_alias,
                    })
            nodes[node_id] = {
                "corpus": label,
                "path": rel,
                "includes": resolved_includes,
                "included_by": [],
                "closure": [],
            }

    # Reverse edges (deduplicated) and preferred edges for closure.
    preferred: dict[str, list[str]] = {}
    for edge in edges:
        dst = nodes.get(edge["to"])
        if dst is not None and edge["from"] not in dst["included_by"]:
            dst["included_by"].append(edge["from"])
        preferred.setdefault(edge["from"], [])
        if edge["to"] not in preferred[edge["from"]]:
            preferred[edge["from"]].append(edge["to"])

    # Transitive closure with cycle tracking (iterative DFS per node).
    cycle_nodes: set[str] = set()
    max_depth = 0
    for start in nodes:
        seen: set[str] = set()
        stack = [(start, ())]
        while stack:
            node, path = stack.pop()
            if node in path:
                idx = path.index(node)
                cycle_nodes.add(node)
                cycle_nodes.update(path[idx + 1:])
                continue
            if node in seen:
                continue
            seen.add(node)
            for nxt in preferred.get(node, []):
                if nxt in nodes:
                    stack.append((nxt, path + (node,)))
        closure = sorted(seen - {start})
        nodes[start]["closure"] = closure
        max_depth = max(max_depth, len(closure))
    for node_id, node in nodes.items():
        node["in_cycle"] = node_id in cycle_nodes

    stats["cycle_nodes"] = len(cycle_nodes)
    stats["max_closure"] = max_depth
    stats["duplicate_includes"] = dup_counts
    stats["distinct_includes"] = len(include_counts)
    stats["scp_targets"] = sum(1 for e in edges if e["to"].lower().endswith(".scp"))

    result = {
        "roots": {label: os.path.relpath(root, ROOT).replace("\\", "/")
                  for label, root in roots},
        "stats": dict(stats),
        "target_kinds": dict(target_kinds),
        "missing": missing,
        "top_includes": include_counts.most_common(20),
        "nodes": nodes,
    }

    print(f"SCP files: {stats['files']}  edges: {stats['edges']}  "
          f"same: {stats.get('edges_same', 0)}  cross: {stats.get('edges_cross', 0)}  "
          f"alias: {stats.get('edges_via_alias', 0)}  "
          f"missing: {stats.get('edges_missing', 0)}  duplicates: {dup_counts}")
    print("target kinds:", dict(target_kinds))
    print(f"cycle nodes: {len(cycle_nodes)}  max closure: {max_depth}")
    if missing:
        print("missing includes:")
        for row in missing:
            print(f"  {row['corpus']}:{row['path']} -> {row['include']}")

    if args.dry_run:
        print("dry run: no output written")
        return 0
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(result, fh, ensure_ascii=False, indent=1)
    print("written:", args.out, f"({os.path.getsize(args.out) / 1048576:.2f} MiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
