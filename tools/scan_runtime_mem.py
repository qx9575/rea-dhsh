#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""scan_runtime_mem.py — 只读扫描 Game.exe 内存中的字符串/路径（无注入）。

用法:
  python tools/scan_runtime_mem.py --pid 22792 --text 幼螈宫 --text 宠物 --pet-paths
  python tools/scan_runtime_mem.py --pid 22792 --path-contains body/450

输出: extracted/paperdoll/runtime/memscan-<pid>.json
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import io
import json
import re
import sys
import time
from pathlib import Path

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

class MEMORY_BASIC_INFORMATION64(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_ulonglong),
        ("AllocationBase", ctypes.c_ulonglong),
        ("AllocationProtect", wt.DWORD),
        ("__alignment1", wt.DWORD),
        ("RegionSize", ctypes.c_ulonglong),
        ("State", wt.DWORD),
        ("Protect", wt.DWORD),
        ("Type", wt.DWORD),
        ("__alignment2", wt.DWORD),
    ]


class MEMORY_BASIC_INFORMATION32(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", wt.DWORD),
        ("AllocationBase", wt.DWORD),
        ("AllocationProtect", wt.DWORD),
        ("RegionSize", wt.DWORD),
        ("State", wt.DWORD),
        ("Protect", wt.DWORD),
        ("Type", wt.DWORD),
    ]


MBI = (MEMORY_BASIC_INFORMATION64 if ctypes.sizeof(ctypes.c_void_p) == 8
       else MEMORY_BASIC_INFORMATION32)

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.ReadProcessMemory.restype = wt.BOOL
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p,
                               ctypes.POINTER(MBI), ctypes.c_size_t]
k32.CloseHandle.argtypes = [wt.HANDLE]

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
MEM_COMMIT = 0x1000
PAGE_NOACCESS = 0x01
PAGE_GUARD = 0x100
CHUNK = 2 * 1024 * 1024

PRINTABLE = set(range(0x20, 0x7F)) | {9}


def open_proc(pid: int):
    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        raise OSError(f"OpenProcess({pid}) err={ctypes.get_last_error()}")
    return h


def iter_regions(h):
    mbi = MBI()
    addr = 0
    limit = 0x80000000
    while addr < limit:
        got = k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi),
                                 ctypes.sizeof(mbi))
        if not got:
            break
        base = int(mbi.BaseAddress or 0)
        size = int(mbi.RegionSize or 0)
        if size <= 0:
            addr = base + 0x1000
            continue
        if (mbi.State == MEM_COMMIT and not (mbi.Protect & PAGE_NOACCESS)
                and not (mbi.Protect & PAGE_GUARD)):
            yield base, size, int(mbi.Protect)
        addr = base + size


def read_mem(h, addr, size):
    buf = (ctypes.c_char * size)()
    n = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size,
                                 ctypes.byref(n)):
        return None
    return buf.raw[: n.value]


def context_ascii(data: bytes, pos: int, span: int = 80) -> str:
    lo = max(0, pos - span)
    hi = min(len(data), pos + span)
    seg = data[lo:hi]
    return "".join(chr(b) if b in PRINTABLE else "." for b in seg)


def printable_run(data: bytes, pos: int, limit: int = 200) -> str:
    lo = pos
    while lo > 0 and data[lo - 1] in PRINTABLE:
        lo -= 1
    hi = pos
    while hi < len(data) and data[hi] in PRINTABLE:
        hi += 1
    if hi - lo > limit:
        return data[pos:pos + limit].decode("latin-1", "replace")
    return data[lo:hi].decode("latin-1", "replace")


