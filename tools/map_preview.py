#!/usr/bin/env python
"""Compose per-map preview images from decoded resources.

Layers (in order):
  1. background  -> extracted/special_jpg_png/<BACKGROUND with .png>
  2. CEL (optional diagnostic, --cel) -> cell bitmap scaled to map pixels
  3. MSK/core cover masks -> placed at placement (x, y)

The cover-mask layer draws static placements (file flag=1) in magenta and,
by default, the first frame of each group (flag=0) in orange.  The CEL
overlay is off by default: layer 1 (first bitmap) is the runtime-confirmed
ground collision layer; layer 0 is the auxiliary second bitmap.  CEL is
drawn only when its grid matches the background canvas.

Outputs one image per map and a CSV manifest.  Never runs the game.

Performance: backgrounds and outputs use OpenCV when available (fast PNG
decode / JPEG encode); `--jobs N` renders maps in parallel (default 6).
The cover masks are painted into single-channel label planes and blended
once per color, instead of per-mask RGBA composites.

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

try:
    import cv2  # noqa: E402
except ImportError:  # Pillow fallback keeps the tool usable without OpenCV
    cv2 = None

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


def resolve_background(rel: str):
    """Return (BGR uint8 array, path) for the map background."""
    stem = os.path.splitext(norm(rel))[0]
    candidates = [os.path.join(ROOT, d, stem + ".png") for d in BG_DIRS]
    candidates += [os.path.join(ROOT, "extracted", "images-standard", norm(rel))]
    for path in candidates:
        if not os.path.exists(path):
            continue
        try:
            if cv2 is not None:
                arr = cv2.imdecode(np.fromfile(path, dtype=np.uint8),
                                   cv2.IMREAD_COLOR)
                if arr is not None:
                    return arr, path
            else:
                rgb = np.array(Image.open(path).convert("RGB"))
                return rgb[:, :, ::-1].copy(), path
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
    # layer 1 = first bitmap (ground; parser param_5 != 0)
    # layer 0 = second bitmap (auxiliary; parser param_5 == 0)
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


def make_lut(color, alpha: int) -> np.ndarray:
    """Per-channel lookup table for a BGR color at alpha/255."""
    a = alpha / 255.0
    base = np.arange(256, dtype=np.float32)[:, None]
    bgr = np.asarray(color[:3], dtype=np.float32)[::-1][None, :]
    return np.clip(base * (1.0 - a) + bgr * a + 0.5, 0, 255).astype(np.uint8)


def flat_lut(color, alpha: int) -> np.ndarray:
    """Channel-major flat LUT: indices 0..255=R, 256..511=G, 512..767=B."""
    lut = make_lut(color, alpha)
    return np.concatenate((lut[:, 0], lut[:, 1], lut[:, 2]))


LUT_STATIC_FILL = flat_lut(STATIC_FILL, STATIC_FILL[3])
LUT_STATIC_EDGE = flat_lut(STATIC_EDGE, STATIC_EDGE[3])
LUT_GROUP_FILL = flat_lut(GROUP_FILL, GROUP_FILL[3])
LUT_GROUP_EDGE = flat_lut(GROUP_EDGE, GROUP_EDGE[3])
LUT_CEL = flat_lut(CEL_COLOR, CEL_COLOR[3])
LUT_OFFSETS = np.array([0, 256, 512], dtype=np.int32)


def paint_label(lbl: np.ndarray, mask: np.ndarray, x: int, y: int,
                value: int) -> None:
    """Paint a binary mask into a uint8 label plane (one channel)."""
    h, w = mask.shape
    H, W = lbl.shape
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(W, x + w), min(H, y + h)
    if x1 <= x0 or y1 <= y0:
        return
    m = mask[y0 - y:y1 - y, x0 - x:x1 - x]
    if m.dtype != bool:
        m = m != 0
    if m.any():
        lbl[y0:y1, x0:x1][m] = value


def apply_labels(arr: np.ndarray, lbl: np.ndarray,
                 lut_a: np.ndarray, lut_b: np.ndarray) -> None:
    """Blend two label classes into the RGB array in one pass each."""
    for value, lut in ((1, lut_a), (2, lut_b)):
        m = lbl == value
        if m.any():
            vals = arr[m]
            arr[m] = lut[vals + LUT_OFFSETS]


def blend_into(arr: np.ndarray, mask: np.ndarray, x: int, y: int,
               lut: np.ndarray) -> None:
    """Blend one binary mask into an RGB uint8 array at (x, y), clipped."""
    h, w = mask.shape
    H, W = arr.shape[:2]
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(W, x + w), min(H, y + h)
    if x1 <= x0 or y1 <= y0:
        return
    m = mask[y0 - y:y1 - y, x0 - x:x1 - x]
    if m.dtype != bool:
        m = m != 0
    if not m.any():
        return
    sub = arr[y0:y1, x0:x1]
    vals = sub[m]
    sub[m] = lut[vals + LUT_OFFSETS]


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


def render_one(entry, args):
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
        height, width = bg.shape[:2]
        row["bg_size"] = f"{width}x{height}"
        # The placement table and the background share the pixel space; INI
        # WIDTH/HEIGHT can describe only a playable sub-area.  Keep the
        # background at native size so masks stay aligned.
        row["width"], row["height"] = width, height
        if entry["width"]:
            ini_w, ini_h = int(entry["width"]), int(entry["height"])
            row["ini_size"] = f"{ini_w}x{ini_h}"
            if (ini_w, ini_h) != (width, height):
                row["warning"] = (
                    f"INI {ini_w}x{ini_h} != background {width}x{height}; "
                    "using background")
        arr = bg

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
            else:
                ci = cel_image(cel_data, args.cel_layer, args.cel_invert)
                fw, fh = ci.size
                map_w = int(entry["width"]) if entry["width"] else width
                map_h = int(entry["height"]) if entry["height"] else height
                map_cw, map_ch = map_w // 16, map_h // 16
                # Map origin in the background: zero when the map fills the
                # canvas; otherwise use the non-black content bbox
                # (TingYuan_*: 896x640 content at background 0,0 with a
                # 128 px black right/bottom margin).
                if (map_w, map_h) == (width, height):
                    off_x = off_y = 0
                else:
                    black = arr.max(axis=2) < 10
                    if black.any() and not black.all():
                        ys, xs = np.nonzero(~black)
                        off_x, off_y = int(xs.min()), int(ys.min())
                    else:
                        off_x = off_y = 0
                    if (off_x, off_y) != (0, 0):
                        row["warning"] = (
                            row["warning"] + "; " if row["warning"] else "") + (
                            f"map origin at background ({off_x},{off_y})")
                # Loader behaviour: keep the map cell grid, copy the CEL
                # top-left and pad missing right/bottom cells with 0xff.
                # CEL masks are cell-resolution; scale 16x before pasting.
                cw_use = min(fw, map_cw)
                ch_use = min(fh, map_ch)
                ci_px = ci.resize((fw * 16, fh * 16), Image.NEAREST)
                cm = np.full((map_ch * 16, map_cw * 16), 255, dtype=np.uint8)
                cm[:ch_use * 16, :cw_use * 16] = np.asarray(ci_px)[
                    :ch_use * 16, :cw_use * 16]
                if (fw, fh) != (map_cw, map_ch):
                    kind = ("padded" if fw <= map_cw and fh <= map_ch
                            else "cropped")
                    row["warning"] = (
                        row["warning"] + "; " if row["warning"] else "") + (
                        f"CEL {fw}x{fh} {kind} top-left to "
                        f"{map_cw}x{map_ch}")
                blocked = int((cm != 0).sum())
                row["cel_layer"] = str(args.cel_layer)
                row["cel_blocked_pct"] = round(
                    blocked / cm.size * 100, 2)
                blend_into(arr, cm, off_x, off_y, LUT_CEL)

        placements = static_n = group_n = 0
        if entry["maskfile"] and not args.no_mask:
            mask_path = resolve_asset(entry["maskfile"], (".core", ".MSK", ".msk"))
            if mask_path:
                msk = parse_msk(open(mask_path, "rb").read())
                row["mask"] = os.path.relpath(mask_path, ROOT).replace(os.sep, "/")
                placements = len(msk.placements)
                cache = {}
                first_frame = {}
                fill_lbl = np.zeros((height, width), dtype=np.uint8)
                edge_lbl = np.zeros((height, width), dtype=np.uint8)
                for pl in msk.placements:
                    if pl.flag != 1:
                        first_frame[pl.group] = min(
                            first_frame.get(pl.group, 99), pl.frame)
                for pl in msk.placements:
                    if pl.flag == 1:
                        label = 1
                        static_n += 1
                    else:
                        if args.groups == "none":
                            continue
                        if args.groups == "first" and pl.frame != first_frame[pl.group]:
                            continue
                        label = 2
                        group_n += 1
                    if pl.record not in cache:
                        m = decode_mask(msk, pl.record)
                        cache[pl.record] = (m, edge_mask(m))
                    m, e = cache[pl.record]
                    paint_label(fill_lbl, m, pl.x, pl.y, label)
                    if not args.no_outline:
                        paint_label(edge_lbl, e, pl.x, pl.y, label)
                apply_labels(arr, fill_lbl, LUT_STATIC_FILL, LUT_GROUP_FILL)
                if not args.no_outline:
                    apply_labels(arr, edge_lbl, LUT_STATIC_EDGE,
                                 LUT_GROUP_EDGE)
            else:
                row["warning"] = (row["warning"] + "; " if row["warning"]
                                  else "") + "mask not found"

        out_name = entry.get("out_name") or entry["name"]
        ext = ".png" if args.format == "png" else ".jpg"
        out_path = os.path.join(args.out, out_name + ext)
        if os.path.exists(out_path) and not args.overwrite:
            row["warning"] = (row["warning"] + "; " if row["warning"]
                              else "") + "exists, skipped"
        else:
            if cv2 is not None:
                if args.format == "png":
                    ok, buf = cv2.imencode(
                        ".png", arr, [cv2.IMWRITE_PNG_COMPRESSION, 6])
                else:
                    ok, buf = cv2.imencode(
                        ".jpg", arr, [cv2.IMWRITE_JPEG_QUALITY, args.quality])
                if not ok:
                    raise RuntimeError(f"imencode failed: {out_path}")
                buf.tofile(out_path)
            else:
                result = Image.fromarray(
                    np.ascontiguousarray(arr[:, :, ::-1]), "RGB")
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


def _render_task(task):
    idx, entry, args = task
    return idx, render_one(entry, args)


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
                    help="draw CEL ground layer (diagnostic)")
    ap.add_argument("--cel-layer", type=int, choices=(0, 1), default=1,
                    help="1=ground (first bitmap), 0=auxiliary (second)")
    ap.add_argument("--cel-invert", action="store_true")
    ap.add_argument("--overwrite", action="store_true")
    ap.add_argument("--allow-missing", action="store_true",
                    help="skip maps whose background is absent instead of erroring")
    ap.add_argument("--jobs", type=int, default=6,
                    help="parallel map workers (default 6; 1 = sequential)")
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

    seen = {}
    for entry in entries:
        base = entry["name"]
        n = seen.get(base, 0)
        seen[base] = n + 1
        entry["out_name"] = base if n == 0 else f"{base}_{n + 1}"

    print(f"selected {len(entries)} map(s); out={os.path.relpath(args.out, ROOT)}")
    print(f"manifest={os.path.relpath(args.manifest, ROOT)} jobs={args.jobs}")
    if args.dry_run:
        for e in entries:
            print(f"  {e['name']}  {e['width']}x{e['height']}  "
                  f"bg={e['background']}  mask={e['maskfile']}")
        return 0

    os.makedirs(args.out, exist_ok=True)
    t0 = time.time()
    tasks = [(i, entry, args) for i, entry in enumerate(entries)]
    results = [None] * len(entries)
    if args.jobs > 1 and len(entries) > 1:
        from multiprocessing import Pool
        with Pool(processes=min(args.jobs, len(entries))) as pool:
            for done, (idx, row) in enumerate(
                    pool.imap_unordered(_render_task, tasks), 1):
                results[idx] = row
                status = ("ERROR " + row["error"] if row["error"]
                          else ("skipped" if row["warning"].startswith(
                              "background missing") else "ok"))
                print(f"[{done}/{len(entries)}] {row['name']}: {status} "
                      f"{row['elapsed_s']}s {row['warning']}")
    else:
        for done, (idx, row) in enumerate(
                ((i, render_one(e, args)) for i, e in enumerate(entries)), 1):
            results[idx] = row
            status = ("ERROR " + row["error"] if row["error"]
                      else ("skipped" if row["warning"].startswith(
                          "background missing") else "ok"))
            print(f"[{done}/{len(entries)}] {row['name']}: {status} "
                  f"{row['elapsed_s']}s {row['warning']}")
    rows = [r for r in results if r is not None]
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
