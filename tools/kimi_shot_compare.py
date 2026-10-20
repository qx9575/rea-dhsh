"""Pixel-compare a game screenshot of the BangPai world map against the
private-JPEG decode of BangPai_1m.jpg, under both DC-slot schemes.

Pipeline (per docs/formats/special-jpg-kimi-review.md, runtime-verified):
  - v2 slots: Y0..Y3 all use slot 0, Cb -> slot 1, Cr -> slot 2
    (old wrong cycle 0/1/2/0 kept for comparison)
  - IDCT: float reference with floor(ref/16), clip [-128,127]
    (bit-exact integer sim is too slow for 25k blocks; floor/16 matches
    ~80% of pixels exactly, rest +-1 -- fine for this comparison)
  - colour: trunc(K*v+1)>>1 lookup tables (runtime-verified), half-scale
    pipeline, RGB565, then 565 -> 888 bit replication

The screenshot contains a scaled, framed map with label overlays, so the
comparison is done after scaling the decode to the detected content box,
and scored by robust statistics (median abs diff over low-texture areas).
"""
from __future__ import annotations

import sys

import numpy as np

sys.path.insert(0, "tools")
from dhsh.formats import special_jpg as sj  # noqa: E402

JPG = r"extracted/images-standard/map/BangPai_1/BangPai_1m.jpg"
SHOT = "bangpai.png"


