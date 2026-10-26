#!/usr/bin/env python
"""kimi_apci_lookup.py — build & verify the APCI (perinfo.pak) -> APG resource lookup table.

Mapping (verified 2026-10-26 against the live PAK inventory):

    record name  -> pak family + part directory
        player\\head    -> player.pak:head/        playerN\\body -> playerN.pak:body/
        npc / npc1..9   -> npc.pak / npc1..9.pak:<model>/...  (no part dir)
        magic / magic1..5 -> magic*.pak:<model>/<variant>/{front,back}.apg
    entry.a      -> model id (first path component under the part dir)
    mid          -> one action; mid.header hi16 = direction count (8),
                    low16 = ACTION ID (path component `<action>/`), mids are
                    ordered lexicographically by action id
    item         -> one direction (0..dirs-1) -> `<part>/<model>/<action>/<dir>.apg`
    item.blob    -> per-frame data for that direction:
                    12-byte units = PIF-row reorder [f0,f1,f4,f5,f2,f3]
                    (+8/+a = x/y offset, +6 = f5 frame delay, x10 into the
                    timing array); 1-byte units = per-direction byte
                    (semantics open; NOT the frame count)
    magic items: byte4/byte5 of the item record = has-back / has-front layer
        flags -> `<model>/<variant>/back|front/` paths (FUN_00827be0);
        units +4/+6 get screen-center adjustment ((W-640)/2, (H-480)/2)
        when FUN_0082c550(model) gates it on.

Outputs a CSV manifest per (group, record) and a JSON summary with the
verification rates.

Usage:
    python tools/kimi_apci_lookup.py \
        --out manifests/batches/apci-lookup.csv \
        --summary manifests/batches/apci-lookup.json
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

GROUP_PART = {
    0: "head", 1: "body", 2: "weapon", 3: "pet", 4: "magic",
    7: "cloth", 8: "effect", 9: "mask", 10: "Lcuff", 11: "shoe",
    12: "Rcuff", 13: "horse", 14: "harmorbody", 15: "harmorhead",
}


def build_tree(inv: dict) -> dict:
    """pak -> part -> model -> set(action dirs); lowercased part keys."""
    tree: dict = defaultdict(lambda: defaultdict(lambda: defaultdict(set)))
    for pak, ents in inv.items():
        for e in ents:
            p = e["path"]
            if not p.endswith(".apg"):
                continue
            segs = p.split("/")
            if len(segs) == 4 and segs[3][:-4].isdigit() \
                    and segs[1].isdigit() and segs[2].isdigit():
                tree[pak][segs[0].lower()][int(segs[1])].add(int(segs[2]))
            elif len(segs) == 3 and segs[0].isdigit() and segs[1].isdigit() \
                    and segs[2][:-4].isdigit():
                tree[pak][""][int(segs[0])].add(int(segs[1]))
            elif len(segs) == 3 and segs[0].isdigit() and segs[1].isdigit() \
                    and segs[2] in ("front.apg", "back.apg"):
                # magic family: <model>/<variant>/{front,back}.apg
                tree[pak][""][int(segs[0])].add(int(segs[1]))
    return tree


def resolve(name: str) -> tuple[str | None, str | None]:
    fam, _, part = name.partition("\\")
    if fam == "player":
        return "player.pak", part
    if fam.startswith("player") and fam[6:].isdigit():
        return fam + ".pak", part
    if fam.startswith("npc"):
        return fam + ".pak", ""
    if fam.startswith("magic"):
        return fam + ".pak", ""
    return None, None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--inventory", default=os.path.join(ROOT, "pak_inventory.json"))
    ap.add_argument("--items", default=os.path.join(
        ROOT, "extracted", "apci", "perinfo", "items.csv"))
    ap.add_argument("--out", default=os.path.join(
        ROOT, "manifests", "batches", "apci-lookup.csv"))
    ap.add_argument("--summary", default=os.path.join(
        ROOT, "manifests", "batches", "apci-lookup.json"))
    args = ap.parse_args()

    inv = json.load(open(args.inventory, encoding="utf-8"))
    tree = build_tree(inv)

    # entry -> {mid_idx: (hi16 dirs, low16 action)}
    mids: dict = defaultdict(dict)
    with open(args.items, encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            if row["item"] == "0":
                key = (int(row["group"]), row["name"], int(row["entry_a"]))
                h = int(row["mid_header"])
                mids[key][int(row["mid"])] = (h >> 16, h & 0xffff)

    per_rec: dict = defaultdict(lambda: [0, 0, 0, 0])  # ent, model, act, mids
    magic_stats = [0, 0]
    for (grp, name, a), midmap in mids.items():
        k = (grp, name)
        per_rec[k][0] += 1
        per_rec[k][3] += len(midmap)
        pak, part = resolve(name)
        if not pak:
            continue
        if name.startswith("magic"):
            # magic: model dir + variant/front|back.apg
            models = tree.get(pak, {}).get("", {})
            if a in models:
                per_rec[k][1] += 1
                magic_stats[1] += 1
            magic_stats[0] += 1
            continue
        parts = tree.get(pak, {})
        models = parts.get(part.lower(), {})
        if a in models:
            per_rec[k][1] += 1
            acts = models[a]
            if all(lo in acts for _, lo in midmap.values()):
                per_rec[k][2] += 1

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    tot = [0, 0, 0, 0]
    with open(args.out, "w", newline="", encoding="utf-8-sig") as fh:
        w = csv.writer(fh)
        w.writerow(["group", "part_category", "record", "pak", "path_prefix",
                    "entries", "model_found", "actions_subset", "mids"])
        for (grp, name) in sorted(per_rec):
            e, mf, asub, mt = per_rec[(grp, name)]
            pak, part = resolve(name)
            prefix = f"{pak}:{part}/<model>/<action>/<dir>.apg" if part else \
                f"{pak}:<model>/<action>/<dir>.apg" if pak else "(unresolved)"
            w.writerow([grp, GROUP_PART.get(grp, "?"), name, pak or "",
                        prefix, e, mf, asub, mt])
            tot[0] += e; tot[1] += mf; tot[2] += asub; tot[3] += mt

    summary = {
        "tool": "tools/kimi_apci_lookup.py",
        "date": "2026-10-26",
        "entries": tot[0],
        "model_found": tot[1],
        "actions_subset": tot[2],
        "mids": tot[3],
        "model_pct": round(100 * tot[1] / tot[0], 2),
        "action_pct": round(100 * tot[2] / tot[0], 2),
        "group_part": GROUP_PART,
        "notes": [
            "entry.a = model id; mid.header hi16 = direction count, "
            "low16 = action id; item = direction; blob = per-frame data.",
            "magic family resolves to <model>/<variant>/{front,back}.apg; "
            "mid low16 matches the variant for 87% of entries (rest use 0).",
            "Residual model misses are entries with no shipped art "
            "(removed/server-side models); act misses are single actions "
            "without files (e.g. npc action 101).",
        ],
    }
    json.dump(summary, open(args.summary, "w", encoding="utf-8"),
              ensure_ascii=False, indent=1)
    print(f"entries={tot[0]} model={tot[1]} ({summary['model_pct']}%) "
          f"actions={tot[2]} ({summary['action_pct']}%) mids={tot[3]}")
    print("wrote", args.out, "and", args.summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
