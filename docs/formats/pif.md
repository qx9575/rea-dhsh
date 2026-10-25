# PIF 格式

- 目标：`D:\software\dhsh\shdata\Game.exe`
  SHA-256 `7a5e2c085ac79557ca104a2917e06f1a7e5f2827e4b68753bbaf1c754a4e78dc`
  （2026-10-21 更新；旧基线 `f6bf66d2…`）
- 日期：2026-10-19
- 实现：`tools/dhsh/formats/pif.py`
- 方法：静态反汇编 + 样本验证；未运行游戏。

## 1. 结构

PIF 是通用的“行 × 字节”表/图像容器，小端序：

```
0x00  char[4]  "PIF\0"
0x04  uint32   count        // 实测 2 或 3；解析器接受 <=3
0x08  uint16   data_offset  // <1 时按 20 处理
0x0a  uint16   height       // 每行字节数（常见 24/28）
0x0c  uint16   width        // 行数
0x0e  uint16   flags        // 实测 2
0x10  uint32   reserved     // 实测 0
0x14  ...      width * height 字节
```

- 数据区从 `data_offset` 开始，共 `width * height` 字节。
- 每行 `height` 字节；`height` 为偶数时按 `height/2` 个 uint16 读取。
- Game.exe 解析器：VA `0x824940`（校验 magic/count、复制 20 字节头、分配并复制
  `width*height` 数据）；文件加载器：VA `0x824770`。

## 2. 两种实际用法

### A) 逐帧表：`shdata/animation/1-3.dat`

- `width` = APG 帧数；
- `height` = 28；
- 每行 = 一帧的 28 字节（14 个 uint16）。

| 文件 | PIF width | 对应 APG | APG 帧数 | 匹配 |
|---|---:|---|---:|---|
| `1.dat` | 44 | `1.apg` | 44 | ✅ |
| `2.dat` | 26 | `2.apg` | 26 | ✅ |
| `3.dat` | 20 | `3.apg` | 20 | ✅ |

示例行：

```text
1.dat: [160,160,160,210,5,200,0,0,0,0,0,0,0,0]
2.dat: [160,160,160,210,5, 80,0,0,0,0,0,0,0,0]
3.dat: [160,160,160,210,5, 80,0,0,0,0,0,0,0,0]
```

各帧字段几乎完全相同；在 `animation/1-3` 这类 APG 播放中主要提供
帧延时，x/y 偏移不参与 APG 导帧（APG 帧头已有 x/y）。

### C) 地图 A16 动画：`map/animate/**/perinfo.dat`

- `width` = 同目录 A16 帧数（如 `YingTianFu/duchang/denglong1` 9 帧）；
- `height` = 28（`count=3`，595/607 文件）或 24（`count=2`，12 个
  `fire00-09`/`water02-03`）；`count<3` 时 vtable 方法 `0x824ba0`
  会把每行按**源 stride=header.height** 复制到新的 28 字节行
  （尾部补 0），并把 header 改写为 `count=3/height=28`，因此后续
  访问器统一按 28 字节工作；
- 地图动画初始化器 `FUN_00431fc0` 对每一帧：
  - `row + 0x04`（第 3 个 u16）= **x 偏移**，`row + 0x06`（第 4 个）
    = **y 偏移**，作为 `WObject::AddPos(x, y)` 加到动画对象位置；
  - `row + 0x0a` = 帧延时（`FUN_00824b60`，0→默认 80 ms）；
  - `row + 0x00/+0x02` 是另一组坐标样值（常相等，如 160/160、
    252/252、50/50、40/40），`row + 0x08` 常为 5，其余列全 0。
- 607 个地图 perinfo：593 个所有行相同，14 个只有延时列变化；
  延时观测 0..400 ms。**x/y 在每个动画内为常量**，不是逐帧轨迹。

### B) 逐 APG 动作表：`perinfo.dat`

- `width` = 同目录下 `.apg` 文件数；
- `height` = 28（少数 24）；
- 每一行对应一个 APG 动作文件。

验证：

| 路径 | PIF width | 同目录 APG 数 | 匹配 |
|---|---:|---:|---|
| `npc2.pak:26139/10/perinfo.dat` | 8 | 8 | ✅ |
| `npc2.pak:26139/40/perinfo.dat` | 2 | 2 | ✅ |
| `magic4.pak:1388/0/perinfo.dat` | 1 | 1 | ✅ |

示例行：

```text
perinfo: [252,252,252,315,5,120,0,0,0,0,0,0,0,0]
npc2:    [160,160,160,210,5, 80,0,0,0,0,0,0,0,0]
```

同样，字段大多为常量；语义待确认。

## 3. 行字段语义（2026-10-23 更新）

每行 28 字节（count=3）= 14 个 uint16；count=2 为 24 字节 =
12 个 uint16，前 6 列布局相同。逐行访问器（旧基线 `0x824a20`）：

