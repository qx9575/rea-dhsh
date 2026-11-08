# 角色资源规格（player*.pak）

## 1. 路径形状

- 玩家部件：`player*.pak:<part>/<model>/<action>/<direction>.apg`
- 参数文件：动作目录 `perinfo.dat`（PIF：锚点/偏移/层叠键/延时）；**可选**，缺失时查看器回退 `(160,210)/80ms`，引擎行为待确认。
- 调色板：同目录或向上 `clrtb.cti`（AI 索引帧需要）。
- NPC：`npc*.pak:<model>/<action>/<direction>.apg`（无部位段）。
- 魔法：`magic*.pak:<model>/<variant>/front.apg|back.apg`。

## 2. 部件统计

| part | 路径模板 | 模型数 | 动作 ID | 伴随文件 |
|---|---|---|---|---|
| body | `<part>/<model>/<action>/<direction>.apg` | 1102 | 10,20,30,40,41,42,43,44,45,46,50,60,70,80,90,91,92,100,101 | `perinfo.dat`（可选）；AI 帧需 `clrtb.cti` |
| cloth | `<part>/<model>/<action>/<direction>.apg` | 452 | 10,20,30,40,41,42,43,44,45,46,50,60,70,80,90,91,92,100,101 | `perinfo.dat`（可选）；AI 帧需 `clrtb.cti` |
| effect | `<part>/<model>/<action>/<direction>.apg` | 98 | 10,20,30,40,41,42,43,44,45,46,50,60,70,80,90,91,92,101 | `perinfo.dat`（可选）；AI 帧需 `clrtb.cti` |
| harmorbody | `<part>/<model>/<action>/<direction>.apg` | 736 | 10,30 | `perinfo.dat`（可选）；AI 帧需 `clrtb.cti` |
| harmorhead | `<part>/<model>/<action>/<direction>.apg` | 687 | 10,30 | `perinfo.dat`（可选）；AI 帧需 `clrtb.cti` |
| head | `<part>/<model>/<action>/<direction>.apg` | 6820 | 10,20,30,40,41,42,43,44,45,46,50,60,70,80,90,91,92,100,101 | `perinfo.dat`（可选）；AI 帧需 `clrtb.cti` |
| horse | `<part>/<model>/<action>/<direction>.apg` | 722 | 10,30 | `perinfo.dat`（可选）；AI 帧需 `clrtb.cti` |
| lcuff | `<part>/<model>/<action>/<direction>.apg` | 436 | 10,20,30,40,41,42,43,44,45,46,50,60,70,80,90,91,92,100,101 | `perinfo.dat`（可选）；AI 帧需 `clrtb.cti` |
| mask | `<part>/<model>/<action>/<direction>.apg` | 6291 | 10,20,30,40,41,42,43,44,45,46,50,60,70,80,90,91,92,100,101 | `perinfo.dat`（可选）；AI 帧需 `clrtb.cti` |
| pet | `<part>/<model>/<action>/<direction>.apg` | 441 | 0,10,20,30,40,50,60,70,80,90,100,101 | `perinfo.dat`（可选）；AI 帧需 `clrtb.cti` |
| rcuff | `<part>/<model>/<action>/<direction>.apg` | 424 | 10,20,30,40,41,42,43,44,45,46,50,60,70,80,90,91,92,100,101 | `perinfo.dat`（可选）；AI 帧需 `clrtb.cti` |
| shoe | `<part>/<model>/<action>/<direction>.apg` | 424 | 10,20,30,40,41,42,43,44,45,46,50,60,70,80,90,91,92,100,101 | `perinfo.dat`（可选）；AI 帧需 `clrtb.cti` |
| weapon | `<part>/<model>/<action>/<direction>.apg` | 610 | 10,20,30,40,41,42,43,44,45,46,50,60,70,80,90,91,92,100,101,401,601 | `perinfo.dat`（可选）；AI 帧需 `clrtb.cti` |

## 3. perinfo.dat 覆盖率（按动作目录）

| part | 动作目录 | 有 perinfo | 缺失 | 覆盖率 |
|---|---|---|---|---|
| body | 3713 | 31 | 3682 | 1% |
| cloth | 5220 | 26 | 5194 | 0% |
| effect | 942 | 5 | 937 | 1% |
| harmorbody | 1472 | 182 | 1290 | 12% |
| harmorhead | 1372 | 176 | 1196 | 13% |
| head | 20042 | 1488 | 18554 | 7% |
| horse | 1468 | 206 | 1262 | 14% |
| lcuff | 5176 | 26 | 5150 | 1% |
| mask | 20074 | 1322 | 18752 | 7% |
| pet | 1339 | 49 | 1290 | 4% |
| rcuff | 4972 | 11 | 4961 | 0% |
| shoe | 4968 | 10 | 4958 | 0% |
| weapon | 6950 | 0 | 6950 | 0% |

> 覆盖率低不等于资源损坏：多数部件依赖默认锚点/延时；只有需要逐动作锚点的部件才配置 perinfo.dat。

## 4. 动作语义（resmirror）

| Action | 含义 |
|---|---|
| 10 | 待机 |
| 20 | 战斗待机 |
| 30 | 跑 |
| 40 | 物理攻击 |
| 50 | 防御 |
| 60 | 受击 |
| 70 | 死亡 |
| 80 | 冲刺 |
| 90 | 魔法攻击 |
| 100 | 肢体语言 |
| -1 | 魔法/宠物（忽略朝向与转换） |
| 10000 | 10 与 30 |
| 10001 | 10–90 |
| 10002 | 10/20/30/50/60/70/80 |
| 10003 | 10002 + 91/92 |
| 10004 | 10–100 + 91/101 |

> 资源中另见 `401`、`601`（weapon 专用）；resmirror 未给出语义，列入未决问题。

## 5. 新增一个部件/模型的最小清单

1. `player*.pak:<part>/<model>/<action>/<direction>.apg`（至少 8 方向；动作按部件能力）。
2. 动作目录 `perinfo.dat`（可选；需要非默认锚点/层叠键/延时时配置）；AI 索引帧需要 `clrtb.cti`。
3. 在 `resmirror.csv` 增加镜像/转换规则（如需要）。
4. 在 `bodyidequal.csv` 或 `viewjob.csv` 建立 ID 关系。
5. 在 `itemSpecData.csv` 建立物品/槽位/职业权限（装备类）。
6. 图标：`ui.pak:icon/small|big/<icon>.a16`。

## 6. 未知

- `effect` / `shadow` 的消费方式。
- part type 16 裤子、8 武器光效的资源目录。
- 发型/翅膀/披风的资源路径（表只给 ID）。
- action 401/601 的语义与触发条件。
