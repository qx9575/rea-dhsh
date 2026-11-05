#!/usr/bin/env python
"""Export a small, explicitly filtered set of NZ parity fixtures."""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from dhsh.nz import HEADER_SIZE, decompress, parse_header  # noqa: E402
from dhsh.pak import iter_pak  # noqa: E402

SUPPORTED_KINDS = (0, 0x0A, 0x0B)
MAX_LIMIT = 100
MAX_SAMPLE_BYTES = 256 * 1024
MAX_TOTAL_BYTES = 8 * 1024 * 1024


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def select_samples(pak: str, path_pattern: str, kinds: set[int], limit: int) -> list[dict]:
    samples: list[dict] = []
    total_bytes = 0
    with open(pak, "rb") as source:
        for entry in iter_pak(pak):
            normalized_path = entry.path.replace("\\", "/")
            if not fnmatch.fnmatchcase(normalized_path.lower(), path_pattern.lower()):
                continue
            if entry.size < HEADER_SIZE:
                continue
            source.seek(entry.offset)
            header = source.read(HEADER_SIZE)
            nz = parse_header(header)
            if nz is None or nz.kind not in kinds:
                continue
            if nz.size1 > MAX_SAMPLE_BYTES or nz.size2 > MAX_SAMPLE_BYTES:
                continue
            if entry.size < HEADER_SIZE + nz.size2:
                raise ValueError(f"truncated NZ entry: {normalized_path}")
            sample_bytes = HEADER_SIZE + nz.size2 + nz.size1
            if total_bytes + sample_bytes > MAX_TOTAL_BYTES:
                break

            payload = source.read(nz.size2)
            if len(payload) != nz.size2:
                raise ValueError(f"truncated NZ payload: {normalized_path}")
            stored = header + payload
            decoded = decompress(nz.kind, payload, nz.size1)
            if len(decoded) != nz.size1:
                raise ValueError(f"decoded size mismatch: {normalized_path}")
            samples.append({
                "path": normalized_path,
                "kind": nz.kind,
                "size1": nz.size1,
                "size2": nz.size2,
                "stored": stored,
                "decoded": decoded,
            })
            total_bytes += sample_bytes
            if len(samples) == limit:
                break
    return samples


def write_samples(out: str, pak: str, samples: list[dict]) -> None:
    os.makedirs(out)
    rows = []
    for index, sample in enumerate(samples):
        stem = f"{index:05d}_{sample['kind']:02x}"
        nz_name = stem + ".nz"
        decoded_name = stem + ".expected"
        with open(os.path.join(out, nz_name), "xb") as output:
            output.write(sample["stored"])
        with open(os.path.join(out, decoded_name), "xb") as output:
            output.write(sample["decoded"])
        rows.append({
            "pak": os.path.basename(pak),
            "path": sample["path"],
            "kind": f"0x{sample['kind']:04x}",
            "size1": sample["size1"],
            "size2": sample["size2"],
            "nz_file": nz_name,
            "expected_file": decoded_name,
            "nz_sha256": sha256(sample["stored"]),
            "decoded_sha256": sha256(sample["decoded"]),
        })
    manifest = {
        "format_version": 1,
        "sample_count": len(rows),
        "samples": rows,
    }
    with open(os.path.join(out, "manifest.json"), "x", encoding="utf-8") as output:
        json.dump(manifest, output, ensure_ascii=False, indent=2)
        output.write("\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pak", required=True, help="one PAK archive to sample")
    parser.add_argument("--path", required=True, help="case-insensitive archive-path glob")
    parser.add_argument("--limit", required=True, type=int, help="sample count, at most 100")
    parser.add_argument("--kind", action="append", type=lambda value: int(value, 0),
                        choices=SUPPORTED_KINDS, help="NZ kind to include; repeatable")
    parser.add_argument("--out", required=True, help="new output directory")
    args = parser.parse_args()

    if not 1 <= args.limit <= MAX_LIMIT:
        parser.error(f"--limit must be between 1 and {MAX_LIMIT}")
    if not args.path:
        parser.error("--path must not be empty")
    if not os.path.isfile(args.pak):
        parser.error("--pak must name an existing archive")
    if os.path.lexists(args.out):
        parser.error("--out must not already exist")

    kinds = set(args.kind) if args.kind else set(SUPPORTED_KINDS)
    samples = select_samples(args.pak, args.path, kinds, args.limit)
    if not samples:
        print("no matching samples", file=sys.stderr)
        return 2
    write_samples(args.out, args.pak, samples)
    counts = Counter(sample["kind"] for sample in samples)
    summary = ", ".join(f"0x{kind:04x}={counts[kind]}" for kind in sorted(counts))
    print(f"exported {len(samples)} samples ({summary}) to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
