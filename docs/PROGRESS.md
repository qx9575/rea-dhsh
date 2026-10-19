# 资源解包进度

- 更新：2026-10-19 20:40
- 数据源：`D:\software\dhsh`（新水浒Q传 1.50.20）
- 基线：`manifests/nz-stats.json`（523,212 个叶子）、`pak_inventory.json`
- 工具：`tools/dhsh/`（PAK 解析、NZ 统计、按类型/扩展名导出）
- 格式文档：`docs/formats/nz.md`、`docs/formats/a16-cti.md`

## 最新突破

- **NZ type 0x0B = LZO1X**：通过 REA + Ghidra 定位 `Game.exe` 的 NZ 解码器
  （VA `0x82d1f0`）与 type 0x0B 解码器（VA `0x8a6c90`），实现
  `tools/dhsh/nz_lzo.py`。样本验证 **133/133**、B1 随机验证 **100/100**。
- **A16 图像解码**：RGB565 + RLE16（compression 2）+ RLE8 alpha（alpha_mode 2）
  已实现（`tools/dhsh/formats/a16.py`）。B0 **23/23**、B1 **100/100** 通过，
  已生成带透明通道的 PNG 样例。
- **CTI**：20/20 头部/尺寸验证；table1/table2 语义待定，但不影响 A16 comp 2。
- 仍未破解：**NZ type 0x0A**（59,222 个叶子）。

## 快速状态

- ✅ **已完成**：PAK 清单；NZ 类型全量统计；全部非 APG 的 type=0 资源导出；Lua/SCP/MAP/INI 文本验证；118 个 TGA 与 34 个标准 JPG 可打开；A16 解码器 B0/B1/B2（1,123/1,123）；NZ 0x0B LZO1X 解码器 B0/B1/B2（1,233/1,233）；APG B0/B1/B2（181+470+1,000 帧全部解码，含 AI+CTI）；CTI 头部 20/20；655 个 APG 样本。
- 🟡 **部分完成**：APG 的 AG metadata/delta/blend 语义待解；特效帧可能需要加法混合；510 个特殊头 JPG 已导出但未解码；A16 少数 comp/alpha 模式待做；0x0B 尚未批量导出；地图二进制已导出未解析。
- ❌ **未完成**：NZ 0x0A 解码（59,222 个叶子）；全量 0x0B 批量解码；全量 APG 导出（207,156 个 type 0 + 204,137 个 0x0B）；`perinfo.pak` APCI；加密数据块。

## 总览

| 指标 | 数值 |
|---|---:|
| 顶层 `.pak` | 41 个（40 个 PAKF + `perinfo.pak` APCI；另有 4 个更新补丁包） |
| PAKF 归档 | 40 个 |
| PAK 叶子资源 | 523,212 |
| NZ type 0（可直接导出） | 249,361（47.7%），2.85 GiB |
| NZ type 0x0A（需 token codec） | 59,222（11.3%），2.51 GiB |
| NZ type 0x0B（LZO1X） | 214,623（41.0%），3.60 GiB |
| 非 NZ / 零长度 | 6 |
| 已导出文件 | 42,605 个，约 758 MiB（磁盘约 833 MiB） |
| 已解码导出的 type 0 叶子 | 42,205 / 249,361（16.9%） |
| 其中非 APG 的 type 0 | 41,550 / 41,550（**100%**） |
| 其中 APG 的 type 0 | 655 / 207,811（0.3%，仅样本） |
| 已解码的 0x0B 样本 | 233 + 1,000 = 1,233 个，0 失败 |
| A16 样本验证 | B0 23 + B1 100 + B2 1,000 = 1,123 个，0 失败 |
| APG B0/B1/B2 | B0 13 动画/181 帧；B1 53 动画/470 帧；B2 134 动画/1,000 帧；全部解码；B2 含 AI 257 帧，34/34 CTI 解析成功 |
| 仍被 codec 阻塞的叶子 | 59,222（仅 0x0A） |

> 结论：**所有非 APG 的 type=0 资源已全部导出**；**type 0x0B 编解码已破解**
> （LZO1X），但尚未批量导出；**type 0x0A 仍是唯一未破解的通用 codec**。

## NZ 类型总览

| 类型 | 叶子数 | 占比 | 字节 | 状态 |
|---|---:|---:|---:|---|
| `0x0000` 原样存储 | 249,361 | 47.7% | 2.85 GiB | 已可导出；非 APG 已全部导出 |
| `0x000A` 16 位 token | 59,222 | 11.3% | 2.51 GiB | ❌ codec 未破解（`0x8a6f60`） |
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

> 0x0B 项已具备解码能力（LZO1X），但尚未批量导出；“已导出”列只统计实际导出文件。
> 0x0A 项仍然阻塞；`psd` 全部是 0x0A。

