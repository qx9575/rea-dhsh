#!/usr/bin/env python
"""Index non-PAK resources under `shdata/` (read-only).

Categories:
  sound/         435 WAV + 21 MP3 (RIFF fmt / MP3 frame headers)
  font/          TTF/TTC + fontinfo.ini (fontTools name tables)
  animation/     3 APG + 3 PIF + 1 MP3 (paired by stem)
  update/skin/   BMP (including `.bmp.rNNNN` variants) + JPG

`save/` is the player's mutable local config and is intentionally not
part of this resource inventory.

Every file is hashed (SHA-256); duplicate content is marked with
`duplicate_of`.  Writes a JSON inventory and a flat CSV manifest; never
modifies the source tree and never runs the game.

Usage:
    python tools/scan_nonpak.py --dry-run --limit 10
    python tools/scan_nonpak.py \
        --out manifests/batches/nonpak-index.json \
        --csv manifests/batches/nonpak-index.csv
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

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from dhsh.formats.map import parse_map_ini  # noqa: E402
from dhsh.formats.apg import parse_ag  # noqa: E402
from dhsh.formats.pif import parse_pif  # noqa: E402

DEFAULT_SOURCE = r"D:\software\dhsh\shdata"
CATEGORIES = (
    ("sound", "sound"),
    ("font", "font"),
    ("animation", "animation"),
    ("update_skin", os.path.join("update", "skin")),
)

WAV_CODECS = {1: "pcm", 2: "ima_adpcm", 3: "pcm_float", 0xFFFE: "extensible"}
MP3_BITRATES = {
    # version bits: 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5; layer III only
    3: [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320],
    2: [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
}
MP3_RATES = {3: [44100, 48000, 32000],
             2: [22050, 24000, 16000],
             0: [11025, 12000, 8000]}


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def wav_info(data: bytes) -> dict:
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return {"error": "not RIFF/WAVE"}
    i = 12
    fmt = None
    data_size = None
    while i + 8 <= len(data):
        cid = data[i:i + 4]
        size = struct.unpack_from("<I", data, i + 4)[0]
        body = data[i + 8:i + 8 + size]
        if cid == b"fmt " and len(body) >= 16:
            tag, channels, rate, byte_rate, block_align, bits = \
                struct.unpack_from("<HHIIHH", body)
            fmt = (tag, channels, rate, byte_rate, block_align, bits)
        elif cid == b"data":
            data_size = size
            break
        i += 8 + size + (size & 1)
    if fmt is None:
        return {"error": "no fmt chunk"}
    tag, channels, rate, byte_rate, block_align, bits = fmt
    meta = {
        "codec": WAV_CODECS.get(tag, f"tag{tag}"),
        "channels": channels,
        "sample_rate": rate,
        "bits": bits,
        "byte_rate": byte_rate,
    }
    if data_size is not None and byte_rate:
        meta["duration_s"] = round(data_size / byte_rate, 3)
    return meta


def _mp3_header(data: bytes, pos: int):
    if pos + 4 > len(data) or data[pos] != 0xFF or (data[pos + 1] & 0xE0) != 0xE0:
        return None
    b1, b2, b3 = data[pos + 1], data[pos + 2], data[pos + 3]
    version = (b1 >> 3) & 3       # 3=MPEG1, 2=MPEG2, 0=MPEG2.5
    layer = (b1 >> 1) & 3         # 1=Layer III
    if version == 1 or layer != 1:
        return None
    bitrate_idx = (b2 >> 4) & 0xF
    rate_idx = (b2 >> 2) & 3
    if bitrate_idx in (0, 15) or rate_idx == 3:
        return None
    bitrate = MP3_BITRATES[version][bitrate_idx] * 1000
    rate = MP3_RATES[version][rate_idx]
    padding = (b2 >> 1) & 1
    channels = 1 if ((b3 >> 6) & 3) == 3 else 2
    samples = 1152 if version == 3 else 576
    length = (144 if version == 3 else 72) * bitrate // rate + padding
    return {"bitrate": bitrate, "rate": rate, "channels": channels,
            "samples": samples, "length": length}


def mp3_info(data: bytes) -> dict:
    pos = 0
    if data[:3] == b"ID3" and len(data) >= 10:
        size = ((data[6] & 0x7F) << 21 | (data[7] & 0x7F) << 14 |
                (data[8] & 0x7F) << 7 | (data[9] & 0x7F))
        pos = 10 + size
    # find the first valid frame
    while pos + 4 <= len(data) and _mp3_header(data, pos) is None:
        pos += 1
    frames = 0
    samples = 0
    rates = set()
    bitrates = set()
    channels = set()
    while pos + 4 <= len(data):
        hdr = _mp3_header(data, pos)
        if hdr is None or hdr["length"] <= 0:
            break
        frames += 1
        samples += hdr["samples"]
        rates.add(hdr["rate"])
        bitrates.add(hdr["bitrate"])
        channels.add(hdr["channels"])
        pos += hdr["length"]
    meta = {"codec": "mpeg1-layer3", "frames": frames}
    if rates and samples:
        rate = max(rates)
        meta.update({
            "sample_rate": rate,
            "channels": min(channels) if channels else None,
            "duration_s": round(samples / rate, 3),
            "bitrate_kbps": (round(sum(bitrates) / len(bitrates) / 1000)
                             if len(bitrates) == 1
                             else "vbr"),
        })
    else:
        meta["error"] = "no MPEG layer III frames"
    return meta


def bmp_info(data: bytes):
    if len(data) < 30 or data[:2] != b"BM":
        return None
    width, height = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0] if len(data) >= 34 else 0
    return {"format": "bmp", "width": abs(width), "height": abs(height),
            "bpp": bpp, "compression": compression,
            "bottom_up": height > 0}


def image_info(path: str, data: bytes):
    bmp = bmp_info(data)
    if bmp:
        return bmp
    try:
        from PIL import Image
        with Image.open(path) as img:
            return {"format": (img.format or "").lower(),
                    "width": img.width, "height": img.height}
    except Exception:  # noqa: BLE001
        return {"format": "unknown"}


def font_info(path: str) -> dict:
    try:
        from fontTools.ttLib import TTCollection, TTFont
    except ImportError:
        return {"error": "fontTools not installed"}
    ext = os.path.splitext(path)[1].lower()
    faces = []
    try:
        if ext == ".ttc":
            collection = TTCollection(path, lazy=True)
            fonts = collection.fonts
        else:
            fonts = [TTFont(path, lazy=True)]
        for font in fonts:
            name = font["name"] if "name" in font else None
            sfnt = font.sfntVersion
            if isinstance(sfnt, str):
                sfnt = sfnt.encode("latin1", "replace").hex()
            faces.append({
                "family": name.getDebugName(1) if name else None,
                "subfamily": name.getDebugName(2) if name else None,
                "full_name": name.getDebugName(4) if name else None,
                "postscript": name.getDebugName(6) if name else None,
                "sfnt_version": sfnt,
                "glyphs": font["maxp"].numGlyphs if "maxp" in font else None,
            })
            if ext != ".ttc":
                font.close()
        if ext == ".ttc":
            for font in fonts:
                font.close()
    except Exception as exc:  # noqa: BLE001
        return {"error": f"{type(exc).__name__}: {exc}", "faces": faces}
    return {"faces": faces, "face_count": len(faces)}


def fontinfo_info(path: str) -> dict:
    try:
        ini = parse_map_ini(open(path, "rb").read())
    except Exception as exc:  # noqa: BLE001
        return {"error": f"{type(exc).__name__}: {exc}"}
    return {"sections": ini}


def collect(source: str):
    items = []
    for category, rel in CATEGORIES:
        root = os.path.join(source, rel)
        if not os.path.isdir(root):
            continue
        for dirpath, _dirnames, filenames in os.walk(root):
            for name in sorted(filenames):
                path = os.path.join(dirpath, name)
                items.append((category, path, os.path.relpath(path, source)))
    return items


def inspect(category: str, path: str, rel: str) -> dict:
    data = open(path, "rb").read()
    ext = os.path.splitext(path)[1].lower()
    # `.bmp.r1571` style suffixes: sniff the magic, keep the real suffix.
    meta = {}
    kind = ext.lstrip(".") or "none"
    if category == "sound":
        meta = wav_info(data) if ext == ".wav" else mp3_info(data)
        kind = "audio"
    elif category == "font":
        if ext in (".ttf", ".otf", ".ttc"):
            meta = font_info(path)
            kind = "font"
        elif ext == ".ini":
            meta = fontinfo_info(path)
            kind = "config"
    elif category == "animation":
        if data[:2] == b"AG":
            ag = parse_ag(data)
            meta = {"format": "apg", "frames": ag.frame_count,
                    "flags": ag.flags}
            kind = "animation"
        elif data[:4] == b"PIF\0":
            pif = parse_pif(data)
            meta = {"format": "pif", "count": pif.count,
                    "width": pif.width, "height": pif.height,
                    "flags": pif.flags}
            kind = "pif"
        elif data[:3] == b"ID3" or data[:1] == b"\xff":
            meta = mp3_info(data)
            kind = "audio"
        else:
            meta = {"format": "unknown"}
    elif category == "update_skin":
        if data[:2] == b"BM":
            meta = bmp_info(data) or {"format": "bmp"}
        else:
            meta = image_info(path, data)
        kind = "image"
    return {
        "category": category,
        "path": rel.replace(os.sep, "/"),
        "ext": ext,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "kind": kind,
        "meta": meta,
    }


def flatten(item: dict) -> dict:
    meta = item["meta"]

    def first_face(key):
        faces = meta.get("faces") or []
        return faces[0].get(key) if faces else ""

    return {
        "category": item["category"],
        "path": item["path"],
        "ext": item["ext"],
        "size": item["size"],
        "sha256": item["sha256"],
        "kind": item["kind"],
        "width": meta.get("width", ""),
        "height": meta.get("height", ""),
        "bpp": meta.get("bpp", ""),
        "codec": meta.get("codec", meta.get("format", "")),
        "channels": meta.get("channels", ""),
        "sample_rate": meta.get("sample_rate", ""),
        "bits": meta.get("bits", ""),
        "bitrate_kbps": meta.get("bitrate_kbps", ""),
        "duration_s": meta.get("duration_s", ""),
        "frames": meta.get("frames", meta.get("frames", meta.get("count", ""))),
        "font_family": first_face("family"),
        "font_subfamily": first_face("subfamily"),
        "duplicate_of": item.get("duplicate_of", ""),
        "error": meta.get("error", ""),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", default=DEFAULT_SOURCE)
    ap.add_argument("--out", default=os.path.join(
        ROOT, "manifests", "batches", "nonpak-index.json"))
    ap.add_argument("--csv", default=os.path.join(
        ROOT, "manifests", "batches", "nonpak-index.csv"))
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if not os.path.isdir(args.source):
        print("missing source", args.source)
        return 2
    started = time.time()
    entries = collect(args.source)
    if args.limit:
        entries = entries[:args.limit]
    print(f"found {len(entries)} non-PAK files under {args.source}")
    if args.dry_run:
        for category, path, rel in entries:
            print(f"  {category:12s} {rel}")
        return 0

    seen = {}
    items = []
    errors = []
    for category, path, rel in entries:
        try:
            item = inspect(category, path, rel)
            first = seen.get(item["sha256"])
            if first:
                item["duplicate_of"] = first
            else:
                seen[item["sha256"]] = item["path"]
            items.append(item)
        except Exception as exc:  # noqa: BLE001
            errors.append({"path": rel.replace(os.sep, "/"),
                           "error": f"{type(exc).__name__}: {exc}"})

    categories = {}
    for category, _rel in CATEGORIES:
        subset = [i for i in items if i["category"] == category]
        if not subset:
            continue
        categories[category] = {
            "files": len(subset),
            "bytes": sum(i["size"] for i in subset),
            "items": subset,
        }
    report = {
        "tool": "tools/scan_nonpak.py",
        "source": args.source,
        "categories": categories,
        "totals": {
            "files": len(items),
            "bytes": sum(i["size"] for i in items),
            "duplicates": sum(1 for i in items if i.get("duplicate_of")),
            "errors": len(errors),
        },
        "errors": errors,
        "elapsed_s": round(time.time() - started, 2),
    }
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=2)
    if args.csv:
        os.makedirs(os.path.dirname(args.csv), exist_ok=True)
        fields = list(flatten(items[0]).keys()) if items else []
        with open(args.csv, "w", newline="", encoding="utf-8-sig") as fh:
            writer = csv.DictWriter(fh, fieldnames=fields)
            writer.writeheader()
            for item in items:
                writer.writerow(flatten(item))
    for category, info in categories.items():
        print(f"  {category:12s} {info['files']:4d} files "
              f"{info['bytes'] / 1048576:8.2f} MiB")
    print(f"done: {len(items)} files, "
          f"{report['totals']['duplicates']} duplicates, "
          f"{len(errors)} errors; manifest={args.out}")
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
