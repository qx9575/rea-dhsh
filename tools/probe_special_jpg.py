#!/usr/bin/env python
"""Probe: decoder for the private FF D8 FF C8 JPEG variant.

Prototype for validation on samples.  Layout and code semantics are derived
from Game.exe 0x8a3dd0 (header), 0x823240/0x8a1fe0 (block), 0x8a3b30 /
0x8a3590 (colour conversion).  IDCT is a float approximation.
"""
from __future__ import annotations

import math
import struct
import sys


def u16(d, o): return struct.unpack_from('<H', d, o)[0]
def u32(d, o): return struct.unpack_from('<I', d, o)[0]


class BitReader:
    __slots__ = ('data', 'pos', 'bit')

    def __init__(self, data, byte_pos, bit_off=0):
        self.data = data
        # The descriptor bit offset is relative to a 32-bit big-endian word;
        # normalise it to a raw MSB-first byte stream position.
        self.pos = byte_pos + (bit_off >> 3)
        self.bit = bit_off & 7

    def read(self, n: int) -> int:
        if n == 0:
            return 0
        end = self.pos * 8 + self.bit + n
        need = (end + 7) // 8
        if need > len(self.data):
            raise ValueError('bitstream overrun')
        v = 0
        for _ in range(n):
            byte = self.data[self.pos]
            v = (v << 1) | ((byte >> (7 - self.bit)) & 1)
            self.bit += 1
            if self.bit == 8:
                self.bit = 0
                self.pos += 1
        return v

    def peek(self, n: int) -> int:
        p, b = self.pos, self.bit
        try:
            return self.read(n)
        finally:
            self.pos, self.bit = p, b


def huffman_decode(br: BitReader, table) -> int:
    for code, length, symbol in table:
        if br.peek(length) == code:
            br.read(length)
            return symbol
    raise ValueError('no huffman code match')


def extend(v: int, n: int) -> int:
    if n == 0:
        return 0
    if v < (1 << (n - 1)):
        return v - (1 << n) + 1
    return v


