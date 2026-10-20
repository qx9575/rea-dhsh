# 资源解包进度

- 更新：2026-10-20 02:40
- 数据源：`D:\software\dhsh`（新水浒Q传 1.50.20）
- 基线：`manifests/nz-stats.json`（523,212 个叶子）、`pak_inventory.json`
- 工具：`tools/dhsh/`（PAK 解析、NZ 统计、按类型/扩展名导出）
- 格式文档：`docs/formats/nz.md`、`docs/formats/a16-cti.md`

## 最新突破

- **NZ type 0x0A = 16 位 LZW**：通过 REA + Ghidra 定位 type 0x0A 解码器
  （VA `0x8a6f60`），实现 `tools/dhsh/nz_lzw10.py` 并接入 `nz.py`。
  B1 **100/100**、B2 **1,000/1,000**、B3 定向 **3,213/3,213**、全量
  **59,222/59,222** 全部通过（0 失败，解码输出 3.34 GiB）。
- **config.pak 0x0A 首批导出**：B0 12/12、B1 100/100、全量 **1,253/1,253**
  通过，输出 14.5 MiB（`extracted/decoded/config/`）。
- **map.pak / map1.pak 0x0A 导出**：B0 25/25、B1 100/100、全量 **365/365**
  通过，输出 69.7 MiB（`extracted/decoded/map/`、`map1/`）。
- **0x0A 解码性能优化**：字典表复用 + 预分配输出 + 可选 numba JIT。
  全量校验 **709.2 s → 25.8 s**，config.pak 端到端导出 **21.7 s → 7.36 s**，
  解码吞吐 1.35 → 77.6 MiB/s。
- **ui.pak 0x0A 导出**：B0 14/14、B1 100/100、全量 **9,375/9,375**
  通过，输出 438.9 MiB（`extracted/decoded/ui/`）；numba 下全量仅 **31.9 s**。
- **npc.* / magic.* 0x0A 导出**：npc 9 个 PAK **4,232/4,232**（589.0 MiB）、
  magic 4 个 PAK **1,062/1,062**（565.1 MiB），全部 0 失败，合计 43.8 s。
- **player.* 0x0A 导出 + 0x0A 全量完成**：player.pak + player1-15
  **42,928/42,928**（APG 41,182、PIF 1,598、TGA 128、CTI 20，1,740.5 MiB，
  162.1 s）；另补齐 800config/plugins 7 个。至此 **0x0A 59,222/59,222 全部导出**。
- **0x0A APG 全量解析扫描**：47,049 个文件、449,011 帧，0 解析错误；
  AP 79,197 / AI 369,814；压缩只用 0/1/2、alpha 只用 0/2；
  **AG flags=1 ⇔ 含 AI 帧**（38,714/38,714）；CTI 覆盖 **100%**
  （369,814 个 AI 帧，0 缺失）；分层 886 帧解码 0 失败。
- **0x0A APG 批量转帧**：47,049 个 APG → **449,011/449,011 张 PNG**，
  3,175.6 MiB，0 错误，**838.6 s**（8 进程，约 535 帧/s）。NumPy 快速路径
  与参考解码器 886/886 帧逐像素一致（解码 4.8×）；PNG level 3。
- **0x0B 全量导出**：LZO1X 增加可选 numba JIT（**171 MiB/s**，约 150×），
  分组并行导出 **214,623/214,623** 个 0x0B 叶子，0 失败，解码 **4,428.5 MiB**；
  player.* 186,487 个 / 1,984.4 MiB / 528.8 s。
- **NZ type 0x0B = LZO1X**：通过 REA + Ghidra 定位 `Game.exe` 的 NZ 解码器
  （VA `0x82d1f0`）与 type 0x0B 解码器（VA `0x8a6c90`），实现
  `tools/dhsh/nz_lzo.py`。样本验证 **133/133**、B1 随机验证 **100/100**。
- **A16 图像解码**：RGB565 + RLE16（compression 2）+ RLE8 alpha（alpha_mode 2）
  已实现（`tools/dhsh/formats/a16.py`）。B0 **23/23**、B1 **100/100** 通过，
  已生成带透明通道的 PNG 样例。
- **CTI**：20/20 头部/尺寸验证；table1/table2 语义待定，但不影响 A16 comp 2。

