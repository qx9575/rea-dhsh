"""LZW (16-bit codes) decompressor used by NZ type 0x0A.

The Hero2D client calls this decoder at Game.exe VA ``0x8a6f60``.  Input is a
stream of little-endian uint16 codes:

    0x0000-0x00FF   literal byte
    0x0100          end of stream
    0x0101-0xFFFF   dictionary code, allocated incrementally

The dictionary starts with codes 0x101 onward.  Each decoded code appends a
new entry ``prev_string + first_byte(current_string)`` (classic LZW).  The
client stores entries as a ``(parent_code, last_byte)`` trie and grows it
until code 0xFFFF; after that the dictionary is frozen and the remaining codes
are decoded against it.

Validated against all 59,222 type-0x0A leaves (exact ``size1`` match).

The prefix/suffix/first/length tables are module-level scratch arrays reused
across calls.  Dynamic entries are always written before they are read, so no
clearing is required.  This is safe for sequential use (the normal case) but
**not thread-safe**: decode concurrently with processes, not threads.

If ``numba`` and ``numpy`` are installed, a cached JIT core is used instead of
the Python loop (tens of times faster in local benchmarks); otherwise the pure
Python path remains available.
"""
from __future__ import annotations

import struct

try:  # optional acceleration
    import numpy as _np
    from numba import njit as _njit
except ImportError:  # pragma: no cover - environment dependent
    _np = None
    _njit = None

END_CODE = 0x100
FIRST_DYNAMIC = 0x101
MAX_CODE = 0xFFFF

# Scratch trie shared by calls.  Literals are constant; dynamic entries are
# overwritten as the dictionary grows within each stream.
_PREFIX = [0] * 0x10000
_SUFFIX = [0] * 0x10000
_FIRST = [0] * 0x10000
_LEN = [0] * 0x10000
for _c in range(256):
    _SUFFIX[_c] = _c
    _FIRST[_c] = _c
    _LEN[_c] = 1

if _njit is not None:
    _NP_PREFIX = _np.zeros(0x10000, dtype=_np.int32)
    _NP_SUFFIX = _np.arange(0x10000, dtype=_np.int32)
    _NP_FIRST = _np.arange(0x10000, dtype=_np.int32)
    _NP_LEN = _np.zeros(0x10000, dtype=_np.int32)
    _NP_LEN[:256] = 1
    _NP_U16 = _np.dtype("<u2")

    @_njit(cache=True)
    def _lzw10_core_numba(codes, expected_size, prefix, suffix, first_of, lens, out):
        """Return output length, or -1 (bad code) / -2 (overflow)."""
        first = codes[0]
        out[0] = first
        pos = 1
        prev = first
        nxt = 0x101
        n = codes.shape[0]
        for i in range(1, n):
            cur = codes[i]
            if cur == 0x100:
                break
            if cur < nxt:
                ln = lens[cur]
                end = pos + ln
                if end > expected_size:
                    return -2
                p = end - 1
                c = cur
                while c >= 0x100:
                    out[p] = suffix[c]
                    p -= 1
                    c = prefix[c]
                out[p] = c
                pos = end
                cur_first = first_of[cur]
            elif cur == nxt:
                # KwKwK: prev_string + first_byte(prev_string).
                ln = lens[prev] + 1
                end = pos + ln
                if end > expected_size:
                    return -2
                p = end - 1
                out[p] = first_of[prev]
                p -= 1
                c = prev
                while c >= 0x100:
                    out[p] = suffix[c]
                    p -= 1
                    c = prefix[c]
                out[p] = c
                pos = end
                cur_first = first_of[prev]
            else:
                return -1

            if nxt <= 0xFFFF:
                prefix[nxt] = prev
                suffix[nxt] = cur_first
                first_of[nxt] = first_of[prev]
                lens[nxt] = lens[prev] + 1
                nxt += 1
            prev = cur
        return pos


class LZW10Error(ValueError):
    pass


def _unpack_codes(src: bytes) -> tuple[int, ...]:
    n = len(src) // 2
    return struct.unpack_from(f"<{n}H", src)


def _check_size(actual: int, expected: int | None, stored: int) -> None:
    if expected is not None and actual != expected:
        raise LZW10Error(
            f"decoded {actual} bytes, expected {expected} "
            f"(consumed {stored} stored bytes)"
        )


