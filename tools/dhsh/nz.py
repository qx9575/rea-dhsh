"""NZ resource wrapper parsing."""
from __future__ import annotations

import struct
from dataclasses import dataclass

from .nz_lzo import lzo1x_decompress
from .nz_lzw10 import lzw10_decompress

MAGIC = b"NZ"
HEADER_SIZE = 12


@dataclass(frozen=True)
class NZHeader:
    kind: int  # 0x0000 stored, 0x000A/0x000B compressed
    size1: int  # logical / uncompressed size
    size2: int  # stored payload size

    @property
    def stored(self) -> bool:
        return self.kind == 0


def parse_header(data: bytes) -> NZHeader | None:
    if len(data) < HEADER_SIZE or data[:2] != MAGIC:
        return None
    kind, size1, size2 = struct.unpack("<HII", data[2:HEADER_SIZE])
    return NZHeader(kind=kind, size1=size1, size2=size2)


class NZError(ValueError):
    pass


def decompress(kind: int, payload: bytes, size1: int) -> bytes:
    """Decode an NZ payload for a supported type.

    0x0000 returns the payload unchanged (callers normally use the stored
    bytes directly), 0x000A is the 16-bit LZW codec, 0x000B is LZO1X.
    """
    if kind == 0:
        return payload
    if kind == 0x000A:
        return lzw10_decompress(payload, size1)
    if kind == 0x000B:
        return lzo1x_decompress(payload, size1)
    raise NZError(f"unsupported NZ type 0x{kind:04x}")
