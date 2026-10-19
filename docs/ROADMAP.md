# 资源与脚本分析路线图（dhsh / Hero2D）

- 目标：`D:\software\dhsh`（《新水浒Q传》客户端 1.50.20）
- 工作区：`D:\ai-work-flow\rea-dhsh`
- 基线日期：2026-10-19
- 已有基线产物：`pak_inventory.json`（523,212 条叶子资源，约 87 MB）
- 已有结论：`CLAUDE.md`（目录与格式初步说明）
- 配套文档：`docs/PLAN.md`（由简入难阶段计划）、`docs/PROGRESS.md`（当前进度）、`docs/formats/`（各格式细节，随分析推进补充）

---

## 1. 目标与边界

### 1.1 交付目标

1. **资源解包**：PAK 归档 → NZ 封装 → 原始资源文件，可全量、可复现地导出。
2. **资源解析**：动画（APG/PIF）、图片（A16/CTI/PG2/TGA/BMP/JPG）、地图（MAP/MAN/CEL/CORE/MSK）、头像图集（APCI）等转换为通用格式（PNG/GIF/JSON）。
3. **脚本提取与分析**：Lua（FSLua）、SCP UI 脚本、CSV 数据表、INI 配置；恢复可读源码、建立 API/事件/窗口清单与数据表数据库。
4. **工具化**：一条命令完成“解包 + 解码 + 分类 + 导出 + 生成清单”，并生成资源浏览器可用的索引。
5. **文档**：每种格式都有“已观察结构 / 推断 / 未知”的记录，可被后续会话复用。

### 1.2 非目标与约束

- 只读分析 `D:\software\dhsh`，不修改、不删除、不重命名源文件。
- 不运行游戏与反外挂组件；优先静态分析，避免注入/调试被保护进程。
- 不涉及服务端协议、账号数据、绕过保护、修改客户端等用途。
- 导出资源仅用于本地研究/兼容性分析，不对外分发游戏素材。

### 1.3 成功的判定标准

| 编号 | 标准 | 验证方式 |
|---|---|---|
| S1 | 40 个顶层 PAKF 全部可递归解析，无越界/错误偏移 | 解包器自检 + 与 `pak_inventory.json` 数量一致 |
| S2 | 抽样叶子文件解包后 magic/结构正确 | TGA/BMP/JPG 可打开；Lua 可 `luac -p`；CSV 可解析 |
| S3 | 至少 99% 的叶子资源可解码（NZ 三种类型全覆盖） | 解码失败清单 + 抽样人工复核 |
| S4 | 至少一套角色/NPC/技能动画可还原为帧序列 | 导出 PNG 序列并播放验证 |
| S5 | 至少一张地图可还原为预览图 | 地图资源拼合验证 |
| S6 | 脚本可按模块导出并建立 API/事件索引 | 脚本数据库 + 抽样语义说明 |
| S7 | 全流程可重复执行，输出有校验和 | 运行两次输出一致 |

---

## 2. 现状基线

### 2.1 已验证事实

**PAKF3001 归档**

```c
struct PakHeader {          // 16 bytes, little-endian
    char     magic[8];      // "PAKF3001"
    uint32_t checksum;      // 未知
    uint32_t entry_count;
};
struct PakEntry {           // 48 bytes
    char     name[32];
    uint32_t offset;        // 相对当前归档基址
    uint32_t size;
    uint32_t type;          // 0 = 叶子文件，1 = 嵌套 PAKF3001
    uint32_t reserved;
};
// 数据区起点 = 0x10 + entry_count * 48
```

解析注意：

- 实际顶层 `.pak` 为 **41 个**：40 个 PAKF 归档 + `perinfo.pak`（`APCI`，不是 PAKF）。
  另有 4 个更新补丁包（`update/_tmp/config.pak`、`update/_tmp/ui.pak`、
  `update/update/*.pak`），本轮未纳入叶子统计。
