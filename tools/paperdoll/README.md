# paperdoll — 独立纸娃娃工具（P2.2 清单 / 渲染 / 校验）

只读原 PAK，不修改 `kimi_viewer` / `client`。当前阶段输出“职业预设 → 部件资源”清单、
合成帧，并批量校验全部预设，作为美术制作、表配置和原生客户端装配的验证器。

## 用法

```bash
# 新版客户端（1.50.27）
PYTHONIOENCODING=utf-8 python tools/paperdoll/cli.py manifest \
  --shdata D:/software/dhsh2/shdata \
  --career 1 --advance 0 --mount-tier 1 --gender 0 \
  --action 10 --direction 0 \
  --out extracted/paperdoll

# 导出全部 272 条 viewjob 预设（原始字段）
PYTHONIOENCODING=utf-8 python tools/paperdoll/cli.py presets \
  --shdata D:/software/dhsh2/shdata --out extracted/paperdoll

# 渲染（PNG 序列 + GIF）
PYTHONIOENCODING=utf-8 python tools/paperdoll/cli.py render \
  --shdata D:/software/dhsh2/shdata \
  --career 1 --advance 0 --mount-tier 1 --gender 0 \
  --action 10 --direction 0 \
  --out extracted/paperdoll

# 批量校验 272 条预设（272 预设 / 2382 部件；约 1.5 分钟）
PYTHONIOENCODING=utf-8 python tools/paperdoll/cli.py validate \
  --shdata D:/software/dhsh2/shdata --action 10 --direction 0 \
  --out extracted/paperdoll
```

# 批量渲染（动作 × 朝向矩阵；输出 render_batch.json）
PYTHONIOENCODING=utf-8 python tools/paperdoll/cli.py render \
  --shdata D:/software/dhsh2/shdata \
  --career 1 --advance 0 --mount-tier 0 --gender 0 \
  --actions 10,20,30 --directions 0,3,5,7 \
  --out extracted/paperdoll --no-gif
