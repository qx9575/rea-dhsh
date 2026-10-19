# 资源分析阶段计划（由简入难）

- 目标：`D:\software\dhsh`（新水浒Q传 1.50.20）
- 原则：**先做不需要破解压缩、格式标准或纯文本的资源**；每一阶段都能独立产出，不把全部工作压在一个未知 codec 上。
- 配套进度：`docs/PROGRESS.md`
- 技术细节：`docs/ROADMAP.md`

## 排序原则

1. **无需 NZ 解码**（type=0）优先。
2. **标准格式**（TGA/JPG/BMP/INI/CSV 文本/Lua 源码）优先于自研格式。
3. **结构简单、样本多**的格式优先（CTI、A16、APG 头）。
4. **NZ 0x0B → 0x0A**：难度高，但收益最大，放在简单资源之后、复杂容器之前。
5. **PG2/APCI/加密块** 最后处理。

## 阶段总表

| 阶段 | 难度 | 内容 | 前置 | 当前状态 | 预计 |
|---|---|---|---|---|---|
| S0 | ★ | 环境、PAK 解析、资源清单、工具链 | — | ✅ 完成 | 0.5 d |
| S1 | ★ | type=0 文本/配置/脚本导出 | S0 | ✅ 完成 | 0.5 d |
| S2 | ★★ | 标准图片（TGA/JPG/BMP）导出与验证 | S0 | 🟡 部分完成（map JPG 特殊） | 0.5–1 d |
| S3 | ★★ | type=0 专有图像 A16/CTI 解析 | S0 | 🟢 B2 1,000/1,000，可批量转 PNG | 1–2 d |
| S4 | ★★★ | type=0 APG/PIF 动画解析与全量导出 | S0/S3 | 🟢 B2 134 动画/1,000 帧；AI+CTI；PIF B0 | 2–3 d |
| S5 | ★★★ | 地图数据（MAP/MAN/CEL/CORE/MSK/MLK + 特殊 JPG） | S1/S2 | 🟡 文本部分完成 | 2–3 d |
| S6 | ★★★★ | NZ 0x0B codec（LZO1X） | S0 + Ghidra | 🟢 B2 1,000/1,000，可批量 | 3–5 d |
| S7 | ★★★★☆ | NZ 0x0A 16 位 LZW codec | S6/静态分析 | ✅ 完成（B1/B2/B3 全通过） | 2–4 d |
| S8 | ★★★★ | 压缩资源批量导出与格式解析 | S6/S7 | 🟢 0x0A/0x0B 均可批量，待导出 | 3–5 d |
| S9 | ★★★★☆ | PG2 / APCI / 复杂容器 | S2/S8 | ❌ 未开始 | 2–3 d |
| S10 | ★★★★★ | 加密/未知数据块 | 需要额外线索 | ❌ 未开始 | 未知 |

难度星级只表示“实现与逆向成本”，不代表收益。**S6/S7 均已破解**（0x0B=LZO1X、0x0A=16 位 LZW，见 `docs/formats/nz.md`），全部 273,845 个压缩叶子都已具备解码能力，下一步是分批导出。

## 分批执行规范（强制）

**不要一次性跑全量。** 任何新解析器/解码器都按以下节奏推进：

| 批次 | 规模 | 验证方式 | 通过条件 |
|---|---:|---|---|
| B0 | 10–20 个样本 | 人工检查 + magic/尺寸/文本 | 无错误 |
| B1 | 100 个 | 自动校验 + 抽样人工 | 失败 ≤ 1% |
| B2 | 1,000 个 | 自动校验 + 失败清单 | 失败 ≤ 1% |
| B3 | 单个 PAK / 单个类别 | 自动校验 + 与统计交叉核对 | 失败 ≤ 1% |
| B4 | 全量 | 全量报表 + 抽样复核 | 失败 ≤ 1% |

配套要求：