- `offset` 相对当前归档基址；递归时必须传递新的 base。
- **size=0 别名**：少数条目（如 `config.pak` 中 `map/*.ini`）的 `size` 字段为 0，实际数据与同偏移的 `.map` 条目共享；真实长度由 NZ 头 `size2` 决定（`12 + size2`）。解析器必须处理，否则会写出空文件。

**NZ 资源封装**（大部分叶子文件）

```
0x00  2  "NZ"
0x02  2  type      // 0x0000 / 0x000A / 0x000B
0x04  4  size1     // 解压后/逻辑大小
0x08  4  size2     // 存储大小（payload 字节数）
0x0C  N  payload
```

全量扫描（523,212 个叶子）的类型分布：

| NZ type | 叶子数 | 占比 | 字节 | 观察 |
|---|---:|---:|---:|---|
| `0x0000` | 249,361 | 47.7% | 2.85 GiB | 原样存储，`size1 == size2`；payload 以真实文件 magic 开头 |
| `0x000B` | 214,623 | 41.0% | 3.60 GiB | 字节流压缩；payload 开头保留大量可读脚本文本与 GBK 中文，后段出现二进制 token |
| `0x000A` | 59,222 | 11.3% | 2.51 GiB | 16 位 token 流；可见 ASCII/GBK 字节以 16 位字存放，另有高字节非 0 的控制/回溯 token |
| 非 NZ / 零长度 | 6 | <0.1% | 0 | 记录为异常条目 |

> 结论：**type=0 覆盖约 47.7% 的叶子资源**；NZ 解码器是解锁其余约 52.3% 的关键路径。
> 当前非 APG 的 type=0 资源已全部导出（41,550/41,550）；APG 仅导出样本（655/207,811）。

**脚本现状**

- `.lua` 共 46 个叶子：7 个 type=0（可直接阅读的 GBK 源码）、18 个 0x0A、21 个 0x0B。
- `.scp` 共 1,940 个：732 个 type=0（文本 DSL，含 `#include`、`WINDOW TYPE`、`RECT`、
  `NAME` 等），625 个 type=0x0A，583 个 type=0x0B。
- `.csv` 数据表为“GBK 字节逐字节扩成 16 位”的编码；可直接从 type=0 样本开始解析。
- 脚本引擎线索：`FSLuaInstall`、`FSLuaUIWins`、`FSLuaUIWindow`、`FSLuaWndScript.RegMsgCall`、
  `CallScProcess`、`FSLuaEvent`。判断为 **Hero2D 的 FSLua 脚本绑定 + SCP UI DSL**。

**格式线索**

- `Game.exe` 内含 `Hero2D`、`PAKF`、`APCI`、`animation\%d.apg`、`npc/%d/10/%d.apg` 等字符串。
- `Main.exe` 含 PDB 路径 `D:\work\hero\Tools\Hero2dTools\GameShell\output\vs2013\main.pdb`，
  说明引擎工程名为 Hero2dTools，VS2013 构建。
- `.apg` 结构：`AG` 文件头 → 偏移/帧索引 → `AP` 块 → JPEG 帧（`FF D8 FF E0`）。
- `.dat` 的 PIF 头与同名 APG 配对，估计保存帧尺寸/锚点/时序。
- `perinfo.pak` 为 `APCI` 容器（头像/头像框图集），与 PAK 不同。
- 大量 APG 文件存在 type=0 原始形式（抽样中 .apg 有约 1/3 为 type=0），可作为
  **已知明文样本**用于对照 codec。

### 2.2 未知/待验证

- NZ `0x0A` / `0x0B` 的精确压缩算法（关键）。
- `.a16`、`.cti`、`.pg2` 的完整字段与像素格式。
- `APCI` 容器布局。
- `.man/.cel/.core/.msk` 地图数据的坐标/图层语义。
- SCP 操作码与 FSLua 扩展 API 的完整语义。
- `001.cpm`、`ccore.dat`、`cltgp.dat`、`sl.dat` 等加密数据块的用途。

---

