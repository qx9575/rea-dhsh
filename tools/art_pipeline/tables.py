"""表字典：读取分类/职业/挂点/技能表，落 SQLite + JSON。只读原 PAK。"""
from __future__ import annotations

import json
import sqlite3
from dataclasses import dataclass
from pathlib import Path

from art_pipeline.common import (
    PakReader,
    TableText,
    read_table,
    to_int,
    write_json,
)
from art_pipeline import idspaces


@dataclass(frozen=True)
class TableSpec:
    key: str
    pak: str
    path: str
    description: str
    header_mode: str = "comment"   # comment | first | none
    min_cols: int = 1
    required: bool = True


TABLE_SPECS: tuple[TableSpec, ...] = (
    TableSpec("resmirror", "config.pak", "misc/resmirror.csv",
              "部件类型/动作/朝向的镜像规则；含权威 part type 与 action 枚举", "comment", 7),
    TableSpec("mirror_effect", "config.pak", "magic/MirrorEffect.csv",
              "部件类型 + bodyID + action + face0..7 的镜像开关", "comment", 10),
    TableSpec("viewjob", "config.pak", "ui/login/viewjob.csv",
              "职业/进阶/坐骑/性别 → 装备、坐骑、骑甲、星神造型预设", "comment", 11),
    TableSpec("bodyidequal", "config.pak", "bodyidequal.csv",
              "怪物/对等身体 ID → 各部件 ID、颜色、武器光效", "comment", 2),
    TableSpec("partinfo", "config.pak", "ui/login/partinfo.csv",
              "建角部件：种类/ID/名称/六种颜色", "comment", 3),
    TableSpec("shapeclrinfo", "config.pak", "ui/user/shapeclrinfo.csv",
              "部件类型/ID → 基础色与 5 个变色参数", "comment", 3),
    TableSpec("mouthairid", "config.pak", "misc/mouthairid.csv",
              "坐骑/体型/飞行/种族 → 头发服务器 ID 与资源 ID", "comment", 6),
    TableSpec("mouthairmirror", "config.pak", "misc/mouthairmirror.csv",
              "旧头发 ID → 新头发 ID", "comment", 3),
    TableSpec("itemtypename", "config.pak", "item/itemtypename.csv",
              "物品槽位 ID → 名称", "none", 2),
    TableSpec("petinfo", "config.pak", "ui/pet/petinfo.csv",
              "宠物分类/名称/说明", "none", 3),
    TableSpec("petequipsetname", "config.pak", "item/petequipsetname.csv",
              "宠物装备套装名", "comment", 2),
    TableSpec("horseinfo", "config.pak", "ui/horse/horseinfo.csv",
              "坐骑 ID/怪物 ID/等级/职业/技能/类型/品级/升级链", "comment", 7),
    TableSpec("horseoffset", "config.pak", "misc/horseoffset.csv",
              "人物/坐骑/宠物/翅膀/披风/星将/头部/脚底/中心特效挂点", "comment", 12),
    TableSpec("cloakoffset", "config.pak", "misc/cloakoffset.csv",
              "披风 ID/人物 ID/坐骑/动作/方向 → 偏移", "comment", 7),
    TableSpec("hpoffset", "config.pak", "ui/combat/hpoffset.csv",
              "目标 ID → 视觉高度偏移", "comment", 2),
    TableSpec("faeriewing", "config.pak", "ui/faeriewing/faeriewing.csv",
              "翅膀等级/资源 ID/属性", "first", 12),
    TableSpec("colorconvert", "config.pak", "misc/colorconvert.csv",
              "坐骑部件颜色 → 新颜色", "comment", 6),
    TableSpec("special_suit", "config.pak", "specialSuit.csv",
              "套装 → 面具/召唤兽/坐骑/1-3 件效果", "first", 7),
    TableSpec("monster_equip_suits", "config.pak", "ui/property/summon/MonsterEquipSuits.csv",
              "召唤兽装备套装", "first", 6),
    TableSpec("summoner_equip_recipe", "config.pak", "SummonerEquipRecipe.csv",
              "召唤兽装备配方", "first", 5),
    TableSpec("skill", "config.pak", "magic/Skill.csv",
              "玩家技能树/光效/攻击帧/费用", "first", 37),
    TableSpec("pet_skill", "config.pak", "ui/pet/skillinfo.csv",
              "宠物技能", "first", 6),
    TableSpec("horse_skill", "config.pak", "ui/horse/horseskill.csv",
              "坐骑技能", "comment", 6),
    TableSpec("ib_skill", "config.pak", "magic/ibskill.csv",
              "IB/骑术技能", "comment", 4),
    TableSpec("armory_skill", "config.pak", "magic/ArmorySkill.csv",
              "兵甲/装备技能", "comment", 4),
    TableSpec("specialty", "config.pak", "ui/stargod/Specialty.csv",
              "星神特技", "first", 7),
    TableSpec("item_spec", "ui.pak", "item/itemSpecData.csv",
              "物品库：ID/图标/名称/职业权限/阶段/等级/槽位/使用类型", "comment", 31),
    TableSpec("monsters", "config.pak", "Monsters.csv",
              "怪物/召唤兽基础表", "comment", 2),
    TableSpec("npchead", "config.pak", "monster/npchead.csv",
              "NPC 头像/模型映射", "comment", 2),
    TableSpec("career_info", "config.pak", "ui/career/CareerInfo.csv",
              "职业进阶任务条件", "first", 2),
    TableSpec("player", "config.pak", "player.csv",
              "玩家基础表（待解释）", "comment", 2),
    TableSpec("lua_window_map", "config.pak", "lua/LuaWindowMap.csv",
              "Lua 窗口名映射（路径按 basename 回退）", "comment", 2),
    TableSpec("msg", "config.pak", "msg.csv",
              "消息节流配置", "comment", 2),
    TableSpec("allstruct", "config.pak", "allstruct.csv",
              "协议/结构定义（字符串索引 + struct begin/end）", "comment", 2),
)


