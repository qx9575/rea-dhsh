"""由表/资源/覆盖数据生成 docs/art-pipeline/*.md。"""
from __future__ import annotations

from collections import Counter
from pathlib import Path

from art_pipeline.common import DEFAULT_DOCS, TableText
from art_pipeline.inventory import InventoryIndex, KNOWN_PLAYER_PARTS
from art_pipeline import idspaces
from art_pipeline.scp_index import ScpIndex
from art_pipeline.lua_index import LuaIndex
from art_pipeline import engine_findings


def _md_table(headers: list[str], rows: list[list]) -> str:
    out = ["| " + " | ".join(headers) + " |",
           "|" + "|".join(["---"] * len(headers)) + "|"]
    for row in rows:
        out.append("| " + " | ".join("" if v is None else str(v) for v in row) + " |")
    return "\n".join(out)


def _resmirror_usage(table: TableText) -> tuple[Counter, Counter]:
    parts = Counter()
    actions = Counter()
    for row in table.rows:
        if len(row) < 7:
            continue
        try:
            parts[int(row[0])] += 1
            actions[int(row[2])] += 1
        except ValueError:
            continue
    return parts, actions


def _action_label(action: int) -> str:
    for base, label in sorted(idspaces.ACTION_GROUPS.items(), reverse=True):
        if base == 100:
            if 100 <= action <= 199:
                return label
        elif base <= action < base + 10:
            return label
    return idspaces.PSEUDO_ACTIONS.get(action, "")


def taxonomy_md(tables: dict[str, TableText], index: InventoryIndex) -> str:
    lines = [
        "# 纸娃娃/美术资源分类（权威表优先）",
        "",
        "- 分析基线：`D:\\software\\dhsh\\shdata`；只读",
        "- 权威部件/动作枚举：`config.pak:misc/resmirror.csv`",
        "- 资源统计：`pak_inventory.json`（523k 叶子）",
        "",
        "## 1. 权威部件类型（resmirror）",
        "",
        _md_table(["Part Type", "含义", "备注"], [
            [k, v,
             "引擎部件类型" if k in (0, 1, 2, 7, 9, 10, 11, 12, 13, 14, 15)
             else "枚举已确认；资源/消费点待验证"]
            for k, v in sorted(idspaces.RESMIRROR_PART_TYPES.items())
        ]),
        "",
    ]
    if "resmirror" in tables:
        parts, actions = _resmirror_usage(tables["resmirror"])
        lines += [
            "实际在 mirror 表中出现的类型：" ,
            "",
            _md_table(["Part Type", "含义", "行数"], [
                [k, idspaces.RESMIRROR_PART_TYPES.get(k, "?"), v]
                for k, v in sorted(parts.items())
            ]),
            "",
            "实际出现的动作 ID：" ,
            "",
            _md_table(["Action", "语义", "行数"], [
                [k, _action_label(k), v] for k, v in sorted(actions.items())
            ]),
            "",
        ]
    lines += [
        "## 2. 三套部件枚举必须分开",
        "",
        _md_table(["ID", "resmirror", "partinfo", "shapeclrinfo"], [
            [i,
             idspaces.RESMIRROR_PART_TYPES.get(i, ""),
             idspaces.PARTINFO_PART_TYPES.get(i, ""),
             idspaces.SHAPECLR_PART_TYPES.get(i, "")]
            for i in sorted(set(idspaces.RESMIRROR_PART_TYPES)
                            | set(idspaces.PARTINFO_PART_TYPES)
                            | set(idspaces.SHAPECLR_PART_TYPES))
        ]),
        "",
        "> 查看器只使用了一套 `PARTS`，把三套枚举混在一起；这是类别不清的主要来源。",
        "",
        "## 3. 资源目录实际统计（player*.pak）",
        "",
        _md_table(["part", "实际目录", "文件数", "模型数", "动作 ID"], [
            [key, ",".join(sorted(info.dirs)), info.files, len(info.models),
             ",".join(str(a) for a in sorted(info.actions))]
            for key, info in sorted(index.parts.items())
        ]),
        "",
        "## 4. 查看器 PARTS 对照",
        "",
        _md_table(["查看器部件", "resmirror 对应", "遗漏/风险"], [
            ["body", "1 身体", ""],
            ["head", "0 头部", ""],
            ["weapon", "2 武器", ""],
            ["cloth", "7 衣服", ""],
            ["shoe", "11 脚部", ""],
            ["mask", "9 脸部", "名称不准确"],
            ["Lcuff", "10 手腕", "与 lcuff 大小写资源合并"],
            ["Rcuff", "12 右腕", "与 rcuff 大小写资源合并"],
            ["horse", "13 坐骑", "未接 horseoffset"],
            ["harmorbody", "14 骑甲身", ""],
            ["harmorhead", "15 骑甲头", ""],
            ["pet", "3 宠物", "未接宠物技能/装备"],
            ["effect", "4 魔法？", "与 magic 的关系待验证"],
            ["—", "8 武器光效", "查看器完全缺失"],
            ["—", "16 裤子", "查看器完全缺失，资源目录未知"],
            ["—", "发型", "mouthairid.csv，独立系统"],
            ["—", "翅膀", "faeriewing.csv，独立系统"],
            ["—", "披风", "cloakoffset.csv，独立系统"],
            ["—", "星将光效", "horseoffset 星将偏移 + viewjob 星神造型"],
        ]),
        "",
        "## 5. 分类树（按系统而非 UI 下拉）",
        "",
        "```text",
        "角色",
        "├─ 本体：body / head / cloth / shoe / mask / Lcuff / Rcuff / weapon / weapon effect / pants",
        "├─ 外观扩展：hair（mouthairid）/ wing（faeriewing）/ cloak（cloakoffset）/ shadow",
        "├─ 坐骑：horse / harmorbody / harmorhead / horseoffset / colorconvert / horseskill",
        "├─ 宠物与召唤兽：pet / petinfo / petequipsetname / MonsterEquipSuits / SummonerEquipRecipe",
        "├─ 技能：Skill.csv（玩家）/ pet skillinfo / horseskill / ibskill / ArmorySkill / Specialty",
        "└─ 星神：viewjob 星神造型列 + Specialty + horseoffset 星将偏移",
        "```",
        "",
        "## 6. 结论",
        "",
        "- Observed：`resmirror` 的部件类型与动作语义、资源目录实际统计、三套枚举的差异。",
        "- Inferred：`effect` 对应 part type 4；需要引擎消费函数确认。",
        "- Observed：`mask` 对应 part type 9 脸部（resmirror 表头直接给出）。",
        "- Unknown：part type 16 裤子、8 武器光效的资源路径；星神造型 ID 的解析方式。",
        "",
    ]
    return "\n".join(lines)


