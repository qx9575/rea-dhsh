"""APG animation packages.

AG header (little-endian):
    0x00  char[2] "AG"
    0x02  uint16  header_size (8 + 8*frame_count)
    0x04  uint16  frame_count
    0x06  uint16  flags / variant
    0x08  uint32  reserved
    0x0c  frame_count * { uint32 frame_index, uint32 tail_offset }

Frame blocks start at header_size and are contiguous.  Each block is an
A16-style image header:
    "AP" or "AI", uint16 header_size (24), int16 x, int16 y,
    uint16 width, uint16 height, uint16 flags, uint8 compression,
    uint8 alpha_mode, uint32 stream1_size, uint32 stream2_size

compression 5 = JPEG in stream1; stream2 is the alpha stream (RLE8 for
alpha_mode 2).  compression 0/1/2 reuse the A16 pixel decoder.
"""
from __future__ import annotations

import io
import struct
from dataclasses import dataclass

from PIL import Image

from .a16 import A16Header, parse_header, decode_pixels, rle8_decode, rgb565_to_rgba


@dataclass(frozen=True)
class AGHeader:
    header_size: int
    frame_count: int
    flags: int
    reserved: int
    index: tuple[tuple[int, int, int], ...]  # (frame_id, flags, offset)


@dataclass(frozen=True)
class APGFrame:
    index: int
    offset: int
    header: A16Header
    block: bytes


def parse_ag(data: bytes) -> AGHeader:
    if len(data) < 16 or data[:2] != b"AG":
        raise ValueError("not an APG file")
    hs, fc, flags = struct.unpack_from("<HHH", data, 2)
    reserved = struct.unpack_from("<I", data, 8)[0]
    if hs < 8 + fc * 8 or hs > len(data):
        raise ValueError(f"invalid AG header size {hs} for {fc} frames")
    index = tuple(struct.unpack_from("<HHI", data, 8 + i * 8) for i in range(fc))
    return AGHeader(hs, fc, flags, reserved, index)


def iter_frames(data: bytes) -> tuple[AGHeader, list[APGFrame]]:
    ag = parse_ag(data)
    frames: list[APGFrame] = []
    pos = ag.header_size
    for i in range(ag.frame_count):
        if pos + 24 > len(data):
            raise ValueError(f"frame {i} out of bounds at {pos:#x}")
        if data[pos : pos + 2] not in (b"AP", b"AI"):
            # tolerate a small amount of padding between blocks
            found = -1
            for delta in range(2, 65):
                if data[pos + delta : pos + delta + 2] in (b"AP", b"AI"):
                    found = pos + delta
                    break
            if found < 0:
                raise ValueError(f"frame {i}: no AP/AI magic at {pos:#x}")
            pos = found
        hdr = parse_header(data[pos:])
        end = pos + hdr.header_size + hdr.stream1_size + hdr.stream2_size
        if end > len(data):
            raise ValueError(f"frame {i}: block end {end:#x} > file {len(data):#x}")
        frames.append(APGFrame(i, pos, hdr, data[pos:end]))
        pos = end
    return ag, frames


def frame_to_rgba(frame: APGFrame, palette=None, palette_table: int = 1) -> Image.Image:
    hdr = frame.header
    block = frame.block
    start = hdr.header_size
    s1 = block[start : start + hdr.stream1_size]
    s2 = block[start + hdr.stream1_size : start + hdr.stream1_size + hdr.stream2_size]
    width, height = hdr.width, hdr.height
    pixel_count = width * height

    # AI blocks are indexed RLE8 pixels + a CTI palette (verified on pet/head).
    if hdr.magic == b"AI":
        if palette is None:
            raise ValueError("AI frame requires a CTI palette")
        indices = rle8_decode(s1, pixel_count)
        alpha = None
        if hdr.alpha_mode == 2:
            alpha = bytes(min(255, v * 255 // 32) for v in rle8_decode(s2, pixel_count))
            if len(alpha) < pixel_count:
                alpha += bytes([255]) * (pixel_count - len(alpha))
        out = bytearray()
        if palette_table == 2:
            colors = palette.table2
            for i, idx in enumerate(indices):
                e = colors[idx]
                r, g, b = (e >> 16) & 255, (e >> 8) & 255, e & 255
                a = alpha[i] if alpha is not None else ((e >> 24) & 255)
                out += bytes((r, g, b, a))
        else:
            colors = palette.table1
            for i, idx in enumerate(indices):
                c = colors[idx]
                r = ((c >> 11) & 31) * 255 // 31
                g = ((c >> 5) & 63) * 255 // 63
                b = (c & 31) * 255 // 31
                a = alpha[i] if alpha is not None else 255
                out += bytes((r, g, b, a))
        return Image.frombytes("RGBA", (width, height), bytes(out))

    if hdr.compression == 5:
        img = Image.open(io.BytesIO(s1)).convert("RGBA")
        if img.size != (width, height):
            img = img.resize((width, height))
        if hdr.alpha_mode == 2:
            alpha = rle8_decode(s2, pixel_count)
            alpha = bytes(min(255, v * 255 // 32) for v in alpha)
            if len(alpha) < pixel_count:
                alpha += bytes([255]) * (pixel_count - len(alpha))
            img.putalpha(Image.frombytes("L", (width, height), alpha))
        return img

    pixels = decode_pixels(hdr, s1)
    alpha = None
    if hdr.alpha_mode == 2:
        alpha = bytes(min(255, v * 255 // 32) for v in rle8_decode(s2, pixel_count))
        if len(alpha) < pixel_count:
            alpha += bytes([255]) * (pixel_count - len(alpha))
    return Image.frombytes("RGBA", (width, height), rgb565_to_rgba(pixels, alpha))

