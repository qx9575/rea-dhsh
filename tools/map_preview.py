#!/usr/bin/env python
"""Compose per-map preview images from decoded resources.

Layers (in order):
  1. background  -> extracted/special_jpg_png/<BACKGROUND with .png>
  2. CEL (optional diagnostic, --cel) -> cell bitmap scaled to map pixels
  3. MSK/core cover masks -> placed at placement (x, y)

The cover-mask layer draws static placements (file flag=1) in red and, by
default, the first frame of each group (flag=0) in orange.  The CEL overlay
is off by default: the CEL parser selects between two bitmaps with its last
parameter and the layer/polarity semantics are not yet verified.

Outputs one image per map and a CSV manifest.  Never runs the game.

Examples:
    python tools/map_preview.py --map BangPai_1 --map YingTianFu \
        --out extracted/_batches/map-preview-b0 \
        --manifest manifests/batches/map-preview-b0.csv
    python tools/map_preview.py --sample 10 --seed 1 --tag b1
    python tools/map_preview.py --all \
        --out extracted/map_preview \
        --manifest manifests/batches/map-preview-full.csv

Output names use the map name only (no version/SHA suffix).
"""
from __future__ import annotations

import argparse
import csv
import os
import random
import struct
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from PIL import Image  # noqa: E402
import numpy as np  # noqa: E402

from dhsh.formats.map import (  # noqa: E402
    decode_mask, parse_map_ini, parse_msk,
)

ROOT_DIRS = ("extracted/decoded", "extracted/decoded_0b",
             "extracted/easy", "extracted/decoded_t0")
MAP_INI_DIR = os.path.join(ROOT, "extracted", "easy", "config", "map")
BG_DIRS = ("extracted/special_jpg_png", "extracted/std_png/std",
           "extracted/std_png/0a", "extracted/std_png/0b")

STATIC_FILL = (255, 0, 180, 55)
STATIC_EDGE = (255, 0, 180, 230)
GROUP_FILL = (255, 140, 0, 65)
GROUP_EDGE = (255, 140, 0, 235)
CEL_COLOR = (0, 80, 255, 70)


def norm(path: str) -> str:
    return path.replace("\\", "/").lstrip("/")


def resolve_asset(rel: str, exts) -> str | None:
    """Find an asset by map-relative path across the decoded corpora."""
    stem = os.path.splitext(norm(rel))[0]
    for root in ROOT_DIRS:
        for ext in exts:
            candidate = os.path.join(ROOT, root, stem + ext)
            if os.path.exists(candidate):
                return candidate
            candidate = os.path.join(ROOT, root, norm(rel))
            if os.path.exists(candidate):
                return candidate
    return None


def resolve_background(rel: str) -> tuple[Image.Image | None, str | None]:
    stem = os.path.splitext(norm(rel))[0]
    candidates = [os.path.join(ROOT, d, stem + ".png") for d in BG_DIRS]
    candidates += [os.path.join(ROOT, "extracted", "images-standard", norm(rel))]
    for path in candidates:
        if not os.path.exists(path):
            continue
        try:
            return Image.open(path).convert("RGBA"), path
        except Exception:  # noqa: BLE001
            continue
    return None, None


def cel_layers(data: bytes):
    """Return (width, height, flags, [first, second?]) raw bitmaps."""
    if len(data) < 24 or data[:4] != b"CEL\0":
        raise ValueError("not a CEL file")
    version, total, flags, width, height = struct.unpack_from("<IIIII", data, 4)
    if version != 2:
        raise ValueError(f"unsupported CEL version {version}")
    stride = (width + 7) // 8
    need = stride * height
    off = 24
    layers = []
    if flags & 1:
        if off + need * 2 > len(data):
            raise ValueError("CEL has two-layer flag but truncated data")
        layers = [data[off:off + need], data[off + need:off + need * 2]]
    else:
        if off + need > len(data):
            raise ValueError("CEL bitmap truncated")
        layers = [data[off:off + need]]
    return width, height, flags, layers