def tables_md(tables: dict[str, TableText]) -> str:
    rows = []
    for key, table in tables.items():
        if key == "resmirror":
            note = "部件/动作枚举 + 镜像规则"
        elif key == "viewjob":
            note = "职业/进阶/坐骑/性别 → 模型预设（13 列）"
        elif key == "bodyidequal":
            note = "怪物/身体 ID → 部件 + 颜色 + 武器光效"
        elif key in ("horseoffset", "cloakoffset", "hpoffset"):
            note = "挂点/偏移"
        elif key == "skill":
            note = "玩家技能树/光效/攻击帧/费用"
        else:
            note = ""
        rows.append([f"`{key}`", f"`{table.pak}:{table.path}`", len(table.rows), note])
    return "\n".join([
        "# 表字典与 ID 空间",
        "",
        "> 路径为 PAK 内逻辑路径；读取工具：`tools/art_pipeline`。",
        "",
        "## 1. 已纳入分析的表",
        "",
        _md_table(["key", "PAK 路径", "行数", "用途"], rows),
        "",
        "## 2. ID 空间",
        "",
        _md_table(["空间", "值", "标签", "来源", "置信度"], [
            [e.space, e.value, e.label, e.source, e.confidence]
            for e in idspaces.id_entries()
        ]),
        "",
        "## 3. 关键关系",
        "",
        "- `viewjob.csv`：`(career, advance, mount_tier, gender)` → 衣服/手套/鞋子/武器/坐骑/骑甲/星神造型。",
        "- `bodyidequal.csv`：怪物/对等身体 ID → 身体/头/衣服/武器/鞋/面具/护腕 + 颜色 + 武器光效。",
        "- `itemSpecData.csv`：物品 ID → 槽位 + 17 职业权限 + 图标 + 阶段/等级。",
        "- `mouthairid.csv`：`(mount_id, body_shape, flying, race)` → 头发服务器/资源 ID。",
        "- `horseinfo.csv`：坐骑 ID → 怪物 ID、类型、品级、技能、升级链。",
        "- `Skill.csv`：技能 ID → 职业/系/阶位/图标/目标数/光效/攻击帧/技能树坐标/前置。",
        "- `specialSuit.csv`：套装 → 面具 + 召唤兽 + 坐骑。",
        "- `MonsterEquipSuits.csv` / `SummonerEquipRecipe.csv`：召唤兽装备。",
        "",
        "## 4. 使用状态",
        "",
        "| 表 | 查看器 | 原生客户端 | 引擎消费点 |",
        "|---|---|---|---|",
        "| resmirror | 未用 | 未用 | 待定位 |",
        "| viewjob | 只用前 11 列 | 未用 | 待定位 |",
        "| bodyidequal | 只用部件 ID，忽略颜色/武器光效 | 未用 | 待定位 |",
        "| mouthairid / faeriewing / cloakoffset | 未用 | 未用 | 待定位 |",
        "| horseoffset / hpoffset | 未用 | 未用 | 待定位 |",
        "| Skill | 只用 7 列 | 未用 | 待定位 |",
        "| itemSpecData | 只用 ID/图标/名称/等级/描述/槽位/职业 | 未用 | 待定位 |",
        "",
        "## 5. 未知",
        "",
        "- 每张表的具体加载函数、加载顺序、是否热重载。",
        "- `allstruct.csv` 与网络消息的字段对应关系。",
        "- `msg.csv` 的节流语义。",
        "- Lua/SCP 索引已建立（`lua-scp-usage.md`），但未发现纸娃娃表被 Lua 直接读取；",
        "  表的消费点更可能在 C++/SCP 消息层，需 Ghidra 定点分析。",
        "",
    ])


