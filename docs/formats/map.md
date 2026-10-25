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
  二进制复核关键行为；地址以当前 SHA 为准（§4.7）。MAN 位置语义
  （x/y=左上）、MLK 双向配对与入口矩形已闭合（§3/§5）。**

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
- `flags & 1` 时文件含两张位图（各 `ceil(width/8)*height` 字节）：
  **第一张 = 正常地面碰撞层**，**第二张 = 辅助层**；`flags=0` 只有一张
  （即地面层）；
- 格子尺寸：地图像素 / 16（如 1536×1024 → 96×64）。

位序验证：对 80 个 CEL 的行/列边界统计置 1 比例，
MSB-first 为 **98.6%**，LSB-first 为 91.6%，与地图边缘墙体一致。

> **2026-10-23 运行时确认（YingTianFu）**：加载器 `FUN_00420790` 对
> `FUN_004252e0` 调用两次——`param_5=1` 的第一张装入 `map+0x34`，
> `param_5=0` 的第二张装入 `map+0x38`。玩家所在格 `(128,125)`：
> `map+0x34` 缓冲 `0x00`、override `0x00`（可走）；`map+0x38`
> 缓冲 `0xff`、override `0x80`（阻挡）。因此**第一张是正常地面碰撞层，
> 第二张不是**。旧 `parse_cel`/`export_map_cel.py` 固定取第二张，已修正
> 为默认第一张；275 张 CEL PNG 已重导（YingTianFu `blocked_ratio`
> 0.9589 → **0.4844**）。
>
> **小图补齐（YTF_WuQiDian，2026-10-23）**：文件 CEL 为 64×40，地图格
> （INI 1024×768 >> 4）为 64×48。运行时对象是 **64×48**；抽查格
> `(0,0)/(10,10)/(33,29)/(50,30)/(63,39)` 的缓冲值与文件位一致，
> 行 40–47 为 `0xff`。`FUN_004252e0` 在文件尺寸小于地图格时保持地图格、
> **左上复制**、右侧/底部补 `0xff`（阻挡），不是居中。地图预览对这类
> 图按左上补齐绘制（§8）。
>
> **截图复核**：`pic/ytf_wuqi_1.png` 与背景模板匹配偏移 `(120,167)`
> （多块区域 0.96–0.99）；把 CEL 可走区域叠到截图上，绿色轮廓贴合
> 地板/楼梯，炉子、墙体与 1024×768 画布黑边均为阻挡。文件 92% 阻挡
> 主要来自黑边/墙体，实际可走区域约 **6.5%** 画布，和画面一致。
>
> **地图坐标小于背景（TingYuan_*，2026-10-23）**：`TingYuan_1` 相机
> `(0,0)`；背景非黑 bbox 为 `(0,0)-(895,639)`，即 896×640 地图内容
> 位于 1024×768 背景的**左上角**，右侧/底部 128px 为黑边；CEL 56×40
> 在 `(0,0)` 完整覆盖。截图 `pic/tingyuan.png` 与背景模板匹配偏移
> `(-1,-26)`，是窗口/标题栏偏移（地图视口从截图 `(1,26)` 开始）；
> CEL 可走轮廓贴合草地/石径。结论：**背景偏移 = (0,0)**，不是居中。
>
> 仍待解：第二张辅助层的确切用途；`JiaYuan_1_1` 这类 CEL 比地图格
> 更大的异常图（预览按左上裁剪处理并告警）。

导出：`tools/export_map_cel.py` → `extracted/map_cel_png/<corpus>/...png`
（白=阻挡，黑=可走）。

## 3. MAN（动画路径表）

解析器 `VA 0x41ee5b`；地图加载器 `FUN_00420790` → 动画对象初始化
`FUN_00431fc0`。

```
0x00  char[4]  "MAN\0"
0x04  uint32   version = 4
0x08  uint32   total_size
0x0c  uint32   reserved = 0
0x10  uint32   data_offset = 28
0x14  uint32   entry_size = 100
0x18  uint32   count
0x1c  ...      count × 100 字节记录：
                  char[64] path            // 动画目录
                  s32 x, y                 // 对象左上角地图像素坐标
                  s32 f48, f4c, f50, f54, f58, f5c, f60
```

- 路径示例：`map\animate\XingXiuCun\DangPu\001`；目录内是
  `00000.a16`… 帧序列 + `perinfo.dat`（PIF）。