| 扩展名 | 总数 | type 0 | type 0A | type 0B | 已导出 | 已解析/验证 | 状态 |
|---|---:|---:|---:|---:|---:|---|---|
| `.apg` | 458,997 | 207,811 | 47,049 | 204,137 | 655（样本） | AG/AP/AI；B1 470/470；AI+CTI | 🟡 图像块/单帧可解；metadata/blend 待解 |
| `.a16` | 29,716 | 20,990 | 3,408 | 5,318 | 20,990 | comp2/alpha0,2 B1 100/100 | 🟢 可转 PNG；comp3/4/5/7 待做 |
| `.cti` | 18,387 | 18,292 | 20 | 75 | 18,292 | 头部 20/20 | 🟡 table 语义待定 |
| `.tga` | 6,549 | 118 | 5,317 | 1,114 | 118 | 118 可打开 | 🟡 0x0B 可解码；0x0A 待破解 |
| `.dat` | 4,642 | 54 | 1,945 | 2,643 | 54 | 原始字节 | 🟡 0x0B 可解码；PIF 待解析 |
| `.scp` | 1,940 | 732 | 625 | 583 | 732 | 731 文本可读 | 🟡 0x0B 可解码；DSL 待解析 |
| `.jpg` | 544 | 544 | 0 | 0 | 544 | 34 标准可打开；510 特殊头 | 🟡 特殊 JPG 待解析 |
| `.csv` | 457 | 0 | 420 | 37 | 0 | — | 🟡 0x0B 可解码；0x0A 待破解 |
| `.map` | 305 | 292 | 0 | 13 | 292 | 文本 INI 验证 | 🟢 单图可用；0x0B 可解码 |
| `.man` | 277 | 172 | 20 | 85 | 172 | 原始字节 | 🟡 0x0B 可解码；地图对象解析待做 |
| `.cel` | 275 | 0 | 57 | 218 | 0 | — | 🟡 0x0B 可解码；0x0A 待破解 |
| `.core` | 274 | 32 | 24 | 218 | 32 | 原始字节 | 🟡 0x0B 可解码；地图数据解析待做 |
| `.ini` | 231 | 183 | 3 | 45 | 183 | 文本可读 | 🟢 0x0B 可解码 |
| `.mlk` | 228 | 52 | 174 | 2 | 52 | `lnk` 头 | 🟡 0x0B 可解码；地图跳转解析待做 |
| `.pg2` | 150 | 38 | 106 | 6 | 38 | `PG` 头 | 🟡 0x0B 可解码；0x0A/容器待解析 |
| `.bmp` | 84 | 0 | 2 | 82 | 0 | — | 🟡 0x0B 可解码；0x0A 待破解 |
| `.lua` | 46 | 7 | 18 | 21 | 7 | GBK 源码解码 | 🟢 7 个可读；0x0B 可解码；0x0A 待破解 |
| `<none>` | 45 | 28 | 0 | 17 | 28 | 多为 CVS 元数据 | 🟢 0x0B 可解码 |
| `.msk` | 26 | 15 | 8 | 3 | 15 | `MSK` magic | 🟡 0x0B 可解码；遮罩解析待做 |
| `.txt` | 22 | 0 | 13 | 9 | 0 | — | 🟡 0x0B 可解码；0x0A 待破解 |
| `.psd` | 13 | 0 | 13 | 0 | 0 | — | ❌ 全部 0x0A，待 codec |
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
| `extracted/misc/` | 66 | type 0 PG2 + 无扩展名文件（23.8 MiB） |
| **合计** | **42,605** | **约 758 MiB 有效载荷** |

对应清单：

- `manifests/easy-extract.csv`
- `manifests/images-standard.csv`
- `manifests/images-proprietary.csv`
- `manifests/samples-apg.csv`
- `manifests/samples-compressed.csv`
- `manifests/misc.csv`

## 已验证内容

- **Lua**：7 个 type=0 源码均可按 GBK 解码（如 `magic/magicFormulas.lua`、`UI/main/activebtn.lua`）。
- **MAP**：292 个文本 INI，含 `[base]`/`WIDTH`/`HEIGHT`/`BACKGROUND`。
- **SCP**：731/732 个 type=0 文件可读（`WINDOW TYPE`、`#include`、`//` 注释等）。
- **TGA**：118/118 可被 Pillow 打开。
- **JPG**：34 个标准 JPEG 可打开；510 个特殊头 JPG（地图/小地图/系统等）使用 `FF D8 FF C8` 私有头，Pillow 无法识别。
- **A16/CTI**：20,990 个 A16 全部以 `AP` 开头；18,292 个 CTI 全部以 `CT` 开头。
- **A16 解码**：compression 0/1/2 + alpha_mode 0/2 已实现；B0 23/23、B1 100/100 通过；RGB565 颜色、透明背景与边缘均正确；样例在 `extracted/_batches/s3-b0/`。
- **NZ 0x0B**：LZO1X 解码器已实现（`tools/dhsh/nz_lzo.py`）；样本 133/133、B1 100/100 通过；解压后可得到 A16/APG/TGA/SCP 等原始内容。
- **CTI**：20/20 头部/尺寸验证；table1=512B、table2=1024B；语义待定。
- **APG**：655/655 样本以 `AG` 开头。
- **ZIP**：1 个有效 ZIP。