## 快速状态

- ✅ **已完成**：PAK 清单；NZ 类型全量统计；全部非 APG 的 type=0 资源导出；Lua/SCP/MAP/INI 文本验证；118 个 TGA 与 34 个标准 JPG 可打开；A16 解码器 B0/B1/B2（1,123/1,123）；NZ 0x0A LZW 解码器全量 59,222/59,222；config.pak 0x0A 全量导出 1,253/1,253；map.pak/map1.pak 0x0A 全量导出 365/365；ui.pak 0x0A 全量导出 9,375/9,375；npc.pak+npc1-9 0x0A 全量导出 4,232/4,232；magic.pak+magic1-5 0x0A 全量导出 1,062/1,062；player.pak+player1-15 0x0A 全量导出 42,928/42,928；**0x0A 全量导出完成 59,222/59,222**；**0x0A APG 批量转帧 449,011/449,011 PNG**；**0x0B 全量导出 214,623/214,623（解码 4,428.5 MiB）**；NZ 0x0B LZO1X 解码器 B0/B1/B2（1,233/1,233）；APG B0/B1/B2（181+470+1,000 帧全部解码，含 AI+CTI）；PIF 解析 B0（type 0/0x0B/0x0A）；CTI 头部 20/20；655 个 APG 样本。
- 🟡 **部分完成**：AG offset 辅助语义、PIF 字段语义、CTI table2 选择逻辑待定；特效混合很可能在外部配置；510 个特殊头 JPG 已导出但未解码；A16 少数 comp/alpha 模式待做；0x0A/0x0B 已全量导出，APG 转帧仅完成 0x0A；地图二进制已导出未解析。
- ❌ **未完成**：全量 0x0A/0x0B 批量解码与导出；全量 APG 导出（207,156 个 type 0 + 204,137 个 0x0B + 47,049 个 0x0A）；`perinfo.pak` APCI；加密数据块。

## 总览

| 指标 | 数值 |
|---|---:|
| 顶层 `.pak` | 41 个（40 个 PAKF + `perinfo.pak` APCI；另有 4 个更新补丁包） |
| PAKF 归档 | 40 个 |
| PAK 叶子资源 | 523,212 |
| NZ type 0（可直接导出） | 249,361（47.7%），2.85 GiB |
| NZ type 0x0A（16 位 LZW） | 59,222（11.3%），2.51 GiB |
| NZ type 0x0B（LZO1X） | 214,623（41.0%），3.60 GiB |
| 非 NZ / 零长度 | 6 |
| 已导出文件 | 42,605 个，约 758 MiB（磁盘约 833 MiB） |
| 已解码导出的 type 0 叶子 | 42,205 / 249,361（16.9%） |
| 其中非 APG 的 type 0 | 41,550 / 41,550（**100%**） |
| 其中 APG 的 type 0 | 655 / 207,811（0.3%，仅样本） |
| 0x0A 全量校验 | **59,222/59,222**，0 失败；解码输出 3.34 GiB（8 进程 25.8s，numba；纯 Python 为 709s） |
| 已解码的 0x0B 样本 | 233 + 1,000 = 1,233 个，0 失败 |
| A16 样本验证 | B0 23 + B1 100 + B2 1,000 = 1,123 个，0 失败 |
| APG B0/B1/B2 | B0 13 动画/181 帧；B1 53 动画/470 帧；B2 134 动画/1,000 帧；全部解码；B2 含 AI 257 帧，34/34 CTI 解析成功 |
| 仍被 codec 阻塞的叶子 | 0（0x0A/0x0B 均已具备解码能力） |

> 结论：**所有非 APG 的 type=0 资源已全部导出**；**type 0x0A（16 位 LZW）与
> type 0x0B（LZO1X）编解码均已破解**，但尚未批量导出。

## NZ 类型总览

| 类型 | 叶子数 | 占比 | 字节 | 状态 |
|---|---:|---:|---:|---|
| `0x0000` 原样存储 | 249,361 | 47.7% | 2.85 GiB | 已可导出；非 APG 已全部导出 |
| `0x000A` 16 位 LZW | 59,222 | 11.3% | 2.51 GiB | ✅ 已破解（`0x8a6f60`），B1/B2/B3 全通过 |
| `0x000B` LZO1X | 214,623 | 41.0% | 3.60 GiB | ✅ 已破解（`0x8a6c90`），B1 100/100 |
| 非 NZ / 零长度 | 6 | <0.1% | 0 | 已记录 |

