# 特殊 JPG（FF D8 FF C8）格式分析

- 日期：2026-10-20
- 目标：`D:\software\dhsh\shdata\Game.exe`
  SHA-256 `f6bf66d24a3523bb6a30541589fabea36bbceec84355463d8e2c99e3b7f16be9`
- 实现：`tools/dhsh/formats/special_jpg.py`、`tools/probe_special_jpg.py`
- 独立复核：`docs/formats/special-jpg-kimi-review.md`、`tools/kimi_*`
- 状态：**容器、Huffman 表、MCU 描述符、块熵解码、颜色转换已与 Game.exe
  对齐并通过截图对拍；IDCT 采用 floor 浮点近似（与 MMX 位精确实现相差
  ±1），尚未批量导出。**

## 1. 语料与识别

在 `extracted/images-standard` 的 544 个 `.jpg` 中：

- 34 个是标准 JPEG（Pillow 可打开）；
- **510 个**以 `FF D8 FF C8` 开头，Pillow 无法识别；主要位于 `map/`
  （地图背景、小地图/缩略图等），合计 488.9 MiB、约 15.5 亿像素。

`Game.exe` 的加载分支（VA `0x8238d0`）：

1. 读取首字（小端）并比较 `0xD8FF`（即 `FF D8`）；
2. 若偏移 2 处的字为 `0xC8FF`（即 `FF C8`），设置私有标志并调用
   头部解析器 VA `0x8a3dd0`；
3. 否则走标准 JPEG 解析器 VA `0x8229d0`。

## 2. 容器结构（510/510 验证）

所有数字均为小端：

```text
0x000  char[4]   FF D8 FF C8
0x004  uint32    header_size = 1000（固定值，不代表表区结束）
0x008  uint32    version（2 = 427 个，3 = 83 个）
0x00c  uint32    width
0x010  uint32    height
0x014  uint8     bpp = 8
0x015  uint16    width（重复）
0x017  uint16    height（重复）
0x019  uint8     组件数 = 3
0x01c            组件记录 ×3，每条 6 字节
0x02c            3 × 64 × uint16 量化表（每表 128 字节）
0x22c            8 个 Huffman 表槽位（变长）
...              mcu_count × 6 字节 MCU 描述符
...              uint32 data_size + 私有熵编码数据，直到文件末尾
```

`0x8a3dd0` 从文件偏移 **0x008** 复制 548 字节到对象 `+0x04`，然后把
游标设为 `0x008 + 0x224 = 0x22c`。

### 2.1 组件记录与量化表

每条 6 字节，偏移 `0x1c`、`0x22`、`0x28`：

```text
u8 quant_idx      // 0..2
u8 ac_idx         // Huffman 槽位 0..7
u8 dc_idx         // Huffman 槽位 0..7
u8[3] 未知
```

解码器读取 `obj+0x18/0x19/0x1a`（= 文件 `0x1c/0x1d/0x1e`）等，
分别对应 q/ac/dc。量化表基址为 `obj+0x28`（= 文件 `0x2c`），
索引步长 128 字节，每表 64 个 uint16。

### 2.2 Huffman 表

从 `0x22c` 开始，按 **2 组 × 4 个数组 = 8 个槽位** 顺序存放：

```text
u32 count
if count > 0:
    count × { u16 code, u8 length, u8 symbol }   // 每项 4 字节
```

- 槽位 0–3 属于第一组，4–7 属于第二组；
- 实际语料中 4 个槽位非空：DC 表 12 项、AC 表 162 项；
- `code` 是规范 Huffman 码值（低位对齐），按 MSB-first 比较；
- 组件记录中的 dc/ac 索引直接指向这 8 个槽位（例如示例中
  comp0 = q0/ac4/dc0，comp1 = q1/ac5/dc1）。

### 2.3 MCU 描述符

表区结束后是 `mcu_count × 6` 字节，其中

```text
mcu_cols = width  / (version == 2 ? 16 : 8)
mcu_rows = height / (version == 2 ? 16 : 8)
```

（510 个文件均满足 width/height 按 MCU 对齐。）

每个 6 字节描述符按 `0x8a3dd0` 拆成：

