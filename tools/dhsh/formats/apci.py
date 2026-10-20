"""APCI container parser (perinfo.pak).

Container layout (validated against shdata/perinfo.pak, 7,808,665 bytes):

    header:
        char[4]  "APCI"
        u32      version = 0x101
    group (up to 20, parsed while cursor < end):
        u16      group_id            (usually equals the group index)
        u16      record_count        (0 is allowed; header is still 4 bytes)
        record[record_count]:
            u16      record_id
            u16      sub_count
            char[32] name
            sub_count x entry:
                u32 a
                u32 b                (mid_count = b & 0xffff, high half 0 here)
                mid_count x mid:
                    u32 header       (item_count = header >> 16)
                    item_count x item:
                        u32 x
                        u32 y
                        blob (see below)

    items with (x & 0xffff) != 0 carry a blob whose size is
        (x >> 24) & 0xff                     if ((y >> 16) & 0xff) == 1
        ((x >> 24) & 0xff) * (x & 0xffff) & 0xffff   otherwise
    (both branches truncated to 16 bits by ``movzx ebx, ax``).

Evidence: Game.exe VA 0x82a460 (parser), 0x82a210 (per-group record
array), 0x826e70 (allocator).  The loader 0x82a6d0 reads the whole file and
calls the parser with (buffer, size, version_out); the non-null buffer makes
the 8-byte entry branch (0x82a56f) execute.  The parser consumes the file
exactly to its final byte.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

MAGIC = b"APCI"
VERSION = 0x101
MAX_GROUPS = 20
RECORD_SIZE = 36


class APCIFormatError(ValueError):
    pass


@dataclass(frozen=True)
class APCIHeader:
    version: int
    end: int


@dataclass(frozen=True)
class APCIItem:
    index: int
    x: int
    y: int
    blob_offset: int
    blob_size: int

    @property
    def width(self) -> int:
        return self.x & 0xFFFF

    @property
    def bytes_per_pixel(self) -> int:
        return (self.x >> 24) & 0xFF

    @property
    def flags(self) -> int:
        return (self.y >> 16) & 0xFF


@dataclass(frozen=True)
class APCIMid:
    index: int
    header: int
    items: tuple[APCIItem, ...]

    @property
    def item_count(self) -> int:
        return self.header >> 16

    @property
    def low16(self) -> int:
        return self.header & 0xFFFF


@dataclass(frozen=True)
class APCIEntry:
    index: int
    a: int
    b: int
    mids: tuple[APCIMid, ...]

    @property
    def mid_count(self) -> int:
        return self.b & 0xFFFF

    @property
    def b_high(self) -> int:
        return self.b >> 16


@dataclass(frozen=True)
class APCIRecord:
    index: int
    record_id: int
    name: str
    entries: tuple[APCIEntry, ...]


@dataclass(frozen=True)
class APCIGroup:
    index: int
    group_id: int
    records: tuple[APCIRecord, ...]


@dataclass(frozen=True)
class APCIFile:
    header: APCIHeader
    groups: tuple[APCIGroup, ...]


def _u16(data: bytes, pos: int) -> int:
    if pos + 2 > len(data):
        raise APCIFormatError(f"u16 overrun at {pos}")
    return struct.unpack_from("<H", data, pos)[0]


def _u32(data: bytes, pos: int) -> int:
    if pos + 4 > len(data):
        raise APCIFormatError(f"u32 overrun at {pos}")
    return struct.unpack_from("<I", data, pos)[0]


def blob_size(x: int, y: int) -> int:
    """Return the stored blob size for one item (0 when absent)."""
    width = x & 0xFFFF
    if width == 0:
        return 0
    byte3 = (x >> 24) & 0xFF
    if ((y >> 16) & 0xFF) == 1:
        return byte3
    return (byte3 * width) & 0xFFFF


def parse_header(data: bytes) -> APCIHeader:
    if len(data) < 8 or data[:4] != MAGIC:
        raise APCIFormatError("missing APCI magic")
    version = struct.unpack_from("<I", data, 4)[0]
    if version != VERSION:
        raise APCIFormatError(f"unsupported APCI version 0x{version:x}")
    return APCIHeader(version, len(data))


def iter_items(data: bytes):
    """Yield ``(group, record, entry, mid, item)`` for every item.

    Stops at the first byte past the container or at the 20-group limit,
    matching the loader's loop.
    """
    hdr = parse_header(data)
    end = hdr.end
    pos = 8
    g = 0
    while pos < end and g < MAX_GROUPS:
        group_id = _u16(data, pos)
        count = _u16(data, pos + 2)
        pos += 4
        for r in range(count):
            if pos + RECORD_SIZE > end:
                raise APCIFormatError(f"record {r} of group {g} overruns at {pos}")
            record_id, sub = struct.unpack_from("<HH", data, pos)
            raw = data[pos + 4:pos + RECORD_SIZE]
            pos += RECORD_SIZE
            name = raw.split(b"\0", 1)[0].decode("gbk", "replace")
            for s in range(sub):
                a = _u32(data, pos)
                b = _u32(data, pos + 4)
                pos += 8
                for m in range(b & 0xFFFF):
                    header = _u32(data, pos)
                    pos += 4
                    item_count = header >> 16
                    for i in range(item_count):
                        if pos + 8 > end:
                            raise APCIFormatError(
                                f"item {i} of group {g} record {r} overruns at {pos}")
                        x = struct.unpack_from("<I", data, pos)[0]
                        y = struct.unpack_from("<I", data, pos + 4)[0]
                        pos += 8
                        size = blob_size(x, y)
                        if pos + size > end:
                            raise APCIFormatError(
                                f"blob overruns at {pos} size={size}")
                        yield (g, group_id, r, record_id, name, s, a, b,
                               m, header, i, x, y, pos, size)
                        pos += size
        g += 1
    if pos != end:
        raise APCIFormatError(f"trailing bytes: parsed to {pos}, size {end}")


def parse_apci(data: bytes) -> APCIFile:
    """Materialise the full tree (687k items for perinfo.pak; prefer iter_items)."""
    hdr = parse_header(data)
    end = hdr.end
    pos = 8
    groups: list[APCIGroup] = []
    g = 0
    while pos < end and g < MAX_GROUPS:
        group_id = _u16(data, pos)
        count = _u16(data, pos + 2)
        pos += 4
        records: list[APCIRecord] = []
        for r in range(count):
            if pos + RECORD_SIZE > end:
                raise APCIFormatError(f"record {r} of group {g} overruns at {pos}")
            record_id, sub = struct.unpack_from("<HH", data, pos)
            raw = data[pos + 4:pos + RECORD_SIZE]
            pos += RECORD_SIZE
            name = raw.split(b"\0", 1)[0].decode("gbk", "replace")
            entries: list[APCIEntry] = []
            for s in range(sub):
                a = _u32(data, pos)
                b = _u32(data, pos + 4)
                pos += 8
                mids: list[APCIMid] = []
                for m in range(b & 0xFFFF):
                    header = _u32(data, pos)
                    pos += 4
                    items: list[APCIItem] = []
                    for i in range(header >> 16):
                        x = _u32(data, pos)
                        y = _u32(data, pos + 4)
                        pos += 8
                        size = blob_size(x, y)
                        if pos + size > end:
                            raise APCIFormatError(
                                f"blob overruns at {pos} size={size}")
                        items.append(APCIItem(i, x, y, pos, size))
                        pos += size
                    mids.append(APCIMid(m, header, tuple(items)))
                entries.append(APCIEntry(s, a, b, tuple(mids)))
            records.append(APCIRecord(r, record_id, name, tuple(entries)))
        groups.append(APCIGroup(g, group_id, tuple(records)))
        g += 1
    if pos != end:
        raise APCIFormatError(f"trailing bytes: parsed to {pos}, size {end}")
    return APCIFile(hdr, tuple(groups))


def record_summary(data: bytes) -> list[dict]:
    """Aggregate per-record counts without materialising every item."""
    hdr = parse_header(data)
    end = hdr.end
    pos = 8
    g = 0
    out: list[dict] = []
    while pos < end and g < MAX_GROUPS:
        group_id = _u16(data, pos)
        count = _u16(data, pos + 2)
        pos += 4
        for r in range(count):
            if pos + RECORD_SIZE > end:
                raise APCIFormatError(f"record {r} of group {g} overruns at {pos}")
            record_id, sub = struct.unpack_from("<HH", data, pos)
            raw = data[pos + 4:pos + RECORD_SIZE]
            pos += RECORD_SIZE
            name = raw.split(b"\0", 1)[0].decode("gbk", "replace")
            entries = mids = items = blobs = blob_bytes = 0
            for s in range(sub):
                a = _u32(data, pos)
                b = _u32(data, pos + 4)
                pos += 8
                entries += 1
                for m in range(b & 0xFFFF):
                    header = _u32(data, pos)
                    pos += 4
                    mids += 1
                    for i in range(header >> 16):
                        x = _u32(data, pos)
                        y = _u32(data, pos + 4)
                        pos += 8
                        size = blob_size(x, y)
                        if pos + size > end:
                            raise APCIFormatError(
                                f"blob overruns at {pos} size={size}")
                        items += 1
                        if size:
                            blobs += 1
                            blob_bytes += size
                        pos += size
            out.append({
                "group": g,
                "group_id": group_id,
                "record": r,
                "record_id": record_id,
                "name": name,
                "sub_count": sub,
                "entries": entries,
                "mids": mids,
                "items": items,
                "blob_count": blobs,
                "blob_bytes": blob_bytes,
            })
        g += 1
    if pos != end:
        raise APCIFormatError(f"trailing bytes: parsed to {pos}, size {end}")
    return out