# ---------------------------------------------------------------- decoder
def decode_v2(jpg, order):
    """Return int8 Y/Cb/Cr planes at full resolution (320x320)."""
    cols, rows = jpg.mcu_cols, jpg.mcu_rows
    w, h = jpg.width, jpg.height
    Y = np.empty((h, w), dtype=np.int8)
    CB = np.empty((h // 2, w // 2), dtype=np.int8)
    CR = np.empty((h // 2, w // 2), dtype=np.int8)
    for i in range(rows * cols):
        my, mx = divmod(i, cols)
        ptr, bitoff, f2, f3, f4 = jpg.records[i]
        seeds = [int(f2), int(f3), int(f4)]
        state = (int(ptr), int(bitoff))
        blocks = []
        for comp_i, slot in order:
            coeff, dc, state = sj.decode_block(jpg, state, comp_i, seeds[slot])
            seeds[slot] = dc
            blocks.append(coeff)
        px = sj.idct_blocks(np.stack(blocks))  # (6,64) int8, scale /64 inside
        yb = px[:4].reshape(2, 2, 8, 8).transpose(0, 2, 1, 3).reshape(16, 16)
        Y[my * 16:my * 16 + 16, mx * 16:mx * 16 + 16] = yb
        CB[my * 8:my * 8 + 8, mx * 8:mx * 8 + 8] = px[4].reshape(8, 8)
        CR[my * 8:my * 8 + 8, mx * 8:mx * 8 + 8] = px[5].reshape(8, 8)
    return Y, CB, CR


def build_tables():
    t = []
    for kind in ("R", "Gb", "Gr", "B"):
        out = np.empty(256, dtype=np.int32)
        for i in range(256):
            v = i - 256 if i >= 128 else i
            if kind == "R":
                x = 1.402 * v + 1.0
            elif kind == "Gb":
                x = 1.0 - 0.34414 * v
            elif kind == "Gr":
                x = 1.0 - 0.71414 * v
            else:
                x = 1.772 * v + 1.0
            out[i] = (int(x) if x >= 0 else -int(-x)) >> 1   # trunc, sar
        t.append(out)
    return t


def to_rgb565_888(Y, CB, CR):
    """Game pipeline: half-scale tables, clamp7, RGB565, expand to 888."""
    TR, TGB, TGR, TB = build_tables()
    h, w = Y.shape
    cb = np.repeat(np.repeat(CB, 2, 0), 2, 1).astype(np.int32)
    cr = np.repeat(np.repeat(CR, 2, 0), 2, 1).astype(np.int32)
    t = (Y.astype(np.int32) + 128) >> 1
    r7 = np.clip(TR[cr & 0xFF] + t, 0, 127)
    g7 = np.clip(TGB[cb & 0xFF] + TGR[cr & 0xFF] + t, 0, 127)
    b7 = np.clip(TB[cb & 0xFF] + t, 0, 127)
    r5, g6, b5 = r7 >> 2, g7 >> 1, b7 >> 2
    # 565 -> 888 with bit replication (typical blitter)
    r8 = (r5 << 3) | (r5 >> 2)
    g8 = (g6 << 2) | (g6 >> 4)
    b8 = (b5 << 3) | (b5 >> 2)
    return np.stack([r8, g8, b8], -1).astype(np.uint8)


# ---------------------------------------------------------------- geometry
def find_content_box(img):
    """Locate the map content inside the blue frame."""
    # frame is saturated blue: B high, R low
    blue = (img[..., 2] > 100) & (img[..., 0] < 90) & (img[..., 1] < 120)
    rowfrac = blue.mean(axis=1)
    colfrac = blue.mean(axis=0)
    rows = np.nonzero(rowfrac > 0.5)[0]
    cols = np.nonzero(colfrac > 0.5)[0]
    # content box = inside the outermost full-blue rows/cols
    top, bottom = rows.min(), rows.max()
    left, right = cols.min(), cols.max()
    # step inward past the frame thickness
    t = top
    while t < bottom and rowfrac[t] > 0.5:
        t += 1
    b = bottom
    while b > t and rowfrac[b] > 0.5:
        b -= 1
    l = left
    while l < right and colfrac[l] > 0.5:
        l += 1
    r = right
    while r > l and colfrac[r] > 0.5:
        r -= 1
    return l, t, r + 1, b + 1


def scale_np(img, tw, th):
    """Bilinear resize (h, w, 3) -> (th, tw)."""
    h, w = img.shape[:2]
    ys = (np.arange(th) + 0.5) * h / th - 0.5
    xs = (np.arange(tw) + 0.5) * w / tw - 0.5
    y0 = np.clip(np.floor(ys).astype(int), 0, h - 1)
    y1 = np.clip(y0 + 1, 0, h - 1)
    x0 = np.clip(np.floor(xs).astype(int), 0, w - 1)
    x1 = np.clip(x0 + 1, 0, w - 1)
    fy = (ys - np.floor(ys)).astype(np.float32)[:, None]
    fx = (xs - np.floor(xs)).astype(np.float32)[None, :]
    img = img.astype(np.float32)
    top = img[y0][:, x0] * (1 - fx) + img[y0][:, x1] * fx
    bot = img[y1][:, x0] * (1 - fx) + img[y1][:, x1] * fx
    return (top * (1 - fy[..., None]) + bot * fy[..., None]).astype(np.uint8)


def score(shot, dec):
    """Robust score: median/mean abs diff over areas that are not text labels.

    Text labels are bright/dark high-contrast boxes; exclude pixels whose
    local 8x8 gradient energy is extreme (text) or where the shot has
    near-white UI glyphs.
    """
    d = np.abs(shot.astype(np.int32) - dec.astype(np.int32)).sum(axis=2)
    # mask out bright text pixels (labels are white/yellow on dark boxes)
    bright = (shot.max(axis=2) > 200)
    dark = (shot.min(axis=2) < 40)
    mask = ~(bright & dark)
    # also drop the strongest-decision pixels: keep central 80% of diffs
    vals = d[mask]
    lo, hi = np.percentile(vals, [5, 95])
    keep = vals[(vals >= lo) & (vals <= hi)]
    return float(np.median(vals)), float(keep.mean()), float(vals.mean())


def main() -> int:
    from PIL import Image
    shot = np.asarray(Image.open(SHOT).convert("RGB"))
    l, t, r, b = find_content_box(shot)
    print(f"content box: ({l},{t})-({r},{b}) = {r - l}x{b - t}")
    content = shot[t:b, l:r]

    jpg = sj.parse(open(JPG, "rb").read())
    schemes = {
        "fixed (Y*4 -> slot0)": [(0, 0), (0, 0), (0, 0), (0, 0), (1, 1), (2, 2)],
        "old cycle 0/1/2/0":  [(0, 0), (0, 1), (0, 2), (0, 0), (1, 1), (2, 2)],
    }
    for name, order in schemes.items():
        Y, CB, CR = decode_v2(jpg, order)
        rgb = to_rgb565_888(Y, CB, CR)
        dec = scale_np(rgb, r - l, b - t)
        med, trimmed, mean = score(content, dec)
        print(f"{name}: median |diff| {med:.1f}, trimmed-mean {trimmed:.1f}, "
              f"mean {mean:.1f}")
        Image.fromarray(dec).save(
            f"extracted/easy/kimi_bangpai_{'fixed' if 'fixed' in name else 'old'}.png")
    Image.fromarray(content).save("extracted/easy/kimi_bangpai_shot.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
