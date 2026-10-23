#!/usr/bin/env python3
"""kimi_msg_sniff.py — Game.exe 通信消息抓取与 TCP 流重组。

原理（纯被动抓包，不碰游戏进程）：

  1. 定位 Game.exe 的已建立 TCP 连接（netstat），得到服务器 IP:port；
  2. 用 Windows 自带 pktmon 按该流过滤抓包（需要管理员）；
  3. pktmon etl2pcap 转成 pcapng；
  4. 纯 Python 解析 pcapng + TCP 重组（无第三方依赖），按方向输出
     c2s / s2c 字节流（.bin）与 hex/ASCII 报告（.txt）；
  5. 试探常见长度前缀帧格式（u16le/u16be/u32le），辅助消息边界识别。

用法：
  python tools/kimi_msg_sniff.py --seconds 60 --out .rea/sniff1
      # 抓 60 秒（会自动定位游戏连接）；Ctrl+C 可提前结束
  python tools/kimi_msg_sniff.py --parse .rea/sniff1.pcapng
      # 只解析已有抓包文件
  python tools/kimi_msg_sniff.py --seconds 30 --remote 101.43.206.92:8000
      # 手动指定服务器地址（跳过进程探测）

输出：<out>.etl / <out>.pcapng / <out>_c2s.bin / <out>_s2c.bin / <out>_report.txt
"""
from __future__ import annotations

import argparse
import os
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# ------------------------------------------------------------ 进程与连接探测

def find_game_flows(proc_name: str = "Game"):
    """返回 [(pid, local_ip, local_port, remote_ip, remote_port), ...]"""
    ps = (
        "$ErrorActionPreference='SilentlyContinue';"
        f"$p=Get-Process {proc_name};"
        "if($p){Get-NetTCPConnection -OwningProcess $p.Id | "
        "Where-Object State -eq 'Established' | "
        "ForEach-Object {\"$($p.Id) $($_.LocalAddress) $($_.LocalPort) "
        "$($_.RemoteAddress) $($_.RemotePort)\"}}"
    )
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                         capture_output=True).stdout.decode("gbk", "replace")
    flows = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 5:
            flows.append((int(parts[0]), parts[1], int(parts[2]),
                          parts[3], int(parts[4])))
    return flows


# ------------------------------------------------------------ pktmon 封装

def pktmon(*args, check=True):
    cmd = ["pktmon", *args]
    r = subprocess.run(cmd, capture_output=True)
    stdout = r.stdout.decode("gbk", "replace")
    stderr = r.stderr.decode("gbk", "replace")
    if check and r.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} 失败({r.returncode}): "
                           f"{stdout}{stderr}\n（pktmon 需要管理员权限）")
    return r


def capture(etl_path: str, remote_ip: str | None, remote_port: int | None,
            seconds: float):
    pktmon("filter", "remove", check=False)
    if remote_ip:
        spec = f"{remote_ip}:{remote_port}" if remote_port else remote_ip
        pktmon("filter", "add", "GameFlow", "-i", remote_ip)
        print(f"过滤器：远端 {spec}（IP 匹配）")
    else:
        print("过滤器：无（抓全量，流量大，慎用）")
    pktmon("start", "--capture", "--pkt-size", "0", "--file-size", "256",
           "-f", etl_path)
    print(f"抓包中 … {seconds}s（Ctrl+C 提前结束）")
    try:
        time.sleep(seconds)
    except KeyboardInterrupt:
        print("手动停止")
    finally:
        pktmon("stop")
        pktmon("filter", "remove", check=False)
    pcapng = os.path.splitext(etl_path)[0] + ".pcapng"
    pktmon("etl2pcap", etl_path, "--out", pcapng)
    print(f"已转换 {pcapng}")
    return pcapng


# ------------------------------------------------------------ pcapng 解析 + TCP 重组

