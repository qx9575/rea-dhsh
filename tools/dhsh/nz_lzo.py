"""LZO1X decompressor used by NZ type 0x0B.

The algorithm follows the classic LZO1X token layout: a leading literal run,
then a stream of literal runs and back-references.  The Hero2D client calls
the same decoder at Game.exe VA 0x8a6c90.
"""
from __future__ import annotations


class LZOError(ValueError):
    pass


def lzo1x_decompress(src: bytes, out_len: int) -> bytes:
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
