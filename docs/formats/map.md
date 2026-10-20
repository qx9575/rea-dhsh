# 地图侧文件格式（MAP / CEL / MAN / MSK / core / MLK）

- 日期：2026-10-20
- 目标：`D:\software\dhsh\shdata\Game.exe`
  SHA-256 `f6bf66d24a3523bb6a30541589fabea36bbceec84355463d8e2c99e3b7f16be9`
- 当前实现：`tools/dhsh/formats/map.py`
- 状态：**MAP/CEL/MAN/MSK/core/MLK 头部与表结构全部解析；CEL 已全量导出
  PNG（275/275，0 错误）。MSK/core 数据区像素/图层语义待定。**

## 1. MAP（文本 INI）

`config/map/*.map`，GBK 文本（个别 UTF-8），示例：

```ini
[base]
WIDTH=3072
HEIGHT=2816
BACKGROUND=map1\BangPai_1\BangPai_1.jpg
CElLFILE=map1\BangPai_1\BangPai_1.CEL
MASKFILE=map1\BangPai_1\BangPai_1.MSK
[ext]
ANIFILE=map1\BangPai_1\BangPai_1.MAN
NAME=帮派
MUSIC=19015
```

- `WIDTH/HEIGHT` 为像素尺寸；
- `BACKGROUND` 指向地图背景（多数是特殊 JPG，见 `special-jpg.md`）；
- `CElLFILE`（原文拼写）、`MASKFILE`、`ANIFILE` 指向同图 CEL/MSK/MAN。
- 解析器：`parse_map_ini()`，返回 `{section: {KEY: value}}`。

## 2. CEL（碰撞位图）

解析器 `VA 0x42563d`。

```
0x00  char[4]  "CEL\0"
0x04  uint32   version = 2
0x08  uint32   total_size（等于文件大小）
0x0c  uint32   flags
0x10  uint32   width_cells
0x14  uint32   height_cells
0x18  ...      位图：每格 1 bit，行优先，行跨度 ceil(width/8)
```

- **位序 MSB-first**：每字节最高位为最左格；
- `1 = 阻挡/不可走`，`0 = 可走`；
- 若 `flags & 1`，`width*height/8` 字节的前置图层位于位图之前；
- 格子尺寸：地图像素 / 16（如 1536×1024 → 96×64）。

位序验证：对 80 个 CEL 的行/列边界统计置 1 比例，
MSB-first 为 **98.6%**，LSB-first 为 91.6%，与地图边缘墙体一致。

导出：`tools/export_map_cel.py` → `extracted/map_cel_png/<corpus>/...png`
（白=阻挡，黑=可走）。

## 3. MAN（动画路径表）

解析器 `VA 0x41ee5b`。

```
0x00  char[4]  "MAN\0"
0x04  uint32   version = 4
0x08  uint32   total_size
0x0c  uint32   reserved = 0
0x10  uint32   data_offset = 28
0x14  uint32   entry_size = 100
0x18  uint32   count
0x1c  ...      count × 100 字节、NUL 结尾的路径
```

路径示例：`map\animate\XingXiuCun\DangPu\001`。

## 4. MSK / core（掩码记录）

两者都是 `MSK\0`、version 5 的容器：

```
0x00  char[4]  "MSK\0"
0x04  uint32   version = 5
0x08  uint32   total_size
0x0c  uint32   flags = 0
0x10  uint32   header_size = 40
0x14  uint32   record_size       // MSK 284，core 28
0x18  uint32   record_count
0x1c  uint32   data_offset       // = 40 + record_size * count
0x20  uint32   ?                 // 观察值 24
0x24  uint32   ?                 // 观察值 10
0x28  ...      records
...            data
```

- **MSK**：每条记录 = 256 字节 BMP 名称（如
  `map\tianshangongquetingyuan\cover\001.bmp`）+ 7 × u32；
- **core**：每条记录 = 7 × u32；
- 7 个 u32 的观察值类似 `(480, 431, 568, 1, 3120, 3421, -112)`，
  可能与矩形/偏移/大小/层号有关，语义待定；
- `data_offset` 之后是数据区，长度 = `total_size - data_offset`；
  像素/图层编码尚未解析。

## 5. MLK（地图跳转表）

```
0x00  char[4]  "lnk\0"
0x04  uint32   version = 3
0x08  uint32   total_size
0x0c  uint32   1
0x10  uint32   count
0x14  ...      count × 108 字节记录：
                  u32 value
                  char[104] 目标 .map 名称（NUL 结尾）
```

示例：`YingTianFu.mlk` 24 条记录，目标如 `YTFr_lDJ.map`、
`YTF_DangPu.map`；`value` 为 0/1，可能是单向/双向或启用标志。

## 6. 语料扫描

`tools/scan_maps.py` 扫描 `extracted/easy`、`decoded`、`decoded_0b`：

| 扩展名 | 数量 |
|---|---:|
| `.map` | 305 |
| `.cel` | 275 |
| `.man` | 277 |
| `.msk` | 26 |
| `.core` | 274 |
| `.mlk` | 228 |

**全部通过头部/尺寸/表结构校验，0 错误**（`manifests/batches/map-scan.json`）。

CEL 全量导出：

| 批次 | 掩码 | 格子数 | 输出 | 耗时 |
|---|---:|---:|---:|---:|
| B0 | 10 | 211,648 | 0.02 MiB | 2.2 s |
| B1 | 50 | 1,077,440 | 0.06 MiB | 1.4 s |
| 全量 | **275** | **5,573,952** | **0.32 MiB** | **2.3 s** |

- 0 错误，275 个 PNG 全部落盘、抽查 60 个尺寸匹配；
- 清单：`map-cel-b0.csv`、`map-cel-b1.csv`、`map-cel-full.csv`。

## 7. 待办

1. MSK/core 数据区的图层/像素编码与 7 字段语义；
2. MAN 路径与实际动画文件的关联（供地图动效）；
3. 与 MAP INI / 背景 JPG 合成完整地图预览；
4. MLK `value` 语义与跳转图。
