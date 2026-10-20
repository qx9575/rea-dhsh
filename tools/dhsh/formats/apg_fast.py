"""NumPy fast paths for APG frame decoding.

The reference implementation in ``dhsh.formats.apg`` walks pixels in Python.
For batch PNG export this module expands RLE runs with NumPy slices and maps
RGB565/indexed pixels through lookup tables.  Output is byte-identical to
``frame_to_rgba`` for the modes used by the 0x0A corpus (AP/AI, compression
0/1/2/5, alpha 0/2).
"""
from __future__ import annotations

import io

import numpy as np
from PIL import Image

# alpha_mode 2 stores 0..32; the reference decoder maps it with min(255, v*255//32).
ALPHA32_TO_255 = np.array(
    [min(255, v * 255 // 32) for v in range(256)], dtype=np.uint8
)

_C565 = np.arange(65536, dtype=np.uint32)
RGB565_TO_RGBA = np.empty((65536, 4), dtype=np.uint8)
RGB565_TO_RGBA[:, 0] = ((_C565 >> 11) & 0x1F) * 255 // 31
RGB565_TO_RGBA[:, 1] = ((_C565 >> 5) & 0x3F) * 255 // 63
RGB565_TO_RGBA[:, 2] = (_C565 & 0x1F) * 255 // 31
RGB565_TO_RGBA[:, 3] = 255


def rle16_to_array(src: bytes, pixel_count: int) -> np.ndarray:
    """Decode the RGB565 RLE16 stream into a uint16 array of pixel_count."""
    out = np.zeros(pixel_count, dtype=np.uint16)
    pos = 0
    i = 0
    n = len(src)
    while i < n and pos < pixel_count:
        ctrl = src[i]
        i += 1
        run = ctrl & 0x7F
        if ctrl & 0x80:
            if i + 2 > n:
                break
            color = src[i] | (src[i + 1] << 8)
            i += 2
            take = min(run, pixel_count - pos)
            if take <= 0:
                break
            out[pos : pos + take] = color
            pos += take
        else:
            npix = min(run, (n - i) // 2, pixel_count - pos)
            if npix <= 0:
                break
            out[pos : pos + npix] = np.frombuffer(
                src[i : i + npix * 2], dtype="<u2"
            )
            i += npix * 2
            pos += npix
    return out


def rle8_to_array(src: bytes, out_len: int, fill: int = 0) -> np.ndarray:
    """Decode an RLE8 stream into a uint8 array; short streams use ``fill``."""
    out = np.full(out_len, fill, dtype=np.uint8)
    pos = 0
    i = 0
    n = len(src)
    while i < n and pos < out_len:
        ctrl = src[i]
        i += 1
        run = ctrl & 0x7F
        if ctrl & 0x80:
            if i >= n:
                break
            value = src[i]
            i += 1
            take = min(run, out_len - pos)
            if take <= 0:
                break
            out[pos : pos + take] = value
            pos += take
        else:
            take = min(run, n - i, out_len - pos)
            if take <= 0:
                break
            out[pos : pos + take] = np.frombuffer(src[i : i + take], dtype=np.uint8)
            i += take
            pos += take
    return out


def cti_rgba_lut(cti, table: int = 1) -> np.ndarray:
    """Build a 256x4 RGBA LUT from a parsed CTI palette."""
    if table == 2:
        t = np.asarray(cti.table2, dtype=np.uint32)
        lut = np.zeros((max(256, len(t)), 4), dtype=np.uint8)
        lut[: len(t), 0] = (t >> 16) & 255
        lut[: len(t), 1] = (t >> 8) & 255
        lut[: len(t), 2] = t & 255
        lut[: len(t), 3] = (t >> 24) & 255
    else:
        t = np.asarray(cti.table1, dtype=np.uint32)
        lut = np.zeros((max(256, len(t)), 4), dtype=np.uint8)
        lut[: len(t), 0] = ((t >> 11) & 0x1F) * 255 // 31
        lut[: len(t), 1] = ((t >> 5) & 0x3F) * 255 // 63
        lut[: len(t), 2] = (t & 0x1F) * 255 // 31
        lut[: len(t), 3] = 255
    # Indices beyond a short palette are not expected; keep unused rows opaque.
    if len(t) < 256:
        lut[len(t) : 256, 3] = 255
    return lut[:256]


def frame_to_rgba_fast(frame, cti=None, table: int = 1, lut=None) -> Image.Image:
    """Decode one APGFrame with NumPy; ``lut`` may be a cached CTI LUT."""
    hdr = frame.header
    block = frame.block
    start = hdr.header_size
    s1 = block[start : start + hdr.stream1_size]
    s2 = block[start + hdr.stream1_size : start + hdr.stream1_size + hdr.stream2_size]
    width, height = hdr.width, hdr.height
    pixel_count = width * height

    if hdr.magic == b"AI":
        if lut is None:
            if cti is None:
                raise ValueError("AI frame requires a CTI palette")
            lut = cti_rgba_lut(cti, table)
        indices = rle8_to_array(s1, pixel_count, 0)
        rgba = lut[indices]
        if hdr.alpha_mode == 2:
            alpha = rle8_to_array(s2, pixel_count, 255)
            rgba[:, 3] = ALPHA32_TO_255[alpha]
        return Image.fromarray(rgba.reshape(height, width, 4), "RGBA")

    if hdr.compression == 5:
        img = Image.open(io.BytesIO(s1)).convert("RGBA")
        if img.size != (width, height):
            img = img.resize((width, height))
        if hdr.alpha_mode == 2:
            alpha = rle8_to_array(s2, pixel_count, 255)
            img.putalpha(
                Image.fromarray(ALPHA32_TO_255[alpha].reshape(height, width), "L")
            )
        return img

    if hdr.compression == 2:
        pixels = rle16_to_array(s1, pixel_count)
    elif hdr.compression == 1:
        color = (s1[0] | (s1[1] << 8)) if len(s1) >= 2 else 0
        pixels = np.full(pixel_count, color, dtype=np.uint16)
    elif hdr.compression == 0:
        npix = min(len(s1) // 2, pixel_count)
        pixels = np.zeros(pixel_count, dtype=np.uint16)
        pixels[:npix] = np.frombuffer(s1[: npix * 2], dtype="<u2")
    else:
        raise NotImplementedError(f"A16 compression {hdr.compression} is not implemented")

    rgba = RGB565_TO_RGBA[pixels]
    if hdr.alpha_mode == 2:
        alpha = rle8_to_array(s2, pixel_count, 255)
        rgba[:, 3] = ALPHA32_TO_255[alpha]
    return Image.fromarray(rgba.reshape(height, width, 4), "RGBA")
