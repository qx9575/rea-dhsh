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
    ("cloth", "cloth", None),
    ("gloves_l", "lcuff", None),
    ("gloves_r", "rcuff", None),
    ("shoes", "shoe", None),
    ("weapon", "weapon", None),
    ("horse", "horse", None),
    ("harmor_head", "harmorhead", None),
    ("harmor_body", "harmorbody", None),
)


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

    @classmethod
    def load(cls, shdata: Path, inventory_path: Path | None = None) -> "PaperDollCatalog":
        reader = PakReader(shdata)
        loaded = tables.load_tables(reader)
        preset_list = careers.parse_presets(loaded["viewjob"])
        entries = inventory.load_entries(shdata, inventory_path)
        index = inventory.build_index(entries)
        from paperdoll.keys import LayerKeyIndex
        keys = LayerKeyIndex(reader)
        return cls(shdata, reader, loaded, preset_list, index, keys)

    def find_preset(self, career: int, advance: int, mount_tier: int, gender: int):
        for p in self.presets:
            if (p.career, p.advance, p.mount_tier, p.gender) == (
                    career, advance, mount_tier, gender):
                return p
        return None

    def _resolve(self, field_name: str, part: str, model: int | None,
                 action: int, direction: int, parse_frames: bool) -> PartAsset | None:
        if model is None or model < 0:
            return None
        asset = PartAsset(field_name, part, model)
        info = self.index.best_model(part, model)
        if info is None:
            asset.note = "资源缺失"
            return asset
        asset.pak = info.pak
        asset.apg = f"{part}/{model}/{action}/{direction}.apg"
        hit = self.reader.find(info.pak, asset.apg)
        asset.exists = hit is not None
        if hit is not None and parse_frames:
            payload = self.reader.read(info.pak, asset.apg)
            if payload is not None:
                try:
                    _hdr, frames = dapg.iter_frames(payload[1])
                    asset.frames = len(frames)
                except Exception:  # noqa: BLE001
                    pass
        pif_rel = f"{part}/{model}/{action}/perinfo.dat"
        pif_hit = self.reader.find(info.pak, pif_rel)
        if pif_hit is not None:
            asset.perinfo = pif_rel
            asset.perinfo_exists = True
            payload = self.reader.read(info.pak, pif_rel)
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
        for cti_rel in (f"{part}/{model}/clrtb.cti", f"{part}/clrtb.cti", "clrtb.cti"):
            cti_hit = self.reader.find(info.pak, cti_rel)
            if cti_hit is not None:
                asset.cti = cti_rel
                break
        asset.keys = self.keys.keys_for(info.pak, part, model, action, direction)
        return asset

    def parts(self, preset, action: int, direction: int,
              body: int = 200, head: int = 105,
              parse_frames: bool = True) -> list[PartAsset]:
        parts: list[PartAsset] = []
        values = {
            "body": body, "head": head,
            "cloth": preset.cloth, "gloves_l": preset.gloves, "gloves_r": preset.gloves,
            "shoes": preset.shoes, "weapon": preset.weapon, "horse": preset.horse,
            "harmor_head": preset.harmor_head, "harmor_body": preset.harmor_body,
        }
        for field_name, part, default in PART_FIELDS:
            model = values.get(field_name, default)
            if model is None:
                model = default
            asset = self._resolve(field_name, part, model, action, direction, parse_frames)
            if asset is not None:
                parts.append(asset)
        return parts

    def manifest(self, preset, action: int, direction: int,
                 body: int = 200, head: int = 105,
                 parse_frames: bool = True) -> dict:
        parts = self.parts(preset, action, direction, body, head, parse_frames)
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
                    skills.append({
                        "id": int(row[0]),
                        "name": row[1],
                        "hit_effect": int(row[8]) if row[8].strip().isdigit() else 0,
                        "attack_effect": int(row[9]) if row[9].strip().isdigit() else 0,
                        "fail_effect": int(row[10]) if row[10].strip().isdigit() else 0,
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
            "parts": [a.as_dict() for a in parts],
            "stargod": stargod,
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
        hit = self.reader.find(info.pak, rel)
        frames = 0
        if hit is not None and parse_frames:
            payload = self.reader.read(info.pak, rel)
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
