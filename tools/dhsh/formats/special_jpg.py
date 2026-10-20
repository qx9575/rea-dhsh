"""Decoder for the private JPEG variant (FF D8 FF C8).

Derived from Game.exe:
    0x8a3dd0  header / Huffman table / MCU descriptor parser
    0x823240  per-MCU driver (version 2 = 16x16 4:2:0, version 3 = 8x8)
    0x8a1fe0  Huffman + dequantisation for one 8x8 block
    0x8a8780  MMX AAN IDCT (signed 8-bit output)
    0x8a3b30 / 0x8a3590  YCbCr -> RGB conversion
    0x8a3c00  conversion-table / IDCT selection

Container:
    0x00  FF D8 FF C8
    0x04  u32 header_size (1000)
    0x08  u32 version (2 or 3)
    0x0c  u32 width
    0x10  u32 height
    0x1c  component records (6 bytes each): q_idx, ac_idx, dc_idx, ...
    0x2c  three 64 x u16 quant tables (128 bytes each)
    0x22c eight Huffman tables: [u32 count][count * {u16 code,u8 len,u8 sym}]
    ...   mcu_count * 6-byte descriptors
    ...   u32 data_size, then the entropy stream

Descriptor (6 bytes):
    f0 = dword & 0x3ff          byte delta of the entropy pointer (cumulative)
    f1 = byte4 & 0x1f           bit offset within the 32-bit big-endian word
    f2 = signed16(word4) >> 5   DC predictor slot 0
    f3 = signed16(dword >> 10)  DC predictor slot 1  (arithmetic, low 16 bits)
    f4 = signed32(dword) >> 21  DC predictor slot 2

Blocks within one MCU are decoded sequentially from the descriptor pointer;
the three DC slots are shared mutable predictors.  Slot use:
    version 3: Y=slot0, Cb=slot1, Cr=slot2
    version 2: Y0=slot0, Y1=slot1, Y2=slot2, Y3=slot0, Cb=slot1, Cr=slot2

The float IDCT here is a faithful-scale approximation (raw sum / 64); the
game uses integer MMX constants, so pixel values may differ by +-1.
"""
from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field

import numpy as np

MAGIC = b"\xff\xd8\xff\xc8"
ZIGZAG = [
    0, 63, 55, 62, 61, 54, 47, 39, 46, 53, 60, 59, 52, 45, 38, 31,
    23, 30, 37, 44, 51, 58, 57, 50, 43, 36, 29, 22, 15, 7, 14, 21,
    28, 35, 42, 49, 56, 48, 41, 34, 27, 20, 13, 6, 5, 12, 19, 26,
    33, 40, 32, 25, 18, 11, 4, 3, 10, 17, 24, 16, 9, 2, 1, 8,
]
IDCT_SCALE = 1.0 / 64.0


class SpecialJPGError(ValueError):
    pass


@dataclass
class HuffmanTable:
    entries: list[tuple[int, int, int]]
    lut: list[int] = field(default_factory=list)
    max_len: int = 0

    def build(self) -> None:
        self.max_len = max((ln for _c, ln, _s in self.entries), default=0)
        if not self.max_len:
            self.lut = []
            return
        size = 1 << self.max_len
        lut = [0] * size
        for code, length, symbol in self.entries:
            shift = self.max_len - length
            if code << shift >= size:
                raise SpecialJPGError(f"huffman code {code}/{length} overflow")
            val = (symbol << 5) | length
            for i in range(1 << shift):
                idx = (code << shift) | i
                if lut[idx] and lut[idx] != val:
                    raise SpecialJPGError("huffman table is not prefix-free")
                lut[idx] = val
        self.lut = lut

    def decode(self, br: "BitReader") -> int:
        if self.lut:
            v = self.lut[br.peek(self.max_len)]
            if not v:
                raise SpecialJPGError("invalid huffman code")
            br.skip(v & 31)
            return v >> 5
        for code, length, symbol in self.entries:
            if br.peek(length) == code:
                br.skip(length)
                return symbol
        raise SpecialJPGError("invalid huffman code")


