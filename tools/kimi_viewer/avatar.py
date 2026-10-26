"""纸娃娃装配器：按游戏内规则合成角色（docs/formats/apci.md §3）。

- 部件层叠顺序 = APCI 1B blob 层叠键（perinfo.pak，启动时建一次索引），
  升序绘制（小 key 先画垫底）；body/pet 取 12B 单元 +4（PIF f4）；
- 锚点/帧延时 = 动作目录 perinfo.dat（PIF 行 f2/f3/f5），缺省 (160,210)/80ms；
- 帧图像 = 各部件 APG（含 AI 索引帧调色板，复用 apg_anim 插件路径）；
- 全部资源直接读 PAK，不依赖 extracted。
"""
from __future__ import annotations

import os
from dataclasses import dataclass

from PIL import Image

from dhsh.formats import apci as dapci
from dhsh.formats.pif import parse_pif
from .scene import blit_clip

PARTS = (
    ("body", "身体"), ("head", "头部"), ("weapon", "武器"),
    ("cloth", "衣服"), ("shoe", "鞋子"), ("mask", "面具"),
    ("Lcuff", "左护腕"), ("Rcuff", "右护腕"), ("horse", "坐骑"),
    ("harmorbody", "坐骑甲身"), ("harmorhead", "坐骑甲头"),
    ("pet", "宠物"), ("effect", "特效"),
)
CANVAS_W, CANVAS_H = 640, 480


@dataclass
class PartAnim:
    part: str
    frames: list          # PIL RGBA
    offsets: list         # 每帧 A16 头 x/y
    anchor: tuple         # perinfo 行 f2/f3
    delay: int            # perinfo 行 f5（ms）
    keys: list            # 层叠键（逐帧或单值）
    path: str             # 调试用 APG 路径


