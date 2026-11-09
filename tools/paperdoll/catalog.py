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
    11450: (("lcuff", "rcuff"), 453, 0,
            "客户端为金色护腕原色（不随衣服变色）；护腕应在武器之后绘制"),
}

# 武器光效（part type 8）：item_id -> (effect 模型, 色相角度)。
# 4400 桃木杖：effect/450，bodyidequal 光效颜色 90 → 90/255*360 ≈ 127°。
ITEM_EFFECT = {
    4400: (450, 127),
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
        return cls(shdata, reader, loaded, preset_list, index, keys, model_parts, mirror)

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

    def equip_overrides(self, item_ids: list[int]) -> tuple[dict[str, int],
                                                            dict[str, int],
                                                            list[dict]]:
        """物品 ID → 部件模型/色相覆盖；返回 (overrides, hues, 解析明细)。

        优先使用运行时实测的 `ITEM_APPEARANCE`（1.50.27，见 asset-spec §4.3）；
        未实测物品回退到 kimi_viewer 公式 `item_id - slot*100`，并在明细中标注。
        """
        overrides: dict[str, int] = {}
        hues: dict[str, int] = {}
        resolved: list[dict] = []
        for item_id in item_ids or []:
            rec = self.item_lookup(item_id)
            if rec is None:
                resolved.append({"item_id": item_id, "status": "item_not_found"})
                continue
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
                                 "status": "measured", "note": note})
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
                             "note": "未实测，沿用 item_id - slot*100"})
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

    def parts(self, preset, action: int, direction: int,
              body: int | None = None, head: int = 105,
              parse_frames: bool = True,
              overrides: dict[str, int] | None = None,
              hues: dict[str, int] | None = None) -> list[PartAsset]:
        parts: list[PartAsset] = []
        overrides = overrides or {}
        hues = hues or {}
        if body is None:
            # 职业/性别默认身体：career*100 + 50*gender（viewjob 同 ID 空间；
            # 1–16 职业的 body/<id> 均存在，职业 0 无此规律时回退 200）。
            body = preset.career * 100 + 50 * preset.gender
            if self.index.best_model("body", body) is None:
                body = 200
        values = {
            "body": body, "head": head,
            "cloth": preset.cloth, "gloves_l": preset.gloves, "gloves_r": preset.gloves,
            "shoes": preset.shoes, "weapon": preset.weapon, "horse": preset.horse,
            "harmor_head": preset.harmor_head, "harmor_body": preset.harmor_body,
        }
        if "gloves" in overrides:
            values["gloves_l"] = overrides["gloves"]
            values["gloves_r"] = overrides["gloves"]
        for field_name, part, default in PART_FIELDS:
            model = overrides.get(field_name, values.get(field_name, default))
            if model is None:
                model = default
            asset = self._resolve(field_name, part, model, action, direction, parse_frames)
            if asset is not None:
                asset.hue = hues.get(field_name, 0)
                parts.append(asset)
        # 武器光效（part type 8）：资源目录 effect/<model>/...；模型来自
        # ITEM_EFFECT 或显式 --override effect=...，没有装备光效时不生成该层。
        effect_model = overrides.get("effect")
        if effect_model:
            asset = self._resolve("effect", "effect", effect_model,
                                  action, direction, parse_frames)
            if asset is not None:
                asset.hue = hues.get("effect", 0)
                parts.append(asset)
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
                 hues: dict[str, int] | None = None) -> dict:
        parts = self.parts(preset, action, direction, body, head, parse_frames,
                           overrides, hues)
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
            "mount_attach": self._mount_attach(
                (overrides or {}).get("horse", preset.horse),
                (overrides or {}).get("body", body)),
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