def main() -> int:
    ap = argparse.ArgumentParser(description="Game.exe 内存只读字符串扫描")
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--text", action="append", default=[],
                    help="待搜索文本（自动尝试 GBK/UTF-16LE/UTF-8），可重复")
    ap.add_argument("--pet-paths", action="store_true",
                    help="额外搜索 pet/ 与 pet\\ 路径片段")
    ap.add_argument("--path-contains", action="append", default=[],
                    help="搜索可打印路径串中包含该子串的记录（如 body/450），可重复")
    ap.add_argument("--max-hits", type=int, default=80)
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    patterns: list[tuple[str, bytes]] = []
    for text in args.text:
        for enc in ("gbk", "utf-16-le", "utf-8"):
            try:
                raw = text.encode(enc)
            except UnicodeEncodeError:
                continue
            if len(raw) >= 2:
                patterns.append((f"{text}/{enc}", raw))
    if args.pet_paths:
        for raw in (b"pet/", b"pet\\", b"\\pet\\", b"/pet/"):
            patterns.append((f"path:{raw!r}", raw))
    path_needles = [(sub, sub.encode("latin-1")) for sub in args.path_contains]

    h = open_proc(args.pid)
    t0 = time.time()
    hits: list[dict] = []
    per_pat: dict[str, int] = {name: 0 for name, _ in patterns}
    path_strings: dict[str, dict] = {}
    scanned = 0
    try:
        for base, size, protect in iter_regions(h):
            offset = 0
            while offset < size:
                n = min(CHUNK, size - offset)
                data = read_mem(h, base + offset, n)
                if data:
                    scanned += len(data)
                    for name, raw in patterns:
                        if per_pat[name] >= args.max_hits:
                            continue
                        start = 0
                        while True:
                            pos = data.find(raw, start)
                            if pos < 0:
                                break
                            per_pat[name] += 1
                            addr = base + offset + pos
                            hits.append({
                                "pattern": name,
                                "address": f"0x{addr:08x}",
                                "context": context_ascii(data, pos),
                            })
                            start = pos + 1
                            if per_pat[name] >= args.max_hits:
                                break
                    if args.pet_paths:
                        for match in re.finditer(rb"[ -~]{0,64}pet[\\/][ -~]{0,64}",
                                                 data, re.IGNORECASE):
                            text = match.group(0).decode("latin-1")
                            addr = base + offset + match.start()
                            entry = path_strings.get(text)
                            if entry is None:
                                if len(path_strings) >= 200:
                                    continue
                                entry = path_strings[text] = {
                                    "text": text, "count": 0,
                                    "addresses": [],
                                }
                            entry["count"] += 1
                            if len(entry["addresses"]) < 5:
                                entry["addresses"].append(f"0x{addr:08x}")
                    for sub, raw in path_needles:
                        start = 0
                        while True:
                            pos = data.find(raw, start)
                            if pos < 0:
                                break
                            text = printable_run(data, pos)
                            addr = base + offset + pos
                            entry = path_strings.get(text)
                            if entry is None:
                                if len(path_strings) >= 400:
                                    start = pos + 1
                                    continue
                                entry = path_strings[text] = {
                                    "text": text, "count": 0,
                                    "addresses": [],
                                }
                            entry["count"] += 1
                            if len(entry["addresses"]) < 5:
                                entry["addresses"].append(f"0x{addr:08x}")
                            start = pos + 1
                offset += n
    finally:
        k32.CloseHandle(h)

    result = {
        "pid": args.pid,
        "scanned_bytes": scanned,
        "seconds": round(time.time() - t0, 1),
        "patterns": per_pat,
        "hits": hits,
        "path_strings": sorted(path_strings.values(), key=lambda e: -e["count"]),
    }
    out = Path(args.out or f"extracted/paperdoll/runtime/memscan-{args.pid}.json")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, ensure_ascii=False, indent=1), encoding="utf-8")

    print(f"scanned {scanned/1e6:.0f} MB in {result['seconds']}s -> {out}")
    for name, count in per_pat.items():
        print(f"  {name}: {count}")
    for item in hits[:20]:
        print(f"  {item['address']} {item['pattern']}: {item['context']}")
    if path_strings:
        print("paths:")
        for item in result["path_strings"][:80]:
            print(f"  x{item['count']:<3} {item['text']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
