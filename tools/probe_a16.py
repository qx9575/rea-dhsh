"""B0 probe: decode a small A16/CTI sample set (10-20 files) and validate.

Usage:
    python tools/probe_a16.py
"""
from __future__ import annotations

import glob
import math
import os
import random
import struct
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "tools"))
from dhsh.formats.a16 import decode_a16, parse_cti, rgb565_to_rgba  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
A16_GLOB = os.path.join(ROOT, "extracted", "images-proprietary", "**", "*.a16")
CTI_GLOB = os.path.join(ROOT, "extracted", "images-proprietary", "**", "*.cti")
OUT = os.path.join(ROOT, "extracted", "_batches", "s3-b0")


def pick_varied(paths: list[str], n: int) -> list[str]:
    """Pick n files spread across sizes, including small and large samples."""
    ordered = sorted(paths, key=os.path.getsize)
    if len(ordered) <= n:
        return ordered
    return [ordered[int(i * (len(ordered) - 1) / (n - 1))] for i in range(n)]


def checker(size: tuple[int, int], step: int = 8) -> Image.Image:
    im = Image.new("RGBA", size, (200, 200, 200, 255))
    d = ImageDraw.Draw(im)
    for y in range(0, size[1], step):
        for x in range(0, size[0], step):
            if ((x // step) + (y // step)) % 2:
                d.rectangle([x, y, x + step - 1, y + step - 1], fill=(245, 245, 245, 255))
    return im


def make_contact(images: list[tuple[str, Image.Image]], out: str, cols: int = 4) -> None:
    thumbs = []
    for label, im in images:
        im = im.convert("RGBA")
        scale = min(200 / max(im.width, 1), 200 / max(im.height, 1))
        im = im.resize((max(1, int(im.width * scale)), max(1, int(im.height * scale))), Image.NEAREST)
        bg = checker((220, 240))
        bg.paste(im, ((220 - im.width) // 2, 24), im)
        d = ImageDraw.Draw(bg)
        d.rectangle([0, 0, 219, 20], fill=(255, 255, 255, 255))
        d.rectangle([0, 221, 219, 239], fill=(255, 255, 255, 255))
        d.text((4, 4), label[:32], fill=(0, 0, 0, 255))
        d.text((4, 222), f"{im.width}x{im.height}", fill=(0, 0, 0, 255))
        thumbs.append(bg)
    rows = math.ceil(len(thumbs) / cols)
    sheet = Image.new("RGBA", (cols * 220, rows * 240), (235, 235, 235, 255))
    for i, t in enumerate(thumbs):
        sheet.paste(t, ((i % cols) * 220, (i // cols) * 240))
    sheet.save(out)
    print("contact sheet:", out)


def a16_probe() -> None:
    paths = sorted(glob.glob(A16_GLOB, recursive=True))
    random.seed(7)
    sample = pick_varied(paths, 20)
    # ensure the already-verified visual samples are included
    for name in ("wait.a16", "nan3-3.a16", "224.a16", "04502.a16"):
        for p in paths:
            if p.endswith(name):
                sample.append(p)
                break
    seen = set()
    sample = [p for p in sample if not (p in seen or seen.add(p))]
    outdir = os.path.join(OUT, "png")
    os.makedirs(outdir, exist_ok=True)
    images = []
    ok = 0
    unsupported = []
    failed = []
    for p in sample:
        data = open(p, "rb").read()
        try:
            img = decode_a16(data)
            rgba = rgb565_to_rgba(img.pixels, img.alpha)
            pil = Image.frombytes("RGBA", (img.header.width, img.header.height), rgba)
            name = os.path.basename(p) + ".png"
            pil.save(os.path.join(outdir, name))
            label = f'{os.path.basename(p)} c{img.header.compression} a{img.header.alpha_mode}'
            images.append((label, pil))
            ok += 1
        except NotImplementedError as exc:
            unsupported.append((p, str(exc)))
        except Exception as exc:  # noqa: BLE001 - probe reports everything
            failed.append((p, repr(exc)))
    make_contact(images, os.path.join(OUT, "a16_contact_b0.png"))
    print(f"A16 sample: {len(sample)} files, decoded {ok}, unsupported {len(unsupported)}, failed {len(failed)}")
    for p, why in unsupported:
        print("  unsupported:", p, why)
    for p, why in failed:
        print("  failed:", p, why)


def cti_probe() -> None:
    paths = sorted(glob.glob(CTI_GLOB, recursive=True))
    random.seed(11)
    sample = random.sample(paths, min(20, len(paths)))
    ok = 0
    bad = []
    swatches = []
    for p in sample:
        data = open(p, "rb").read()
        try:
            cti = parse_cti(data)
            expected = cti.header.header_size + cti.header.table1_size + cti.header.table2_size
            if expected != len(data):
                raise ValueError(f"size mismatch {len(data)} != {expected}")
            ok += 1
            if len(swatches) < 2:
                grid1 = Image.new("RGB", (16 * 12, 16 * 12), (0, 0, 0))
                grid2a = Image.new("RGB", (16 * 12, 16 * 12), (0, 0, 0))
                grid2b = Image.new("RGB", (16 * 12, 16 * 12), (0, 0, 0))
                for i in range(min(256, cti.header.count)):
                    c = cti.table1[i]
                    rgb = ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31
                    grid1.putpixel(((i % 16) * 12, (i // 16) * 12), rgb)
                    e = cti.table2[i]
                    # try ARGB and BGRA byte orders
                    a1, r1, g1, b1 = (e >> 24) & 255, (e >> 16) & 255, (e >> 8) & 255, e & 255
                    grid2a.putpixel(((i % 16) * 12, (i // 16) * 12), (r1, g1, b1))
                    b2, g2, r2, a2 = e & 255, (e >> 8) & 255, (e >> 16) & 255, (e >> 24) & 255
                    grid2b.putpixel(((i % 16) * 12, (i // 16) * 12), (r2, g2, b2))
                swatches.append((p, grid1, grid2a, grid2b))
        except Exception as exc:  # noqa: BLE001 - probe reports everything
            bad.append((p, repr(exc)))
    print(f"CTI sample: {len(sample)} files, validated {ok}, failed {len(bad)}")
    for p, why in bad:
        print("  failed:", p, why)
    if swatches:
        p, g1, g2a, g2b = swatches[0]
        sheet = Image.new("RGB", (3 * (16 * 12 + 10), 16 * 12 + 24), (30, 30, 30))
        sheet.paste(g1, (0, 20))
        sheet.paste(g2a, (16 * 12 + 10, 20))
        sheet.paste(g2b, (2 * (16 * 12 + 10), 20))
        sheet.save(os.path.join(OUT, "cti_swatches.png"))
        print("cti swatches:", os.path.join(OUT, "cti_swatches.png"), "from", p)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    a16_probe()
    cti_probe()