class BitReader:
    __slots__ = ("data", "pos", "bit", "_base", "_cache")

    def __init__(self, data: bytes, pos: int, bit: int = 0):
        self.data = data
        # descriptor bit offsets are relative to a 32-bit big-endian word
        self.pos = pos + (bit >> 3)
        self.bit = bit & 7
        self._base = self.pos
        chunk = data[self.pos:self.pos + 8]
        if len(chunk) < 8:
            chunk = chunk + b"\0" * (8 - len(chunk))
        self._cache = int.from_bytes(chunk, "big")

    def _refresh(self) -> None:
        self._base = self.pos
        chunk = self.data[self.pos:self.pos + 8]
        if len(chunk) < 8:
            chunk = chunk + b"\0" * (8 - len(chunk))
        self._cache = int.from_bytes(chunk, "big")

    def peek(self, n: int) -> int:
        if n == 0:
            return 0
        off = (self.pos - self._base) * 8 + self.bit
        if off + n > 64:
            self._refresh()
            off = self.bit
        return (self._cache >> (64 - off - n)) & ((1 << n) - 1)

    def skip(self, n: int) -> None:
        self.bit += n
        self.pos += self.bit >> 3
        self.bit &= 7

    def read(self, n: int) -> int:
        if n == 0:
            return 0
        off = (self.pos - self._base) * 8 + self.bit
        if off + n > 64:
            self._refresh()
            off = self.bit
        v = (self._cache >> (64 - off - n)) & ((1 << n) - 1)
        self.bit += n
        self.pos += self.bit >> 3
        self.bit &= 7
        return v


def extend(v: int, n: int) -> int:
    if n == 0:
        return 0
    return v - (1 << n) + 1 if v < (1 << (n - 1)) else v


@dataclass
class SpecialJPEG:
    data: bytes
    version: int
    width: int
    height: int
    comps: list[tuple[int, int, int]]
    quants: list[list[int]]
    tables: list[HuffmanTable]
    records: np.ndarray            # (mcu, 5) int64: ptr, bitoff, f2, f3, f4
    data_start: int
    data_size: int
    mcu_cols: int
    mcu_rows: int


def parse(data: bytes) -> SpecialJPEG:
    if len(data) < 0x22C or data[:4] != MAGIC:
        raise SpecialJPGError("not a private FF D8 FF C8 JPEG")
    version = struct.unpack_from("<I", data, 8)[0]
    if version not in (2, 3):
        raise SpecialJPGError(f"unsupported version {version}")
    width = struct.unpack_from("<I", data, 0xC)[0]
    height = struct.unpack_from("<I", data, 0x10)[0]
    if not width or not height:
        raise SpecialJPGError("zero image dimension")
    comps = []
    for off in (0x1C, 0x22, 0x28):
        q, ac, dc = data[off], data[off + 1], data[off + 2]
        if q > 2 or ac > 7 or dc > 7:
            raise SpecialJPGError(f"bad component record at {off:#x}")
        comps.append((q, ac, dc))
    quants = []
    for qi in range(3):
        base = 0x2C + qi * 128
        quants.append(np.frombuffer(data, dtype="<u2", count=64,
                                    offset=base).astype(np.int32).tolist())
    tables = []
    pos = 0x22C
    for _ in range(8):
        if pos + 4 > len(data):
            raise SpecialJPGError("huffman table count overruns")
        cnt = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        if cnt > 512 or pos + cnt * 4 > len(data):
            raise SpecialJPGError("huffman table overruns")
        entries = [struct.unpack_from("<HBB", data, pos + 4 * i)
                   for i in range(cnt)]
        pos += cnt * 4
        tab = HuffmanTable(entries)
        tab.build()
        tables.append(tab)

    mcu_w = 16 if version == 2 else 8
    if width % mcu_w or height % mcu_w:
        raise SpecialJPGError(f"dimensions {width}x{height} not MCU aligned")
    cols = width // mcu_w
    rows = height // mcu_w
    mcu_count = cols * rows
    desc_start = pos
    if desc_start + mcu_count * 6 + 4 > len(data):
        raise SpecialJPGError("descriptor table overruns")
    data_start = desc_start + mcu_count * 6 + 4
    data_size = struct.unpack_from("<I", data, desc_start + mcu_count * 6)[0]
    if data_start + data_size != len(data):
        raise SpecialJPGError(
            f"data size mismatch: {data_start}+{data_size} != {len(data)}")

    recs = np.empty((mcu_count, 5), dtype=np.int64)
    cum = 0
    for i in range(mcu_count):
        o = desc_start + i * 6
        dw = struct.unpack_from("<I", data, o)[0]
        w4 = struct.unpack_from("<H", data, o + 4)[0]
        cum = (cum + (dw & 0x3FF)) & 0xFFFFFFFF
        f2 = w4 >> 5
        if f2 >= 0x400:
            f2 -= 0x800
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
        recs[i] = (data_start + cum, data[o + 4] & 0x1F, f2, f3, f4)
    return SpecialJPEG(data, version, width, height, comps, quants, tables,
                       recs, data_start, data_size, cols, rows)


