"""B0 APG probe: parse AG headers and extract a few animation frames."""
from __future__ import annotations

import glob
import math
import os
import random
import sys
from collections import Counter

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "tools"))
from dhsh.formats.a16 import parse_cti  # noqa: E402
from dhsh.formats.apg import frame_to_rgba, iter_frames  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "extracted", "_batches", "s4-apg-b0")
APG_ROOT = os.path.join(ROOT, "extracted", "samples", "apg")
CTI_ROOT = os.path.join(ROOT, "extracted", "images-proprietary")


def find_cti(apg_path: str):
    """Walk up from the APG path until a sibling clrtb.cti exists.

    Extracted paths are <root>/<pak-stem>/<logical path>; CTI files live under
    images-proprietary/<pak-stem>/<logical dir>/clrtb.cti.
    """
    rel = os.path.relpath(apg_path, APG_ROOT)
    parts = rel.split(os.sep)
    if len(parts) < 2:
        return None
    pak = parts[0]
    logical = os.path.dirname(os.path.join(*parts[1:])) if len(parts) > 2 else ""
    d = logical
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


def checker(size, step=8):
    im = Image.new("RGBA", size, (200, 200, 200, 255))
    d = ImageDraw.Draw(im)
    for y in range(0, size[1], step):
        for x in range(0, size[0], step):
            if ((x // step) + (y // step)) % 2:
                d.rectangle([x, y, x + step - 1, y + step - 1], fill=(245, 245, 245, 255))
    return im


def cell(label, im, maxdim=120):
    scale = min(maxdim / max(im.width, 1), maxdim / max(im.height, 1))
    im = im.resize((max(1, int(im.width * scale)), max(1, int(im.height * scale))), Image.NEAREST)
    bg = checker((140, 160))
    bg.paste(im, ((140 - im.width) // 2, 20), im)
    d = ImageDraw.Draw(bg)
    d.rectangle([0, 0, 139, 18], fill=(255, 255, 255, 255))
    d.text((3, 3), label[:26], fill=(0, 0, 0, 255))
    return bg


def main():
    os.makedirs(OUT, exist_ok=True)
    files = sorted(glob.glob(os.path.join(ROOT, "extracted", "samples", "apg", "**", "*.apg"), recursive=True),
                   key=os.path.getsize)
    random.seed(99)
    picks = [files[int(i * (len(files) - 1) / 9)] for i in range(10)]
    # include standalone animations
    for p in ("1.apg", "2.apg", "3.apg"):
        full = os.path.join(r"D:\software\dhsh\shdata\animation", p)
        if os.path.exists(full):
            picks.append(full)
    rows = []
    total_frames = 0
    total_ok = 0
    all_comps = Counter()
    all_alphas = Counter()
    errors = []
    for p in picks:
        data = open(p, "rb").read()
        try:
            ag, frames = iter_frames(data)
        except Exception as exc:  # noqa: BLE001
            errors.append((p, f"{type(exc).__name__}: {exc}"))
            continue
        comps = Counter()
        alphas = Counter()
        magics = Counter()
        decoded = []
        palette = None
        cti_path = find_cti(p) if p.startswith(APG_ROOT) else None
        if cti_path:
            palette = parse_cti(open(cti_path, "rb").read())
        for fr in frames:
            comps[fr.header.compression] += 1
            alphas[fr.header.alpha_mode] += 1
            magics[fr.header.magic.decode("latin1")] += 1
            all_comps[fr.header.compression] += 1
            all_alphas[fr.header.alpha_mode] += 1
            total_frames += 1
            try:
                img = frame_to_rgba(fr, palette=palette)
                total_ok += 1
                if len(decoded) < 8:
                    name = f"{os.path.basename(p)}-f{fr.index:03d}.png"
                    img.save(os.path.join(OUT, name))
                    decoded.append((f"f{fr.index} c{fr.header.compression} {fr.header.magic.decode('latin1')}", img))
            except Exception as exc:  # noqa: BLE001
                errors.append((p, f"frame {fr.index}: {type(exc).__name__}: {exc}"))
        print(f"{p}: size={len(data)} frames={ag.frame_count} decoded={len(decoded)} comp={dict(comps)} alpha={dict(alphas)} magic={dict(magics)}")
        rows.append((os.path.basename(p), decoded))

    # combined contact sheet
    cols = max(1, max(len(r[1]) for r in rows)) if rows else 1
    sheet = Image.new("RGBA", (cols * 140, len(rows) * 160), (235, 235, 235, 255))
    d = ImageDraw.Draw(sheet)
    for r, (name, imgs) in enumerate(rows):
        d.text((4, r * 160 + 142), name[:24], fill=(0, 0, 0, 255))
        for c, (label, im) in enumerate(imgs):
            sheet.paste(cell(label, im), (c * 140, r * 160))
    out = os.path.join(OUT, "apg_contact_b0.png")
    sheet.save(out)
    print(f"frames decoded {total_ok}/{total_frames}; comps {dict(all_comps)}; alphas {dict(all_alphas)}")
    print("contact:", out)
    for e in errors[:20]:
        print("ERROR", e)


if __name__ == "__main__":
    main()
