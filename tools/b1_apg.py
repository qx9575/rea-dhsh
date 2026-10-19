"""APG B1: validate ~50 animations and profile AG index/metadata positions."""
from __future__ import annotations

import glob
import json
import os
import random
import sys
from collections import Counter

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "tools"))
from dhsh.formats.a16 import parse_cti  # noqa: E402
from dhsh.formats.apg import iter_frames, frame_to_rgba  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPORT = os.path.join(ROOT, "manifests", "batches", "s4-apg-b1.json")
APG_ROOT = os.path.join(ROOT, "extracted", "samples", "apg")
CTI_ROOT = os.path.join(ROOT, "extracted", "images-proprietary")


def find_cti(apg_path: str):
    rel = os.path.relpath(apg_path, APG_ROOT)
    parts = rel.split(os.sep)
    if len(parts) < 2:
        return None
    pak = parts[0]
    d = os.path.dirname(os.path.join(*parts[1:])) if len(parts) > 2 else ""
    while d and d != ".":
        for pattern in (
            os.path.join(CTI_ROOT, pak, d, "clrtb.cti"),
            os.path.join(CTI_ROOT, "*", d, "clrtb.cti"),
        ):
            cands = glob.glob(pattern)
            if cands:
                return cands[0]
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return None


def main(limit=50):
    files = sorted(glob.glob(os.path.join(ROOT, "extracted", "samples", "apg", "**", "*.apg"), recursive=True))
    random.seed(2026)
    random.shuffle(files)
    picks = files[:limit]
    for name in ("1.apg", "2.apg", "3.apg"):
        p = os.path.join(r"D:\software\dhsh\shdata\animation", name)
        if os.path.exists(p):
            picks.append(p)
    stats = {
        "samples": 0,
        "frames": 0,
        "frames_decoded": 0,
        "frames_failed": 0,
        "compression": Counter(),
        "alpha_mode": Counter(),
        "magic": Counter(),
        "flags": Counter(),
        "palette_resolved": 0,
        "palette_missing": 0,
        "frame_count_hist": Counter(),
        "index_position": Counter(),
        "index_in_s2_tail_distance": Counter(),
        "errors": [],
        "sample_details": [],
    }
    for p in picks:
        data = open(p, "rb").read()
        try:
            ag, frames = iter_frames(data)
        except Exception as exc:  # noqa: BLE001
            stats["errors"].append({"path": p, "error": f"{type(exc).__name__}: {exc}"})
            continue
        stats["samples"] += 1
        stats["frame_count_hist"][ag.frame_count] += 1
        stats["flags"][ag.flags] += 1
        ok = 0
        fail = []
        palette = None
        if any(fr.header.magic == b"AI" for fr in frames):
            cti = find_cti(p) if p.startswith(APG_ROOT) else None
            if cti:
                palette = parse_cti(open(cti, "rb").read())
                stats["palette_resolved"] += 1
            else:
                stats["palette_missing"] += 1
        for fr in frames:
            stats["frames"] += 1
            stats["compression"][fr.header.compression] += 1
            stats["alpha_mode"][fr.header.alpha_mode] += 1
            stats["magic"][fr.header.magic.decode("latin1")] += 1
            try:
                frame_to_rgba(fr, palette=palette)
                ok += 1
                stats["frames_decoded"] += 1
            except Exception as exc:  # noqa: BLE001
                stats["frames_failed"] += 1
                fail.append(f"{fr.index}:{type(exc).__name__}:{exc}")
        # classify AG index offsets against frame blocks
        if ag.index and frames:
            for idx, _iflag, off in ag.index:
                placed = False
                for fr in frames:
                    s1_start = fr.offset + fr.header.header_size
                    s2_start = s1_start + fr.header.stream1_size
                    end = s2_start + fr.header.stream2_size
                    if s1_start <= off < s2_start:
                        stats["index_position"]["block_s1"] += 1
                        placed = True
                        break
                    if s2_start <= off < end:
                        stats["index_position"]["block_s2"] += 1
                        stats["index_in_s2_tail_distance"][min(end - off, 64)] += 1
                        placed = True
                        break
                    if fr.offset <= off < s1_start:
                        stats["index_position"]["block_header"] += 1
                        placed = True
                        break
                if not placed:
                    stats["index_position"]["outside_blocks"] += 1
        stats["sample_details"].append(
            {"path": p, "frames": len(frames), "decoded": ok, "failed": len(fail), "errors": fail[:5]}
        )
    def conv(o):
        if isinstance(o, Counter):
            return {str(k): v for k, v in o.items()}
        raise TypeError
    with open(REPORT, "w", encoding="utf-8") as fh:
        json.dump(stats, fh, ensure_ascii=False, indent=1, default=conv)
    print(json.dumps(stats, ensure_ascii=False, indent=1, default=conv)[:8000])
    print("report:", REPORT)


if __name__ == "__main__":
    main()
