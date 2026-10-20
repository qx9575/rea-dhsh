"""Numba fast path for the private JPEG MCU entropy decode.

Only the scalar Huffman/dequantisation loop is JIT-compiled; IDCT and colour
conversion stay vectorised in special_jpg.decode.  The bit-exact behaviour
matches the pure-Python path (same integer wrap/saturation and zigzag index).
"""
from __future__ import annotations

import numpy as np
from numba import njit


@njit(cache=True, inline="always")
def _load64(data, pos):
    n = data.shape[0]
    v = 0
    for i in range(8):
        idx = pos + i
        b = 0
        if idx < n:
            b = data[idx]
        v = (v << 8) | b
    return v


@njit(cache=True)
def _decode_block(data, q, dc_lut, dmax, ac_lut, amax, seed, zigzag,
                  pos, bit, base, cache, out):
    """Returns dc, pos, bit, base, cache, ok."""
    off = (pos - base) * 8 + bit
    if off + dmax > 64:
        base = pos
        cache = _load64(data, pos)
        off = bit
    v = dc_lut[(cache >> (64 - off - dmax)) & ((1 << dmax) - 1)]
    if v == 0:
        return np.int64(0), pos, bit, base, cache, False
    bit += v & 31
    pos += bit >> 3
    bit &= 7
    cat = v >> 5

    off = (pos - base) * 8 + bit
    if off + cat > 64:
        base = pos
        cache = _load64(data, pos)
        off = bit
    diff = (cache >> (64 - off - cat)) & ((1 << cat) - 1)
    bit += cat
    pos += bit >> 3
    bit &= 7
    if cat and diff < (1 << (cat - 1)):
        diff -= (1 << cat) - 1
    dc = seed + diff

    out[:] = 0
    dc_coeff = (dc * q[0]) & 0xFFFF
    if dc_coeff >= 0x8000:
        dc_coeff -= 0x10000
    out[0] = dc_coeff

    k = 1
    while k < 64:
        off = (pos - base) * 8 + bit
        if off + amax > 64:
            base = pos
            cache = _load64(data, pos)
            off = bit
        v = ac_lut[(cache >> (64 - off - amax)) & ((1 << amax) - 1)]
        if v == 0:
            return np.int64(0), pos, bit, base, cache, False
        bit += v & 31
        pos += bit >> 3
        bit &= 7
        sym = v >> 5
        run = sym >> 4
        size = sym & 15
        if size == 0:
            if run == 15:
                k += 16
                continue
            break
        k += run
        if k >= 64:
            return np.int64(0), pos, bit, base, cache, False
        off = (pos - base) * 8 + bit
        if off + size > 64:
            base = pos
            cache = _load64(data, pos)
            off = bit
        val = (cache >> (64 - off - size)) & ((1 << size) - 1)
        bit += size
        pos += bit >> 3
        bit &= 7
        if val < (1 << (size - 1)):
            val -= (1 << size) - 1
        prod = val * q[k]
        if prod > 32767:
            prod = 32767
        elif prod < -32768:
            prod = -32768
        out[zigzag[64 - k]] = prod
        k += 1
    return dc, pos, bit, base, cache, True


@njit(cache=True)
def decode_row(data, recs, row_start, cols, v2,
               order_comp, order_slot, n_order,
               dc_luts, dcmax, ac_luts, acmax,
               quants, comps, zigzag,
               y_out, cb_out, cr_out):
    """Decode one MCU row.  Returns 0 on success, -1 on a decode error."""
    for mx in range(cols):
        i = row_start + mx
        ptr = recs[i, 0]
        bitoff = recs[i, 1]
        s0 = recs[i, 2]
        s1 = recs[i, 3]
        s2 = recs[i, 4]
        pos = ptr + (bitoff >> 3)
        bit = bitoff & 7
        base = pos
        cache = _load64(data, pos)
        ybi = 0
        for oi in range(n_order):
            comp = order_comp[oi]
            slot = order_slot[oi]
            if comp == 0:
                if v2:
                    out = y_out[mx * 4 + ybi]
                    ybi += 1
                else:
                    out = y_out[mx]
            elif comp == 1:
                out = cb_out[mx]
            else:
                out = cr_out[mx]
            qi = comps[comp, 0]
            if slot == 0:
                seed = s0
            elif slot == 1:
                seed = s1
            else:
                seed = s2
            dc, pos, bit, base, cache, ok = _decode_block(
                data, quants[qi], dc_luts[comp], dcmax[comp],
                ac_luts[comp], acmax[comp], seed, zigzag,
                pos, bit, base, cache, out)
            if not ok:
                return -1
            if slot == 0:
                s0 = dc
            elif slot == 1:
                s1 = dc
            else:
                s2 = dc
    return 0