## 分类进度

| 类别 | 叶子总数 | type 0 | type 0A | type 0B | 已导出 type 0 | type 0 剩余 |
|---|---:|---:|---:|---:|---:|---:|
| 角色 | 445,999 | 216,580 | 42,928 | 186,487 | 18,579 | 198,001（APG） |
| UI/图标 | 33,502 | 21,610 | 9,375 | 2,517 | 21,081 | 529（APG） |
| NPC | 30,915 | 8,697 | 4,232 | 17,986 | 258 | 8,439（APG） |
| 地图 | 5,893 | 872 | 365 | 4,655 | 872 | 0 |
| 配置/数据/脚本 | 3,159 | 1,278 | 1,259 | 621 | 1,278 | 0 |
| 技能/特效 | 2,695 | 308 | 1,062 | 1,325 | 121 | 187（APG） |
| UI 背景 | 658 | 5 | 0 | 653 | 5 | 0 |
| 表情动画 | 226 | 9 | 0 | 217 | 9 | 0 |
| 插件/UI | 165 | 2 | 1 | 162 | 2 | 0 |
| **合计** | **523,212** | **249,361** | **59,222** | **214,623** | **42,205** | **207,156** |

## 格式进度

> 0x0A（16 位 LZW）与 0x0B（LZO1X）均已具备解码能力，但尚未批量导出；
> “已导出”列只统计实际导出文件。0x0A 定向验证（B3）覆盖全部 1,945 个 PIF、
> 625 个 SCP、420 个 CSV、174 个 MLK、13 个 PSD 等稀有脚本/数据扩展名。

| 扩展名 | 总数 | type 0 | type 0A | type 0B | 已导出 | 已解析/验证 | 状态 |
|---|---:|---:|---:|---:|---:|---|---|
| `.apg` | 458,997 | 207,811 | 47,049 | 204,137 | 655（样本） | AG/AP/AI；B1 470/470；AI+CTI | 🟡 图像块/单帧可解；metadata/blend 待解 |
| `.a16` | 29,716 | 20,990 | 3,408 | 5,318 | 20,990 | comp2/alpha0,2 B1 100/100 | 🟢 可转 PNG；comp3/4/5/7 待做 |
| `.cti` | 18,387 | 18,292 | 20 | 75 | 18,292 | 头部 20/20 | 🟡 table 语义待定 |
| `.tga` | 6,549 | 118 | 5,317 | 1,114 | 118 | 118 可打开 | 🟡 0x0A/0x0B 可解码；批量导出待做 |
| `.dat` | 4,642 | 54 | 1,945 | 2,643 | 54 | PIF 解析 B0（type 0/0x0A/0x0B） | 🟡 1,945 个 0x0A PIF 全部解压；字段语义待定 |
| `.scp` | 1,940 | 732 | 625 | 583 | 732 | 731 文本可读 | 🟡 0x0A/0x0B 可解码；625 个 0x0A 文本验证；DSL 待解析 |
| `.jpg` | 544 | 544 | 0 | 0 | 544 | 34 标准可打开；510 特殊头 | 🟡 特殊 JPG 待解析 |
| `.csv` | 457 | 0 | 420 | 37 | 0 | 420/420 文本解码 | 🟢 0x0A/0x0B 可解码；GBK/GB18030/UTF-8 混杂 |
| `.map` | 305 | 292 | 0 | 13 | 292 | 文本 INI 验证 | 🟢 单图可用；0x0B 可解码 |
| `.man` | 277 | 172 | 20 | 85 | 172 | 原始字节 | 🟡 0x0A/0x0B 可解码；地图对象解析待做 |
| `.cel` | 275 | 0 | 57 | 218 | 0 | 0x0A 解压样本通过 | 🟡 0x0A/0x0B 可解码；格式解析待做 |
| `.core` | 274 | 32 | 24 | 218 | 32 | 原始字节 | 🟡 0x0A/0x0B 可解码；地图数据解析待做 |
| `.ini` | 231 | 183 | 3 | 45 | 183 | 文本可读 | 🟢 0x0A 3/3（2 个为 `lnk` 别名） |
| `.mlk` | 228 | 52 | 174 | 2 | 52 | `lnk` 头 | 🟡 0x0A 174/174 解压，`lnk` 二进制；跳转解析待做 |
| `.pg2` | 150 | 38 | 106 | 6 | 38 | `PG` 头 | 🟡 0x0A/0x0B 可解码；容器解析待做 |
| `.bmp` | 84 | 0 | 2 | 82 | 0 | — | 🟡 0x0A/0x0B 可解码 |
| `.lua` | 46 | 7 | 18 | 21 | 7 | GBK 源码解码 | 🟢 0x0A/0x0B 可解码；18 个 0x0A 文本验证 |
| `<none>` | 45 | 28 | 0 | 17 | 28 | 多为 CVS 元数据 | 🟢 0x0B 可解码 |
| `.msk` | 26 | 15 | 8 | 3 | 15 | `MSK` magic | 🟡 0x0B 可解码；遮罩解析待做 |
| `.txt` | 22 | 0 | 13 | 9 | 0 | 13/13 文本解码 | 🟢 0x0A/0x0B 可解码 |
| `.psd` | 13 | 0 | 13 | 0 | 0 | 13/13 解压成功 | 🟡 `8BPS` 解析/转码待做 |
| `.db` | 2 | 0 | 0 | 2 | 0 | — | 🟡 0x0B 可解码 |
| `.gal` | 1 | 0 | 0 | 1 | 0 | — | 🟡 0x0B 可解码 |
| `.zip` | 1 | 1 | 0 | 0 | 1 | 有效 ZIP | 🟢 |

