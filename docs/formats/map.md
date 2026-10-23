# 地图侧文件格式（MAP / CEL / MAN / MSK / core / MLK）

- 日期：2026-10-23
- 目标：`D:\software\dhsh\shdata\Game.exe`
  SHA-256 **`7a5e2c085ac79557ca104a2917e06f1a7e5f2827e4b68753bbaf1c754a4e78dc`**
  （2026-10-21 客户端更新；旧分析基线 `f6bf66d2…`）。**注意：Game.exe 可能在
  每次运行后被启动器/保护组件改写；运行后先跑 `tools/snapshot_gameexe.py`，
  以 pinned 快照和地址字节差异为准。**
- 当前实现：`tools/dhsh/formats/map.py`
- 状态：**MAP/CEL/MAN/MSK/core/MLK 全部解析；MSK/core 数据区编码已破解，
  300/300 文件、12,755 个遮罩、717,967,584 像素、0 错误；全量导出
  12,755 张 PNG（15.85 MiB）。消费端语义已在旧基线静态分析、并在当前
  二进制复核关键行为；地址以当前 SHA 为准（§4.7）。**

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

## 4. MSK / core（前景遮罩）

### 4.1 加载链

- 地图加载器读取 `MASKFILE`（INI 中写作 `...MSK`），调用 `0x27aa0`；
- `0x27aa0` 通过 `0xb580` 把路径扩展名替换成 `.core`（不是比较），
  再调用解析器 `0x274b0`（magic `MSK\0`、version 5）；
- 因此**当前客户端实际加载的是同目录的 `.core`**；`.MSK` 是旧格式：
  26 个文件中大多数是 40 字节空文件（`record_count=0`），只有
  `LiangShanShanJiao`（7 条）、`QYQXHaoZhai`（8 条）、`TXWGHaoZhai`
  （3 条）等含记录；其记录带 256 字节 BMP 名称
  （如 `map\liangshanshanjiao\cover\00001.bmp`）。当前语料中只有
  `FuXianDao/cover/` 还保留 82 个 1bpp BMP，其余 MSK 名称指向的文件
  已不存在。

### 4.2 容器头

```text
0x00  char[4]  "MSK\0"
0x04  u32      version = 5
0x08  u32      total_size（等于文件大小）
0x0c  u32      flags = 0
0x10  u32      header_size = 40
0x14  u32      record_size（core 28；MSK 284）
0x18  u32      record_count
0x1c  u32      data_offset = 40 + record_size * record_count
0x20  u32      placement_size = 24
0x24  u32      placement_count
```

### 4.3 记录表（`header_size` 起，`record_size` 字节/条）

| 字段 | 含义 | 证据 |
|---|---|---|
| `name[record_size-28]` | BMP 路径；core 无此字段 | MSK 记录 |
| u32 f0 | 遮罩宽度 `width` | 与 RLE 行和、运行时对象 +0x08 一致 |
| u32 f1 | 遮罩高度 `height` | 与行数、运行时 +0x0c 一致 |
| s32 f2 | `base_y`（地线截距） | 运行时 `sortY = y + f2 - (x*f6>>8)` |
| u32 f3 | 类别（观测 1 为主，少量 2..10） | 语义待定 |
| u32 f4 | **绝对文件偏移**（blob 起点） | 解析器 `obj+0x24 = file + f4` |
| u32 f5 | blob 字节长度 | 相邻记录 `f4+f5` 连续，末条到 EOF |
| s32 f6 | Q8.8 地线斜率 | 常见 0/±128；见 4.5 |

- blob 位于放置表之后，按记录顺序连续排列；300/300 文件的
  `f4 >= placement_end`、`f4+f5 <= file_size`，末条恰好到文件末尾。

### 4.4 放置表（`data_offset` 起，24 字节/条）

```text
u32 record_index    // < record_count
s32 x               // 遮罩左上角地图像素 X
s32 y               // 遮罩左上角地图像素 Y
u32 flag            // 1 = 静态遮罩；0 = 组/动画成员
u32 group           // 组 id（0 = 无）
u32 frame           // 组内帧/部件序号
```

- `x/y` 是有符号 32 位；运行时对象逐字段对照 563/563 一致（含负坐标）。
- `flag=1` 占 16,009/16,114 条；`flag=0` 仅出现在 `BangPai_1`（105 条）。
  BangPai_1 的组内帧同 x、底边对齐、高度递增（group 7：
  167/374/408/458/457 px，底边 5091..5104），形态为树木/喷泉类
  **生长动画帧**；`group` 为组 id、`frame` 为帧序号（强推断，
  消费函数尚未定位）。
