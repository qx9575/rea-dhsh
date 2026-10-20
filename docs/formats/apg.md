# APG 动画格式

- 目标：`D:\software\dhsh\shdata\Game.exe`
  SHA-256 `f6bf66d24a3523bb6a30541589fabea36bbceec84355463d8e2c99e3b7f16be9`
- 日期：2026-10-19
- 当前实现：`tools/dhsh/formats/apg.py`
- 状态：**0x0A 47,049 文件/449,011 帧、0x0B 204,137 文件/1,824,600 帧全部解析；
  模式/CTI 覆盖 100%；已导出 449,011 + 1,824,600 张 PNG（0 错误）**；
  AG offset/混合语义与 type 0 语料仍待处理。

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
| 3/4/7/11 | 未实现（0x0A 语料未出现；type 0/0x0B 待查） | ❌ |

| alpha_mode | 含义 | 状态 |
|---|---|---|
| 0 | 不透明 | ✅ |
| 2 | RLE8，0..32 缩放 | ✅（常见） |
| 1/3/4/5 | 未实现（0x0A 语料未出现；type 0/0x0B 待查） | ❌ |

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

AG 帧迭代已经确认了 index 条目结构。进一步验证：

- 采样样本中 `flags` 全部为 0，未发现帧级混合/增量标志；
- 用 AG offset 作为 alpha 起点解码会得到错误的拼接/黑块图像；
- 用每个图像块自己的 s2 起点解码 alpha 才是正确的（宠物/头像/角色/按钮均已验证）；
- 因此 **AG offset 不是每帧 alpha 起点**，更像共享/辅助数据指针；当前静态导帧可以安全忽略它。

混合模式方面：

- AP/AI 块头与 AG 头的 flags 与“特效/普通帧”没有明显对应关系；
- 魔法/光效的黑底很可能由**外部技能/动作配置**决定（渲染时加法混合），不是 APG 文件内的标志；
- 当前导出保留原始 RGBA 帧数据，游戏内混合策略需在渲染层另行实现。

## 7. 0x0A 语料全量扫描（2026-10-20）

对 `extracted/decoded/` 下全部 0x0A APG 做结构扫描（`tools/scan_apg.py`）与 CTI
覆盖检查（`tools/check_apg_cti.py`），报告 `manifests/batches/apg-0a-scan.json`、
`apg-cti.json`。

- **47,049 个文件、449,011 帧，0 解析错误**（合计 3,222,416,327 字节）。
- 块类型：`AP` 79,197、`AI` 369,814（AI 占 82.4%）。
- compression：2=446,158、1=2,310、0=543；**没有 3/4/7/11**。
- alpha_mode：2=448,399、0=612；**没有 1/3/4/5**。
- 帧头长度全部 24；AG index flags 全部 0。
- AG header flags：0=8,335、1=38,714；交叉统计 **flags=1 当且仅当文件含 AI 帧**
  （38,714/38,714），flags=0 全部为纯 AP 文件。可将该字段理解为“使用 AI/CTI 调色板”。
- 帧数分布：8 帧 31,448 个文件（最多），其次 10 帧 3,406、12 帧 2,761、18 帧 2,482、
  14 帧 966、4 帧 860、19 帧 857 等。
- **CTI 覆盖 100%**：38,714 个含 AI 的文件、369,814 个 AI 帧全部能在同 PAK 内
  找到 `clrtb.cti`（沿逻辑目录向上查找），0 缺失；另存在 1 个 `121clrtb.cti` 变体。
- 分层抽样解码：player/npc/magic/ui 共 100 个文件、886 帧，
  **886/886 成功**（AI/2/2 436、AP/2/2 440、AP/1/2 10），0 失败。

## 8. 已知问题（重要）

1. **AG offset 的辅助语义未完全解码**：index 条目已确认
   `(u16 frame_id, u16 flags, u32 offset)`；offset 不是每帧 alpha 起点，
   当前静态导帧忽略它即可。其真实用途（时序/锚点/共享子帧）仍待确认。
2. **混合模式在 APG 之外**：特效帧的黑底很可能由技能/动作配置的加法混合决定；
   APG 文件内未发现帧级 blend 标志。当前导出为原始 RGBA。
3. **CTI table2**：与 table1 颜色差异明显，尚未确认选择条件；当前默认 table1。
4. **AI 锚点**：`x/y` 字段未用于画布合成。
5. 0x0A APG 已批量转帧（**449,011/449,011 PNG**，3,175.6 MiB，0 错误）；
   type 0 与 0x0B APG 尚待导出/转帧。
