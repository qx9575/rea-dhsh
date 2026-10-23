#!/usr/bin/env python3
"""kimi_msg_decode.py — 离线解密抓包流中的消息（s2c 带自动重同步 / c2s 需从登录帧起）。

用法：
  python tools/kimi_msg_decode.py 流.bin [--out 报告.txt] [--min-cmd 0x2000] [--max-cmd 0x46ff]
  python tools/kimi_msg_decode.py 流.bin --c2s [--out 报告.txt]

s2c 流格式：[u16le 长度(不含头)][加密载荷]，逐帧自包含解密（kimi_msg_crypto）。
遇到 TCP 重组洞/分隔零字节时逐字节向前扫描，用“解密后 cmd 落在有效区间”
做重同步校验。

c2s 流：首帧必须是以 6f00 开头的登录帧（Blowfish 静态密钥，明文含账号/密码/会话值/
MAC），据此派生 RC4 会话密钥解后续帧（密钥跨帧连续，不支持中途重同步）。
报告中登录帧的账号/密码自动脱敏。
"""
from __future__ import annotations

import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from kimi_msg_crypto import decrypt_payload, load_key, decode_c2s_stream  # noqa: E402


def mask(s: bytes) -> str:
    """凭据脱敏：只留首尾各 1 字符。"""
    t = s.decode("gbk", "replace")
    return (t[:1] + "***" + t[-1:]) if len(t) > 2 else "***"


def main_c2s(args) -> int:
    s = open(args.stream, "rb").read()
    frames = decode_c2s_stream(s)
    lines = []
    cmds = collections.Counter()
    for f in frames:
        if not f.get("ok"):
            lines.append(f"@{f['offset']:07x} !! {f.get('error', '解密失败')}")
            continue
        if f["kind"] == "login-blowfish":
            info = f
            lines.append(
                f"@{f['offset']:07x} len={f['wire_len'] + 2:<5d} cmd=0x4004  [登录帧 Blowfish]"
                f" 账号={mask(info['account'])} 密码={mask(info['password'])}"
                f" 会话={info['session']:#010x} seq={info['seq']:#010x}")
            cmds[0x4004] += 1
            continue
        cmds[f["cmd"]] += 1
        payload = f["plain"][2:]
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in payload)
        tail = "" if f["trailer_ok"] else "  [MD5尾校验失败]"
        lines.append(f"@{f['offset']:07x} len={f['wire_len'] + 2:<5d} cmd={f['cmd']:#06x}  "
                     f"{payload[:40].hex()}  {asc[:40]}{tail}")
    lines.append("")
    lines.append(f"共 {len(frames)} 帧（c2s：首帧 Blowfish 登录，其余 RC4+MD5尾）")
    lines.append("命令分布: " + str({hex(k): v for k, v in cmds.most_common(30)}))
    text = "\n".join(lines)
    if args.out:
        open(args.out, "w", encoding="utf-8").write(text)
        print(f"已写 {args.out}（{len(frames)} 帧，c2s）")
    else:
        print(text)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("stream", help="kimi_msg_sniff 输出的重组流 .bin")
    ap.add_argument("--c2s", action="store_true", help="按 c2s 方向解密（须含登录帧）")
    ap.add_argument("--out", help="输出报告路径（默认打印）")
    ap.add_argument("--min-cmd", default="0x2000")
    ap.add_argument("--max-cmd", default="0x46ff")
    ap.add_argument("--max-gap-scan", type=int, default=4096)
    args = ap.parse_args()

    if args.c2s:
        return main_c2s(args)

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