```text
f0 = dword & 0x3ff                 // 熵数据字节偏移增量（10 位）
f1 = byte[4] & 0x1f                // 32 位大端字内的位偏移（0..31）
f2 = signed16(word[4]) >> 5        // DC 预测槽 0
f3 = signed16(signed32(dword << 11) >> 21)  // DC 预测槽 1
f4 = signed32(dword) >> 21         // DC 预测槽 2
```

- `f0` 逐 MCU 累加，起点为数据区首字节，得到每个 MCU 的熵数据指针；
- 描述符本身还保存 **上一 MCU 的最终 DC 预测值**，因此每个 MCU 可独立解码
  （随机访问）；v3 的 `f2` 与上一 MCU 解出的 DC 在样本中逐个精确相等；
- 12 字节内存记录布局：`+0` 指针（累加后）、`+4` 低 5 位位偏移、
  `+6` f2、`+8` f3、`+10` f4。

### 2.4 数据区

```text
uint32 data_size
data_size 字节熵编码数据
```

对 510 个文件验证：`表区末尾 + mcu_count*6 + 4 + data_size == 文件长度`，
**510/510 全部成立**。

## 3. 解码流程（已实现）

### 3.1 MCU 驱动（VA 0x823240）

- `version == 2`：MCU 16×16，4 个 Y 块 + Cb + Cr（4:2:0），
  输出转换器 `0x8a3590`；
- `version == 3`：MCU 8×8，Y/Cb/Cr 各 1 块（4:4:4），
  输出转换器 `0x8a3b30`；
- 每个 MCU 的记录被复制到栈上，**三个 DC 字在多次块解码间共享并原地更新**；
- 块解码顺序与 DC 槽位：

```text
v3: Y=slot0, Cb=slot1, Cr=slot2
v2: Y0=Y1=Y2=Y3=slot0, Cb=slot1, Cr=slot2
```

（4 个 Y 块共享同一条 DC 链；Y3 的预测值传给下一 MCU 的 Y0。）

- 同一 MCU 内的块共享一个位流状态（`0x8a1fe0` 是 cdecl，指针/位偏移写回
  栈上记录）；下一个 MCU 从自己的描述符指针重新开始。

### 3.2 块熵解码（VA 0x8a1fe0）

- 位流按 **32 位大端字**读取，位序 MSB-first（等价于原始字节流的 MSB-first）；
- 起始位置 = 描述符指针 + `f1`（`f1` 是 32 位字内的位偏移）；
- DC：Huffman 符号 = 类别，随后读该类别的差分位并做 JPEG 符号扩展，
  加 `f2/f3/f4` 槽中的预测值；
- AC：标准 run/size 码，`0xF0` 跳过 16 个系数、`0x00` 结束；
- **私有 zigzag 表按倒序使用**：扫描第 k 个 AC 系数（k 从 1 起）写入
  `ZIGZAG[64-k]`，量化表仍按扫描序索引 `quant[k]`；表本身（VA `0xab62c8`）
  倒过来读即正常的低频→高频顺序；
- DC 乘积按 `imul` 后只存 AX 做 **int16 回绕**；AC 乘积做 **int16 饱和**
  （`0x8a215d`–`0x8a2170`）；
- 系数写入 64 个 int16 后调用 IDCT 函数指针 `[0xb0c9b8]`。

私有 zigzag 表原表（解码第 k 个 AC 系数使用 `ZIGZAG[64-k]`）：

```text
0,63,55,62,61,54,47,39,46,53,60,59,52,45,38,31,
23,30,37,44,51,58,57,50,43,36,29,22,15,7,14,21,
28,35,42,49,56,48,41,34,27,20,13,6,5,12,19,26,
33,40,32,25,18,11,4,3,10,17,24,16,9,2,1,8
```

### 3.3 IDCT 与颜色转换

- IDCT 函数 `0x8a8780`：MMX AAN 定点实现，`packsswb` 输出 **有符号 8 位**；
- 独立复核（`kimi_idct_sim.py` 逐条解释 439 条 MMX 指令）确认：
  - 相对教材公式 `(1/4)Σ C(u)C(v)F·cos·cos` 的缩放为 **1/16**
    （即 `raw_sum/64`）；
  - 末级为 `psraw 4`（floor）+ `packsswb` 饱和，**无舍入偏置**；
  - 输出字节序为 `y*8+x`；矩阵形式 `T·F·Tᵀ` 得到的是 `x*8+y`，
    因此每个块需转置；
  - 预缩放表 64×u16 位于 VA `0xab6350`（文件偏移 `0x6b6350`）。
