#!/usr/bin/env python3
"""kimi_apci_zpeek.py — 动态验证 APCI 1B blob = 纸娃娃部件渲染层叠键。

静态结论（docs/formats/apci.md §3）：
  FUN_00827970 (0x827970) 把 APCI item 的 1B blob 展开成 u16 数组挂到
  角色对象 obj+0x488+4×group；装配函数 FUN_008281e0 逐帧把各部件的
  (group, key) 对送 qsort (0x939ce0)，比较器 0x8271e0 按 key 升序，
  排序结果就是部件绘制顺序（key 小 = 先画 = 垫底）。

本脚本挂钩两点（当前构建 7a5e，挂前校验字节）：
  1. FUN_00827970 出口：转储某部件刚填充的 key 数组（按方向/动作变化）。
  2. qsort 入口/出口（按比较器 0x8271e0 过滤）：排序前后的 (group,key)。

用法：
  python tools/kimi_apci_zpeek.py                 # 挂钩并打印
  python tools/kimi_apci_zpeek.py --out .rea/apci_zpeek.jsonl
然后在游戏里让角色原地转向（走满 8 个方向），观察 mask(9) 的 key 变化。
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

EXE = r"D:\software\dhsh\shdata\Game.exe"

# 挂钩点 VA 与期望字节（7a5e 构建，pefile 从磁盘提取）
POINTS = {
    "zfill": (0x827970, "83 ec 14 81 7c 24 1c ff ff 00 00"),   # FUN_00827970
    "qsort": (0x939CE0, "8b 44 24 08 81 ec 00 01 00 00"),      # qsort
    "cmp":   (0x8271E0, "8b 44 24 04 8b 4c 24 08 0f b7 40 02"),
}

GROUPS = {0: "head", 1: "body", 2: "weapon", 3: "pet", 4: "magic",
          7: "cloth", 8: "effect", 9: "mask", 10: "Lcuff", 11: "shoe",
          12: "Rcuff", 13: "horse", 14: "harmorbody", 15: "harmorhead"}

JS = r"""
var CMP = ptr($CMP);

Interceptor.attach(ptr($ZFILL), {
  onEnter: function (args) {
    this.group = args[0].toInt32() & 0xffff;
    this.model = args[1].toInt32() & 0xffff;
    this.action = args[2].toInt32() & 0xffff;
    this.dir = args[3].toInt32() & 0xffff;
    this.obj = args[4];
  },
  onLeave: function (retval) {
    if (retval.toInt32() !== 1 || this.obj.isNull()) return;
    try {
      var count = this.obj.readU16();
      if (count < 1 || count > 64) return;
      var arr = this.obj.add(0x488 + this.group * 4).readPointer();
      if (arr.isNull()) return;
      var n = Math.min(count, 24), keys = [];
      for (var i = 0; i < n; i++) keys.push(arr.add(i * 2).readU16());
      send({ kind: 'zfill', group: this.group, model: this.model,
             action: this.action, dir: this.dir, count: count,
             obj: this.obj.toString(), keys: keys });
    } catch (e) { }
  }
});

Interceptor.attach(ptr($QSORT), {
  onEnter: function (args) {
    if (!args[3].equals(CMP)) return;
    var num = args[1].toInt32();
    if (num < 1 || num > 18) return;
    this.base = args[0];
    this.num = num;
    var pre = [];
    for (var i = 0; i < num; i++)
      pre.push([this.base.add(i * 4).readU16(), this.base.add(i * 4 + 2).readU16()]);
    this.pre = pre;
  },
  onLeave: function () {
    if (this.base === undefined) return;
    try {
      var post = [];
      for (var i = 0; i < this.num; i++)
        post.push([this.base.add(i * 4).readU16(), this.base.add(i * 4 + 2).readU16()]);
      send({ kind: 'sort', num: this.num, pre: this.pre, post: post });
    } catch (e) { }
  }
});