_EXT_THR = [0] + [1 << (n - 1) for n in range(1, 17)]
_EXT_OFF = [0] + [(1 << n) - 1 for n in range(1, 17)]


def decode_block(jpg: SpecialJPEG, state, comp: int, seed: int, out):
    """Decode one 8x8 block into *out* (preallocated int32[64]).

    Returns (dc, (byte_pos, bit_offset)).
    """
    q, ac_i, dc_i = jpg.comps[comp]
    quant = jpg.quants[q]
    dc_tab = jpg.tables[dc_i]
    ac_tab = jpg.tables[ac_i]
    dc_lut, dc_max = dc_tab.lut, dc_tab.max_len
    ac_lut, ac_max = ac_tab.lut, ac_tab.max_len
    if not dc_lut or not ac_lut:
        raise SpecialJPGError("empty Huffman table")
    br = BitReader(jpg.data, state[0], state[1])
    peek = br.peek
    skip = br.skip
    read = br.read
    v = dc_lut[peek(dc_max)]
    if not v:
        raise SpecialJPGError("invalid DC Huffman code")
    skip(v & 31)
    cat = v >> 5
    if cat > 16:
        raise SpecialJPGError(f"bad DC category {cat}")
    diff = read(cat)
    if diff < _EXT_THR[cat]:
        diff -= _EXT_OFF[cat]
    dc = seed + diff
    out.fill(0)
    # The game stores only AX after `imul word [edi]`: 16-bit wrap for DC.
    dc_coeff = (dc * quant[0]) & 0xFFFF
    out[0] = dc_coeff - 0x10000 if dc_coeff >= 0x8000 else dc_coeff
    k = 1
    while k < 64:
        v = ac_lut[peek(ac_max)]
        if not v:
            raise SpecialJPGError("invalid AC Huffman code")
        skip(v & 31)
        sym = v >> 5
        run, size = sym >> 4, sym & 0xF
        if size == 0:
            if run == 15:
                k += 16
                continue
            break
        k += run
        if k >= 64:
            raise SpecialJPGError("AC run overflow")
        val = read(size)
        if val < _EXT_THR[size]:
            val -= _EXT_OFF[size]
        prod = val * quant[k]
        # imul result is saturated to int16 before the store (0x8a215d).
        if prod > 32767:
            prod = 32767
        elif prod < -32768:
            prod = -32768
        out[ZIGZAG[64 - k]] = prod
        k += 1
    return dc, (br.pos, br.bit)


_COS = np.array([[math.cos((2 * x + 1) * u * math.pi / 16)
                  for u in range(8)] for x in range(8)])
_C = np.array([1 / math.sqrt(2)] + [1.0] * 7)
_T = _COS * _C[None, :]           # T[x, u]
_TT = _T.T.copy()


