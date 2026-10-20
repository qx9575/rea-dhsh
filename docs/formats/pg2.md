# PG2 图像容器

- 日期：2026-10-20
- 目标：`D:\software\dhsh\shdata\Game.exe`
  SHA-256 `f6bf66d24a3523bb6a30541589fabea36bbceec84355463d8e2c99e3b7f16be9`
- 当前实现：`tools/dhsh/formats/pg2.py`、`tools/export_pg2.py`
- 状态：**结构已解析；150 个文件 / 1,606 帧全部导出为 PNG（0 错误）。**

## 1. 文件头

小端序，由 `VA 0x82cec0` 的 PG 分支验证：

```
0x00  char[2]  "PG"
0x02  uint16   header_size = 20
0x04  uint16   frame_count
0x06  uint8    version = 1
0x07  uint8    reserved
0x08  uint32   total_size（等于文件大小）
0x0c  uint32   resource_id / 参数（0、10、2025 等）
0x10  uint16   frame_entry_size = 40
0x12  uint16   reserved
0x14  frame_count × 40 字节帧表项
```

解析器校验：

- `total_size == len(data)`；
- `header_size == 20`、`version == 1`、`entry_size == 40`；
- `header_size + frame_count * entry_size <= total_size`；
- `frame_count > 0`。

## 2. 帧表项（40 字节）

| 偏移 | 类型 | 观察/含义 |
|---:|---|---|
| 0x00 | u16 × 6 | 帧属性（资源 id、锚点等，语义待定） |
| 0x0c | u16 | x |
| 0x0e | u16 | y |
| 0x10 | u16 | width |
| 0x12 | u16 | height |
| 0x14 | u16 | width2 |
| 0x16 | u16 | height2 |
| 0x18 | u32 | reserved = 0 |
| 0x1c | u16 | aux0（帧迭代输出字段） |
| 0x1e | u16 | aux1 |
| 0x20 | u32 | data_offset（相对文件起始） |
| 0x24 | u32 | data_size |

`VA 0x82ce00` 的帧迭代读取 `entry + 0x1c/0x20/0x24`；数据指针 =
`base + data_offset`，`data_size` 为帧块长度。

## 3. 帧数据

每个帧块是一份完整的 **AP 块**（与 APG 内帧块相同）：

```
0x00  char[2]  "AP"
0x02  uint16   header_size = 24
0x04  int16    x
0x06  int16    y
0x08  uint16   width
0x0a  uint16   height
0x0c  uint16   flags
0x0e  uint8    compression
0x0f  uint8    alpha_mode
0x10  uint32   stream1_size
0x14  uint32   stream2_size
0x18  ...      stream1 + stream2
```

恒等式：`data_size == 24 + stream1_size + stream2_size`（1,606/1,606 验证）。

因此直接复用 A16 解码器：compression 0/1/2/5（5=JPEG）、alpha 0/2。

## 4. 语料扫描

三份导出语料：

| 语料 | 文件数 |
|---|---:|
| type 0（`extracted/misc`） | 38 |
| 0x0A（`extracted/decoded`） | 106 |
| 0x0B（`extracted/decoded_0b`） | 6 |
| **合计** | **150** |

- **1,606 帧**；每文件帧数 2–101，常见 10 帧（87 个）、8 帧（35 个）。
- 帧块 magic 全部为 `AP`；compression：1=6、2=1,165、5=435；
  alpha_mode 全部为 2（RLE8）。
- 头部/帧表/尺寸校验 **0 错误**。

## 5. 批量导出

工具：`tools/export_pg2.py`（8 进程；NumPy A16 快速路径）
输出：`extracted/pg2_png/<corpus>/<logical path>/<pg2 stem>/fNNN.png`

| 批次 | 文件 | 帧 | 输出 | 耗时 |
|---|---:|---:|---:|---:|
| B0 | 10 | 102 | 2.1 MiB | 3.3 s |
| B1 | 40 | 381 | 13.8 MiB | 5.7 s |
| 全量 | 150 | **1,606** | **66.1 MiB** | **16.1 s** |

- 全量 0 错误；磁盘 1,606 个 PNG，第 3 节恒等式全部成立。
- fast 与 reference 解码器在 46 个分层样本（comp 1/2/5，alpha 2）上
  **逐字节一致**。
- 抽查 40 个文件共 408 帧，Pillow 全部可打开。
- 清单：`manifests/batches/pg2-b0.csv`、`pg2-b1.csv`、`pg2-full.csv`。

## 6. 待办

1. 帧表项前 12 字节（f0–f5）的语义（资源 id/锚点/画布）；
2. 帧级合成（若同一 PG2 的多帧需要按 x/y 叠加）；
3. 与 APG 动画、地图背景的关联。