def asset_spec_md(index: InventoryIndex) -> str:
    rows = []
    for key, info in sorted(index.parts.items()):
        if key == "shadow":
            continue
        rows.append([
            key,
            "`<part>/<model>/<action>/<direction>.apg`",
            len(info.models),
            ",".join(str(a) for a in sorted(info.actions)),
            "`perinfo.dat`（可选）；AI 帧需 `clrtb.cti`",
        ])
    coverage: dict[str, list[int]] = {}
    for row in index.perinfo_coverage:
        agg = coverage.setdefault(row["part"], [0, 0, 0])
        agg[0] += row["action_dirs"]
        agg[1] += row["with_perinfo"]
        agg[2] += row["missing"]
    coverage_rows = [
        [part, a, b, c,
         f"{b / a:.0%}" if a else "—"]
        for part, (a, b, c) in sorted(coverage.items())
    ]
    return "\n".join([
        "# 角色资源规格（player*.pak）",
        "",
        "## 1. 路径形状",
        "",
        "- 玩家部件：`player*.pak:<part>/<model>/<action>/<direction>.apg`",
        "- 参数文件：动作目录 `perinfo.dat`（PIF：锚点/偏移/层叠键/延时）；**可选**，缺失时查看器回退 `(160,210)/80ms`，引擎行为待确认。",
        "- 调色板：同目录或向上 `clrtb.cti`（AI 索引帧需要）。",
        "- NPC：`npc*.pak:<model>/<action>/<direction>.apg`（无部位段）。",
        "- 魔法：`magic*.pak:<model>/<variant>/front.apg|back.apg`。",
        "",
        "## 2. 部件统计",
        "",
        _md_table(["part", "路径模板", "模型数", "动作 ID", "伴随文件"], rows),
        "",
        "## 3. perinfo.dat 覆盖率（按动作目录）",
        "",
        _md_table(["part", "动作目录", "有 perinfo", "缺失", "覆盖率"], coverage_rows),
        "",
        "> 覆盖率低不等于资源损坏：多数部件依赖默认锚点/延时；只有需要逐动作锚点的部件才配置 perinfo.dat。",
        "",
        "## 4. 动作语义（resmirror）",
        "",
        _md_table(["Action", "含义"], [
            [k, v] for k, v in sorted(idspaces.ACTION_GROUPS.items())
        ] + [[k, v] for k, v in sorted(idspaces.PSEUDO_ACTIONS.items())]),
        "",
        "> 资源中另见 `401`、`601`（weapon 专用）；resmirror 未给出语义，列入未决问题。",
        "",
        "## 5. 新增一个部件/模型的最小清单",
        "",
        "1. `player*.pak:<part>/<model>/<action>/<direction>.apg`（至少 8 方向；动作按部件能力）。",
        "2. 动作目录 `perinfo.dat`（可选；需要非默认锚点/层叠键/延时时配置）；AI 索引帧需要 `clrtb.cti`。",
        "3. 在 `resmirror.csv` 增加镜像/转换规则（如需要）。",
        "4. 在 `bodyidequal.csv` 或 `viewjob.csv` 建立 ID 关系。",
        "5. 在 `itemSpecData.csv` 建立物品/槽位/职业权限（装备类）。",
        "6. 图标：`ui.pak:icon/small|big/<icon>.a16`。",
        "",
        "## 6. 未知",
        "",
        "- `effect` / `shadow` 的消费方式。",
        "- part type 16 裤子、8 武器光效的资源目录。",
        "- 发型/翅膀/披风的资源路径（表只给 ID）。",
        "- action 401/601 的语义与触发条件。",
        "",
    ])