- 每批使用独立的 `--out`、`--manifest`（例如 `manifests/batches/s3-a16-b1.csv`）。
- 批量命令必须带 `--per-pak-limit` / `--limit` / `--max-size`，先 `--dry-run` 检查选择集。
- 新 codec 先建立 20–100 对“已知明文/压缩”样本，输出长度必须等于 `size1`。
- 失败样本保留在 `extracted/_failed/` 与失败清单中，不删除，便于回归。
- 仅当上一批通过后，才进入下一批；**任何失败率超标都停止并修正解析器**。

标准命令示例：

```bash
# B0：跨 10 个 PAK 各取 1 个 A16 样本（最多 10 个）
python -m dhsh extract --out ../extracted/_batches/s3-a16-b0 \
  --types 0 --exts .a16 --per-pak-limit 1 --limit 10 --max-size 200000 \
  --strip-nz --dry-run
# 去掉 --dry-run 执行并生成 manifest
python -m dhsh extract --out ../extracted/_batches/s3-a16-b0 \
  --types 0 --exts .a16 --per-pak-limit 1 --limit 10 --max-size 200000 \
  --strip-nz --manifest ../manifests/batches/s3-a16-b0.csv
```

---

## S0 环境与清单（已完成）

产出：

- `tools/dhsh/`：PAK 解析、NZ 头解析、统计与导出 CLI。
- `manifests/nz-stats.json` / `nz-summary.csv`：523,212 个叶子的类型与大小统计。
- `pak_inventory.json`：全量路径索引。
- REA 6.7.0 + Ghidra 12.1.2 可用（见 `docs/REA-STATUS.md`）。

验收：45 个 `.pak` 中，41 个顶层（40 个 PAKF + `perinfo.pak` APCI）+ 4 个更新补丁包；
40 个 PAKF 共 523,212 个叶子；`perinfo.pak` 单独记录为 APCI。

## S1 type=0 文本/配置/脚本（已完成）

内容：无需解压的 `.lua`、`.scp`、`.ini`、`.map`、`.mlk`、`.man`、`.core`、`.msk`、`.dat`、`.zip`、无扩展名文件。

已完成：

- 导出 1,540 个文件到 `extracted/easy/`。
- 7 个 Lua 源码可按 GBK 解码；292 个 `.map` 为文本 INI；731/732 个 SCP 为文本 DSL。
- 183 个 `.ini`、172 个 `.man`、52 个 `.mlk`、54 个 `.dat`、32 个 `.core`、15 个 `.msk`、1 个 `.zip`。

下一步（属于 S5/S8）：

- SCP 文本 DSL → UI 层级树；
- Lua 源码 → 模块/API/事件索引；
- `.map`/`.mlk`/`.man`/`.core`/`.msk` → 地图对象与图层。

## S2 标准图片（部分完成）

内容：TGA/JPG/BMP。

已完成：

- 导出 662 个文件（501.3 MiB）到 `extracted/images-standard/`。
- 118 个 TGA 全部可被 Pillow 打开；34 个普通 JPG 可打开。
- 510 个特殊头 JPG（地图/小地图/系统等）使用 `FF D8 FF C8`，Pillow 无法识别，归入 S5。

下一步：

- 统计标准 JPG/TGA 的尺寸与用途；
- 为 510 个特殊头 JPG 编写解析器（见 S5）。

## S3 A16/CTI 图像（B0/B1 通过）

内容：Hero2D 的 16 位图像与调色板。格式细节见 `docs/formats/a16-cti.md`。

已完成：

- 导出 39,282 个文件（185.8 MiB）到 `extracted/images-proprietary/`：
  - `.a16` 20,990 个，全部以 `AP` 开头；
  - `.cti` 18,292 个，全部以 `CT` 开头。
- **A16 解码器**：RGB565 + RLE16（compression 2）+ RLE8 alpha（alpha_mode 2）；
  B0 23/23、B1 100/100 通过，已生成带透明通道的 PNG 样例。