另：`perinfo.pak`（7.4 MB）不是 PAKF，而是 `APCI` 容器，尚未解析。

## 已导出目录

| 目录 | 文件数 | 内容 |
|---|---:|---|
| `extracted/easy/` | 1,540 | type 0 文本/配置/脚本（Lua/SCP/INI/MAP/MLK/MAN/CORE/MSK/DAT/ZIP） |
| `extracted/images-standard/` | 662 | TGA/JPG（501.3 MiB） |
| `extracted/images-proprietary/` | 39,282 | type 0 A16/CTI（185.8 MiB） |
| `extracted/samples/apg/` | 655 | type 0 APG 样本（20.4 MiB） |
| `extracted/samples/compressed/` | 400 | 0x0A/0x0B 原始 NZ，供 codec 分析（25.5 MiB） |
| `extracted/decoded/config/` | 1,253 | config.pak 的 0x0A 全量解码（CSV/SCP/Lua/TXT/MLK/PIF/APG/A16/INI，14.5 MiB） |
| `extracted/decoded/map/` + `map1/` | 365 | map.pak/map1.pak 的 0x0A 全量解码（CEL/MAN/core/MSK/TGA/A16/PIF/APG，69.7 MiB） |
| `extracted/decoded/ui/` | 9,375 | ui.pak 的 0x0A 全量解码（TGA/A16/APG/PIF/PG2/PSD/CSV/BMP/TXT/SCP，438.9 MiB） |
| `extracted/decoded/npc*/` | 4,232 | npc.pak + npc1-9 的 0x0A 全量解码（APG/TGA/PIF，589.0 MiB） |
| `extracted/decoded/magic*/` | 1,062 | magic.pak + magic1-5 的 0x0A 全量解码（APG/PIF，565.1 MiB） |
| `extracted/decoded/player*/` | 42,928 | player.pak + player1-15 的 0x0A 全量解码（APG/PIF/TGA/CTI，1,740.5 MiB） |
| `extracted/decoded/800config/` + `plugins/` | 7 | 0x0A 收尾（SCP 6、Lua 1） |
| `extracted/apg_frames/` | 449,011 | 0x0A APG 帧 PNG（449,011 帧，3,175.6 MiB） |
| `extracted/decoded_0b/` | 214,623 | 0x0B 全量解码（APG 204,137、A16 5,318、PIF 2,643、TGA 1,114 等，4,428.5 MiB） |
| `extracted/misc/` | 66 | type 0 PG2 + 无扩展名文件（23.8 MiB） |
| **合计** | **765,461** | **约 11.5 GiB 有效载荷** |