def idct_blocks(coeff: np.ndarray) -> np.ndarray:
    """coeff (N,64) -> signed int8 samples (N,64), row-major y*8+x."""
    f = coeff.reshape(-1, 8, 8).astype(np.float64)
    out = (_T @ f @ _TT) * IDCT_SCALE
    out = np.floor(out)                      # game: psraw 4, no bias
    out = out.transpose(0, 2, 1)             # (x,y) -> (y,x) row-major
    out = np.clip(out, -128, 127).astype(np.int32)
    return out.reshape(-1, 64).astype(np.int8)


def _build_color_tables() -> np.ndarray:
    """Game tables (0x8a3d46): trunc(K*v + bias) >> 1, v = signed int8."""
    tabs = np.empty((4, 256), dtype=np.int32)
    for i in range(256):
        v = i - 256 if i >= 128 else i
        tabs[0, i] = int(1.402 * v + 1.0) >> 1
        tabs[1, i] = int(1.0 - 0.34414 * v) >> 1
        tabs[2, i] = int(1.0 - 0.71414 * v) >> 1
        tabs[3, i] = int(1.772 * v + 1.0) >> 1
    return tabs


_COLOR_TABLES = _build_color_tables()



def _prepare_numba(jpg: SpecialJPEG) -> dict:
    """Build the int32 arrays consumed by special_jpg_numba.decode_row."""
    max_bits = max((t.max_len for t in jpg.tables), default=0) or 1
    lmax = 1 << max_bits
    dc_luts = np.zeros((3, lmax), dtype=np.int32)
    ac_luts = np.zeros((3, lmax), dtype=np.int32)
    dcmax = np.zeros(3, dtype=np.int32)
    acmax = np.zeros(3, dtype=np.int32)
    for c, (_q, aci, dci) in enumerate(jpg.comps):
        t = jpg.tables[dci]
        if t.lut:
            dc_luts[c, :len(t.lut)] = t.lut
        dcmax[c] = t.max_len
        t = jpg.tables[aci]
        if t.lut:
            ac_luts[c, :len(t.lut)] = t.lut
        acmax[c] = t.max_len
    if jpg.version == 2:
        oc = np.array([0, 0, 0, 0, 1, 2], dtype=np.int32)
        oslot = np.array([0, 0, 0, 0, 1, 2], dtype=np.int32)
        n_order = 6
    else:
        oc = np.array([0, 1, 2], dtype=np.int32)
        oslot = np.array([0, 1, 2], dtype=np.int32)
        n_order = 3
    return {
        "data": np.frombuffer(jpg.data, dtype=np.uint8),
        "recs": jpg.records,
        "order_comp": oc,
        "order_slot": oslot,
        "n_order": n_order,
        "dc_luts": dc_luts,
        "dcmax": dcmax,
        "ac_luts": ac_luts,
        "acmax": acmax,
        "quants": np.array(jpg.quants, dtype=np.int32),
        "comps": np.array(jpg.comps, dtype=np.int32),
        "zigzag": np.array(ZIGZAG, dtype=np.int32),
    }


