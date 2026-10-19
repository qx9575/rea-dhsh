"""LZW (16-bit codes) decompressor used by NZ type 0x0A.

The Hero2D client calls this decoder at Game.exe VA ``0x8a6f60``.  Input is a
stream of little-endian uint16 codes:

    0x0000-0x00FF   literal byte
    0x0100          end of stream
    0x0101-0xFFFF   dictionary code, allocated incrementally

The dictionary starts with codes 0x101 onward.  Each decoded code appends a
new entry ``prev_string + first_byte(current_string)`` (classic LZW).  The
client stores entries as a ``(parent_code, last_byte)`` trie at ``0xb30328``
and grows it until code 0xFFFF; after that the dictionary is frozen and the
remaining codes are decoded against it.

Validated against 267/267 type-0x0A samples (exact ``size1`` match).
"""
from __future__ import annotations

import struct

END_CODE = 0x100
FIRST_DYNAMIC = 0x101
MAX_CODE = 0xFFFF


class LZW10Error(ValueError):
    pass


def lzw10_decompress(src: bytes, expected_size: int | None = None) -> bytes:
    """Decode an NZ type-0x0A payload.

    ``expected_size`` is the NZ header ``size1``; when given, a length
    mismatch raises instead of returning partial output.
    """
    n = len(src) // 2
    if n == 0:
        if expected_size not in (None, 0):
            raise LZW10Error(f"empty payload for expected size {expected_size}")
        return b""

    codes = struct.unpack_from(f"<{n}H", src)
    first = codes[0]
    if first == END_CODE:
        if expected_size not in (None, 0):
            raise LZW10Error(f"empty stream for expected size {expected_size}")
        return b""
    if first > 0xFF:
        raise LZW10Error(f"first code {first:#06x} is not a literal")

    prefix = [-1] * 0x10000
    suffix = [0] * 0x10000
    first_of = [0] * 0x10000
    for c in range(256):
        suffix[c] = c
        first_of[c] = c

    out = bytearray()
    out.append(first)
    prev = first
    nxt = FIRST_DYNAMIC
    stack: list[int] = []

    for cur in codes[1:]:
        if cur == END_CODE:
            break
        if cur < nxt:
            # Walk the prefix chain, emitting bytes back-to-front.
            stack.clear()
            c = cur
            while c >= 0x100:
                stack.append(suffix[c])
                c = prefix[c]
                if c < 0:
                    raise LZW10Error(f"broken prefix chain for code {cur:#06x}")
            out.append(c)
            out.extend(reversed(stack))
            cur_first = first_of[cur]
        elif cur == nxt:
            # KwKwK: current string is prev_string + first_byte(prev_string).
            cur_first = first_of[prev]
            stack.clear()
            c = prev
            while c >= 0x100:
                stack.append(suffix[c])
                c = prefix[c]
                if c < 0:
                    raise LZW10Error(f"broken prefix chain for code {prev:#06x}")
            out.append(c)
            out.extend(reversed(stack))
            out.append(cur_first)
        else:
            raise LZW10Error(
                f"code {cur:#06x} beyond next code {nxt:#06x} at output {len(out)}"
            )

        if nxt <= MAX_CODE:
            prefix[nxt] = prev
            suffix[nxt] = cur_first
            first_of[nxt] = first_of[prev]
            nxt += 1
        prev = cur

    if expected_size is not None and len(out) != expected_size:
        raise LZW10Error(
            f"decoded {len(out)} bytes, expected {expected_size} "
            f"(consumed {len(src)} stored bytes)"
        )
    return bytes(out)
