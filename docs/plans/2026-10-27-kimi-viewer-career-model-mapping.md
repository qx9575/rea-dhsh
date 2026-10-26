# kimi_viewer 职业联动与模型映射 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 修正人物与装备的职业联动、播放装备模型动画、移除坐骑技能入口，并建立有证据来源的模型 ID 名称映射及待确认清单。

**Architecture:** 在 `avatar.py` 集中实现 CSV 解码、职业权限、模型映射与动画选择，`avatar_view.py` 只负责选项重建和展示。静态表映射优先，运行时证据只补静态缺口；每项改动先用真实 PAK 数据的离屏断言复现失败。

**Tech Stack:** Python、PySide6/PyQt5、Pillow、现有 `PakSource`、`dhsh.formats.scp.decode_scp`、Qt offscreen。

---

### Task 1: 修复文本解码与装备职业过滤

**Files:**
- Modify: `tools/kimi_viewer/avatar.py:245-259,394-404,445-464,519-562`
- Verify: 内联 Python，不新增测试文件

**Step 1: 写失败断言**

以 `PYTHONPATH=tools QT_QPA_PLATFORM=offscreen python -u -` 启动，加载真实 PAK；断言 `灵兽骨甲`、`桃木杖`、`神火妖瞳` 名称无乱码，术士过滤包含桃木杖且不含青铜剑，剑客过滤相反。

**Step 2: 运行并确认 RED**

预期物品名断言因 GB18030 错解 UTF-8 而失败；保留原始职业标志输出，区分编码错误和过滤错误。

**Step 3: 最小实现**

复用 `decode_scp` 解码 `Skill.csv`、`Monsters.csv`、`viewjob.csv`、`itemSpecData.csv`、`horseinfo.csv`，不复制编码探测逻辑。职业下拉索引继续显式转换为物品职业列索引。

**Step 4: 运行并确认 GREEN**

相同断言全部通过；打印各职业物品数量，确认过滤后列表确实变化。

**Step 5: Commit**

`git add tools/kimi_viewer/avatar.py && git commit -m "kimi_viewer: fix CSV decoding and career filters"`

### Task 2: 建立来源明确的映射层

**Files:**
- Modify: `tools/kimi_viewer/avatar.py:394-404,467-479,538-566`
- Modify: `tools/kimi_viewer/avatar_view.py:243-250,336-345,385-409,491-552,596-611`
- Verify: 内联 Python

**Step 1: 写失败断言**

验证 `horse/210` 不再标成闪电犬；`horse/810` 对应闪电犬；`Lcuff/Rcuff` 模型集合非空；骑甲 `100` 可显示绸羊鞍；NPC 重名保留别名；无名称模型返回待确认而不是猜测。

**Step 2: 运行并确认 RED**

预期旧 30800 公式、护腕大小写和缺少骑甲映射导致失败。

**Step 3: 最小实现**

新增统一名称查询结构，保留名称列表与来源。用 `horseinfo` 的职业、等级和 `viewjob` 的职业、进阶、性别、坐骑档交叉得到坐骑映射；删除 30800 通用公式。扩展 NPC 音效/头像补充映射，加入 `bodyidequal` 次级别名。`all_part_models` 对目录比较小写化，保留资源原始 part 用于装配。

**Step 4: 运行并确认 GREEN**

相同断言通过，输出每类已确认/待确认/总数。映射只接受实际存在的目标资源，不跨槽位猜名。

**Step 5: Commit**

`git add tools/kimi_viewer/avatar.py tools/kimi_viewer/avatar_view.py && git commit -m "kimi_viewer: add sourced model name mappings"`

### Task 3: 人物职业联动与坐骑界面

**Files:**
- Modify: `tools/kimi_viewer/avatar.py:445-479`
- Modify: `tools/kimi_viewer/avatar_view.py:258-285,360-435,515-552,635-716`
- Verify: 内联 Qt offscreen

**Step 1: 写失败断言**

实例化 `AvatarDialog`，切术士后断言人物武器候选与术士物品模型集合一致且不只默认套装；切剑客后集合变化。断言坐骑类别 `row_skill` 全部控件隐藏，人物类别恢复。

**Step 2: 运行并确认 RED**

预期人物仍显示全量装备，坐骑技能仍可见。