- 其余 `group/frame` 均为 0；`flag/p4/p5` 的消费端语义仍待反汇编确认。

### 4.5 数据区 RLE（已完全破解）

```text
u16 word = (covered << 15) | run_length
```

- `covered`：1 = 遮罩覆盖像素（前景物件轮廓），0 = 透明；
- `run_length`：1..32767，**不跨扫描行**；
- 逐行解码，每行 run 长度之和等于 `width`，总行数 `height`；
- 解码后与地图背景叠加：遮罩轮廓与树、建筑、人物、浮空岛等物件
  **逐像素吻合**（`extracted/_probe_core/FuXianDao_core_overlay_real.png`）。

全量校验（`tools/dhsh/formats/map.py` + `decode_mask`）：

| 指标 | 数值 |
|---|---:|
| 文件 | **300/300**（274 core + 26 MSK，0 错误） |
| 记录/遮罩 | **12,755** |
| 像素 | **717,967,584** |
| RLE run | **11,421,604** |
| 行/长度校验失败 | **0** |

字段语义（明确区分观测与推断）：

- **观测**：解析器 `0x274b0` 把 f2/f6 合成为
  `obj+0x10 = y + f2 - ((x*f6)>>8)`；运行时 563/563 对象验证该公式，
  其中 146 个记录 `f6 != 0`。
- **观测**：f6 非零值显著聚集在 ±128（64.7% 在 ±(128±48)，
  另有 ±32/±64/±96 次峰），即 Q8.8 下约 0.5 的斜率，符合 2:1
  等距视角的物件地线方向。
- **强推断**：f2 为地线截距、f6 为地线斜率；`obj+0x10` 是该物件的
  深度/排序基准（Y-sort），用于决定前景遮罩与角色的前后关系。
  尚未定位读取 `obj+0x10`/`obj+0x14` 的消费函数，故不作定论。
- **观测**：f3 以 1 为主（11,759/12,755），另有 2..10；语义未定。
- **观测**：运行时 `obj+0x28` 是从 802 起递增的运行时 id（`call
  [0x97d134]` 的返回值），不是文件字段，不参与导出。

### 4.7 消费端语义（静态反汇编 + 运行时只读验证）

> **版本基线**：消费端分析最初基于旧 SHA `f6bf66d2…`；2026-10-21 客户端更新为
> `7a5e2c08…`。2026-10-23 已在当前二进制复核主链路：
> 解析器 **0x4272d0**、加载器 **0x4278c0**、ApplyMask **0x4266d0**、
> 地线深度测试 **0x426756**、暗化 **0x4264b0**、group/frame **0x426980**、
> vtable 包装 **0x41e760**。下文若仍出现旧子地址（`0x4269xx`/`0x426bxx` 等），
> 属于旧基线证据，迁移时以当前二进制为准。
> 证据：旧基线 Ghidra 批次 `.rea/kimi_batch*.json` 与 `.rea/kimi_*.c`；
> 持久项目 `ghidra-proj/dhsh` 仍为旧 SHA；当前 SHA 已导入新项目
> `ghidra-proj/dhsh-7a5e`（程序名 `Game-7a5e…exe`），并用
> `kimi_ghidra_batch.py` 复核了以下函数。**当前 vtable 0x9809f8 实测**：
> `+0x00=0x421d10`（地图 ctor）、`+0x18=0x41e760`（ApplyMask）、
> `+0x1c=0x41e780`（restore → `0x426580` → `0x425ea0`）、
> `+0x20=0x41e790`（clear → `0x426570` → `0x425d50`），与旧基线一一对应。

**容器归属**：地图对象 `map+0x40` = 遮罩容器指针（`operator_new(0x148)` +
ctor `0x427390`，由地图 ctor `0x421d10` 创建；dtor `0x421b90` → `0x427250`）。
`DAT_00ab91b0` 是**指向 app 大结构的全局指针**（该结构在 WinMain 栈上，
运行时 `*(0x00ab91b0)` 为栈地址），地图对象 = `app+0x48`，
容器 = `*(app+0x88)`。vector 为 `vector<Elem*>`（指针数组，元素 44 字节堆分配），
begin/end/cap = 容器 `+0x28/+0x2c/+0x30`。容器 `+0x38/+0x3c` 另维护运行时
RLE 分配链表。

