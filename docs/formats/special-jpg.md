# 特殊 JPG（FF D8 FF C8）格式分析

- 日期：2026-10-20
- 目标：`D:\software\dhsh\shdata\Game.exe`
  SHA-256 `f6bf66d24a3523bb6a30541589fabea36bbceec84355463d8e2c99e3b7f16be9`
- 实现：`tools/dhsh/formats/special_jpg.py`、`tools/probe_special_jpg.py`
- 状态：**容器、Huffman 表、MCU 描述符、块熵解码已破解并通过结构校验
  （大图 960,000 块 0 错误）；IDCT 缩放与颜色转换目前为近似实现，尚未批量导出。**

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
v2: Y0=slot0, Y1=slot1, Y2=slot2, Y3=slot0, Cb=slot1, Cr=slot2
```

- 同一 MCU 内的块共享一个位流状态（`0x8a1fe0` 是 cdecl，指针/位偏移写回
  栈上记录）；下一个 MCU 从自己的描述符指针重新开始。

### 3.2 块熵解码（VA 0x8a1fe0）

- 位流按 **32 位大端字**读取，位序 MSB-first（等价于原始字节流的 MSB-first）；
- 起始位置 = 描述符指针 + `f1`（`f1` 是 32 位字内的位偏移）；
- DC：Huffman 符号 = 类别，随后读该类别的差分位并做 JPEG 符号扩展，
  加 `f2/f3/f4` 槽中的预测值；
- AC：标准 run/size 码，`0xF0` 跳过 16 个系数、`0x00` 结束；
- 系数按 **私有 zigzag 表**（VA `0xab62c8`，64 字节，非标准 JPEG 顺序）
  写入自然序，并乘以量化表对应项；
- 量化结果写入 64 个 int16，随后调用 IDCT 函数指针 `[0xb0c9b8]`。

私有 zigzag（扫描序号 → 自然序号）：

```text
0,63,55,62,61,54,47,39,46,53,60,59,52,45,38,31,
23,30,37,44,51,58,57,50,43,36,29,22,15,7,14,21,
28,35,42,49,56,48,41,34,27,20,13,6,5,12,19,26,
33,40,32,25,18,11,4,3,10,17,24,16,9,2,1,8
```

### 3.3 IDCT 与颜色转换（近似）

- IDCT 函数 `0x8a8780`：MMX AAN 定点实现，`packsswb` 输出 **有符号 8 位**；
- 颜色转换 `0x8a3b30`（v3）/ `0x8a3590`（v2）：
  常量确认为 `1.402`、`0.34414`、`0.71414`、`1.772`，标准 full-range
  YCbCr→RGB；表 `0xb0ced0/0xb0cdc8/0xb0cbc8/0xb0ccc8` 在
  `0x8a3c00` 中按有符号字节建立；
- 当前 Python 实现使用标准 float IDCT，缩放取 `raw_sum / 64`
  （标准公式为 `raw_sum / 4`），颜色转换用标准公式。
  该缩放使有符号 Y 落在约 `±128`（样本饱和率 0.12%），视觉合理；
  **精确的 MMX 常数/舍入仍待复核**。

## 4. 验证结果

| 样本 | 版本/尺寸 | 块数 | Huffman/越界错误 |
|---|---|---:|---:|
| `QHXu_dQHSD_q.jpg` | v3 128×112 | 672 | 0 |
| `YTFDL_2_q.jpg` | v2 240×128 | 720 | 0 |
| `BangPai_1.jpg` | v2 6400×6400 | 960,000 | 0 |

- `BangPai_1.jpg` 全部 160,000 个 MCU 解完，说明描述符指针/位偏移、
  Huffman 表索引、块顺序与 v2 槽位语义均对齐；
- v3 样本中 `f2[M] == (f2+diff)[M-1]` 连续成立，验证 DC 预测链；
- 510 个文件的结构校验（表区 + 描述符 + data_size == 文件长度）全部通过。

## 5. 工具

| 路径 | 说明 |
|---|---|
| `tools/dhsh/formats/special_jpg.py` | 正式解析/解码模块（numpy IDCT 批量） |
| `tools/probe_special_jpg.py` | 原型：逐块 Python 解码、DC 假设对比、样本 PNG |
| `manifests/batches/special-jpg-code-dump.txt` | 关键函数反汇编（8 个函数，约 2,000 行），供独立复核 |

## 6. 后续

1. **精确 IDCT/颜色转换复核**：由外部独立分析 `0x8a8780` 的 MMX 常数与
   `0x8a3b30/0x8a3590` 的查表逻辑，替换当前近似；
2. 批量导出：10 个样本 → 50 → 510 个 PNG（当前模块已能解码大图，0 错误）；
3. 若有运行截图/原图，做像素级对拍验证。