def new_career_md(coverage: dict) -> str:
    missing = coverage.get("missing_by_career_field", [])
    status_label = {"missing": "真缺失", "id_space_mismatch": "ID 空间不匹配"}
    missing_rows = [[m["career"], m["career_name"], m["field"],
                     status_label.get(m.get("status", ""), m.get("status", "")),
                     m["count"]]
                    for m in missing[:60]]
    status_counts = coverage.get("status_counts", {})
    status_rows = [[k, v] for k, v in sorted(status_counts.items())]
    return "\n".join([
        "# 在原 17 职业框架内扩展：清单",
        "",
        "> 范围：不新增第 18 个职业 ID；只修改/补全现有 `viewjob` 预设与资源。",
        "",
        "## 1. 职业与外观",
        "",
        "1. `config.pak:ui/login/viewjob.csv` 增加/修改一行：",
        "   `career, advance, mount_tier, gender, cloth, gloves, shoes, weapon, horse, harmor_head, harmor_body, hero_branch, stargod_model`。",
        "2. 资源 ID 必须真实存在；用 `python tools/art_pipeline/cli.py careers` 检查覆盖。",
        "3. 颜色：`bodyidequal.csv`（头/衣/武器/鞋/面具/护腕 + 武器光效）。",
        "",
        "## 2. 美术资源",
        "",
        "| 类别 | 必需资源 |",
        "|---|---|",
        "| 角色 | body/head/cloth/shoe/mask/Lcuff/Rcuff/weapon + 动作/朝向 + perinfo.dat（可选）+ clrtb.cti（AI 帧） |",
        "| 武器光效 | part type 8 资源（目录待确认） |",
        "| 坐骑 | horse/harmorbody/harmorhead + 动作 10/30 + horseoffset/colorconvert |",
        "| 发型 | mouthairid.csv 的头发服务器/资源 ID |",
        "| 翅膀 | faeriewing.csv 的翅膀资源 ID |",
        "| 披风 | cloakoffset.csv 的披风 ID + 动作/方向偏移 |",
        "| 星神 | viewjob 星神造型 ID（已确认指向 npc*.pak 模型）+ Specialty + horseoffset 星将偏移 |",
        "",
        "## 3. 数据表",
        "",
        "1. `itemSpecData.csv`：物品 ID、图标、17 职业权限、槽位、阶段/等级。",
        "2. `Skill.csv`：职业/系/阶位、图标、目标数、三个光效、攻击帧、技能树坐标、前置技能、费用。",
        "3. `horseinfo.csv` / `horseskill.csv`：坐骑类型/品级/技能/升级链。",
        "4. `petinfo.csv` / `petequipsetname.csv` / `MonsterEquipSuits.csv` / `SummonerEquipRecipe.csv`：宠物/召唤兽。",
        "5. `specialSuit.csv`：面具 + 召唤兽 + 坐骑套装。",
        "",
        "## 4. UI / Lua",
        "",
        "- 图标：`ui.pak:icon/small|big`、`ui.pak:user/head`。",
        "- 窗口：`config.pak` / `ui.pak` 的 SCP；`MESSAGE ... CODE \"Lua函数\"`。",
        "- Lua：`config.pak:lua/*.lua` + SCP `#script`；`LuaWindowMap.csv`。",
        "",
        "## 5. 当前覆盖缺口（自动统计）",
        "",
        "> “ID 空间不匹配”表示该 ID 在 `player*.pak` 的**其他部件**中存在，",
        "> 不一定是美术缺失；需要按引擎实际部件语义修正 `viewjob` 列或确认跨部件复用。",
        "",
        _md_table(["状态", "数量"], status_rows),
        "",
        _md_table(["career", "职业", "字段", "状态", "数量"], missing_rows) if missing_rows
        else "当前 `viewjob` 预设未发现资源缺失。",
        "",
        "## 6. 验证",
        "",
        "```bash",
        "python tools/art_pipeline/cli.py all --shdata D:/software/dhsh/shdata",
        "# 查看 SQLite：",
        "#   viewjob_presets / career_coverage / resource_models",
        "```",
        "",
        "## 7. 边界",
        "",
        "- 现有表的职业标志固定 17 列；新增第 18 个职业需要确认引擎数组上限。",
        "- 动作/部件枚举若在 C++ 中硬编码，新增类别需要引擎改动。",
        "",
    ])