## 3. 总体技术路线

```
                        ┌──────────────────────────┐
                        │ Game.exe 静态分析 (Ghidra)│
                        │ 定位 PAKF/NZ/APG 读写函数 │
                        └────────────▲─────────────┘
                                     │ 线索/算法
┌──────────┐    ┌──────────┐    ┌────┴─────┐    ┌────────────┐
│ 40 × PAK │───▶│ PAK 解包 │───▶│ NZ 解码  │───▶│ 资源格式解析 │──▶ PNG/GIF/JSON
└──────────┘    └────┬─────┘    └────┬─────┘    └────────────┘
                     │               │
                     ▼               ▼
              ┌────────────┐   ┌────────────┐   ┌──────────────┐
              │ 清单/SQLite│   │ 脚本解码    │──▶│ Lua/SCP 分析 │──▶ API/UI 文档
              └────────────┘   └────────────┘   └──────────────┘
```

**两条并行主线**

- A 线（资源）：PAK → NZ → APG/A16/CTI/PG2/地图 → 导出与预览。
- B 线（脚本）：PAK → NZ/原文 → Lua/SCP/CSV → 源码/API/数据表。
- 两线共享同一个解包器、清单库和 NZ 解码器。

**关键路径**：NZ codec（0x0A/0x0B） → 后续几乎所有图像/动画/脚本解析都依赖它。

---

## 4. 分阶段计划

### P0 准备与基线固化（0.5 天）

任务：

1. 建立目录：`tools/`、`tests/`、`extracted/`、`manifests/`、`docs/formats/`、`notes/`。
2. 固化客户端身份：拷贝 `conf.ini`、`ver.csv` 内容到 `manifests/`；记录源目录大小与文件哈希
   （至少 45 个 `.pak` 的 SHA-256 清单）。
3. 建立样本库：从每个 PAK 抽取 type=0 / 0x0A / 0x0B 的小、中、大样本，保存为
   `tests/fixtures/`（建议 < 50 MB），并记录预期输出（图片可打开、Lua 可解析等）。
4. 确认工具链：Python 3.13（命令名 `python`）、Ghidra/IDA 是否可用；REA MCP 当前未注册，
   需要原生反编译时再按 REA 流程配置。
5. 生成版本化清单：`manifests/paks.sha256`、`manifests/inventory-summary.csv`。

验收：样本库可重复生成；PAK 哈希固定；后续解码结果可追溯到具体客户端版本。

### P1 PAK 递归解包器（1–2 天）

任务：

1. 实现 `tools/dhsh/pak.py`：
   - 校验 `PAKF3001`、`entry_count` 上限（防御坏索引）。
   - 递归展开 `type=1`；偏移始终相对当前归档基址。
   - 处理 `size=0` / 空条目 / 非 PAKF 的 type=1（记录异常，不崩溃）。
2. 导出两种形态：
   - `extracted/raw/<pak>/<nested path>/<leaf>`：保留叶子原始字节（含 NZ 头）。
   - `extracted/nz/<pak>/<path>`：剥离 NZ 头后的 payload（供 codec 处理）。
3. 生成 SQLite 清单 `manifests/resources.db`：
   - 表 `paks`（名称、大小、条目数、SHA-256）
   - 表 `resources`（pak、逻辑路径、物理偏移、size1、size2、nz_type、扩展名、SHA-256）
   - 表 `failures`（异常条目与原因）
4. 用 `pak_inventory.json` 交叉校验叶子总数与路径集合。

验收：40 个 PAKF 全部解析；叶子总数 523,212（允许因异常条目有少量差异并解释）；
任意随机样本的偏移与大小在文件范围内。

交付：`tools/dhsh/pak.py`、`manifests/resources.db`、`manifests/paks.sha256`。

### P2 NZ 解码器（3–5 天，关键路径）

#### P2.1 类型 0（半天）

- 直接剥离 12 字节头，校验 `size1 == size2 == len(payload)`。
- 输出到 `extracted/decoded/`，按扩展名分类。

