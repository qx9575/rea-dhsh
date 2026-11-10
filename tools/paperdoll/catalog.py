"""职业预设 → 部件资源清单。"""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

from dhsh.formats import apg as dapg
from dhsh.formats.pif import parse_pif

from art_pipeline import careers, inventory, tables
from art_pipeline.common import PakReader


PART_FIELDS = (
    ("body", "body", 200),
    ("head", "head", 105),
    ("mask", "mask", None),
    ("cloth", "cloth", None),
    ("gloves_l", "lcuff", None),
    ("gloves_r", "rcuff", None),
    ("shoes", "shoe", None),
    ("weapon", "weapon", None),
    ("horse", "horse", None),
    ("harmor_head", "harmorhead", None),
    ("harmor_body", "harmorbody", None),
)

# itemSpecData 槽位 → 玩家部件目录。10 帽子→head、20 衣服→cloth、40 武器→weapon、
# 90 鞋子→shoe、100 面罩→mask、110 护手→lcuff/rcuff、270 骑甲→harmorbody；
# 80 腰带沿用 kimi_viewer 的 mask 约定。映射来源：resmirror part type + 参考查看器，
# 尚未经原生客户端运行时验证（见 README）。
ITEM_SLOT_PART = {
    10: ("head",),
    20: ("cloth",),
    40: ("weapon",),
    80: ("mask",),
    90: ("shoe",),
    100: ("mask",),
    110: ("lcuff", "rcuff"),
    270: ("harmorbody",),
}

# 运行时实测的装备外观（2026-11-09，1.50.27 内存+截图；见 asset-spec §4.3）。
# item_id -> (部件目录, 模型或 None=无可见变化, 色相角度, 备注)
ITEM_APPEARANCE = {
    1400: (("head",), None, 0, "穿脱无可见变化（帽子隐藏）"),
    2450: (("cloth",), 453, 180, "原始橙黄，客户端 +180° 呈宝蓝"),
    4400: (("weapon",), 450, 0, "武器另有额外光晕待定位"),
    8400: (("mask",), 450, 0, "腰带"),
    9450: (("shoe",), 453, 180, ""),
    11450: (("lcuff", "rcuff"), 453, 180,
            "客户端手腕为蓝色（与衣服同色相）；此前按金色原色处理有误"),
}

# 武器光效（part type 8）：item_id -> (effect 模型, 色相角度)。
# 4400 桃木杖：effect/450，bodyidequal 光效颜色 90 → 90/255*360 ≈ 127°。
ITEM_EFFECT = {
    4400: (450, 127),
}

# 星将模式下已确认的高阶物品外观；优先于低阶 item_id 公式。
# 来源于 bodyidequal 36600（浪子星将·男）与 2026-11-10 用户目视确认。
STAR_GENERAL_ITEM_APPEARANCE = {
    1612: (("head",), 601, 0, "神佑之冠（浪）：36600 head 611 缺失，回退 601"),
    2641: (("cloth",), 621, 0, "神佑仙袍（浪）：36600 cloth 621"),
    4640: (("weapon",), 621, 0, "神佑血匕（浪）：36600 weapon 621"),
    8612: (("mask",), None, 0, "神佑神带（浪）：36600 mask -1，不可见"),
    9638: (("shoe",), 621, 0, "神佑仙靴（浪）：36600 shoe 621"),
    11638: (("lcuff", "rcuff"), 621, 0, "神佑仙腕（浪）：36600 cuff 621"),
}

# 部件目录 → paperdoll 字段名（与 PART_FIELDS 一致）。
PART_FIELD = {
    "body": "body", "head": "head", "mask": "mask", "cloth": "cloth",
    "lcuff": "gloves_l", "rcuff": "gloves_r", "shoe": "shoes",
    "weapon": "weapon", "horse": "horse", "harmorhead": "harmor_head",
    "harmorbody": "harmor_body", "effect": "effect",
}

# 部件目录 → resmirror 部件类型（config.pak:misc/resmirror.csv）。
# 0 头部 / 1 身体 / 2 武器 / 7 衣服 / 9 脸部 / 10 手腕 / 11 脚部 / 12 右腕 /
# 13 坐骑 / 14 骑甲身 / 15 骑甲头；当前表只定义 0/1/2/4/7/9/10/11/12。
PART_MIRROR_TYPE = {
    "head": 0, "body": 1, "weapon": 2, "cloth": 7, "mask": 9,
    "lcuff": 10, "shoe": 11, "rcuff": 12, "horse": 13,
    "harmorbody": 14, "harmorhead": 15, "effect": 8,
}

# resmirror 的“动作组”别名（表头注释）：这些行的朝向和转换后动作/朝向被忽略，
# 展开为组内每个具体动作；组内动作不能作为普通精确键。
MIRROR_ALIAS_GROUPS = {
    10000: frozenset((10, 30)),
    10001: frozenset(range(10, 91, 10)),
    10002: frozenset((10, 20, 30, 50, 60, 70, 80)),
    10003: frozenset((10, 20, 30, 50, 60, 70, 80, 91, 92)),
    10004: frozenset(range(10, 101, 10)) | {91, 101},
}

# 玩家部件缺失时允许回退到 npc*.pak 的部位。神骑（mount tier 2）的
# horse/harmor 模型（如 20000/20700/21700）是 NPC 风格资源，
# 路径为 npc*.pak:<model>/<action>/<dir>.apg。
NPC_FALLBACK_PARTS = frozenset(("horse", "harmorbody", "harmorhead"))


class MirrorIndex:
    """resmirror：按 (部件类型, 部件 id, 动作, 朝向) 替换资源模型。

    表头语义（config.pak:misc/resmirror.csv）：
      部件类型, 部件id, 动作id, 朝向id, 转换后部件id, 转换后动作id, 转换后朝向id
    动作 10000–10004 是动作组别名：忽略朝向与“转换后动作/朝向”，只取转换后部件 id。
    0x8a6490（1.50.27）把键打包为 part_type*10+dir、action&0x7f、model&0xfffff，
    与本实现一致；direction/action 输出在现有数据中除别名外保持不变。
    """

    def __init__(self, table=None):
        self.exact: dict[tuple[int, int, int, int], tuple[int, int, int]] = {}
        self.alias: dict[tuple[int, int, int], int] = {}
        self.rows = 0
        if table is None:
            return
        for row in table.rows:
            if len(row) < 7:
                continue
            try:
                pt, pid, action, direction, pid2, action2, direction2 = (
                    int(cell) for cell in row[:7])
            except ValueError:
                continue
            self.rows += 1
            if action in MIRROR_ALIAS_GROUPS:
                self.alias[(pt, pid, action)] = pid2
            else:
                self.exact[(pt, pid, action, direction)] = (pid2, action2, direction2)

    def convert(self, part_type: int, model: int, action: int,
                direction: int) -> tuple[int, int, int, str]:
        hit = self.exact.get((part_type, model, action, direction))
        if hit is not None:
            return hit[0], hit[1], hit[2], "exact"
        for alias, group in MIRROR_ALIAS_GROUPS.items():
            if action in group:
                target = self.alias.get((part_type, model, alias))
                if target is not None:
                    return target, action, direction, f"alias:{alias}"
        return model, action, direction, ""


