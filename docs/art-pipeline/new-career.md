# 在原 17 职业框架内扩展：清单

> 范围：不新增第 18 个职业 ID；只修改/补全现有 `viewjob` 预设与资源。

## 1. 职业与外观

1. `config.pak:ui/login/viewjob.csv` 增加/修改一行：
   `career, advance, mount_tier, gender, cloth, gloves, shoes, weapon, horse, harmor_head, harmor_body, hero_branch, stargod_model`。
2. 资源 ID 必须真实存在；用 `python tools/art_pipeline/cli.py careers` 检查覆盖。
3. 颜色：`bodyidequal.csv`（头/衣/武器/鞋/面具/护腕 + 武器光效）。

## 2. 美术资源

| 类别 | 必需资源 |
|---|---|
| 角色 | body/head/cloth/shoe/mask/Lcuff/Rcuff/weapon + 动作/朝向 + perinfo.dat（可选）+ clrtb.cti（AI 帧） |
| 武器光效 | part type 8 资源（目录待确认） |
| 坐骑 | horse/harmorbody/harmorhead + 动作 10/30 + horseoffset/colorconvert |
| 发型 | mouthairid.csv 的头发服务器/资源 ID |
| 翅膀 | faeriewing.csv 的翅膀资源 ID |
| 披风 | cloakoffset.csv 的披风 ID + 动作/方向偏移 |
| 星神 | viewjob 星神造型 ID（已确认指向 npc*.pak 模型）+ Specialty + horseoffset 星将偏移 |

## 3. 数据表

1. `itemSpecData.csv`：物品 ID、图标、17 职业权限、槽位、阶段/等级。
2. `Skill.csv`：职业/系/阶位、图标、目标数、三个光效、攻击帧、技能树坐标、前置技能、费用。
3. `horseinfo.csv` / `horseskill.csv`：坐骑类型/品级/技能/升级链。
4. `petinfo.csv` / `petequipsetname.csv` / `MonsterEquipSuits.csv` / `SummonerEquipRecipe.csv`：宠物/召唤兽。
5. `specialSuit.csv`：面具 + 召唤兽 + 坐骑套装。

## 4. UI / Lua

- 图标：`ui.pak:icon/small|big`、`ui.pak:user/head`。
- 窗口：`config.pak` / `ui.pak` 的 SCP；`MESSAGE ... CODE "Lua函数"`。
- Lua：`config.pak:lua/*.lua` + SCP `#script`；`LuaWindowMap.csv`。

## 5. 当前覆盖缺口（自动统计）

> “ID 空间不匹配”表示该 ID 在 `player*.pak` 的**其他部件**中存在，
> 不一定是美术缺失；需要按引擎实际部件语义修正 `viewjob` 列或确认跨部件复用。

| 状态 | 数量 |
|---|---|
| found | 1407 |
| found_npc | 64 |
| id_space_mismatch | 169 |
| missing | 262 |
| unconfigured | 338 |

| career | 职业 | 字段 | 状态 | 数量 |
|---|---|---|---|---|
| 1 | 剑客 | harmor_body | ID 空间不匹配 | 6 |
| 2 | 武师 | harmor_body | ID 空间不匹配 | 6 |
| 3 | 医师 | harmor_body | ID 空间不匹配 | 6 |
| 4 | 术士 | harmor_body | ID 空间不匹配 | 6 |
| 5 | 道士 | harmor_body | ID 空间不匹配 | 6 |
| 5 | 道士 | weapon | ID 空间不匹配 | 3 |
| 6 | 浪子 | harmor_body | ID 空间不匹配 | 6 |
| 7 | 弓手 | harmor_body | ID 空间不匹配 | 6 |
| 9 | 墨者 | weapon | ID 空间不匹配 | 4 |
| 10 | 巫煞 | cloth | ID 空间不匹配 | 6 |
| 10 | 巫煞 | gloves | ID 空间不匹配 | 12 |
| 10 | 巫煞 | shoes | ID 空间不匹配 | 6 |
| 10 | 巫煞 | weapon | ID 空间不匹配 | 2 |
| 11 | 魂武 | harmor_body | ID 空间不匹配 | 6 |
| 12 | 寒星 | cloth | ID 空间不匹配 | 2 |
| 12 | 寒星 | gloves | ID 空间不匹配 | 4 |
| 12 | 寒星 | harmor_body | 真缺失 | 6 |
| 12 | 寒星 | harmor_head | 真缺失 | 6 |
| 12 | 寒星 | horse | 真缺失 | 6 |
| 12 | 寒星 | shoes | ID 空间不匹配 | 2 |
| 13 | 花间舞 | harmor_body | 真缺失 | 12 |
| 13 | 花间舞 | harmor_head | 真缺失 | 12 |
| 13 | 花间舞 | horse | 真缺失 | 12 |
| 14 | 剑九 | harmor_body | 真缺失 | 12 |
| 14 | 剑九 | harmor_head | 真缺失 | 12 |
| 14 | 剑九 | horse | 真缺失 | 12 |
| 15 | 魔刀 | cloth | ID 空间不匹配 | 10 |
| 15 | 魔刀 | cloth | 真缺失 | 8 |
| 15 | 魔刀 | gloves | ID 空间不匹配 | 20 |
| 15 | 魔刀 | gloves | 真缺失 | 16 |
| 15 | 魔刀 | harmor_body | 真缺失 | 12 |
| 15 | 魔刀 | harmor_head | 真缺失 | 12 |
| 15 | 魔刀 | horse | 真缺失 | 12 |
| 15 | 魔刀 | shoes | ID 空间不匹配 | 10 |
| 15 | 魔刀 | shoes | 真缺失 | 8 |
| 15 | 魔刀 | weapon | ID 空间不匹配 | 10 |
| 15 | 魔刀 | weapon | 真缺失 | 8 |
| 16 | 百变 | cloth | ID 空间不匹配 | 6 |
| 16 | 百变 | cloth | 真缺失 | 12 |
| 16 | 百变 | gloves | ID 空间不匹配 | 12 |
| 16 | 百变 | gloves | 真缺失 | 24 |
| 16 | 百变 | harmor_body | 真缺失 | 12 |
| 16 | 百变 | harmor_head | 真缺失 | 12 |
| 16 | 百变 | horse | 真缺失 | 12 |
| 16 | 百变 | shoes | ID 空间不匹配 | 6 |
| 16 | 百变 | shoes | 真缺失 | 12 |
| 16 | 百变 | weapon | ID 空间不匹配 | 6 |
| 16 | 百变 | weapon | 真缺失 | 12 |

## 6. 验证

```bash
python tools/art_pipeline/cli.py all --shdata D:/software/dhsh/shdata
# 查看 SQLite：
#   viewjob_presets / career_coverage / resource_models
```

## 7. 边界

- 现有表的职业标志固定 17 列；新增第 18 个职业需要确认引擎数组上限。
- 动作/部件枚举若在 C++ 中硬编码，新增类别需要引擎改动。
