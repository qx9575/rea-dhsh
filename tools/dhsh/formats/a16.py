"""A16 / CTI resource parsing for the Hero2D client.

A16 = 16-bit bitmap. Header is little-endian:

    0x00 char[2]  "AP" (or "AL")
    0x02 uint16   header size (24)
    0x04 int16    x offset
    0x06 int16    y offset
    0x08 uint16   width
    0x0a uint16   height
    0x0c uint16   flags / format hint
    0x0e uint8    pixel compression (0=raw, 1=solid, 2=RLE16, ...)
    0x0f uint8    alpha stream mode
    0x10 uint32   stream 1 size (pixels)
    0x14 uint32   stream 2 size (alpha)
    0x18 ...      stream 1, then stream 2

RLE16 (compression=2) control byte:
    bit7=1: repeat next uint16 colour (ctrl & 0x7f) times
    bit7=0: copy (ctrl & 0x7f) uint16 pixels literally
"""
from __future__ import annotations

import struct
from dataclasses import dataclass


@dataclass(frozen=True)
class A16Header:
    magic: bytes
    header_size: int
    x: int
    y: int
    width: int
    height: int
    flags: int
    compression: int
    alpha_mode: int
    stream1_size: int
    stream2_size: int


@dataclass(frozen=True)
class A16Image:
    header: A16Header
    pixels: list[int]  # 16-bit RGB565 values, width*height
    alpha: bytes | None


def parse_header(data: bytes) -> A16Header:
    if len(data) < 24 or data[:2] not in (b"AP", b"AL", b"AI"):
        raise ValueError("not an A16/AI file")
    header_size = struct.unpack_from("<H", data, 2)[0]
    x, y, w, h = struct.unpack_from("<hhhh", data, 4)
    flags = struct.unpack_from("<H", data, 12)[0]
    compression = data[14]
    alpha_mode = data[15]
    s1, s2 = struct.unpack_from("<II", data, 16)
    if header_size < 24 or header_size > len(data):
        raise ValueError(f"invalid A16 header size {header_size}")
    return A16Header(
        magic=data[:2],
        header_size=header_size,
        x=x,
        y=y,
        width=w,
        height=h,
        flags=flags,
        compression=compression,
        alpha_mode=alpha_mode,
        stream1_size=s1,
        stream2_size=s2,
    )


def rle16_decode(src: bytes, pixel_count: int) -> list[int]:
    out: list[int] = []
    i = 0
    n = len(src)
    while i < n and len(out) < pixel_count:
        ctrl = src[i]
        i += 1
        count = ctrl & 0x7F
        if ctrl & 0x80:
            if i + 2 <= n:
                color = src[i] | (src[i + 1] << 8)
                i += 2
                out.extend([color] * count)
        else:
            nbytes = min(count * 2, n - i)
            for j in range(0, nbytes - 1, 2):
                out.append(src[i + j] | (src[i + j + 1] << 8))
            i += nbytes
    return out[:pixel_count]


def rle8_decode(src: bytes, out_len: int) -> bytes:
    """Alpha-mask RLE (A16 alpha_mode=2, Game.exe VA 0x8a75c0)."""
    out = bytearray()
    i = 0
    n = len(src)
    while i < n and len(out) < out_len:
        ctrl = src[i]
        i += 1
        count = ctrl & 0x7F
        if ctrl & 0x80:
            if i >= n:
                break
            value = src[i]
            i += 1
            out.extend(bytes([value]) * count)
        else:
            nbytes = min(count, n - i)
            out.extend(src[i : i + nbytes])
            i += nbytes
    return bytes(out[:out_len])


def decode_pixels(header: A16Header, stream1: bytes) -> list[int]:
    pixel_count = header.width * header.height
    if header.compression == 0:  # raw RGB565
        pixels = [
            stream1[i] | (stream1[i + 1] << 8)
            for i in range(0, min(len(stream1), pixel_count * 2) - 1, 2)
        ]
    elif header.compression == 1:  # solid colour
        color = stream1[0] | (stream1[1] << 8) if len(stream1) >= 2 else 0
        pixels = [color] * pixel_count
    elif header.compression == 2:  # RLE16
        pixels = rle16_decode(stream1, pixel_count)
    else:
        raise NotImplementedError(f"A16 compression {header.compression} is not implemented")
    if len(pixels) < pixel_count:
        pixels.extend([0] * (pixel_count - len(pixels)))
    return pixels


