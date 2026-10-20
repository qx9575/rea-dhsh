"""Verify the YCbCr->RGB pipeline of Game.exe against the standard formula.

Reproduces, from the disassembly:
  0x8a3c00 table build (x87, __ftol2 with FPU truncate mode (verified in a running process)):
      T_R [i] = rne(1.402  * v + 1.0) >> 1     (sar)   [0xb0ced0], v=(int8)i
      T_Gb[i] = rne(1.0 - 0.34414 * v) >> 1            [0xb0cdc8]
      T_Gr[i] = rne(1.0 - 0.71414 * v) >> 1            [0xb0cbc8]
      T_B [i] = rne(1.772  * v + 1.0) >> 1             [0xb0ccc8]
  pixel helpers 0x8a3840 (RGB555) / 0x8a38d0 (RGB565):
      t  = (Y + 0x80) >> 1          (sar, floor; Y signed int8)
      R7 = clamp(T_R[Cr] + t, 0, 127)
      G7 = clamp(T_Gb[Cb] + T_Gr[Cr] + t, 0, 127)   (int8 wrap add, in-range)
      B7 = clamp(T_B[Cb] + t, 0, 127)
      555: (R7>>2)<<10 | (G7>>2)<<5 | (B7>>2)
      565: (R7>>2)<<11 | (G7>>1)<<5 | (B7>>2)
  MMX v2 converter 0x8a3590: same math via paddsb/psubusb saturation,
  2x2 nearest chroma upsampling (punpcklbw duplication), RGB555 masks
  (0x8a32d0 is the RGB565 variant, not dumped).

Compares against the standard full-range BT.601 float formula and reports
channel differences in final 5/6-bit units.
"""
from __future__ import annotations

import sys

import numpy as np


def rne(x: float) -> int:
    # runtime-verified: game runs with FPU truncate mode -> toward zero
    return int(x) if x >= 0 else -int(-x)


def build_tables():
    t_r = np.empty(256, dtype=np.int32)
    t_gb = np.empty(256, dtype=np.int32)
    t_gr = np.empty(256, dtype=np.int32)
    t_b = np.empty(256, dtype=np.int32)
    for i in range(256):
        v = i - 256 if i >= 128 else i          # movsx bl
        t_r[i] = rne(1.402 * v + 1.0) >> 1
        t_gb[i] = rne(1.0 - 0.34414 * v) >> 1
        t_gr[i] = rne(1.0 - 0.71414 * v) >> 1
        t_b[i] = rne(1.772 * v + 1.0) >> 1
    return t_r, t_gb, t_gr, t_b


def clamp7(x: int) -> int:
    if x < 0:
        return 0
    return 0x7F if x & ~0x7F else x


def game_pixel(y, cb, cr, tabs, mode565: bool):
    t_r, t_gb, t_gr, t_b = tabs
    t = (y + 0x80) >> 1
    r = clamp7(t_r[cr & 0xFF] + t)
    g = clamp7(((t_gb[cb & 0xFF] + t_gr[cr & 0xFF] + 256) % 256
                if False else t_gb[cb & 0xFF] + t_gr[cr & 0xFF]) + t)
    b = clamp7(t_b[cb & 0xFF] + t)
    if mode565:
        return (r >> 2, g >> 1, b >> 2)
    return (r >> 2, g >> 2, b >> 2)


def std_pixel(y, cb, cr):
    r = y + 1.402 * cr + 128.0
    g = y - 0.34414 * cb - 0.71414 * cr + 128.0
    b = y + 1.772 * cb + 128.0
    return (min(255, max(0, int(np.rint(r)))),
            min(255, max(0, int(np.rint(g)))),
            min(255, max(0, int(np.rint(b)))))


def main() -> int:
    tabs = build_tables()
    print("table ranges:",
          f"T_R [{tabs[0].min()},{tabs[0].max()}]",
          f"T_Gb [{tabs[1].min()},{tabs[1].max()}]",
          f"T_Gr [{tabs[2].min()},{tabs[2].max()}]",
          f"T_B [{tabs[3].min()},{tabs[3].max()}]")
    print("sample entries (v=-128,-1,0,1,127):")
    for v in (-128, -1, 0, 1, 127):
        i = v & 0xFF
        print(f"  v={v:4d}: T_R={tabs[0][i]:3d} T_Gb={tabs[1][i]:3d} "
              f"T_Gr={tabs[2][i]:3d} T_B={tabs[3][i]:3d}")

    for mode565 in (False, True):
        maxd = [0, 0, 0]
        hist = [np.zeros(5, dtype=np.int64) for _ in range(3)]
        for y in range(-128, 128, 7):
            for cb in range(-128, 128, 9):
                for cr in range(-128, 128, 9):
                    gr, gg, gb = game_pixel(y, cb, cr, tabs, mode565)
                    sr, sg, sb = std_pixel(y, cb, cr)
                    # standard 8-bit -> 5/6-bit reference (truncating, as the
                    # game effectively truncates everywhere)
                    ref = (sr >> 3, sg >> (2 if mode565 else 3), sb >> 3)
                    for ch, (a, b) in enumerate(zip((gr, gg, gb), ref)):
                        d = abs(a - b)
                        maxd[ch] = max(maxd[ch], d)
                        hist[ch][min(d, 4)] += 1
        tag = "565" if mode565 else "555"
        print(f"RGB{tag}: max channel diff vs std>>shift = {maxd}")
        for ch, name in enumerate("RGB"):
            print(f"   {name}: |d|=0,1,2,3,>=4 -> {hist[ch].tolist()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
