"""LZO1X decompressor used by NZ type 0x0B.

The algorithm follows the classic LZO1X token layout: a leading literal run,
then a stream of literal runs and back-references.  The Hero2D client calls
the same decoder at Game.exe VA 0x8a6c90.

A cached numba JIT core is used when numpy/numba are installed (the Python
loop is the fallback).  Both paths are byte-identical on the validated sample
set; the JIT path is roughly two orders of magnitude faster.
"""
from __future__ import annotations

try:  # optional acceleration
    import numpy as _np
    from numba import njit as _njit
except ImportError:  # pragma: no cover - environment dependent
    _np = None
    _njit = None


class LZOError(ValueError):
    pass


if _njit is not None:

    @_njit(cache=True, boundscheck=True)
    def _lzo1x_core(src, out_len, out):
        ip = 0
        op = 0
        n = src.shape[0]
        t = src[0]
        state = 0  # 0=MAIN, 1=FIRST, 2=MATCH, 3=DONE

        if t > 17:
            ip = 1
            t -= 17
            if t < 4:
                if ip + t > n or op + t > out_len:
                    return -1
                for _ in range(t):
                    out[op] = src[ip]
                    ip += 1
                    op += 1
                if ip >= n:
                    return -1
                t = src[ip]
                ip += 1
                state = 2
            else:
                if ip + t > n or op + t > out_len:
                    return -1
                for _ in range(t):
                    out[op] = src[ip]
                    ip += 1
                    op += 1
                state = 1
        else:
            ip = 0
            state = 0

        while True:
            if state == 0:  # MAIN
                if ip >= n:
                    return -1
                t = src[ip]
                ip += 1
                if t >= 16:
                    state = 2
                    continue
                if t == 0:
                    while True:
                        if ip >= n:
                            return -1
                        if src[ip] != 0:
                            break
                        t += 255
                        ip += 1
                    t += 15 + src[ip]
                    ip += 1
                count = t + 3
                if ip + count > n or op + count > out_len:
                    return -1
                for _ in range(count):
                    out[op] = src[ip]
                    ip += 1
                    op += 1
                state = 1
                continue

            if state == 1:  # FIRST
                if ip >= n:
                    return -1
                t = src[ip]
                ip += 1
                if t >= 16:
                    state = 2
                    continue
                if ip >= n:
                    return -1
                b = src[ip]
                ip += 1
                m_pos = op - (1 + 0x0800) - (t >> 2) - (b << 2)
                count = 3
                if m_pos < 0 or op + count > out_len:
                    return -2
                for _ in range(count):
                    out[op] = out[m_pos]
                    op += 1
                    m_pos += 1
                state = 3
                continue

            if state == 2:  # MATCH
                if t >= 64:
                    if ip >= n:
                        return -1
                    b = src[ip]
                    ip += 1
                    m_pos = op - 1 - ((t >> 2) & 7) - (b << 3)
                    count = (t >> 5) + 1
                elif t >= 32:
                    t &= 31
                    if t == 0:
                        while True:
                            if ip >= n:
                                return -1
                            if src[ip] != 0:
                                break
                            t += 255
                            ip += 1
                        t += 31 + src[ip]
                        ip += 1
                    if ip + 1 >= n:
                        return -1
                    v = src[ip] | (src[ip + 1] << 8)
                    ip += 2
                    m_pos = op - 1 - (v >> 2)
                    count = t + 2
                elif t >= 16:
                    m_pos = op - ((t & 8) << 11)
                    t &= 7
                    if t == 0:
                        while True:
                            if ip >= n:
                                return -1
                            if src[ip] != 0:
                                break
                            t += 255
                            ip += 1
                        t += 7 + src[ip]
                        ip += 1
                    if ip + 1 >= n:
                        return -1
                    v = src[ip] | (src[ip + 1] << 8)
                    ip += 2
                    m_pos -= v >> 2
                    if m_pos == op:
                        if ip != n:
                            return -3
                        return op
                    m_pos -= 0x4000
                    count = t + 2
                else:
                    if ip >= n:
                        return -1
                    b = src[ip]
                    ip += 1
                    m_pos = op - 1 - (t >> 2) - (b << 2)
                    count = t + 2
                if m_pos < 0 or op + count > out_len:
                    return -2
                for _ in range(count):
                    out[op] = out[m_pos]
                    op += 1
                    m_pos += 1
                state = 3
                continue

            if state == 3:  # DONE
                if ip < 2:
                    return -1
                t = src[ip - 2] & 3
                if t:
                    if ip + t > n or op + t > out_len:
                        return -1
                    for _ in range(t):
                        out[op] = src[ip]
                        ip += 1
                        op += 1
                    if ip >= n:
                        return -1
                    t = src[ip]
                    ip += 1
                    state = 2
                else:
                    state = 0
                continue

            return -4