def parse_pcapng(path: str):
    """yield (ts_us, ip_src, ip_dst, sport, dport, seq, ack, flags, payload)"""
    with open(path, "rb") as f:
        data = f.read()
    pos = 0
    endian = "<"
    linktype = 1
    if_tsresol = {}
    if_idx = -1
    while pos + 12 <= len(data):
        btype, blen = struct.unpack_from(endian + "II", data, pos)
        body = data[pos + 8: pos + blen - 4]
        if btype == 0x0A0D0D0A:  # Section Header
            bom = body[:4]
            endian = "<" if bom == b"\x4d\x3c\x2b\x1a" else ">"
            if_tsresol.clear()
            if_idx = -1
        elif btype == 1:  # Interface Description
            linktype = struct.unpack_from(endian + "H", body, 0)[0]
            if_idx += 1
            tsr = 6  # 默认微秒
            # options 里找 if_tsresol (code 9)
            op = 8
            while op + 4 <= len(body):
                code, ln = struct.unpack_from(endian + "HH", body, op)
                if code == 0:
                    break
                if code == 9 and ln >= 1:
                    v = body[op + 4]
                    tsr = (v & 0x7F) if not (v & 0x80) else -(v & 0x7F)
                op += 4 + ((ln + 3) & ~3)
            if_tsresol[if_idx] = tsr
        elif btype == 6:  # Enhanced Packet
            iid, tsh, tsl, caplen = struct.unpack_from(endian + "IIII", body, 0)
            ts = ((tsh << 32) | tsl)
            resol = if_tsresol.get(iid, 6)
            ts_us = ts * (10 ** (6 - resol)) if resol >= 0 else ts
            pkt = body[20:20 + caplen]
            yield from _parse_frame(pkt, linktype, ts_us)
        pos += blen if blen > 0 else len(data) - pos


def _parse_frame(pkt: bytes, linktype: int, ts_us: float):
    if linktype == 1:  # Ethernet
        if len(pkt) < 14:
            return
        eth_type = struct.unpack_from(">H", pkt, 12)[0]
        off = 14
        if eth_type == 0x8100 and len(pkt) >= 18:  # VLAN
            eth_type = struct.unpack_from(">H", pkt, 16)[0]
            off = 18
        if eth_type != 0x0800:
            return
    elif linktype == 0:  # NULL/loopback
        off = 4
    else:  # 其他链路类型暂不支持
        return
    if len(pkt) < off + 20:
        return
    ihl = (pkt[off] & 0x0F) * 4
    proto = pkt[off + 9]
    if proto != 6:  # 只处理 TCP
        return
    ip_src = socket.inet_ntoa(pkt[off + 12:off + 16])
    ip_dst = socket.inet_ntoa(pkt[off + 16:off + 20])
    toff = off + ihl
    if len(pkt) < toff + 20:
        return
    sport, dport, seq, ack = struct.unpack_from(">HHII", pkt, toff)
    doff = (pkt[toff + 12] >> 4) * 4
    flags = pkt[toff + 13]
    payload = pkt[toff + doff:]
    yield (ts_us, ip_src, ip_dst, sport, dport, seq, ack, flags, payload)


def reassemble(segments):
    """[(seq, payload)] → (bytes, 空洞数)。简单按 seq 排序去重。"""
    segs = sorted((s, p) for s, p in segments if p)
    if not segs:
        return b"", 0
    base = segs[0][0]
    buf = bytearray()
    end = base
    gaps = 0
    for seq, payload in segs:
        if seq < end:  # 重传/重叠
            overlap = end - seq
            if overlap >= len(payload):
                continue
            payload = payload[overlap:]
            seq = end
        elif seq > end:
            gaps += 1
            buf.extend(b"\x00" * (seq - end))
            end = seq
        buf.extend(payload)
        end += len(payload)
    return bytes(buf), gaps


def hexdump(data: bytes, base: int = 0, limit: int = 4096) -> str:
    lines = []
    for off in range(0, min(len(data), limit), 16):
        chunk = data[off:off + 16]
        hexs = " ".join(f"{b:02x}" for b in chunk)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{base + off:08x}  {hexs:<48s}  {asc}")
    if len(data) > limit:
        lines.append(f"… 共 {len(data):,} 字节，仅显示前 {limit}")
    return "\n".join(lines)


