"""基础纸娃娃渲染器：APG 帧 + APCI 层叠键 + PIF 锚点/延时。"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from PIL import Image

from dhsh.formats import a16 as da16
from dhsh.formats import apg as dapg

from paperdoll.catalog import PaperDollCatalog, PartAsset

CANVAS_W, CANVAS_H = 640, 480


@dataclass
class DecodedPart:
    asset: PartAsset
    frames: list[Image.Image] = field(default_factory=list)
    offsets: list[tuple[int, int]] = field(default_factory=list)
    anchor: tuple[int, int] = (160, 210)
    delay: int = 80
    keys: list[int] = field(default_factory=lambda: [99])


def decode_part(cat: PaperDollCatalog, asset: PartAsset) -> DecodedPart:
    decoded = DecodedPart(asset)
    if not asset.exists:
        return decoded
    hit = cat.reader.read(asset.pak, asset.apg, exact=True)
    if hit is None:
        return decoded
    _hdr, frames = dapg.iter_frames(hit[1])
    palette = None
    if any(f.header.magic == b"AI" for f in frames):
        if not asset.cti:
            return decoded
        cti = cat.reader.read(asset.pak, asset.cti, exact=True)
        if cti is not None:
            palette = da16.parse_cti(cti[1])
    images = []
    for frame in frames:
        try:
            images.append(dapg.frame_to_rgba(frame, palette))
        except Exception:  # noqa: BLE001
            return decoded
    decoded.frames = images
    decoded.offsets = [(f.header.x, f.header.y) for f in frames]
    if asset.anchor:
        decoded.anchor = asset.anchor
    if asset.delay:
        decoded.delay = asset.delay
    decoded.keys = asset.keys or [99]
    return decoded


def _blit_clip(canvas: Image.Image, image: Image.Image, x: int, y: int) -> None:
    if image.width <= 0 or image.height <= 0:
        return
    left = max(0, x)
    top = max(0, y)
    right = min(canvas.width, x + image.width)
    bottom = min(canvas.height, y + image.height)
    if left >= right or top >= bottom:
        return
    crop = image.crop((left - x, top - y, right - x, bottom - y))
    canvas.alpha_composite(crop, (left, top))


def assemble(cat: PaperDollCatalog, assets: list[PartAsset]) -> tuple[list[Image.Image], int, list[str]]:
    decoded: list[DecodedPart] = []
    diagnostics: list[str] = []
    for asset in assets:
        part = decode_part(cat, asset)
        if not part.frames:
            diagnostics.append(f"{asset.part}/{asset.model}: no frames or palette")
            continue
        decoded.append(part)
    if not decoded:
        return [], 80, diagnostics
    by_part = {p.asset.part: p for p in decoded}
    mount_shift = None
    if "body" in by_part and "horse" in by_part:
        body, horse = by_part["body"], by_part["horse"]
        if body.frames and horse.frames:
            bx = body.offsets[0][0] + body.frames[0].width / 2
            by = body.offsets[0][1] + body.frames[0].height
            hx = horse.offsets[0][0] + horse.frames[0].width / 2
            hy = horse.offsets[0][1] + horse.frames[0].height
            mount_shift = (round(bx - hx), round(by - hy))
    total = max(len(p.frames) for p in decoded)
    delay = decoded[0].delay
    output: list[Image.Image] = []
    for frame_index in range(total):
        canvas = Image.new("RGBA", (CANVAS_W, CANVAS_H), (0, 0, 0, 0))
        order = []
        for part in decoded:
            idx = frame_index % len(part.frames)
            key = part.keys[min(idx, len(part.keys) - 1)] if part.keys else 99
            order.append((key, part.asset.part, part, idx))
        order.sort(key=lambda item: (item[0], item[1]))
        for _key, part_name, part, idx in order:
            ox, oy = part.offsets[idx] if idx < len(part.offsets) else (0, 0)
            ax, ay = part.anchor
            if mount_shift and part_name in ("horse", "harmorbody", "harmorhead"):
                ax += mount_shift[0]
                ay += mount_shift[1]
            _blit_clip(canvas, part.frames[idx].convert("RGBA"),
                       CANVAS_W // 2 - ax + ox, CANVAS_H // 2 - ay + oy)
        output.append(canvas)
    return output, delay, diagnostics


def save_frames(frames: list[Image.Image], delay: int, out_dir: Path,
                make_gif: bool = True, stem: str = "frame") -> list[Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    paths = []
    for index, frame in enumerate(frames):
        path = out_dir / f"{stem}_{index:03d}.png"
        frame.save(path)
        paths.append(path)
    if make_gif and frames:
        gif = out_dir / "animation.gif"
        frames[0].save(gif, save_all=True, append_images=frames[1:],
                       duration=max(1, delay), loop=0, disposal=2)
        paths.append(gif)
    return paths
