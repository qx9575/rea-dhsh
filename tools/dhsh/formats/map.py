"""Hero2D map side files: MAP (INI), CEL, MAN, MSK/core, MLK.

Layouts verified against Game.exe and the extracted corpus:

CEL (collision bitmask, parser VA 0x42563d):
    0x00 "CEL\\0"; 0x04 u32 version=2; 0x08 u32 total_size;
    0x0c u32 flags; 0x10 u32 width_cells; 0x14 u32 height_cells;
    0x18 ... one bitset (MSB-first, 1 = blocked), row stride
    ceil(width/8).  If flags & 1, an extra width*height/8-byte layer
    precedes the mask.

MAN (animation path table, parser VA 0x41ee5b):
    0x00 "MAN\\0"; 0x04 u32 version=4; 0x08 u32 total_size;
    0x0c u32 0; 0x10 u32 data_offset; 0x14 u32 entry_size (100);
    0x18 u32 count; entries are null-terminated 100-byte paths.

MSK / core (cover-mask records, loader VA 0x27aa0 -> parser VA 0x274b0):
    0x00 "MSK\\0"; 0x04 u32 version=5; 0x08 u32 total_size;
    0x0c u32 flags; 0x10 u32 header_size=40;
    0x14 u32 record_size; 0x18 u32 record_count; 0x1c u32 data_offset;
    0x20 u32 placement_size=24; 0x24 u32 placement_count.
    MSK record = char[record_size-28] name + 7 * u32 fields (284 bytes);
    core record = 7 * u32 fields (28 bytes).
    Fields: width, height, base_y (signed), kind, blob_offset (absolute),
    blob_size, slope (signed Q8.8).
    Placement = 6 * u32: record_index, x, y, flag, group, frame
    (x/y signed; flag 1 = static, 0 = group member).
    Blob = u16 run words: (covered << 15) | length, row-major, never
    crossing a scanline; each row sums to width.

    The map loader rewrites MASKFILE's extension to ".core" (VA 0x27aa0),
    so `.core` is the active format; `.MSK` (with BMP names) is legacy.

MLK (map jump/link table):
    0x00 "lnk\\0"; 0x04 u32 version=3; 0x08 u32 total_size;
    0x0c u32 1; 0x10 u32 count;
    records = u32 value + char[104] target map name (108 bytes).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass


# ---------------------------------------------------------------------------
# MAP INI
# ---------------------------------------------------------------------------

def parse_map_ini(data: bytes) -> dict[str, dict[str, str]]:
    """Parse the GBK/UTF-8 map INI into {section: {key: value}}."""
    text = None
    for enc in ("utf-8-sig", "gb18030", "gbk"):
        try:
            text = data.decode(enc)
            break
        except UnicodeDecodeError:
            continue
    if text is None:
        text = data.decode("latin1")
    result: dict[str, dict[str, str]] = {}
    section = ""
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(("#", ";", "//")):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            result.setdefault(section, {})
            continue
        if "=" in line:
            key, value = line.split("=", 1)
            result.setdefault(section, {})[key.strip().upper()] = value.strip()
    return result


# ---------------------------------------------------------------------------
# CEL
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class CEL:
    flags: int
    width: int
    height: int
    bits: bytes  # packed rows, MSB-first

    def get(self, x: int, y: int) -> int:
        if not (0 <= x < self.width and 0 <= y < self.height):
            return 1
        stride = (self.width + 7) // 8
        byte = self.bits[y * stride + (x >> 3)]
        return (byte >> (7 - (x & 7))) & 1


def parse_cel(data: bytes, layer: int = 1) -> CEL:
    """Parse a CEL collision bitmap.

    ``flags & 1`` files carry two bitmaps selected by the loader's last
    argument (``FUN_004252e0``): layer 1 (first bitmap) is installed at
    ``map+0x34`` and is the normal ground collision layer; layer 0 (second
    bitmap) is the auxiliary layer at ``map+0x38``.  1 = blocked, 0 = walkable
    (runtime-confirmed on YingTianFu, 2026-10-23).
    """
    if len(data) < 24 or data[:4] != b"CEL\0":
        raise ValueError("not a CEL file")
    version, total, flags, width, height = struct.unpack_from("<IIIII", data, 4)
    if version != 2:
        raise ValueError(f"unsupported CEL version {version}")
    if total != len(data):
        raise ValueError(f"CEL size {total} != file size {len(data)}")
    stride = (width + 7) // 8
    needed = stride * height
    if flags & 1:
        if layer not in (0, 1):
            raise ValueError(f"invalid CEL layer {layer}")
        offset = 24 if layer == 1 else 24 + needed
        required = 24 + needed * 2
    else:
        offset = 24
        required = 24 + needed
    if required > len(data):
        raise ValueError(
            f"CEL mask truncated ({required} > {len(data)})")
    return CEL(flags, width, height, data[offset : offset + needed])


def cel_to_image(data: bytes, layer: int = 1):
    """Return a PIL 'L' image: 0 = walkable, 255 = blocked."""
    from PIL import Image

    cel = parse_cel(data, layer)
    stride = (cel.width + 7) // 8
    out = bytearray(cel.width * cel.height)
    for y in range(cel.height):
        row = cel.bits[y * stride : (y + 1) * stride]
        pos = y * cel.width
        for x, b in enumerate(row):
            base = pos + x * 8
            n = min(8, cel.width - x * 8)
            for i in range(n):
                out[base + i] = 255 if (b >> (7 - i)) & 1 else 0
    return Image.frombytes("L", (cel.width, cel.height), bytes(out))


# ---------------------------------------------------------------------------
# MAN
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class MAN:
    data_offset: int
    entry_size: int
    entries: tuple[str, ...]


def parse_man(data: bytes) -> MAN:
    if len(data) < 28 or data[:4] != b"MAN\0":
        raise ValueError("not a MAN file")
    version, total, _reserved, data_offset, entry_size, count = struct.unpack_from(
        "<IIIIII", data, 4
    )
    if version != 4:
        raise ValueError(f"unsupported MAN version {version}")
    if total != len(data):
        raise ValueError(f"MAN size {total} != file size {len(data)}")
    end = data_offset + entry_size * count
    if end > len(data):
        raise ValueError("MAN entries truncated")
    entries = []
    for i in range(count):
        raw = data[data_offset + i * entry_size : data_offset + (i + 1) * entry_size]
        entries.append(raw.split(b"\0", 1)[0].decode("latin1"))
    return MAN(data_offset, entry_size, tuple(entries))


# ---------------------------------------------------------------------------
# MSK / core
# ---------------------------------------------------------------------------

def _s32(value: int) -> int:
    return value - 0x100000000 if value >= 0x80000000 else value


@dataclass(frozen=True)
class MSKRecord:
    """One cover-mask definition.

    ``data_offset`` is absolute in the container file (not relative to the
    placement table); ``data_size`` is the length of the RLE blob.
    """

    name: str                  # BMP path in legacy MSK, "" in core
    width: int                 # f0, mask width in pixels
    height: int                # f1, mask height in pixels
    base_y: int                # f2, signed ground-line intercept
    kind: int                  # f3, observed 1 (normal), 2..10 rare
    data_offset: int           # f4, absolute file offset of the blob
    data_size: int             # f5, blob byte length
    slope: int                 # f6, signed Q8.8 ground-line slope

    @property
    def fields(self) -> tuple[int, int, int, int, int, int, int]:
        return (self.width, self.height, self.base_y & 0xFFFFFFFF,
                self.kind, self.data_offset, self.data_size,
                self.slope & 0xFFFFFFFF)


@dataclass(frozen=True)
class MSKPlacement:
    record: int                # index into MSK.records
    x: int                     # signed map pixel X (mask left edge)
    y: int                     # signed map pixel Y (mask top edge)
    flag: int                  # 1 = static cover, 0 = group member
    group: int                 # animation/part group id (0 = none)
    frame: int                 # part/frame index within the group


@dataclass(frozen=True)
class MSK:
    version: int
    total_size: int
    flags: int
    header_size: int
    record_size: int
    record_count: int
    placement_size: int        # observed 24
    placement_count: int
    data_offset: int           # start of the placement table
    blob_offset: int           # first blob offset (= data_offset + pc*24)
    records: tuple[MSKRecord, ...]
    placements: tuple[MSKPlacement, ...]
    data: bytes                # complete container bytes


def parse_msk(data: bytes) -> MSK:
    if len(data) < 40 or data[:4] != b"MSK\0":
        raise ValueError("not an MSK file")
    (version, total, flags, header_size, record_size, count,
     data_offset, placement_size, placement_count) = struct.unpack_from(
        "<9I", data, 4)
    if version != 5:
        raise ValueError(f"unsupported MSK version {version}")
    if total != len(data):
        raise ValueError(f"MSK size {total} != file size {len(data)}")
    if header_size != 40:
        raise ValueError(f"unexpected MSK header size {header_size}")
    if record_size < 28:
        raise ValueError(f"unexpected MSK record size {record_size}")
    if data_offset != header_size + record_size * count:
        raise ValueError("MSK data offset does not match records")
    if placement_size != 24:
        raise ValueError(f"unexpected placement size {placement_size}")
    place_end = data_offset + placement_size * placement_count
    if place_end > len(data):
        raise ValueError("MSK placement table truncated")

    records = []
    for i in range(count):
        off = header_size + i * record_size
        raw = data[off : off + record_size]
        name_len = record_size - 28
        name = raw[:name_len].split(b"\0", 1)[0].decode("latin1")
        (w, h, f2, kind, f4, f5, f6) = struct.unpack_from(
            "<7I", raw, name_len)
        if f4 < place_end or f4 + f5 > len(data):
            raise ValueError(f"MSK record {i} blob overruns the file")
        records.append(MSKRecord(name, w, h, _s32(f2), kind, f4, f5,
                                 _s32(f6)))

    placements = []
    for i in range(placement_count):
        off = data_offset + i * placement_size
        rec, x, y, flag, group, frame = struct.unpack_from("<6I", data, off)
        if rec >= count:
            raise ValueError(f"MSK placement {i} references record {rec}")
        placements.append(MSKPlacement(rec, _s32(x), _s32(y), flag,
                                       group, frame))

    return MSK(version, total, flags, header_size, record_size, count,
               placement_size, placement_count, data_offset, place_end,
               tuple(records), tuple(placements), data)


def _record_blob(msk: MSK, rec: MSKRecord) -> bytes:
    end = rec.data_offset + rec.data_size
    if rec.data_offset < 40 or end > len(msk.data):
        raise ValueError("MSK blob is outside the container")
    return msk.data[rec.data_offset:end]


def iter_runs(msk: MSK, record: int | MSKRecord):
    """Yield ``(covered, run_length)`` RLE words of one record.

    Each 16-bit word is ``(covered << 15) | length``; runs never cross a
    scanline and the lengths of every row sum to ``width``.
    """
    rec = msk.records[record] if isinstance(record, int) else record
    blob = _record_blob(msk, rec)
    if len(blob) & 1:
        raise ValueError(f"odd MSK blob length {len(blob)}")
    for (word,) in struct.iter_unpack("<H", blob):
        yield word >> 15, word & 0x7FFF


def decode_mask(msk: MSK, record: int | MSKRecord):
    """Decode one record's RLE blob into a uint8 (height, width) 0/1 mask.

    Strict: raises if a run crosses a row boundary or the decoded pixel
    count does not equal ``width * height``.
    """
    import numpy as np

    rec = msk.records[record] if isinstance(record, int) else record
    width, height = rec.width, rec.height
    if width <= 0 or height <= 0:
        raise ValueError(f"invalid mask dimensions {width}x{height}")
    mask = np.zeros((height, width), dtype=np.uint8)
    x = y = 0
    for covered, length in iter_runs(msk, rec):
        if length == 0:
            raise ValueError("zero-length MSK run")
        left = length
        while left:
            take = min(left, width - x)
            if take <= 0:
                raise ValueError("MSK run crosses a row boundary")
            if covered:
                mask[y, x:x + take] = 1
            x += take
            left -= take
            if x == width:
                x = 0
                y += 1
    if x != 0 or y != height:
        raise ValueError(
            f"MSK mask decoded {y} rows + {x} px, expected {width}x{height}")
    return mask


# ---------------------------------------------------------------------------
# MLK
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class MLKRecord:
    value: int
    target: str


@dataclass(frozen=True)
class MLK:
    records: tuple[MLKRecord, ...]


def parse_mlk(data: bytes) -> MLK:
    if len(data) < 20 or data[:4] != b"lnk\0":
        raise ValueError("not an MLK file")
    version, total, _unknown, count = struct.unpack_from("<IIII", data, 4)
    if version != 3:
        raise ValueError(f"unsupported MLK version {version}")
    if total != len(data):
        raise ValueError(f"MLK size {total} != file size {len(data)}")
    if 20 + count * 108 != len(data):
        raise ValueError("MLK record table does not match file size")
    records = []
    for i in range(count):
        off = 20 + i * 108
        value = struct.unpack_from("<I", data, off)[0]
        name = data[off + 4 : off + 108].split(b"\0", 1)[0].decode("latin1")
        records.append(MLKRecord(value, name))
    return MLK(tuple(records))
