"""NZ resource wrapper parsing."""
from __future__ import annotations

import struct
from dataclasses import dataclass

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
