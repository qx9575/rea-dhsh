# art_pipeline — 纸娃娃/美术流程只读分析

本目录是独立分析工具，**不修改** `tools/kimi_viewer/`、`client/` 或 `D:\software\dhsh`。
目标是把“原客户端已有的类别、子部件、表配置、资源使用”从数据层反推出来，
优先服务现有 17 职业/现有类别，而不是先做新职业。

## 1. 设计原则

1. **权威表优先**：部件/动作枚举以 `config.pak:misc/resmirror.csv` 为准；
   `viewjob.csv`/`bodyidequal.csv` 等表定义 ID 关系；查看器的六类别只是 UI 归纳。
2. **ID 空间分开**：`resmirror`、`partinfo`、`shapeclrinfo` 是三套不同的部件类型枚举，
   不能合并成一个 `PARTS`。
3. **只读**：只读 PAK、`pak_inventory.json`；输出只写 `extracted/art_pipeline/` 与
   `docs/art-pipeline/`。
4. **可复现**：所有结论落 SQLite/JSON；文档由数据生成，不手写覆盖。

## 2. 快速开始

```bash
# 仓库根目录；PYTHONIOENCODING 避免 GBK 控制台乱码
PYTHONIOENCODING=utf-8 python tools/art_pipeline/cli.py all

# 分步
PYTHONIOENCODING=utf-8 python tools/art_pipeline/cli.py tables
PYTHONIOENCODING=utf-8 python tools/art_pipeline/cli.py inventory
PYTHONIOENCODING=utf-8 python tools/art_pipeline/cli.py careers
PYTHONIOENCODING=utf-8 python tools/art_pipeline/cli.py scripts
PYTHONIOENCODING=utf-8 python tools/art_pipeline/cli.py report
```

默认路径：

| 参数 | 默认值 |
|---|---|
| `--shdata` | `D:\software\dhsh\shdata` |
| `--out` | `extracted/art_pipeline/` |
| `--docs` | `docs/art-pipeline/` |
| `--inventory` | `kimi_viewer_data/pak_inventory.json`（存在则用，缺失则实时扫 PAK） |

`--rebuild-inventory` 强制忽略 JSON 缓存、直接扫描 PAK。

## 3. 产出

### 3.1 数据

| 文件 | 内容 |
|---|---|
| `extracted/art_pipeline/art_pipeline.sqlite` | 所有表、派生关系、资源明细、职业覆盖 |
| `extracted/art_pipeline/tables.json` | 表清单与行数 |
| `extracted/art_pipeline/inventory_summary.json` | 部件/模型/动作/异常/perinfo 覆盖 |
| `extracted/art_pipeline/career_coverage.json` | 272 条 viewjob 预设的覆盖统计 |
| `extracted/art_pipeline/tables/*.csv` | 解码后的原始表文本（便于对照） |
| `extracted/art_pipeline/scp_index.json` | 1,940 个 SCP 的窗口/消息/脚本摘要 |
| `extracted/art_pipeline/lua_index.json` | 46 个 `.lua` + SCP `#script` 的 FSLua API/窗口/回调摘要 |

### 3.2 文档（自动生成）

| 文件 | 内容 |
|---|---|
| `docs/art-pipeline/taxonomy.md` | 权威部件/动作枚举、三套枚举差异、资源统计、查看器对照 |
| `docs/art-pipeline/tables.md` | 表字典、ID 空间、关键关系、使用状态 |
| `docs/art-pipeline/asset-spec.md` | 路径形状、部件统计、perinfo 覆盖率、资源清单 |
| `docs/art-pipeline/new-career.md` | 在原 17 职业框架内扩展的清单与覆盖缺口 |
| `docs/art-pipeline/lua-scp-usage.md` | Lua/SCP 交互索引：FSLua API、窗口、MESSAGE、CallScProcess、回调交集 |
| `docs/art-pipeline/engine-consumers.md` | Ghidra 定点分析：表加载函数、镜像消费者、Lua 宿主、硬上限 |
| `docs/art-pipeline/open-questions.md` | 未决问题（P0–P4） |

## 4. 模块

