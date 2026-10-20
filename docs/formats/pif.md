# PIF 格式

- 目标：`D:\software\dhsh\shdata\Game.exe`
  SHA-256 `f6bf66d24a3523bb6a30541589fabea36bbceec84355463d8e2c99e3b7f16be9`
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

各帧字段几乎完全相同；目前只能推断为**动画/特效参数**（位置、尺寸、速度等），
不能确认是逐帧锚点。APG 帧头本身已经给出 x/y/width/height。

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

## 3. 行字段语义（2026-10-20 更新）

每行 28 字节 = **14 个 uint16**，与 PG2 帧表项的前 28 字节布局一致。
`VA 0x824a20` 是逐行访问器：

```
row = base + index * 0x1c
delay = word[row + 0x0a]      // f5
if delay < 1: delay = 0x50    // 默认 80 ms
```

- **f5（第 6 个 u16）= 帧延时（毫秒）**；`0x824a20` 在值 <1 时返回默认 80；
- 消费方 `0x4ec1e0` 把每行 f5 存入延时数组；`0x452850` 同样读取该字段；
- 因此 PIF 的用途是**逐帧/逐动作延时表**，不是位置或锚点；
- 其余字段（f0–f4、f6 之后）仍待定，可能是命中框/画布参数。

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
- type 0x000A：仍未破解（11.3% 的 codec）。

B0 采样（655 个 APG 样本）：

- 3 个 standalone PIF 全部解析，width 与 APG 帧数匹配；
- 找到 7 个 0x0B perinfo PIF 并解析；
- 23 个 0x0A perinfo PIF 被阻塞；
- 其余 APG 目录没有 perinfo.dat（PIF 是可选的辅助数据）。

报告：`manifests/batches/pif-b0.json`。

## 5. 结论

- PIF 解析已完成（type 0/0x0B）；0x0A 仍待 codec。
- **f5 = 帧延时（毫秒）**，默认 80；PIF 是动画播放时序的辅助表。
- PIF 不是 APG 静态导帧的必需品：APG 帧头已包含位置/尺寸。
- 其余字段语义仍是推断；后续可结合 `perinfo.dat` 与 AG 辅助数据继续分析。

## 6. 代码

```python
from dhsh.formats.pif import parse_pif
pif = parse_pif(data)
print(pif.count, pif.width, pif.height, pif.flags)
for row in pif.rows:
    print(row[5], "ms")          # 帧延时
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