对应清单：

- `manifests/easy-extract.csv`
- `manifests/images-standard.csv`
- `manifests/images-proprietary.csv`
- `manifests/samples-apg.csv`
- `manifests/samples-compressed.csv`
- `manifests/misc.csv`
- `manifests/batches/nz0a-config-b0.csv`、`nz0a-config-b1.csv`、`nz0a-config-full.csv`
- `manifests/batches/nz0a-map-b0.csv`、`nz0a-map-b1.csv`、`nz0a-map-full.csv`
- `manifests/batches/nz0a-ui-b0.csv`、`nz0a-ui-b1.csv`、`nz0a-ui-full.csv`
- `manifests/batches/nz0a-npc-b0.csv`、`nz0a-npc-b1.csv`、`nz0a-npc-full.csv`
- `manifests/batches/nz0a-magic-b0.csv`、`nz0a-magic-b1.csv`、`nz0a-magic-full.csv`
- `manifests/batches/nz0a-player-b0.csv`、`nz0a-player-b1.csv`、`nz0a-player-full.csv`
- `manifests/batches/nz0a-misc-full.csv`
- `manifests/batches/apg-frames-b0.csv`、`apg-frames-b1.csv`、`apg-frames-b1b.csv`、`apg-frames-full.csv`
- `manifests/batches/nz0b-b0-*.csv`、`nz0b-b1-*.csv`、`nz0b-*-full.csv`

## 已验证内容

- **Lua**：7 个 type=0 源码均可按 GBK 解码（如 `magic/magicFormulas.lua`、`UI/main/activebtn.lua`）。
- **MAP**：292 个文本 INI，含 `[base]`/`WIDTH`/`HEIGHT`/`BACKGROUND`。
- **SCP**：731/732 个 type=0 文件可读（`WINDOW TYPE`、`#include`、`//` 注释等）。
- **TGA**：118/118 可被 Pillow 打开。
- **JPG**：34 个标准 JPEG 可打开；510 个特殊头 JPG（地图/小地图/系统等）使用 `FF D8 FF C8` 私有头，Pillow 无法识别。
- **A16/CTI**：20,990 个 A16 全部以 `AP` 开头；18,292 个 CTI 全部以 `CT` 开头。
- **A16 解码**：compression 0/1/2 + alpha_mode 0/2 已实现；B0 23/23、B1 100/100 通过；RGB565 颜色、透明背景与边缘均正确；样例在 `extracted/_batches/s3-b0/`。
- **NZ 0x0B**：LZO1X 解码器已实现（`tools/dhsh/nz_lzo.py`）；样本 133/133、B1 100/100 通过；解压后可得到 A16/APG/TGA/SCP 等原始内容。
- **NZ 0x0A**：16 位 LZW 解码器已实现（`tools/dhsh/nz_lzw10.py`），接入 `nz.py`；B1 100/100、B2 1,000/1,000、B3 3,213/3,213 通过；全量 59,222/59,222 长度/格式校验通过，0 失败，并已全量导出（解码输出 3.34 GiB）。
- **文本编码**：0x0A 文本以 GBK 为主，GB18030 覆盖其扩展区；个别新 UI SCP 为 UTF-8；CP936 独立 `0x80`（欧元）已单独处理；`.mlk`/部分 `.ini` 别名是 `lnk\0` 二进制。
- **CTI**：20/20 头部/尺寸验证；table1=512B、table2=1024B；语义待定。
- **APG**：655/655 样本以 `AG` 开头；0x0A 语料 47,049 文件/449,011 帧全部解析，模式覆盖 100%，CTI 覆盖 100%。
- **ZIP**：1 个有效 ZIP。

## 已知问题