def rgb565_to_rgba(pixels: list[int], alpha: bytes | None = None) -> bytes:
    out = bytearray()
    for i, c in enumerate(pixels):
        r = (c >> 11) & 0x1F
        g = (c >> 5) & 0x3F
        b = c & 0x1F
        a = alpha[i] if alpha is not None and i < len(alpha) else 255
        out += bytes((r * 255 // 31, g * 255 // 63, b * 255 // 31, a))
    return bytes(out)


def decode_a16(data: bytes) -> A16Image:
    hdr = parse_header(data)
    start = hdr.header_size
    s1 = data[start : start + hdr.stream1_size]
    s2 = data[start + hdr.stream1_size : start + hdr.stream1_size + hdr.stream2_size]
    pixels = decode_pixels(hdr, s1)
    pixel_count = hdr.width * hdr.height
    if hdr.alpha_mode == 0:
        alpha = None
    elif hdr.alpha_mode == 2:
        # The alpha stream is RLE8; observed values are 0..32 (0 = transparent,
        # 32 = opaque), so scale to 0..255.
        alpha = bytes(min(255, v * 255 // 32) for v in rle8_decode(s2, pixel_count))
        if len(alpha) < pixel_count:
            alpha = alpha + bytes([255]) * (pixel_count - len(alpha))
    else:
        # alpha_mode 3/4 use an NZ-wrapped stream; 1/5/7/8 are not implemented yet.
        alpha = None
    return A16Image(header=hdr, pixels=pixels, alpha=alpha)


def decode_a16_rgba(data: bytes):
    """Decode a standalone A16 to a PIL RGBA image (reference path).

    compression 5 stores a complete JPEG in stream 1, mirroring APG frames.
    """
    import io

    from PIL import Image  # imported lazily to keep header parsing dependency-free

    hdr = parse_header(data)
    start = hdr.header_size
    s1 = data[start : start + hdr.stream1_size]
    s2 = data[start + hdr.stream1_size : start + hdr.stream1_size + hdr.stream2_size]
    pixel_count = hdr.width * hdr.height

    if hdr.compression == 5:
        img = Image.open(io.BytesIO(s1)).convert("RGBA")
        if img.size != (hdr.width, hdr.height):
            img = img.resize((hdr.width, hdr.height))
        if hdr.alpha_mode == 2:
            alpha = bytes(
                min(255, v * 255 // 32) for v in rle8_decode(s2, pixel_count)
            )
            if len(alpha) < pixel_count:
                alpha += bytes([255]) * (pixel_count - len(alpha))
            img.putalpha(Image.frombytes("L", (hdr.width, hdr.height), alpha))
        return img

    img = decode_a16(data)
    return Image.frombytes(
        "RGBA",
        (hdr.width, hdr.height),
        rgb565_to_rgba(img.pixels, img.alpha),
    )


# ---------------------------------------------------------------------------
# CTI palette
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class CTIHeader:
    header_size: int
    count: int
    bpp: int
    format: int
    table1_size: int
    table2_size: int


@dataclass(frozen=True)
class CTI:
    header: CTIHeader
    table1: tuple[int, ...]  # count uint16 values
    table2: tuple[int, ...]  # count uint32 values


def parse_cti(data: bytes) -> CTI:
    if len(data) < 20 or data[:2] != b"CT":
        raise ValueError("not a CTI file")
    header_size = struct.unpack_from("<H", data, 2)[0]
    count, bpp = struct.unpack_from("<HH", data, 4)
    fmt = struct.unpack_from("<I", data, 8)[0]
    t1_size, t2_size = struct.unpack_from("<II", data, 12)
    if header_size + t1_size + t2_size > len(data):
        raise ValueError("truncated CTI file")
    t1 = struct.unpack_from(f"<{count}H", data, header_size)
    t2 = struct.unpack_from(f"<{count}I", data, header_size + t1_size)
    return CTI(
        header=CTIHeader(header_size, count, bpp, fmt, t1_size, t2_size),
        table1=t1,
        table2=t2,
    )
