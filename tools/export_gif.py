#!/usr/bin/env python
"""Compose APG animations onto a canvas and export animated GIFs.

Usage (from the repository root):
    python tools/export_gif.py --sample 10 --seed 1 \
        --out extracted/_batches/gif-b0 --manifest manifests/batches/gif-b0.csv

    python tools/export_gif.py --match player/body --limit 50 --all \
        --out extracted/_batches/gif-player

Frames are placed at their APG header x/y; the canvas is the union of all
frame rectangles.  Per-frame delays come from a paired PIF (``f5`` field,
milliseconds; see docs/formats/pif.md) when one matches the frame count,
otherwise ``--delay`` (default 80 ms, matching the PIF default).

AI frames resolve CTI through the same index used by export_apg.py.
"""
from __future__ import annotations

import argparse
import csv
import os
import random
import sys
import time
from collections import Counter
from multiprocessing import Pool

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from dhsh.formats.apg import iter_frames  # noqa: E402
from dhsh.formats.pif import parse_pif  # noqa: E402
from check_apg_cti import (  # noqa: E402
    resolve_cti, load_or_build_cti_cache,
)

try:
    from dhsh.formats.apg_fast import frame_to_rgba_fast, cti_rgba_lut
    FAST = True
except ImportError:  # pragma: no cover - environment dependent
    FAST = False

DEFAULT_ROOTS = (
    ("t0", os.path.join("extracted", "decoded_t0")),
    ("0a", os.path.join("extracted", "decoded")),
    ("0b", os.path.join("extracted", "decoded_0b")),
)
CTI_ROOTS = (
    os.path.join(ROOT, "extracted", "decoded"),
    os.path.join(ROOT, "extracted", "decoded_0b"),
    os.path.join(ROOT, "extracted", "images-proprietary"),
)


def iter_apg(root: str):
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if name.lower().endswith(".apg"):
                yield os.path.join(dirpath, name)


def _init_worker(index) -> None:
    import check_apg_cti as _c
    _c._INDEX = index


def find_pif_delays(apg_path: str, frame_count: int, default: int):
    """Return (pif_path or None, delays list).  PIF row field f5 = delay ms."""
    folder = os.path.dirname(apg_path)
    stem = os.path.splitext(os.path.basename(apg_path))[0]
    names = [stem + ".dat", "perinfo.dat"]
    try:
        names += sorted(n for n in os.listdir(folder)
                        if n.lower().endswith(".dat") and n not in names)
    except OSError:
        pass
    for name in names:
        path = os.path.join(folder, name)
        if not os.path.isfile(path):
            continue
        try:
            pif = parse_pif(open(path, "rb").read())
        except Exception:  # noqa: BLE001 - PIF is optional
            continue
        if pif.width == frame_count and pif.height >= 12:
            return path, [max(1, int(row[5])) for row in pif.rows]
    return None, [default] * frame_count


def to_gif_frames(images, scale: int):
    from PIL import Image

    out = []
    for img in images:
        if scale and max(img.width, img.height) > scale:
            s = scale / max(img.width, img.height)
            img = img.resize((max(1, int(img.width * s)),
                              max(1, int(img.height * s))), Image.LANCZOS)
        # Quantize to 255 colours and reserve index 255 for transparency.
        rgb = Image.new("RGB", img.size, (0, 0, 0))
        rgb.paste(img, mask=img.getchannel("A"))
        p = rgb.quantize(colors=255, method=Image.Quantize.MEDIANCUT)
        pal = (p.getpalette() or [])[:768]
        p.putpalette(pal + [0] * (768 - len(pal)))
        transparent = img.getchannel("A").point(lambda a: 255 if a < 128 else 0)
        p.paste(255, mask=transparent)
        p.info["transparency"] = 255
        out.append(p)
    return out


