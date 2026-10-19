"""dhsh resource toolkit: PAK inventory, NZ statistics and extraction.

Run from the tools directory:
    python -m dhsh stats --out ../manifests/nz-stats.json
    python -m dhsh extract --out ../extracted/easy --types 0 --exts .lua,.scp
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import struct
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path

from .pak import iter_pak, safe_relative_path
from .nz import HEADER_SIZE, parse_header

DEFAULT_SOURCE = Path(os.environ.get("DHSH_SOURCE", r"D:\software\dhsh\shdata"))


def _paks(source: Path) -> list[Path]:
    return sorted(source.glob("*.pak"))


def _parse_types(values: list[str] | None) -> set[int] | None:
    if not values:
        return None
    out: set[int] = set()
    for raw in values:
        for part in raw.replace(";", ",").split(","):
            part = part.strip()
            if not part:
                continue
            out.add(int(part, 0))
    return out


def _parse_exts(values: list[str] | None) -> set[str] | None:
    if not values:
        return None
    out: set[str] = set()
    for raw in values:
        for part in raw.replace(";", ",").split(","):
            part = part.strip().lower()
            if not part:
                continue
            if part != "<none>" and not part.startswith("."):
                part = "." + part
            out.add(part)
    return out


def _leaf_info(f, entry) -> tuple[int, int, int, int]:
    """Return (nz_kind, size1, size2, resolved_size).

    Some PAK entries (notably .ini aliases sharing a .map offset) carry size=0
    while the real NZ payload is still present.  In that case the NZ header's
    stored size determines the span.
    """
    f.seek(entry.offset)
    head = f.read(HEADER_SIZE)
    parsed = parse_header(head)
    if parsed is None:
        return -1, entry.size, entry.size, entry.size
    resolved = entry.size
    if resolved < HEADER_SIZE:
        resolved = HEADER_SIZE + parsed.size2
    return parsed.kind, parsed.size1, parsed.size2, resolved


def cmd_list(args: argparse.Namespace) -> int:
    source = Path(args.source)
    types = _parse_types(args.types)
    exts = _parse_exts(args.exts)
    count = 0
    for path in _paks(source):
        if args.pak and path.stem not in args.pak:
            continue
        with open(path, "rb") as f:
            for e in iter_pak(path):
                ext = os.path.splitext(e.name)[1].lower() or "<none>"
                kind, s1, s2, resolved = _leaf_info(f, e)
                if types is not None and kind not in types:
                    continue
                if exts is not None and ext not in exts:
                    continue
                print(f"{path.name}\t{e.path}\t0x{kind:04x}\t{e.size}\t{resolved}\t{s1}\t{s2}")
                count += 1
                if args.limit and count >= args.limit:
                    return 0
    return 0


def cmd_stats(args: argparse.Namespace) -> int:
    source = Path(args.source)
    by_pak: dict[str, Counter] = {}
    by_ext: dict[str, Counter] = {}
    bytes_by_pak: dict[str, Counter] = {}
    bytes_by_ext: dict[str, Counter] = {}
    totals: Counter = Counter()
    total_bytes: Counter = Counter()
    zero_length = 0

    started = time.time()
    paks = _paks(source)
    for idx, path in enumerate(paks, 1):
        counts: Counter = Counter()
        sizes: Counter = Counter()
        with open(path, "rb") as f:
            for e in iter_pak(path):
                kind, _s1, _s2, resolved = _leaf_info(f, e)
                if resolved == 0:
                    zero_length += 1
                ext = os.path.splitext(e.name)[1].lower() or "<none>"
                counts[kind] += 1
                sizes[kind] += resolved
                by_ext.setdefault(ext, Counter())[kind] += 1
                bytes_by_ext.setdefault(ext, Counter())[kind] += resolved
                totals[kind] += 1
                total_bytes[kind] += resolved
        by_pak[path.name] = counts
        bytes_by_pak[path.name] = sizes
        print(f"[{idx}/{len(paks)}] {path.name}: {dict(counts)}", file=sys.stderr)
    elapsed = time.time() - started

    def named(counter: Counter) -> dict[str, int]:
        return {f"0x{k:04x}" if k >= 0 else "not-nz": v for k, v in sorted(counter.items())}

    result = {
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "source": str(source),
        "elapsed_seconds": round(elapsed, 1),
        "total_leaves": sum(totals.values()),
        "zero_length": zero_length,
        "types": named(totals),
        "type_bytes": named(total_bytes),
        "by_pak": {k: named(v) for k, v in by_pak.items()},
        "by_pak_bytes": {k: named(v) for k, v in bytes_by_pak.items()},
        "by_ext": {k: named(v) for k, v in sorted(by_ext.items())},
        "by_ext_bytes": {k: named(v) for k, v in sorted(bytes_by_ext.items())},
    }

    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(result, ensure_ascii=False, indent=1), encoding="utf-8")
        print(f"wrote {out}")
    if args.summary:
        out = Path(args.summary)
        out.parent.mkdir(parents=True, exist_ok=True)
        with open(out, "w", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            w.writerow(["scope", "key", "nz_type", "count", "bytes"])
            for kind, count in sorted(totals.items()):
                w.writerow(["total", "", f"0x{kind:04x}", count, total_bytes[kind]])
            for ext, counter in sorted(by_ext.items()):
                for kind, count in sorted(counter.items()):
                    w.writerow(["ext", ext, f"0x{kind:04x}", count, bytes_by_ext[ext][kind]])
            for pak, counter in sorted(by_pak.items()):
                for kind, count in sorted(counter.items()):
                    w.writerow(["pak", pak, f"0x{kind:04x}", count, bytes_by_pak[pak][kind]])
        print(f"wrote {out}")

    print(f"scanned {sum(totals.values())} leaves in {elapsed:.1f}s; zero-length {zero_length}")
    for kind, count in sorted(totals.items()):
        print(f"  type 0x{kind:04x}: {count} leaves, {total_bytes[kind] / 1073741824:.2f} GiB")
    return 0


def cmd_extract(args: argparse.Namespace) -> int:
    source = Path(args.source)
    out_root = Path(args.out)
    types = _parse_types(args.types)
    exts = _parse_exts(args.exts)
    if not any([args.types, args.exts, args.pak, args.limit, args.per_pak_limit, args.dry_run]):
        print(
            "refusing to extract every leaf without a filter; "
            "add --types/--exts/--pak or --limit/--per-pak-limit (or --dry-run)",
            file=sys.stderr,
        )
        return 2
    manifest_path = Path(args.manifest) if args.manifest else None
    if manifest_path:
        manifest_path.parent.mkdir(parents=True, exist_ok=True)

    written = 0
    written_bytes = 0
    skipped = 0
    total = 0
    limit_reached = False
    started = time.time()

    manifest_fh = open(manifest_path, "w", newline="", encoding="utf-8") if manifest_path else None
    writer = None
    if manifest_fh:
        writer = csv.writer(manifest_fh)
        writer.writerow(
            [
                "pak",
                "path",
                "ext",
                "nz_type",
                "size1",
                "size2",
                "entry_size",
                "resolved_size",
                "stored_size",
                "out_path",
                "sha256",
            ]
        )

    try:
        for path in _paks(source):
            if args.pak and path.stem not in args.pak:
                continue
            per_pak = 0
            with open(path, "rb") as f:
                for e in iter_pak(path):
                    ext = os.path.splitext(e.name)[1].lower() or "<none>"
                    kind, s1, s2, resolved = _leaf_info(f, e)
                    if types is not None and kind not in types:
                        continue
                    if exts is not None and ext not in exts:
                        continue
                    if args.max_size and resolved > args.max_size:
                        skipped += 1
                        continue
                    if args.per_pak_limit and per_pak >= args.per_pak_limit:
                        break
                    if args.limit and total >= args.limit:
                        limit_reached = True
                        break
                    total += 1
                    per_pak += 1
                    rel = safe_relative_path(e.path)
                    dest = out_root / path.stem / rel
                    if args.dry_run:
                        print(f"would write {dest} ({resolved} bytes, type 0x{kind:04x})")
                    else:
                        if dest.exists() and not args.overwrite:
                            skipped += 1
                            continue
                        dest.parent.mkdir(parents=True, exist_ok=True)
                        f.seek(e.offset)
                        data = f.read(resolved)
                        stored = data
                        if args.strip_nz and kind == 0 and data[:2] == b"NZ" and len(data) >= HEADER_SIZE:
                            stored = data[HEADER_SIZE:]
                        dest.write_bytes(stored)
                        written += 1
                        written_bytes += len(stored)
                        digest = ""
                        if args.hash:
                            digest = hashlib.sha256(stored).hexdigest()
                        if writer:
                            writer.writerow(
                                [
                                    path.name,
                                    e.path,
                                    ext,
                                    f"0x{kind:04x}",
                                    s1,
                                    s2,
                                    e.size,
                                    resolved,
                                    len(stored),
                                    str(dest),
                                    digest,
                                ]
                            )
            if limit_reached:
                break
    finally:
        if manifest_fh:
            manifest_fh.close()

    elapsed = time.time() - started
    print(
        f"{'dry-run: ' if args.dry_run else ''}selected {total} leaves, "
        f"wrote {written} files ({written_bytes / 1048576:.1f} MiB), skipped {skipped}, {elapsed:.1f}s"
    )
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="dhsh", description=__doc__)
    p.add_argument("--source", default=str(DEFAULT_SOURCE), help="directory containing *.pak")
    sub = p.add_subparsers(dest="command", required=True)

    ls = sub.add_parser("list", help="list matching leaves")
    ls.add_argument("--pak", nargs="*", help="PAK stems (e.g. ui player)")
    ls.add_argument("--types", nargs="*", help="NZ types, e.g. 0 0x0a 0x0b")
    ls.add_argument("--exts", nargs="*", help="extensions, e.g. .lua .scp")
    ls.add_argument("--limit", type=int)
    ls.set_defaults(func=cmd_list)

    st = sub.add_parser("stats", help="scan all leaves and report NZ type statistics")
    st.add_argument("--out", help="write JSON statistics here")
    st.add_argument("--summary", help="write CSV summary here")
    st.set_defaults(func=cmd_stats)

    ex = sub.add_parser("extract", help="extract leaves with optional filters")
    ex.add_argument("--out", required=True, help="output root directory")
    ex.add_argument("--pak", nargs="*", help="PAK stems (e.g. ui config)")
    ex.add_argument("--types", nargs="*", help="NZ types to include, e.g. 0")
    ex.add_argument("--exts", nargs="*", help="extensions to include, e.g. .lua .scp")
    ex.add_argument("--max-size", type=int, help="skip leaves larger than this many bytes")
    ex.add_argument("--limit", type=int, help="maximum number of selected leaves")
    ex.add_argument("--per-pak-limit", type=int, help="maximum selected leaves per PAK")
    ex.add_argument("--strip-nz", action="store_true", help="strip the 12-byte NZ header for type 0")
    ex.add_argument("--hash", action="store_true", help="compute SHA-256 for each output file")
    ex.add_argument("--manifest", help="write extraction manifest CSV here")
    ex.add_argument("--dry-run", action="store_true")
    ex.add_argument("--overwrite", action="store_true")
    ex.set_defaults(func=cmd_extract)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