def cel_image(data: bytes, layer: int, invert: bool) -> Image.Image:
    width, height, flags, layers = cel_layers(data)
    # layer 1 = first bitmap (parser param_5 != 0)
    # layer 0 = second bitmap when flags&1, else the only bitmap
    if layer == 1 or len(layers) == 1:
        buf = layers[0]
    else:
        buf = layers[1]
    stride = (width + 7) // 8
    out = bytearray(width * height)
    for y in range(height):
        row = buf[y * stride:(y + 1) * stride]
        base = y * width
        for x, b in enumerate(row):
            pos = base + x * 8
            n = min(8, width - x * 8)
            for i in range(n):
                bit = (b >> (7 - i)) & 1
                out[pos + i] = 255 if (bit ^ invert) else 0
    return Image.frombytes("L", (width, height), bytes(out))


def edge_mask(arr: np.ndarray) -> np.ndarray:
    """Return a 2-pixel binary edge of a 0/1 mask."""
    a = arr.astype(bool)
    er = a.copy()
    er[1:, :] &= a[:-1, :]
    er[:-1, :] &= a[1:, :]
    er[:, 1:] &= a[:, :-1]
    er[:, :-1] &= a[:, 1:]
    er2 = er.copy()
    er2[1:, :] &= er[:-1, :]
    er2[:-1, :] &= er[1:, :]
    er2[:, 1:] &= er[:, :-1]
    er2[:, :-1] &= er[:, 1:]
    return ((a & ~er2).astype(np.uint8)) * 255


def paste_mask(overlay: Image.Image, mask: Image.Image, x: int, y: int,
               color) -> int:
    w, h = overlay.size
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(w, x + mask.width), min(h, y + mask.height)
    if x1 <= x0 or y1 <= y0:
        return 0
    crop = mask.crop((x0 - x, y0 - y, x1 - x, y1 - y))
    overlay.paste(color, (x0, y0, x1, y1), crop)
    return 1


def select_maps(args):
    paths = sorted(
        os.path.join(MAP_INI_DIR, n) for n in os.listdir(MAP_INI_DIR)
        if n.lower().endswith(".map")
    )
    entries = []
    for path in paths:
        try:
            ini = parse_map_ini(open(path, "rb").read())
        except Exception:  # noqa: BLE001
            continue
        base = ini.get("base", {})
        ext = ini.get("ext", {})
        name = os.path.splitext(os.path.basename(path))[0]
        entries.append({
            "name": name, "ini": path,
            "width": base.get("WIDTH"), "height": base.get("HEIGHT"),
            "background": base.get("BACKGROUND", ""),
            "cellfile": base.get("CElLFILE", base.get("CELLFILE", "")),
            "maskfile": base.get("MASKFILE", ""),
            "title": ext.get("NAME", ""),
        })
    if args.map:
        wanted = {m.lower() for m in args.map}
        entries = [e for e in entries if e["name"].lower() in wanted]
        found = {e["name"].lower() for e in entries}
        missing = wanted - found
        if missing:
            print("map(s) not found:", ", ".join(sorted(missing)))
    if args.sample:
        rng = random.Random(args.seed)
        entries = rng.sample(entries, min(args.sample, len(entries)))
    elif args.limit:
        entries = entries[:args.limit]
    return entries