- **x/y = 左上位置**：把 `YingTianFu/duchang/denglong1` 首帧（A16，
  header 偏移 0,0）按 `(2628,421)` 叠到背景，带 alpha 模板匹配
  （`TM_SQDIFF`）在该点精确命中；`--man` 预览中灯笼与背景贴合。
- 加载器只读 `path/x/y`，随后用 `(*obj+0x2c)(x, y)` 设置动画对象位置；
  其余字段由 `FUN_00431fc0` 复制到对象槽位：
  `f48→obj+0x58`、`f4c→布尔标记`、`f50→obj+0x1e0`、
  `f54→bit0..bit2 三个开关`、`f58→obj+0x1d8`（有符号）、
  `f5c/f60→obj+0x1e8/+0x1ec`（×10）。
- **f48 = 深度排序 Y 偏移**：getter `0x824740` 返回
  `obj+0x58 + obj.y`；setter `0x824770`；排序调用者 `0x7feba0` 在某个
  标记为真时再加 10000。语料值 −730…1012：火焰/水面的负值把特效压到
  地形后，正的 1000 左右用于前景。
- **f4c = 启用/自动开始**：语料恒为 1；初始化器同时写
  `obj+0x1dc=1`、`obj+0x112=1`、`obj+0x113=1`。
- **f5c/f60 = 定时重置参数**：初始化器把 `f5c*10`、`f60*10` 写入
  `obj+0x1e8/+0x1ec`；`FUN_004312e0` 在计时到期时设置
  `next = now + f5c*10 + rand()%(f60*10 + 1)`。按 500…4000 /
  1000…5000 的取值，即约 5–40 s 基础间隔 + 10–50 s 随机抖动。
- **f50 = 对象查找键（推断为宿主/地图对象 ID）**：初始化器写入
  `obj+0x1e0`；`FUN_0040ff80`/`FUN_0041f2b0` 遍历动画对象时以
  `obj+0x1e0 == id` 匹配，再调用 `FUN_004314d0`（重载）或
  `FUN_004312c0`（切换 active）。MAN 语料中恒为 0，表示未绑定。
- **f58 = 排序斜率（Q8.8）**：vtable 方法 `0x431290` 返回
  `y + f48 + (f58 * arg >> 8)`；`f48` 是常量偏置，`f58` 是随参数
  变化的斜率（−135…312 ≈ −0.53…1.22），用于斜向地面的深度排序。
- **f54 = 三个 bit**：分别写入 `obj+0x1dd/+0x1de/+0x1e4`。对当前
  Game.exe 整个 `.text` 做 `mem.disp` 扫描：**没有任何指令读取
  +0x1dd/+0x1de**；+0x1e4 的读取只出现在其他类（非动画类），因此这
  三个 bit 对地图 MAN 很可能是**遗留/未使用**，语义保留开放。
- 语料分布（257 个 MAN 路径副本 → **95 份唯一内容、1,700 条记录**）：
  `f4c` 恒为 1、`f50` 恒为 0；`f54` 仅 0/1/5/7；`f48` 有 531 种值
  （−730…1012）；`f58` 有 79 种值（多数 0，−25770…312，极端值仅个例）；
  `f5c` 为 0/500…4000、`f60` 为 0/1000…5000 的毫秒样式值（对象内 ×10）。
  `f48/f4c/f50/f5c/f58/f60` 已给出消费点；`f54` 未发现读取点。
- **运行时验证（2026-10-25，YingTianFu）**：RPM 只读枚举 103 个动画
  对象，`(x, y, f48, f58, f5c*10, f60*10, f54 bits)` 与 MAN 文件
  103 条记录**多重集完全一致**（runtime-only=0、file-only=0）。
  f54 分布 83×0 / 20×7；f58 非零 11、f5c/f60 非零各 20。
  证据 `manifests/batches/man-runtime-YingTianFu.json`。这确认了
  字段→对象槽位映射；f54 仍是“已存但无读取点”。
