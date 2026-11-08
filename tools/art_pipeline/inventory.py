"""资源 taxonomy：扫描 PAK 叶子，按部件/模型/动作/朝向归档。

只读 pak_inventory.json 或实时 PAK；输出 JSON 摘要 + SQLite 明细。
"""
from __future__ import annotations

import json
import os
import sqlite3
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

from art_pipeline.common import (
    DEFAULT_INVENTORY,
    PakReader,
    iter_batches,
    write_json,
)
from art_pipeline import idspaces


@dataclass(frozen=True)
class ResEntry:
    pak: str
    path: str
    name: str
    size: int


@dataclass
class ModelInfo:
    pak: str
    part_key: str
    part_dir: str
    model: int
    actions: set[int] = field(default_factory=set)
    directions: set[int] = field(default_factory=set)
    files: int = 0
    has_perinfo: bool = False
    has_cti: bool = False


@dataclass
class PartInfo:
    part_key: str
    dirs: set[str] = field(default_factory=set)
    paks: set[str] = field(default_factory=set)
    models: set[int] = field(default_factory=set)
    actions: set[int] = field(default_factory=set)
    files: int = 0


KNOWN_PLAYER_PARTS = {
    "body", "head", "weapon", "cloth", "shoe", "mask", "lcuff", "rcuff",
    "horse", "harmorbody", "harmorhead", "pet", "effect", "shadow",
}


@dataclass
class InventoryIndex:
    entries: list[ResEntry]
    parts: dict[str, PartInfo]
    models: dict[tuple[str, int], list[ModelInfo]]
    npc_models: dict[int, ModelInfo]
    magic_models: dict[int, set[str]]
    ui_top: dict[str, int]
    config_top: dict[str, int]
    anomalies: list[tuple[str, str, int]]
    perinfo_coverage: list[dict]

    def model_candidates(self, part: str, model: int) -> list[ModelInfo]:
        return self.models.get((part.lower(), model), [])

    def best_model(self, part: str, model: int) -> ModelInfo | None:
        candidates = self.model_candidates(part, model)
        if not candidates:
            return None
        return sorted(candidates, key=lambda m: (m.pak != "player.pak", m.pak))[0]

    def has_model(self, part: str, model: int) -> bool:
        return bool(self.models.get((part.lower(), model)))

    def actions(self, part: str, model: int) -> set[int]:
        info = self.best_model(part, model)
        return info.actions if info else set()

    def summary(self) -> dict:
        return {
            "entries": len(self.entries),
            "player_parts": {
                key: {
                    "dirs": sorted(info.dirs),
                    "paks": sorted(info.paks),
                    "files": info.files,
                    "models": len(info.models),
                    "actions": sorted(info.actions),
                }
                for key, info in sorted(self.parts.items())
            },
            "npc_models": len(self.npc_models),
            "magic_models": len(self.magic_models),
            "ui_top": dict(sorted(self.ui_top.items(), key=lambda kv: -kv[1])),
            "config_top": dict(sorted(self.config_top.items(), key=lambda kv: -kv[1])),
            "perinfo_coverage": self.perinfo_coverage,
            "anomalies": [
                {"kind": kind, "detail": detail, "count": count}
                for kind, detail, count in self.anomalies
            ],
        }


def load_entries(shdata: Path, inventory_path: Path | None = DEFAULT_INVENTORY) -> list[ResEntry]:
    if inventory_path and Path(inventory_path).is_file():
        data = json.loads(Path(inventory_path).read_text(encoding="utf-8"))
        entries: list[ResEntry] = []
        for pak, rows in data.items():
            for row in rows:
                entries.append(ResEntry(pak, row.get("path", ""), row.get("name", ""),
                                        int(row.get("size", 0))))
        return entries
    reader = PakReader(shdata)
    entries = []
    for fn in sorted(os.listdir(shdata)):
        if not fn.lower().endswith(".pak"):
            continue
        for path, name, offset, size in reader._ensure(fn):
            entries.append(ResEntry(fn, path, name, size))
    return entries


def _split_path(path: str) -> list[str]:
    return path.replace("\\", "/").split("/")