class AvatarSource:
    def __init__(self, src):
        self.src = src
        self._keys = None       # (record 名, 模型, 动作) -> {方向: [key...]}
        self._anim_cache = {}   # (pak, part, model, action, direction) -> PartAnim

    # ---- 目录/条目 ----
    def families(self) -> list[str]:
        """有身体/头部部件目录的 PAK（player*.pak 等）。"""
        out = []
        for pak, dirs in self.src._by_pak_dir.items():
            tops = {d.split("/")[0] for d in dirs}
            if "body" in tops or "head" in tops:
                out.append(pak)
        return sorted(out)

    def _dirs_under(self, pak: str, rel: str) -> list[str]:
        prefix = rel.lower() + "/"
        names = set()
        for d in self.src._by_pak_dir.get(pak, {}):
            if d.startswith(prefix):
                names.add(d[len(prefix):].split("/")[0])
        return sorted(names, key=lambda s: (0, int(s)) if s.isdigit() else (1, s))

    def models(self, pak: str, part: str) -> list[str]:
        return self._dirs_under(pak, part)

    def actions(self, pak: str, part: str, model: str) -> list[str]:
        return self._dirs_under(pak, f"{part}/{model}")

    def directions(self, pak: str, part: str, model: str, action: str) -> list[int]:
        out = []
        for e in self.src.list_dir(f"{part}/{model}/{action}"):
            stem = os.path.splitext(e.name)[0]
            if e.ext == ".apg" and stem.isdigit():
                out.append(int(stem))
        return sorted(out)

    def _entry_in(self, pak: str, path: str):
        d, _, name = path.rpartition("/")
        return self.src._by_pak_dir.get(pak, {}).get(d.lower(), {}).get(name.lower())

    # ---- APCI 层叠键索引 ----
    def _ensure_keys(self):
        if self._keys is not None:
            return
        ent = None
        for e in self.src.entries:
            if e.pak == "perinfo.pak":
                ent = e
                break
        if ent is None:
            raise FileNotFoundError("perinfo.pak 不在索引中")
        data, _ = self.src.decode_entry(ent)
        keys: dict = {}
        for (g, gid, r, rid, name, s, a, b, m, header, i,
             x, y, off, size) in dapci.iter_items(data):
            action = header & 0xFFFF
            bpp = (x >> 24) & 0xFF
            slot = keys.setdefault((name, a, action), {})
            if bpp == 1:
                slot[i] = list(data[off:off + size])
            elif bpp == 12:
                slot[i] = [int.from_bytes(data[off + 4 + 12 * k:off + 6 + 12 * k],
                                          "little")
                           for k in range(size // 12)]
            else:
                slot[i] = []
        self._keys = keys

    def keys_for(self, pak: str, part: str, model: int, action: int,
                 direction: int) -> list:
        self._ensure_keys()
        name = f"{os.path.splitext(pak)[0]}\\{part}"
        rec = self._keys.get((name, model, action), {})
        return rec.get(direction, [])

    # ---- 单部件动画 ----
    def part_anim(self, pak: str, part: str, model: int, action: int,
                  direction: int) -> PartAnim | None:
        cache_key = (pak, part, model, action, direction)
        if cache_key in self._anim_cache:
            return self._anim_cache[cache_key]
        rel = f"{part}/{model}/{action}/{direction}.apg"
        ent = self._entry_in(pak, rel)
        if ent is None:
            self._anim_cache[cache_key] = None
            return None
        from .plugins.apg_anim import APGPlugin
        payload, _ = self.src.decode_entry(ent)
        res = APGPlugin().load(payload, payload, ent, self.src)
        frames = res["frames"]
        # 帧头偏移
        from dhsh.formats import apg as dapg
        _, apg_frames = dapg.iter_frames(payload)
        offsets = [(f.header.x, f.header.y) for f in apg_frames[:len(frames)]]
        anchor, delay = (160, 210), 80
        pif_ent = self._entry_in(pak, f"{part}/{model}/{action}/perinfo.dat")
        if pif_ent is not None:
            try:
                pif = parse_pif(self.src.decode_entry(pif_ent)[0])
                if direction < pif.width:
                    row = pif.rows[direction]
                    if len(row) > 3:
                        anchor = (row[2], row[3])
                    if len(row) > 5:
                        delay = max(1, row[5])
            except Exception:  # noqa: BLE001
                pass
        keys = self.keys_for(pak, part, model, action, direction)
        anim = PartAnim(part, frames, offsets, anchor, delay, keys, rel)
        self._anim_cache[cache_key] = anim
        return anim

    # ---- 装配 ----
    def assemble(self, pak: str, parts: dict, action: int, direction: int):
        """parts: {部位: 模型id 或 None}。返回 (帧列表[PIL RGBA], 帧延时ms)。

        总帧数取各部件最大帧数，短帧组循环；逐帧按层叠键升序合成。
        """
        anims = []
        for part, _label in PARTS:
            model = parts.get(part)
            if model is None:
                continue
            anim = self.part_anim(pak, part, model, action, direction)
            if anim is not None:
                anims.append(anim)
        if not anims:
            return [], 80
        total = max(len(a.frames) for a in anims)
        delay = anims[0].delay
        frames = []
        for f in range(total):
            canvas = Image.new("RGBA", (CANVAS_W, CANVAS_H), (0, 0, 0, 0))
            order = []
            for anim in anims:
                idx = f % len(anim.frames)
                key = anim.keys[min(idx, len(anim.keys) - 1)] if anim.keys else 99
                order.append((key, anim.part, anim, idx))
            order.sort(key=lambda t: (t[0], t[1]))
            for _key, _part, anim, idx in order:
                ox, oy = anim.offsets[idx] if idx < len(anim.offsets) else (0, 0)
                # 角色参考点（perinfo f2/f3）对齐画布中心，帧头偏移保持相对布局
                blit_clip(canvas, anim.frames[idx].convert("RGBA"),
                          CANVAS_W // 2 - anim.anchor[0] + ox,
                          CANVAS_H // 2 - anim.anchor[1] + oy)
            frames.append(canvas)
        return frames, delay