def export_chunk(task: tuple[list[tuple[str, str]], dict]) -> dict:
    items, cfg = task
    from PIL import Image

    rows: list[dict] = []
    counters: Counter = Counter()
    errors: list[tuple[str, str]] = []
    cti_cache: dict[str, tuple[object, object]] = {}

    for label, path in items:
        counters["files"] += 1
        t0 = time.perf_counter()
        try:
            with open(path, "rb") as fh:
                data = fh.read()
            ag, frames = iter_frames(data)
        except Exception as exc:  # noqa: BLE001
            errors.append((path, f"{type(exc).__name__}: {exc}"))
            continue

        rel = os.path.relpath(path, cfg["root_by_label"][label])
        parts = rel.split(os.sep)
        pak = parts[0] if len(parts) > 1 else ""
        logical = os.path.dirname(os.path.join(*parts[1:])) if len(parts) > 2 else ""
        logical = "" if logical == "." else logical

        ai_frames = [fr for fr in frames if fr.header.magic == b"AI"]
        cti = None
        lut = None
        if ai_frames:
            cti_path, _src = resolve_cti(pak, logical)
            if cti_path:
                if cti_path not in cti_cache:
                    try:
                        from dhsh.formats.a16 import parse_cti
                        parsed = parse_cti(open(cti_path, "rb").read())
                        cti_cache[cti_path] = (
                            parsed,
                            cti_rgba_lut(parsed, 1) if FAST else None,
                        )
                    except Exception as exc:  # noqa: BLE001
                        errors.append((path, f"CTI {cti_path}: {exc}"))
                        cti_cache[cti_path] = (None, None)
                cti, lut = cti_cache[cti_path]
            else:
                errors.append((path, "AI frame without a resolvable clrtb.cti"))
                continue

        min_x = min(fr.header.x for fr in frames)
        min_y = min(fr.header.y for fr in frames)
        max_x = max(fr.header.x + fr.header.width for fr in frames)
        max_y = max(fr.header.y + fr.header.height for fr in frames)
        width, height = max(1, max_x - min_x), max(1, max_y - min_y)
        if cfg["max_dim"] and max(width, height) > cfg["max_dim"]:
            errors.append((path, f"canvas {width}x{height} exceeds --max-dim"))
            continue

        images = []
        try:
            for fr in frames:
                if FAST:
                    img = frame_to_rgba_fast(fr, cti=cti, table=1, lut=lut)
                else:
                    from dhsh.formats.apg import frame_to_rgba
                    img = frame_to_rgba(fr, palette=cti, palette_table=1)
                canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
                canvas.paste(img, (fr.header.x - min_x, fr.header.y - min_y), img)
                images.append(canvas)
            pif_path, delays = find_pif_delays(path, len(frames), cfg["delay"])
            gif_frames = to_gif_frames(images, cfg["scale"])
        except Exception as exc:  # noqa: BLE001
            errors.append((path, f"{type(exc).__name__}: {exc}"))
            continue

        stem = os.path.splitext(rel)[0]
        out = os.path.join(cfg["out_root"], label, stem + ".gif")
        try:
            os.makedirs(os.path.dirname(out), exist_ok=True)
            if cfg["overwrite"] or not os.path.exists(out):
                gif_frames[0].save(
                    out, "GIF", save_all=True, append_images=gif_frames[1:],
                    duration=delays, loop=0, disposal=2, transparency=255,
                    optimize=False,
                )
            size_out = os.path.getsize(out)
            counters["written"] += 1
            counters["frames"] += len(frames)
            counters["bytes"] += size_out
            counters["pif_delays"] += 1 if pif_path else 0
        except Exception as exc:  # noqa: BLE001
            errors.append((path, f"save: {type(exc).__name__}: {exc}"))
            continue

        rows.append({
            "corpus": label,
            "path": rel.replace(os.sep, "/"),
            "frames": len(frames),
            "width": width,
            "height": height,
            "delay_ms": cfg["delay"],
            "pif": os.path.relpath(pif_path, ROOT).replace(os.sep, "/") if pif_path else "",
            "delay_total_ms": sum(delays),
            "bytes": size_out,
            "out": out,
            "elapsed": round(time.perf_counter() - t0, 3),
            "error": "",
        })
    return {"rows": rows, "counters": dict(counters), "errors": errors}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", action="append", default=None,
                    help="corpus as LABEL=PATH (repeatable); defaults to t0/0a/0b")
    ap.add_argument("--match", default=None, help="case-insensitive path substring")
    ap.add_argument("--sample", type=int, default=0)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--delay", type=int, default=80, help="default frame delay (ms)")
    ap.add_argument("--scale", type=int, default=512,
                    help="downscale so max(W,H) <= scale; 0 disables")
    ap.add_argument("--max-dim", type=int, default=4096)
    ap.add_argument("--out", default=os.path.join(ROOT, "extracted", "gif"))
    ap.add_argument("--manifest", default=None)
    ap.add_argument("--cti-cache", default=os.path.join(
        ROOT, "manifests", "batches", "cti-index.json"))
    ap.add_argument("--overwrite", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()

    if not args.all and not args.sample and not args.limit and not args.dry_run:
        ap.error("refusing full corpus without --all; use --sample/--limit first")

    roots: list[tuple[str, str]] = []
    if args.root:
        for spec in args.root:
            if "=" not in spec:
                ap.error(f"--root expects LABEL=PATH, got {spec!r}")
            label, rel = spec.split("=", 1)
            roots.append((label, os.path.abspath(rel)))
    else:
        roots = [(label, os.path.join(ROOT, rel)) for label, rel in DEFAULT_ROOTS]

    items: list[tuple[str, str]] = []
    root_by_label: dict[str, str] = {}
    for label, root in roots:
        if not os.path.isdir(root):
            ap.error(f"root not found: {root}")
        found = sorted(iter_apg(root))
        if args.match:
            needle = args.match.lower()
            found = [p for p in found if needle in p.replace(os.sep, "/").lower()]
        print(f"root {label}: {root} ({len(found)} APG after filter)")
        if args.limit:
            found = found[: args.limit]
        root_by_label[label] = root
        items.extend((label, path) for path in found)

    if args.sample and args.sample < len(items):
        random.Random(args.seed).shuffle(items)
        items = items[: args.sample]

    if args.dry_run:
        print(f"would export {len(items)} GIFs")
        return 0

    cfg = {
        "out_root": args.out,
        "delay": args.delay,
        "scale": args.scale,
        "max_dim": args.max_dim,
        "overwrite": args.overwrite,
        "root_by_label": root_by_label,
    }
    index = load_or_build_cti_cache(args.cti_cache, CTI_ROOTS)
    started = time.time()
    nchunks = max(1, args.jobs * 2)
    chunks = [(items[i::nchunks], cfg) for i in range(nchunks)]
    chunks = [c for c in chunks if c[0]]
    with Pool(args.jobs, initializer=_init_worker, initargs=(index,)) as pool:
        parts = pool.map(export_chunk, chunks)

    counters: Counter = Counter()
    rows: list[dict] = []
    errors: list[tuple[str, str]] = []
    for part in parts:
        counters.update(part["counters"])
        rows.extend(part["rows"])
        errors.extend(part["errors"])
    elapsed = time.time() - started

    if args.manifest:
        os.makedirs(os.path.dirname(os.path.abspath(args.manifest)), exist_ok=True)
        fields = ["corpus", "path", "frames", "width", "height", "delay_ms",
                  "pif", "delay_total_ms", "bytes", "out", "elapsed", "error"]
        with open(args.manifest, "w", newline="", encoding="utf-8") as fh:
            writer = csv.DictWriter(fh, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    print(f"GIF: {counters['written']}/{counters['files']} files, "
          f"{counters['frames']} frames, {counters['bytes'] / 1048576:.1f} MiB, "
          f"PIF delays {counters['pif_delays']}, {len(errors)} errors, "
          f"{elapsed:.1f}s")
    for path, why in errors[:20]:
        print("  error:", path, why)
    if args.manifest:
        print("manifest:", args.manifest)
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
