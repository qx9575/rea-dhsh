#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""kimi_mem_read.py — 只读转储 Game.exe 指定地址的内存（ReadProcessMemory，无注入无挂钩）。

用法:
  python tools/kimi_mem_read.py --pid 12345 --reads 0xae132c:51 0xae1360:20 [--out .rea/dump.json]

输出 JSON: 每个地址的 hex 字节 + 可打印预览（敏感字段由调用方自行脱敏）。
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import io
import json
import os
import sys

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.ReadProcessMemory.restype = wt.BOOL


def open_proc(pid: int):
    h = kernel32.OpenProcess(0x0010 | 0x0400, False, pid)  # VM_READ | QUERY_INFORMATION
    if not h:
        raise OSError(f"OpenProcess({pid}) 失败 err={ctypes.get_last_error()}")
    return h


def read_mem(h, addr: int, size: int) -> bytes:
    buf = (ctypes.c_char * size)()
    n = ctypes.c_size_t(0)
    if not kernel32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(n)):
        raise OSError(f"RPM(0x{addr:x},{size}) 失败 err={ctypes.get_last_error()}")
    return buf.raw[: n.value]


def preview(data: bytes) -> str:
    return "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in data)


def main() -> int:
    ap = argparse.ArgumentParser(description="Game.exe 内存只读转储")
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--reads", nargs="+", required=True,
                    help="地址:长度 列表，如 0xae132c:51 0xad0f10:1032")
    ap.add_argument("--out", default=None, help="结果 JSON 路径")
    args = ap.parse_args()

    h = open_proc(args.pid)
    result = {"pid": args.pid, "reads": {}}
    try:
        for spec in args.reads:
            addr_s, size_s = spec.split(":")
            addr, size = int(addr_s, 16), int(size_s, 0)
            data = read_mem(h, addr, size)
            result["reads"][addr_s.lower()] = {
                "size": len(data), "hex": data.hex(), "preview": preview(data),
            }
            print(f"0x{addr:08x} [{len(data):4d}] {data.hex()}")
            print(f"           preview: {preview(data)}")
    finally:
        kernel32.CloseHandle(h)

    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as fh:
            json.dump(result, fh, ensure_ascii=False, indent=1)
        print(f"已保存: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