def open_questions_md() -> str:
    return "\n".join([
        "# 未决问题（按优先级）",
        "",
        "## P0：决定能否扩展",
        "",
        "1. **已确认**：17 职业由 `viewjob` 加载器 `0x523790`（1.50.22）/`0x5238b0`（1.50.27）的 `career < 0x11` 检查硬限制；第 18 个职业必须改 C++/原生客户端。",
        "2. **部分确认**：`resmirror` 加载器接受 part type `< 0x15`（0–20）、direction `< 9`；action 未看到硬上限。",
        "3. **部分确认**：`viewjob` 查找已定位 `0x520550` / `0x5205c0`；`+0x1a..+0x32` 部件字段的读取点仍待确认。",
        "",
        "## P1：类别与资源",
        "",
        "4. part type 16 裤子、8 武器光效的资源目录与消费函数。",
        "5. `effect` 与 part type 4 魔法/特效的对应关系。",
        "6. 发型（mouthairid 资源 ID）、翅膀（faeriewing）、披风的实际资源路径。",
        "7. 星神造型 ID 的解析：是 body/head 模型，还是独立资源？",
        "8. weapon 动作 `401`/`601` 的语义与触发条件。",
        "9. perinfo.dat 覆盖率低（head/body/weapon 大量缺失）：默认锚点/延时是否就是引擎实际行为？",
        "",
        "## P2：挂点与技能",
        "",
        "10. `horseoffset` / `cloakoffset` / APCI 层叠键的优先级。",
        "11. `Skill.csv` 攻击帧如何驱动光效生成与挂点。",
        "12. 坐骑/骑甲/人物底心对齐的引擎公式。",
        "",
        "## P3：交互与通信",
        "",
        "13. SCP `MESSAGE` → C++ 分发函数；Lua 回调子集已索引，剩余走内置/C++ 回调。",
        "14. `FSLuaSystem` / `FSLuaWndScript` / `FSLuaHorseMng` / `FSLuaSummonMng` / `FSLuaTUserHorseInfo` 的 C++ 方法语义与表读取关系。",
        "15. `allstruct.csv` 与网络消息字段的对应；外观/技能/坐骑谁是服务器权威。",
        "16. `msg.csv` 的节流规则。",
        "",
        "## P4：数据质量",
        "",
        "17. CSV 中英文逗号、引号、BOM、空列的统一处理。",
        "18. `itemSpecData` 槽位 → 部件 → 模型公式的完整验证。",
        "19. 大小写目录（Lcuff/lcuff、Rcuff/rcuff）是否为新旧资源并存。",
        "",
    ])