- 全量扫描（`tools/scan_map_anim.py` →
  `manifests/batches/map-anim-scan.json`）：292 个 INI 中 **283 个**
  找到 MAN 文件（9 个整图不在当前导出树：`JingYingSaiChang`、
  `LiangShan_GuanYueTing`、`PLJY_FB`、`PengLai_FB`、`QFYD_FB`、
  `SMTD_TuJiTan`、`YGXu_dJYG`、`ZhongQiu_1`、`ZhongQiu_2` —— 与缺
  背景的图一致）；**1,968 条记录**、每图去重后 **713 个**动画目录；
  **710/713 个目录可解析**（3 个缺失：`Tiaozhan2/hongdeng`、
  `Tiaozhan2/DengGuang`、`fire10`），按图累计 **4,952 个帧文件**；
  497 个全局去重目录中 444 个含帧文件，53 个只有 `perinfo.dat`
  （帧未导出），帧文件实际为 `.a16` 2,256 + `.tga` 154（去重后 2,410 个）。
- **路径回退**：部分 INI 写 `map\Foo\Foo.MAN`，语料实际在
  `map1/Foo/Foo.MAN`（如 `DanRenTiaoZhan_3`、`HFZLFangJian_1`）。
  `scan_map_anim.py` 与 `map_preview.py` 会依次尝试原路径、`map1/`
  兄弟路径和去掉 `map/` 前缀的形式；缺少 `map1/` 回退时会把 8 张图
  误报为 MAN 缺失。

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
vector 563 元素全部 `active=1 group=0`（非帮派图；`+0x18` 是运行时
激活标志，不是文件放置 flag），`sortY` 基本升序 ✓；
f6 实测值 {-168,-139,-128,-125,-105,-56,-53,+111,+120,+149} 与静态
"±128 地线斜率"判读一致 ✓。元素坐标/尺寸与当前地图物件吻合。

> **2026-10-23 复核结果（修正版 `run_mask_peek.py`）**：在 SHA `7a5e2c08…`
> 的 pinned 快照对应客户端上实跑 YingTianFu：`app=0x19cbfc`、容器
> `0x14a0fa68`，placement_count=563、vector=563、cap100=100、cap50=50、
> staging=0、disable=0，563 个元素全部 `active=1 group=0`；7 个关键地址
> （解析器/loader/ApplyMask/深度测试/group-frame/darken/vtable）的 16 字节
> 与 pinned 快照全部一致（0 differ），`RESULT: PASS`。明细见
> `manifests/batches/mask-peek-YingTianFu.json`。
>
> 同一 SHA 下另在 **XingXiuCun** 复跑一次：placement_count=386、vector=386、
> cap100/50 仍为 100/50、staging=0、disable=0，386 个元素全部
> `active=1 group=0`（运行时 `+0x18` 是 active 标志，见 §4.9），7/7 关键
> 字节一致，`RESULT: PASS`；明细见
> `manifests/batches/mask-peek-XingXiuCun.json`。
> 输出名已带地图名，跨场景复跑不再互相覆盖。
>
> 修正版首跑时枚举不到进程：自写的 Toolhelp `PROCESSENTRY32` 是 32 位布局，
> 在 64 位 Python 下 `Process32First` 失败且被静默当成“未找到”。已改用
> PSAPI（`EnumProcesses`/`EnumProcessModules`/`GetModuleFileNameExW`），
> 与 `tools/kimi_probe_runtime.py` 的 `find_game()` 同一路径。

### 4.8 运行时复跑（修正版）

1. 启动游戏并进入**有遮罩的地图**（例如 YingTianFu；不要停在登录界面）。
2. 在仓库根目录运行封装命令：

```bash
python tools/run_mask_peek.py
```

它会先调用 `snapshot_gameexe.py` 固化/比对当前 SHA，再只读读取：
`app=*(0xab91b0)`、容器 `*(app+0x88)`、placement_count、cap100/50、
staging count、向量长度、disable、36 字节元素摘要（`+0x18` active /
`+0x1c` group / `+0x20` frame），以及关键地址字节
是否与 pinned 快照一致。输出 `manifests/batches/mask-peek-<map>.json`
（文件名只含地图名，不含版本/SHA；跨场景不会覆盖），末尾打印
`RESULT: PASS/FAIL`。

3. 也可直接跑原脚本（只打印，不判定）：

```bash
python tools/kimi_peek_masks.py
```

4. 如果 `Game.exe` 运行后 SHA 变了：`python tools/snapshot_gameexe.py`
   会把新版本加入 `extracted/gameexe-snapshots/`，并打印与上一版的
   字节/4KiB 页/关键地址差异；地址仍一致时可继续使用当前静态结论。

