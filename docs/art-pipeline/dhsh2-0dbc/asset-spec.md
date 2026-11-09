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

> **大小写目录（已确认）**：`Lcuff` 284 个模型 / `lcuff` 152 个，`Rcuff` 272 / `rcuff` 152，
> 两侧模型集合无交集；115 以上的护手模型只存在于 `Lcuff`/`Rcuff`，10–564 段的低段模型
> 在 `lcuff`/`rcuff`。资源查找大小写不敏感（paperdoll 的精确路径字典按小写匹配），
> 但美术侧新增/导出时应写入真实目录名。

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

### 4.1 resmirror 转换表（已确认，非朝向镜像）

`config.pak:misc/resmirror.csv` 是**资源模型替换表**，不是朝向镜像表。CSV 首行注释给出列义：

```text
部件类型, 部件id(0–65534), 动作id(0–254), 朝向id(0–7),
转换后部件id, 转换后动作id, 转换后朝向id
```

- 查表键 = `(部件类型, 部件 id, 动作, 朝向)`；命中后按“转换后”三元组取资源。
- 6951 行中**所有行的转换后朝向都等于输入朝向**，方向不变；精确行的转换后动作
  也等于输入动作——因此该表只做**部件 id 重定向**（换模型），不翻转/补朝向。
- 动作 `10000–10004` 是动作组别名：按表头注释，这些行忽略朝向与“转换后动作/朝向”，
  只取转换后部件 id，并展开到组内每个具体动作。当前表中没有任何 `(部件类型, 部件 id)`
  同时拥有多个别名动作，展开无冲突。
- 1.50.27 加载器 `0x8a4f40` 与查表函数 `0x8a6490` 的键打包与上表一致：
  `part_type*10 + direction`、`action & 0x7f`、`model & 0xfffff`（另有 model>>16 高位），
  并要求 `part_type <= 0x14`、`action < 0xff`、`direction <= 8`。
- 覆盖范围：部件类型 0/1/2/4/7/9/10/11/12；当前表没有 3 宠物、8 武器光效、13 坐骑、
  14/15 骑甲的记录，这些类型查表即恒等。
- 实例：职业 10 的 `viewjob` 外形 id `cloth/gloves/shoes=1009`（女性 `1059`）
  经别名 `10004` 重定向到资源模型 `309`（`359`），对应 `player*.pak:cloth/309/...`
  等真实目录；未应用该转换时会误报“资源缺失”。

paperdoll 已按此实现（`MirrorIndex`）：先精确匹配，再按 `10000→10004` 顺序匹配别名组；
命中后 `model/action/direction` 三元组进入后续 APG/PIF/CTI/APCI 解析，并在
`manifest.parts[].mirror_rule`（`exact` / `alias:10004`）中留痕。

## 5. 新增一个部件/模型的最小清单

1. `player*.pak:<part>/<model>/<action>/<direction>.apg`（至少 8 方向；动作按部件能力）。
2. 动作目录 `perinfo.dat`（可选；需要非默认锚点/层叠键/延时时配置）；AI 索引帧需要 `clrtb.cti`。
3. 在 `resmirror.csv` 增加外形 id → 资源模型的重定向（如需要）。
4. 在 `bodyidequal.csv` 或 `viewjob.csv` 建立 ID 关系。
5. 在 `itemSpecData.csv` 建立物品/槽位/职业权限（装备类）。
6. 图标：`ui.pak:icon/small|big/<icon>.a16`。

## 6. 未知

- `effect` / `shadow` 的消费方式。
- part type 16 裤子、8 武器光效的资源目录。
- 发型/翅膀/披风的资源路径（表只给 ID）。
- action 401/601 的语义与触发条件。