// 挂前字节校验（ArrayBuffer 不能直接放 payload，转 hex 字符串）
function hexOf(p, n) {
  var b = new Uint8Array(p.readByteArray(n)), s = [];
  for (var i = 0; i < b.length; i++) s.push(('0' + b[i].toString(16)).slice(-2));
  return s.join(' ');
}
send({ kind: 'verify',
       zfill: hexOf(ptr($ZFILL), 10),
       qsort: hexOf(ptr($QSORT), 10),
       cmp: hexOf(ptr($CMP), 10) });
"""


def expected_bytes() -> dict:
    import pefile
    pe = pefile.PE(EXE, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    data = open(EXE, "rb").read()

    def va2off(va):
        rva = va - base
        for s in pe.sections:
            if s.VirtualAddress <= rva < s.VirtualAddress + s.Misc_VirtualSize:
                return s.PointerToRawData + (rva - s.VirtualAddress)
        raise ValueError(hex(va))

    return {name: data[va2off(va):va2off(va) + 10]
            for name, (va, _) in POINTS.items()}


def fmt_pairs(pairs):
    return " ".join(f"{GROUPS.get(g, g)}:{k}" for g, k in pairs)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, default=None)
    ap.add_argument("--out", help="JSONL 输出路径")
    args = ap.parse_args()

    import frida

    pid = args.pid
    if pid is None:
        for proc in frida.get_local_device().enumerate_processes():
            if proc.name.lower() == "game.exe":
                pid = proc.pid
                break
    if pid is None:
        print("Game.exe 未运行")
        return 1
    print(f"附加 Game.exe pid={pid}")

    exp = expected_bytes()
    session = frida.attach(pid)
    js = (JS.replace("$ZFILL", hex(POINTS["zfill"][0]))
            .replace("$QSORT", hex(POINTS["qsort"][0]))
            .replace("$CMP", hex(POINTS["cmp"][0])))
    script = session.create_script(js)

    fh = open(args.out, "a", encoding="utf-8") if args.out else None
    t0 = time.perf_counter()
    stats = {"zfill": 0, "sort": 0}

    def on_message(msg, data):
        if msg.get("type") != "send":
            print("脚本消息:", msg)
            return
        pl = msg["payload"]
        t = time.perf_counter() - t0
        if pl["kind"] == "verify":
            ok = True
            for name in ("zfill", "qsort", "cmp"):
                got = pl[name]
                want = exp[name].hex(" ")
                match = got == want
                ok &= match
                print(f"  校验 {name}: {'OK' if match else 'MISMATCH'}")
                if not match:
                    print(f"    内存 {got}\n    磁盘 {want}")
            print("字节校验" + ("全部通过，挂钩已生效" if ok else "失败——进程版本与磁盘不符，勿信后续输出"))
            return
        stats[pl["kind"]] = stats.get(pl["kind"], 0) + 1
        if pl["kind"] == "zfill":
            gname = GROUPS.get(pl["group"], str(pl["group"]))
            print(f"[{t:7.2f}s] zfill {gname:>6}(g{pl['group']}) "
                  f"model={pl['model']} action={pl['action']} dir={pl['dir']} "
                  f"count={pl['count']} keys={pl['keys']}")
        else:
            print(f"[{t:7.2f}s] sort n={pl['num']} pre[{fmt_pairs(pl['pre'])}] "
                  f"-> post[{fmt_pairs(pl['post'])}]")
        if fh:
            fh.write(json.dumps({"t": t, **pl}, ensure_ascii=False) + "\n")
            fh.flush()

    script.on("message", on_message)
    script.load()
    print("挂钩完成。请在游戏里让角色原地转向（走满 8 个方向），Ctrl+C 停止。")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    print(f"\n共捕获 zfill {stats.get('zfill', 0)} / sort {stats.get('sort', 0)} 条")
    if fh:
        fh.close()
    session.detach()
    return 0


if __name__ == "__main__":
    sys.exit(main())