### 4.9 BangPai_1 的 group/frame 语义（2026-10-23 闭合）

**数据侧**：`config/map/BangPai_1.ini` 列出 11 种建筑类型（1 聚义堂、
2 厢房、3 冶炼房、4 比武场、5 库房、6 金库、7/8/9/10 箭塔、11 研究院），
命名规则 `类型*1000+级别`。core 放置表的 `group` 即建筑类型、`frame`
即级别：`map/BangPai_1` 300 个放置中 50 个动态项（groups 1–10），
`map1/BangPai_1` 238 个中 55 个（groups 1–11），frames 均为 1–5；
其余 298 个 core 全部静态（§7）。

**消费链**（pinned 项目 `ghidra-proj/dhsh-7a5e`，地址均为当前 SHA）：

| 地址 | 角色 |
|---|---|
| `0x7a2240` | 主命令分发器（旧基线 `FUN_007a2120`，17 KB switch） |
| `0x7a339c` | `case 0x3077` 调用点 → `FUN_00410000` |
| `0x410000` | 帮派地图加载器：读 `config/map/<map>.ini`，遍历 13 个建筑槽 |
| `0x41f2b0` | 单建筑更新：调 `group_frame` + 碰撞/对象可见性 |
| `0x426980` | `group_frame(group, frame)`：按组选帧 |
| `0x427a60` | 次级消费者：0x6c 步长表，`+0x68`=group，`+0x64`=可见 |
| `0x4312c0` | 建筑对象（type 6）可见性 |

`FUN_00410000` 的**函数入参**是 13 条 × 10 字节的建筑记录（stride `0x0A`）：
`byte0` = 状态，`byte9` = 建筑类型；类型 0 表示空槽。令
`frame = (state + 1) / 2`（整数除法）：state 0→0（隐藏），1/2→1，
3/4→2，5/6→3，7/8→4，9/10→5。INI 中未出现的类型会调用
`FUN_0041f2b0(type, 0)` 隐藏整组，并在 `PosTypeN` 处创建 0 级对象。
这是函数级布局；线上 `0x3077` 报文的加密封装尚未验证（现有抓包为
高熵流，需先定位协议加解密层）。

`FUN_00426980` 的关键逻辑（伪代码，来自反编译）：

```c
if (group < 1 || group > 11 || frame < 0 || frame > 6) return 0;
for (each element in container vector) {
    if (element->group == group) {          // +0x1c
        if (element->frame == frame)        // +0x20
            element->active = 1;            // +0x18
        else
            element->active = 0;
    }
}
```

**结论**：这里没有帧率、循环或时间动画；`0x426980` 只被 `0x41f2b0`
直接调用，`0x41f2b0` 只被 `0x410000` 调用，`0x410000` 只被
`0x7a339c`（`case 0x3077`）调用。即 **完全由命令 `0x3077` 的建筑状态
选择 `(group, frame)`，状态更新时即时切换**；客户端不自行推进帧。
运行时元素 `+0x18` 是这里的 `active` 标志，不是文件放置表的 `flag`
（文件 flag=1 静态、0 组员），`run_mask_peek.py` 已把字段名改为
`active_1`。证据摘要：`manifests/batches/bangpai-playback.json`。

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

加载器 `FUN_00428390`（version 3）；`FUN_00428a00` 在进图时按 target 递归
预载目标图的 MLK，形成连通图。

```
0x00  char[4]  "lnk\0"
0x04  uint32   version = 3
0x08  uint32   total_size
0x0c  uint32   1
0x10  uint32   count
0x14  ...      count × 108 字节记录：
                  u32 value        // 本图链路 id
                  char[64] target  // 目标 .map 名称
                  s32 paired       // 目标图反向链路的 value
                  s32 x, y, w, h   // 本图入口矩形
                  s32 dest_x, dest_y
                  s32 extra0, extra1, extra2
```

- **`paired` = 目标图反向记录的 `value`（双向配对已验证）**：
  313 条记录中 308 条存在反向边，**308/308** 双向满足
  `forward.paired == reverse.value` 且 `reverse.paired == forward.value`；
  5 条无反向边（`SLZC_FB→LJJY/CangZhou`、`YuMiKuaiPao→HuangNiGang_20old`、
  `ZhongQiu_1/2→YingTianFu`）。