#### P2.2 类型 0x0B（字节流 codec）

观察与假设（需验证）：

- payload 前段保留大量原文（GBK 文本、`WINDOW TYPE` 等），后段出现高频二进制 token。
- 形态非常像 **LZSS/LZ77 类字节流压缩**：控制字节 + 字面量/回溯对。
- 部分文件首字节为 `0x00`，其后的文本几乎逐字节可读，说明“字面量直出”是 codec 的一部分。

实施步骤：

1. 从 `tests/fixtures/` 选取 20–50 个 type=0x0B 样本，统计：
   - 字节熵、高低位分布、token 间隔；
   - `size1`（解压后大小）与 payload 长度的关系；
   - 同一内容是否存在 type=0 原始版本（优先寻找同名/近似文件）。
2. 建立“已知明文测试集”：例如 type=0 的 Lua/SCP/CSV 与可读 payload 前缀，用于验证候选算法。
3. 实现候选解码器（先按常见 LZSS：flag 字节 + 8 个 token，offset/length 编码逐个试），
   用以下硬性条件筛选：
   - 输出长度精确等于 `size1`；
   - 输出可被 GBK 解码且可读比例显著提升；
   - Lua 文件可 `luac -p`；图片文件 magic/尺寸正确。
4. 若不匹配，转 P2.4 的静态分析定位算法。

#### P2.3 类型 0x0A（16 位 token codec）

观察与假设（需验证）：

- payload 以 16 位字为单位；字面量形式为 `0x00XX`（低字节为 ASCII 或 GBK 字节）。
- 高字节非 0 的字（`0x01XX`、`0x02XX`……）出现频繁，像是控制码/回溯 token。
- 与 0x0B 可能是同一 LZ 核心的“宽字符版本”，也可能是独立的 token 字典压缩。

实施步骤：

1. 按 16 位小端切分，统计 token 高字节分布、重复模式、与已解码文本的对应关系。
2. 尝试“字面量直出 + 回溯 token”的 LZSS 变体；确定 offset/length 的位宽与携带方式。
3. 用 `size1` 作为输出长度的硬校验；对脚本样本验证 `FSLuaInstall` 等关键字是否完整出现。
4. 与 0x0B 解码器共享 token 解析框架，避免重复实现。

#### P2.4 Game.exe 静态分析（与 P2.2/P2.3 并行）

目标：找到 NZ/PAKF 读取与解压函数，直接确认算法，而不是盲猜。

环境说明：REA 6.7.0 已注册并对 Claude Code 可用（当前会话需重连才会出现 MCP 工具，
CLI 可直接使用）；Ghidra 12.1.2 位于 `D:\software\ghidra_12.1.2_PUBLIC`，需要
`GHIDRA_INSTALL_DIR`。详见 `docs/REA-STATUS.md`。

1. 用 Ghidra（或 IDA/REA）打开 `shdata/Game.exe`（只读副本）。
2. 字符串交叉引用：
   - `PAKF3001`、`NZ`、`APCI`、`.apg`、`animation\%d.apg`；
   - `Hero2D`、`FSLua`；
   - PDB 线索：`Hero2dTools`、`VS2013`。
3. 通过导入表/调用点定位文件读取 → 头解析 → `malloc(size1)` → 解压 → 分派的路径。
4. 重点识别：
   - 读取 type 字段后的 `switch`；
   - 0x0A/0x0B 分支各自调用的解压函数；
   - 是否存在标准库（zlib/LZO/LZ4）特征，或自研位流。
5. 输出 `docs/formats/nz.md`：函数地址、伪代码要点、token 语法、边界条件。
6. 若静态分析受阻，再评估受控动态分析（在隔离环境、只读副本、不触碰反外挂）；
   默认不做注入/挂钩。

#### P2.5 校验与回退