def parse(path: str):
    d = open(path, 'rb').read()
    if d[:4] != b'\xff\xd8\xff\xc8':
        raise ValueError('not private jpeg')
    ver = u32(d, 8)
    w = u32(d, 0xC)
    h = u32(d, 0x10)
    if u32(d, 4) != 1000:
        pass  # header size constant observed as 1000; not needed for offsets
    comps = []
    for off in (0x1C, 0x22, 0x28):
        q, ac, dc = d[off], d[off + 1], d[off + 2]
        comps.append((q, ac, dc))
    quants = [[u16(d, 0x2C + q * 128 + 2 * i) for i in range(64)]
              for q in range(3)]
    tables = []
    pos = 0x22C
    for _ in range(8):
        cnt = u32(d, pos)
        pos += 4
        entries = []
        if cnt:
            ptr = pos
            entries = [struct.unpack_from('<HBB', d, ptr + 4 * i)
                       for i in range(cnt)]
            pos += cnt * 4
        tables.append(entries)
    desc_start = pos
    mcu = (w // (16 if ver == 2 else 8)) * (h // (16 if ver == 2 else 8))
    data_start = desc_start + mcu * 6 + 4
    data_size = u32(d, desc_start + mcu * 6)
    if data_start + data_size != len(d):
        raise ValueError(f'size mismatch {data_start + data_size} != {len(d)}')
    mcus = []
    cum = 0
    for i in range(mcu):
        o = desc_start + i * 6
        dw = u32(d, o)
        w4 = u16(d, o + 4)
        cum = (cum + (dw & 0x3FF)) & 0xFFFFFFFF
        f2 = w4 >> 5
        if f2 >= 0x400:
            f2 -= 0x800
        # Game.exe: shl eax,11 / sar eax,21 / mov word -> signed from bit20,
        # low 16 bits kept, consumed as a 16-bit signed value.
        v = (dw << 11) & 0xFFFFFFFF
        if v & 0x80000000:
            v -= 0x100000000
        v >>= 21
        f3 = v & 0xFFFF
        if f3 >= 0x8000:
            f3 -= 0x10000
        f4 = dw >> 21
        if f4 >= 0x400:
            f4 -= 0x800
        mcus.append((data_start + cum, d[o + 4] & 0x1F, f2, f3, f4))
    return {
        'version': ver, 'width': w, 'height': h, 'comps': comps,
        'quants': quants, 'tables': tables, 'data': d,
        'data_start': data_start, 'data_size': data_size,
        'mcu_count': mcu, 'mcus': mcus,
    }


ZZ = [0, 63, 55, 62, 61, 54, 47, 39, 46, 53, 60, 59, 52, 45, 38, 31,
      23, 30, 37, 44, 51, 58, 57, 50, 43, 36, 29, 22, 15, 7, 14, 21,
      28, 35, 42, 49, 56, 48, 41, 34, 27, 20, 13, 6, 5, 12, 19, 26,
      33, 40, 32, 25, 18, 11, 4, 3, 10, 17, 24, 16, 9, 2, 1, 8]


def decode_block(info, state, comp, seed):
    ptr, bitoff = state
    q, ac_i, dc_i = info['comps'][comp]
    quant = info['quants'][q]
    ac_tab = info['tables'][ac_i]
    dc_tab = info['tables'][dc_i]
    br = BitReader(info['data'], ptr, bitoff)
    cat = huffman_decode(br, dc_tab)
    diff = extend(br.read(cat), cat)
    dc = seed + diff
    block = [0] * 64
    block[0] = dc * quant[0]
    k = 1
    while k < 64:
        sym = huffman_decode(br, ac_tab)
        if sym == 0:
            break
        run, size = sym >> 4, sym & 0xF
        if size == 0:
            if run == 15:
                k += 16
                continue
            raise ValueError('bad ac symbol')
        k += run
        if k >= 64:
            raise ValueError('ac run overflow')
        val = extend(br.read(size), size)
        block[ZZ[k]] = val * quant[k]
        k += 1
    return block, dc, (br.pos, br.bit)


COS = [[math.cos((2 * x + 1) * u * math.pi / 16) for x in range(8)]
       for u in range(8)]
C = [1 / math.sqrt(2)] + [1.0] * 7


def idct(block):
    out = [0] * 64
    for y in range(8):
        for x in range(8):
            s = 0.0
            for v in range(8):
                if block[v * 8 + 0] == 0 and False:
                    pass
                cv = C[v]
                for u in range(8):
                    s += (C[u] * cv * block[v * 8 + u]
                          * COS[u][x] * COS[v][y])
            out[y * 8 + x] = max(0, min(255, int(round(s / 4.0 + 128))))
    return out


def decode(path, out_png=None, seed_mode='seed'):
    info = parse(path)
    d = info['data']
    w, h = info['width'], info['height']
    ver = info['version']
    mcu_w = 16 if ver == 2 else 8
    cols, rows = w // mcu_w, h // mcu_w
    from PIL import Image
    img = Image.new('RGB', (w, h))
    px = img.load()
    errors = 0
    ends = []
    for my in range(rows):
        for mx in range(cols):
            i = my * cols + mx
            rec = info['mcus'][i]
            nblocks = 6 if ver == 2 else 3
            blocks = []
            seeds = [rec[2], rec[3], rec[4]]
            if seed_mode == 'zero':
                seeds = [0, 0, 0]
            # (component, seed slot) in the order executed by 0x823240;
            # the three DC words are shared between calls and updated in place.
            if ver == 2:
                order = [(0, 0), (0, 1), (0, 2), (0, 0), (1, 1), (2, 2)]
            else:
                order = [(0, 0), (1, 1), (2, 2)]
            state = (rec[0], rec[1])
            for comp_i, slot in order:
                try:
                    b, dc, state = decode_block(info, state, comp_i, seeds[slot])
                    seeds[slot] = dc
                except Exception:
                    errors += 1
                    b = [0] * 64
                blocks.append(idct(b))
            if ver == 2:
                # Y0..Y3 2x2, Cb, Cr
                for by in range(2):
                    for bx in range(2):
                        yb = blocks[by * 2 + bx]
                        for yy in range(8):
                            for xx in range(8):
                                Y = yb[yy * 8 + xx]
                                Cb = blocks[4][((yy // 2) * 8 + xx // 2)]
                                Cr = blocks[5][((yy // 2) * 8 + xx // 2)]
                                r = Y + 1.402 * (Cr - 128)
                                g = Y - 0.344136 * (Cb - 128) - 0.714136 * (Cr - 128)
                                b = Y + 1.772 * (Cb - 128)
                                px[mx * 16 + bx * 8 + xx, my * 16 + by * 8 + yy] = (
                                    max(0, min(255, int(r))), max(0, min(255, int(g))),
                                    max(0, min(255, int(b))))
            else:
                for yy in range(8):
                    for xx in range(8):
                        Y = blocks[0][yy * 8 + xx]
                        Cb = blocks[1][yy * 8 + xx]
                        Cr = blocks[2][yy * 8 + xx]
                        r = Y + 1.402 * (Cr - 128)
                        g = Y - 0.344136 * (Cb - 128) - 0.714136 * (Cr - 128)
                        b = Y + 1.772 * (Cb - 128)
                        px[mx * 8 + xx, my * 8 + yy] = (
                            max(0, min(255, int(r))), max(0, min(255, int(g))),
                            max(0, min(255, int(b))))
    if out_png:
        img.save(out_png)
    return info, errors


if __name__ == '__main__':
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    mode = sys.argv[3] if len(sys.argv) > 3 else 'seed'
    info, errs = decode(path, out, mode)
    print(f"version={info['version']} {info['width']}x{info['height']} "
          f"mcus={info['mcu_count']} data={info['data_size']} errors={errs}")