def lua_scp_md(scp: ScpIndex, lua: LuaIndex) -> str:
    summary = lua.summary()
    scp_summary = scp.summary()
    api_rows = [[k, v] for k, v in list(summary["apis"].items())[:25]]
    install_rows = [[k, v] for k, v in list(summary.get("installs", {}).items())[:40]]
    window_rows = [[k, v] for k, v in list(summary["windows"].items())[:25]]
    call_rows = [[k, v] for k, v in list(summary["callsc"].items())[:25]]
    msg_rows = [[k, v] for k, v in list(summary["messages"].items())[:30]]
    scp_msg_counts: Counter = Counter(m.message for m in scp.messages)
    scp_code_counts: Counter = Counter(m.code for m in scp.messages)
    lua_reg = {name for _unit, api, name in lua.messages if api == "RegMsgCall"}
    matched = {code for code in scp_code_counts if code in lua_reg}
    relevant = [f for f in scp.files
                if any(k in f.path.lower() for k in
                       ("user", "property", "horse", "pet", "stargod", "equip", "login"))]
    relevant_msgs = [m for m in scp.messages
                     if any(k in m.scp_key.lower() for k in
                            ("user", "property", "horse", "pet", "stargod", "equip", "login"))]
    sample = ""
    for unit in lua.units:
        if "RegMsgCall" in unit.text:
            sample = "\n".join(unit.text.splitlines()[:35])
            break
    return "\n".join([
        "# Lua / SCP 交互索引",
        "",
        "> 只做静态提取，不执行 Lua；来源为 46 个 `.lua` 与 1,940 个 SCP 的 `#script`。",
        "",
        "## 1. 规模",
        "",
        _md_table(["项", "数量"], [
            ["SCP 文件", scp_summary["files"]],
            ["SCP 窗口/控件", scp_summary["windows"]],
            ["SCP MESSAGE 绑定", scp_summary["messages"]],
            ["SCP #script 块", scp_summary["scripts"]],
            ["Lua 单元（.lua + #script）", summary["units"]],
            ["其中 .lua", summary["lua_units"]],
            ["其中 #script", summary["scp_units"]],
        ]),
        "",
        "## 2. FSLua API 使用频次",
        "",
        _md_table(["API", "次数"], api_rows),
        "",
        "## 3. FSLuaInstall 模块",
        "",
        _md_table(["模块", "次数"], install_rows),
        "",
        "## 4. 窗口名（GetRootWindow/SearchRootWindow/CreateRoot*）",
        "",
        _md_table(["窗口名", "次数"], window_rows),
        "",
        "## 5. CallScProcess",
        "",
        _md_table(["进程名", "次数"], call_rows),
        "",
        "## 6. RegMsgCall / RegFunction 名称",
        "",
        _md_table(["名称", "次数"], msg_rows),
        "",
        "## 7. SCP MESSAGE 名称（Top 20）",
        "",
        _md_table(["MESSAGE", "次数"], [[k, v] for k, v in scp_msg_counts.most_common(20)]),
        "",
        "## 8. SCP CODE 与 Lua 回调的交集",
        "",
        _md_table(["指标", "数量"], [
            ["SCP 不同 CODE 名", len(scp_code_counts)],
            ["Lua RegMsgCall 回调名", len(lua_reg)],
            ["两者交集", len(matched)],
            ["只在 SCP 中（C++/内置回调）", len(scp_code_counts) - len(matched)],
        ]),
        "",
        "交集示例：" + ("、".join(sorted(matched)[:20]) if matched else "无"),
        "",
        "## 9. 纸娃娃相关 SCP 文件",
        "",
        _md_table(["指标", "数量"], [
            ["相关 SCP 文件（user/property/horse/pet/stargod/equip/login）", len(relevant)],
            ["相关 MESSAGE 绑定", len(relevant_msgs)],
        ]),
        "",
        "## 10. 观察",
        "",
        "- Lua 主要承担 UI 行为：窗口查找、`CallScProcess`、消息/生命周期回调；",
        "  未发现直接读取 `viewjob`/`resmirror`/`mouthairid` 等纸娃娃表的 Lua 代码。",
        "- `FSLuaInstall` 注册了 `FSLuaHorseMng`、`FSLuaSummonMng`、`FSLuaTUserHorseInfo` 等宿主模块，",
        "  坐骑/召唤兽的运行时逻辑很可能在这些 C++ 模块里，而不是 Lua 表读取。",
        "- 纸娃娃的部件/坐骑/技能装配更可能由 C++/SCP 消息层消费表；Lua 只是 UI 触发。",
        "- `RegMsgCall(\"函数名\")` 的参数是**回调函数名**，不是消息名；消息名在 SCP 的 `MESSAGE` 绑定里。",
        "- 大量 SCP `CODE` 不在 Lua 回调集合中，说明它们走 C++/内置消息处理，下一步要用 Ghidra 定位分发函数。",
        "",
        "## 11. 示例脚本",
        "",
        "```lua",
        sample,
        "```",
        "",
        "## 12. 未知",
        "",
        "- `FSLuaSystem` / `FSLuaWndScript` 各方法在 C++ 中的完整语义。",
        "- `FSLuaHorseMng` / `FSLuaSummonMng` 等模块的方法与表读取关系。",
        "- SCP `MESSAGE` 名称 → 消息 ID → C++ 分发函数 → Lua 回调的完整链路。",
        "- 纸娃娃表在 C++ 中的消费函数（下一步用 Ghidra 定位）。",
        "",
    ])