- 全量解码统计：成功 / 失败按类型、PAK、扩展名分组。
- 失败样本人工抽查；保证失败原因可解释（截断、未知 type、损坏）。
- 若 codec 短期无法完全破解：优先保证 type=0 全量导出，并记录 0x0A/0x0B 待办，
  不让整条资源线停滞。

验收：抽样 200 个资源（覆盖三种 type、主要扩展名）解码正确；输出长度符合 `size1`；
失败率 < 1%（并列出失败清单）。

### P3 资源格式解析与导出（3–5 天，可与 P4 并行）

按“价值/成本”排序：

#### P3.1 图片：TGA/BMP/JPG（0.5 天）

- type=0 的图片直接导出；压缩类型在 P2 后导出。
- 统一转 PNG，保留原始尺寸/alpha。

#### P3.2 APG 动画 + PIF（1–2 天）

- 解析 `AG` 头与帧索引（偏移/帧号对）；
- 定位 `AP` 块，提取内嵌 JPEG 帧（`FF D8`…`FF D9`）；
- 解析 PIF（`.dat`）中的帧尺寸/锚点/时序；
- 输出：`frames/frame_000.png` + `meta.json`，并合成 GIF/WebM 预览；
- 先做一套角色/NPC 动画作为样板，再批量。

#### P3.3 A16 / CTI / PG2（1–2 天）

- `A16`：从 payload 的 `AP` 块推导宽高、位深、调色板/像素格式；先用抽样试渲染，
  验证颜色（565/555/ARGB 等）后固化。
- `CTI`：`CT` 头 + 调色板表；与 A16/索引图关联。
- `PG2`：`PG` 容器；解析块表，提取内嵌图像；优先处理大图（UI/活动背景）。

#### P3.4 地图：MAP/MAN/CEL/CORE/MSK/MLK（1–2 天）

- `.map` 是 INI（宽高、背景图路径），先做单图预览；
- `.man/.cel/.core` 是结构化网格/对象数据，按头部计数与坐标表解析；
- `.msk` 是遮罩；`.mlk` 是跳转/链接脚本；
- 输出：地图缩略图 + 对象列表 JSON（坐标、类型、资源引用）。

#### P3.5 perinfo.pak / APCI（0.5–1 天）

- 解析 `APCI` 头、名称表、偏移/尺寸表；
- 与 `ui.pak:user/head` 等头像资源关联；
- 输出头像图集与索引。

验收：每个类别至少 1 个可视化成功样例；导出物可被通用软件打开；
有格式文档（`docs/formats/*.md`）和失败样本说明。

### P4 脚本与数据提取分析（2–4 天，可与 P3 并行）

#### P4.1 Lua 源码提取（0.5 天）

- 先导出所有 type=0 Lua（原始 GBK 源码）；
- 检测是否为 Lua 5.1/5.3 源码；用对应 `luac -p` 语法校验；
- 统计模块：`magic/`、`UI/`、`UI2/`、插件 `ui/`；
- 建立调用图：从 `FSLuaInstall`、`FSLuaWndScript.RegMsgCall`、`CallScProcess` 入手。

#### P4.2 FSLua API 与事件（1 天）

- 从源码中提取全局函数、窗口类、事件注册、消息名；
- 输出 `docs/script-api.md`：
  - 窗口生命周期（Open/Close/OnActivityClick 等）
  - UI 控件/事件绑定
  - 与服务端消息分发的接口（只做静态说明）
- 用脚本数据库记录每个函数的定义位置与调用方。

#### P4.3 SCP UI DSL（1 天）

- type=0 的 SCP 直接解析：`WINDOW TYPE`、`NAME`、`RECT`、`SETZORDER`、`END`、
  `#include`、`//` 注释；
- 产出 UI 层级树 JSON（窗口/控件/坐标/资源引用）；
- P2 完成后，对 0x0A/0x0B 的 SCP 套用同一解析器；
- 关联 `UI`/`UI2` 两套布局（推测对应不同分辨率）。

#### P4.4 CSV 数据表（1 天）

