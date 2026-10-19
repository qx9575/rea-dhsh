# APG 动画格式

- 目标：`D:\software\dhsh\shdata\Game.exe`
  SHA-256 `f6bf66d24a3523bb6a30541589fabea36bbceec84355463d8e2c99e3b7f16be9`
- 日期：2026-10-19
- 当前实现：`tools/dhsh/formats/apg.py`
- 状态：**结构 B0 通过（181/181 图像块）**；帧元数据/增量/混合语义仍在分析。

## 1. AG 文件头

小端序：

```
0x00  char[2]  "AG"
0x02  uint16   header_size        // 8 + 8 * frame_count
0x04  uint16   frame_count
0x06  uint16   flags / variant     // 常见 0 或 1
0x08  uint32   reserved = 0
0x0c  frame_count × {
          uint16 frame_id
          uint16 flags
          uint32 offset
      }
```

- 头部长度 = `8 + 8 * frame_count`（已在 0x28/0x48/0xA8/0xC8 等样本验证）。
- `offset` 指向帧数据/共享数据（多数落在某个帧块的 s2 内部，少数在 s1/块头/块外）。
- 同一动画中不同帧可能共享同一个 offset（例如 body 样本的 frame4/frame7 都是 0x926），
  说明 offset 指向的是可共享的 alpha/命令流位置，而不是唯一的帧记录。
- `frame_id` 通常为 0..frame_count-1；`flags` 多数为 0。

## 2. 帧图像块（AP / AI）

每个块是一个 A16 风格的头：

```
0x00  char[2]  "AP" 或 "AI"
0x02  uint16   header_size = 24
0x04  int16    x offset
0x06  int16    y offset
0x08  uint16   width
0x0a  uint16   height
0x0c  uint16   flags
0x0e  uint8    compression
0x0f  uint8    alpha_mode
0x10  uint32   stream1_size
0x14  uint32   stream2_size
0x18  ...      stream1，随后 stream2
```

- 块结束 = `block_start + 24 + stream1_size + stream2_size`。
- 下一个块从块结束处开始（已验证：`btnnextmon` 40→1648→3267→4871；
  `player6 body` 72→2434→...；`3.apg` 168→42005→...）。
- `AP` 与 `AI` 都按 A16 头处理；`AI` 只是另一种图像块标记。

## 3. AP / AI 的两种像素路径

| magic | 像素路径 | 状态 |
|---|---|---|
| `AP` | direct RGB565：compression 0/1/2/5（5=JPEG） | ✅ |
| `AI` | **8 位索引 + CTI 调色板**：compression 2 实际是 RLE8 索引流 | ✅（B1 84/84） |

`AI` 的识别方法：

- 对 `AI` 块，stream1 用 **RLE8** 解码，输出长度恰好 `width*height`；
- 解码结果作为索引，查同目录（向上最多 3 级）的 `clrtb.cti`；
- CTI `table1`（256×u16 RGB565）或 `table2`（256×u32）给出颜色；
- 默认使用 `table1`，`table2` 是另一套颜色（可能是 32 位/变体调色板，选择逻辑待定）。

已验证：宠物 36、头像 24962 等 AI 动画使用 CTI 后颜色正确（蓝色宠物、紫色头像）；
`table2` 会得到黄色/红色变体，说明两张表不是简单的 16/32 位转换。

`AI` 帧的 `x/y` 字段是画布锚点；当前导出忽略锚点，只输出图像块本身。

## 4. compression / alpha

| compression | 含义 | 状态 |
|---|---|---|
| 0 | 原始 RGB565 | ✅ |
| 1 | 单色 | ✅ |
| 2 | RLE16 | ✅（常见） |
| 5 | **stream1 = 完整 JPEG**；stream2 = alpha | ✅（3.apg 等） |
| 3/4/7/11 | 未实现 | ❌ |

| alpha_mode | 含义 | 状态 |
|---|---|---|
| 0 | 不透明 | ✅ |
| 2 | RLE8，0..32 缩放 | ✅（常见） |
| 1/3/4/5 | 未实现 | ❌ |

JPEG 帧验证：`3.apg` 第一帧 stream1 从 `FF D8` 到 `FF D9` 的长度恰好等于
`stream1_size`（28,789 字节）。

## 5. B0 / B1 结果

- **B0**：13 个动画、**181/181** 图像块解码成功；
  magic AP=173、AI=8；comp 2=158、1=3、5=20；alpha 2=168、0=13；
  AI 宠物通过 CTI 正确上色。
- **B1**：53 个动画、**470/470** 帧解码成功；
  magic AP=386、AI=84；comp 2=435、1=15、5=20；alpha 2=457、0=13；
  13 个含 AI 的样本全部找到 CTI（missing=0）。
- **B2**：134 个动画、**1,000/1,000** 帧解码成功；
  magic AP=743、AI=257；comp 2=991、1=9；alpha 2=1,000；
  34 个含 AI 的样本全部找到 CTI（missing=0）。
- 输出：`extracted/_batches/s4-apg-b0/`（PNG 帧 + contact sheet）。
- 角色身体/头像、宠物、UI、JPEG 大动画帧均已正确解码。

## 6. 逆向进展（AG/PG 加载器）

已定位的关键代码（Game.exe，静态分析）：

| 功能 | 地址/说明 |
|---|---|
| AG/PG 头解析 | VA `0x82cec0`；AG magic 检查 `0x82cf54`；对象 +0x18=1 表示 AG，2 表示 PG |
| AG 帧迭代 | VA `0x82cd70`：读取 `data + 8 + i*8`，返回 `(frame_id, flags, offset)` |
| PG 帧迭代 | VA `0x82ce00`：使用 40 字节帧表项（+0x1c、+0x20、+0x24 等） |
| APG 文件加载/图像装载 | VA `0x7f8210`；帧图像加载 `0x7f8630`；文件读取 `0x80f8b0` |
| APG 路径构造 | `%s/%d.apg` ref `0x8ed6d1`；`animation\%d.apg` ref `0x40f01d` |

AG 帧迭代已经确认了 index 条目结构；**offset 指向共享 alpha/命令流位置**，
具体命令语义与混合模式仍待继续解析。

## 7. 已知问题（重要）

1. **AG 帧元数据/命令流未完全解码**：index 条目已确认
   `(u16 frame_id, u16 flags, u32 offset)`，但 offset 指向的共享 alpha/命令流
   尚未解析；目前未用于合成。
2. **混合模式**：部分特效帧（魔法、光效）需要加法/特技混合；当前输出是普通
   RGBA，黑底在游戏内可能被混合掉。
3. **CTI table2**：与 table1 颜色差异明显，尚未确认选择条件；当前默认 table1。
4. **AI 锚点**：`x/y` 字段未用于画布合成。
5. 当前 APG 解析适合导出图像块/单帧；要按游戏内效果播放，仍需帧元数据/混合语义。

## 8. 下一步

1. B2：1,000 帧 APG 解码 + CTI 解析回归；
2. 用 REA + Ghidra 定位 APG 加载/播放函数（字符串 `AG`、`AP`、`AI`、
   `animation\%d.apg`、`FSPlayer::LoadPicture`），解析 AG index/metadata；
3. 确认混合模式与 CTI table1/table2 选择；
4. 解析 PIF，生成 GIF/WebM；
5. 全量导出 type=0 APG（207,156 个）与 0x0B APG（204,137 个）。
