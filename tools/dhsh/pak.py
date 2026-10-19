"""PAKF3001 archive parsing for the dhsh (Hero2D) client."""
from __future__ import annotations

import os
import struct
from dataclasses import dataclass
from typing import BinaryIO, Iterator

MAGIC = b"PAKF3001"
MAX_ENTRIES = 200_000


@dataclass(frozen=True)
class PakEntry:
    name: str
    path: str
    offset: int  # absolute file offset of the entry payload
    size: int
    kind: int  # 0 = leaf, 1 = nested PAKF3001 (or unknown)
    depth: int


def _read_archive(f: BinaryIO, base: int) -> tuple[int, list[tuple[str, int, int, int]]] | None:
    f.seek(base)
    header = f.read(16)
    if len(header) < 16 or header[:8] != MAGIC:
        return None
    checksum, count = struct.unpack("<II", header[8:16])
    if count > MAX_ENTRIES:
        return None
    entries: list[tuple[str, int, int, int]] = []
    for _ in range(count):
        record = f.read(48)
        if len(record) < 48:
            return None
        name = record[:32].split(b"\0", 1)[0].decode("latin1", "replace")
        offset, size, kind, _reserved = struct.unpack("<IIII", record[32:48])
        entries.append((name, offset, size, kind))
    return checksum, entries


def _walk_entries(
    f: BinaryIO,
    base: int,
    base_path: str,
    depth: int,
    max_depth: int,
) -> Iterator[PakEntry]:
    parsed = _read_archive(f, base)
    if parsed is None:
        return
    _checksum, entries = parsed
    for name, offset, size, kind in entries:
        path = f"{base_path}/{name}" if base_path else name
        pos = base + offset
        if kind == 1 and depth < max_depth:
            parsed_child = _read_archive(f, pos)
            if parsed_child is not None:
                yield from _walk_entries(f, pos, path, depth + 1, max_depth)
                continue
        yield PakEntry(name=name, path=path, offset=pos, size=size, kind=kind, depth=depth)


def walk_archive(
    f: BinaryIO,
    base: int = 0,
    prefix: str = "",
    depth: int = 0,
    max_depth: int = 8,
) -> Iterator[PakEntry]:
    """Yield leaves of an archive, recursing into nested PAKF3001 entries."""
    yield from _walk_entries(f, base, prefix, depth, max_depth)


def iter_pak(path: str | os.PathLike[str], max_depth: int = 8) -> Iterator[PakEntry]:
    with open(path, "rb") as f:
        yield from walk_archive(f, 0, "", 0, max_depth)


def top_level(path: str | os.PathLike[str]) -> list[tuple[str, int, int, int]]:
    with open(path, "rb") as f:
        parsed = _read_archive(f, 0)
    return parsed[1] if parsed else []


def safe_relative_path(path: str) -> str:
    """Normalize an archive path and reject traversal components."""
    parts = path.replace("\\", "/").split("/")
    clean = [p for p in parts if p not in ("", ".", "..")]
    if len(clean) != len([p for p in parts if p not in ("", ".")]):
        raise ValueError(f"unsafe archive path: {path!r}")
    if not clean:
        raise ValueError(f"empty archive path: {path!r}")
    return "/".join(clean)
