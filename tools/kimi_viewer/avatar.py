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
        family = os.path.splitext(pak)[0]
        name = f"{family}\\{part}" if part else family
        rec = self._keys.get((name, model, action), {})
        return rec.get(direction, [])

    # ---- 单部件动画 ----
    def part_anim(self, pak: str, part: str, model: int, action: int,
                  direction: int, dye=None) -> PartAnim | None:
        cache_key = (pak, part, model, action, direction, dye)
        if cache_key in self._anim_cache:
            return self._anim_cache[cache_key]
        rel = (f"{part}/{model}/{action}/{direction}.apg" if part
               else f"{model}/{action}/{direction}.apg")
        ent = self._entry_in(pak, rel)
        if ent is None:
            self._anim_cache[cache_key] = None
            return None
        from dhsh.formats import apg as dapg
        payload, _ = self.src.decode_entry(ent)
        _, apg_frames = dapg.iter_frames(payload)
        has_ai = any(f.header.magic == b"AI" for f in apg_frames)
        if dye is not None and has_ai:
            # 染色路径：AI 帧改用 hue/sat/val 变体调色板
            from dhsh.formats import a16 as da16
            palette = None
            sib = self.src.resolve_clrtb(ent)
            if sib is not None:
                cti = da16.parse_cti(self.src.decode_entry(sib)[0])
                palette = dye_palette(cti, *dye)
            if palette is None:
                raise ValueError("含 AI 索引帧，未找到 clrtb.cti 调色板")
            frames = []
            for f in apg_frames:
                if f.header.magic == b"AI":
                    frames.append(dapg.frame_to_rgba(f, palette))
                else:
                    from .plugins.image_a16_pg2 import _decode_ap_block
                    frames.append(_decode_ap_block(f.block))
        else:
            from .plugins.apg_anim import APGPlugin
            res = APGPlugin().load(payload, payload, ent, self.src)
            frames = res["frames"]
        offsets = [(f.header.x, f.header.y) for f in apg_frames[:len(frames)]]
        anchor, delay = (160, 210), 80
        pif_rel = (f"{part}/{model}/{action}/perinfo.dat" if part
                   else f"{model}/{action}/perinfo.dat")
        pif_ent = self._entry_in(pak, pif_rel)
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
    def assemble(self, pak: str, parts: dict, action: int, direction: int,
                 dye=None):
        """parts: {部位: 模型id 或 None}。返回 (帧列表[PIL RGBA], 帧延时ms)。

        总帧数取各部件最大帧数，短帧组循环；逐帧按层叠键升序合成。
        dye=(hue, sat_pct, val_pct) 时对 AI 索引帧应用染色变体。
        """
        anims = []
        for part, _label in PARTS:
            model = parts.get(part)
            if model is None:
                continue
            anim = self.part_anim(pak, part, model, action, direction, dye)
            if anim is not None:
                anims.append(anim)
        if not anims:
            return [], 80
        # 骑乘对齐：坐骑与身体按“底心”对齐（修正坐骑与人物分离的问题）
        by_part = {a.part: a for a in anims}
        mount_shift = None
        if "horse" in by_part and "body" in by_part:
            body, horse = by_part["body"], by_part["horse"]
            if body.frames and horse.frames:
                bx = body.offsets[0][0] + body.frames[0].width / 2
                by = body.offsets[0][1] + body.frames[0].height
                hx = horse.offsets[0][0] + horse.frames[0].width / 2
                hy = horse.offsets[0][1] + horse.frames[0].height
                mount_shift = (round(bx - hx), round(by - hy))
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
                ax, ay = anim.anchor
                if mount_shift and _part in ("horse", "harmorbody", "harmorhead"):
                    ax += mount_shift[0]
                    ay += mount_shift[1]
                # 角色参考点（perinfo f2/f3）对齐画布中心，帧头偏移保持相对布局
                blit_clip(canvas, anim.frames[idx].convert("RGBA"),
                          CANVAS_W // 2 - ax + ox, CANVAS_H // 2 - ay + oy)
            frames.append(canvas)
        return frames, delay


# ---------------------------------------------------------------------------
# 技能特效（magic*.pak，Skill.csv 提供技能名与光效模型 ID）
# ---------------------------------------------------------------------------

@dataclass
class SkillInfo:
    id: int
    name: str
    desc: str
    hit: int        # 受击光效模型（0=无）
    attack: int     # 攻击光效模型
    fail: int       # 失败光效模型
    career: int = 0  # 技能所属职业（viewjob 同 ID 空间）


def load_skills(src) -> list[SkillInfo]:
    ent = src.find_path("magic/Skill.csv")
    if ent is None:
        return []
    text = src.decode_entry(ent)[0].decode("gb18030", "replace")
    out = []
    for line in text.splitlines()[1:]:
        cols = line.split(",")
        if len(cols) < 11 or not cols[0].strip().isdigit():
            continue
        def num(i):
            return int(cols[i]) if len(cols) > i and cols[i].strip().isdigit() else 0
        out.append(SkillInfo(num(0), cols[1].strip(), cols[2].strip(),
                             num(8), num(9), num(10), num(3)))
    return out


def magic_variants(src, model: int) -> list[str]:
    """magic 模型可用的变体目录（含 front/back.apg 的目录）。"""
    for pak in ("magic.pak", "magic1.pak", "magic2.pak", "magic3.pak",
                "magic4.pak", "magic5.pak"):
        dirs = src._by_pak_dir.get(pak)
        if not dirs:
            continue
        prefix = f"{model}/"
        found = set()
        for d in dirs:
            if d.startswith(prefix):
                found.add(d[len(prefix):].split("/")[0])
        if found:
            return sorted(found, key=lambda s: (0, int(s)) if s.isdigit() else (1, s))
    return []


class MagicMixer:
    """magic 特效加载：front/back 双层帧，按角色同一参考系摆放。"""

    def __init__(self, src):
        self.src = src
        self._cache = {}

    def anim(self, model: int, variant: str, side: str):
        """side = front/back；返回 (frames, offsets, delay) 或 None。"""
        key = (model, variant, side)
        if key in self._cache:
            return self._cache[key]
        rel = f"{model}/{variant}/{side}.apg"
        ent = None
        for pak in ("magic.pak", "magic1.pak", "magic2.pak", "magic3.pak",
                    "magic4.pak", "magic5.pak"):
            ent = self.src._by_pak_dir.get(pak, {}).get(
                rel.rsplit("/", 1)[0].lower(), {}).get(rel.rsplit("/", 1)[1])
            if ent is not None:
                break
        if ent is None:
            self._cache[key] = None
            return None
        from .plugins.apg_anim import APGPlugin
        from dhsh.formats import apg as dapg
        payload, _ = self.src.decode_entry(ent)
        res = APGPlugin().load(payload, payload, ent, self.src)
        _, apg_frames = dapg.iter_frames(payload)
        offsets = [(f.header.x, f.header.y) for f in apg_frames[:len(res["frames"])]]
        out = (res["frames"], offsets, 80)
        self._cache[key] = out
        return out


def composite_with_magic(avatar_frames: list, magic_back, magic_front,
                         canvas_w: int = CANVAS_W, canvas_h: int = CANVAS_H):
    """把 magic 特效叠到角色帧序列上：back 在角色后、front 在角色前。

    magic_back/front = (frames, offsets, delay) 或 None。
    """
    seqs = [s for s in (magic_back, magic_front) if s]
    total = max([len(avatar_frames)] + [len(s[0]) for s in seqs] + [0])
    if total == 0:
        return avatar_frames
    out = []
    for f in range(total):
        canvas = Image.new("RGBA", (canvas_w, canvas_h), (0, 0, 0, 0))
        def draw(seq):
            frames, offsets, _delay = seq
            idx = f % len(frames)
            ox, oy = offsets[idx] if idx < len(offsets) else (0, 0)
            blit_clip(canvas, frames[idx].convert("RGBA"),
                      canvas_w // 2 - 160 + ox, canvas_h // 2 - 210 + oy)
        if magic_back:
            draw(magic_back)
        if avatar_frames:
            canvas.alpha_composite(avatar_frames[f % len(avatar_frames)], (0, 0))
        if magic_front:
            draw(magic_front)
        out.append(canvas)
    return out


# ---------------------------------------------------------------------------
# CTI 染色（hue/sat/val 变体；HSV 公式已在灰度 CTI 上与 table1 逐值吻合）
# ---------------------------------------------------------------------------

def _hsv_to_rgb(h: int, s: int, v: int):
    if s == 0:
        return v, v, v
    sector, frac = divmod(h, 42)
    sector %= 6
    f = frac / 42.0
    p = v * (255 - s) // 255
    q = v * (255 - int(s * f)) // 255
    t = v * (255 - int(s * (1.0 - f))) // 255
    if sector == 0:
        return v, t, p
    if sector == 1:
        return q, v, p
    if sector == 2:
        return p, v, t
    if sector == 3:
        return p, q, v
    if sector == 4:
        return t, p, v
    return v, p, q


def _rgb565(r: int, g: int, b: int) -> int:
    return ((r * 31 // 255) << 11) | ((g * 63 // 255) << 5) | (b * 31 // 255)


def dye_palette(cti, hue: int, sat_pct: int, val_pct: int):
    """由 CTI table2（HSV 主表）生成染色后的 table1（RGB565 调色板）。

    hue: 0..251（42 扇区轮）；sat_pct/val_pct: 0..200（百分比）。
    """
    import struct
    from types import SimpleNamespace
    table = []
    for v32 in cti.table2:
        h, s, v, _pad = struct.unpack("<4B", struct.pack("<I", v32))
        h = (h + hue) % 252
        s = min(255, s * sat_pct // 100)
        v = min(255, v * val_pct // 100)
        table.append(_rgb565(*_hsv_to_rgb(h, s, v)))
    return SimpleNamespace(table1=tuple(table), table2=cti.table2)


# ---------------------------------------------------------------------------
# NPC 单模型（npc*.pak：<model>/<action>/<dir>.apg，无部位段；
# 名字取 config.pak/monster/Monsters.csv 的 shape→怪物名字）
# ---------------------------------------------------------------------------

def load_monster_names(src) -> dict[int, str]:
    ent = src.find_path("monster/Monsters.csv")
    if ent is None:
        return {}
    text = src.decode_entry(ent)[0].decode("gb18030", "replace")
    names = {}
    for line in text.splitlines()[1:]:
        cols = line.split(",")
        if len(cols) > 1 and cols[0].strip().isdigit():
            names[int(cols[0])] = cols[1].strip()
    return names


class NpcSource(AvatarSource):
    def families(self) -> list[str]:
        return sorted(pak for pak in self.src._by_pak_dir
                      if pak.startswith("npc"))

    def models(self, pak: str, part: str = "") -> list[str]:
        tops = {d.split("/")[0] for d in self.src._by_pak_dir.get(pak, {}) if d}
        return sorted((m for m in tops if m.isdigit()), key=int)

    def actions(self, pak: str, part: str, model: str) -> list[str]:
        return self._dirs_under(pak, model)

    def assemble_npc(self, pak: str, model: int, action: int, direction: int):
        anim = self.part_anim(pak, "", model, action, direction)
        if anim is None:
            return [], 80
        out = []
        for i, frame in enumerate(anim.frames):
            canvas = Image.new("RGBA", (CANVAS_W, CANVAS_H), (0, 0, 0, 0))
            ox, oy = anim.offsets[i] if i < len(anim.offsets) else (0, 0)
            blit_clip(canvas, frame.convert("RGBA"),
                      CANVAS_W // 2 - anim.anchor[0] + ox,
                      CANVAS_H // 2 - anim.anchor[1] + oy)
            out.append(canvas)
        return out, anim.delay


# ---------------------------------------------------------------------------
# 职业（config.pak/ui/login/viewjob.csv：职业/进阶/坐骑/性别 → 装备模型预设）
# ---------------------------------------------------------------------------

CAREER_NAMES = ("初心者", "剑客", "武师", "医师", "术士", "道士", "浪子",
                "弓手", "禁卫", "墨者", "巫煞", "魂武", "寒星", "花间舞",
                "剑九", "魔刀", "百变")
ADVANCE_NAMES = ("职业", "英雄", "星将")
MOUNT_NAMES = ("徒步", "坐骑", "神骑")


def load_viewjob(src) -> dict:
    """返回 {(职业, 进阶, 坐骑档, 性别): {部位: 模型id}}（-1 = 无）。"""
    ent = src.find_path("ui/login/viewjob.csv")
    if ent is None:
        return {}
    text = src.decode_entry(ent)[0].decode("gb18030", "replace")
    out = {}
    for line in text.splitlines()[1:]:
        cols = line.split(",")
        if len(cols) < 11 or not cols[0].strip().isdigit():
            continue
        vals = [int(c) if c.strip().lstrip("-").isdigit() else -1 for c in cols[:11]]
        career, advance, mount, gender = vals[:4]
        cloth, gloves, shoes, weapon, horse, hhead, hbody = vals[4:11]
        out[(career, advance, mount, gender)] = {
            "cloth": cloth, "Lcuff": gloves, "Rcuff": gloves,
            "shoe": shoes, "weapon": weapon, "horse": horse,
            "harmorhead": hhead, "harmorbody": hbody,
        }
    return out


def all_part_models(src, part: str) -> list[tuple[str, int]]:
    """跨 player*.pak 枚举某部位的全部模型：[(pak, model_id)]。"""
    out = []
    for pak, dirs in src._by_pak_dir.items():
        if not pak.startswith("player"):
            continue
        prefix = part + "/"
        models = set()
        for d in dirs:
            if d.startswith(prefix):
                models.add(d[len(prefix):].split("/")[0])
        out.extend((pak, int(m)) for m in sorted(models, key=int))
    return out


def all_npc_models(src) -> list[tuple[str, int]]:
    """跨 npc*.pak 枚举全部 NPC 模型：[(pak, model_id)]。"""
    out = []
    for pak, dirs in src._by_pak_dir.items():
        if not pak.startswith("npc"):
            continue
        models = {d.split("/")[0] for d in dirs if d}
        out.extend((pak, int(m)) for m in sorted(models, key=int))
    return out
