#!/usr/bin/env python3
"""Dump PE32 facts and compare two builds of the same image.

Usage:
    python tools/pe_facts.py dump Game.exe --out facts.txt --json facts.json
    python tools/pe_facts.py compare old.exe new.exe --anchors anchors.json \
        --out compare.txt --json compare.json

The dump format mirrors help/pe.txt (SHA256 / Base / SECTION / IMPORT / EXPORT)
so a fresh dump can replace a stale reference.  compare reports section-layout
changes, export RVA changes, byte-diff ranges and anchor mapping; anchor VAs are
absolute (image base + RVA) and are searched in the new image when the bytes at
the same VA changed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

IMAGE_FILE_MACHINE_I386 = 0x014C


def _entropy(data: bytes) -> float:
    if not data:
        return 0.0
    counts = [0] * 256
    for b in data:
        counts[b] += 1
    total = len(data)
    out = 0.0
    for c in counts:
        if c:
            p = c / total
            out -= p * math.log2(p)
    return round(out, 2)


def _u16(data: bytes, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def _u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def _cstr(data: bytes, off: int) -> str:
    end = data.find(b"\0", off)
    if end < 0:
        end = len(data)
    return data[off:end].decode("latin1", "replace")


@dataclass
class Section:
    name: str
    virtual_size: int
    virtual_address: int
    raw_size: int
    raw_offset: int
    characteristics: int
    entropy: float

    @property
    def raw_end(self) -> int:
        return self.raw_offset + self.raw_size

    @property
    def va_end(self) -> int:
        return self.virtual_address + max(self.virtual_size, self.raw_size)


@dataclass
class Export:
    name: str
    rva: int


@dataclass
class ImportModule:
    name: str
    functions: list[str] = field(default_factory=list)


@dataclass
class PEInfo:
    path: str
    sha256: str
    size: int
    machine: int
    timestamp: int
    characteristics: int
    image_base: int
    entry_rva: int
    size_of_image: int
    size_of_headers: int
    subsystem: int
    dll_characteristics: int
    sections: list[Section]
    imports: list[ImportModule]
    exports: list[Export]
    _data: bytes = field(repr=False, default=b"")

    # -- address mapping -------------------------------------------------
    def rva_to_offset(self, rva: int) -> int | None:
        for s in self.sections:
            if s.virtual_address <= rva < s.virtual_address + max(s.virtual_size, s.raw_size):
                delta = rva - s.virtual_address
                if delta < s.raw_size:
                    return s.raw_offset + delta
                return None
        if rva < self.size_of_headers:
            return rva
        return None

    def offset_to_rva(self, offset: int) -> int | None:
        for s in self.sections:
            if s.raw_offset <= offset < s.raw_end:
                return s.virtual_address + (offset - s.raw_offset)
        if offset < self.size_of_headers:
            return offset
        return None

    def va_to_offset(self, va: int) -> int | None:
        return self.rva_to_offset(va - self.image_base)

    def offset_to_va(self, offset: int) -> int | None:
        rva = self.offset_to_rva(offset)
        return None if rva is None else self.image_base + rva

    def read_va(self, va: int, length: int) -> bytes | None:
        off = self.va_to_offset(va)
        if off is None:
            return None
        return self._data[off:off + length]

    def section_at_va(self, va: int) -> Section | None:
        rva = va - self.image_base
        for s in self.sections:
            if s.virtual_address <= rva < s.va_end:
                return s
        return None

    def export_map(self) -> dict[str, int]:
        out: dict[str, int] = {}
        for e in self.exports:
            out.setdefault(e.name, e.rva)
        return out


def parse_pe(path: str | Path) -> PEInfo:
    data = Path(path).read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError(f"not a PE file: {path}")
    e_lfanew = _u32(data, 0x3C)
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise ValueError(f"missing PE signature: {path}")
    coff = e_lfanew + 4
    machine = _u16(data, coff)
    if machine != IMAGE_FILE_MACHINE_I386:
        raise ValueError(f"unsupported machine 0x{machine:04x} (PE32 only): {path}")
    number_of_sections = _u16(data, coff + 2)
    timestamp = _u32(data, coff + 4)
    size_of_optional = _u16(data, coff + 16)
    characteristics = _u16(data, coff + 18)
    opt = coff + 20
    magic = _u16(data, opt)
    if magic != 0x10B:
        raise ValueError(f"unsupported optional-header magic 0x{magic:04x} (PE32 only): {path}")
    entry_rva = _u32(data, opt + 16)
    image_base = _u32(data, opt + 28)
    size_of_image = _u32(data, opt + 56)
    size_of_headers = _u32(data, opt + 60)
    subsystem = _u16(data, opt + 68)
    dll_characteristics = _u16(data, opt + 70)
    number_of_rva = _u32(data, opt + 92)
    dirs = opt + 96
    dir_export = (_u32(data, dirs + 0), _u32(data, dirs + 4)) if number_of_rva > 0 else (0, 0)
    dir_import = (_u32(data, dirs + 8), _u32(data, dirs + 12)) if number_of_rva > 1 else (0, 0)

    sections: list[Section] = []
    sec_off = opt + size_of_optional
    for i in range(number_of_sections):
        base = sec_off + i * 40
        name = data[base:base + 8].split(b"\0", 1)[0].decode("latin1", "replace")
        vsize, vaddr, raw_size, raw_off = struct.unpack_from("<IIII", data, base + 8)
        chars = _u32(data, base + 36)
        raw = data[raw_off:raw_off + raw_size]
        sections.append(Section(name, vsize, vaddr, raw_size, raw_off, chars, _entropy(raw)))

    info = PEInfo(
        path=str(path), sha256=hashlib.sha256(data).hexdigest(), size=len(data),
        machine=machine, timestamp=timestamp, characteristics=characteristics,
        image_base=image_base, entry_rva=entry_rva, size_of_image=size_of_image,
        size_of_headers=size_of_headers, subsystem=subsystem,
        dll_characteristics=dll_characteristics,
        sections=sections, imports=[], exports=[],
    )
    info._data = data

    # -- imports ---------------------------------------------------------
    imp_rva, imp_size = dir_import
    if imp_rva:
        off = info.rva_to_offset(imp_rva)
        if off is not None:
            for desc_off in range(off, min(off + max(imp_size, 0x1000), len(data)), 20):
                oft, _ts, _fc, name_rva, ft = struct.unpack_from("<IIIII", data, desc_off)
                if not (oft or name_rva or ft):
                    break
                name_off = info.rva_to_offset(name_rva)
                mod_name = _cstr(data, name_off) if name_off is not None else f"<rva 0x{name_rva:x}>"
                funcs: list[str] = []
                thunk_rva = oft or ft
                thunk_off = info.rva_to_offset(thunk_rva)
                if thunk_off is not None:
                    j = 0
                    while True:
                        value = _u32(data, thunk_off + j * 4)
                        if not value:
                            break
                        if value & 0x80000000:
                            funcs.append(f"ordinal_{value & 0xFFFF}")
                        else:
                            hint_off = info.rva_to_offset(value)
                            if hint_off is not None:
                                funcs.append(_cstr(data, hint_off + 2))
                        j += 1
                info.imports.append(ImportModule(mod_name, funcs))

    # -- exports ---------------------------------------------------------
    exp_rva, exp_size = dir_export
    if exp_rva:
        off = info.rva_to_offset(exp_rva)
        if off is not None:
            (_chars, _ts, _maj, _min, _name_rva, base_ord,
             nfunc, nname, funcs_rva, names_rva, ords_rva) = struct.unpack_from("<IIHHIIIIIII", data, off)
            funcs_off = info.rva_to_offset(funcs_rva)
            names_off = info.rva_to_offset(names_rva)
            ords_off = info.rva_to_offset(ords_rva)
            if funcs_off is not None and names_off is not None and ords_off is not None:
                for i in range(nname):
                    name_rva = _u32(data, names_off + i * 4)
                    name_off = info.rva_to_offset(name_rva)
                    if name_off is None:
                        continue
                    name = _cstr(data, name_off)
                    ord_index = _u16(data, ords_off + i * 2)
                    if ord_index >= nfunc:
                        continue
                    func_rva = _u32(data, funcs_off + ord_index * 4)
                    info.exports.append(Export(name, func_rva))
                for ord_index in range(nfunc):
                    func_rva = _u32(data, funcs_off + ord_index * 4)
                    if func_rva == 0:
                        continue
                    if not any(e.rva == func_rva for e in info.exports):
                        info.exports.append(Export(f"ordinal_{base_ord + ord_index}", func_rva))
    return info


def dump_text(info: PEInfo) -> str:
    lines: list[str] = []
    lines.append(f"SHA256 {info.sha256}")
    lines.append(
        f"Base {info.image_base:08X} Entry RVA {info.entry_rva:08X} Size {info.size_of_image:08X}"
    )
    for s in info.sections:
        name_repr = repr(s.name.encode("latin1") + b"\0" * (8 - len(s.name.encode("latin1"))))
        lines.append(
            f"SECTION {name_repr} RVA={s.virtual_address:08X} VS={s.virtual_size:X} "
            f"RAW={s.raw_offset:X} SIZE={s.raw_size:X} entropy={s.entropy:.2f} flags={s.characteristics:08X}"
        )
    for imp in info.imports:
        lines.append(f"IMPORT {imp.name}: {', '.join(imp.functions)}")
    lines.append(f"EXPORT DLL {str(b'Game.exe')}")
    for exp in sorted(info.exports, key=lambda e: (e.rva, e.name)):
        lines.append(f"EXPORT RVA={exp.rva:08X} {exp.name}")
    return "\n".join(lines) + "\n"


def dump_json(info: PEInfo) -> dict[str, Any]:
    return {
        "path": info.path,
        "sha256": info.sha256,
        "size": info.size,
        "machine": f"0x{info.machine:04x}",
        "timestamp": info.timestamp,
        "characteristics": f"0x{info.characteristics:04x}",
        "image_base": info.image_base,
        "entry_rva": info.entry_rva,
        "entry_va": info.image_base + info.entry_rva,
        "size_of_image": info.size_of_image,
        "size_of_headers": info.size_of_headers,
        "subsystem": info.subsystem,
        "dll_characteristics": f"0x{info.dll_characteristics:04x}",
        "sections": [s.__dict__ for s in info.sections],
        "imports": [{"name": m.name, "count": len(m.functions), "functions": m.functions}
                    for m in info.imports],
        "exports": [{"name": e.name, "rva": e.rva, "va": info.image_base + e.rva}
                    for e in sorted(info.exports, key=lambda e: (e.rva, e.name))],
    }


def _diff_ranges(old: bytes, new: bytes, merge_gap: int) -> list[tuple[int, int]]:
    """Return [start, end) file-offset ranges where bytes differ, merged by gap."""
    limit = min(len(old), len(new))
    ranges: list[list[int]] = []
    i = 0
    while i < limit:
        if old[i] == new[i]:
            i += 1
            continue
        start = i
        while i < limit and old[i] != new[i]:
            i += 1
        if ranges and start - ranges[-1][1] <= merge_gap:
            ranges[-1][1] = i
        else:
            ranges.append([start, i])
    if len(old) != len(new):
        ranges.append([limit, max(len(old), len(new))])
    return [(a, b) for a, b in ranges]


def _search(data: bytes, pattern: bytes, max_hits: int = 8) -> list[int]:
    hits: list[int] = []
    pos = data.find(pattern)
    while pos >= 0 and len(hits) < max_hits:
        hits.append(pos)
        pos = data.find(pattern, pos + 1)
    return hits


def _hex(data: bytes | None, n: int = 32) -> str:
    if not data:
        return "<none>"
    return " ".join(f"{b:02x}" for b in data[:n])


def compare(old: PEInfo, new: PEInfo, anchors: list[dict[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {
        "old": {"path": old.path, "sha256": old.sha256, "size": old.size,
                "image_base": old.image_base, "entry_rva": old.entry_rva,
                "size_of_image": old.size_of_image},
        "new": {"path": new.path, "sha256": new.sha256, "size": new.size,
                "image_base": new.image_base, "entry_rva": new.entry_rva,
                "size_of_image": new.size_of_image},
        "same_image_base": old.image_base == new.image_base,
        "same_entry_rva": old.entry_rva == new.entry_rva,
        "sections": [],
        "diff_ranges": [],
        "anchors": [],
        "exports": {},
    }
    if old.image_base != new.image_base:
        result["notes"] = ["image base differs; anchor VA translation assumes new image base"]

    for a, b in zip(old.sections, new.sections):
        result["sections"].append({
            "name": a.name,
            "old": {"rva": a.virtual_address, "vs": a.virtual_size, "raw": a.raw_offset,
                    "size": a.raw_size, "entropy": a.entropy},
            "new": {"rva": b.virtual_address, "vs": b.virtual_size, "raw": b.raw_offset,
                    "size": b.raw_size, "entropy": b.entropy},
            "identical_layout": (a.virtual_address, a.virtual_size, a.raw_offset, a.raw_size) ==
                                 (b.virtual_address, b.virtual_size, b.raw_offset, b.raw_size),
        })
    if len(old.sections) != len(new.sections):
        result.setdefault("notes", []).append(
            f"section count differs: old={len(old.sections)} new={len(new.sections)}")

    ranges = _diff_ranges(old._data, new._data, merge_gap=16)
    total_changed = sum(b - a for a, b in ranges)
    result["changed_bytes"] = total_changed
    result["changed_range_count"] = len(ranges)
    for start, end in ranges:
        va = old.offset_to_va(start)
        result["diff_ranges"].append({
            "offset": start,
            "end": end,
            "length": end - start,
            "va": va,
            "va_end": (va + (end - start)) if va is not None else None,
            "section": (old.section_at_va(va).name if va is not None and old.section_at_va(va) else None),
            "old_head": _hex(old._data[start:start + 32]),
            "new_head": _hex(new._data[start:start + 32]),
        })

    # -- exports ---------------------------------------------------------
    old_ex = old.export_map()
    new_ex = new.export_map()
    missing = sorted(set(old_ex) - set(new_ex))
    added = sorted(set(new_ex) - set(old_ex))
    changed = []
    deltas: dict[int, int] = {}
    for name in sorted(set(old_ex) & set(new_ex)):
        if old_ex[name] != new_ex[name]:
            delta = new_ex[name] - old_ex[name]
            deltas[delta] = deltas.get(delta, 0) + 1
            changed.append({"name": name, "old_rva": old_ex[name], "new_rva": new_ex[name],
                            "delta": delta})
    result["exports"] = {
        "old_count": len(old_ex),
        "new_count": len(new_ex),
        "missing_in_new": missing,
        "added_in_new": added,
        "changed_rva": changed,
        "delta_histogram": {str(k): v for k, v in sorted(deltas.items(), key=lambda kv: -kv[1])},
    }

    # -- anchors ---------------------------------------------------------
    for anchor in anchors:
        name = anchor["name"]
        va_old = int(str(anchor["va"]), 0)
        expected = anchor.get("expected_hex")
        expected_bytes = bytes.fromhex(expected) if expected else None
        rva = va_old - old.image_base
        va_new = new.image_base + rva
        old_bytes = old.read_va(va_old, 48)
        new_bytes = new.read_va(va_new, 48)
        entry: dict[str, Any] = {
            "name": name,
            "old_va": va_old,
            "va_at_same_rva": va_new,
            "old_head": _hex(old_bytes),
            "new_head": _hex(new_bytes),
            "same_bytes_at_same_va": old_bytes == new_bytes,
            "expected_hex": expected,
        }
        if expected_bytes is not None:
            entry["expected_matches_old"] = old_bytes is not None and old_bytes.startswith(expected_bytes)
            entry["expected_matches_new_at_same_va"] = (
                new_bytes is not None and new_bytes.startswith(expected_bytes))
        # search the new image for the old/expected byte signature
        pattern = expected_bytes or (old_bytes[:32] if old_bytes else None)
        if pattern:
            hits = _search(new._data, pattern)
            entry["signature_search_hits"] = [
                {"offset": h,
                 "va": new.offset_to_va(h),
                 "section": (new.section_at_va(new.offset_to_va(h)).name
                             if new.offset_to_va(h) is not None and new.section_at_va(new.offset_to_va(h)) else None)}
                for h in hits
            ]
            entry["signature_unique_in_new"] = len(hits) == 1
            if len(hits) == 1:
                hit_va = new.offset_to_va(hits[0])
                entry["mapped_new_va"] = hit_va
                entry["mapped_delta"] = None if hit_va is None else hit_va - va_old
        result["anchors"].append(entry)
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("dump", help="dump PE facts")
    d.add_argument("exe")
    d.add_argument("--out")
    d.add_argument("--json")

    c = sub.add_parser("compare", help="compare two PE32 builds")
    c.add_argument("old")
    c.add_argument("new")
    c.add_argument("--anchors", help="JSON list of {name, va, expected_hex?}")
    c.add_argument("--out")
    c.add_argument("--json")

    args = ap.parse_args()
    if args.cmd == "dump":
        info = parse_pe(args.exe)
        text = dump_text(info)
        if args.out:
            Path(args.out).write_text(text, encoding="utf-8")
            print(f"wrote {args.out}")
        if args.json:
            Path(args.json).write_text(
                json.dumps(dump_json(info), ensure_ascii=False, indent=1), encoding="utf-8")
            print(f"wrote {args.json}")
        if not args.out and not args.json:
            print(text)
        return 0

    old = parse_pe(args.old)
    new = parse_pe(args.new)
    anchors: list[dict[str, Any]] = []
    if args.anchors:
        anchors = json.loads(Path(args.anchors).read_text(encoding="utf-8"))
    result = compare(old, new, anchors)
    text = json.dumps(result, ensure_ascii=False, indent=1)
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
        print(f"wrote {args.out}")
    if args.json:
        Path(args.json).write_text(text, encoding="utf-8")
        print(f"wrote {args.json}")
    if not args.out and not args.json:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
