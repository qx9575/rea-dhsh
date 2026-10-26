"""场景构建器：从 PAK 直接合成地图场景（不经 extracted 中间文件）。

图层（绘制顺序）：背景 → MAN 动画 → GAL 云雾 → MSK/core 遮罩 →
CEL 碰撞 → MLK 跳转框。除背景外各层可独立开关。

返回的 SceneData 全部为 PIL 图像与纯数据，构建在后台线程执行，
GUI（scene_view.py）负责合成显示与动画播放。
"""
from __future__ import annotations

import os
from dataclasses import dataclass, field

import numpy as np
from PIL import Image

from dhsh.formats import map as dmap
from dhsh.formats.a16 import parse_header as parse_a16_header
from dhsh.formats.pif import parse_pif

MAX_ANIM_FRAMES = 2000   # 全场景动画帧数保护
DEFAULT_DELAY = 80       # 无 perinfo.dat 时的默认帧延时 ms


@dataclass
class AnimSprite:
    x: int
    y: int
    frames: list
    offsets: list          # 每帧 (ox, oy)，来自 A16 头
    delays: list           # 每帧 ms
    name: str


@dataclass
class Layer:
    key: str
    label: str
    visible: bool
    image: object = None   # PIL RGBA（静态叠加层）
    anims: list = field(default_factory=list)


@dataclass
class SceneData:
    width: int
    height: int
    background: object     # PIL RGB
    layers: list           # [Layer]
    meta: dict


def _decode_image(src, entry):
    """任意图像 Entry → PIL RGBA（复用插件体系的解码路径）。

    大 JPEG（如 45MP 特殊 JPG 背景）首次解码约 20s：解码结果写入
    ``src.imgcache_dir`` 磁盘缓存，后续打开只需普通 PNG 解码时间。
    """
    cache_path = None
    imgcache = getattr(src, "imgcache_dir", None)
    if imgcache and entry.ext in (".jpg", ".jpeg"):
        safe = entry.key.replace("/", "_").replace(":", "_")
        try:
            st = os.stat(os.path.join(src.shdata_dir, entry.pak))
            sig = f"{int(st.st_mtime)}_{st.st_size}"
        except OSError:
            sig = "0"
        cache_path = os.path.join(imgcache, f"{safe}.{sig}.png")
        if os.path.exists(cache_path):
            try:
                img = Image.open(cache_path)
                img.load()
                return img.convert("RGBA")
            except Exception:  # noqa: BLE001 — 缓存损坏则重解码
                pass
    payload, _ = src.decode_entry(entry)
    if entry.ext in (".jpg", ".jpeg", ".bmp", ".png", ".tga"):
        from .plugins.std_image import StdImagePlugin
        import time
        t0 = time.perf_counter()
        img = StdImagePlugin().load(payload, payload, entry, src)["frames"][0].convert("RGBA")
        if cache_path and time.perf_counter() - t0 > 1.0:
            try:
                os.makedirs(imgcache, exist_ok=True)
                try:
                    import cv2
                    cv2.imwrite(cache_path, cv2.cvtColor(
                        __import__("numpy").array(img), cv2.COLOR_RGBA2BGRA))
                except ImportError:
                    img.save(cache_path, "PNG", compress_level=1)
            except Exception:  # noqa: BLE001 — 缓存写不出不影响预览
                pass
        return img
    if entry.ext == ".gal":
        from .plugins.image_a16_pg2 import GALPlugin
        return GALPlugin().load(payload, payload, entry, src)["frames"][0]
    if entry.ext == ".a16":
        from .plugins.image_a16_pg2 import _decode_ap_block
        return _decode_ap_block(payload)
    raise ValueError(f"不支持的图像类型: {entry.ext}")


def _load_anim(src, directory: str, x: int, y: int, budget: list[int]) -> AnimSprite | None:
    """读取一个 MAN 动画目录：全部 A16 帧 + 头偏移 + perinfo 帧延时。"""
    files = src.list_dir(directory)
    if not files:
        return None
    a16s = [e for e in files if e.ext == ".a16"]

    def sort_key(e):
        stem = os.path.splitext(e.name)[0]
        return (0, int(stem)) if stem.isdigit() else (1, e.name)

    a16s.sort(key=sort_key)
    frames, offsets = [], []
    if a16s:
        for e in a16s:
            if budget[0] <= 0:
                break
            budget[0] -= 1
            payload, _ = src.decode_entry(e)
            hdr = parse_a16_header(payload)
            from .plugins.image_a16_pg2 import _decode_ap_block
            frames.append(_decode_ap_block(payload))
            offsets.append((hdr.x, hdr.y))
    else:
        tgas = sorted((e for e in files if e.ext == ".tga"), key=lambda e: e.name)
        for e in tgas[:1]:
            frames.append(_decode_image(src, e))
            offsets.append((0, 0))
    if not frames:
        return None
    delays = [DEFAULT_DELAY] * len(frames)
    pif_entry = src.find_path(directory + "/perinfo.dat")
    if pif_entry is not None:
        try:
            payload, _ = src.decode_entry(pif_entry)
            pif = parse_pif(payload)
            for i in range(min(len(frames), pif.width)):
                delays[i] = max(1, pif.rows[i][5]) if len(pif.rows[i]) > 5 else DEFAULT_DELAY
        except Exception:  # noqa: BLE001 — 缺/坏 perinfo 用默认延时
            pass
    return AnimSprite(x, y, frames, offsets, delays, directory)