1. **特殊 JPG 私有格式**：510 个文件（地图/小地图/系统背景等）以 `FF D8 FF C8` 开头，不是标准 JPEG；需要按对应图像格式解析（可能与分块/拼图有关）。
2. **0x0A/0x0B 已全量导出**：0x0A 59,222 + 0x0B 214,623，0 失败；剩余工作转为 APG 转帧（type 0/0x0B）、语义解析与容器格式。
3. **A16 少数模式**：compression 3/4/5/7/11、alpha_mode 1/3/4/5 尚未实现。
4. **type 0/0x0B APG 尚未导出/转帧**：0x0A APG 已完成 449,011/449,011 PNG；type 0 APG（207,156 个）与 0x0B APG（204,137 个）仍待导出，0x0B 也尚未批量解压。
5. **perinfo.pak APCI**：头像图集容器未解析。
6. **PAK size=0 别名**：部分条目（如 `.ini`）的 `size=0`，实际数据与同偏移的 `.map` 条目共享；已按 NZ 头 `size2` 解析长度。参见 `tools/dhsh/cli.py` 的 `_leaf_info`。
7. **文本解析**：SCP/Lua/MAP 只是导出与初步验证，尚无完整语义/UI 树解析。
8. **非 PAK 资源**：`sound/`（456 个 wav/mp3）、`font/`、`animation/`、`update/skin/` 尚未建立索引/转码。
9. **非 ASCII 资源名**：PAK 名字段按 Latin-1 解码，GBK 文件名（如 `说明.txt`、`全服梦想奖励.psd`）在导出路径中显示为乱码；文件内容正常。后续可在 `tools/dhsh/pak.py` 用 GBK→Latin-1 fallback 修正并重导受影响的批次。

## 下一批工作（按 `docs/PLAN.md`）

> 严格遵守 `docs/PLAN.md` 的“分批执行规范”：先 10–20 个样本，再 100 → 1,000 → 全量；任何失败率超标立即停止。

1. 0x0A/0x0B 导出已完成（59,222 + 214,623）；下一步为 **0x0B APG 批量转帧**（204,137 个）与 **type 0 APG 导出/转帧**（207,156 个）。
2. S4 APG：0x0A 结构/模式/CTI/转帧已完成；下一步 AG offset 辅助语义、CTI table2。
3. S5：地图特殊 JPG、MAN/CEL/CORE/MSK 解析。
4. PIF 字段语义与 GIF/WebM 生成。

## 更新记录