- 解析“GBK 字节扩 16 位”的编码：取低字节 → 按 GBK 解码 → 去除 BOM/`NZ` 头残留；
- 统一转 UTF-8 CSV，并按表名建立 SQLite 表（`item`、`monster`、`Skill`、`map` 等）；
- 建立字段字典与外键候选（ID、名称、资源路径）。

#### P4.5 编译/压缩脚本恢复

- P2 完成后，批量解出 0x0A/0x0B 脚本；
- 对比同名的 type=0/源码版本，确认解压结果确为 Lua 源码；
- 若存在真正的 Lua 字节码（`\x1bLua`），按版本使用 `unluac`/`luadec` 类工具，
  不运行脚本。

验收：脚本源码可按目录导出；至少 30 个 UI 脚本的窗口/事件关系可读；
数据表可查询；FSLua API 文档覆盖主要入口。

### P5 工具化与浏览（2–3 天）

- CLI：`python -m dhsh <command>`
  - `paks`：列出归档与条目
  - `extract --pak ui --out ...`：解包
  - `decode --all`：NZ 解码
  - `images` / `animations` / `maps`：导出
  - `scripts`：导出 Lua/SCP、生成 API 索引
  - `db`：构建/查询 SQLite
  - `verify`：校验输出与清单
- 可选：本地 Web 资源浏览器（缩略图 + 路径 + 类型 + 搜索）。
- 所有操作幂等、可并行、失败可续跑。

### P6 验证与交付（1–2 天）

- 全量跑通；生成解码成功率报表与失败清单；
- 抽样人工复核图片/动画/地图/脚本；
- 更新 `CLAUDE.md` 与 `docs/formats/`；
- 记录未解问题与下一步（例如 ANI/effect 细节、加密数据块）。

---

## 5. 里程碑与工作量

| 里程碑 | 内容 | 依赖 | 估算 | 关键产出 |
|---|---|---|---:|---|
| M0 | 基线固化 + 样本库 | — | 0.5 d | `tests/fixtures/`、哈希清单 |
| M1 | PAK 递归解包 + 清单库 | M0 | 1–2 d | `resources.db`、原始导出 |
| M2 | NZ 解码器（0/0A/0B） | M1 + 静态分析 | 3–5 d | `nz.py`、`docs/formats/nz.md` |
| M3 | 图片/APG/PIF 导出 | M2 | 2–3 d | PNG 帧、GIF/WebM 预览 |
| M4 | 地图/APCI/其他格式 | M2 | 2–3 d | 地图预览、头像索引 |
| M5 | Lua/SCP/CSV 脚本分析 | M2（部分可提前） | 2–4 d | 源码导出、API/UI 索引、数据表 |
| M6 | CLI 与浏览器 | M3+M5 | 2–3 d | 一条命令全流程 |
| M7 | 验证与文档 | 全部 | 1–2 d | 报表、格式文档、更新 CLAUDE.md |

单人、熟悉工具的前提下总计约 **2–3 周**。最大不确定性在 M2（NZ codec），
因此 M1 与 type=0 导出应最先完成，保证任何情况下都有可用产出。

---

## 6. 优先级与取舍

| 优先级 | 工作 | 理由 |
|---|---|---|
| P0 | PAK 解包 + type=0 全量导出 | 立即拿到约 38% 资源、Lua 源码、部分图片/地图 |
| P0 | NZ 0x0B/0x0A 解码 | 解锁其余约 62%；影响所有图片/动画/脚本 |
| P1 | APG/PIF 动画 | 资源量最大（458,997 个 APG） |
| P1 | Lua/SCP 源码与数据表 | 直接回答“脚本代码”需求 |
| P2 | TGA/A16/CTI/PG2 图像 | 视觉验证与资源浏览 |
| P2 | 地图预览 | 高价值但结构复杂 |
| P3 | APCI 头像图集 | 范围明确、成本低 |
| P3 | 加密数据块（001.cpm 等） | 用途未知、优先级最低 |

---

## 7. 风险与对策

