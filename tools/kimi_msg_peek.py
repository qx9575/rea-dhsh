#!/usr/bin/env python3
"""kimi_msg_peek.py — 只读方式从运行中的 Game.exe 读取解密后的消息缓冲区。

原理：Game.exe 的接收泵 FUN_007a71f0 把 recv 到的（已解密）字节追加到全局
缓冲区 DAT_00ad1318，计数在 DAT_00ae1320，随后由 FUN_007a7090 按
[u16 长度][u16 命令号][payload] 分帧派发到 FUN_007a2120。
本脚本用 ReadProcessMemory 轮询该缓冲区，跟踪计数增长捕获新消息。

用法：
  python tools/kimi_msg_peek.py                 # 单次读当前缓冲区
  python tools/kimi_msg_peek.py --watch 60      # 轮询 60 秒，收集所有新消息
  python tools/kimi_msg_peek.py --watch 0       # 直到 Ctrl+C
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

BUF_ADDR = 0x00AD1318
CNT_ADDR = 0x00AE1320
BUF_MAX = 0xFFFE

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.ReadProcessMemory.restype = wt.BOOL


def find_pid(name="Game.exe"):
    import subprocess
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV",
                          "/NH"], capture_output=True).stdout.decode("gbk", "replace")
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split(",")]
        if len(parts) >= 2 and parts[0].lower() == name.lower():
            return int(parts[1])
    return None


class ProcReader:
    def __init__(self, pid):
        self.h = kernel32.OpenProcess(0x0010 | 0x0400, False, pid)  # VM_READ|QUERY
        if not self.h:
            raise OSError(f"OpenProcess({pid}) 失败 err={ctypes.get_last_error()}")

    def read(self, addr, size):
        buf = (ctypes.c_char * size)()
        n = ctypes.c_size_t(0)
        if not kernel32.ReadProcessMemory(self.h, addr, buf, size,
                                          ctypes.byref(n)):
            raise OSError(f"RPM {addr:#x} err={ctypes.get_last_error()}")
        return buf.raw[:n.value]

    def close(self):
        if self.h:
            kernel32.CloseHandle(self.h)
            self.h = None


def parse_frames(buf: bytes):
    """[u16 len 不含头][u16 cmd][payload] 分帧。"""
    pos = 0
    out = []
    while pos + 4 <= len(buf):
        ln = struct.unpack_from("<H", buf, pos)[0]
        if ln < 2 or pos + 2 + ln > len(buf):
            break
        cmd = struct.unpack_from("<H", buf, pos + 2)[0]
        out.append((pos, ln, cmd, buf[pos + 4:pos + 2 + ln]))
        pos += 2 + ln
    return out, pos


def show(buf: bytes, limit=24):
    frames, used = parse_frames(buf)
    print(f"缓冲区 {len(buf)} 字节，解析 {len(frames)} 帧（消耗 {used} 字节）")
    for off, ln, cmd, payload in frames[:limit]:
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in payload)
        print(f"  @{off:05x} len={ln:<4d} cmd={cmd:#06x}  {payload[:32].hex()}  {asc[:32]}")
    if len(frames) > limit:
        print(f"  … 共 {len(frames)} 帧")
    cmds = {}
    for _, _, c, _ in frames:
        cmds[c] = cmds.get(c, 0) + 1
    print("命令分布:", {hex(k): v for k, v in sorted(cmds.items())})


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--watch", type=float, default=None,
                    help="轮询秒数（0=直到 Ctrl+C）")
    ap.add_argument("--interval", type=float, default=0.02)
    args = ap.parse_args()

    pid = find_pid()
    if not pid:
        print("Game.exe 未运行")
        return 1
    print(f"Game.exe pid={pid}")
    pr = ProcReader(pid)
    try:
        cnt = struct.unpack("<I", pr.read(CNT_ADDR, 4))[0]
        print(f"计数={cnt} (0xae1320)，缓冲区上限 {BUF_MAX}")
        if args.watch is None:
            show(pr.read(BUF_ADDR, min(cnt, BUF_MAX)))
            return 0

        # 轮询模式：计数增长区间 = 新到达的明文消息
        t0 = time.perf_counter()
        seen = 0
        last = cnt
        collected = bytearray()
        while True:
            if args.watch and time.perf_counter() - t0 > args.watch:
                break
            try:
                cnt = struct.unpack("<I", pr.read(CNT_ADDR, 4))[0]
            except OSError:
                break
            if cnt > last:
                new = pr.read(BUF_ADDR + last, cnt - last)
                frames, _ = parse_frames(new)
                for _, ln, cmd, payload in frames:
                    seen += 1
                    asc = "".join(chr(b) if 32 <= b < 127 else "." for b in payload)
                    print(f"[{time.perf_counter()-t0:7.2f}s] cmd={cmd:#06x} "
                          f"len={ln:<5d} {payload[:24].hex()}  {asc[:24]}")
                collected += new
                last = cnt
            elif cnt < last:
                last = cnt  # 缓冲区被消费压缩
            time.sleep(args.interval)
        print(f"\n共捕获 {seen} 条新消息，{len(collected)} 字节")
    finally:
        pr.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