def _decode_numba(src: bytes, out_len: int) -> bytes:
    arr = _np.frombuffer(src, dtype=_np.uint8)
    out = _np.empty(out_len, dtype=_np.uint8)
    op = _lzo1x_core(arr, out_len, out)
    if op < 0:
        raise LZOError(f"LZO decode failed (status {op})")
    return out[:op].tobytes()


def lzo1x_decompress(src: bytes, out_len: int) -> bytes:
    if not src:
        raise LZOError("empty LZO stream")
    if out_len == 0:
        return b""
    if _njit is not None:
        return _decode_numba(src, out_len)

    # Pure Python fallback.
    ip = 0
    op = 0
    n = len(src)
    out = bytearray(out_len)

    def literals(count: int) -> None:
        nonlocal ip, op
        if count < 0 or ip + count > n or op + count > out_len:
            raise LZOError(f"literal out of bounds ip={ip} op={op} count={count}")
        out[op : op + count] = src[ip : ip + count]
        ip += count
        op += count

    def match(m_pos: int, count: int) -> None:
        nonlocal op
        if m_pos < 0 or op + count > out_len:
            raise LZOError(f"match out of bounds m_pos={m_pos} op={op} count={count}")
        for _ in range(count):
            out[op] = out[m_pos]
            op += 1
            m_pos += 1

    t = src[0]
    if t > 17:
        ip = 1
        t -= 17
        if t < 4:
            literals(t)
            t = src[ip]
            ip += 1
            state = "MATCH"
        else:
            literals(t)
            state = "FIRST"
    else:
        # The first byte is not consumed: the main loop reads it as a token.
        ip = 0
        state = "MAIN"

    while True:
        if state == "MAIN":
            t = src[ip]
            ip += 1
            if t >= 16:
                state = "MATCH"
                continue
            if t == 0:
                while src[ip] == 0:
                    t += 255
                    ip += 1
                t += 15 + src[ip]
                ip += 1
            literals(t + 3)
            state = "FIRST"
            continue

        if state == "FIRST":
            t = src[ip]
            ip += 1
            if t >= 16:
                state = "MATCH"
                continue
            b = src[ip]
            ip += 1
            match(op - (1 + 0x0800) - (t >> 2) - (b << 2), 3)
            state = "DONE"
            continue

        if state == "MATCH":
            if t >= 64:
                b = src[ip]
                ip += 1
                m_pos = op - 1 - ((t >> 2) & 7) - (b << 3)
                count = (t >> 5) + 1
            elif t >= 32:
                t &= 31
                if t == 0:
                    while src[ip] == 0:
                        t += 255
                        ip += 1
                    t += 31 + src[ip]
                    ip += 1
                v = src[ip] | (src[ip + 1] << 8)
                ip += 2
                m_pos = op - 1 - (v >> 2)
                count = t + 2
            elif t >= 16:
                m_pos = op - ((t & 8) << 11)
                t &= 7
                if t == 0:
                    while src[ip] == 0:
                        t += 255
                        ip += 1
                    t += 7 + src[ip]
                    ip += 1
                v = src[ip] | (src[ip + 1] << 8)
                ip += 2
                m_pos -= v >> 2
                if m_pos == op:
                    if ip != n:
                        raise LZOError(f"trailing input ip={ip} n={n}")
                    return bytes(out[:op])
                m_pos -= 0x4000
                count = t + 2
            else:
                b = src[ip]
                ip += 1
                m_pos = op - 1 - (t >> 2) - (b << 2)
                count = t + 2
            match(m_pos, count)
            state = "DONE"
            continue

        if state == "DONE":
            t = src[ip - 2] & 3
            if t:
                literals(t)
                t = src[ip]
                ip += 1
                state = "MATCH"
            else:
                state = "MAIN"
            continue

        raise LZOError(f"unknown state {state}")