def build_index(entries: list[ResEntry]) -> InventoryIndex:
    parts: dict[str, PartInfo] = {}
    models: dict[tuple[str, int], list[ModelInfo]] = defaultdict(list)
    npc_models: dict[int, ModelInfo] = {}
    magic_models: dict[int, set[str]] = defaultdict(set)
    ui_top: dict[str, int] = defaultdict(int)
    config_top: dict[str, int] = defaultdict(int)

    model_by_key: dict[tuple[str, str, int], ModelInfo] = {}
    perinfo_dir_set: set[tuple[str, str, int, str]] = set()
    action_dir_set: set[tuple[str, str, int, str]] = set()
    non_apg: dict[tuple[str, str], int] = defaultdict(int)

    for entry in entries:
        seg = _split_path(entry.path)
        if not seg or not seg[0]:
            continue
        pak = entry.pak
        if pak.startswith("ui"):
            ui_top[seg[0]] += 1
            continue
        if pak.startswith("config"):
            config_top[seg[0]] += 1
            continue
        if pak.startswith("npc"):
            if len(seg) >= 3 and seg[0].isdigit() and seg[1].isdigit():
                model = int(seg[0])
                action = int(seg[1])
                info = npc_models.get(model)
                if info is None:
                    info = ModelInfo(pak, "npc", "npc", model)
                    npc_models[model] = info
                info.actions.add(action)
                stem = Path(seg[-1]).stem
                if stem.isdigit():
                    info.directions.add(int(stem))
                info.files += 1
                if entry.name.lower() == "perinfo.dat":
                    info.has_perinfo = True
            continue
        if pak.startswith("magic"):
            if len(seg) >= 2 and seg[0].isdigit():
                magic_models[int(seg[0])].add(seg[1])
            continue
        if not pak.startswith("player"):
            continue
        if len(seg) < 2:
            continue
        part_dir = seg[0]
        part_key = part_dir.lower()
        if part_key == "shadow" or len(seg) < 2:
            continue
        model = int(seg[1]) if seg[1].isdigit() else None
        if model is None:
            continue
        p = parts.setdefault(part_key, PartInfo(part_key))
        p.dirs.add(part_dir)
        p.paks.add(pak)
        p.models.add(model)
        p.files += 1
        if len(seg) >= 3 and seg[2].isdigit():
            p.actions.add(int(seg[2]))
        key = (pak, part_key, model)
        info = model_by_key.get(key)
        if info is None:
            info = ModelInfo(pak, part_key, part_dir, model)
            model_by_key[key] = info
            models[(part_key, model)].append(info)
        info.files += 1
        if len(seg) >= 3 and seg[2].isdigit():
            action = int(seg[2])
            info.actions.add(action)
            action_dir_set.add((pak, part_key, model, seg[2]))
        if len(seg) >= 4:
            stem = Path(seg[3]).stem
            if stem.isdigit():
                info.directions.add(int(stem))
        if entry.name.lower() == "perinfo.dat":
            info.has_perinfo = True
            perinfo_dir_set.add((pak, part_key, model,
                                 seg[2] if len(seg) > 2 else ""))
        if entry.name.lower() == "clrtb.cti":
            info.has_cti = True
        ext = os.path.splitext(entry.name)[1].lower()
        if part_key in KNOWN_PLAYER_PARTS and ext not in (".apg", ".dat", ".cti"):
            non_apg[(part_key, ext or "<none>")] += 1

    anomalies: list[tuple[str, str, int]] = []
    for part_key, info in sorted(parts.items()):
        if len(info.dirs) > 1:
            anomalies.append(("case_variant", f"{part_key}: {sorted(info.dirs)}",
                              len(info.dirs)))
        if part_key not in KNOWN_PLAYER_PARTS:
            anomalies.append(("unknown_part_dir", part_key, info.files))
    for part_key, ext in sorted(non_apg.items()):
        anomalies.append(("non_standard_ext", f"{part_key[0]}/{part_key[1]}", ext))
    # perinfo 覆盖率：按 pak/part 汇总，不逐目录刷屏
    action_counts: dict[tuple[str, str], int] = {}
    for pak, part_key, _model, _action in action_dir_set:
        action_counts[(pak, part_key)] = action_counts.get((pak, part_key), 0) + 1
    perinfo_counts: dict[tuple[str, str], int] = {}
    for pak, part_key, _model, _action in perinfo_dir_set:
        perinfo_counts[(pak, part_key)] = perinfo_counts.get((pak, part_key), 0) + 1
    perinfo_coverage = [
        {
            "pak": pak,
            "part": part_key,
            "action_dirs": total,
            "with_perinfo": perinfo_counts.get((pak, part_key), 0),
            "missing": total - perinfo_counts.get((pak, part_key), 0),
        }
        for (pak, part_key), total in sorted(action_counts.items())
    ]
    # 合并相同 anomaly
    merged: dict[tuple[str, str], int] = {}
    for kind, detail, count in anomalies:
        merged[(kind, detail)] = merged.get((kind, detail), 0) + count
    anomalies = [(kind, detail, count) for (kind, detail), count in sorted(merged.items())]

    return InventoryIndex(entries, parts, dict(models), npc_models,
                          dict(magic_models), dict(ui_top), dict(config_top),
                          anomalies, perinfo_coverage)