- 2026-10-19：完成 PAK/NZ 统计（523,212 叶子）；导出全部非 APG 的 type=0 资源；建立 400 个压缩样本；验证文本/图片/动画头；REA + Ghidra 可用。
- 2026-10-19 20:40：破解 NZ 0x0B（LZO1X），实现 `nz_lzo.py`，B1 100/100；实现 A16 RGB565/RLE16/RLE8 alpha 解码，B0 23/23、B1 100/100；CTI 头部 20/20；新增 `docs/formats/nz.md`、`docs/formats/a16-cti.md`。B1 报告：`manifests/batches/s3-b1-validation.json`。
- 2026-10-19 21:10：B2 通过：A16 1,000/1,000、0x0B 1,000/1,000（`manifests/batches/b2-validation.json`）；APG B0 结构通过：13 个动画、181/181 图像块（`manifests/batches/s4-apg-b0.json`、`docs/formats/apg.md`）。APG 的 metadata/delta/blend 语义仍待解析。
- 2026-10-19 21:40：APG B1 通过：53 个动画、470/470 帧；发现 `AI` 块 = RLE8 索引 + CTI，13/13 AI 样本成功解析调色板（`manifests/batches/s4-apg-b1.json`）；特效帧的混合模式、AG metadata、CTI table2 选择仍待解。
- 2026-10-19 22:00：APG B2 通过：134 个动画、1,000/1,000 帧；AP=743、AI=257；34/34 AI 样本 CTI 解析成功（`manifests/batches/s4-apg-b2.json`）。
- 2026-10-19 22:15：APG AG 帧索引结构确认：`u16 frame_id + u16 flags + u32 offset`；offset 指向共享 alpha/命令流位置（可被多帧复用）；已定位 AG 帧迭代 `0x82cd70`、APG 加载器 `0x7f8210`。混合模式与命令流语义仍待解。
- 2026-10-19 22:35：验证 AG offset **不是**每帧 alpha 起点（用它解码会出现拼接/黑块）；每个图像块自己的 s2 起点才是正确 alpha。index flags 全为 0；混合模式很可能来自外部技能/动作配置，而非 APG 文件。静态导帧逻辑保持不变，B1 回归 470/470。
- 2026-10-19 23:00：PIF 解析完成（type 0/0x0B）：standalone 1-3.dat width=44/26/20 与 APG 帧数一致；perinfo.dat width=同目录 APG 动作数（8/2/1 验证）；报告 `manifests/batches/pif-b0.json`，文档 `docs/formats/pif.md`。
- 2026-10-19 23:55：**破解 NZ 0x0A（16 位 LZW）**：实现 `tools/dhsh/nz_lzw10.py` 并接入 `nz.py`；B1 100/100、B2 1,000/1,000、B3 定向 3,213/3,213 通过（含全部 1,945 个 PIF、625 个 SCP、420 个 CSV、174 个 MLK、13 个 PSD）。修正文本校验：GB18030 覆盖 GBK 扩展区、UTF-8 SCP、CP936 独立 0x80、`lnk` 二进制识别。报告 `manifests/batches/nz0a-b1.json`、`nz0a-b2.json`、`nz0a-b3-scripts-pif-v2.json`。
- 2026-10-20 00:15：**config.pak 0x0A 全量导出**：B0 12/12、B1 100/100、全量 1,253/1,253，0 失败，14.5 MiB；输出 `extracted/decoded/config/`，清单 `manifests/batches/nz0a-config-b0.csv`、`nz0a-config-b1.csv`、`nz0a-config-full.csv`。CSV/SCP/Lua/TXT 文本、MLK/INI 的 `lnk` 二进制、APG/A16/PIF 均校验通过。
- 2026-10-20 00:20：**0x0A 全量校验通过**：59,222/59,222 个叶子解码长度等于 `size1`，0 失败；18 种扩展名全部通过（APG 47,049、TGA 5,317、A16 3,408、PIF 1,945、SCP 625、CSV 420 等），解码输出 3.34 GiB；8 进程约 709 秒。报告 `manifests/batches/nz0a-full.json`，工具 `tools/validate_0a.py`。
- 2026-10-20 00:30：**map.pak / map1.pak 0x0A 全量导出**：B0 25/25、B1 100/100、全量 365/365，0 失败，69.7 MiB；输出 `extracted/decoded/map/`、`map1/`，清单 `nz0a-map-b0.csv`、`nz0a-map-b1.csv`、`nz0a-map-full.csv`。magic 规律：`.cel→CEL\0`、`.man→MAN\0`、`.core/.msk→MSK\0`；TGA 141 个 type 2、28 个 type 10（RLE）。
- 2026-10-20 00:40：**0x0A 解码性能优化**：复用模块级 LZW 字典/长度表、按 `size1` 预分配输出并直接倒写，新增可选 numba JIT（`cache=True`，无 numba 时回退纯 Python）。config.pak 纯解码 10.75s → 0.19s（77.6 MiB/s）；全量校验 709.2s → 25.8s；config 端到端导出 21.7s → 7.36s。回归：config+map 1,618 个文件逐字节一致，全量 59,222/59,222 仍 0 失败。
- 2026-10-20 00:50：**ui.pak 0x0A 全量导出**：B0 14/14、B1 100/100、全量 9,375/9,375，0 失败，438.9 MiB，31.9s；输出 `extracted/decoded/ui/`，清单 `nz0a-ui-b0.csv`、`nz0a-ui-b1.csv`、`nz0a-ui-full.csv`。扩展名：TGA 4,596、A16 3,346、APG 1,105、PIF 201、PG2 106、PSD 13、CSV 4、BMP 2、TXT 1、SCP 1。发现非 ASCII 文件名按 Latin-1 呈现的乱码问题（内容正常）。
- 2026-10-20 01:00：**npc.* / magic.* 0x0A 全量导出**：npc 9 个 PAK 4,232/4,232（APG 3,684、TGA 424、PIF 124，589.0 MiB，23.1s）；magic 4 个 PAK 1,062/1,062（APG 1,061、PIF 1，565.1 MiB，15.1s）。B0 各 6/6、B1 各 100/100，全量校验 5,294/5,294 全部通过；输出 `extracted/decoded/npc*/`、`magic*/`，清单 `nz0a-npc-*`、`nz0a-magic-*`。
- 2026-10-20 01:15：**player.* 0x0A 全量导出 + 0x0A 完成**：player.pak + player1-15 42,928/42,928（APG 41,182、PIF 1,598、TGA 128、CTI 20，1,740.5 MiB，162.1s）；补齐 800config/plugins 7 个。B0 6/6、B1 100/100，全量校验 42,928/42,928 全部通过；至此 0x0A 59,222/59,222 全部导出。清单 `nz0a-player-*`、`nz0a-misc-full.csv`。
- 2026-10-20 01:30：**0x0A APG 全量解析扫描**：`tools/scan_apg.py` 扫描 47,049 个 0x0A APG、449,011 帧，0 解析错误；压缩只出现 0/1/2、alpha 只出现 0/2；AP 79,197、AI 369,814；**AG flags=1 ⇔ 含 AI**（38,714/38,714）。`tools/check_apg_cti.py` 验证 369,814 个 AI 帧 CTI **100%** 覆盖（0 缺失）；player/npc/magic/ui 分层 100 文件/886 帧解码 0 失败。报告 `manifests/batches/apg-0a-scan.json`、`apg-cti.json`，文档 `docs/formats/apg.md` 第 7 节。
- 2026-10-20 02:00：**0x0A APG 批量转帧完成**：`tools/export_apg.py` + `tools/dhsh/formats/apg_fast.py`（NumPy RLE/LUT、共享 CTI 索引、8 进程、PNG level 3）导出 47,049 个 APG 的 **449,011/449,011 张 PNG**，3,175.6 MiB，0 错误，838.6s（约 535 帧/s）。快速路径与参考解码器 886/886 帧逐像素一致（4.8×）；B0 20/186、B1 200/1,910、B1b 1,000/9,615 均 0 失败；全量尺寸抽查 304 帧全部匹配。输出 `extracted/apg_frames/`，清单 `apg-frames-full.csv`。
- 2026-10-20 02:40：**0x0B 全量导出完成**：`nz_lzo.py` 增加可选 numba JIT（171 MiB/s，约 150×；numba 缺失时回退纯 Python），分组并行导出 **214,623/214,623** 个 0x0B 叶子，0 长度不匹配、0 失败，解码 **4,428.5 MiB**；其中 player.* 186,487 个/1,984.4 MiB（528.8s），npc 17,986、map 4,655、ui 2,517、magic 1,325、other 1,495、config 158。输出 `extracted/decoded_0b/`，清单 `nz0b-*-full.csv`；B0 35/35、B1 700/700、采样 1,000/1,000 校验通过。