def _color_to_degrees(value: int) -> int:
    """bodyidequal 的 0–251 颜色参数 → HSV 色相角度（90→127°）。"""
    if value <= 0:
        return 0
    return round((value % 256) / 255 * 360)


@dataclass(frozen=True)
class StarGeneralRow:
    """bodyidequal 36xxx/38xxx 的星将/星神形态外观。"""

    star_id: int
    career: int
    gender: int
    variant: int
    body: int
    head: int
    cloth: int
    weapon: int
    effect: int
    shoe: int
    mask: int
    cuff: int
    hues: dict[str, int] = field(default_factory=dict)

    def as_dict(self) -> dict:
        return {
            "star_id": self.star_id,
            "career": self.career,
            "gender": self.gender,
            "variant": self.variant,
            "body": self.body,
            "head": self.head,
            "cloth": self.cloth,
            "weapon": self.weapon,
            "effect": self.effect,
            "shoe": self.shoe,
            "mask": self.mask,
            "cuff": self.cuff,
            "hues": dict(self.hues),
        }


class StarGeneralIndex:
    """bodyidequal 36xxx/38xxx 星将/星神形态外观表。

    行格式（16 列）：
        形态ID, body, head, head_color, cloth, cloth_color, weapon, weapon_color,
        effect, effect_color, shoe, shoe_color, mask, mask_color, cuff, cuff_color

    形态 ID 编码：36xNN = 职业 x（1–9）的星将；380NN–386NN = 职业 10–16。
    末位 5 为女性、其余为男性；十位是 variant（0–3）。
    颜色字段 0–251，按 HSV 色相角度使用（90 → 127°）。
    """

    # 已由运行时截图/用户确认的默认形态；其余职业暂按同职业同性别的最低 variant 回退。
    KNOWN_DEFAULTS = {
        (6, 0): 36600,   # 浪子星将·男
        (6, 1): 36615,   # 浪子星将·女（潜影）
    }

    def __init__(self, table=None):
        self.rows: dict[int, StarGeneralRow] = {}
        if table is None:
            return
        for row in table.rows:
            if len(row) < 16:
                continue
            try:
                sid = int(row[0])
            except ValueError:
                continue
            if not (36100 <= sid < 37000 or 38000 <= sid < 39000):
                continue
            try:
                body, head, head_c, cloth, cloth_c, weapon, weapon_c, \
                    effect, effect_c, shoe, shoe_c, mask, mask_c, cuff, cuff_c = (
                        int(cell) for cell in row[1:16])
            except ValueError:
                continue
            if 36100 <= sid < 37000:
                career = (sid // 100) % 10
            else:
                career = 10 + (sid // 100) % 10
            gender = 1 if sid % 10 == 5 else 0
            variant = (sid // 10) % 10
            self.rows[sid] = StarGeneralRow(
                star_id=sid, career=career, gender=gender, variant=variant,
                body=body, head=head, cloth=cloth, weapon=weapon,
                effect=effect, shoe=shoe, mask=mask, cuff=cuff,
                hues={
                    "head": _color_to_degrees(head_c),
                    "cloth": _color_to_degrees(cloth_c),
                    "weapon": _color_to_degrees(weapon_c),
                    "effect": _color_to_degrees(effect_c),
                    "shoe": _color_to_degrees(shoe_c),
                    "mask": _color_to_degrees(mask_c),
                    "cuff": _color_to_degrees(cuff_c),
                },
            )

    def get(self, star_id: int | None) -> StarGeneralRow | None:
        if star_id is None:
            return None
        return self.rows.get(int(star_id))

    def default(self, career: int, gender: int) -> StarGeneralRow | None:
        known = self.KNOWN_DEFAULTS.get((career, gender))
        if known is not None:
            return self.get(known)
        candidates = [
            row for row in self.rows.values()
            if row.career == career and row.gender == gender
        ]
        if not candidates:
            return None
        return min(candidates, key=lambda row: (row.variant, row.star_id))


@dataclass(frozen=True)
class MountRow:
    """horseinfo.csv 的坐骑元数据（职业/等级/类型/品级）。"""

    mount_id: int
    name: str
    monster_id: int
    best_career: int
    level: int
    flying: int
    mount_type: str
    grade: str

    def as_dict(self) -> dict:
        return {
            "mount_id": self.mount_id,
            "name": self.name,
            "monster_id": self.monster_id,
            "best_career": self.best_career,
            "level": self.level,
            "flying": self.flying,
            "mount_type": self.mount_type,
            "grade": self.grade,
        }


class MountIndex:
    """坐骑元数据与通用模型解析。

    - `horseinfo`：坐骑 ID → 名称/怪物 ID/最佳职业/等级/类型/品级；
    - `viewjob`：职业/进阶/性别/坐骑等级 → mount/harmor ID（preset 已带）；
    - `horseoffset`：坐骑模型 → 挂点偏移（由 PaperDollCatalog._mount_attach 使用）。

    资源模型按“horse + harmor 回退”通用处理，不为 6100 等单个模型写分支。
    """

    # 运行时观察到的模型名（非 horseinfo 的坐骑 ID 空间）。
    MODEL_NOTES = {
        2000: {"name": "金鸟", "source": "sample (descriptive)"},
        3750: {"name": "鹿", "source": "runtime"},
        4200: {"name": "蓝豹 A", "source": "sample (descriptive)"},
        4250: {"name": "蓝豹 B", "source": "sample (descriptive)"},
        4300: {"name": "蓝紫兽", "source": "sample (descriptive)"},
        4400: {"name": "凤凰", "source": "runtime"},
        4450: {"name": "火凤变体", "source": "sample (descriptive)"},
        4500: {"name": "火兽", "source": "sample (descriptive)"},
        4550: {"name": "阴阳赤炎兽", "source": "runtime"},
        6000: {"name": "龙/坐骑模型 6000", "source": "runtime"},
        6100: {"name": "赤炎龙", "source": "runtime 2026-11-10"},
        6150: {"name": "红色火龙变体（6150）", "source": "runtime"},
        7600: {"name": "浪子神骑（男）", "source": "viewjob"},
        7650: {"name": "浪子神骑（女）", "source": "viewjob"},
        8250: {"name": "棕兽", "source": "sample (descriptive)"},
        8300: {"name": "骷髅号", "source": "runtime"},
        8450: {"name": "白兔", "source": "sample (descriptive)"},
        8900: {"name": "紫狼", "source": "sample (descriptive)"},
        12950: {"name": "金环坐骑", "source": "sample (descriptive)"},
        14300: {"name": "滑雪坐骑", "source": "sample (descriptive)"},
        14650: {"name": "冰龙", "source": "runtime 2026-11-10"},
        15010: {"name": "双鱼坐骑", "source": "sample (descriptive)"},
        15070: {"name": "蓝鲸坐骑", "source": "sample (descriptive)"},
    }

    def __init__(self, table=None):
        self.rows: dict[int, MountRow] = {}
        if table is None:
            return
        for row in table.rows:
            if len(row) < 11:
                continue
            try:
                mount_id = int(row[0])
                monster_id = int(row[1])
                level = int(row[2])
                best_career = int(row[3])
                flying = int(row[7])
            except ValueError:
                continue
            self.rows[mount_id] = MountRow(
                mount_id=mount_id, name=row[5], monster_id=monster_id,
                best_career=best_career, level=level, flying=flying,
                mount_type=row[9], grade=row[10],
            )

    def get(self, mount_id: int | None) -> MountRow | None:
        if mount_id is None:
            return None
        return self.rows.get(int(mount_id))

    def note(self, model: int | None) -> dict:
        if model is None:
            return {}
        return dict(self.MODEL_NOTES.get(int(model), {}))

    def resolve(self, preset, mount_model: int | None = None,
                mount_id: int | None = None, form: str = "ride") -> dict:
        """通用坐骑解析。

        form="ride"：人物骑在 horse 上，harmor 回退同 ID；
        form="god"：神骑/骑甲一体，body/head/horse/harmor 都替换为坐骑族模型，
        装备层由调用方隐藏（body/head 的实际资源由 parts 解析）。
        """
        row = self.get(mount_id)
        model = mount_model if mount_model and mount_model > 0 else None
        if model is None and preset is not None and preset.horse > 0:
            model = preset.horse
        harmor_head = model
        harmor_body = model
        if form == "god":
            return {
                "mount_id": mount_id,
                "mount_model": model,
                "form": "god",
                "body": model,
                "horse": model,
                "harmor_head": model,
                "harmor_body": model,
                "name": (row.name if row else self.note(model).get("name")),
                "source": self.note(model).get("source") or ("horseinfo" if row else None),
                "horseinfo": row.as_dict() if row else None,
            }
        if preset is not None and preset.harmor_head > 0:
            harmor_head = preset.harmor_head
        if preset is not None and preset.harmor_body > 0:
            harmor_body = preset.harmor_body
        note = self.note(model)
        return {
            "mount_id": mount_id,
            "mount_model": model,
            "form": "ride",
            "horse": model,
            "harmor_head": harmor_head,
            "harmor_body": harmor_body,
            "name": (row.name if row else note.get("name")),
            "source": note.get("source") or ("horseinfo" if row else None),
            "horseinfo": row.as_dict() if row else None,
        }


@dataclass(frozen=True)
class SummonRow:
    """monsters.csv 的 shape/名称；召唤兽外观按 shape 解析。"""

    shape: int
    names: tuple[str, ...]
    element: int
    race: int
    subtype: int
    next_shape: int
    weight: int
    category: str

    def as_dict(self) -> dict:
        return {
            "shape": self.shape,
            "names": list(self.names),
            "element": self.element,
            "race": self.race,
            "subtype": self.subtype,
            "next_shape": self.next_shape,
            "weight": self.weight,
            "category": self.category,
        }


class SummonIndex:
    """monsters.csv → shape/名称索引；任何 shape 都可用 --summon 渲染。"""

    KNOWN_PET_NAMES = {
        "水水", "浒浒", "穷奇", "天神", "饕餮", "辟邪", "来福", "旺财",
        "皮皮", "格格", "团团", "圆圆", "悠悠", "嘻嘻",
    }

    def __init__(self, table=None):
        self.by_shape: dict[int, SummonRow] = {}
        self.by_name: dict[str, list[int]] = {}
        if table is None:
            return
        names_by_shape: dict[int, list[str]] = {}
        meta: dict[int, tuple[int, int, int, int, int]] = {}
        for row in table.rows:
            if len(row) < 8:
                continue
            try:
                shape = int(row[0])
                element = int(row[2])
                race = int(row[3])
                subtype = int(row[4])
                next_shape = int(row[6])
                weight = int(row[7])
            except ValueError:
                continue
            names_by_shape.setdefault(shape, []).append(row[1])
            meta[shape] = (element, race, subtype, next_shape, weight)
            self.by_name.setdefault(row[1], []).append(shape)
        for shape, names in names_by_shape.items():
            element, race, subtype, next_shape, weight = meta[shape]
            joined = " ".join(names)
            if "魔星" in joined:
                category = "moxing"
            elif "迷你" in joined:
                category = "mini"
            elif any(name in self.KNOWN_PET_NAMES for name in names):
                category = "pet"
            else:
                category = "monster"
            self.by_shape[shape] = SummonRow(
                shape=shape, names=tuple(names), element=element, race=race,
                subtype=subtype, next_shape=next_shape, weight=weight,
                category=category,
            )

    def get(self, shape: int | None) -> SummonRow | None:
        if shape is None:
            return None
        return self.by_shape.get(int(shape))

    def find_name(self, name: str) -> list[int]:
        return sorted(set(self.by_name.get(name, [])))


@dataclass
class PartAsset:
    field: str
    part: str
    model: int
    pak: str = ""
    apg: str = ""
    exists: bool = False
    frames: int = 0
    perinfo: str = ""
    perinfo_exists: bool = False
    anchor: tuple[int, int] | None = None
    delay: int | None = None
    cti: str = ""
    keys: list[int] = field(default_factory=list)
    status: str = ""
    found_in: list[str] = field(default_factory=list)
    actions: list[int] = field(default_factory=list)
    directions: list[int] = field(default_factory=list)
    resolved_model: int | None = None
    resolved_action: int | None = None
    resolved_direction: int | None = None
    mirror_rule: str = ""
    hue: int = 0
    note: str = ""
    frame_offset: int = 0
    center_frames: bool = False

    def as_dict(self) -> dict:
        return {
            "field": self.field,
            "part": self.part,
            "model": self.model,
            "resolved_model": self.resolved_model,
            "resolved_action": self.resolved_action,
            "resolved_direction": self.resolved_direction,
            "mirror_rule": self.mirror_rule,
            "hue": self.hue,
            "frame_offset": self.frame_offset,
            "center_frames": self.center_frames,
            "pak": self.pak,
            "apg": self.apg,
            "exists": self.exists,
            "frames": self.frames,
            "perinfo": self.perinfo,
            "perinfo_exists": self.perinfo_exists,
            "anchor": list(self.anchor) if self.anchor else None,
            "delay": self.delay,
            "cti": self.cti,
            "keys": self.keys,
            "status": self.status,
            "found_in": self.found_in,
            "actions": self.actions,
            "directions": self.directions,
            "note": self.note,
        }


@dataclass
class PaperDollCatalog:
    shdata: Path
    reader: PakReader
    tables: dict
    presets: list
    index: inventory.InventoryIndex
    keys: object
    model_parts: dict[int, set[str]] = field(default_factory=dict)
    mirror: MirrorIndex = field(default_factory=MirrorIndex)
    star_general: StarGeneralIndex = field(default_factory=StarGeneralIndex)
    mounts: MountIndex = field(default_factory=MountIndex)
    summons: SummonIndex = field(default_factory=SummonIndex)

    @classmethod
    def load(cls, shdata: Path, inventory_path: Path | None = None,
             load_keys: bool = True) -> "PaperDollCatalog":
        from paperdoll.reader import CachedPakReader

        reader = CachedPakReader(PakReader(shdata))
        loaded = tables.load_tables(reader)
        preset_list = careers.parse_presets(loaded["viewjob"])
        entries = inventory.load_entries(shdata, inventory_path)
        index = inventory.build_index(entries)
        model_parts: dict[int, set[str]] = {}
        for part_key, model in index.models:
            model_parts.setdefault(model, set()).add(part_key)
        keys = None
        if load_keys:
            from paperdoll.keys import LayerKeyIndex
            keys = LayerKeyIndex(reader)
        mirror = MirrorIndex(loaded.get("resmirror"))
        star_general = StarGeneralIndex(loaded.get("bodyidequal"))
        mounts = MountIndex(loaded.get("horseinfo"))
        summons = SummonIndex(loaded.get("monsters"))
        return cls(shdata, reader, loaded, preset_list, index, keys,
                   model_parts, mirror, star_general, mounts, summons)

    def find_preset(self, career: int, advance: int, mount_tier: int, gender: int):
        for p in self.presets:
            if (p.career, p.advance, p.mount_tier, p.gender) == (
                    career, advance, mount_tier, gender):
                return p
        return None

    def _magic_index(self) -> dict[int, dict]:
        """magic*.pak 的 <model>/<variant>/front.apg|back.apg 索引（懒加载）。"""
        cached = getattr(self, "_magic", None)
        if cached is not None:
            return cached
        index: dict[int, dict] = {}
        for fp in sorted(self.shdata.glob("magic*.pak")):
            pak = fp.name
            for path, _name, _offset, _size in self.reader.entries(pak):
                seg = path.replace("\\", "/").split("/")
                if len(seg) < 3:
                    continue
                try:
                    model = int(seg[0])
                except ValueError:
                    continue
                rec = index.setdefault(model, {"pak": pak, "variants": set(),
                                               "files": set()})
                rec["variants"].add(seg[1])
                rec["files"].add(seg[2].lower())
        self._magic = index
        return index

    def resolve_effect(self, effect_id: int | None) -> dict:
        """技能光效 ID → magic 模型 + 变体/前后层文件。"""
        if effect_id is None or effect_id <= 0:
            return {"id": effect_id, "exists": False, "note": "未配置"}
        rec = self._magic_index().get(effect_id)
        if rec is None:
            return {"id": effect_id, "exists": False, "note": "magic*.pak 中未找到"}
        return {
            "id": effect_id,
            "exists": True,
            "pak": rec["pak"],
            "variants": sorted(rec["variants"], key=int),
            "files": sorted(rec["files"]),
        }

    def skill_effect_id(self, skill_id: int, kind: str = "attack") -> int | None:
        """Skill.csv 技能 ID → 受击/攻击/失败光效 ID。"""
        index = {"hit": 8, "attack": 9, "fail": 10}.get(kind)
        if index is None:
            return None
        table = self.tables.get("skill")
        if table is None:
            return None
        for row in table.rows:
            if len(row) <= index:
                continue
            try:
                if int(row[0]) != int(skill_id):
                    continue
            except ValueError:
                continue
            try:
                value = int(row[index])
            except ValueError:
                return None
            return value if value > 0 else None
        return None

    def resolve_skill_effect(self, effect_id: int, variant: int | str | None = None,
                             layer: str = "front",
                             parse_frames: bool = True) -> PartAsset:
        """技能光效：magic*.pak:<id>/<variant>/front.apg|back.apg。

        layer="back" 绘制在人物后方，layer="front" 绘制在前景。
        """
        asset = PartAsset(f"skill_{layer}", f"skill_{layer}", effect_id)
        asset.center_frames = True
        rec = self._magic_index().get(effect_id)
        if rec is None:
            asset.status = "missing"
            asset.note = "magic*.pak 中未找到光效"
            return asset
        variants = sorted(rec["variants"], key=lambda v: int(v) if v.isdigit() else v)
        if variant is not None and str(variant) in variants:
            var = str(variant)
        elif variants:
            var = variants[0]
        else:
            var = "0"
        asset.pak = rec["pak"]
        asset.apg = f"{effect_id}/{var}/{layer}.apg"
        asset.resolved_action = int(var) if var.isdigit() else None
        asset.status = "found" if layer + ".apg" in rec["files"] else "apg_missing"
        hit = self.reader.find(rec["pak"], asset.apg, exact=True)
        asset.exists = hit is not None
        if not asset.exists:
            asset.status = "apg_missing"
            asset.note = f"magic {effect_id}/{var}/{layer}.apg 不存在"
        if hit is not None and parse_frames:
            payload = self.reader.read(rec["pak"], asset.apg, exact=True)
            if payload is not None:
                try:
                    _hdr, frames = dapg.iter_frames(payload[1])
                    asset.frames = len(frames)
                except Exception:  # noqa: BLE001
                    pass
        return asset

    def effect_coverage(self) -> dict:
        """Skill.csv 全部光效 ID 在 magic*.pak 中的覆盖统计。"""
        ids: set[int] = set()
        table = self.tables.get("skill")
        if table is not None:
            for row in table.rows:
                if len(row) < 11:
                    continue
                for index in (8, 9, 10):
                    try:
                        value = int(row[index])
                    except ValueError:
                        continue
                    if value > 0:
                        ids.add(value)
        missing = sorted(effect_id for effect_id in ids
                         if effect_id not in self._magic_index())
        return {
            "effect_ids": len(ids),
            "resolved": len(ids) - len(missing),
            "missing": missing,
        }

    def _item_index(self) -> dict[int, dict]:
        cached = getattr(self, "_items", None)
        if cached is not None:
            return cached
        items: dict[int, dict] = {}
        table = self.tables.get("item_spec")
        if table is not None:
            for row in table.rows:
                if len(row) < 31:
                    continue
                try:
                    item_id = int(row[0])
                    slot = int(row[30])
                except ValueError:
                    continue
                items[item_id] = {"item_id": item_id, "name": row[2],
                                  "slot": slot, "model": item_id - slot * 100}
        self._items = items
        return items

    def item_lookup(self, item_id: int) -> dict | None:
        return self._item_index().get(item_id)

    def equip_overrides(self, item_ids: list[int],
                        star_general: bool = False) -> tuple[dict[str, int],
                                                             dict[str, int],
                                                             list[dict]]:
        """物品 ID → 部件模型/色相覆盖；返回 (overrides, hues, 解析明细)。

        星将模式优先使用 `STAR_GENERAL_ITEM_APPEARANCE`（如神佑浪套 621），
        否则使用运行时实测的 `ITEM_APPEARANCE`；未实测物品回退到低阶公式
        `item_id % 1000 + 50/3`，并在明细中标注。
        """
        overrides: dict[str, int] = {}
        hues: dict[str, int] = {}
        resolved: list[dict] = []
        for item_id in item_ids or []:
            rec = self.item_lookup(item_id)
            if rec is None:
                resolved.append({"item_id": item_id, "status": "item_not_found"})
                continue
            measured = None
            status = "measured"
            if star_general:
                measured = STAR_GENERAL_ITEM_APPEARANCE.get(item_id)
                if measured is not None:
                    status = "star_general_measured"
            if measured is None:
                measured = ITEM_APPEARANCE.get(item_id)
            if measured is not None:
                part_list, model, hue, note = measured
                fields = [PART_FIELD[p] for p in part_list]
                if model is not None:
                    for field_name in fields:
                        overrides[field_name] = model
                        hues[field_name] = hue
                resolved.append({**rec, "part": list(part_list), "field": fields,
                                 "appearance_model": model, "hue": hue,
                                 "status": status, "note": note})
                if status == "measured":
                    effect = ITEM_EFFECT.get(item_id)
                    if effect is not None:
                        effect_model, effect_hue = effect
                        overrides["effect"] = effect_model
                        hues["effect"] = effect_hue
                        resolved[-1]["effect_model"] = effect_model
                        resolved[-1]["effect_hue"] = effect_hue
                continue
            part_list = ITEM_SLOT_PART.get(rec["slot"])
            if not part_list:
                resolved.append({**rec, "status": "unknown_slot"})
                continue
            fields = [PART_FIELD[p] for p in part_list]
            model = rec["model"]
            for field_name in fields:
                overrides[field_name] = model
                hues[field_name] = 0
            resolved.append({**rec, "part": list(part_list), "field": fields,
                             "appearance_model": model, "hue": 0,
                             "status": "formula_fallback",
                             "note": "未实测，沿用 item_id % 1000 + 槽位偏移"})
        return overrides, hues, resolved

    def _resolve_npc(self, asset: PartAsset, npc_info, model: int,
                     action: int, direction: int,
                     parse_frames: bool) -> PartAsset:
        """npc*.pak:<model>/<action>/<dir>.apg（神骑/骑甲等 NPC 风格模型）。"""
        asset.pak = npc_info.pak
        asset.apg = f"{model}/{action}/{direction}.apg"
        asset.actions = sorted(npc_info.actions)
        asset.directions = sorted(npc_info.directions)
        hit = self.reader.find(npc_info.pak, asset.apg, exact=True)
        asset.exists = hit is not None
        if asset.exists:
            asset.status = "found_npc"
        else:
            asset.status = "apg_missing"
            asset.note = f"npc {action}/{direction}.apg 不存在"
        if hit is not None and parse_frames:
            payload = self.reader.read(npc_info.pak, asset.apg, exact=True)
            if payload is not None:
                try:
                    _hdr, frames = dapg.iter_frames(payload[1])
                    asset.frames = len(frames)
                except Exception:  # noqa: BLE001
                    pass
        return asset

    def _resolve(self, field_name: str, part: str, model: int | None,
                 action: int, direction: int, parse_frames: bool) -> PartAsset | None:
        if model is None or model < 0:
            return None
        mirror_type = PART_MIRROR_TYPE.get(part)
        resolved_model, resolved_action, resolved_dir, rule = model, action, direction, ""
        if mirror_type is not None:
            resolved_model, resolved_action, resolved_dir, rule = self.mirror.convert(
                mirror_type, model, action, direction)
        asset = PartAsset(field_name, part, model)
        asset.resolved_model = resolved_model
        asset.resolved_action = resolved_action
        asset.resolved_direction = resolved_dir
        asset.mirror_rule = rule
        info = self.index.best_model(part, resolved_model)
        npc_info = None
        if info is None and part in NPC_FALLBACK_PARTS:
            npc_info = self.index.npc_models.get(resolved_model)
        if info is None and npc_info is None:
            others = sorted(self.model_parts.get(resolved_model, set()) - {part})
            if others:
                asset.status = "id_space_mismatch"
                asset.found_in = others
                asset.note = "该 ID 存在于: " + ",".join(others)
            else:
                asset.status = "missing"
                asset.note = "资源缺失"
            if rule:
                asset.note = (f"resmirror {rule} → {resolved_model}；" + asset.note)
            return asset
        if npc_info is not None:
            return self._resolve_npc(asset, npc_info, resolved_model,
                                     resolved_action, resolved_dir, parse_frames)
        asset.pak = info.pak
        part_dir = info.part_dir or part
        asset.apg = (f"{part_dir}/{resolved_model}/{resolved_action}/"
                     f"{resolved_dir}.apg")
        asset.actions = sorted(info.actions)
        asset.directions = sorted(info.directions)
        hit = self.reader.find(info.pak, asset.apg, exact=True)
        asset.exists = hit is not None
        if asset.exists:
            asset.status = "found_mirror" if rule else "found"
        else:
            asset.status = "apg_missing"
            asset.note = f"{resolved_action}/{resolved_dir}.apg 不存在"
            if rule:
                asset.note = (f"resmirror {rule} → {resolved_model}；" + asset.note)
        if hit is not None and parse_frames:
            payload = self.reader.read(info.pak, asset.apg, exact=True)
            if payload is not None:
                try:
                    _hdr, frames = dapg.iter_frames(payload[1])
                    asset.frames = len(frames)
                except Exception:  # noqa: BLE001
                    pass
        pif_rel = f"{part_dir}/{resolved_model}/{resolved_action}/perinfo.dat"
        pif_hit = self.reader.find(info.pak, pif_rel, exact=True)
        if pif_hit is not None:
            asset.perinfo = pif_rel
            asset.perinfo_exists = True
            payload = self.reader.read(info.pak, pif_rel, exact=True)
            if payload is not None:
                try:
                    pif = parse_pif(payload[1])
                    if resolved_dir < pif.width:
                        row = pif.rows[resolved_dir]
                        if len(row) > 3:
                            asset.anchor = (row[2], row[3])
                        if len(row) > 5:
                            asset.delay = max(1, row[5])
                except Exception:  # noqa: BLE001
                    pass
        for cti_rel in (f"{part_dir}/{resolved_model}/clrtb.cti",
                        f"{part_dir}/clrtb.cti", "clrtb.cti"):
            cti_hit = self.reader.find(info.pak, cti_rel, exact=True)
            if cti_hit is not None:
                asset.cti = cti_rel
                break
        asset.keys = (self.keys.keys_for(info.pak, part, resolved_model,
                                         resolved_action, resolved_dir)
                      if self.keys is not None else [])
        return asset

    def resolve_wing(self, model: int, direction: int,
                     parse_frames: bool = True) -> PartAsset:
        """仙羽/翅膀：faeriewing 的翅膀 ID → player*.pak:pet/<id>/0/<dir>.apg。

        翅膀资源在 pet 空间、动作固定 0，不是普通宠物的 10/30。
        """
        asset = PartAsset("wing", "wing", model)
        info = self.index.best_model("pet", model)
        if info is None:
            asset.status = "missing"
            asset.note = "pet/翅膀模型不存在"
            return asset
        asset.pak = info.pak
        asset.apg = f"{info.part_dir}/{model}/0/{direction}.apg"
        hit = self.reader.find(info.pak, asset.apg, exact=True)
        asset.exists = hit is not None
        asset.status = "found" if asset.exists else "apg_missing"
        if not asset.exists:
            asset.note = "wing 0/<dir>.apg 不存在"
        if hit is not None and parse_frames:
            payload = self.reader.read(info.pak, asset.apg, exact=True)
            if payload is not None:
                try:
                    _hdr, frames = dapg.iter_frames(payload[1])
                    asset.frames = len(frames)
                except Exception:  # noqa: BLE001
                    pass
        for cti_rel in (f"pet/{model}/clrtb.cti", "pet/clrtb.cti", "clrtb.cti"):
            cti_hit = self.reader.find(info.pak, cti_rel, exact=True)
            if cti_hit is not None:
                asset.cti = cti_rel
                break
        asset.keys = (self.keys.keys_for(info.pak, "pet", model, 0, direction)
                      if self.keys is not None else [])
        return asset

    def resolve_pet(self, model: int, action: int | None, direction: int,
                    parse_frames: bool = True) -> PartAsset:
        """宠物：player*.pak:pet/<id>/<action>/<dir>.apg。

        action=None 时优先 10（待机）→ 0（翅膀/部分宠物）→ 最小动作。
        """
        asset = PartAsset("pet", "pet", model)
        info = self.index.best_model("pet", model)
        if info is None:
            asset.status = "missing"
            asset.note = "pet 模型不存在"
            return asset
        acts = set(info.actions)
        if action is not None and action in acts:
            act = action
        elif 10 in acts:
            act = 10
        elif 0 in acts:
            act = 0
        elif acts:
            act = min(acts)
        else:
            act = 0
        asset.pak = info.pak
        asset.apg = f"{info.part_dir}/{model}/{act}/{direction}.apg"
        asset.resolved_action = act
        asset.resolved_direction = direction
        asset.actions = sorted(acts)
        asset.directions = sorted(info.directions)
        hit = self.reader.find(info.pak, asset.apg, exact=True)
        asset.exists = hit is not None
        asset.status = "found" if asset.exists else "apg_missing"
        if not asset.exists:
            asset.note = f"pet {act}/{direction}.apg 不存在"
        if hit is not None and parse_frames:
            payload = self.reader.read(info.pak, asset.apg, exact=True)
            if payload is not None:
                try:
                    _hdr, frames = dapg.iter_frames(payload[1])
                    asset.frames = len(frames)
                except Exception:  # noqa: BLE001
                    pass
        for cti_rel in (f"pet/{model}/clrtb.cti", "pet/clrtb.cti", "clrtb.cti"):
            cti_hit = self.reader.find(info.pak, cti_rel, exact=True)
            if cti_hit is not None:
                asset.cti = cti_rel
                break
        asset.keys = (self.keys.keys_for(info.pak, "pet", model, act, direction)
                      if self.keys is not None else [])
        return asset

    def resolve_summon(self, shape: int, action: int | None, direction: int,
                       parse_frames: bool = True) -> PartAsset:
        """召唤兽：npc*.pak:<shape>/<action>/<dir>.apg。"""
        asset = PartAsset("summon", "summon", shape)
        info = self.index.npc_models.get(shape)
        summon_row = self.summons.get(shape)
        if info is None:
            asset.status = "missing"
            asset.note = "npc*.pak 中未找到该 shape"
            return asset
        if summon_row is not None:
            asset.note = " / ".join(summon_row.names)
        acts = set(info.actions)
        if action is not None and action in acts:
            act = action
        elif 10 in acts:
            act = 10
        elif acts:
            act = min(acts)
        else:
            act = 10
        asset.pak = info.pak
        asset.apg = f"{shape}/{act}/{direction}.apg"
        asset.resolved_action = act
        asset.resolved_direction = direction
        asset.actions = sorted(acts)
        asset.directions = sorted(info.directions)
        hit = self.reader.find(info.pak, asset.apg, exact=True)
        asset.exists = hit is not None
        asset.status = "found_npc" if asset.exists else "apg_missing"
        if not asset.exists:
            asset.note = f"summon {act}/{direction}.apg 不存在"
        if hit is not None and parse_frames:
            payload = self.reader.read(info.pak, asset.apg, exact=True)
            if payload is not None:
                try:
                    _hdr, frames = dapg.iter_frames(payload[1])
                    asset.frames = len(frames)
                except Exception:  # noqa: BLE001
                    pass
        for cti_rel in (f"{shape}/clrtb.cti", "clrtb.cti"):
            cti_hit = self.reader.find(info.pak, cti_rel, exact=True)
            if cti_hit is not None:
                asset.cti = cti_rel
                break
        return asset

    def _mount_head_model(self, model: int) -> int:
        """神骑一体形态的头部模型：优先 model+2/3，再回退本体。"""
        if model is None or model <= 0:
            return model
        for candidate in (model + 2, model + 3, model, model + 1):
            if self.index.best_model("head", candidate) is not None:
                return candidate
        return model

    def _star_head_model(self, star: StarGeneralRow, head: int) -> int:
        """星将行 head ID 缺失时的回退：base(100/600) + 风格位。

        例：男 611 → head/601；女 662 → head/656；若精确 ID 存在则保留。
        发色不在这里处理，由 --head-hue / 角色自定义颜色单独应用。
        """
        if head != star.head or head <= 0:
            return head
        if self.index.best_model("head", head) is not None:
            return head
        base = (head // 100) * 100
        if star.gender == 1:
            base += 50
        style = (head // 10) % 10
        candidate = base + style
        if self.index.best_model("head", candidate) is not None:
            return candidate
        return head

    def parts(self, preset, action: int, direction: int,
              body: int | None = None, head: int = 105,
              parse_frames: bool = True,
              overrides: dict[str, int] | None = None,
              hues: dict[str, int] | None = None,
              star_general: StarGeneralRow | None = None,
              mount_model: int | None = None,
              mount_id: int | None = None,
              mount_form: str = "ride",
              wing_model: int | None = None,
              pet_model: int | None = None,
              pet_action: int | None = None,
              pet_frame: int = 0,
              summon_shape: int | None = None,
              summon_action: int | None = None,
              skill_effect_id: int | None = None,
              skill_variant: int | str | None = None,
              skill_layers: tuple[str, ...] = ("back", "front")) -> list[PartAsset]:
        parts: list[PartAsset] = []
        overrides = overrides or {}
        hues = hues or {}
        star = star_general
        # 星将形态优先于 viewjob 基础外观；显式 --body/--head 仍可覆盖。
        # 星将行自带 body/head/cloth/weapon/effect/shoe/mask/cuff 与颜色；
        # --override/--equip 的最后覆盖在下面的 loop 中合并。
        if star is not None:
            if body is None:
                body = star.body
            if head == 105:
                head = star.head
        elif body is None:
            # 职业/性别默认身体：career*100 + 50*gender（viewjob 同 ID 空间；
            # 1–16 职业的 body/<id> 均存在，职业 0 无此规律时回退 200）。
            body = preset.career * 100 + 50 * preset.gender
            if self.index.best_model("body", body) is None:
                body = 200
        values = {
            "body": body, "head": head, "mask": None,
            "cloth": preset.cloth, "gloves_l": preset.gloves, "gloves_r": preset.gloves,
            "shoes": preset.shoes, "weapon": preset.weapon, "horse": preset.horse,
            "harmor_head": preset.harmor_head, "harmor_body": preset.harmor_body,
        }
        star_hues: dict[str, int] = {}
        if star is not None:
            values.update({
                "body": body,
                "head": self._star_head_model(star, head),
                "cloth": star.cloth if star.cloth > 0 else None,
                "gloves_l": star.cuff if star.cuff > 0 else None,
                "gloves_r": star.cuff if star.cuff > 0 else None,
                "shoes": star.shoe if star.shoe > 0 else None,
                "weapon": star.weapon if star.weapon > 0 else None,
                "mask": star.mask if star.mask > 0 else None,
            })
            star_hues = dict(star.hues)
        effective_hues = dict(star_hues)
        effective_hues.update(hues)
        if mount_model is not None or mount_id is not None:
            mount = self.mounts.resolve(preset, mount_model, mount_id, mount_form)
            if mount.get("form") == "god":
                model = mount["mount_model"]
                values.update({
                    "body": model,
                    "head": self._mount_head_model(model),
                    "horse": model,
                    "harmor_head": model,
                    "harmor_body": model,
                    # 神骑一体形态隐藏人物装备层；显式 --override 仍可覆盖。
                    "cloth": None,
                    "gloves_l": None,
                    "gloves_r": None,
                    "shoes": None,
                    "weapon": None,
                    "mask": None,
                })
                star = None
                star_hues = {}
            else:
                values["horse"] = mount["horse"]
                values["harmor_head"] = mount["harmor_head"]
                values["harmor_body"] = mount["harmor_body"]
        if "gloves" in overrides:
            values["gloves_l"] = overrides["gloves"]
            values["gloves_r"] = overrides["gloves"]
        for field_name, part, default in PART_FIELDS:
            model = overrides.get(field_name, values.get(field_name, default))
            if model is None:
                model = default
            asset = self._resolve(field_name, part, model, action, direction, parse_frames)
            if asset is not None:
                asset.hue = effective_hues.get(field_name, 0)
                parts.append(asset)
        # 武器光效（part type 8）：资源目录 effect/<model>/...；模型来自
        # ITEM_EFFECT、星将形态行或显式 --override effect=...。
        effect_model = overrides.get("effect")
        if effect_model is None and star is not None and star.effect > 0:
            effect_model = star.effect
        if effect_model:
            asset = self._resolve("effect", "effect", effect_model,
                                  action, direction, parse_frames)
            if asset is not None:
                asset.hue = effective_hues.get("effect", 0)
                parts.append(asset)
        # 仙羽/翅膀：pet/<id>/0/<dir> 空间，绘制在人物后方（render 层序 -1）。
        if wing_model is not None:
            wing = self.resolve_wing(wing_model, direction, parse_frames)
            wing.hue = effective_hues.get("wing", 0)
            parts.append(wing)
        # 宠物：pet/<id>/<action>/<dir>，允许跳过影子帧（frame0）。
        if pet_model is not None:
            pet = self.resolve_pet(pet_model, pet_action, direction, parse_frames)
            pet.frame_offset = max(0, pet_frame)
            parts.append(pet)
        # 召唤兽：npc*.pak:<shape>/<action>/<dir>，独立于玩家/宠物空间。
        if summon_shape is not None:
            summon = self.resolve_summon(summon_shape, summon_action,
                                         direction, parse_frames)
            parts.append(summon)
        # 技能光效：back 在人物后方，front 在前景；两者可同时存在。
        if skill_effect_id is not None:
            for layer in skill_layers:
                if layer not in ("back", "front"):
                    continue
                skill = self.resolve_skill_effect(
                    skill_effect_id, skill_variant, layer, parse_frames)
                parts.append(skill)
        # 配对/派生层的层叠键：护腕左右共享；武器光效跟随武器层。
        by_part = {a.part: a for a in parts}
        lcuff, rcuff = by_part.get("lcuff"), by_part.get("rcuff")
        if lcuff is not None and rcuff is not None:
            if not lcuff.keys and rcuff.keys:
                lcuff.keys = list(rcuff.keys)
            if not rcuff.keys and lcuff.keys:
                rcuff.keys = list(lcuff.keys)
        effect, weapon = by_part.get("effect"), by_part.get("weapon")
        if effect is not None and weapon is not None and not effect.keys:
            effect.keys = list(weapon.keys)
        return parts

    def _mount_attach(self, mount_model: int, body_model: int) -> dict | None:
        """horseoffset：坐骑模型 → 人物/阴影/宠物/…挂点偏移。"""
        if mount_model is None or mount_model <= 0:
            return None
        table = self.tables.get("horseoffset")
        if table is None:
            return None
        keys = ("character_dx", "shadow_dx", "pet_dx", "wing_dx", "cloak_dx",
                "stargod_dx", "head_dx", "foot_aura_dx", "foot_effect_dx",
                "center_effect_dx")
        best = None
        for row in table.rows:
            try:
                vals = [int(cell) for cell in row[:12]]
            except ValueError:
                continue
            if len(vals) < 12 or vals[1] != mount_model:
                continue
            if best is None or vals[0] == body_model:
                best = vals
                if vals[0] == body_model:
                    break
        if best is None:
            return None
        return {
            "character_id": best[0],
            "mount_id": best[1],
            "offsets": dict(zip(keys, best[2:])),
        }

    def manifest(self, preset, action: int, direction: int,
                 body: int | None = None, head: int = 105,
                 parse_frames: bool = True,
                 overrides: dict[str, int] | None = None,
                 hues: dict[str, int] | None = None,
                 star_general: StarGeneralRow | None = None,
                 mount_model: int | None = None,
                 mount_id: int | None = None,
                 mount_form: str = "ride",
                 wing_model: int | None = None,
                 pet_model: int | None = None,
                 pet_action: int | None = None,
                 pet_frame: int = 0,
                 summon_shape: int | None = None,
                 summon_action: int | None = None,
                 skill_effect_id: int | None = None,
                 skill_variant: int | str | None = None,
                 skill_layers: tuple[str, ...] = ("back", "front")) -> dict:
        parts = self.parts(preset, action, direction, body, head, parse_frames,
                           overrides, hues, star_general, mount_model, mount_id,
                           mount_form, wing_model, pet_model, pet_action, pet_frame,
                           summon_shape, summon_action, skill_effect_id,
                           skill_variant, skill_layers)
        mount = None
        if mount_model is not None or mount_id is not None:
            mount = self.mounts.resolve(preset, mount_model, mount_id, mount_form)
        stargod = self._resolve_stargod(preset.stargod_model, action, direction, parse_frames)
        missing = [a.as_dict() for a in parts if not a.exists]
        skills = []
        skill_table = self.tables.get("skill")
        if skill_table is not None:
            for row in skill_table.rows:
                if len(row) < 11:
                    continue
                try:
                    career = int(row[3])
                    if career != preset.career:
                        continue
                    hit = int(row[8]) if row[8].strip().isdigit() else 0
                    attack = int(row[9]) if row[9].strip().isdigit() else 0
                    fail = int(row[10]) if row[10].strip().isdigit() else 0
                    skills.append({
                        "id": int(row[0]),
                        "name": row[1],
                        "hit_effect": hit,
                        "attack_effect": attack,
                        "fail_effect": fail,
                        "effects": {
                            "hit": self.resolve_effect(hit),
                            "attack": self.resolve_effect(attack),
                            "fail": self.resolve_effect(fail),
                        },
                    })
                except ValueError:
                    continue
        return {
            "preset": {
                "career": preset.career,
                "advance": preset.advance,
                "mount_tier": preset.mount_tier,
                "gender": preset.gender,
                "hero_branch": preset.hero_branch,
                "stargod_model": preset.stargod_model,
            },
            "action": action,
            "direction": direction,
            "overrides": overrides or {},
            "hues": hues or {},
            "parts": [a.as_dict() for a in parts],
            "stargod": stargod,
            "star_general": star_general.as_dict() if star_general else None,
            "mount": mount,
            "mount_attach": self._mount_attach(
                (mount or {}).get("horse")
                or (overrides or {}).get("horse", preset.horse),
                (overrides or {}).get("body", body)),
            "wing": next((a.as_dict() for a in parts if a.field == "wing"), None),
            "pet": next((a.as_dict() for a in parts if a.field == "pet"), None),
            "summon": next((a.as_dict() for a in parts if a.field == "summon"), None),
            "skill_effect": ({
                "id": skill_effect_id,
                "variant": skill_variant,
                "layers": list(skill_layers),
            } if skill_effect_id is not None else None),
            "missing": missing,
            "skills": skills,
        }

    def _resolve_stargod(self, model: int, action: int, direction: int,
                         parse_frames: bool) -> dict | None:
        if model <= 0:
            return None
        info = self.index.npc_models.get(model)
        if info is None:
            return {"model": model, "exists": False, "note": "npc*.pak 中未找到"}
        rel = f"{model}/{action}/{direction}.apg"
        hit = self.reader.find(info.pak, rel, exact=True)
        frames = 0
        if hit is not None and parse_frames:
            payload = self.reader.read(info.pak, rel, exact=True)
            if payload is not None:
                try:
                    _hdr, parsed = dapg.iter_frames(payload[1])
                    frames = len(parsed)
                except Exception:  # noqa: BLE001
                    pass
        return {
            "model": model,
            "pak": info.pak,
            "apg": rel,
            "exists": hit is not None,
            "frames": frames,
            "actions": sorted(info.actions),
        }