```
row = base + index * stride      // stride = header.height（28 或 24）
delay = word[row + 0x0a]
if delay < 1: delay = 0x50        // 默认 80 ms
```

| 偏移 | 字段 | 证据 |
|---|---|---|
| +0x00, +0x02 | 一组坐标样值（常相等） | 语料统计；语义待定 |
| +0x04 | **x 偏移（u16）** | `FUN_00431fc0` 读 `*(ushort*)(row+4)` → `WObject::AddPos` |
| +0x06 | **y 偏移（u16）** | 同上，读 `row+6` |
| +0x08 | 常为 5，少量 0 | 语料统计；语义待定 |
| +0x0a | **帧延时（ms）**，0→80 | `FUN_00824b60` 返回 `row+0x0a`，0 时默认 0x50 |
| +0x0c… | count=3 时全 0；count=2 行到 +0x16 | 语料统计 |

- `FUN_00824b60` 的延时读取使用 28 字节 stride；`count=2` 的 24 字节行
  在解析时已由 `0x824ba0` 归一化为 28 字节（尾部补 0），所以不存在
  单独的延时分支。当前 `parse_pif` 按 header.height 原始切行，读取
  前 6 列的结果与归一化后一致。
- 地图 perinfo 的 x/y 是**动画级常量偏移**，不是逐帧锚点；逐帧位置仍
  由 A16 帧头 x/y 提供。

standalone 动画验证：

| 文件 | PIF width | APG 帧数 | PIF f5 | GIF 总时长 |
|---|---:|---:|---:|---:|
| `animation/1.dat` | 44 | 44 | 200 | 8,800 ms |
| `animation/2.dat` | 26 | 26 | 80 | 2,080 ms |
| `animation/3.dat` | 20 | 20 | 80 | 1,600 ms |

`perinfo.dat` 的行对应同目录 APG 动作；f5 仍可按延时解释。

## 4. NZ 类型与覆盖

`perinfo.dat` 本身可能再包一层 NZ：

- type 0x0000：原样 PIF；
- type 0x000B：LZO1X 压缩，可用 `tools/dhsh/nz_lzo.py` 解码；
- type 0x000A：16 位 LZW，已破解（`tools/dhsh/nz_lzw10.py`）。

B0 采样（655 个 APG 样本）：

- 3 个 standalone PIF 全部解析，width 与 APG 帧数匹配；
- 找到 7 个 0x0B perinfo PIF 并解析；
- 23 个 0x0A perinfo PIF 在当时的批次被阻塞（0x0A codec 后续已破解）。
- 其余 APG 目录没有 perinfo.dat（PIF 是可选的辅助数据）。

报告：`manifests/batches/pif-b0.json`。

## 5. 结论

- PIF 解析已完成（type 0/0x0A/0x0B）。
- **f5 = 帧延时（毫秒）**，默认 80；地图 A16 动画中
  **f2/f3（+0x04/+0x06）= x/y 偏移**，加到动画对象位置。
- PIF 不是 APG 静态导帧的必需品：APG 帧头已包含位置/尺寸；地图 A16
  动画才使用行内 x/y 偏移。
- `count=2` 的 24 字节行在加载时由 `0x824ba0` 归一化为
  count=3/28 字节（尾部补 0），不存在独立延时分支；
  其余字段（+0/+2/+8）语义仍是推断。

## 6. 代码

```python
from dhsh.formats.pif import parse_pif
pif = parse_pif(data)
print(pif.count, pif.width, pif.height, pif.flags)
for row in pif.rows:
    print(row[2], row[3], "offset")   # x=+0x04 (index 2), y=+0x06 (index 3)
    print(row[5], "ms")               # 帧延时 (+0x0a)
```

## 7. GIF 导出

`tools/export_gif.py` 把 APG 帧按帧头 x/y 合成到画布，生成 GIF：

- 每帧 RGBA 先解码，画布取所有帧矩形的并集；
- 延时优先使用同目录 PIF（`width == frame_count`）的 f5，否则默认 80 ms；
- AI 帧沿用 `check_apg_cti` 的 CTI 解析；
- AI/JPEG/RLE 全部复用 `apg_fast`。

验证批次（输出在 `extracted/_batches/`）：

| 批次 | 文件 | 帧 | 结果 |
|---|---:|---:|---|
| B0 | 10 | 80 | 0.1 MiB，0 错误 |
| B1 | 50 | 413 | 0.7 MiB，0 错误 |
| animation/1-3 | 3 | 90 | 4.7 MiB，3/3 使用 PIF 延时 |

GIF 编码时 Pillow 会合并完全相同的相邻帧，但总时长保持不变
（例如 `1.apg` 42 个 GIF 帧、总时长 8,800 ms）。