**Step 3: 最小实现**

由物品职业权限生成各装备部位允许的模型 ID，再与实际资源求交集；身体/头部不限制。坐骑按已验证职业关系过滤。合法旧选择保留，否则选第一项；批量重建期间阻止递归信号，最后统一刷新动作和画面。坐骑类别不显示技能行，资源明细也不附加技能。

**Step 4: 运行并确认 GREEN**

职业切换、技能职业过滤、红莲诀切换和类别切换断言通过，无旧帧残留。

**Step 5: Commit**

`git add tools/kimi_viewer/avatar.py tools/kimi_viewer/avatar_view.py && git commit -m "kimi_viewer: link avatar equipment and mounts to career"`

### Task 4: 装备模型动画

**Files:**
- Modify: `tools/kimi_viewer/avatar.py:538-548`
- Modify: `tools/kimi_viewer/avatar_view.py:554-595,635-716,718-771`
- Verify: 内联 Qt offscreen

**Step 1: 写失败断言**

选择桃木杖、灵兽骨甲，要求预览来自对应 `weapon/400`、`cloth/450` APG，存在多帧时 `ImagePage.timer.isActive()`；无模型物品保持图标/详情并显示无模型动画。

**Step 2: 运行并确认 RED**

预期当前 `_equip_changed` 只设置单张图标。

**Step 3: 最小实现**

增加物品到 `(part, model)` 的严格槽位转换；装备选择时先查实际 APG，调用现有 `part_anim` 和动作/方向回退，设置多帧与延时。没有模型时走原图标路径。资源明细显示实际动画路径。

**Step 4: 运行并确认 GREEN**

装备动画、自动播放、静态图标降级全部通过，其他类别不回归。

**Step 5: Commit**

`git add tools/kimi_viewer/avatar.py tools/kimi_viewer/avatar_view.py && git commit -m "kimi_viewer: animate equipment models"`

### Task 5: 定向补全运行时映射

**Files:**
- Create: `manifests/kimi_viewer_model_coverage.csv`（确认/待确认、来源）
- Create: `tools/kimi_memory_model_probe.py`（仅确需运行时探针时）
- Modify: `tools/kimi_viewer/avatar.py`
- Verify: 静态覆盖统计与运行时对照记录

**Step 1: 静态覆盖审计**

列出 horse、pet、npc、body/head、装备和骑甲的待确认 ID，优先处理实际常用与界面首屏资源；不得把服务器下发但无法绑定资源 ID 的文本当成名称。

**Step 2: 运行时最小探针**

复用现有 Frida 接入方式，只读用户已启动的 Game.exe。先定位宠物/坐骑界面或对象中的资源 ID 与名称字段；需要用户操作时一次只要求一个明确动作。

**Step 3: 证据验收**

同一对象至少两个独立字段或一次受控切换形成 ID→名称对应；记录构建、对象/界面、操作和结果。不能复核的结果继续待确认。

**Step 4: 合入已证实映射并复测**

映射层加入有证据条目和来源，所有模型仍完整列出，未确认项保持 `待确认(ID)`。

**Step 5: Commit**

`git add tools/kimi_viewer/avatar.py tools/kimi_memory_model_probe.py manifests/kimi_viewer_model_coverage.csv && git commit -m "kimi_viewer: document and extend verified model mappings"`

### Task 6: 整体验证与文档

**Files:**
- Modify: `tools/kimi_viewer/README.md`
- Modify: `tools/kimi_viewer/docs/优化.md`
- Modify: `docs/PROGRESS.md`
- Verify: 真实窗口与离屏回归

**Step 1: 全量离屏回归**

运行 Tasks 1–4 的全部断言，确认六类别、职业联动、技能切换、动画、映射和静态降级通过。

**Step 2: 真实窗口验收**

运行 `python tools/kimi_viewer.py`，依次检查术士/剑客切换、装备过滤与动画、坐骑无技能、名称/待确认显示、复制资源明细。

**Step 3: 更新文档**

记录正确映射规则、覆盖统计、运行时证据与未决项，删除旧 30800 公式和“装备仅图标”等过期说明。

**Step 4: 最终验证与提交**

`git diff --check && git status --short`，确认只有预期文件；提交文档及最终修复。不得提交 `pic/` 或 `.rea/` 中凭据。
