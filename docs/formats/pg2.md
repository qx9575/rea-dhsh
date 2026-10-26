# PG2 图像容器

- 日期：2026-10-20
- 目标：`D:\software\dhsh\shdata\Game.exe`
  SHA-256 `f6bf66d24a3523bb6a30541589fabea36bbceec84355463d8e2c99e3b7f16be9`
- 当前实现：`tools/dhsh/formats/pg2.py`、`tools/export_pg2.py`
- 状态：**结构已解析；150 个文件 / 1,606 帧全部导出为 PNG（0 错误）；
  消费链已于当前 SHA `7a5e…` 复核（§6），f0–f5 确认无运行时消费者。**
- 注：§1/§2 的 VA `0x82cec0`/`0x82ce00` 为旧基线 `f6bf…`；当前构建
  对应 `FUN_0082d000`（分派器）/`FUN_0082ceb0`（帧迭代器），见 §6。

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
| 0x00 | u16 × 6 | 帧属性 f0–f5：**当前构建无运行时消费者**（见 §6）；数据画像：f0–f3 为编辑器矩形样式坐标（f0/f2 常见 320/252/160 成对、f1/f3 常见 352/160/407/210）、**f4 恒 5**、**f5 为延时样式值（80×1022、120×481、250×101、2000×2，与 PIF f5 同刻度）** |
| 0x0c | u16 | x（321/1,606 为 0） |
| 0x0e | u16 | y |
| 0x10 | u16 | width |
| 0x12 | u16 | height |
| 0x14 | u16 | width2 |
| 0x16 | u16 | height2 |
| 0x18 | u32 | reserved = 0 |
| 0x1c | u16 | aux0 = **帧槽序号**（150/150 文件均为顺序 0..n-1；迭代器读出后与位图数组上限做边界检查） |
| 0x1e | u16 | aux1 = 0（恒） |
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

## 6. 消费链与待办（2026-10-26 当前 SHA `7a5e…` 复核）

**消费链**（当前构建地址；旧文档 `0x82cec0`/`0x82ce00` 为旧基线）：

- `FUN_0082d000`：AG/PG 格式分派器（`0x4741`="AG"→type 1；
  `0x4750`="PG"→type 2）；PG 分支校验 `total_size/frame_count/version==1/
  header_size + entry_size×count ≤ total_size`，帧表指针存入对象 `+8`；
- `FUN_0082d120`：按路径装载资源并分派（`FUN_0080f9f0` 取数据）；
- `FUN_007f8b90`：主装载器，按扩展名分派 `.apg`/`.pg2`（字符串比较
  实证），解码为 FSAIndexBitmap/WAlphaBitmap 位图数组；
- `FUN_0082ceb0`：帧迭代器；PG 分支读出 `entry+0x1c`（aux0=帧槽序号，
  调用方 `FUN_007f8330` 用它做位图数组下标的边界检查）、`+0x20/+0x24`
  （AP 块位置/长度），并可经 `param_4` 导出 40 字节原始条目；
- `FUN_007f8330`：批量解码循环；`param_7 != NULL` 时把每帧 40 字节
  条目逐字导出给调用方——**但全部调用点（`FUN_007f8ae0` 的两个调用者
  `FUN_00809390`/`FUN_008ed650`，以及 `FUN_007f8b90` 的迭代循环）都传
  NULL**。

**结论：f0–f5（以及帧表的 x/y/w/h/w2/h2/aux1）在当前构建无运行时
消费者**。条目导出通道（`FUN_007f8330` 的 `param_7`）存在但全链路传
NULL；帧表对象随解码结束即释放（`FUN_0082cfc0`），运行时的位置/锚点
全部来自帧内 AP 块头（x/y）。f0–f5 属编辑期/制作侧元数据，还原可忽略
（数据画像见 §2：f5 为延时样式值，若自制播放器可参考）。

待办核销：

1. ~~帧表项前 12 字节（f0–f5）的语义~~：**无运行时消费者（2026-10-26
   闭合）**，数据画像 f0–f3 编辑器矩形、f4 恒 5、f5 延时样式；
2. ~~帧级合成~~：无帧表驱动的叠加——aux0 只是槽位序号（全部文件
   顺序 0..n-1），合成位置取 AP 块头锚点；
3. ~~与 APG 动画的关联~~：同一装载器按扩展名分派（`.apg`/`.pg2`），
   帧数据块同为 AP 格式，解码路径完全共用。