- 颜色转换（`0x8a3b30` v3 / `0x8a32d0` v2 的活动 RGB565 路径）：
  - 查表（`0x8a3d46`，x87 截断模式）：
    `T_R=trunc(1.402v+1)>>1`、`T_Gb=trunc(1-0.34414v)>>1`、
    `T_Gr=trunc(1-0.71414v)>>1`、`T_B=trunc(1.772v+1)>>1`（v 为 int8）；
  - 像素：`t=(Y+128)>>1`，
    `R7=clamp(T_R[Cr]+t,0,127)`、
    `G7=clamp(T_Gb[Cb]+T_Gr[Cr]+t,0,127)`、
    `B7=clamp(T_B[Cb]+t,0,127)`；
  - RGB565：`(R7>>2)<<11 | (G7>>1)<<5 | (B7>>2)`；
    v2 的 4:2:0 色度按 2×2 最近邻复制；
  - 运行时实测活动路径为 MMX IDCT + RGB565（`0x8a8780`/`0x8a32d0`/
    `0x8a38d0`，掩码 `0xf800f800`/`0x07e007e0`）；dump 中的 `0x8a3590`
    是 RGB555 变体，不是本机路径。
- Python 实现采用 float IDCT + `floor` + 逐块转置，与位精确 MMX 实现
  相差约 ±1；颜色转换按上述查表/RGB565 管线实现，再展开为 888 PNG。

## 4. 验证结果

| 样本 | 版本/尺寸 | 块数 | Huffman/越界错误 |
|---|---|---:|---:|
| `QHXu_dQHSD_q.jpg` | v3 128×112 | 672 | 0 |
| `YTFDL_2_q.jpg` | v2 240×128 | 720 | 0 |
| `BangPai_1.jpg` | v2 6400×6400 | 960,000 | 0 |

- `BangPai_1.jpg` 全部 160,000 个 MCU 解完，说明描述符指针/位偏移、
  Huffman 表索引、块顺序与 v2 槽位语义均对齐；
- v3 样本中 `f2[M] == (f2+diff)[M-1]` 连续成立，验证 DC 预测链；
- v2 DC 槽位修正（4 个 Y 块全部 slot0）后，Y-DC 平面相邻差均值
  289 → **28**，MCU 内 4 块 DC 极差中位数 341 → **22**
  （`tools/kimi_verify_slots.py`）；
- 与独立复核的全修正输出对比（`extracted/easy/kimi_bp_*_fullfix.png`）：
  - `BangPai_1m.jpg`（v2 320×320）：平均绝对差 **1.10**、61.2% 像素完全一致、
    最大差 9（来自 float IDCT ±1 与内部定点截断）；
  - `BangPai_1_q.jpg`（v3 256×224）：平均绝对差 2.44，结构/颜色一致；
- 真机世界地图截图对拍（`bangpai.png`）：修正三处块级 bug 后，
  并排图 `kimi_side_by_side.png` 与游戏画面一致，残差来自 UI 缩放重采样；
- 510 个文件的结构校验（表区 + 描述符 + data_size == 文件长度）全部通过。

## 5. 工具

| 路径 | 说明 |
|---|---|
| `tools/dhsh/formats/special_jpg.py` | 正式解析/解码模块（numpy IDCT 批量） |
| `tools/probe_special_jpg.py` | 原型：逐块 Python 解码、DC 假设对比、样本 PNG |
| `manifests/batches/special-jpg-code-dump.txt` | 关键函数反汇编（8 个函数，约 2,000 行），供独立复核 |

## 6. 后续

1. 批量导出：10 个样本 → 50 → 510 个 PNG（当前模块已能解码大图，0 错误）；
2. 可选：用 `tools/kimi_idct_sim.py` 的位精确 MMX 解释器替代 float IDCT，
   以做到逐像素一致（速度较慢，批处理可保留 float 路径）；
3. 若有更多运行截图，可继续做像素级对拍。
