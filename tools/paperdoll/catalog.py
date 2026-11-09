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

# 部件目录 → paperdoll 字段名（与 PART_FIELDS 一致）。
PART_FIELD = {
    "body": "body", "head": "head", "mask": "mask", "cloth": "cloth",
    "lcuff": "gloves_l", "rcuff": "gloves_r", "shoe": "shoes",
    "weapon": "weapon", "horse": "horse", "harmorhead": "harmor_head",
    "harmorbody": "harmor_body",
}


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
    note: str = ""

    def as_dict(self) -> dict:
        return {
            "field": self.field,
            "part": self.part,
            "model": self.model,
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
        return cls(shdata, reader, loaded, preset_list, index, keys, model_parts)

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

    def equip_overrides(self, item_ids: list[int]) -> tuple[dict[str, int], list[dict]]:
        """物品 ID → 部件模型覆盖；返回 (overrides, 解析明细)。

        model 公式（kimi_viewer 参考实现）：model = item_id - slot*100。
        未识别槽位/物品仍记录在明细里，不静默丢弃。
        """
        overrides: dict[str, int] = {}
        resolved: list[dict] = []
        for item_id in item_ids or []:
            rec = self.item_lookup(item_id)
            if rec is None:
                resolved.append({"item_id": item_id, "status": "item_not_found"})
                continue
            parts = ITEM_SLOT_PART.get(rec["slot"])
            if not parts:
                resolved.append({**rec, "status": "unknown_slot"})
                continue
            model = rec["model"]
            fields = [PART_FIELD[p] for p in parts]
            for field_name in fields:
                overrides[field_name] = model
            resolved.append({**rec, "part": list(parts), "field": fields,
                             "status": "mapped"})
        return overrides, resolved

    def _resolve(self, field_name: str, part: str, model: int | None,
                 action: int, direction: int, parse_frames: bool) -> PartAsset | None:
        if model is None or model < 0:
            return None
        asset = PartAsset(field_name, part, model)
        info = self.index.best_model(part, model)
        if info is None:
            others = sorted(self.model_parts.get(model, set()) - {part})
            if others:
                asset.status = "id_space_mismatch"
                asset.found_in = others
                asset.note = "该 ID 存在于: " + ",".join(others)
            else:
                asset.status = "missing"
                asset.note = "资源缺失"
            return asset
        asset.pak = info.pak
        part_dir = info.part_dir or part
        asset.apg = f"{part_dir}/{model}/{action}/{direction}.apg"
        asset.actions = sorted(info.actions)
        asset.directions = sorted(info.directions)
        hit = self.reader.find(info.pak, asset.apg, exact=True)
        asset.exists = hit is not None
        asset.status = "found" if asset.exists else "apg_missing"
        if not asset.exists:
            asset.note = f"{action}/{direction}.apg 不存在"
        if hit is not None and parse_frames:
            payload = self.reader.read(info.pak, asset.apg, exact=True)
            if payload is not None:
                try:
                    _hdr, frames = dapg.iter_frames(payload[1])
                    asset.frames = len(frames)
                except Exception:  # noqa: BLE001
                    pass
        pif_rel = f"{part_dir}/{model}/{action}/perinfo.dat"
        pif_hit = self.reader.find(info.pak, pif_rel, exact=True)
        if pif_hit is not None:
            asset.perinfo = pif_rel
            asset.perinfo_exists = True
            payload = self.reader.read(info.pak, pif_rel, exact=True)
            if payload is not None:
                try:
                    pif = parse_pif(payload[1])
                    if direction < pif.width:
                        row = pif.rows[direction]
                        if len(row) > 3:
                            asset.anchor = (row[2], row[3])
                        if len(row) > 5:
                            asset.delay = max(1, row[5])
                except Exception:  # noqa: BLE001
                    pass
        for cti_rel in (f"{part_dir}/{model}/clrtb.cti", f"{part_dir}/clrtb.cti",
                        "clrtb.cti"):
            cti_hit = self.reader.find(info.pak, cti_rel, exact=True)
            if cti_hit is not None:
                asset.cti = cti_rel
                break
        asset.keys = (self.keys.keys_for(info.pak, part, model, action, direction)
                      if self.keys is not None else [])
        return asset

    def parts(self, preset, action: int, direction: int,
              body: int = 200, head: int = 105,
              parse_frames: bool = True,
              overrides: dict[str, int] | None = None) -> list[PartAsset]:
        parts: list[PartAsset] = []
        overrides = overrides or {}
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
                parts.append(asset)
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
                 body: int = 200, head: int = 105,
                 parse_frames: bool = True,
                 overrides: dict[str, int] | None = None) -> dict:
        parts = self.parts(preset, action, direction, body, head, parse_frames, overrides)
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
