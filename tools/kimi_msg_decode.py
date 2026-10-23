#!/usr/bin/env python3
"""kimi_msg_decode.py — 离线解密抓包流中的 s2c 消息（带自动重同步）。

用法：
  python tools/kimi_msg_decode.py 流.bin [--out 报告.txt] [--min-cmd 0x2000] [--max-cmd 0x46ff]

流格式：[u16le 长度(不含头)][加密载荷]，逐帧自包含解密（kimi_msg_crypto）。
遇到 TCP 重组洞/分隔零字节时逐字节向前扫描，用“解密后 cmd 落在有效区间”
做重同步校验。
"""
from __future__ import annotations

import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from kimi_msg_crypto import decrypt_payload, load_key  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("stream", help="kimi_msg_sniff 输出的重组流 .bin")
    ap.add_argument("--out", help="输出报告路径（默认打印）")
    ap.add_argument("--min-cmd", default="0x2000")
    ap.add_argument("--max-cmd", default="0x46ff")
    ap.add_argument("--max-gap-scan", type=int, default=4096)
    args = ap.parse_args()

    lo = int(args.min_cmd, 16)
    hi = int(args.max_cmd, 16)
    load_key()
    s = open(args.stream, "rb").read()

    def try_frame(off):
        if off + 4 > len(s):
            return None
        ln = struct.unpack_from("<H", s, off)[0]
        if ln < 2 or ln > 8000 or off + 2 + ln > len(s):
            return None
        dec = decrypt_payload(s[off + 2: off + 2 + ln])
        cmd = struct.unpack_from("<H", dec, 0)[0]
        return (ln, cmd, dec) if lo <= cmd <= hi else None

    lines = []
    cmds = collections.Counter()
    gaps = []
    pos = 0
    n = 0
    while pos + 4 <= len(s):
        r = try_frame(pos)
        if r is None:
            nxt = None
            for off in range(pos + 1, min(pos + args.max_gap_scan, len(s) - 4)):
                r2 = try_frame(off)
                if r2:
                    nxt = off
                    break
            if nxt is None:
                break
            gaps.append((pos, nxt - pos))
            pos = nxt
            r = try_frame(pos)
        ln, cmd, dec = r
        cmds[cmd] += 1
        n += 1
        payload = dec[2:]
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in payload)
        lines.append(f"@{pos:07x} len={ln + 2:<5d} cmd={cmd:#06x}  "
                     f"{payload[:40].hex()}  {asc[:40]}")
        pos += 2 + ln

    lines.append("")
    lines.append(f"共 {n} 帧，重同步间隙 {len(gaps)} 处（多为抓包丢包洞）")
    lines.append("命令分布: " + str({hex(k): v for k, v in cmds.most_common(30)}))
    text = "\n".join(lines)
    if args.out:
        open(args.out, "w", encoding="utf-8").write(text)
        print(f"已写 {args.out}（{n} 帧，间隙 {len(gaps)}）")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
