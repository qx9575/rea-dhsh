# paperdoll — 独立纸娃娃工具（P2.1 清单）

只读原 PAK，不修改 `kimi_viewer` / `client`。当前阶段输出“职业预设 → 部件资源”清单，
作为美术制作、表配置和原生客户端装配的验证器。

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
```

## 渲染说明

- 层叠顺序：APCI 1B 层叠键升序；key 相同时按部件名排序。
- 锚点/延时：动作目录 `perinfo.dat`（PIF f2/f3/f5）；缺省 `(160,210)/80ms`。
- 坐骑对齐：body 与 horse 的底心对齐；`horse/harmorbody/harmorhead` 应用偏移。
- 画布：640×480，角色参考点对齐画布中心。
- 已验证：测试预设（career=1, advance=0, mount=1, gender=0, action=10, dir=0）
  的 11 帧 RGBA 哈希与 `kimi_viewer.AvatarSource` 逐帧一致。

## 输出字段

`manifest.json`：

| 字段 | 含义 |
|---|---|
| `preset` | career/advance/mount_tier/gender/hero_branch/stargod_model |
| `parts[]` | 每个部件的 `field/part/model/pak/apg/exists/frames` |
| `perinfo` / `anchor` / `delay` | 动作目录 PIF；缺失时为默认/空 |
| `cti` | 同目录或向上找到的 `clrtb.cti` |
| `keys` | APCI 层叠键（逐帧或单值） |
| `stargod` | 星神造型（`npc*.pak` 模型） |
| `missing[]` | 资源缺失项 |
| `skills[]` | 该职业在 `Skill.csv` 中的技能与光效 ID |

## 当前覆盖

- 身体/头默认 `200/105`；可用 `--body/--head` 覆盖。
- 衣服/手套/鞋/武器/坐骑/骑甲来自 `viewjob`。
- 星神来自 `viewjob` 第 13 列，解析到 `npc*.pak`。
- 技能来自 `Skill.csv`，按职业过滤。

## 已知限制

- 只解析一个动作/方向；批量动作/方向待加。
- `rcuff` 的 APCI 键在部分模型为空（表/资源本身可能只登记一侧）。
- 尚未渲染 PNG/GIF；渲染器是下一步。
- 装备物品覆盖（`itemSpecData` 槽位公式）和染色尚未接入。

## 下一步

1. `validate`：批量检查 272 条预设的必需资源。
2. 接入 `itemSpecData` 装备覆盖与 `Skill.csv` 光效前后层。
3. 染色（CTI HSV）与坐骑变色。
4. 多动作/多方向批量渲染与 C++ 客户端对拍。
