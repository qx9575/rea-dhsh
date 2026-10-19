"""PIF B0 probe: parse standalone PIFs and pair them with APG frame counts."""
from __future__ import annotations

import json
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "tools"))
from dhsh.formats.apg import parse_ag  # noqa: E402
from dhsh.formats.pif import parse_pif  # noqa: E402
from dhsh.nz_lzo import lzo1x_decompress  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = r"D:\software\dhsh\shdata"
INV = json.load(open(os.path.join(ROOT, "pak_inventory.json"), encoding="utf-8"))
REPORT = os.path.join(ROOT, "manifests", "batches", "pif-b0.json")


def find_entry(pak: str, logical: str):
    logical = logical.replace("\\", "/").lower()
    for e in INV.get(pak, []):
        if e["name"] == "UNREADABLE":
            continue
        if e["path"].replace("\\", "/").lower() == logical:
            return e
    return None


def read_entry(pak: str, e) -> bytes:
    with open(os.path.join(SRC, pak), "rb") as f:
        f.seek(e["offset"])
        return f.read(e["size"])


def load_nz(raw: bytes):
    if raw[:2] != b"NZ":
        return -1, raw, None
    typ, s1, s2 = struct.unpack_from("<HII", raw, 2)
    payload = raw[12 : 12 + s2]
    if typ == 0:
        return typ, payload, s1
    if typ == 0x0B:
        return typ, lzo1x_decompress(payload, s1), s1
    return typ, None, s1


def find_pif(pak: str, apg_logical: str):
    dirn = os.path.dirname(apg_logical.replace("\\", "/"))
    candidates = []
    # same directory, then parents
    d = dirn
    while d and d != ".":
        candidates.append(d + "/perinfo.dat")
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    # sibling numeric direction directories under the parent (e.g. head/24962/10)
    parent = os.path.dirname(dirn)
    if parent and parent != ".":
        prefix = parent.lower() + "/"
        for e in INV.get(pak, []):
            if e["name"] == "UNREADABLE":
                continue
            p = e["path"].replace("\\", "/")
            if not p.lower().startswith(prefix) or not p.lower().endswith("/perinfo.dat"):
                continue
            middle = p[len(parent) + 1 : -len("/perinfo.dat")]
            if middle.isdigit():
                candidates.append(p)
    seen = set()
    for cand in candidates:
        key = cand.lower()
        if key in seen:
            continue
        seen.add(key)
        e = find_entry(pak, cand)
        if e:
            return cand, e
    return None, None


def main():
    report = {"standalone": [], "paired": [], "blocked_0x0a": 0, "missing": 0, "errors": []}
    # standalone animation PIFs
    for name in ("1.dat", "2.dat", "3.dat"):
        p = os.path.join(SRC, "animation", name)
        if not os.path.exists(p):
            continue
        try:
            pif = parse_pif(open(p, "rb").read())
            apg = os.path.join(SRC, "animation", name.replace(".dat", ".apg"))
            fc = parse_ag(open(apg, "rb").read()).frame_count
            report["standalone"].append(
                {
                    "file": name,
                    "count": pif.count,
                    "width": pif.width,
                    "height": pif.height,
                    "flags": pif.flags,
                    "apg_frames": fc,
                    "match": pif.width == fc,
                    "unique_rows": len(set(pif.rows)),
                    "first_row": list(pif.rows[0]) if pif.rows else [],
                }
            )
        except Exception as exc:  # noqa: BLE001
            report["errors"].append({"file": name, "error": f"{type(exc).__name__}: {exc}"})

    # pair APG samples with sibling perinfo.dat
    sample_root = os.path.join(ROOT, "extracted", "samples", "apg")
    apgs = []
    for dirpath, _dirs, files in os.walk(sample_root):
        for fn in files:
            if fn.lower().endswith(".apg"):
                apgs.append(os.path.join(dirpath, fn))
    apgs.sort()
    for apg_path in apgs:
        rel = os.path.relpath(apg_path, sample_root).replace(os.sep, "/")
        parts = rel.split("/")
        if len(parts) < 2:
            continue
        pak, logical = parts[0] + ".pak", "/".join(parts[1:])
        pif_logical, pif_entry = find_pif(pak, logical)
        if not pif_entry:
            report["missing"] += 1
            continue
        try:
            typ, data, s1 = load_nz(read_entry(pak, pif_entry))
        except Exception as exc:  # noqa: BLE001
            report["errors"].append({"apg": logical, "pif": pif_logical, "error": str(exc)})
            continue
        if typ == 0x0A or data is None:
            report["blocked_0x0a"] += 1
            continue
        try:
            pif = parse_pif(data)
            ag = parse_ag(open(apg_path, "rb").read())
            report["paired"].append(
                {
                    "apg": logical,
                    "pif": pif_logical,
                    "nz_type": typ,
                    "width": pif.width,
                    "height": pif.height,
                    "count": pif.count,
                    "apg_frames": ag.frame_count,
                    "match": pif.width == ag.frame_count,
                    "unique_rows": len(set(pif.rows)),
                    "first_row": list(pif.rows[0]) if pif.rows else [],
                }
            )
        except Exception as exc:  # noqa: BLE001
            report["errors"].append({"apg": logical, "pif": pif_logical, "error": f"{type(exc).__name__}: {exc}"})

    report["summary"] = {
        "standalone": len(report["standalone"]),
        "paired_0x0b": len(report["paired"]),
        "matched_width": sum(1 for x in report["paired"] if x["match"]),
        "blocked_0x0a": report["blocked_0x0a"],
        "missing": report["missing"],
        "errors": len(report["errors"]),
        "unique_row_hist": dict(Counter(x["unique_rows"] for x in report["paired"])),
    }
    os.makedirs(os.path.dirname(REPORT), exist_ok=True)
    with open(REPORT, "w", encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=1)
    print(json.dumps(report["summary"], ensure_ascii=False, indent=1))
    print("standalone:")
    for x in report["standalone"]:
        print(" ", x)
    print("paired sample:")
    for x in report["paired"][:10]:
        print(" ", x)
    print("report:", REPORT)


if __name__ == "__main__":
    main()