def _decode_sized(codes: tuple[int, ...], expected_size: int) -> bytes:
    prefix = _PREFIX
    suffix = _SUFFIX
    first_of = _FIRST
    lens = _LEN

    out = bytearray(expected_size)
    first = codes[0]
    out[0] = first
    pos = 1
    prev = first
    nxt = FIRST_DYNAMIC

    for cur in codes[1:]:
        if cur == END_CODE:
            break
        if cur < nxt:
            ln = lens[cur]
            end = pos + ln
            if end > expected_size:
                raise LZW10Error(
                    f"output overflow: {end} > expected size {expected_size}"
                )
            p = end - 1
            c = cur
            while c >= 0x100:
                out[p] = suffix[c]
                p -= 1
                c = prefix[c]
            out[p] = c
            pos = end
            cur_first = first_of[cur]
        elif cur == nxt:
            # KwKwK: current string is prev_string + first_byte(prev_string).
            ln = lens[prev] + 1
            end = pos + ln
            if end > expected_size:
                raise LZW10Error(
                    f"output overflow: {end} > expected size {expected_size}"
                )
            p = end - 1
            out[p] = first_of[prev]
            p -= 1
            c = prev
            while c >= 0x100:
                out[p] = suffix[c]
                p -= 1
                c = prefix[c]
            out[p] = c
            pos = end
            cur_first = first_of[prev]
        else:
            raise LZW10Error(
                f"code {cur:#06x} beyond next code {nxt:#06x} at output {pos}"
            )

        if nxt <= MAX_CODE:
            prefix[nxt] = prev
            suffix[nxt] = cur_first
            first_of[nxt] = first_of[prev]
            lens[nxt] = lens[prev] + 1
            nxt += 1
        prev = cur

    _check_size(pos, expected_size, len(codes) * 2)
    return bytes(out[:pos])


def _decode_unsized(codes: tuple[int, ...]) -> bytes:
    """Fallback used when the caller does not know ``size1``."""
    prefix = _PREFIX
    suffix = _SUFFIX
    first_of = _FIRST
    lens = _LEN

    out = bytearray()
    first = codes[0]
    out.append(first)
    prev = first
    nxt = FIRST_DYNAMIC
    scratch = bytearray(0x10000)

    for cur in codes[1:]:
        if cur == END_CODE:
            break
        if cur < nxt:
            ln = lens[cur]
            p = ln - 1
            c = cur
            while c >= 0x100:
                scratch[p] = suffix[c]
                p -= 1
                c = prefix[c]
            scratch[p] = c
            out += scratch[:ln]
            cur_first = first_of[cur]
        elif cur == nxt:
            ln = lens[prev] + 1
            p = ln - 1
            scratch[p] = first_of[prev]
            p -= 1
            c = prev
            while c >= 0x100:
                scratch[p] = suffix[c]
                p -= 1
                c = prefix[c]
            scratch[p] = c
            out += scratch[:ln]
            cur_first = first_of[prev]
        else:
            raise LZW10Error(
                f"code {cur:#06x} beyond next code {nxt:#06x} at output {len(out)}"
            )

        if nxt <= MAX_CODE:
            prefix[nxt] = prev
            suffix[nxt] = cur_first
            first_of[nxt] = first_of[prev]
            lens[nxt] = lens[prev] + 1
            nxt += 1
        prev = cur

    return bytes(out)


def _decode_sized_numba(src: bytes, expected_size: int) -> bytes:
    n = len(src) // 2
    codes = _np.frombuffer(src[: n * 2], dtype=_NP_U16)
    out = _np.empty(expected_size, dtype=_np.uint8)
    pos = _lzw10_core_numba(
        codes, expected_size, _NP_PREFIX, _NP_SUFFIX, _NP_FIRST, _NP_LEN, out
    )
    if pos == -1:
        raise LZW10Error("invalid LZW code (beyond next dictionary code)")
    if pos == -2:
        raise LZW10Error(f"output overflow (expected size {expected_size})")
    _check_size(int(pos), expected_size, n * 2)
    return out.tobytes()


def lzw10_decompress(src: bytes, expected_size: int | None = None) -> bytes:
    """Decode an NZ type-0x0A payload.

    ``expected_size`` is the NZ header ``size1``; when given, a length
    mismatch raises instead of returning partial output.
    """
    if len(src) < 2:
        if expected_size not in (None, 0):
            raise LZW10Error(f"empty payload for expected size {expected_size}")
        return b""

    first = struct.unpack_from("<H", src)[0]
    if first == END_CODE:
        if expected_size not in (None, 0):
            raise LZW10Error(f"empty stream for expected size {expected_size}")
        return b""
    if first > 0xFF:
        raise LZW10Error(f"first code {first:#06x} is not a literal")

    if expected_size is None:
        return _decode_unsized(_unpack_codes(src))
    if expected_size == 0:
        return b""
    if expected_size > 0x7FFFFFFF:
        raise LZW10Error(f"unsupported output size {expected_size}")
    if _njit is not None:
        return _decode_sized_numba(src, expected_size)
    return _decode_sized(_unpack_codes(src), expected_size)
