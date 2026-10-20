"""NumPy fast paths for standalone A16 images.

Shares RLE/LUT helpers with ``apg_fast``.  Output matches
``dhsh.formats.a16.decode_a16_rgba`` for compression 0/1/2/5 and alpha 0/2,
which covers the whole extracted corpus (29,716 files).
"""
from __future__ import annotations

import io

import numpy as np
from PIL import Image

from .a16 import parse_header
from .apg_fast import ALPHA32_TO_255, RGB565_TO_RGBA, rle8_to_array, rle16_to_array


def a16_to_rgba_fast(data: bytes) -> Image.Image:
    hdr = parse_header(data)
    start = hdr.header_size
    s1 = data[start : start + hdr.stream1_size]
    s2 = data[start + hdr.stream1_size : start + hdr.stream1_size + hdr.stream2_size]
    width, height = hdr.width, hdr.height
    pixel_count = width * height

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
        raise NotImplementedError(
            f"A16 compression {hdr.compression} is not implemented"
        )

    rgba = RGB565_TO_RGBA[pixels]
    if hdr.alpha_mode == 2:
        alpha = rle8_to_array(s2, pixel_count, 255)
        rgba[:, 3] = ALPHA32_TO_255[alpha]
    return Image.fromarray(rgba.reshape(height, width, 4), "RGBA")