6. **AG header flags 已解释**：`1` = 文件含 AI（CTI 调色板）帧，`0` = 纯 AP；
   该判断在 0x0A 语料 47,049 个文件上完全成立（见第 7 节）。

## 9. 下一步

1. 0x0A/0x0B APG 转帧已完成（449,011 + 1,824,600 PNG）；下一步导出 type 0 APG（207,156 个）后复用本工具；
2. 继续解析 AG offset 的辅助语义（时序/锚点/共享子帧）；
3. 确认混合模式与 CTI table1/table2 选择；
4. 解析 PIF，生成 GIF/WebM；
5. 0x0B APG 已完成转帧（**1,824,600 PNG**）；type=0 APG（207,156 个）待导出后转帧。

## 10. APG 批量转帧工具（0x0A，2026-10-20）

- 工具：`tools/export_apg.py`；快速像素路径：`tools/dhsh/formats/apg_fast.py`
- 输出：`extracted/apg_frames/<pak>/<logical dir>/<apg stem>/fNNN.png`
- 清单：`manifests/batches/apg-frames-*.csv`（每个 APG 一行）

性能优化：

- **NumPy RLE run 展开**：按控制码切片赋值，不再逐像素 Python 循环；
- **RGB565 65536 色 LUT** + **CTI 256 色 LUT**，AI/AP 共用一次查表；
- 父进程只建一次 CTI 索引，通过 `Pool(initializer=...)` 共享给 worker；
- 8 进程、按 APG 分块、PNG `compress_level=3`（基准速度/体积最平衡）；
- 快速路径与参考解码器 **886/886 帧逐像素一致**，解码约 **4.8×**。

已跑批次：

| 批次 | APG 文件 | 帧 | 输出 | 耗时 |
|---|---:|---:|---:|---:|
| B0 | 20 | 186 | 2.9 MiB | 4.3 s |
| B1 | 200 | 1,910 | 12.4 MiB | 8.8 s |
| B1b | 1,000 | 9,615 | 68.7 MiB | 25.4 s |
| 全量 | 47,049 | 449,011 | **3,175.6 MiB** | **838.6 s** |

全量校验：449,011/449,011 张 PNG，0 错误；30 个随机 APG 的 304 帧尺寸全部与
帧头一致；输出 `extracted/apg_frames/`，清单 `manifests/batches/apg-frames-full.csv`。
按 8 进程约 535 帧/s、3.8 MiB/s 的端到端速度。

> 全量输出约数 GiB、数十万 PNG；建议先按 `--pak`/`--sample` 分片，再跑 `--all`。

## 11. 0x0B APG 语料扫描与转帧（2026-10-20）

- **204,137 个 APG、1,824,600 帧，0 解析错误**；像素 4.93B（raw RGBA 约 18.4 GiB）。
- 块类型：AP 256,639、AI 1,567,961（85.9% AI）。
- compression：2=1,823,240、1=1,360；alpha_mode：2=1,824,581、0=19。
- AG flags：0=29,122、1=175,015；**flags=1 当且仅当含 AI 帧**（175,015/175,015），
  与 0x0A 语料结论一致。
- **CTI 覆盖 100%**：175,015 个 AI 文件 / 1,567,961 帧；174,975 个 same-pak，
  40 个 `Lcuff/521` 文件使用 **sibling-palette** 回退到 `Rcuff/521/clrtb.cti`
  （同 ID 的 L/R/cloth 调色板在 471/571/621/671 上逐字节相同）。
- 转帧工具复用 `export_apg.py`：
  `--root extracted/decoded_0b --cti-roots extracted/decoded_0b extracted/decoded extracted/images-proprietary`
- 样本批次：B0 20 APG/175 帧/0.5 MiB/5.1s；B1 200/1,678/4.3 MiB/10.9s；
  B1b 1,000/8,770/26.9 MiB/19.9s。
- **全量**：204,137 个 APG → **1,824,600/1,824,600 张 PNG**，4,748.0 MiB，
  0 错误，**1,977.8 s**（约 923 帧/s）；输出 `extracted/apg_frames_0b/`，
  清单 `manifests/batches/apg-0b-frames-full.csv`。
- 校验：磁盘 PNG 数与 manifest 一致、0 部分写入；10 个随机 APG + 4 个
  `Lcuff/521` 文件共 144 帧与参考解码器逐像素一致（0 差异）。
