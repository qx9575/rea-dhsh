"""原有 17 职业预设的资源覆盖检查。

读取 viewjob.csv 的 13 列，逐行检查衣服/手套/鞋子/武器/坐骑/骑甲/星神造型
在 player*.pak 中是否存在；输出 SQLite 明细 + JSON 摘要。
"""
from __future__ import annotations

import json
import sqlite3
from dataclasses import dataclass
from pathlib import Path

from art_pipeline.common import TableText, write_json
from art_pipeline.inventory import InventoryIndex
from art_pipeline import idspaces


@dataclass(frozen=True)
class Preset:
    career: int
    advance: int
    mount_tier: int
    gender: int
    cloth: int
    gloves: int
    shoes: int
    weapon: int
    horse: int
    harmor_head: int
    harmor_body: int
    hero_branch: int
    stargod_model: int


def _int(value: str | None, default: int = -1) -> int:
    if value is None:
        return default
    text = value.strip()
    try:
        return int(text, 10)
    except ValueError:
        return default


def parse_presets(table: TableText) -> list[Preset]:
    presets: list[Preset] = []
    for row in table.rows:
        if len(row) < 11:
            continue
        vals = [_int(row[i] if i < len(row) else None) for i in range(13)]
        if vals[0] < 0:
            continue
        presets.append(Preset(*vals))
    return presets


PART_FIELDS = (
    ("cloth", "cloth"),
    ("gloves", "lcuff"),
    ("gloves", "rcuff"),
    ("shoes", "shoe"),
    ("weapon", "weapon"),
    ("horse", "horse"),
    ("harmor_head", "harmorhead"),
    ("harmor_body", "harmorbody"),
)


def _check_part(index: InventoryIndex, part: str, model: int,
                model_to_parts: dict[int, set[str]]) -> tuple[bool, str, str, str, str]:
    if model < 0:
        return True, "", "", "unconfigured", "未配置(-1)"
    infos = index.model_candidates(part, model)
    if not infos:
        others = sorted(model_to_parts.get(model, set()))
        if others:
            return False, "", "", "id_space_mismatch", "该 ID 存在于: " + ",".join(others)
        return False, "", "", "missing", "资源缺失"
    info = index.best_model(part, model)
    assert info is not None
    actions = ",".join(str(a) for a in sorted(info.actions))
    return True, info.pak, actions, "found", ""


def build_coverage(presets: list[Preset], index: InventoryIndex,
                   db_path: Path, out_dir: Path) -> dict:
    details: list[tuple] = []
    summary_status: dict[str, int] = {}
    summary_missing: dict[tuple[int, str, str], int] = {}
    stargod_found = 0
    stargod_total = 0
    model_to_parts: dict[int, set[str]] = {}
    for (part_key, model) in index.models:
        model_to_parts.setdefault(model, set()).add(part_key)
    for p in presets:
        for field_name, part in PART_FIELDS:
            model = getattr(p, field_name)
            found, pak, actions, status, note = _check_part(
                index, part, model, model_to_parts)
            summary_status[status] = summary_status.get(status, 0) + 1
            if status in ("missing", "id_space_mismatch"):
                key = (p.career, field_name, status)
                summary_missing[key] = summary_missing.get(key, 0) + 1
            details.append((p.career, p.advance, p.mount_tier, p.gender,
                            field_name, part, model, int(found), pak, actions,
                            status, note))
        stargod = p.stargod_model
        if stargod > 0:
            stargod_total += 1
            npc = index.npc_models.get(stargod)
            if npc is not None:
                stargod_found += 1
                summary_status["found_npc"] = summary_status.get("found_npc", 0) + 1
                details.append((p.career, p.advance, p.mount_tier, p.gender,
                                "stargod_model", "npc", stargod, 1,
                                npc.pak, ",".join(str(a) for a in sorted(npc.actions)),
                                "found_npc", "命中 NPC 模型"))
                continue
            hit = None
            for part in ("body", "head", "effect", "horse", "harmorbody"):
                if index.has_model(part, stargod):
                    hit = part
                    break
            if hit is not None:
                stargod_found += 1
                summary_status["found_player"] = summary_status.get("found_player", 0) + 1
                note = f"命中 {hit}"
                best = index.best_model(hit, stargod)
                hit_pak = best.pak if best else ""
                details.append((p.career, p.advance, p.mount_tier, p.gender,
                                "stargod_model", hit, stargod, 1, hit_pak, "",
                                "found_player", note))
            else:
                summary_status["missing"] = summary_status.get("missing", 0) + 1
                key = (p.career, "stargod_model", "missing")
                summary_missing[key] = summary_missing.get(key, 0) + 1
                details.append((p.career, p.advance, p.mount_tier, p.gender,
                                "stargod_model", "?", stargod, 0, "", "",
                                "missing", "未在 player*.pak 或 npc*.pak 中找到；可能为图标/未实装"))

    conn = sqlite3.connect(db_path)
    cur = conn.cursor()
    cur.execute("DROP TABLE IF EXISTS career_coverage")
    cur.execute("""CREATE TABLE career_coverage(
        career INTEGER, advance INTEGER, mount_tier INTEGER, gender INTEGER,
        field TEXT, part TEXT, model INTEGER, found INTEGER,
        pak TEXT, actions TEXT, status TEXT, note TEXT)""")
    cur.executemany("INSERT INTO career_coverage VALUES (?,?,?,?,?,?,?,?,?,?,?,?)", details)
    conn.commit()
    conn.close()

    missing_rows = [
        {"career": career, "career_name": idspaces.CAREERS.get(career, str(career)),
         "field": field, "status": status, "count": count}
        for (career, field, status), count in sorted(summary_missing.items())
    ]
    summary = {
        "presets": len(presets),
        "expected_presets": 17 * 3 * 2 * 3,
        "part_checks": len(details),
        "status_counts": summary_status,
        "missing_checks": summary_status.get("missing", 0),
        "id_space_mismatch_checks": summary_status.get("id_space_mismatch", 0),
        "missing_by_career_field": missing_rows,
        "stargod": {
            "rows_with_model": stargod_total,
            "found": stargod_found,
            "unresolved": stargod_total - stargod_found,
        },
        "by_career": _by_career(details),
    }
    write_json(out_dir / "career_coverage.json", summary)
    return summary


def _by_career(details: list[tuple]) -> dict[str, dict]:
    out: dict[str, dict] = {}
    for row in details:
        career = row[0]
        entry = out.setdefault(str(career), {
            "career_name": idspaces.CAREERS.get(career, str(career)),
            "rows": 0, "found": 0, "missing": 0,
            "id_space_mismatch": 0, "unconfigured": 0,
            "found_npc": 0, "found_player": 0,
        })
        entry["rows"] += 1
        status = row[10]
        entry[status] = entry.get(status, 0) + 1
    return dict(sorted(out.items(), key=lambda kv: int(kv[0])))


def build_careers(tables: dict[str, TableText], index: InventoryIndex,
                  db_path: Path, out_dir: Path) -> dict:
    table = tables.get("viewjob")
    if table is None:
        raise SystemExit("viewjob.csv not found")
    presets = parse_presets(table)
    return build_coverage(presets, index, db_path, out_dir)