| 风险 | 概率 | 影响 | 对策 |
|---|---|---|---|
| NZ codec 自研且无公开实现 | 高 | 高 | 静态分析 Game.exe + 已知明文测试 + 位流穷举；先交付 type=0 子集 |
| 反外挂/保护阻碍动态调试 | 高 | 中 | 只做静态分析；不注入、不修改；不运行受保护进程 |
| APG/A16 多版本差异 | 中 | 中 | 按 PAK/版本分组统计，解析器做版本分支；以样本驱动 |
| 脚本语法/引擎版本不确定 | 中 | 中 | 先用 `luac -p` 与源码关键字确认；保留原文 |
| 523k 资源导致 I/O/磁盘压力 | 中 | 中 | SQLite 清单、流式处理、按需导出；不一次性全解压 |
| 文本编码混乱（GBK/宽化） | 高 | 低 | 统一解码工具 + 样本回归测试 |
| 误把推断写成定论 | 中 | 中 | 文档强制三栏：观察/推断/未知；每条结论附样本路径 |

---

## 8. 目录与工具结构（建议）

```
rea-dhsh/
├─ CLAUDE.md
├─ docs/
│  ├─ ROADMAP.md                  ← 本文
│  ├─ formats/
│  │  ├─ pak.md
│  │  ├─ nz.md
│  │  ├─ apg-pif.md
│  │  ├─ images.md
│  │  └─ scripts.md
│  └─ script-api.md
├─ tools/
│  └─ dhsh/
│     ├─ __init__.py
│     ├─ pak.py                   # PAKF3001
│     ├─ nz.py                    # NZ 0x00/0x0A/0x0B
│     ├─ formats/
│     │  ├─ apg.py
│     │  ├─ images.py
│     │  ├─ mapdata.py
│     │  └─ apci.py
│     ├─ scripts/
│     │  ├─ lua_extract.py
│     │  └─ scp.py
│     └─ cli.py
├─ tests/
│  ├─ fixtures/
│  └─ test_pak.py / test_nz.py / ...
├─ manifests/
│  ├─ resources.db
│  ├─ paks.sha256
│  └─ inventory-summary.csv
├─ extracted/
│  ├─ raw/
│  ├─ decoded/
│  └─ export/
└─ notes/
```

---

## 9. 第一批 48 小时执行清单

1. **创建目录与样本库**（P0）。
2. **实现 PAK 解包 + SQLite 清单**（P1），先导出：
   - 全部 type=0 Lua / SCP / CSV / INI / MAP；
   - 全部 type=0 图片与 type=0 APG 样本。
3. **搭建 NZ 测试框架**：输入样本 → 输出解码结果 → 校验 `size1` / magic / 文本可读性。
4. **启动 Game.exe 静态分析**：字符串交叉引用（`PAKF3001`、`NZ`、`APCI`），
   定位读取与解压分支；即使尚未完全解出 codec，也先记录函数地址与伪代码。
5. **产出第一份脚本报告**：从 type=0 Lua 中抽取 `FSLuaInstall` / 事件注册 / UI 窗口清单。
6. **确认 APG 样本可拆帧**：选 1 个 type=0 APG，按 AG/AP/JPEG 结构导出首帧。

48 小时后的预期成果：

- 可浏览的资源清单（按类型/PAK/大小）；
- 一批可直接阅读的 Lua/配置源文件；
- 至少一个动画/图片导出样例；
- NZ codec 的函数线索与测试框架。

---

## 10. 证据与文档规范

- 每条格式结论标注来源：样本路径（PAK + 逻辑路径 + 偏移）、观察字节、推断依据。
- 文档分三节：`Observed`（实测）、`Inferred`（推断）、`Unknown`（未知）。
- 解析器对未知字段保留原始字节，不强行解释。
- 任何批量导出都生成 manifest（输入哈希、输出哈希、工具版本、时间戳）。
- 重要样本放入 `tests/fixtures/` 并附预期结果，作为回归基线。