def render_one(entry, args, used_names):
    t0 = time.time()
    row = {
        "name": entry["name"], "title": entry["title"],
        "ini": os.path.relpath(entry["ini"], ROOT).replace(os.sep, "/"),
        "width": entry["width"], "height": entry["height"],
        "ini_size": "",
        "background": "", "bg_size": "",
        "cel": "", "cel_cells": "", "cel_layer": "", "cel_blocked_pct": "",
        "mask": "", "placements": "", "static": "", "groups": "",
        "output": "", "bytes": "", "elapsed_s": "", "warning": "",
        "error": "",
    }
    try:
        bg, bg_path = resolve_background(entry["background"])
        if bg is None:
            if args.allow_missing:
                row["warning"] = "background missing; skipped"
                row["elapsed_s"] = round(time.time() - t0, 2)
                return row
            raise FileNotFoundError(
                f"background not found: {entry['background']}")
        row["background"] = os.path.relpath(bg_path, ROOT).replace(os.sep, "/")
        row["bg_size"] = f"{bg.width}x{bg.height}"
        # The placement table and the background share the pixel space; INI
        # WIDTH/HEIGHT can describe only a playable sub-area.  Keep the
        # background at native size so masks stay aligned.
        width, height = bg.size
        row["width"], row["height"] = width, height
        if entry["width"]:
            ini_w, ini_h = int(entry["width"]), int(entry["height"])
            row["ini_size"] = f"{ini_w}x{ini_h}"
            if (ini_w, ini_h) != (width, height):
                row["warning"] = (
                    f"INI {ini_w}x{ini_h} != background {width}x{height}; "
                    "using background")
        overlay = Image.new("RGBA", (width, height), (0, 0, 0, 0))

        cel_path = (resolve_asset(entry["cellfile"], (".CEL", ".cel"))
                    if entry["cellfile"] else None)
        cel_data = None
        cel_size = None
        if cel_path:
            cel_data = open(cel_path, "rb").read()
            cw, ch, _flags, _layers = cel_layers(cel_data)
            cel_size = (cw * 16, ch * 16)
            row["cel"] = os.path.relpath(cel_path, ROOT).replace(os.sep, "/")
            row["cel_cells"] = f"{cw}x{ch}"

        if args.cel:
            if cel_data is None:
                row["warning"] = (row["warning"] + "; " if row["warning"]
                                  else "") + "CEL not found"
            elif cel_size != (width, height):
                row["warning"] = (row["warning"] + "; " if row["warning"]
                                  else "") + (
                    f"CEL {cel_size[0]}x{cel_size[1]} != canvas "
                    f"{width}x{height}; CEL not drawn")
            else:
                ci = cel_image(cel_data, args.cel_layer, args.cel_invert)
                blocked = sum(1 for p in ci.getdata() if p)
                row["cel_layer"] = str(args.cel_layer)
                row["cel_blocked_pct"] = round(
                    blocked / (ci.width * ci.height) * 100, 2)
                overlay.paste(CEL_COLOR, (0, 0), ci)

        placements = static_n = group_n = 0
        if entry["maskfile"] and not args.no_mask:
            mask_path = resolve_asset(entry["maskfile"], (".core", ".MSK", ".msk"))
            if mask_path:
                msk = parse_msk(open(mask_path, "rb").read())
                row["mask"] = os.path.relpath(mask_path, ROOT).replace(os.sep, "/")
                placements = len(msk.placements)
                cache = {}
                first_frame = {}
                for pl in msk.placements:
                    if pl.flag != 1:
                        first_frame[pl.group] = min(
                            first_frame.get(pl.group, 99), pl.frame)
                for pl in msk.placements:
                    if pl.flag == 1:
                        fill, edge = STATIC_FILL, STATIC_EDGE
                        static_n += 1
                    else:
                        if args.groups == "none":
                            continue
                        if args.groups == "first" and pl.frame != first_frame[pl.group]:
                            continue
                        fill, edge = GROUP_FILL, GROUP_EDGE
                        group_n += 1
                    if pl.record not in cache:
                        arr = decode_mask(msk, pl.record)
                        cache[pl.record] = (
                            Image.fromarray(arr * 255),
                            Image.fromarray(edge_mask(arr)))
                    fill_img, edge_img = cache[pl.record]
                    paste_mask(overlay, fill_img, pl.x, pl.y, fill)
                    if not args.no_outline:
                        paste_mask(overlay, edge_img, pl.x, pl.y, edge)
            else:
                row["warning"] = (row["warning"] + "; " if row["warning"]
                                  else "") + "mask not found"

        result = Image.alpha_composite(bg, overlay).convert("RGB")
        out_name = entry["name"]
        if out_name in used_names:
            out_name = f"{out_name}_{used_names[entry['name']] + 1}"
        used_names[entry["name"]] = used_names.get(entry["name"], 0) + 1
        ext = ".png" if args.format == "png" else ".jpg"
        out_path = os.path.join(args.out, out_name + ext)
        os.makedirs(args.out, exist_ok=True)
        if os.path.exists(out_path) and not args.overwrite:
            row["warning"] = (row["warning"] + "; " if row["warning"]
                              else "") + "exists, skipped"
        else:
            if args.format == "png":
                result.save(out_path, "PNG", compress_level=6)
            else:
                result.save(out_path, "JPEG", quality=args.quality,
                            optimize=False)
        row["placements"] = placements
        row["static"] = static_n
        row["groups"] = group_n
        row["output"] = os.path.relpath(out_path, ROOT).replace(os.sep, "/")
        row["bytes"] = os.path.getsize(out_path) if os.path.exists(out_path) else 0
        row["elapsed_s"] = round(time.time() - t0, 2)
        return row
    except Exception as exc:  # noqa: BLE001
        row["error"] = f"{type(exc).__name__}: {exc}"
        row["elapsed_s"] = round(time.time() - t0, 2)
        return row


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--map", action="append", default=None,
                    help="map stem to render (repeatable)")
    ap.add_argument("--sample", type=int, default=0)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--tag", default="b0")
    ap.add_argument("--out", default=None)
    ap.add_argument("--manifest", default=None)
    ap.add_argument("--format", choices=("jpg", "png"), default="jpg")
    ap.add_argument("--quality", type=int, default=85)
    ap.add_argument("--no-mask", action="store_true",
                    help="omit cover-mask layer (clean background)")
    ap.add_argument("--no-outline", action="store_true",
                    help="omit the 2 px mask outline")
    ap.add_argument("--groups", choices=("none", "first", "all"), default="first",
                    help="group placements to draw (default: first frame)")
    ap.add_argument("--cel", action="store_true",
                    help="draw CEL overlay (diagnostic; layer semantics unverified)")
    ap.add_argument("--cel-layer", type=int, choices=(0, 1), default=1,
                    help="1=first bitmap, 0=second (when flags&1)")
    ap.add_argument("--cel-invert", action="store_true")
    ap.add_argument("--overwrite", action="store_true")
    ap.add_argument("--allow-missing", action="store_true",
                    help="skip maps whose background is absent instead of erroring")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if not (args.map or args.sample or args.limit or args.all):
        ap.error("refusing a full run without --sample/--limit/--all/--map")

    entries = select_maps(args)
    if not entries:
        print("no maps selected")
        return 2
    if not args.out:
        args.out = os.path.join(
            ROOT, "extracted", "map_preview" if args.all
            else os.path.join("_batches", f"map-preview-{args.tag}"))
    if not args.manifest:
        args.manifest = os.path.join(
            ROOT, "manifests", "batches",
            f"map-preview-{'full' if args.all else args.tag}.csv")
    if not os.path.isabs(args.out):
        args.out = os.path.join(ROOT, args.out)
    if not os.path.isabs(args.manifest):
        args.manifest = os.path.join(ROOT, args.manifest)

    print(f"selected {len(entries)} map(s); out={os.path.relpath(args.out, ROOT)}")
    print(f"manifest={os.path.relpath(args.manifest, ROOT)}")
    if args.dry_run:
        for e in entries:
            print(f"  {e['name']}  {e['width']}x{e['height']}  "
                  f"bg={e['background']}  mask={e['maskfile']}")
        return 0

    rows = []
    used_names = {}
    t0 = time.time()
    for i, entry in enumerate(entries, 1):
        row = render_one(entry, args, used_names)
        rows.append(row)
        status = ("ERROR " + row["error"] if row["error"]
                  else ("skipped" if row["warning"].startswith("background missing")
                        else "ok"))
        print(f"[{i}/{len(entries)}] {entry['name']}: {status} "
              f"{row['elapsed_s']}s {row['warning']}")
    os.makedirs(os.path.dirname(args.manifest), exist_ok=True)
    with open(args.manifest, "w", newline="", encoding="utf-8-sig") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    errors = sum(1 for r in rows if r["error"])
    print(f"done: {len(rows)} maps, {errors} errors, "
          f"{time.time() - t0:.1f}s; manifest={args.manifest}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