def load_tables(reader: PakReader) -> dict[str, TableText]:
    out: dict[str, TableText] = {}
    for spec in TABLE_SPECS:
        table = read_table(reader, spec.key, spec.pak, spec.path)
        if table is None:
            if spec.required:
                print(f"[tables] WARN missing {spec.key}: {spec.pak}:{spec.path}")
            continue
        out[spec.key] = table
    return out


def data_rows(spec: TableSpec, table: TableText) -> list[tuple[str, ...]]:
    rows = list(table.rows)
    if spec.header_mode == "first" and rows:
        rows = rows[1:]
    return [row for row in rows if len(row) >= spec.min_cols]


def _int(value: str | None, default: int = -1) -> int:
    result = to_int(value, default)
    return default if result is None else result


def _ints(row: tuple[str, ...], count: int, default: int = -1) -> list[int]:
    return [_int(row[i] if i < len(row) else None, default) for i in range(count)]


def build_sqlite(tables: dict[str, TableText], db_path: Path) -> None:
    db_path.parent.mkdir(parents=True, exist_ok=True)
    if db_path.exists():
        db_path.unlink()
    conn = sqlite3.connect(db_path)
    cur = conn.cursor()
    cur.execute("""CREATE TABLE table_specs(
        key TEXT PRIMARY KEY, pak TEXT, path TEXT, description TEXT,
        header_mode TEXT, row_count INTEGER, comment_count INTEGER)""")
    cur.execute("""CREATE TABLE table_rows(
        table_key TEXT, row_index INTEGER, cells TEXT,
        PRIMARY KEY(table_key, row_index))""")
    cur.execute("""CREATE TABLE id_spaces(
        space TEXT, value TEXT, label TEXT, source TEXT, confidence TEXT)""")
    cur.execute("""CREATE TABLE viewjob_presets(
        career INTEGER, advance INTEGER, mount_tier INTEGER, gender INTEGER,
        cloth INTEGER, gloves INTEGER, shoes INTEGER, weapon INTEGER,
        horse INTEGER, harmor_head INTEGER, harmor_body INTEGER,
        hero_branch INTEGER, stargod_model INTEGER)""")
    cur.execute("""CREATE TABLE bodyidequal(
        monster_id INTEGER, body_id INTEGER, head_id INTEGER, head_color INTEGER,
        cloth_id INTEGER, cloth_color INTEGER, weapon_id INTEGER, weapon_color INTEGER,
        weapon_effect_id INTEGER, weapon_effect_color INTEGER,
        shoe_id INTEGER, shoe_color INTEGER, mask_id INTEGER, mask_color INTEGER,
        cuff_id INTEGER, cuff_color INTEGER)""")
    cur.execute("""CREATE TABLE partinfo(
        kind INTEGER, part_id INTEGER, name TEXT,
        color1 TEXT, color2 TEXT, color3 TEXT, color4 TEXT, color5 TEXT, color6 TEXT)""")
    cur.execute("""CREATE TABLE shapeclr(
        kind INTEGER, part_id INTEGER, base_color INTEGER,
        p1 INTEGER, p2 INTEGER, p3 INTEGER, p4 INTEGER, p5 INTEGER, note TEXT)""")
    cur.execute("""CREATE TABLE hair_map(
        mount_id INTEGER, body_shape INTEGER, flying INTEGER, race INTEGER,
        hair_server_id TEXT, hair_resource_id TEXT)""")
    cur.execute("""CREATE TABLE mount_info(
        mount_id INTEGER, monster_id INTEGER, level INTEGER, best_career INTEGER,
        require_skill INTEGER, names TEXT, description TEXT, flying INTEGER,
        yuanbao INTEGER, mount_type TEXT, grade TEXT, special TEXT,
        source TEXT, dex_order INTEGER, upgrade_mount_id INTEGER,
        gender INTEGER, zodiac TEXT, appearance TEXT, personality TEXT,
        race TEXT, activatable INTEGER)""")
    cur.execute("""CREATE TABLE attach_horse(
        character_id INTEGER, mount_id INTEGER, character_dx INTEGER,
        shadow_dx INTEGER, pet_dx INTEGER, wing_dx INTEGER, cloak_dx INTEGER,
        stargod_dx INTEGER, head_dx INTEGER, foot_aura_dx INTEGER,
        foot_effect_dx INTEGER, center_effect_dx INTEGER)""")
    cur.execute("""CREATE TABLE attach_cloak(
        cloak_id INTEGER, character_id INTEGER, mount_id INTEGER,
        action INTEGER, direction INTEGER, dx INTEGER, dy INTEGER)""")
    cur.execute("""CREATE TABLE hpoffset(target_id INTEGER, height_offset INTEGER)""")
    cur.execute("""CREATE TABLE colorconvert(
        part_type INTEGER, part_id INTEGER, color INTEGER,
        new_part_id INTEGER, new_color INTEGER, note TEXT)""")
    cur.execute("""CREATE TABLE special_suit(
        suit TEXT, mask TEXT, summon TEXT, mount TEXT,
        one_piece TEXT, two_piece TEXT, three_piece TEXT)""")
    cur.execute("""CREATE TABLE monster_equip_suit(
        equip_id INTEGER, name TEXT, suit TEXT, suit_attr INTEGER,
        exclusive_doll TEXT, generic_doll TEXT)""")
    cur.execute("""CREATE TABLE skill(
        skill_id INTEGER, name TEXT, description TEXT, career INTEGER,
        system INTEGER, tier INTEGER, icon INTEGER, target_count INTEGER,
        hit_effect INTEGER, attack_effect INTEGER, fail_effect INTEGER,
        rush INTEGER, enemy INTEGER, damage_json TEXT, attack_frames_json TEXT,
        sp_json TEXT, money_json TEXT, points_json TEXT,
        ui_x INTEGER, ui_y INTEGER, prereq_id INTEGER,
        bg_color TEXT, alpha TEXT)""")
    cur.execute("""CREATE TABLE pet_skill(
        skill_id INTEGER, name TEXT, description TEXT, kind INTEGER,
        money INTEGER, abandon_money INTEGER)""")
    cur.execute("""CREATE TABLE horse_skill(
        skill_row INTEGER, skill_id INTEGER, skill_level INTEGER,
        mount_level INTEGER, name TEXT, description TEXT)""")
    cur.execute("""CREATE TABLE ib_skill(
        skill_id INTEGER, icon INTEGER, name TEXT, description TEXT)""")
    cur.execute("""CREATE TABLE armory_skill(
        name TEXT, list_name TEXT, level INTEGER, description TEXT)""")
    cur.execute("""CREATE TABLE specialty(
        specialty_id INTEGER, icon INTEGER, name TEXT, description TEXT,
        terrain TEXT, monster TEXT, starcard INTEGER)""")
    cur.execute("""CREATE TABLE item_type(slot INTEGER, name TEXT)""")
    cur.execute("""CREATE TABLE item_spec(
        item_id INTEGER, icon INTEGER, name TEXT, career_flags_json TEXT,
        phase INTEGER, level INTEGER, stack INTEGER, bag_size INTEGER,
        use_type INTEGER, use_range INTEGER, dead_use INTEGER, auto_use INTEGER,
        description TEXT, sound INTEGER, slot INTEGER, use_action INTEGER)""")
    cur.execute("""CREATE TABLE structure(
        name TEXT, field_count INTEGER, fields_json TEXT)""")

    cur.executemany("INSERT INTO id_spaces VALUES (?,?,?,?,?)",
                    [(e.space, e.value, e.label, e.source, e.confidence)
                     for e in idspaces.id_entries()])

    spec_by_key = {s.key: s for s in TABLE_SPECS}
    derived: dict[str, TableText] = {}
    for key, table in tables.items():
        spec = spec_by_key[key]
        rows = table.rows[1:] if spec.header_mode == "first" and table.rows else table.rows
        derived[key] = TableText(key, table.pak, table.path, table.text,
                                 table.comments, tuple(rows))
    for key, table in tables.items():
        spec = spec_by_key[key]
        cur.execute("INSERT INTO table_specs VALUES (?,?,?,?,?,?,?)",
                    (key, table.pak, table.path, spec.description, spec.header_mode,
                     len(table.rows), len(table.comments)))
        cur.executemany("INSERT INTO table_rows VALUES (?,?,?)",
                        [(key, i, json.dumps(list(row), ensure_ascii=False))
                         for i, row in enumerate(table.rows)])

    # ---- derived tables ----
    if "resmirror" in derived:
        _derive_resmirror(cur, derived["resmirror"])
    if "viewjob" in derived:
        _derive_viewjob(cur, derived["viewjob"])
    if "bodyidequal" in derived:
        _derive_bodyidequal(cur, derived["bodyidequal"])
    if "partinfo" in derived:
        _derive_partinfo(cur, derived["partinfo"])
    if "shapeclrinfo" in derived:
        _derive_shapeclr(cur, derived["shapeclrinfo"])
    if "mouthairid" in derived:
        _derive_hair(cur, derived["mouthairid"])
    if "horseinfo" in derived:
        _derive_horse(cur, derived["horseinfo"])
    if "horseoffset" in derived:
        _derive_attach_horse(cur, derived["horseoffset"])
    if "cloakoffset" in derived:
        _derive_attach_cloak(cur, derived["cloakoffset"])
    if "hpoffset" in derived:
        _derive_hpoffset(cur, derived["hpoffset"])
    if "colorconvert" in derived:
        _derive_colorconvert(cur, derived["colorconvert"])
    if "special_suit" in derived:
        _derive_special_suit(cur, derived["special_suit"])
    if "monster_equip_suits" in derived:
        _derive_monster_suit(cur, derived["monster_equip_suits"])
    if "skill" in derived:
        _derive_skill(cur, derived["skill"])
    if "pet_skill" in derived:
        _derive_pet_skill(cur, derived["pet_skill"])
    if "horse_skill" in derived:
        _derive_horse_skill(cur, derived["horse_skill"])
    if "ib_skill" in derived:
        _derive_ib_skill(cur, derived["ib_skill"])
    if "armory_skill" in derived:
        _derive_armory_skill(cur, derived["armory_skill"])
    if "specialty" in derived:
        _derive_specialty(cur, derived["specialty"])
    if "itemtypename" in derived:
        _derive_item_type(cur, derived["itemtypename"])
    if "item_spec" in derived:
        _derive_item_spec(cur, derived["item_spec"])
    if "allstruct" in derived:
        _derive_structures(cur, derived["allstruct"])

    conn.commit()
    conn.close()


