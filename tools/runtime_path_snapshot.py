#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""runtime_path_snapshot.py — 只读快照 Game.exe 内存里的角色资源路径，并支持前后差分。

用途：不注入、不截图，用“操作前 / 操作后”两次内存快照的差集，找出
换装/显示角色时客户端新加载的 player*/<part>/<model>/<action>/<dir> 路径。

用法:
  python tools/runtime_path_snapshot.py --pid 11172 --out extracted/paperdoll/runtime/paths-before.json
  # 在游戏里换一件装备 / 打开面板 / 让角色出现
  python tools/runtime_path_snapshot.py --pid 11172 --out extracted/paperdoll/runtime/paths-after.json
  python tools/runtime_path_snapshot.py --diff before.json after.json
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import scan_runtime_mem as sm  # noqa: E402

PART = r"(?:body|head|mask|cloth|weapon|shoe|lcuff|rcuff|horse|effect|harmorbody|harmorhead|pet)"
RX = re.compile(
    rb"(?:player\d*|npc\d*)[/\\]" + PART.encode() +
    rb"[/\\]\d+[/\\]\d+[/\\]\d+(?:\.apg)?",
    re.IGNORECASE,
)


def snapshot(pid: int, max_paths: int = 20000,
             self_only: bool = False) -> dict:
    h = sm.open_proc(pid)
    paths: dict[str, dict] = {}
    scanned = 0
    t0 = time.time()
    try:
        for base, size, _protect in sm.iter_regions(h):
            off = 0
            while off < size:
                n = min(sm.CHUNK, size - off)
                data = sm.read_mem(h, base + off, n)
                if data:
                    scanned += len(data)
                    for m in RX.finditer(data):
                        text = m.group(0).decode("latin-1").replace("\\", "/")
                        if text.endswith(".apg"):
                            text = text[:-4]
                        if self_only and not text.startswith("player/"):
                            continue
                        entry = paths.get(text)
                        if entry is None:
                            if len(paths) >= max_paths:
                                continue
                            entry = paths[text] = {"count": 0, "addresses": []}
                        entry["count"] += 1
                        if len(entry["addresses"]) < 3:
                            entry["addresses"].append(
                                f"0x{base + off + m.start():08x}")
                off += n
    finally:
        sm.k32.CloseHandle(h)
    return {
        "pid": pid,
        "scanned_bytes": scanned,
        "seconds": round(time.time() - t0, 1),
        "paths": dict(sorted(paths.items())),
    }


def diff(before: dict, after: dict) -> dict:
    def norm(items):
        out = set()
        for p in items:
            out.add(p[:-4] if p.endswith(".apg") else p)
        return out
    b = norm(before.get("paths", {}))
    a = norm(after.get("paths", {}))
    return {
        "added": sorted(a - b),
        "removed": sorted(b - a),
        "common": len(a & b),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="Game.exe 角色资源路径只读快照/差分")
    ap.add_argument("--pid", type=int)
    ap.add_argument("--out", default="")
    ap.add_argument("--max-paths", type=int, default=20000)
    ap.add_argument("--self-only", action="store_true",
                    help="only keep paths whose prefix is exactly player/")
    ap.add_argument("--diff", nargs=2, metavar=("BEFORE", "AFTER"))
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()

    if args.diff:
        before = json.loads(Path(args.diff[0]).read_text(encoding="utf-8"))
        after = json.loads(Path(args.diff[1]).read_text(encoding="utf-8"))
        result = diff(before, after)
        print(f"added {len(result['added'])}, removed {len(result['removed'])}, "
              f"common {result['common']}")
        for p in result["added"]:
            print("  +", p)
        for p in result["removed"]:
            print("  -", p)
        if args.json_out:
            Path(args.json_out).write_text(
                json.dumps(result, ensure_ascii=False, indent=1), encoding="utf-8")
            print("saved", args.json_out)
        return 0

    if not args.pid:
        ap.error("--pid is required unless --diff is used")
    result = snapshot(args.pid, args.max_paths, args.self_only)
    out = Path(args.out or f"extracted/paperdoll/runtime/paths-{args.pid}.json")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"scanned {result['scanned_bytes']/1e6:.0f} MB in {result['seconds']}s, "
          f"{len(result['paths'])} unique paths -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
