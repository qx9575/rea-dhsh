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

# 术士女 + 灵兽套装 + 桃木杖（body 默认自动取 450）
PYTHONIOENCODING=utf-8 python tools/paperdoll/cli.py render \
  --shdata D:/software/dhsh2/shdata \
  --career 4 --advance 0 --mount-tier 0 --gender 1 --head 450 \
  --actions 10 --directions 2 \
  --equip 1400 --equip 2450 --equip 8400 --equip 9450 \
  --equip 11450 --equip 4400 \
  --out extracted/paperdoll/equip

# 浪子星将（潜影·女，bodyidequal 36615）
PYTHONIOENCODING=utf-8 python tools/paperdoll/cli.py render \
  --shdata D:/software/dhsh2/shdata \
  --career 6 --advance 2 --mount-tier 0 --gender 1 \
  --star-general 36615 --actions 10 --directions 0,3 \
  --out extracted/paperdoll/star-general --no-gif
```

## 星将/星神形态

`bodyidequal.csv` 的 `36xxx/38xxx` 行是星将/星神形态外观，优先于普通
`viewjob`/装备公式：

```text
36x00–36x35：职业 1–9 的星将形态
380xx–386xx：职业 10–16 的星将形态
```

- `--star-general ID`：显式指定形态行（如浪子星将女 `36615`、男 `36600`）。
- `--star-auto`：按 `career/gender` 取默认行；当前只硬编码了浪子
  `(6,0)→36600`、`(6,1)→36615`，其他职业按同职业同性别的最低 variant 回退。
- 星将行提供 `body/head/cloth/weapon/effect/shoe/mask/cuff` 及颜色；
  命中后 `--override`/`--equip` 仍可覆盖单个部件。
- 已确认：浪子星将女 `36615` → `body 650 / cloth 671 / weapon 671 /
  shoe 671 / cuff 671 / effect 650`；`head/662` 当前 PAK 缺失。
- 参考：`docs/art-pipeline/dhsh2-0dbc/relations.md` §2.2，
  `manifests/relations/star-general-appearance.csv`。

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

低阶实测公式（不再是 `item_id - slot*100`）：

```text
weapon           = item_id % 1000 + 50   （4400→450，4402→452）
cloth/shoe/cuff  = item_id % 1000 + 3    （2450→453，9450→453，11450→453）
mask/belt        = item_id % 1000 + 50   （8400→450）
```

高阶套装/星将形态不套该公式，走 `bodyidequal` 星将行、套装表或
`ITEM_APPEARANCE` 实测覆盖；未识别槽位/物品会在 `manifest.equip[]` 中以
`unknown_slot` / `item_not_found` 记录，不会静默丢弃。

实测装备（1.50.27 客户端，`ITEM_APPEARANCE`）：灵兽骨甲 2450→`cloth/453` +180°、
灵兽骨腕 11450→`Lcuff/Rcuff/453` +180°（客户端手腕为蓝色）、灵兽皮靴 9450→`shoe/453` +180°、
桃木杖 4400→`weapon/450` + `effect/450`（光效色相 127°）、灵兽束腰 8400→`mask/450`。
`--effect-hue` 可覆盖武器光效色相；`--equip 4400` 会自动追加 effect 层。

> 槽位 → 部件映射来自 `resmirror` part type 与参考查看器，**尚未经原生客户端运行时验证**；
> 渲染结果可用于人工比对（见“已知限制”）。


## 渲染说明

- 层叠顺序：APCI 1B 层叠键升序；key 相同时按 `PART_DRAW_ORDER` 的部件顺序
  （body→shoe→cloth→rcuff→weapon→effect→lcuff→head→mask）。右护腕在武器下、
  左护腕在武器上；effect 默认跟随武器层叠键。护腕颜色与衣服同为 +180°。
- 锚点/延时：动作目录 `perinfo.dat`（PIF f2/f3/f5）；缺省 `(160,210)/80ms`。
- 坐骑：直接按 APG 帧 x/y + APCI 层叠键合成，**不做底心平移**；`horseoffset`
  仅作为挂点参考（已在附近玩家 player4/player6/player9 的坐骑上用渲染验证）。
- 资源定位：APG/PIF/CTI 一律按**完整相对路径**精确匹配，不做 basename 回退
  （避免 `0.apg`/`perinfo.dat` 误命中其他模型）。
- 批量渲染：`--actions` / `--directions` 接受逗号分隔列表；每个组合输出独立目录，
  汇总写入 `<out>/render_batch.json`（含 `status=ok/no_frames` 与跳过原因）。
- 朝向编号（2026-11-09 客户端截图对照）：`0=右, 1=右下, 2=下, 3=下左,
  4=左, 5=左上, 6=上, 7=右上`（屏幕坐标顺时针）。
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

- 身体默认 `career*100 + 50*gender`（如术士女=450、剑客男=100）；无法命中时回退
  `200`。头部默认 `105`，可用 `--body/--head` 覆盖。
- 衣服/手套/鞋/武器/坐骑/骑甲来自 `viewjob`。
- 武器光效 `effect/<model>/…`：由 `ITEM_EFFECT`（当前 4400→450，色相 127°）或
  `--override effect=MODEL` 生成；没有装备光效时不加该层。
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
- `rcuff` 的 APCI 键在部分模型为空；paperdoll 在 rcuff 缺键时继承 lcuff 的键。
- 当前客户端缺少新职业部分神骑模型：13–16 职业 mount tier 2 的 `20700/21700/…`
  在全部 PAK 中都找不到（可能未实装/未下载），仍按 `missing` 记录。
- 新职业 15/16 的 `615xx/616xx` 外观 id 与 12 职业的 `6602/6652`
  不在对应部件空间（部分只在 `effect`/`head` 空间），需运行时确认实际渲染方式。
- `--equip` 的槽位 → 部件映射未经原生客户端运行时验证；`item_id - slot*100`
  公式在有同名模型跨部件空间时会歧义（需 resmirror/引擎消费点佐证）。
- **实测外观映射与公式不符**（2026-11-09 内存+截图）：灵兽骨甲 2450→`cloth/453`
  +180° 色相、骨腕 11450→`Lcuff/Rcuff/453` +180°、皮靴 9450→`shoe/453`、
  桃木杖 4400→`weapon/450` + `effect/450` 光效（127°）、束腰 8400→`mask/450`。
  已接入 `ITEM_APPEARANCE`/`ITEM_EFFECT`，详见
  `docs/art-pipeline/dhsh2-0dbc/asset-spec.md` §4.3。
- 护腕/手在武器之上的精确层级仍按截图近似（`PART_DRAW_ORDER`）；客户端手部
  像素较小，待更高分辨率对拍确认。
- 染色（CTI `shapeclrinfo`/`colorconvert`）与坐骑变色尚未接入。
- 宠物“幼螈宫”未渲染（蓝色球模型 ID 仍未定位）。
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