def engine_md() -> str:
    f = engine_findings
    loader_rows = [[x["table"], f"`{x['loader']}`", f"`{x.get('loader_0dbc', '')}`",
                    x["struct_size"], x["notes"]]
                   for x in f.TABLE_LOADERS]
    mirror_rows = [[f"`{addr}`", desc] for addr, desc in f.MIRROR_CONSUMERS]
    mirror_rows_0dbc = [[f"`{addr}`", desc] for addr, desc in f.MIRROR_CONSUMERS_0DBC]
    host_rows = []
    for host in f.LUA_HOSTS:
        for method in host.get("methods", []):
            host_rows.append([host["module"], f"`{host['class_init']}`",
                              f"`{f.LUA_HOST_REG_0DBC.get(host['module'], '')}`",
                              method[0], f"`{method[1]}`", method[2]])
    field_rows = []
    for host in f.LUA_HOSTS:
        for field in host.get("fields", []):
            field_rows.append([host["module"], field[0], field[1], field[2]])
    limit_rows = [[name, detail] for name, detail in f.HARD_LIMITS]
    viewjob_rows = [[x[0], x[1], x[2]] for x in f.VIEWJOB_STRUCT_0DBC]
    lookup_rows = [[f"`{x[0]}`", x[1]] for x in f.VIEWJOB_LOOKUPS_0DBC]
    skill = f.SKILL_LAYOUT_0DBC
    skill_rows = [
        ["loader", f"`{skill['loader']}`"],
        ["row parser", f"`{skill['row_parser']}`"],
        ["row size", skill["row_size"]],
        ["manager fields", skill["manager_fields"]],
        ["exists", skill["exists"]],
        ["notes", skill["notes"]],
    ]
    return "\n".join([
        "# 引擎消费函数定点分析",
        "",
        f"- 旧基线 SHA-256：`{f.BINARY_SHA256}`（1.50.22）",
        f"- 新基线 SHA-256：`{f.BINARY_SHA256_0DBC}`（{f.VERSION_0DBC}）",
        f"- Ghidra 项目：`{f.GHIDRA_PROJECT}`（旧基线）",
        "- 地址为 VA；只读静态分析，不运行游戏",
        "",
        "## 1. 表 → 加载函数",
        "",
        _md_table(["表", "旧基线 loader", "新基线 loader", "结构/大小", "观察"], loader_rows),
        "",
        "### 1.1 `viewjob` 运行时结构（0dbc）",
        "",
        _md_table(["偏移", "字段", "观察"], viewjob_rows),
        "",
        "### 1.2 `viewjob` 查找函数（0dbc）",
        "",
        _md_table(["地址", "作用"], lookup_rows),
        "",
        "### 1.3 Skill 主管理器（0dbc）",
        "",
        _md_table(["项", "值"], skill_rows),
        "",
        "## 2. `FSBodyResMirror` 消费者（旧基线）",
        "",
        _md_table(["地址", "作用"], mirror_rows),
        "",
        "### 2.1 新基线消费者（0dbc）",
        "",
        _md_table(["地址", "作用"], mirror_rows_0dbc),
        "",
        "## 3. Lua 宿主模块与方法",
        "",
        _md_table(["模块", "旧类初始化", "新注册/方法表", "方法", "旧地址", "说明"], host_rows),
        "",
        "### 3.1 `FSLuaTUserHorseInfo` 字段",
        "",
        _md_table(["模块", "偏移", "字段", "说明"], field_rows),
        "",
        "## 4. 硬上限与枚举",
        "",
        _md_table(["项", "观察"], limit_rows),
        "",
        "> 0dbc 复核：`viewjob` 加载器 `0x5238b0` 仍为 `cmp ebx, 0x11`；"
        "`resmirror` 加载器 `0x8a7180` 仍有 `cmp ebx, 9` 方向检查。",
        "",
        "## 5. 对扩展的直接结论",
        "",
        "1. **第 18 个职业不能只靠改表**：`viewjob.csv` 加载器 `0x523790`（旧）/`0x5238b0`（新）"
        "检查 `career < 0x11`；超出 0–16 的行会被直接丢弃。要新增职业必须改 C++ 或原生客户端。",
        "2. **部件类型有扩展余量**：`resmirror` 加载器接受 part type `< 0x15`（0–20），"
        "但当前表只定义 0–16；17–20 的语义和资源目录仍未知，可作为后续探测方向。",
        "3. **方向固定 0–8**：`resmirror` 加载器要求 direction `< 9`。",
        "4. **`GetHorseHead` / `GetSummonerHead` 是 UI 头像**：分别走 "
        "`ui/horse/head/%d/%d.a16` 和 `FSSummonMng` 头像查找，不是 3D 坐骑/召唤兽装配。",
        "5. **坐骑/披风/星将偏移是独立表**：`horseoffset` 0x2c 结构包含人物/阴影/宠物/"
        "翅膀/披风/星将/头部/脚底光环/脚底特效/中心特效偏移；`cloakoffset` 按"
        "身体图 + 动作 + 方向存偏移。",
        "6. **`viewjob` 每行复制两份性别**：加载器把同一行插入 gender=0 和 gender=1；"
        "性别是查找键的一部分。",
        "7. **1.50.27 的纸娃娃数据未变**：`viewjob`/`resmirror`/`bodyidequal`/`horseoffset` 等"
        "表与旧版一致；变化集中在 `item_spec`（+7 行）与少量 PAK 资源。",
        "",
        "## 6. 未决",
        "",
        "- `viewjob` 的运行时查找函数与 0x3c 结构各偏移的精确消费点。",
        "- `FUN_0082ba00`（旧）/新主 Skill 管理器的完整技能结构与攻击帧消费。",
        "- part type 17–20 的消费者与资源目录。",
        "- `faeriewing.csv` 的资源加载段（0x6de530 主要是 UI 绑定）。",
        "- `specialSuit.csv` 未在二进制中找到直接字符串引用，可能由 Lua/SCP 或动态路径加载。",
        "- 0dbc 的 Lua 宿主方法实现地址、FSBodyResMirror 消费者角色映射仍待逐个确认。",
        "",
    ])


def build_reports(tables: dict[str, TableText], index: InventoryIndex,
                  coverage: dict, docs_dir: Path = DEFAULT_DOCS,
                  scp: ScpIndex | None = None,
                  lua: LuaIndex | None = None) -> None:
    docs_dir.mkdir(parents=True, exist_ok=True)
    (docs_dir / "taxonomy.md").write_text(taxonomy_md(tables, index), encoding="utf-8")
    (docs_dir / "tables.md").write_text(tables_md(tables), encoding="utf-8")
    (docs_dir / "asset-spec.md").write_text(asset_spec_md(index), encoding="utf-8")
    (docs_dir / "new-career.md").write_text(new_career_md(coverage), encoding="utf-8")
    (docs_dir / "open-questions.md").write_text(open_questions_md(), encoding="utf-8")
    (docs_dir / "engine-consumers.md").write_text(engine_md(), encoding="utf-8")
    if scp is not None and lua is not None:
        (docs_dir / "lua-scp-usage.md").write_text(lua_scp_md(scp, lua), encoding="utf-8")