```

## 装备覆盖

`--override PART=MODEL` 直接覆盖部件模型（`mask/cloth/weapon/shoes/gloves/…`）。

`--equip ITEM_ID` 按 `ui.pak:item/itemSpecData.csv` 的物品槽位覆盖：

| 物品槽位 | 部件目录 | paperdoll 字段 | 物品示例 |
|---:|---|---|---|
| 10 帽子 | `head` | `head` | 1010 天罡头盔 |
| 20 衣服 | `cloth` | `cloth` | 2100 轻质藤甲 |
| 40 武器 | `weapon` | `weapon` | 4100 青铜剑 |
| 80 腰带 | `mask` | `mask` | 8000 棉布束腰 |
| 90 鞋子 | `shoe` | `shoes` | 9010 皇帝的新靴 |
| 100 面罩 | `mask` | `mask` | 10100 宝宝嘴（剑） |
| 110 护手 | `lcuff` + `rcuff` | `gloves_l` / `gloves_r` | 11100 精钢护腕 |
| 270 骑甲 | `harmorbody` | `harmor_body` | 27100 绸羊鞍 |

模型公式（沿用 `kimi_viewer.equipment_model`）：`model = item_id - slot*100`。
未识别槽位/物品会在 `manifest.equip[]` 中以 `unknown_slot` / `item_not_found` 记录，
不会静默丢弃；`--override` 的显式值优先于 `--equip`。

> 槽位 → 部件映射来自 `resmirror` part type 与参考查看器，**尚未经原生客户端运行时验证**；
> 渲染结果可用于人工比对（见“已知限制”）。


## 渲染说明

- 层叠顺序：APCI 1B 层叠键升序；key 相同时按部件名排序。
- 锚点/延时：动作目录 `perinfo.dat`（PIF f2/f3/f5）；缺省 `(160,210)/80ms`。
- 坐骑对齐：body 与 horse 的底心对齐；`horse/harmorbody/harmorhead` 应用偏移。
- 资源定位：APG/PIF/CTI 一律按**完整相对路径**精确匹配，不做 basename 回退
  （避免 `0.apg`/`perinfo.dat` 误命中其他模型）。
- 批量渲染：`--actions` / `--directions` 接受逗号分隔列表；每个组合输出独立目录，
  汇总写入 `<out>/render_batch.json`（含 `status=ok/no_frames` 与跳过原因）。
- 画布：640×480，角色参考点对齐画布中心。
- 已验证：测试预设（career=1, advance=0, mount=1, gender=0, action=10, dir=0）
  的 11 帧 RGBA 哈希与 `kimi_viewer.AvatarSource` 逐帧一致。

## 输出字段

`manifest.json`：

| 字段 | 含义 |
|---|---|
| `preset` | career/advance/mount_tier/gender/hero_branch/stargod_model |
| `parts[]` | 每个部件的 `field/part/model/pak/apg/exists/frames/status` |
| `status` | `found` / `found_mirror`（经 resmirror 重定向后命中）/ `found_npc`（神骑/骑甲回退到 `npc*.pak`）/ `missing`（ID 在任一部件空间都不存在）/ `id_space_mismatch`（ID 存在但不在该部件空间，`found_in` 给出空间）/ `apg_missing`（模型存在但动作/朝向 APG 缺失） |
| `resolved_model` / `resolved_action` / `resolved_direction` | resmirror 转换后实际用于取资源的三元组 |
| `mirror_rule` | `""` = 未转换；`exact` = 精确行；`alias:<id>` = 动作组别名（如 `alias:10004`） |
| `actions` / `directions` | 该模型在资源里实际具备的动作/朝向列表 |
| `perinfo` / `anchor` / `delay` | 动作目录 PIF；缺失时为默认/空 |
| `cti` | 同目录或向上找到的 `clrtb.cti` |
| `keys` | APCI 层叠键（逐帧或单值） |
| `stargod` | 星神造型（`npc*.pak` 模型） |
| `mount_attach` | `horseoffset` 中该坐骑模型的挂点偏移（`character_dx/shadow_dx/…`；无记录时为 null） |
| `missing[]` | 资源缺失项 |
| `equip[]` | `--equip` 的逐物品解析结果（`item_id/name/slot/model/part/field/status`） |
| `skills[]` | 该职业在 `Skill.csv` 中的技能与光效 ID；`effects` 给出 `magic*.pak:<model>/<variant>/front.apg|back.apg` 命中情况 |

`validate.json`（`validate` 命令）：

| 字段 | 含义 |
|---|---|
| `presets` / `parts_checked` / `missing` | 扫描范围与未命中总数 |
| `mirror_resolved` | 经 resmirror 重定向后命中的部件数（当前 24） |
| `npc_resolved` | 经 `npc*.pak` 回退命中的坐骑/骑甲部件数（当前 18） |
| `by_field` | 每个部件的 `parts/missing/id_space_mismatch/apg_missing/mirror_resolved/npc_resolved` |
| `by_career` | 每个职业的 `parts/missing` |
| `missing_samples[]` | 最多 500 条未命中明细（含 `status/found_in`） |
| `effects` | `Skill.csv` 光效 ID 在 `magic*.pak` 的覆盖（当前 462/464） |

## 当前覆盖

- 身体/头默认 `200/105`；可用 `--body/--head` 覆盖。
- 衣服/手套/鞋/武器/坐骑/骑甲来自 `viewjob`。
- 星神来自 `viewjob` 第 13 列，解析到 `npc*.pak`。
- 技能来自 `Skill.csv`，按职业过滤。
- 技能光效：`hit/attack/fail_effect` 解析到 `magic*.pak` 模型与变体（464 个光效 ID 中 462 个命中；
  缺 18000、19039）。
- 装备覆盖：`--override PART=MODEL` / `--equip ITEM_ID`。
- `resmirror` 转换：按 `(部件类型, 部件id, 动作, 朝向)` 重定向资源模型，含
  10000–10004 动作组别名；manifest 输出 `resolved_model/mirror_rule`。
- 神骑回退：`horse/harmorbody/harmorhead` 在玩家包缺失时按 NPC 路径
  `npc*.pak:<model>/<action>/<dir>.apg` 查找（如 20000/20050）。
- 批量渲染：动作 × 朝向矩阵；`render_batch.json` 汇总每组合的帧数/跳过原因。
- 坐骑挂点：`manifest.mount_attach` 输出 `horseoffset` 偏移（渲染仍用底心对齐）。
- 批量校验：272 条预设、2382 个部件；当前结果 389 未命中
  = 244 `missing` + 145 `id_space_mismatch` + 0 `apg_missing`，
  另有 24 个部件经 resmirror 重定向后命中（职业 10 外形 1009/1059 → 309/359），
  18 个经 `npc*.pak` 回退命中（职业 12 神骑 20000/20050）。

## 已知限制

- **resmirror 不是朝向镜像**：表中 6951 行的转换后朝向都等于输入朝向；它只做
  部件 id 替换（含动作组别名）。部分动作只存少量朝向（如 body action 20 只有 3/7），
  缺失朝向仍按 `apg_missing` 跳过；引擎是否有别的朝向回退规则尚未确认。
- `rcuff` 的 APCI 键在部分模型为空（表/资源本身可能只登记一侧）。
- 当前客户端缺少新职业部分神骑模型：13–16 职业 mount tier 2 的 `20700/21700/…`
  在全部 PAK 中都找不到（可能未实装/未下载），仍按 `missing` 记录。
- 新职业 15/16 的 `615xx/616xx` 外观 id 与 12 职业的 `6602/6652`
  不在对应部件空间（部分只在 `effect`/`head` 空间），需运行时确认实际渲染方式。
- `--equip` 的槽位 → 部件映射未经原生客户端运行时验证；`item_id - slot*100`
  公式在有同名模型跨部件空间时会歧义（需 resmirror/引擎消费点佐证）。
- 染色（CTI `shapeclrinfo`/`colorconvert`）与坐骑变色尚未接入。
- 星神只定位模型，未合成到帧。
- 技能光效只做资源定位，尚未把 `front.apg/back.apg` 合成到角色帧。
- `mount_attach` 只输出偏移，尚未应用到渲染对齐。

## 下一步

1. 确认新职业（13–16）外观 ID `61500+`（`mouthairid` 身体外形 / `effect` 空间）
   到实际部件资源的转换规则。
2. 用原生客户端截图对 `--equip` 各槽位做逐项对拍，确认槽位 → 部件映射。
3. 把 `Skill.csv` 光效 `front/back` 层按动作/帧合成到渲染帧。
4. 染色（CTI HSV）与坐骑变色。
5. 应用 `mount_attach`/`cloakoffset` 做挂点对齐并与 C++ 客户端对拍。
