#!/usr/bin/env python3
"""kimi_msg_hook.py — Frida 挂钩 Game.exe 消息层，抓取双向明文协议。

挂钩点（旧构建静态定位 + 新构建字节签名校准，抗版本更新）：
  c2s 明文：FUN_007a0f60( conn, msg )  ← 出站消息加密/序号盖章之前的最后一站
            msg = [u16 长度(不含头)][u16 命令(0x40xx)][payload]
  s2c 明文：FUN_007a2120( msg, len, flag )  ← 入站消息总分发器
            msg = [u16 长度(不含头)][u16 命令(0x30xx)][payload]

校准原理：当前运行版本（10-21 更新）相对 Ghidra 分析的旧构建代码段
整体平移 +0x120，数据段不变。脚本用旧构建字节签名在磁盘文件中搜索
得到实际地址，再校验进程内存一致性；搜索失败回退到已知地址。

用法：
  python tools/kimi_msg_hook.py                  # 挂钩并打印（Ctrl+C 停）
  python tools/kimi_msg_hook.py --out .rea/msg.jsonl   # 同时写 JSONL
  python tools/kimi_msg_hook.py --pid 6208       # 指定进程（默认按名字找 Game.exe）
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kimi_msg_crypto import decrypt_frame  # noqa: E402

# 旧构建字节签名（SEH 立即数通配）→ 旧 VA 与 10-21 构建的已知新 VA
SIGS = {
    # FUN_007a0f60 出站预处理（明文）
    "c2s": ("64 a1 00 00 00 00 6a ff 68 ?? ?? ?? ?? 50 8b 44 24 10 "
            "64 89 25 00 00 00 00 83 ec 1c 57 33 ff 3b c7",
            0x7A1080),
    # FUN_007a2120 入站总分发（明文）
    "s2c": ("55 8b ec 6a ff 68 ?? ?? ?? ?? 64 a1 00 00 00 00 50 "
            "64 89 25 00 00 00 00 81 ec 20 06 00 00 a1 c0 72 ab 00",
            0x7A2240),
    # FUN_007a0730 出站 send-all：暂存区即线上明文（文本已序列化进去）
    "c2s_wire": ("55 8b ec 6a ff 68 70 a1 a3 00 68 ?? ?? ?? ?? "
                 "64 a1 00 00 00 00 50 64 89 25 00 00 00 00 83 ec 18 53 56 57",
                 0x7A0850),
    # FUN_00934e30 会话加密入口：src 缓冲区是加密前的线上明文（文本已序列化）
    "c2s_plain": ("8b 44 24 04 8b 08 53 8b 5c 24 0c 55 8b 6c 24 14 "
                  "8b d3 56 8b 70 04 c1 ea 03 85 d2 57 8b 7c 24 20",
                  0x934F70),
}

JS = r"""
Interceptor.attach(ptr($C2S), {
  onEnter: function (args) {
    var p = args[1];
    if (p.isNull()) return;
    var len = p.readU16() + 2;
    if (len < 4 || len > 0x8000) return;
    // 多读 96B：变长文本可能在声明长度之后
    var cap = Math.min(len + 96, 8192);
    send({ dir: 'c2s', cmd: p.add(2).readU16(), len: len, cap: cap },
         p.readByteArray(cap));
  }
});
// c2s_wire: send-all FUN_007a0730(新 0x7a0850) 入口——暂存区里是最终线上明文
// (buf, len, 0)，buf = [u16 len][u16 cmd][...]
Interceptor.attach(ptr($C2SW), {
  onEnter: function (args) {
    var p = args[0];
    var len = args[1].toInt32();
    if (p.isNull() || len < 4 || len > 0x10000) return;
    send({ dir: 'c2s_wire', cmd: p.add(2).readU16(), len: len },
         p.readByteArray(len));
  }
});
// c2s_plain: FUN_00934e30(ctx, len, src, dst) 入口——src-2 起是完整明文帧
Interceptor.attach(ptr($C2SP), {
  onEnter: function (args) {
    var len = args[1].toInt32();
    if (len < 2 || len > 0x10000) return;
    var p = args[2].sub(2);   // 帧起始（含 u16 长度头）
    send({ dir: 'c2s_plain', cmd: p.add(2).readU16(), len: len + 2 },
         p.readByteArray(len + 2));
  }
});
Interceptor.attach(ptr($S2C), {
  onEnter: function (args) {
    var p = args[0];
    var len = args[1].toInt32();
    var flag = args[2].toInt32();
    if (p.isNull() || len < 4 || len > 0x100000) return;
    // flag=0 的帧客户端不解密（登录期明文帧），原样记录由离线端判断
    send({ dir: 's2c', cmd: p.add(2).readU16(), len: len, flag: flag },
         p.readByteArray(len));
  }
});
"""


def sig_search(data: bytes, pattern: str) -> list[int]:
    """通配签名搜索，返回文件偏移列表。"""
    regex = b"".join(b"." if p == "??" else re.escape(bytes([int(p, 16)]))
                     for p in pattern.split())
    return [m.start() for m in re.finditer(b"(?=" + regex + b")", data)]


def calibrate(exe_path: str) -> dict:
    """在磁盘文件中按签名定位挂钩点 VA；失败回退已知地址。"""
    data = open(exe_path, "rb").read()
    out = {}
    for name, (sig, fallback) in SIGS.items():
        hits = sig_search(data, sig)
        va = None
        if len(hits) == 1:
            va = hits[0] + 0x400000
            src = f"签名命中 文件偏移 {hits[0]:#x}"
        else:
            va = fallback
            src = f"签名命中 {len(hits)} 处，回退已知地址"
        out[name] = va
        print(f"  {name}: VA {va:#x}（{src}）")
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, default=None)
    ap.add_argument("--wait", action="store_true",
                    help="等待 Game.exe 启动后再挂钩（登录抓包用）")
    ap.add_argument("--out", help="JSONL 输出路径")
    ap.add_argument("--exe", default=r"D:\software\dhsh\shdata\Game.exe",
                    help="Game.exe 磁盘路径（用于签名校准）")
    args = ap.parse_args()

    import frida

    pid = args.pid
    if pid is None:
        while True:
            for proc in frida.get_local_device().enumerate_processes():
                if proc.name.lower() == "game.exe":
                    pid = proc.pid
                    break
            if pid is not None:
                break
            if not args.wait:
                print("Game.exe 未运行")
                return 1
            print("等待 Game.exe 启动 …", flush=True)
            time.sleep(0.5)
    print(f"附加 Game.exe pid={pid}")

    print("签名校准挂钩点:")
    addrs = calibrate(args.exe)

    session = frida.attach(pid)

    js = (JS.replace("$C2SW", hex(addrs["c2s_wire"]))   # 先长后短，防前缀冲突
            .replace("$C2SP", hex(addrs["c2s_plain"]))
            .replace("$C2S", hex(addrs["c2s"]))
            .replace("$S2C", hex(addrs["s2c"])))
    script = session.create_script(js)

    fh = open(args.out, "a", encoding="utf-8") if args.out else None
    counts = {"c2s": 0, "s2c": 0, "c2s_wire": 0, "c2s_plain": 0}
    t0 = time.perf_counter()

    def on_message(msg, data):
        if msg.get("type") != "send":
            print("脚本消息:", msg)
            return
        pl = msg["payload"]
        d = pl["dir"]
        counts[d] += 1
        raw = bytes(data)
        if d == "s2c":
            if pl.get("flag", 1):
                # 分发器入口拿到的是密文帧，本地解密为明文
                dec = decrypt_frame(raw)
                cmd = int.from_bytes(dec[2:4], "little")
                payload = dec[4:]
            else:
                # flag=0：客户端未解密（登录期明文帧）
                dec = raw
                cmd = int.from_bytes(raw[2:4], "little")
                payload = raw[4:]
        else:
            cmd = pl["cmd"]
            payload = raw[4:]
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in payload)
        print(f"[{time.perf_counter()-t0:8.2f}s] {d} cmd={cmd:#06x} "
              f"len={pl['len']:<5d} {payload[:28].hex()}  {asc[:28]}")
        if fh:
            fh.write(json.dumps({"t": time.perf_counter() - t0, "dir": d,
                                 "cmd": cmd, "len": pl["len"],
                                 "flag": pl.get("flag"),
                                 "hex": (dec if d == "s2c" else raw).hex()})
                     + "\n")
            fh.flush()

    script.on("message", on_message)
    script.load()
    print(f"挂钩完成：c2s @{addrs['c2s']:#x}  c2s_plain @{addrs['c2s_plain']:#x}  "
          f"c2s_wire @{addrs['c2s_wire']:#x}  s2c @{addrs['s2c']:#x}")
    print("等待消息 …（Ctrl+C 停止；在游戏里走动/聊天可产生流量）")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    print(f"\n共捕获 c2s {counts['c2s']} / c2s_plain {counts['c2s_plain']} / "
          f"c2s_wire {counts['c2s_wire']} / s2c {counts['s2c']} 条")
    if fh:
        fh.close()
    session.detach()
    return 0


if __name__ == "__main__":
    sys.exit(main())