## 复现命令

```bash
cd tools
# 全量类型统计
python -m dhsh stats --out ../manifests/nz-stats.json --summary ../manifests/nz-summary.csv
# 非 APG 的 type=0 文本/数据
python -m dhsh extract --out ../extracted/easy --types 0 \
  --exts .ini .lua .scp .map .mlk .man .core .msk .dat .zip .csv .txt .db \
  --strip-nz --hash --overwrite --manifest ../manifests/easy-extract.csv
# 标准图片
python -m dhsh extract --out ../extracted/images-standard --types 0 \
  --exts .jpg .tga .bmp --strip-nz --overwrite --manifest ../manifests/images-standard.csv
# A16/CTI
python -m dhsh extract --out ../extracted/images-proprietary --types 0 \
  --exts .cti .a16 --strip-nz --overwrite --manifest ../manifests/images-proprietary.csv
# APG 样本与压缩样本
python -m dhsh extract --out ../extracted/samples/apg --types 0 --exts .apg \
  --per-pak-limit 20 --max-size 500000 --strip-nz --overwrite --manifest ../manifests/samples-apg.csv
python -m dhsh extract --out ../extracted/samples/compressed --types 0x0a 0x0b \
  --exts .apg .a16 .tga .scp .lua .csv .cti --per-pak-limit 10 --max-size 300000 \
  --overwrite --manifest ../manifests/samples-compressed.csv
# 0x0A LZW 验证（B1/B2/B3/全量）
python ../tools/probe_0a.py --count 100 --seed 1 --batch b1
python ../tools/probe_0a.py --count 1000 --seed 2 --batch b2
python ../tools/probe_0a.py --count 100000 \
  --exts .csv,.scp,.mlk,.dat,.lua,.txt,.ini,.psd,.bmp --batch b3-scripts-pif-v2
python ../tools/probe_0a.py --count 100000 --types 0x0a --batch full
```