def _cel_overlay(src, cellfile: str, base: dict, w: int, h: int,
                 bg_rgb: np.ndarray) -> Image.Image | None:
    """CEL 碰撞层：16px/格，蓝半透明；地图格按左上裁剪/补齐（同 map_preview）。"""
    cel_ent = src.find_path(cellfile, (".cel",))
    if cel_ent is None:
        return None
    payload, _ = src.decode_entry(cel_ent)
    cel_img = dmap.cel_to_image(payload, layer=1)   # L：255=阻挡
    cw, ch = cel_img.size
    map_w = int(base.get("WIDTH", 0) or w)
    map_h = int(base.get("HEIGHT", 0) or h)
    map_cw, map_ch = map_w // 16, map_h // 16
    use_w, use_h = min(cw, map_cw), min(ch, map_ch)
    scaled = cel_img.resize((cw * 16, ch * 16), Image.NEAREST)
    plane = np.zeros((map_ch * 16, map_cw * 16), dtype=np.uint8)  # 0=可走
    plane[:use_h * 16, :use_w * 16] = np.asarray(scaled)[:use_h * 16, :use_w * 16]
    # 地图原点：内容不足整图时取非黑内容左上角（TingYuan_* 黑边）
    off_x = off_y = 0
    if (map_w, map_h) != (w, h):
        black = bg_rgb.max(axis=2) < 10
        if black.any() and not black.all():
            ys, xs = np.nonzero(~black)
            off_x, off_y = int(xs.min()), int(ys.min())
    out = np.zeros((h, w, 4), dtype=np.uint8)
    blocked = plane != 0
    hh, ww = plane.shape
    x2, y2 = min(w, off_x + ww), min(h, off_y + hh)
    if x2 > off_x and y2 > off_y:
        region = out[off_y:y2, off_x:x2]
        region[blocked[:y2 - off_y, :x2 - off_x]] = (0, 80, 255, 70)
    return Image.fromarray(out, "RGBA")


def _mask_overlay(src, ini, w: int, h: int) -> tuple[Image.Image | None, int, int]:
    """MSK/core 遮罩：静态 magenta、组首帧 orange；返回 (图, 静态数, 组帧数)。"""
    msk_ent = src.find_path(ini["maskfile"], (".core", ".msk"))
    if msk_ent is None:
        return None, 0, 0
    payload, _ = src.decode_entry(msk_ent)
    msk = dmap.parse_msk(payload)
    fill = np.zeros((h, w), dtype=np.uint8)
    first = {}
    for pl in msk.placements:
        if pl.flag != 1:
            first[pl.group] = min(first.get(pl.group, 99), pl.frame)
    cache = {}
    static_n = group_n = 0
    for pl in msk.placements:
        if pl.flag == 1:
            label = 1
            static_n += 1
        else:
            if pl.frame != first[pl.group]:
                continue
            label = 2
            group_n += 1
        if pl.record not in cache:
            cache[pl.record] = dmap.decode_mask(msk, pl.record)
        m = cache[pl.record]
        mh, mw = m.shape
        x0, y0 = max(0, pl.x), max(0, pl.y)
        x1, y1 = min(w, pl.x + mw), min(h, pl.y + mh)
        if x1 <= x0 or y1 <= y0:
            continue
        sub = m[y0 - pl.y:y1 - pl.y, x0 - pl.x:x1 - pl.x]
        region = fill[y0:y1, x0:x1]
        region[sub != 0] = label
    out = np.zeros((h, w, 4), dtype=np.uint8)
    out[fill == 1] = (255, 0, 180, 90)
    out[fill == 2] = (255, 140, 0, 110)
    return Image.fromarray(out, "RGBA"), static_n, group_n


def _link_overlay(src, map_stem: str, w: int, h: int) -> tuple[Image.Image | None, int]:
    mlk_ent = src.find_path(f"config/map/{map_stem}.mlk")
    if mlk_ent is None:
        return None, 0
    payload, _ = src.decode_entry(mlk_ent)
    links = dmap.parse_mlk_links(payload)
    out = np.zeros((h, w, 4), dtype=np.uint8)

    def rect(x, y, rw, rh, color):
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(w, x + rw), min(h, y + rh)
        if x1 <= x0 or y1 <= y0:
            return
        for yy in (y0, y1 - 1):
            if 0 <= yy < h:
                out[yy, x0:x1] = color
        for xx in (x0, x1 - 1):
            if 0 <= xx < w:
                out[y0:y1, xx] = color

    def cross(cx, cy, color):
        for d in range(-7, 8):
            if 0 <= cx + d < w and 0 <= cy < h:
                out[cy, cx + d] = color
            if 0 <= cx < w and 0 <= cy + d < h:
                out[cy + d, cx] = color

    for l in links:
        rect(l.x, l.y, l.w, l.h, (0, 255, 255, 255))
        cross(l.dest_x, l.dest_y, (255, 140, 0, 255))
    return Image.fromarray(out, "RGBA"), len(links)