def decode(jpg: SpecialJPEG, fast: bool = True) -> np.ndarray:
    """Return an (h, w, 3) uint8 RGB image.

    fast=True uses the optional numba MCU kernel (falls back to pure Python
    when numba is unavailable).
    """
    cols, rows = jpg.mcu_cols, jpg.mcu_rows
    mcu_w = 16 if jpg.version == 2 else 8
    w, h = jpg.width, jpg.height
    img = np.empty((h, w, 3), dtype=np.uint8)
    order = ([(0, 0), (0, 0), (0, 0), (0, 0), (1, 1), (2, 2)]
             if jpg.version == 2 else [(0, 0), (1, 1), (2, 2)])
    v2 = jpg.version == 2
    nb = None
    fast_ctx = None
    if fast:
        try:
            from . import special_jpg_numba as nb  # noqa: F401
            fast_ctx = _prepare_numba(jpg)
        except ImportError:
            nb = None
            fast_ctx = None
    recs = None if fast_ctx is not None else jpg.records.tolist()
    tr, tgb, tgr, tb = _COLOR_TABLES
    for my in range(rows):
        y_coeff = np.empty((cols * (4 if v2 else 1), 64), dtype=np.int32)
        cb_coeff = np.empty((cols, 64), dtype=np.int32)
        cr_coeff = np.empty((cols, 64), dtype=np.int32)
        base = my * cols
        if fast_ctx is not None:
            rc = nb.decode_row(
                fast_ctx["data"], fast_ctx["recs"], base, cols, v2,
                fast_ctx["order_comp"], fast_ctx["order_slot"],
                fast_ctx["n_order"], fast_ctx["dc_luts"], fast_ctx["dcmax"],
                fast_ctx["ac_luts"], fast_ctx["acmax"], fast_ctx["quants"],
                fast_ctx["comps"], fast_ctx["zigzag"],
                y_coeff, cb_coeff, cr_coeff)
            if rc != 0:
                raise SpecialJPGError(f"numba decode error in row {my}")
        else:
            for mx in range(cols):
                ptr, bitoff, f2, f3, f4 = recs[base + mx]
                seeds = [f2, f3, f4]
                state = (ptr, bitoff)
                ybi = 0
                for comp_i, slot in order:
                    if comp_i == 0:
                        if v2:
                            out = y_coeff[mx * 4 + ybi]
                            ybi += 1
                        else:
                            out = y_coeff[mx]
                    elif comp_i == 1:
                        out = cb_coeff[mx]
                    else:
                        out = cr_coeff[mx]
                    dc, state = decode_block(jpg, state, comp_i,
                                             seeds[slot], out)
                    seeds[slot] = dc
        y = idct_blocks(y_coeff)
        cb = idct_blocks(cb_coeff)
        cr = idct_blocks(cr_coeff)
        if v2:
            y = y.reshape(cols, 2, 2, 8, 8).transpose(0, 1, 3, 2, 4)
            y = y.reshape(cols, 16, 16)          # mx, y, x
            row_y = y.transpose(1, 0, 2).reshape(16, cols * 16)
            cb = np.repeat(np.repeat(cb.reshape(cols, 8, 8), 2, axis=1),
                           2, axis=2).transpose(1, 0, 2).reshape(16, cols * 16)
            cr = np.repeat(np.repeat(cr.reshape(cols, 8, 8), 2, axis=1),
                           2, axis=2).transpose(1, 0, 2).reshape(16, cols * 16)
        else:
            row_y = y.reshape(cols, 8, 8).transpose(1, 0, 2).reshape(8, cols * 8)
            cb = cb.reshape(cols, 8, 8).transpose(1, 0, 2).reshape(8, cols * 8)
            cr = cr.reshape(cols, 8, 8).transpose(1, 0, 2).reshape(8, cols * 8)
        # Game pipeline: half-scale 7-bit YCbCr, lookup tables, RGB565.
        yf = row_y.astype(np.int32)
        cbf = cb.astype(np.int32)
        crf = cr.astype(np.int32)
        t = (yf + 128) >> 1
        r7 = np.clip(tr[crf & 0xFF] + t, 0, 127)
        g7 = np.clip(tgb[cbf & 0xFF] + tgr[crf & 0xFF] + t, 0, 127)
        b7 = np.clip(tb[cbf & 0xFF] + t, 0, 127)
        r5, g6, b5 = r7 >> 2, g7 >> 1, b7 >> 2
        rgb = np.empty((mcu_w, w, 3), dtype=np.uint8)
        rgb[..., 0] = (r5 << 3) | (r5 >> 2)
        rgb[..., 1] = (g6 << 2) | (g6 >> 4)
        rgb[..., 2] = (b5 << 3) | (b5 >> 2)
        img[my * mcu_w:(my + 1) * mcu_w] = rgb
    return img


def decode_file(path: str) -> np.ndarray:
    with open(path, "rb") as fh:
        return decode(parse(fh.read()))
