"""PG2 image container (Hero2D).

Layout (little-endian), verified against Game.exe VA 0x82cec0 / 0x82ce00
and all 150 extracted files:

    0x00 char[2]  "PG"
    0x02 uint16   header_size = 20
    0x04 uint16   frame_count
    0x06 uint8    version = 1
    0x07 uint8    reserved
    0x08 uint32   total_size (== file size)
    0x0c uint32   resource id / parameter
    0x10 uint16   frame_entry_size = 40
    0x12 uint16   reserved
    0x14 frame_count × 40-byte entries:
         +0x00 uint16 f0
         +0x02 uint16 f1
         +0x04 uint16 f2
         +0x06 uint16 f3
         +0x08 uint16 f4
         +0x0a uint16 f5
         +0x0c uint16 x
         +0x0e uint16 y
         +0x10 uint16 width
         +0x12 uint16 height
         +0x14 uint16 width2
         +0x16 uint16 height2
         +0x18 uint32 reserved
         +0x1c uint16 aux0
         +0x1e uint16 aux1
         +0x20 uint32 data_offset   (relative to file start)
         +0x24 uint32 data_size

Each frame payload is a complete ``AP`` block (24-byte A16 header followed by
stream1/stream2), so ``dhsh.formats.a16_fast.a16_to_rgba_fast`` decodes it.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass


@dataclass(frozen=True)
class PG2Header:
    header_size: int
    frame_count: int
    version: int
    total_size: int
    resource_id: int
    entry_size: int


@dataclass(frozen=True)
class PG2Frame:
    index: int
    f0: int
    f1: int
    f2: int
    f3: int
    f4: int
    f5: int
    x: int
    y: int
    width: int
    height: int
    width2: int
    height2: int
    aux0: int
    aux1: int
    data_offset: int
    data_size: int


def parse_header(data: bytes) -> PG2Header:
    if len(data) < 20 or data[:2] != b"PG":
        raise ValueError("not a PG2 file")
    header_size, frame_count, version, _reserved = struct.unpack_from("<HHBB", data, 2)
    total_size, resource_id = struct.unpack_from("<II", data, 8)
    entry_size = struct.unpack_from("<H", data, 16)[0]
    if header_size != 20:
        raise ValueError(f"unsupported PG2 header size {header_size}")
    if version != 1:
        raise ValueError(f"unsupported PG2 version {version}")
    if entry_size != 40:
        raise ValueError(f"unsupported PG2 entry size {entry_size}")
    if total_size != len(data):
        raise ValueError(f"PG2 size {total_size} != file size {len(data)}")
    return PG2Header(header_size, frame_count, version, total_size,
                     resource_id, entry_size)


def iter_frames(data: bytes) -> tuple[PG2Header, list[PG2Frame]]:
    hdr = parse_header(data)
    frames: list[PG2Frame] = []
    for i in range(hdr.frame_count):
        off = hdr.header_size + i * hdr.entry_size
        if off + hdr.entry_size > len(data):
            raise ValueError(f"PG2 frame table truncated at entry {i}")
        (f0, f1, f2, f3, f4, f5, x, y, w, h, w2, h2) = struct.unpack_from(
            "<12H", data, off
        )
        aux0, aux1 = struct.unpack_from("<HH", data, off + 0x1C)
        data_offset, data_size = struct.unpack_from("<II", data, off + 0x20)
        if data_offset + data_size > len(data):
            raise ValueError(f"PG2 frame {i} data out of range")
        frames.append(PG2Frame(
            index=i, f0=f0, f1=f1, f2=f2, f3=f3, f4=f4, f5=f5,
            x=x, y=y, width=w, height=h, width2=w2, height2=h2,
            aux0=aux0, aux1=aux1,
            data_offset=data_offset, data_size=data_size,
        ))
    return hdr, frames


def frame_block(data: bytes, frame: PG2Frame) -> bytes:
    """Return the AP block payload for one frame."""
    block = data[frame.data_offset : frame.data_offset + frame.data_size]
    if len(block) < 24 or block[:2] != b"AP":
        raise ValueError(f"PG2 frame {frame.index} is not an AP block")
    return block
