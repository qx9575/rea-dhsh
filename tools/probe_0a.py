#!/usr/bin/env python
"""Validate the NZ type 0x0A (16-bit LZW) decoder on random PAK leaves.

Usage (from the tools directory or repository root):
    python tools/probe_0a.py --count 100 --seed 1 --batch b1
    python tools/probe_0a.py --count 1000 --seed 2 --batch b2

Each sampled leaf is read straight from its PAK using pak_inventory.json,
decoded, checked against the NZ header size1, and sanity-checked by extension
(APG/PIF parse, image magic, UTF-8/GB18030 text decode).
"""
from __future__ import annotations

import argparse
import json
import os
import random
import struct
import sys
import time
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
sys.path.insert(0, TOOLS)

from dhsh.nz import HEADER_SIZE, parse_header, decompress  # noqa: E402
from dhsh.formats.apg import parse_ag  # noqa: E402
from dhsh.formats.pif import parse_pif  # noqa: E402

SOURCE = os.environ.get("DHSH_SOURCE", r"D:\software\dhsh\shdata")
INVENTORY = os.path.join(ROOT, "pak_inventory.json")

TEXT_EXTS = {".scp", ".csv", ".lua", ".txt", ".ini", ".map", ".cfg"}
MAGIC = {
    ".apg": b"AG",
    ".dat": b"PIF\0",
    ".a16": b"AP",
    ".cti": b"CT",
    ".bmp": b"BM",
    ".jpg": b"\xff\xd8",
    ".psd": b"8BPS",
    ".zip": b"PK",
    ".msk": b"MSK",
    ".pg2": b"PG",
    ".mlk": b"lnk",
}


def load_leaves(types: set[int]) -> list[dict]:
    inv = json.load(open(INVENTORY, encoding="utf-8"))
    out: list[dict] = []
    for pak, entries in inv.items():
        for e in entries:
            if e.get("name") == "UNREADABLE":
                continue
            head = e.get("head") or ""
            if len(head) < HEADER_SIZE * 2:
                continue
            raw = bytes.fromhex(head[: HEADER_SIZE * 2])
            if raw[:2] != b"NZ":
                continue
            kind, size1, size2 = struct.unpack("<HII", raw[2:12])
            if kind not in types:
                continue
            out.append({
                "pak": pak,
                "path": e["path"],
                "ext": os.path.splitext(e["name"])[1].lower() or "<none>",
                "offset": e["offset"],
                "size": e["size"],
                "size1": size1,
                "size2": size2,
            })
    return out


def read_leaf(handles: dict, leaf: dict) -> tuple[bytes, bytes]:
    fh = handles.get(leaf["pak"])
    if fh is None:
        fh = open(os.path.join(SOURCE, leaf["pak"]), "rb")
        handles[leaf["pak"]] = fh
    fh.seek(leaf["offset"])
    size = leaf["size"]
    if size < HEADER_SIZE:
        head = fh.read(HEADER_SIZE)
        nz = parse_header(head)
        if nz is None:
            raise ValueError("not NZ")
        body = fh.read(nz.size2)
        return head, body
    data = fh.read(size)
    nz = parse_header(data)
    if nz is None:
        raise ValueError("not NZ")
    return data[:HEADER_SIZE], data[HEADER_SIZE:HEADER_SIZE + nz.size2]


def _decode_gb18030(data: bytes) -> tuple[str, int]:
    """Decode GB18030, treating standalone 0x80 as the CP936 euro sign.

    ``0x80`` is a valid GBK trail byte (e.g. ``a8 80``); only a byte the
    decoder cannot pair is replaced.  Returns the text and replacement count.
    """
    try:
        return data.decode("gb18030"), 0
    except UnicodeDecodeError:
        pass
    out: list[str] = []
    rest = data
    replaced = 0
    while rest:
        try:
            out.append(rest.decode("gb18030"))
            break
        except UnicodeDecodeError as exc:
            if rest[exc.start] != 0x80:
                raise
            out.append(rest[: exc.start].decode("gb18030"))
            out.append("€")
            replaced += 1
            rest = rest[exc.start + 1:]
    return "".join(out), replaced


def decode_text(data: bytes) -> tuple[str, str]:
    """Decode script/config text and report the codec that succeeded.

    Most files are GBK; newer UI scripts are UTF-8.  GB18030 is a strict
    superset of GBK and covers its unassigned blocks (e.g. 0xAAA1).
    """
    try:
        return data.decode("utf-8"), "utf-8"
    except UnicodeDecodeError:
        pass
    text, replaced = _decode_gb18030(data)
    return text, "gb18030" if not replaced else f"gb18030+cp936({replaced})"


