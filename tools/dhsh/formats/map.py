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

MSK / core (mask records):
    0x00 "MSK\\0"; 0x04 u32 version=5; 0x08 u32 total_size;
    0x0c u32 flags; 0x10 u32 header_size=40;
    0x14 u32 record_size; 0x18 u32 record_count; 0x1c u32 data_offset.
    MSK record = char[256] name + 7 * u32 fields (record_size 284).
    core record = 7 * u32 fields (record_size 28).

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


def parse_cel(data: bytes) -> CEL:
    if len(data) < 24 or data[:4] != b"CEL\0":
        raise ValueError("not a CEL file")
    version, total, flags, width, height = struct.unpack_from("<IIIII", data, 4)
    if version != 2:
        raise ValueError(f"unsupported CEL version {version}")
    if total != len(data):
        raise ValueError(f"CEL size {total} != file size {len(data)}")
    mask_bytes = width * height // 8
    stride = (width + 7) // 8
    needed = stride * height
    offset = 24
    if flags & 1:
        offset += mask_bytes
    if offset + needed > len(data):
        raise ValueError(f"CEL mask truncated ({offset}+{needed} > {len(data)})")
    return CEL(flags, width, height, data[offset : offset + needed])


def cel_to_image(data: bytes):
    """Return a PIL 'L' image: 0 = walkable, 255 = blocked."""
    from PIL import Image

    cel = parse_cel(data)
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

@dataclass(frozen=True)
class MSKRecord:
    name: str
    fields: tuple[int, ...]


@dataclass(frozen=True)
class MSK:
    record_size: int
    data_offset: int
    records: tuple[MSKRecord, ...]
    data: bytes


def parse_msk(data: bytes) -> MSK:
    if len(data) < 40 or data[:4] != b"MSK\0":
        raise ValueError("not an MSK file")
    (version, total, _flags, header_size, record_size, count,
     data_offset, _a, _b) = struct.unpack_from("<IIIIIIIII", data, 4)
    if version != 5:
        raise ValueError(f"unsupported MSK version {version}")
    if total != len(data):
        raise ValueError(f"MSK size {total} != file size {len(data)}")
    if header_size != 40:
        raise ValueError(f"unexpected MSK header size {header_size}")
    if data_offset != header_size + record_size * count:
        raise ValueError("MSK data offset does not match records")
    records = []
    for i in range(count):
        off = header_size + i * record_size
        raw = data[off : off + record_size]
        if record_size >= 284:
            name = raw[:256].split(b"\0", 1)[0].decode("latin1")
            fields = struct.unpack_from("<7I", raw, 256)
        else:
            name = ""
            fields = struct.unpack_from("<7I", raw, 0)
        records.append(MSKRecord(name, tuple(fields)))
    return MSK(record_size, data_offset, tuple(records),
               data[data_offset:])


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