| 模块 | 职责 |
|---|---|
| `common.py` | 按需扫描 PAK、NZ 解码、文本解码、CSV 整体解析（保留引号内换行） |
| `idspaces.py` | 部件/动作/职业/进阶/坐骑/性别/槽位的 ID 空间注册表 |
| `tables.py` | 表读取、派生表、SQLite 构建 |
| `inventory.py` | 资源 taxonomy、模型/动作/朝向、perinfo 覆盖、异常 |
| `careers.py` | `viewjob` 预设解析与资源覆盖检查 |
| `scp_index.py` | SCP 窗口/控件、`MESSAGE`、`#script` 索引 |
| `lua_index.py` | `.lua` + `#script` 的 FSLua API、窗口名、回调名、CallScProcess |
| `engine_findings.py` | Ghidra 定点分析结果（表加载函数、镜像消费者、Lua 宿主、硬上限） |
| `report.py` | 由数据生成 `docs/art-pipeline/*.md` |
| `cli.py` | 命令行入口 |

## 5. SQLite 常用查询

```sql
-- 权威部件类型
SELECT * FROM id_spaces WHERE space = 'part_type.resmirror' ORDER BY CAST(value AS INT);

-- 某职业的全部预设
SELECT * FROM viewjob_presets WHERE career = 13;

-- 某部件的资源模型
SELECT pak, model, actions, directions, files, has_perinfo
FROM resource_models WHERE part_key = 'head' ORDER BY pak, model;

-- 职业覆盖缺口（status: found / missing / id_space_mismatch / unconfigured）
SELECT career, field, part, model, status, note
FROM career_coverage WHERE status != 'found' ORDER BY career, field, model;

-- 技能（玩家）
SELECT skill_id, name, career, system, tier, hit_effect, attack_effect, fail_effect
FROM skill ORDER BY skill_id;

-- 发型映射
SELECT * FROM hair_map WHERE mount_id = 0;

-- 挂点
SELECT * FROM attach_horse LIMIT 10;
SELECT * FROM attach_cloak WHERE action = 10 AND direction = 0 LIMIT 10;

-- SCP 窗口/消息
SELECT scp_key, window, message, arg, code FROM scp_messages
WHERE message = 'FSM_BTN_MSG' LIMIT 20;

-- Lua 窗口/回调/模块
SELECT * FROM lua_windows LIMIT 20;
SELECT * FROM lua_messages WHERE api = 'RegMsgCall' LIMIT 20;
SELECT module, COUNT(*) FROM lua_installs GROUP BY module ORDER BY 2 DESC;

-- SCP CODE 与 Lua 回调交集
SELECT DISTINCT m.code FROM scp_messages m
JOIN lua_messages l ON l.name = m.code AND l.api = 'RegMsgCall';
```

## 6. 表与资源约定

- 表读取优先按逻辑路径精确匹配，失败按 basename 回退（大小写不敏感）。
- CSV 解析使用 `csv.reader` 整体解析，保留引号内换行；首列以 `#`/`//` 开头的行归为注释。
- 玩家资源路径：`<part>/<model>/<action>/<direction>.apg`；
  动作目录 `perinfo.dat` 可选，缺失时查看器回退 `(160,210)/80ms`。
- NPC：`<model>/<action>/<direction>.apg`；魔法：`<model>/<variant>/front|back.apg`。
- `Lcuff/lcuff`、`Rcuff/rcuff` 在索引里按小写归一，但保留实际目录名并记录 `case_variant`。

## 7. 新增一张表

1. 在 `tables.py` 的 `TABLE_SPECS` 增加 `TableSpec`（key、pak、path、header_mode、min_cols）。
2. 如需结构化查询，在 `build_sqlite` 中增加 `CREATE TABLE` 与 `_derive_*`。
3. 在 `report.py` 的 `tables_md` 中补充用途；必要时在 `taxonomy.md`/`asset-spec.md` 增加段落。
4. 重跑 `cli.py all`，检查 `tables.json` 行数与 SQLite 计数。

## 8. 当前边界

- 尚未接入：Lua 静态索引、SCP 交互图、C++ 引擎消费函数、网络协议字段。
- `resmirror` 只覆盖部分部件类型；`effect`/`shadow`/发型/翅膀/披风/星神的消费路径仍未知。
- 职业覆盖里的“缺失”是**按 viewjob 资源 ID 直接查 player*.pak** 的结果；
  星神/神骑可能使用不同 ID 空间，不能据此断定美术资源不存在。
- 17 职业/3 进阶/3 坐骑/8 方向是否硬编码在 C++ 中，尚未验证。

## 9. 只读保证

- 不写入 `D:\software\dhsh`。
- 不修改 `tools/kimi_viewer`、`client` 或现有工具。
- 生成的中间数据在 `extracted/`（已被 `.gitignore` 忽略），文档在 `docs/art-pipeline/`。