## 已知问题

1. **特殊 JPG 私有格式**：510 个文件（地图/小地图/系统背景等）以 `FF D8 FF C8` 开头，不是标准 JPEG；需要按对应图像格式解析（可能与分块/拼图有关）。
2. **NZ 0x0A codec 未破解**：59,222 个叶子仍未解码（11.3%），包括 420 个 CSV、部分 SCP/Lua/TGA/A16/APG。
3. **0x0B 尚未批量解码**：214,623 个叶子（3.60 GiB）已具备解码能力，但只验证了 233 个样本。
4. **A16 少数模式**：compression 3/4/5/7/11、alpha_mode 1/3/4/5 尚未实现。
5. **APG 仅导出样本**：207,156 个 type=0 APG 尚未导出；0x0B APG 也待批量。
6. **perinfo.pak APCI**：头像图集容器未解析。
7. **PAK size=0 别名**：部分条目（如 `.ini`）的 `size=0`，实际数据与同偏移的 `.map` 条目共享；已按 NZ 头 `size2` 解析长度。参见 `tools/dhsh/cli.py` 的 `_leaf_info`。
8. **文本解析**：SCP/Lua/MAP 只是导出与初步验证，尚无完整语义/UI 树解析。
9. **非 PAK 资源**：`sound/`（456 个 wav/mp3）、`font/`、`animation/`、`update/skin/` 尚未建立索引/转码。

## 下一批工作（按 `docs/PLAN.md`）

> 严格遵守 `docs/PLAN.md` 的“分批执行规范”：先 10–20 个样本，再 100 → 1,000 → 全量；任何失败率超标立即停止。

1. S3/S6 B2 已完成（各 1,000/1,000）；下一批为全量 A16 转 PNG 与 0x0B 批量导出（仍按 PAK/扩展名分片）。
2. S4 APG：B2 已完成（134 动画/1,000 帧）；下一步解析 AG metadata/blend、PIF，再全量导出。
3. S5：地图特殊 JPG、MAN/CEL/CORE/MSK 解析。
4. S7：0x0A token codec（先 20 个脚本样本）。

## 更新记录

- 2026-10-19：完成 PAK/NZ 统计（523,212 叶子）；导出全部非 APG 的 type=0 资源；建立 400 个压缩样本；验证文本/图片/动画头；REA + Ghidra 可用。
- 2026-10-19 20:40：破解 NZ 0x0B（LZO1X），实现 `nz_lzo.py`，B1 100/100；实现 A16 RGB565/RLE16/RLE8 alpha 解码，B0 23/23、B1 100/100；CTI 头部 20/20；新增 `docs/formats/nz.md`、`docs/formats/a16-cti.md`。B1 报告：`manifests/batches/s3-b1-validation.json`。
- 2026-10-19 21:10：B2 通过：A16 1,000/1,000、0x0B 1,000/1,000（`manifests/batches/b2-validation.json`）；APG B0 结构通过：13 个动画、181/181 图像块（`manifests/batches/s4-apg-b0.json`、`docs/formats/apg.md`）。APG 的 metadata/delta/blend 语义仍待解析。
- 2026-10-19 21:40：APG B1 通过：53 个动画、470/470 帧；发现 `AI` 块 = RLE8 索引 + CTI，13/13 AI 样本成功解析调色板（`manifests/batches/s4-apg-b1.json`）；特效帧的混合模式、AG metadata、CTI table2 选择仍待解。
- 2026-10-19 22:00：APG B2 通过：134 个动画、1,000/1,000 帧；AP=743、AI=257；34/34 AI 样本 CTI 解析成功（`manifests/batches/s4-apg-b2.json`）。
- 2026-10-19 22:15：APG AG 帧索引结构确认：`u16 frame_id + u16 flags + u32 offset`；offset 指向共享 alpha/命令流位置（可被多帧复用）；已定位 AG 帧迭代 `0x82cd70`、APG 加载器 `0x7f8210`。混合模式与命令流语义仍待解。

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
```