- **CTI**：20/20 头部/尺寸验证（20 + 512 + 1024 字节）；table 语义待定。

下一步（严格分批）：

1. B2：1,000 个 A16 转 PNG；
2. 通过后全量转换 20,990 个 type=0 A16；
3. 处理 compression 3/4/5/7/11、alpha_mode 1/3/4/5；
4. 再处理 0x0B A16（5,318 个）与 0x0A A16（3,408 个）。

验收：至少 10 组 A16/CTI 能正确还原颜色；导出工具可批量运行。

## S4 APG/PIF 动画（B0 结构通过）

内容：`AG` 动画包与 `PIF` 元数据。格式细节见 `docs/formats/apg.md`。

已完成：

- 抽取 655 个 type=0 APG 样本（20.4 MiB）到 `extracted/samples/apg/`；
- 655/655 以 `AG` 开头；
- 确认 AG 头（header_size/frame_count/index）、AP/AI 图像块、A16 头、JPEG 帧；
- B0：13 个动画（含 `animation/1-3.apg`）、**181/181** 图像块解码成功；
- B1：53 个动画、**470/470** 帧解码成功；magic AP=386、AI=84；
- B2：134 个动画、**1,000/1,000** 帧解码成功；magic AP=743、AI=257；34/34 AI 调色板解析成功；
- **AI 块 = RLE8 索引 + CTI**：含 AI 的样本全部找到 `clrtb.cti`（missing=0），
  宠物/头像颜色正确；
- comp 2=991、1=9；alpha 2=1,000（B2）；
- 输出 `extracted/_batches/s4-apg-b0/`（PNG 帧 + contact sheet），报告
  `manifests/batches/s4-apg-b1.json` / `s4-apg-b2.json`；
- PIF B0：standalone `1-3.dat` 的 width=44/26/20 与 APG 帧数一致；
  `perinfo.dat` width=同目录 APG 动作数（8/2/1 验证）；type 0/0x0B 已解析，
  23 个 0x0A PIF 被阻塞（`docs/formats/pif.md`、`manifests/batches/pif-b0.json`）。

未解决：

- AG offset 的辅助语义（时序/锚点/共享子帧）仍待确认；已验证它不是每帧 alpha 起点，
  当前静态导帧忽略它；
- 特效混合很可能由外部技能/动作配置决定，APG 文件内未发现 blend 标志；
- CTI table1/table2 的选择逻辑；
- PIF `.dat` 尚未解析。
- 已定位 APG 加载器 `0x7f8210`、AG 帧迭代 `0x82cd70`，作为后续深入入口。

下一步：

1. B2：1,000 帧 APG + CTI 解析回归；
2. 用 REA + Ghidra 定位 APG 加载/播放代码，解析 AG index/metadata 与 blend；
3. 解析 PIF，生成 GIF/WebM；
4. 全量导出 type=0 APG（207,156）与 0x0B APG（204,137）。

验收：至少一套角色/NPC 动画可播放；导出器可批量运行。

## S5 地图数据（部分完成）

内容：`.map`（文本 INI）、`.man`/`.cel`/`.core`/`.msk`、`.mlk`、特殊头 JPG。

已完成：

- 292 个 `.map` 文本 INI；
- 172 个 `.man`、32 个 `.core`、15 个 `.msk`、52 个 `.mlk`（原始字节）；
- 510 个特殊头 JPG（地图/小地图/系统等）已导出，但未解码。

下一步：

1. 解析 MAN/CEL/CORE/MSK 头部计数、网格/对象/碰撞表；
2. 解析特殊 JPG 的 `FFD8 FFC8` 头（宽度、高度、分块、压缩方式）；
3. 将地图背景、遮挡、碰撞拼合为预览图；
4. 关联 `.map` 中的 BACKGROUND 路径与资源。

验收：至少一张地图可生成缩略图 + 对象列表。

## S6 NZ 0x0B codec（已破解：LZO1X）

