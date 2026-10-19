"""PIF per-frame / per-info table.

Header (little-endian, 20 bytes):
    0x00  char[4]  "PIF\\0"
    0x04  uint32   count        # observed 2 or 3; parser accepts <=3
    0x08  uint16   data_offset  # < 1 means 20
    0x0a  uint16   height       # bytes per row (e.g. 24/28)
    0x0c  uint16   width        # number of rows; often equals APG frame_count
    0x0e  uint16   flags        # observed 2
    0x10  uint32   reserved     # observed 0
    0x14  ...      width * height bytes

Each row is treated as height bytes (height/2 uint16 values).  For the
standalone animation files (animation/1-3.dat) width equals the paired APG
frame count; fields are mostly constant, so the exact per-field semantics
remain an inference.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

MAGIC = b"PIF\0"
HEADER_SIZE = 20


@dataclass(frozen=True)
class PIF:
    count: int
    data_offset: int
    height: int
    width: int
    flags: int
    reserved: int
    rows: tuple[tuple[int, ...], ...]


def parse_pif(data: bytes) -> PIF:
    if len(data) < HEADER_SIZE or data[:4] != MAGIC:
        raise ValueError("not a PIF file")
    count, data_offset, height, width, flags, reserved = struct.unpack_from("<IHHHHI", data, 4)
    if count > 3:
        raise ValueError(f"unsupported PIF count {count}")
    if data_offset < 1:
        data_offset = HEADER_SIZE
    total = width * height
    if width <= 0 or height <= 0 or data_offset + total > len(data):
        raise ValueError(
            f"invalid PIF dimensions width={width} height={height} "
            f"offset={data_offset} size={len(data)}"
        )
    rows: list[tuple[int, ...]] = []
    if height % 2 == 0:
        half = height // 2
        for i in range(width):
            rows.append(struct.unpack_from(f"<{half}H", data, data_offset + i * height))
    else:
        for i in range(width):
            rows.append(tuple(data[data_offset + i * height : data_offset + (i + 1) * height]))
    return PIF(count, data_offset, height, width, flags, reserved, tuple(rows))