- **矩形 = 本图入口触发区**：YingTianFu 24 条记录在预览中把青色框
  叠到各店铺门口脚垫上（`YTF_YouXiDian`/`YTF_BiaoJu` 截图核对）；
  `w/h == 0` 时加载器改写为 32、x/y 减 32 —— 当前语料 **0 条**触发该分支。
- **`dest` = 本图出生格（2026-10-25 运行时确认）**：从 YingTianFu
  进入 `YTF_YaMen` 后，玩家 px=`(520,552)`、格 `(32,34)`；目标图反向
  记录 dest=`(512,544)`（格 32,34 的左上角），玩家站在格中心
  `(+8,+8)`。源图记录的 dest=`(6960,2128)` 不在 1280×896 的店内，
  排除“目标图使用源图 dest”。因此每条记录的 dest 是**该记录所属
  地图上的出生格**：进入目标图用目标图反向记录的 dest；返回源图用
  源图记录的 dest。反向复测：从衙门回到 YingTianFu 后玩家
  px=`(6968,2136)`、格 `(435,133)`，正好是源记录 dest `(6960,2128)`
  的格中心。311 条记录中 246 条 dest 紧邻入口矩形；证据
  `manifests/batches/mlk-dest-YTF_YaMen.json`。`extra0` 仅 0..7
  （疑为朝向/方向），`extra1` 多为 1（6 条为 0），`extra2` 多为 0
  （2 条为 5/6）；三者语义仍未定。
- `value` 在 221/228 个文件中为 0..count−1 连续序号（7 个文件有跳号），
  因此是**链路 id**，不是开关。
- 全量扫描（`manifests/batches/map-anim-scan.json` /
  `map-anim-edges.csv`）：**228 个文件、313 条记录**；311 条 target
  能在 292 个 INI 中找到；缺失的 2 条恰为 `ZGFHaoZhai`/`ZGFTingYuan`
  互相指向——这两个 MLK 存在但对应 `.map` INI 不在当前导出树。

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

MAN/MLK 专项扫描（`tools/scan_map_anim.py`，只解析、不解码像素）：

| 项 | 结果 |
|---|---:|
| 含 ANIFILE 的 INI | 292（283 个找到 MAN） |
| MAN 记录 / 每图去重目录 | 1,968 / 713 |
| 可解析动画目录 | 710（3 个缺失） |
| 按图累计帧文件 | 4,952 |
| 全局去重目录 / 含帧目录 | 497 / 444（53 个只有 `perinfo.dat`） |
| MLK 文件 / 记录 | 228 / 313 |
| 双向配对成功 | 308/308 |
| target 缺失 / 无反向边 | 2 / 5 |

- 清单：`manifests/batches/map-anim-scan.json`、
  `manifests/batches/map-anim-edges.csv`；0 解析错误。

## 7. 待办

1. ~~MSK/core 播放逻辑~~ **已闭合（§4.9）**：不是时间动画——`group`=建筑
   类型、`frame`=等级，由命令 `0x3077` 下发的建筑状态选择，无帧率/循环。
   静态数据面已全量扫描（`tools/scan_core_placements.py` →
   `manifests/batches/core-placement-scan.json`）：**300/300 文件、
   16,114 个放置、0 解析错误**；只有 `BangPai_1` 两个版本含非静态项——
   `map/BangPai_1` 300 个放置中 50 个 `flag=0`，groups 1–10；
   `map1/BangPai_1` 238 个中 55 个，groups 1–11；两者 frames 均为 1–5。
   其余 **298 个文件全部 `flag=1/group=0/frame=0`**。`obj+0x10/+0x14`
   的地线用法已由 `sortY = y + f2 - (x*f6>>8)` 确认。
2. ~~MAN 路径与实际动画文件的关联~~ **已完成（§3）**：283/292 图、
   1,968 条记录 → 710/713 目录可解析；`map_preview.py --man` 已叠首帧。
   剩余：`f48/f54/f58/f5c/f60` 语义、`perinfo.dat` 逐帧锚点/计时、
   53 个无帧目录、9 个 MAN 缺失图（整图未导出）。
3. ~~与 MAP INI / 背景 JPG 合成完整地图预览~~ **已完成（§8）**；剩余：
   第二张 CEL 辅助层用途、`JiaYuan_1_1` 等 CEL 大于地图格的异常图。