def build_scene(src, entry) -> SceneData:
    """从 .map INI 条目构建场景。背景缺失时抛异常（调用方降级文本）。"""
    payload, _ = src.decode_entry(entry)
    ini = dmap.parse_map_ini(payload)
    base = ini.get("base", {})
    ext = ini.get("ext", {})
    info = {
        "cellfile": base.get("CELLFILE", ""),
        "maskfile": base.get("MASKFILE", ""),
        "anifile": ext.get("ANIFILE", ""),
        "title": ext.get("NAME", ""),
    }
    bg_ent = src.find_path(base.get("BACKGROUND", ""), (".jpg", ".png", ".tga", ".bmp"))
    if bg_ent is None:
        raise FileNotFoundError(f"背景未找到: {base.get('BACKGROUND', '')}")
    bg = _decode_image(src, bg_ent)
    w, h = bg.size
    bg_rgb = np.asarray(bg.convert("RGB"))
    layers: list[Layer] = []
    meta = {"场景": info["title"] or entry.path, "背景": bg_ent.key,
            "画布": f"{w}×{h}", "插件": "场景合成"}

    # MAN 动画（可播放层）
    anims: list[AnimSprite] = []
    man_missing = 0
    if info["anifile"]:
        man_ent = src.find_path(info["anifile"], (".man",))
        if man_ent is not None:
            man_payload, _ = src.decode_entry(man_ent)
            records = dmap.parse_man_entries(man_payload)
            budget = [MAX_ANIM_FRAMES]
            cache: dict[str, AnimSprite | None] = {}
            for rec in records:
                directory = rec.path.replace("\\", "/")
                if directory not in cache:
                    cache[directory] = _load_anim(src, directory, rec.x, rec.y, budget)
                anim = cache[directory]
                if anim is None:
                    man_missing += 1
                else:
                    anims.append(AnimSprite(rec.x, rec.y, anim.frames,
                                            anim.offsets, anim.delays, directory))
            meta["MAN"] = f"{len(records)} 条记录，{len(anims)} 条可播放" + (
                f"，{man_missing} 条缺帧" if man_missing else "")
            meta["MAN 表"] = man_ent.key
        else:
            meta["MAN"] = "MAN 表未找到"
    layers.append(Layer("man", "MAN 动画", True, anims=anims))

    # GAL 云雾（mapframe CHANNEL；全屏白雾，默认关，可手动叠加）
    channel = ini.get("mapframe", {}).get("CHANNEL", "")
    if channel:
        gal_ent = src.find_path(channel, (".gal",))
        if gal_ent is not None:
            layers.append(Layer("gal", "云雾", False, image=_decode_image(src, gal_ent)))
            meta["云雾"] = gal_ent.key + "（默认关闭，勾选后叠加）"

    # MSK/core 遮罩（默认关）
    if info["maskfile"]:
        img, static_n, group_n = _mask_overlay(src, info, w, h)
        if img is not None:
            layers.append(Layer("mask", "遮罩", False, image=img))
            meta["遮罩"] = f"静态 {static_n} / 组首帧 {group_n}"

    # CEL 碰撞（默认开，用户要看碰撞区域）
    if info["cellfile"]:
        img = _cel_overlay(src, info["cellfile"], base, w, h, bg_rgb)
        if img is not None:
            layers.append(Layer("cel", "碰撞", True, image=img))
            meta["碰撞"] = "CEL 地面层，蓝色=阻挡"

    # MLK 跳转（默认关）
    stem = os.path.splitext(entry.name)[0]
    img, n_links = _link_overlay(src, stem, w, h)
    if img is not None:
        layers.append(Layer("links", "跳转", False, image=img))
        meta["跳转"] = f"{n_links} 条链路（青框=入口，橙十字=出生格）"

    return SceneData(w, h, bg.convert("RGB"), layers, meta)


def blit_clip(base: Image.Image, img: Image.Image, x: int, y: int):
    """带边界裁剪的 alpha 合成（动画帧可能越出画布）。"""
    w, h = base.size
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(w, x + img.width), min(h, y + img.height)
    if x1 <= x0 or y1 <= y0:
        return
    crop = img.crop((x0 - x, y0 - y, x1 - x, y1 - y))
    base.alpha_composite(crop, (x0, y0))


def composite(scene: SceneData, visible_only: bool = True,
              anim_frame: int = 0) -> Image.Image:
    """把场景合成为一张 RGB 图（导出用）。动画取第 anim_frame 帧。"""
    base = scene.background.convert("RGBA")
    for layer in scene.layers:
        if visible_only and not layer.visible:
            continue
        if layer.image is not None:
            base.alpha_composite(layer.image, (0, 0))
        for anim in layer.anims:
            idx = min(anim_frame, len(anim.frames) - 1)
            ox, oy = anim.offsets[idx]
            blit_clip(base, anim.frames[idx].convert("RGBA"),
                      anim.x + ox, anim.y + oy)
    return base.convert("RGB")