**主消费函数 `FUN_004266d0`（旧基线 `0x4268b0`；ApplyMask，唯一 vector 遍历消费者）**：
逐个元素检查 `elem+0x18 flag != 0`（flag=0 直接跳过）→ 地线深度测试
`param_4(y) < ((elem+0x14 f6 * param_3(x)) >> 8) + elem+0x10 sortY`
（当前 `0x426747-0x42675d`：`IMUL f6,x; SAR 8; ADD sortY; CMP y; JGE skip`）→
矩形相交（helper `0x4265b0`）→ 首个命中时把角色像素块复制到暂存，
逐行走 RLE，covered=1 的像素调 `FUN_004264b0`（旧基线 `0x426690`）做 MMX 暗化
（`psraw 3; & 0x1f1f1f1f1f1f1f1f`，DAT_00aa9e68），`*param_5` 换成暂存块。
即 **遮挡表现为角色被盖部分变暗（ghost），不是重画背景**。

**渲染调用点**：精灵绘制函数 `FUN_0081ac30` 在 blit 前调地图虚函数
vtable+0x18（当前 `0x81ae3b` / `0x81b0c6`：`CALL [EAX+0x18]`），即包装
**`0x41e760`**（`MOV ECX,[ECX+0x40]; JMP 0x4266d0`，旧基线 `0x41e940`）；
命中后绘制并调 vtable+0x1c（恢复位图 channel）。地图 vtable = `0x9809f8`。

**暂存栈协议（第二轮闭合）**：容器 `+0x08` 起内嵌暂存结构
`{+0x08 cap=100, +0x0c=50, +0x10 count, +0x14 rec_begin, +0x18 rec_end}`，
记录 16 字节 `{buffer, capacity, saved_loc, original}`：
- 压栈：ApplyMask 入口 `FUN_00426640` → `FUN_00426460`/`FUN_00426410`
  （ECX=容器+8，`count++`，返回 `rec_begin + old_count*16`；当前地址
  `0x426460`/`0x426410`，旧基线 `0x426640`/`0x4265f0`）；
- 恢复：vtable+0x1c = `0x41e780` → `0x426580` → `0x425ea0`（当前；
  旧基线 `0x41e960`/`0x426760`/`0x426080`），取栈顶
  `*saved_loc = original`（不弹计数）；
- 清空：vtable+0x20 = `0x41e790` → `0x426570` → `0x425d50`（当前；
  旧基线 `0x41e970`/`0x426750`/`0x425f30`）：`count = 0`；
- 未命中弹栈：ApplyMask 全程无命中时 `0x42696a: DEC [容器+0x10]`
  （当前；旧基线 `0x426b4a`），命中路径以 `EMMS` 收尾（`0x42695e`）。

**flag/group/frame**：`FUN_00426980`（旧基线 `0x426b60`；group 1..11, frame 0..6）
把同组（`elem+0x1c`）中选中帧（`elem+0x20`）的 `flag(+0x18)` 置 1、其余置 0，
并打日志 "可见的遮挡:%d,等级:%d"（DAT_00980d10）。驱动链：
`FUN_00410000` 读 `config/map/<地图>.ini` 的 `constructor/PosType%d/Pos%dX/Y`
（默认路径 `map/banpaitest/`），调 `FUN_0041f490(group, frame=(v+1)/2)` →
`0x426b60` 切遮罩帧 + `0x427c40` 置场景对象（0x6c 记录 +0x68==group）可见标志
+ 通知 type==6 对象；`FUN_00410000` 由主命令分发器 `FUN_007a2120`
（17 KB 巨型 switch，命令域 0x2000..0x40fb）的 **case 0x3077** 调用；
分发器调用方为 UI 消息路由 `FUN_006c2140`/`0x6c2220`/`0x6c22b0`/`0x6c2890`
与 `FUN_007a6f70`/`0x7a7090`。
BangPai_1 的 105 条 flag=0 = 帮派建筑各建造阶段的遮罩帧。

**动态遮罩**：`FUN_00427740` 从 WAlphaBitmap 的 alpha 通道逐行生成 RLE
（alpha≥0x10 → covered），创建 flag=1 元素入容器（`0x426c90` 按 id 移除）。
调用方 `FUN_0043d410` / `FUN_0043ee00`（精灵 "app:current" 当前帧）与
`FUN_004f7130`（地图 ANIFILE 帧，模板在 `+600`），即角色/动画物件实时注册遮挡。