def check_output(ext: str, data: bytes) -> tuple[bool, str]:
    # ``lnk`` map-link files are binary; some size=0 .ini aliases point at
    # the same payload, so check the magic before treating anything as text.
    if data.startswith(b"lnk\0"):
        return True, "lnk binary"
    if ext in TEXT_EXTS:
        try:
            text, codec = decode_text(data)
        except UnicodeDecodeError as exc:
            return False, f"text decode error: {exc}"
        bad = text.count("�")
        nul = text.count("\x00")
        if len(text) and (bad / len(text) > 0.02 or nul / len(text) > 0.01):
            return False, f"text suspicious bad={bad} nul={nul} len={len(text)}"
        return True, f"text ok ({codec}) chars={len(text)}"
    if ext == ".apg":
        if data[:2] != b"AG":
            return False, "missing AG"
        try:
            ag = parse_ag(data)
        except ValueError as exc:
            return False, f"AG parse: {exc}"
        return True, f"AG frames={ag.frame_count} hdr={ag.header_size}"
    if ext == ".dat":
        if data[:4] != b"PIF\0":
            return False, "missing PIF"
        try:
            pif = parse_pif(data)
        except ValueError as exc:
            return False, f"PIF parse: {exc}"
        return True, f"PIF {pif.width}x{pif.height}"
    want = MAGIC.get(ext)
    if want:
        if not data.startswith(want):
            return False, f"magic {data[:8].hex(' ')} != {want.hex(' ')}"
        return True, "magic ok"
    if ext == ".tga":
        if len(data) < 18:
            return False, "short TGA"
        return True, f"TGA type={data[2]}"
    if ext == ".bmp":
        return (data[:2] == b"BM"), "BMP magic"
    return True, f"head={data[:8].hex(' ')}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--batch", default="b1")
    ap.add_argument("--types", default="0x0a", help="comma list, e.g. 0x0a,0x0b")
    ap.add_argument("--exts", help="comma list of extensions to keep, e.g. .dat,.csv")
    ap.add_argument("--out", help="JSON report path")
    args = ap.parse_args()

    types = {int(p, 0) for p in args.types.replace(",", " ").split()}
    leaves = load_leaves(types)
    if args.exts:
        wanted = {
            (p if p.startswith(".") else "." + p).lower()
            for p in args.exts.replace(";", ",").split(",")
            if p.strip()
        }
        leaves = [leaf for leaf in leaves if leaf["ext"] in wanted]
    print(f"inventory: {len(leaves)} leaves of type {sorted(hex(t) for t in types)}")
    rng = random.Random(args.seed)
    picked = rng.sample(leaves, min(args.count, len(leaves)))

    handles: dict[str, object] = {}
    by_ext = Counter()
    by_ext_ok = Counter()
    failures: list[dict] = []
    samples: list[dict] = []
    started = time.time()
    total_out = 0

    try:
        for i, leaf in enumerate(picked, 1):
            rec = dict(leaf)
            try:
                head, payload = read_leaf(handles, leaf)
                nz = parse_header(head)
                data = decompress(nz.kind, payload, nz.size1)
                ok_len = len(data) == nz.size1
                ok_check, note = check_output(leaf["ext"], data)
                ok = ok_len and ok_check
                rec.update({
                    "out_len": len(data),
                    "head": data[:12].hex(" "),
                    "check": note,
                    "ok": ok,
                })
                if not ok:
                    rec["error"] = (
                        f"length {len(data)} != {nz.size1}" if not ok_len else note
                    )
            except Exception as exc:  # noqa: BLE001 - report every failure
                ok = False
                rec["ok"] = False
                rec["error"] = f"{type(exc).__name__}: {exc}"
            by_ext[leaf["ext"]] += 1
            if ok:
                by_ext_ok[leaf["ext"]] += 1
                total_out += len(data)
            else:
                failures.append(rec)
                print(f"FAIL [{i}/{len(picked)}] {leaf['pak']}:{leaf['path']} {rec.get('error')}",
                      file=sys.stderr)
            if len(samples) < 40:
                samples.append(rec)
            if i % 100 == 0:
                print(f"  {i}/{len(picked)} decoded, {len(failures)} failed, "
                      f"{total_out / 1048576:.1f} MiB out", flush=True)
    finally:
        for fh in handles.values():
            fh.close()

    elapsed = time.time() - started
    report = {
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "batch": args.batch,
        "algorithm": "NZ 0x0A: 16-bit LZW (Game.exe VA 0x8a6f60)",
        "types": sorted(types),
        "requested": len(picked),
        "decoded": len(picked) - len(failures),
        "failed": len(failures),
        "output_bytes": total_out,
        "elapsed_seconds": round(elapsed, 1),
        "by_ext": {
            ext: {"total": by_ext[ext], "ok": by_ext_ok[ext]}
            for ext in sorted(by_ext)
        },
        "failures": failures,
        "sample_rows": samples,
    }
    out = args.out or os.path.join(ROOT, "manifests", "batches", f"nz0a-{args.batch}.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=1)
    print(f"\n{report['decoded']}/{report['requested']} decoded, {report['failed']} failed, "
          f"{total_out / 1048576:.1f} MiB output in {elapsed:.1f}s")
    print("by ext:", {k: f"{v['ok']}/{v['total']}" for k, v in report["by_ext"].items()})
    print("wrote", out)
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