4. ~~MLK `value` 语义与跳转图~~ **已闭合（§5）**：`value`=本图链路 id，
   `paired`=目标图反向链路 id（308/308），矩形=入口触发区，跳转图已导出。
   剩余：`extra0..2` 语义与 `dest` 的运行时落点验证。

## 8. 地图预览（`tools/map_preview.py`）

图层顺序：背景（special JPG 解码 PNG）→ 可选 CEL（诊断，默认关闭）→
可选 MAN 首帧（`--man`）→ MSK/core 遮罩 → 可选 MLK 入口框/落点
（`--links`，诊断）。

- 静态遮罩（文件 `flag=1`）用洋红浅填充 + 2px 描边；`flag=0` 的 group
  成员默认只画每组最小帧（橙色）。`--no-mask` 可输出纯地图，
  `--groups none|first|all` 控制动态项。
- `--man` 按 MAN 记录把动画目录的首个 A16（无 A16 时取 TGA）帧
  按 `x/y + A16 header 偏移` alpha 混合到背景；YingTianFu 实测
  103 条记录绘制 93 条（10 条对应目录缺帧），灯笼/马匹/摊位等与背景
  自然贴合。`--man-frame N` 可选目录内其它帧（越界夹到末帧）。
- `--links` 用青色 2px 框画 MLK 入口矩形、框左上角标 target 文件名，
  橙色十字标 `dest`；YingTianFu 24/24、BangPai_1 6/6 全部绘出。
  预览里入口框与店铺门口脚垫重合，是 `rect = 入口触发区` 的直观证据。
- **画布 = 背景原生像素尺寸**；placement 直接按 `x/y` 贴图，边缘越界
  裁剪。INI `WIDTH/HEIGHT` 可能只是可玩区域，不能用来缩放背景：
  - `TingYuan_3`：INI 896×640，背景 1024×768（背景含黑边，mask bbox
    到 1025×747）；
  - `HFZLHaoZhai`：INI 3328×2304，背景/CEL 2816×2048。
- CEL 默认不绘制（保持画面简洁）；`--cel` 使用已确认的地面层
  （第一张，`--cel-layer 1`）。地图坐标=背景的图（大多数店铺）以及
  地图坐标小于背景的图（`TingYuan_*`，内容在背景左上角）均按
  **左上补齐/裁剪**绘制；背景右侧/底部的黑边保持不动。CEL 文件比
  地图格大时按左上裁剪并告警。
- 输出 `<map>.jpg`（文件名无版本/SHA），清单 `map-preview-*.csv`。
- 批次：B0 4 张、B1 10 张、B3 100 张、全量 **292 张已导出 INI**
  （283 渲染、9 张背景/遮罩在当前 PAK 索引中缺失，`--allow-missing`
  跳过），0 错误，**306.7 MB / 52.5 s**（`--jobs 6`）。7 张 INI 与背景
  尺寸不一致（TingYuan_1/2/3/4/HZ、HFZLHaoZhai、ZhongQiu_1_HC），均按
  背景原生尺寸处理并在清单中告警。缺失：`JingYingSaiChang`、
  `LiangShan_GuanYueTing`、`PLJY_FB`、`PengLai_FB`、`QFYD_FB`、
  `SMTD_TuJiTan`、`YGXu_dJYG`、`ZhongQiu_1`、`ZhongQiu_2`。
  > 注：`pak_inventory.json` 记录 305 个 `.map`，当前导出树 `config/map`
  > 有 292 个 INI；其余 13 个可能是别名/未落盘路径，待单独核对。
- **性能（2026-10-23）**：背景解码/成品编码优先用 OpenCV（PNG 解码、
  JPEG 编码）；遮罩先画到单通道标签图，再按颜色一次性混合；`--jobs N`
  多进程并行（默认 6）。全量 **276.5 s → 52.5 s（约 5.3×）**，无
  OpenCV 时回退 Pillow，`--jobs 1` 可复现顺序结果。
- 示例：

```bash
python tools/map_preview.py --map BangPai_1 --map YingTianFu
python tools/map_preview.py --map YingTianFu --man --links --tag manmlk
python tools/map_preview.py --sample 10 --seed 1 --tag b1 --allow-missing
python tools/map_preview.py --all --allow-missing
```