def _rows(table: TableText) -> list[tuple[str, ...]]:
    return [row for row in table.rows if any(cell for cell in row)]


def _derive_resmirror(cur, table: TableText) -> None:
    # 数据行只用于校验实际使用的 part type/action；枚举来自 idspaces。
    used_types: dict[int, int] = {}
    used_actions: dict[int, int] = {}
    for row in _rows(table):
        if len(row) < 7:
            continue
        part_type = _int(row[0])
        action = _int(row[2])
        if part_type >= 0:
            used_types[part_type] = used_types.get(part_type, 0) + 1
        if action >= 0:
            used_actions[action] = used_actions.get(action, 0) + 1
    cur.execute("CREATE TABLE resmirror_usage(space TEXT, value INTEGER, rows INTEGER)")
    cur.executemany("INSERT INTO resmirror_usage VALUES (?,?,?)",
                    [("part_type", k, v) for k, v in sorted(used_types.items())]
                    + [("action", k, v) for k, v in sorted(used_actions.items())])


def _derive_viewjob(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 11:
            continue
        vals = _ints(row, 13)
        if vals[0] < 0:
            continue
        cur.execute("INSERT INTO viewjob_presets VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    tuple(vals))


def _derive_bodyidequal(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 2:
            continue
        vals = _ints(row, 16)
        cur.execute("INSERT INTO bodyidequal VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    tuple(vals))


def _derive_partinfo(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 3:
            continue
        pad = list(row) + [""] * 6
        cur.execute("INSERT INTO partinfo VALUES (?,?,?,?,?,?,?,?,?)",
                    (_int(row[0]), _int(row[1]), row[2],
                     pad[3], pad[4], pad[5], pad[6], pad[7], pad[8]))


def _derive_shapeclr(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 3:
            continue
        vals = _ints(row, 8)
        note = row[8] if len(row) > 8 else ""
        cur.execute("INSERT INTO shapeclr VALUES (?,?,?,?,?,?,?,?,?)",
                    (vals[0], vals[1], vals[2], vals[3], vals[4], vals[5],
                     vals[6], vals[7], note))


def _derive_hair(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 6:
            continue
        cur.execute("INSERT INTO hair_map VALUES (?,?,?,?,?,?)",
                    (_int(row[0]), _int(row[1]), _int(row[2]), _int(row[3]),
                     row[4].strip(), row[5].strip()))


def _derive_horse(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 7:
            continue
        pad = list(row) + [""] * 21
        cur.execute("INSERT INTO mount_info VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    (_int(pad[0]), _int(pad[1]), _int(pad[2]), _int(pad[3]),
                     _int(pad[4]), pad[5], pad[6], _int(pad[7]), _int(pad[8]),
                     pad[9], pad[10], pad[11], pad[12], _int(pad[13]),
                     _int(pad[14]), _int(pad[15]), pad[16], pad[17], pad[18],
                     pad[19], _int(pad[20])))


def _derive_attach_horse(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 2:
            continue
        vals = _ints(row, 12)
        cur.execute("INSERT INTO attach_horse VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                    tuple(vals))


def _derive_attach_cloak(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 7:
            continue
        vals = _ints(row, 7)
        cur.execute("INSERT INTO attach_cloak VALUES (?,?,?,?,?,?,?)", tuple(vals))


def _derive_hpoffset(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 2:
            continue
        cur.execute("INSERT INTO hpoffset VALUES (?,?)", (_int(row[0]), _int(row[1])))


def _derive_colorconvert(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 5:
            continue
        cur.execute("INSERT INTO colorconvert VALUES (?,?,?,?,?,?)",
                    (_int(row[0]), _int(row[1]), _int(row[2]),
                     _int(row[3]), _int(row[4]), row[5] if len(row) > 5 else ""))


def _derive_special_suit(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 7:
            continue
        cur.execute("INSERT INTO special_suit VALUES (?,?,?,?,?,?,?)", tuple(row[:7]))


def _derive_monster_suit(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 6:
            continue
        cur.execute("INSERT INTO monster_equip_suit VALUES (?,?,?,?,?,?)",
                    (_int(row[0]), row[1], row[2], _int(row[3]), row[4], row[5]))


def _derive_skill(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 34:
            continue
        pad = list(row) + [""] * 40
        cur.execute("INSERT INTO skill VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    (_int(pad[0]), pad[1], pad[2], _int(pad[3]), _int(pad[4]),
                     _int(pad[5]), _int(pad[6]), _int(pad[7]), _int(pad[8]),
                     _int(pad[9]), _int(pad[10]), _int(pad[11]), _int(pad[12]),
                     json.dumps(pad[13:21], ensure_ascii=False),
                     json.dumps(pad[21:29], ensure_ascii=False),
                     pad[29], pad[30], pad[31], _int(pad[32]), _int(pad[33]),
                     _int(pad[34]), pad[35], pad[36]))


def _derive_pet_skill(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 6:
            continue
        cur.execute("INSERT INTO pet_skill VALUES (?,?,?,?,?,?)",
                    (_int(row[0]), row[1], row[2], _int(row[3]), _int(row[4]), _int(row[5])))


def _derive_horse_skill(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 6:
            continue
        cur.execute("INSERT INTO horse_skill VALUES (?,?,?,?,?,?)",
                    (_int(row[0]), _int(row[1]), _int(row[2]), _int(row[3]), row[4], row[5]))


def _derive_ib_skill(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 4:
            continue
        cur.execute("INSERT INTO ib_skill VALUES (?,?,?,?)",
                    (_int(row[0]), _int(row[1]), row[2], row[3]))


def _derive_armory_skill(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 4:
            continue
        cur.execute("INSERT INTO armory_skill VALUES (?,?,?,?)",
                    (row[0], row[1], _int(row[2]), row[3]))


def _derive_specialty(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 7:
            continue
        cur.execute("INSERT INTO specialty VALUES (?,?,?,?,?,?,?)",
                    (_int(row[0]), _int(row[1]), row[2], row[3], row[4], row[5], _int(row[6])))


def _derive_item_type(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 2:
            continue
        cur.execute("INSERT INTO item_type VALUES (?,?)", (_int(row[0]), row[1]))


def _derive_item_spec(cur, table: TableText) -> None:
    for row in _rows(table):
        if len(row) < 31:
            continue
        pad = list(row) + [""] * 32
        careers = [pad[i].strip() for i in range(3, 20)]
        cur.execute("INSERT INTO item_spec VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    (_int(pad[0]), _int(pad[1]), pad[2],
                     json.dumps(careers, ensure_ascii=False), _int(pad[20]),
                     _int(pad[21]), _int(pad[22]), _int(pad[23]), _int(pad[24]),
                     _int(pad[25]), _int(pad[26]), _int(pad[27]), pad[28],
                     _int(pad[29]), _int(pad[30]), _int(pad[31])))


def _derive_structures(cur, table: TableText) -> None:
    current = None
    fields: list[dict] = []
    for row in _rows(table):
        if len(row) < 2:
            continue
        kind, value = row[0].strip(), row[1].strip()
        if kind == "struct" and value == "begin":
            current = row[2].strip() if len(row) > 2 else ""
            fields = []
            continue
        if kind == "struct" and value == "end":
            if current:
                cur.execute("INSERT INTO structure VALUES (?,?,?)",
                            (current, len(fields), json.dumps(fields, ensure_ascii=False)))
            current = None
            fields = []
            continue
        if current:
            fields.append({
                "type": kind,
                "name": value,
                "length": row[2].strip() if len(row) > 2 else "",
                "display": row[3].strip() if len(row) > 3 else "",
            })


def summarize_tables(tables: dict[str, TableText]) -> dict:
    return {
        spec.key: {
            "pak": tables[spec.key].pak,
            "path": tables[spec.key].path,
            "description": spec.description,
            "rows": len(tables[spec.key].rows),
            "comments": len(tables[spec.key].comments),
            "header_mode": spec.header_mode,
        }
        for spec in TABLE_SPECS if spec.key in tables
    }


def dump_table_texts(tables: dict[str, TableText], out_dir: Path) -> None:
    table_dir = out_dir / "tables"
    table_dir.mkdir(parents=True, exist_ok=True)
    for key, table in tables.items():
        (table_dir / f"{key}.csv").write_text(table.text, encoding="utf-8")


def build_tables(shdata: Path, out_dir: Path) -> dict[str, TableText]:
    reader = PakReader(shdata)
    tables = load_tables(reader)
    out_dir.mkdir(parents=True, exist_ok=True)
    dump_table_texts(tables, out_dir)
    write_json(out_dir / "tables.json", summarize_tables(tables))
    build_sqlite(tables, out_dir / "art_pipeline.sqlite")
    return tables
