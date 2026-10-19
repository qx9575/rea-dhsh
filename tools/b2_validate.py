"""B2 validation: 1,000 A16 images + 1,000 NZ 0x0B leaves.

Does not perform a full export. Writes a JSON report and a small PNG sample.
"""
from __future__ import annotations

import glob
import json
import os
import random
import struct
import sys
import time
from collections import Counter

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "tools"))
from dhsh.formats.a16 import decode_a16, rgb565_to_rgba  # noqa: E402
from dhsh.nz_lzo import lzo1x_decompress  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = r"D:\software\dhsh\shdata"
OUT = os.path.join(ROOT, "extracted", "_batches", "s3-b2")
REPORT = os.path.join(ROOT, "manifests", "batches", "b2-validation.json")


def checker(size, step=8):
    im = Image.new("RGBA", size, (200, 200, 200, 255))
    d = ImageDraw.Draw(im)
    for y in range(0, size[1], step):
        for x in range(0, size[0], step):
            if ((x // step) + (y // step)) % 2:
                d.rectangle([x, y, x + step - 1, y + step - 1], fill=(245, 245, 245, 255))
    return im


def contact(items, out, cols=4):
    thumbs = []
    for label, im in items:
        im = im.convert("RGBA")
        scale = min(180 / max(im.width, 1), 180 / max(im.height, 1))
        im = im.resize((max(1, int(im.width * scale)), max(1, int(im.height * scale))), Image.NEAREST)
        bg = checker((200, 220))
        bg.paste(im, ((200 - im.width) // 2, 22), im)
        d = ImageDraw.Draw(bg)
        d.rectangle([0, 0, 199, 19], fill=(255, 255, 255, 255))
        d.text((3, 3), label[:30], fill=(0, 0, 0, 255))
        thumbs.append(bg)
    rows = (len(thumbs) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * 200, rows * 220), (235, 235, 235, 255))
    for i, t in enumerate(thumbs):
        sheet.paste(t, ((i % cols) * 200, (i // cols) * 220))
    sheet.save(out)


def validate_a16(limit=1000):
    files = glob.glob(os.path.join(ROOT, "extracted", "images-proprietary", "**", "*.a16"), recursive=True)
    random.seed(20261019)
    random.shuffle(files)
    sample = files[:limit]
    ok = 0
    failed = []
    comp = Counter()
    alpha = Counter()
    pngs = []
    for p in sample:
        try:
            img = decode_a16(open(p, "rb").read())
            comp[img.header.compression] += 1
            alpha[img.header.alpha_mode] += 1
            if len(img.pixels) != img.header.width * img.header.height:
                raise ValueError("pixel count")
            if img.header.alpha_mode == 2:
                if img.alpha is None or len(img.alpha) != img.header.width * img.header.height:
                    raise ValueError("alpha length")
            ok += 1
            if len(pngs) < 20 and img.header.width <= 160 and img.header.height <= 160:
                rgba = rgb565_to_rgba(img.pixels, img.alpha)
                pngs.append((os.path.basename(p), Image.frombytes("RGBA", (img.header.width, img.header.height), rgba)))
        except Exception as exc:  # noqa: BLE001
            failed.append({"path": p, "error": f"{type(exc).__name__}: {exc}"})
    os.makedirs(OUT, exist_ok=True)
    if pngs:
        contact(pngs, os.path.join(OUT, "a16_contact_b2.png"))
    return {
        "sample": len(sample),
        "decoded": ok,
        "failed": len(failed),
        "compression": {str(k): v for k, v in comp.items()},
        "alpha_mode": {str(k): v for k, v in alpha.items()},
        "failures": failed[:20],
        "contact_sheet": os.path.join(OUT, "a16_contact_b2.png") if pngs else None,
    }


def validate_nz0b(limit=1000, per_pak_cap=250):
    inv = json.load(open(os.path.join(ROOT, "pak_inventory.json"), encoding="utf-8"))
    random.seed(20261020)
    candidates = []
    for pak, entries in inv.items():
        if pak == "perinfo.pak":
            continue
        taken = 0
        with open(os.path.join(SRC, pak), "rb") as f:
            for e in entries:
                if taken >= per_pak_cap:
                    break
                if e["name"] == "UNREADABLE" or e["size"] < 12:
                    continue
                f.seek(e["offset"])
                h = f.read(12)
                if h[:2] == b"NZ":
                    kind, s1, s2 = struct.unpack_from("<HII", h, 2)
                    if kind == 0x0B:
                        candidates.append((pak, e["path"], e["offset"], s1, s2))
                        taken += 1
    random.shuffle(candidates)
    sample = candidates[:limit]
    ok = 0
    failed = []
    by_ext = Counter()
    by_ext_fail = Counter()
    a16_ok = 0
    a16_fail = 0
    for pak, path, offset, s1, s2 in sample:
        ext = os.path.splitext(path)[1].lower()
        by_ext[ext] += 1
        try:
            with open(os.path.join(SRC, pak), "rb") as f:
                f.seek(offset + 12)
                payload = f.read(s2)
            out = lzo1x_decompress(payload, s1)
            if len(out) != s1:
                raise ValueError(f"length {len(out)} != {s1}")
            if ext == ".a16":
                if not out.startswith(b"AP"):
                    raise ValueError("A16 magic")
                try:
                    decode_a16(out)
                    a16_ok += 1
                except Exception:
                    a16_fail += 1
            elif ext == ".apg":
                if not (out.startswith(b"AG") or out.startswith(b"A\x00G\x00")):
                    raise ValueError("APG magic")
            elif ext == ".cti":
                if not out.startswith(b"CT"):
                    raise ValueError("CTI magic")
            elif ext == ".bmp":
                if not out.startswith(b"BM"):
                    raise ValueError("BMP magic")
            elif ext == ".tga":
                if len(out) < 18:
                    raise ValueError("TGA short")
            ok += 1
        except Exception as exc:  # noqa: BLE001
            by_ext_fail[ext] += 1
            failed.append({"pak": pak, "path": path, "ext": ext, "error": f"{type(exc).__name__}: {exc}"})
    return {
        "candidates": len(candidates),
        "sample": len(sample),
        "decoded": ok,
        "failed": len(failed),
        "by_ext": {k: v for k, v in by_ext.items()},
        "by_ext_failed": {k: v for k, v in by_ext_fail.items()},
        "a16_nested_decode_ok": a16_ok,
        "a16_nested_decode_failed": a16_fail,
        "failures": failed[:20],
    }


def main():
    started = time.time()
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(os.path.dirname(REPORT), exist_ok=True)
    result = {
        "date": "2026-10-19",
        "a16": validate_a16(),
        "nz_0x0b": validate_nz0b(),
        "elapsed_seconds": None,
    }
    result["elapsed_seconds"] = round(time.time() - started, 1)
    with open(REPORT, "w", encoding="utf-8") as fh:
        json.dump(result, fh, ensure_ascii=False, indent=1)
    print(json.dumps(result, ensure_ascii=False, indent=1))
    print("report:", REPORT)


if __name__ == "__main__":
    main()