**字段复核结论**：
- f2/f6/obj+0x10：已验证为地线参数，**用于遮挡深度测试（非 vector 排序）**；
  全二进制未见对遮罩 vector 的 qsort。
- f3(kind)、f5(blob 长度)：**运行时无任何消费**——解析器 `0x4274b0`
  只读记录字段 0/1/2/4/6（且步长硬编码 0x1c，MSK 284 字节记录根本不会进解析）。
- 容器 `+0x40` 为"禁用遮罩"开关：仅 ctor/加载器写 0；全二进制字节级扫描
  `89 /r`（寄存器写）与 `C7 /0`（立即数写）对 `+0x40` 的 3 个命中
  （`0x40175a`/`0x404cb2`/`0x5106a2`）经反编译核实均为**无关类**
  （`FUN_00510660` 是另一 list 管理器，+0x28 是链表非 vector）→ 遮挡常开。
- IsCover/GetCoverPt（`WBitmap::IsCover` 0x7f0ae0 等）属于位图类像素级命中，
  与遮罩容器无调用关系；遮罩不参与点击判定（未见相关引用）。

**运行时只读验证（2026-10-21，`tools/kimi_peek_masks.py`，
仅 `ReadProcessMemory`，无注入）**：对运行中 Game.exe 实测
`app=*(0x00ab91b0)`（栈上）、容器 `*(app+0x88)` 存在；
`+0x40 disable=0`（遮挡常开✓）；暂存 `count` 空闲时=0、cap=100/50 ✓；
vector 563 元素全部 `flag=1 group=0`（非帮派图），`sortY` 基本升序 ✓；
f6 实测值 {-168,-139,-128,-125,-105,-56,-53,+111,+120,+149} 与静态
"±128 地线斜率"判读一致 ✓。元素坐标/尺寸与当前地图物件吻合。

> **2026-10-23 复核注记**：原脚本的偏移标签与静态布局不一致（把
> `+0x00` 打印成 init100，而实际为 placement_count；cap100/50 在
> `+0x08/+0x0c`，staging count 在 `+0x10`）。脚本已修正（§4.7 顶部布局），
> 但游戏已退出，尚未用修正版重跑；上段运行数字来自旧脚本，需在下次启动
> 游戏时复验。

### 4.6 导出

`tools/export_core_masks.py`（遮罩白=覆盖，黑=透明；已存在则跳过）：

| 批次 | 文件 | 记录 | 像素 | 输出 | 耗时 |
|---|---:|---:|---:|---:|---:|
| B0 | 10 | 564 | 22,624,352 | 0.50 MiB | 3.8 s |
| B1 | 50 | 2,165 | 121,738,080 | 3.04 MiB | 7.1 s |
| 全量 | **300** | **12,755** | **717,967,584** | **15.85 MiB** | **21.3 s** |

- 输出：`extracted/core_masks/<corpus>/<源路径>.core/0000.png`
  （MSK 同理，保留扩展名避免同目录同名 `.core`/`.MSK` 冲突）；
- 清单：`manifests/batches/core-mask-b0.csv`、`core-mask-b1.csv`、
  `core-mask-full.csv`；
- 全量 12,755 张 PNG 全部落盘，抽查 200 张尺寸与清单一致，0 错误。


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

**全部通过头部/尺寸/表结构校验；MSK/core 数据区 RLE 也已全量解码
（300/300 文件、12,755 记录、0 错误）**（`manifests/batches/map-scan.json`）。

CEL 全量导出：

| 批次 | 掩码 | 格子数 | 输出 | 耗时 |
|---|---:|---:|---:|---:|
| B0 | 10 | 211,648 | 0.02 MiB | 2.2 s |
| B1 | 50 | 1,077,440 | 0.06 MiB | 1.4 s |
| 全量 | **275** | **5,573,952** | **0.32 MiB** | **2.3 s** |

- 0 错误，275 个 PNG 全部落盘、抽查 60 个尺寸匹配；
- 清单：`map-cel-b0.csv`、`map-cel-b1.csv`、`map-cel-full.csv`。

## 7. 待办

1. MSK/core 消费函数：确认 `obj+0x10/+0x14`（f2/f6 地线）在遮挡排序中的
   用法，以及 `flag/group/frame` 的播放逻辑（帧率、循环、触发）；
2. MAN 路径与实际动画文件的关联（供地图动效）；
3. 与 MAP INI / 背景 JPG 合成完整地图预览（遮罩数据已可用于叠加）；
4. MLK `value` 语义与跳转图。
