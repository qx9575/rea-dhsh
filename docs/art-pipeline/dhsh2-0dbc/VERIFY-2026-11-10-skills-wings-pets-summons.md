# 技能 / 翅膀 / 宠物 / 召唤兽验证（2026-11-10）

## 结论

| 模块 | 状态 | 说明 |
|---|---|---|
| 技能光效 | ✅ 基本闭合 | 464 个光效 ID，462 个在 magic*.pak 中解析；缺 18000、19039 |
| 翅膀 | 🟡 资源已验证 | `faeriewing.csv` 翅膀 ID 52/58/59 → `pet/<id>/0/<dir>.apg`（action 0） |
| 宠物 | 🟡 资源已验证 | `pet/<id>/<action>/<dir>.apg`，441 个模型；部分 frame0 是影子帧 |
| 召唤兽 | 🟡 资源已验证，归属待定 | `monsters.csv` shape → `npc*.pak:<shape>/10/<dir>.apg`；需区分召唤兽与普通怪物 |

## 1. 技能

- 数据：`Skill.csv`（`config.pak:magic/Skill.csv`）
- 光效列：受击光效（第 8 列）、攻击光效（第 9 列）、失败光效（第 10 列）
- 解析：`magic*.pak:<effect_id>/<variant>/front.apg|back.apg`
- 覆盖：`effect_coverage()` → `effect_ids=464, resolved=462, missing=[18000,19039]`
- 样例：

```text
5  袭心诀  职业1  18104 → magic.pak variants[0] front.apg
45 流星    职业2  18201 → magic.pak variants[3,7] front/back.apg
103 法力吸收 职业5 103  → magic.pak variants[0] front.apg
```

- 待做：技能光效的 front/back 合成、职业过滤展示、按技能 ID 批量导出。

## 2. 翅膀（仙羽）

`faeriewing.csv` 的“翅膀”列是资源 ID；已验证：

```text
翅膀 ID 52 → player.pak:pet/52/0/<dir>.apg
翅膀 ID 58 → player4.pak:pet/58/0/<dir>.apg
翅膀 ID 59 → player4.pak:pet/59/0/<dir>.apg
```

- 动作目录是 **0**，不是普通宠物的 10/30；
- CTI：`pet/<id>/clrtb.cti`；
- 渲染样例：`extracted/paperdoll/pet_wings/wings_52_58_59.png`
- paperdoll 已支持 `--wing ID`（2026-11-10）：解析为 `pet/<id>/0/<dir>.apg`，
  绘制层序 `wing = -1`（人物后方），`manifest.wing` 输出资源信息。
- 待做：把翅膀作为独立层接进 paperdoll（应绘制在 body 之后/之前待客户端确认），
  并读取 `faeriewing` 等级/翅膀 ID。

## 3. 宠物

资源：`player*.pak:pet/<id>/<action>/<dir>.apg`，共 441 个模型。

已抽样：

```text
pet/1   action 0  player.pak   蓝色球状
pet/9   action 0  player.pak   黄色环状
pet/30  action 0  player.pak   橙色宠物
pet/94  action 10 player1.pak  正常
pet/200 action 10 player1.pak  正常
pet/210 action 10 player1.pak  正常
pet/52/58/59 → 翅膀（见上）
```

渲染样例：`extracted/paperdoll/pet_wings/pets_sample2.png`

paperdoll 已支持 `--pet ID` / `--pet-action` / `--pet-frame`（2026-11-10）：
动作默认 10→0，层序 `pet=-2`；`--pet-frame 1` 可跳过影子帧。

问题：

- `pet/12101`、`pet/12350` 的 frame0 是黑色影子帧；资源为 AI 帧，
  需选非影子帧或确认 palette（`pet/<id>/clrtb.cti` 存在）。
- `petinfo.csv` 在当前提取中是占位名称（名称0/名称1），宠物名需要从
  `shapelist.scp` 或运行时内存补。

## 4. 召唤兽 / 怪物

- `monsters.csv` 共 434 个非零 shape；
- 模型：`npc*.pak:<shape>/10/<dir>.apg`（部分动作/朝向不同）；
- 已抽样并渲染：

```text
10030 npc.pak   10045 npc.pak   10104 npc1.pak
20000 npc.pak   20005 npc.pak   20036 npc.pak
20050 npc.pak   20101 npc.pak
```

样例：`extracted/paperdoll/pet_wings/summons_sample.png`

已确认的召唤兽 shape（`manifests/relations/summon-shapes.csv`，全量）：

- 429 个 `monsters.csv` shape（402 个在 npc*.pak 有模型）；
- 分类：`pet`（水水/浒浒/穷奇/…）、`mini`（迷你五兽）、`moxing`（魔星）、
  `monster`（野外怪物/NPC 等）；
- 示例：控偶师 `26317`、魔星·吴用 `26308`、水水 `26054`、迷你洪荒巨兽 `26175`；
- 任意 shape 都可用 `--summon SHAPE` 或 `--summon-name NAME` 渲染。

paperdoll 已支持 `--summon` / `--summon-name`（2026-11-10）：解析
`npc*.pak:<shape>/<action>/<dir>.apg`，层序 `summon=-3`；26054/26175/
控偶师/魔星·吴用 已验证。

问题：

- `monsters.csv` 包含普通 NPC/怪物，尚未区分“召唤兽”专属 shape；
  `MonsterEquipSuits.csv`、`summoner_equip_recipe.csv` 是装备/配方数据，
  不能直接当外观表。
- 需要从 `summon` 相关 CSV/SCP 或运行时内存确定召唤兽 shape 列表。

## 5. 产物

```text
manifests/relations/pet-samples.csv
manifests/relations/summon-samples.csv
extracted/paperdoll/pet_wings/wings_52_58_59.png
extracted/paperdoll/pet_wings/pets_sample2.png
extracted/paperdoll/pet_wings/summons_sample.png
```

## 6. 下一步

1. paperdoll 增加 `wing` 层：从 `faeriewing` 读翅膀 ID → `pet/<id>/0/<dir>`。
2. 宠物解析器：`pet/<id>` + 非影子帧选择 + 名称表（shapelist/内存）。
3. 召唤兽 shape 列表：从 `summon` 数据或运行时确认，再接入 `npc*.pak`。
4. 技能光效：front/back 合成与按职业/技能导出。