规模：214,623 个叶子，3.60 GiB；占全部叶子 41.0%。格式细节见 `docs/formats/nz.md`。

已完成：

- 定位 `Game.exe` NZ 通用解码器（VA `0x82d1f0`）与 type 0x0B 解码器（VA `0x8a6c90`）；
- 确认算法为 **LZO1X**，实现 `tools/dhsh/nz_lzo.py`；
- 样本 133/133、B1 100/100 解压成功；输出长度等于 `size1`，A16/APG/TGA/SCP magic 检查通过。

下一步：

1. ~~B2：1,000 个 0x0B 叶子解码~~（已完成 1,000/1,000）；
2. 按 PAK/扩展名分批导出 214,623 个 0x0B；
3. 对解压后的 APG/TGA/A16/SCP 复用对应格式解析器；
4. 保留失败样本与长度校验。

## S7 NZ 0x0A codec（已完成：16 位 LZW）

规模：59,222 个叶子，2.51 GiB；占 11.3%。格式细节见 `docs/formats/nz.md`。

已完成：

- 定位 `Game.exe` type 0x0A 解码器（VA `0x8a6f60`）；确认算法为**按 16 位码读取的经典 LZW**：
  `0x0000–0x00FF` 字面量、`0x0100` 结束、`0x0101+` 动态字典；
- 实现 `tools/dhsh/nz_lzw10.py` 并接入 `tools/dhsh/nz.py`；
- 样本 267/267、B1 100/100、B2 1,000/1,000、B3 定向 3,213/3,213、全量
  **59,222/59,222** 全部通过；输出长度严格等于 `size1`，0 失败；
- 1,945 个 0x0A PIF、13 个 PSD、420 个 CSV、625 个 SCP、174 个 MLK 全部解压成功；
- 文本编码确认：GBK 为主，GB18030 覆盖扩展区，个别 SCP 为 UTF-8，CP936 独立 `0x80` 单独处理。

下一步：

1. 按 PAK/扩展名分批导出 59,222 个 0x0A；
2. 对解压后的 APG/A16/TGA/PIF 复用对应格式解析器；
3. 保留失败样本与长度校验。

## S8 压缩资源批量导出与解析（未开始）

内容：S6/S7 成功后，批量导出：
- 204,137 个 0x0B APG、47,049 个 0x0A APG；
- 5,318 个 0x0B A16、3,408 个 0x0A A16；
- 420 个 0x0A CSV、625 个 0x0A SCP、18 个 0x0A Lua 等；
- 各扩展名的 0x0B 版本。

产出：

- `extracted/decoded/` 全量资源；
- 统一清单与校验和；
- 复用 S3–S5 的格式解析器做批量转换。

验收：解码成功率 > 99%；全量导出可重复执行。

## S9 PG2 / APCI / 复杂容器（未开始）

- `.pg2` 150 个（38 个 type=0 已导出，112 个待解码）；
- `perinfo.pak` 的 `APCI` 头像图集；
- 解析块表、尺寸、调色板、图像数据。

验收：PG2 可转 PNG；APCI 可导出头像图集与索引。

## S10 加密/未知数据块（未开始）

`001.cpm`、`ccore.dat`、`cltgp.dat`、`sl.dat`、`ACDC.dat`（gzip）等。先确认用途，再决定是否值得逆向。

---

## 建议的下一步

1. **S3/S6/S7 全量导出**：A16 20,990 个转 PNG；0x0A/0x0B 共 273,845 个按 PAK/扩展名分片导出。
2. **S4**：PIF 字段语义与 AG metadata；随后 APG 批量导出。
3. **S5**：地图特殊 JPG、MAN/CEL/CORE/MSK 解析。
4. **S8**：先跑 0x0A 全量长度校验，再开始 0x0A/0x0B 批量导出。
5. 每次完成一批，更新 `docs/PROGRESS.md` 与对应 `docs/formats/*.md`。