def guess_framing(stream: bytes):
    """试探长度前缀帧格式，返回按一致性排序的候选。"""
    cands = []
    for name, hsz, fmt, incl in [
        ("u16le 含头", 2, "<H", True), ("u16le 不含头", 2, "<H", False),
        ("u16be 含头", 2, ">H", True), ("u16be 不含头", 2, ">H", False),
        ("u32le 含头", 4, "<I", True), ("u32le 不含头", 4, "<I", False),
    ]:
        pos = 0
        frames = 0
        ok = True
        while pos + hsz <= len(stream) and frames < 100000:
            ln = struct.unpack_from(fmt, stream, pos)[0]
            total = ln if incl else ln + hsz
            if total <= 0 or total > 10 * 1024 * 1024:
                ok = False
                break
            pos += total
            frames += 1
            if frames > 2 and pos >= len(stream):
                break
        if ok and frames >= 2:
            coverage = pos / len(stream) if stream else 0
            cands.append((coverage, frames, name))
    cands.sort(reverse=True)
    return cands[:3]


# ------------------------------------------------------------ 主流程

def analyze(pcapng_path: str, out_prefix: str, focus_ip: str | None):
    flows = {}
    for (ts, s, d, sp, dp, seq, ack, flags, payload) in parse_pcapng(pcapng_path):
        if focus_ip and focus_ip not in (s, d):
            continue
        key = (s, sp, d, dp)
        flows.setdefault(key, []).append((ts, seq, payload, flags))
    if not flows:
        print("没有匹配的 TCP 包")
        return

    report = [f"抓包文件: {pcapng_path}", f"流数量: {len(flows)}", ""]
    n = 0
    for (s, sp, d, dp), pkts in sorted(flows.items(),
                                       key=lambda kv: -len(kv[1])):
        # 只保留两端往返的流（把两个方向合并到一个连接）
        n += 1
        c2s_segs = [(seq, p) for ts, seq, p, fl in pkts]
        stream, gaps = reassemble(c2s_segs)
        report.append(f"=== 方向 {s}:{sp} → {d}:{dp} ===")
        report.append(f"包数 {len(pkts)}，重组 {len(stream):,} 字节，空洞 {gaps}")
        if stream:
            report.append("帧格式试探: " + (", ".join(
                f"{nm}(覆盖{cov:.0%},{fr}帧)" for cov, fr, nm in guess_framing(stream))
                or "无一致格式"))
            report.append(hexdump(stream, limit=2048))
            suffix = "c2s" if sp > dp or (focus_ip and d == focus_ip) else "s2c"
            out_bin = f"{out_prefix}_{suffix}_{s.replace('.', '_')}_{sp}.bin"
            with open(out_bin, "wb") as f:
                f.write(stream)
            report.append(f"完整流已存: {out_bin}")
        report.append("")

    report_path = f"{out_prefix}_report.txt"
    with open(report_path, "w", encoding="utf-8") as f:
        f.write("\n".join(report))
    print(f"报告: {report_path}")
    # 控制台打印摘要
    print("\n".join(report[:60]))


def main() -> int:
    ap = argparse.ArgumentParser(description="Game.exe 通信消息抓取/解析")
    ap.add_argument("--seconds", type=float, default=30, help="抓包时长")
    ap.add_argument("--remote", help="手动指定服务器 ip:port（跳过进程探测）")
    ap.add_argument("--out", default=os.path.join(ROOT, ".rea", "sniff1"),
                    help="输出前缀")
    ap.add_argument("--parse", metavar="PCAPNG", help="只解析已有抓包文件")
    args = ap.parse_args()

    if args.parse:
        ip = args.remote.split(":")[0] if args.remote else None
        analyze(args.parse, os.path.splitext(args.parse)[0], ip)
        return 0

    remote_ip = remote_port = None
    if args.remote:
        remote_ip, _, p = args.remote.partition(":")
        remote_port = int(p) if p else None
    else:
        flows = find_game_flows()
        if not flows:
            print("未发现 Game.exe 的已建立 TCP 连接（游戏是否已登录？）")
            return 1
        for pid, lip, lp, rip, rp in flows:
            print(f"发现连接: pid={pid} {lip}:{lp} → {rip}:{rp}")
        _, _, _, remote_ip, remote_port = flows[0]

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    pcapng = capture(args.out + ".etl", remote_ip, remote_port, args.seconds)
    analyze(pcapng, args.out, remote_ip)
    return 0


if __name__ == "__main__":
    sys.exit(main())