def write_sqlite(index: InventoryIndex, db_path: Path) -> None:
    conn = sqlite3.connect(db_path)
    cur = conn.cursor()
    for table in ("resource_parts", "resource_models", "resource_files",
                  "resource_anomalies", "perinfo_coverage"):
        cur.execute(f"DROP TABLE IF EXISTS {table}")
    cur.execute("""CREATE TABLE resource_parts(
        part_key TEXT PRIMARY KEY, dirs TEXT, paks TEXT, files INTEGER,
        models INTEGER, actions TEXT)""")
    cur.execute("""CREATE TABLE resource_models(
        pak TEXT, part_key TEXT, part_dir TEXT, model INTEGER,
        actions TEXT, directions TEXT, files INTEGER,
        has_perinfo INTEGER, has_cti INTEGER,
        PRIMARY KEY(pak, part_key, model))""")
    cur.execute("""CREATE TABLE resource_files(
        pak TEXT, path TEXT, name TEXT, ext TEXT, size INTEGER,
        part_key TEXT, model INTEGER, action TEXT, direction TEXT)""")
    cur.execute("""CREATE TABLE resource_anomalies(
        kind TEXT, detail TEXT, count INTEGER)""")
    cur.execute("""CREATE TABLE perinfo_coverage(
        pak TEXT, part_key TEXT, action_dirs INTEGER,
        with_perinfo INTEGER, missing INTEGER)""")

    for key, info in sorted(index.parts.items()):
        cur.execute("INSERT INTO resource_parts VALUES (?,?,?,?,?,?)",
                    (key, json.dumps(sorted(info.dirs), ensure_ascii=False),
                     json.dumps(sorted(info.paks), ensure_ascii=False),
                     info.files, len(info.models), json.dumps(sorted(info.actions))))
    for key, infos in sorted(index.models.items()):
        for info in infos:
            cur.execute("INSERT OR REPLACE INTO resource_models VALUES (?,?,?,?,?,?,?,?,?)",
                        (info.pak, info.part_key, info.part_dir, info.model,
                         json.dumps(sorted(info.actions)), json.dumps(sorted(info.directions)),
                         info.files, int(info.has_perinfo), int(info.has_cti)))
    for batch in iter_batches(index.entries, 5000):
        rows = []
        for entry in batch:
            seg = _split_path(entry.path)
            part_key = seg[0].lower() if seg else ""
            model = None
            action = None
            direction = None
            if entry.pak.startswith("player") and len(seg) >= 3:
                model = int(seg[1]) if seg[1].isdigit() else None
                action = seg[2] if seg[2].isdigit() else None
                if len(seg) >= 4:
                    direction = Path(seg[3]).stem
            elif entry.pak.startswith("npc") and len(seg) >= 3:
                part_key = "npc"
                model = int(seg[0]) if seg[0].isdigit() else None
                action = seg[1] if seg[1].isdigit() else None
                direction = Path(seg[2]).stem
            elif entry.pak.startswith("magic") and len(seg) >= 3:
                part_key = "magic"
                model = int(seg[0]) if seg[0].isdigit() else None
                action = seg[1]
                direction = Path(seg[2]).stem
            rows.append((entry.pak, entry.path, entry.name,
                         os.path.splitext(entry.name)[1].lower(), entry.size,
                         part_key, model, action, direction))
        cur.executemany("INSERT INTO resource_files VALUES (?,?,?,?,?,?,?,?,?)", rows)
    cur.executemany("INSERT INTO resource_anomalies VALUES (?,?,?)", index.anomalies)
    cur.executemany("INSERT INTO perinfo_coverage VALUES (?,?,?,?,?)",
                    [(row["pak"], row["part"], row["action_dirs"],
                      row["with_perinfo"], row["missing"])
                     for row in index.perinfo_coverage])
    conn.commit()
    conn.close()


def build_inventory(shdata: Path, out_dir: Path, db_path: Path,
                    inventory_path: Path | None = DEFAULT_INVENTORY) -> InventoryIndex:
    entries = load_entries(shdata, inventory_path)
    index = build_index(entries)
    write_json(out_dir / "inventory_summary.json", index.summary())
    write_sqlite(index, db_path)
    return index


def resmirror_types_used(index: InventoryIndex) -> dict[int, int]:
    """参数预留：真正的使用统计在 tables.py 的 resmirror_usage 表中。"""
    return {}


def part_expected_dirs() -> dict[int, str]:
    return {value: label for value, label in idspaces.RESMIRROR_PART_TYPES.items()}
