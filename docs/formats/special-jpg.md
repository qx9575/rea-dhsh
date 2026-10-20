# 特殊 JPG（FF D8 FF C8）格式分析

- 日期：2026-10-20
- 目标：`D:\software\dhsh\shdata\Game.exe`
  SHA-256 `f6bf66d24a3523bb6a30541589fabea36bbceec84355463d8e2c99e3b7f16be9`
- 状态：**容器结构已解析（510/510 校验通过）；熵编码为私有格式，尚未实现解码。**

## 1. 语料与识别

在 `extracted/images-standard` 的 544 个 `.jpg` 中：

- 34 个是标准 JPEG（Pillow 可打开）；
- **510 个**以 `FF D8 FF C8` 开头，Pillow 无法识别；主要位于 `map/`（地图背景、
  小地图/缩略图等）。

`Game.exe` 的加载分支（VA `0x8238d0`）：

1. 读取首字（小端）并比较 `0xD8FF`（即 `FF D8`）；
2. 若偏移 2 处的字为 `0xC8FF`（即 `FF C8`），设置标志并调用
   **自定义解码器 VA `0x8a3dd0`**；
3. 否则走标准 JPEG 解析器 VA `0x8229d0`。

因此 `FF C8` 不是标准 JPEG 的 SOF 标记，而是该私有格式的标识。

## 2. 容器结构（510/510 验证）

所有数字均为小端：

```
0x000  char[2]   "FF D8"            // 与标准 JPEG 相同的 SOI
0x002  char[2]   "FF C8"            // 私有格式标识
0x004  uint32    header_size = 1000 // 头部/表区大小（实测全部为 1000）
0x008  uint32    version            // 2 = 427 个，3 = 83 个
0x00c  uint32    width
0x010  uint32    height
0x014  uint8     bpp = 8
0x015  uint16    width  (重复)
0x017  uint16    height (重复)
...
0x008+548 (=0x22c)  四组 Huffman 表（见下）
...                  表区结束位置因文件而异（0x3B8–0x7BC 等）
...                  每个 MCU 6 字节描述符
...                  uint32 data_size
...                  data_size 字节的私有熵编码数据，直到文件末尾
```

字段 `header_size` 与表区结束并不总是相等：1,000 只是固定头部大小，
Huffman 表是变长的，必须按下面的算法逐项解析才能找到 MCU 描述符起点。

### 2.1 548 字节固定头

VA `0x8a3dd0` 入口先把文件偏移 `0x008` 开始的 **137 个 dword（548 字节）**
复制到对象 `+0x04`，然后指针推进到 `0x22c`。

观察到的字段：

| 偏移 | 类型 | 值/含义 |
|---:|---|---|
| 0x008 | u32 | version（2/3） |
| 0x00c | u32 | width |
| 0x010 | u32 | height |
| 0x014 | u8 | 8（bpp） |
| 0x015 | u16 | width |
| 0x017 | u16 | height |
| 0x019 | u8 | 3（组件数） |
| 0x01a | u16×64 | 量化表 1（16 位量化值） |
| 0x09a | u16×64 | 量化表 2 |
| 剩余 | — | 组件/扫描相关字段与固定表，语义待定 |

### 2.2 Huffman 表

从偏移 `0x22c` 开始，按 **2 组 × 4 个数组** 解析（VA `0x8a3dd0` 的循环）：

```
for group in 0..1:
    for array in 0..3:
        count = u32
        if count > 0:
            entries[count] = { u16 code, u8 length, u8 symbol }
```

- 每项 4 字节；`code` 是**规范 Huffman 码值**（低位对齐），`length` 为码长；
- 实际语料中有效表为 4 个：DC 表 10/9 项，AC 表 49/36 项，
  另外两个数组 count=0；
- 对 510 个文件逐项校验，码值全部符合 canonical Huffman 规则，
  `length` 范围 2..16，`symbol` 是标准 JPEG 的 DC/AC 符号
  （AC 表包含 `0x00`、`0xF0`、`0x0F` 等 run/size 值）。

### 2.3 MCU 描述符

表区结束后：

```
mcu_cols = width  / (version == 2 ? 16 : 8)
mcu_rows = height / (version == 2 ? 16 : 8)
mcu_count = mcu_cols * mcu_rows
descriptors = mcu_count × 6 字节
```

每个描述符被 VA `0x8a3dd0` 拆成 5 个字段写入 12 字节的 MCU 记录：

- `dword & 0x3FF`
- `byte[4] & 0x1F`
- `signed16(word[4]) >> 5`
- `signed32(dword << 11) >> 21`
- `signed32(dword) >> 21`

后续 MCU 的记录会累加前一 MCU 的字段（差分/偏移语义，具体含义待定）。

### 2.4 数据区

描述符之后：

```
uint32 data_size
data_size 字节熵编码数据
```

对 510 个文件验证：按上述动态表区解析后，
`表区末尾 + mcu_count*6 + 4 + data_size == 文件长度`，**510/510 全部成立**。

## 3. 为什么不能直接还原为标准 JPEG

已尝试根据解析出的量化表 / Huffman 表重建标准 JPEG（SOI + DQT(16-bit) +
DHT + SOF0 4:2:0 + SOS + 数据区），libjpeg 能在头部阶段识别，但熵解码报
`broken data stream`。

观察到的差异：

- 数据区**不含** `FF DA`、`FF D9`、`FF 00` 转义；168 个 `FF` 字节均未跟随 `00`；
- 数据区是 6 字节/MCU 的描述符之后的私有码流，不是标准 JPEG 的 MCU 熵编码；
- 6 字节/MCU 描述符超出标准 JPEG 所需，说明解码器使用了额外的每 MCU 状态
  （可能是差分 DC、位偏移或上下文）。

因此需要移植/仿真自定义解码器，不能用现成 JPEG 库直接解码。

## 4. 已定位代码

| 功能 | VA |
|---|---|
| JPG 加载入口（文件读取后调用） | `0x822900` |
| 格式分派（FF D8 / FF C8） | `0x8238d0` |
| 标准 JPEG 标记解析 | `0x8229d0` |
| **私有格式头部/表解析** | `0x8a3dd0` |
| 表查找/哈希辅助（含 `0xdeadbeef` 掩码） | `0x8a4060` |
| 32 位码流/Huffman 辅助 | `0x8a4110`、`0x8a4200`、`0x8a4310`、`0x8a4360` |

## 5. 后续方案

1. **移植熵解码器**：沿 `0x8a3dd0` 的调用链继续反编译，重点确认
   - 位读取顺序与 32 位缓冲布局；
   - 6 字节 MCU 描述符的语义；
   - 反量化/IDCT/YCbCr 转换路径。
2. **Unicorn 局部仿真**：只加载 `0x8a3dd0` 及其调用函数，桩掉 malloc/free/打印，
   用原始文件缓冲作为输入，输出 RGBA。仅做静态代码仿真，不运行游戏本体。
3. 先做 5–10 个样本（不同 version、尺寸）验证，再批量 510 个。

在解码器完成前，这 510 个文件保持原始 `.jpg` 导出，不参与 PNG 批量转换。
