# perinfo.pak / APCI 容器（已解析）

- 日期：2026-10-20
- 目标：`D:\software\dhsh\shdata\perinfo.pak`（7,808,665 字节，
  SHA-256 `bf7e0e9f5456d3c9…`）；代码证据来自
  `shdata\Game.exe`（SHA-256 `f6bf66d2…16be9`）
- 实现：`tools/dhsh/formats/apci.py`、`tools/export_apci.py`
- 状态：**完整遍历成功，解析恰好消耗到文件最后一个字节；687,229 个 item、
  219 条 record 全部导出，0 错误。**

## 1. 文件头

```text
0x00  char[4]  "APCI"
0x04  uint32   version = 0x101
0x08  group 序列
```

`Game.exe`：

- 文件加载器 VA `0x82a6d0`：`fopen`/`fseek`/`ftell`/`malloc(size+1)`/
  `fread` 后调用解析器 `0x82a460(buffer, size, version_out)`，
  失败重试 4 次（调用点 `0x7ed919`，参数 `"perinfo.pak"`, `version_out=NULL`）；
- 解析器 VA `0x82a460` 校验 `APCI` 与 `0x101`，最多 20 个 group；
- 每个 group 的记录数组由辅助函数 `0x82a210` 维护（`count*0x2c+4`，
  记录在内存中 44 字节：36 字节头 + 指针 + 对齐）；
- 分配器 VA `0x826e70`（`ecx` = 上下文，跟踪分配块）。

## 2. 容器布局（已验证）

```text
group (最多 20 个，游标 < end 时继续):
    u16 group_id          # 本文件中等于 group 序号
    u16 record_count      # 可以为 0（头部仍占 4 字节）
    record × record_count:
        u16 record_id
        u16 sub_count
        char[32] name     # GBK，NUL 填充
        sub_count × entry:
            u32 a
            u32 b         # mid_count = b & 0xffff（高 16 位本文件恒 0）
            mid_count × mid:
                u32 header            # item_count = header >> 16
                item_count × item:
                    u32 x
                    u32 y
                    blob (可选)
```

item 的 blob 大小（VA `0x82a5fc`–`0x82a60f`）：

```text
width = x & 0xffff
if width == 0:                       # 无 blob（仅 8 字节 item）
    size = 0
elif ((y >> 16) & 0xff) == 1:        # byte[y+2] == 1
    size = (x >> 24) & 0xff
else:
    size = (((x >> 24) & 0xff) * width) & 0xffff   # movzx ebx, ax 截断
```

- `0x82a54c` 读取的“模式”实际是解析器第 3 个栈参数 `version_out`：
  加载器传入 `NULL` 时走 4 字节 entry 分支，非空时走 8 字节分支；
  但 `[esp+0x3c]` 在该处指向 **第 1 个参数 buffer**（非空），因此
  `perinfo.pak` 实际执行 **8 字节 entry 分支**（`0x82a56f`）。
- 之前的移植版本误用 4 字节 entry，且未做 `& 0xffff` 截断，
  才会在 `player1\Rcuff` / `player1\effect` 附近错位；这些字符串是
  **record 名称**，不是额外层级。
- `parse_scp` 无关；本次解析与 SCP 阶段相互独立。

## 3. 解析结果

| 项 | 数值 |
|---|---:|
| 文件大小 | 7,808,665 B |
| group 头 | 20（0–19） |
| 有记录的 group | 14（0–4、7–15） |
| 空 group | 5、6、16–19（各 4 字节头） |
| record | **219**（203 个不同名称） |
| entry | 22,341 |
| mid | 87,676 |
| item | **687,229** |
| 有 blob 的 item | 432,838 |
| blob 总字节 | **1,773,429** |
| 解析结束位置 | 7,808,665（= 文件末尾） |

record 名称按部位分组：`player\head`、`body`、`weapon`、`pet`、
`magic`、`cloth`、`effect`、`mask`、`Lcuff`、`shoe`、`Rcuff`、
`horse`、`harmorbody`、`harmorhead`，以及 `npc`/`npc1`…/`magic1`… 等；
同一名称可在多个 group 出现（如 group 0 与 1 都有 `player\head`），
需以 `(group, record)` 定位。

字段观察（2026-10-21 已赋予语义，Ghidra 批次 42-50 + 全量 CSV 验证）：

| 字段 | 语义 | 证据 |
|---|---|---|
| `entry.a` | **动作/动画 ID（部分记录）** | 常见 200/250/201/251、101500+；但并非所有 entry 都落在 `FUN_007ed6d0` 的 4 个区间（100..9999、40000..49999、61500..64999、101500..103511）。全量统计：**9,364/22,341 entry（41.9%）、211,310/687,229 item（30.7%）越界**。不能写“完全吻合”，需按 record 类型/调用者区分或追实际入参 |
| `entry.b` 高 16 位 | 保留（全 0） | 解析器只取低 16 位 |
| `entry.b` 低 16 位 | mid 数 | 解析器 `0x82a460` |
| `mid.header` 高 16 位 | **方向数** | 按 item 加权 **683,352/687,229 = 99.44%** 为 8；按 mid 计数 85,419/87,676 = 97.43%；按 entry（首个 mid）91.56%。旧文档 99.4% 是 item 加权口径 |
| `mid.header` 低 16 位 | **每方向帧数** | 10/30 最多，其次 90/20/60/80/50/70/40/100/101 |
| `item.x` 低 16 位 | 矩阵布局时的行数（= 方向数） | 8 最多 |
| `item.x` 高字节 | 每单元字节数 | 1 = 帧索引字节（579,741 item）；12 = 6×u16 锚点/坐标记录（107,407）；另有 81 个 item 为 0（空/边界记录） |
| `item.y` 字节 2 | 布局标志：1 = 一维 blob；0 = 矩阵 blob | 全部 item：1=424,876、0=262,353；有 blob 的 432,838 item：1=424,876、0=7,962 |
| blob | 逐（方向×帧）单元数据 | 大小公式 100% 验证（687,229/687,229）：`y_hi8==1 → x_hi8`；否则 `x_hi8 × x_lo16` |

**blob 内容**：1 字节单元 = 帧索引（如 `0b`）；12 字节单元 = 6×u16
锚点组（样本 `a000 a000 0000 c800 a000 d200` = (160,160),(0,200),(160,210)，
随 entry 变化的第 4 值为锚点/偏移参数）。

**解析器→内存布局**（`FUN_0082a460`，APCI v0x101 = 8 字节 entry）：
record 0x2c 内存步长（36B 文件头 + 子表指针）；entry 16B（a, b, mid 表指针）；
mid 12B（header, item 表指针）；item 16B（x, y, blob 指针，blob 就地截断 & 0xffff）。

**消费链**：`FUN_007ed8a0` 启动加载 "perinfo.pak" → 全局 `DAT_00ae4b08`
（0xac 字节对象）→ 21 个消费者经 `FUN_007ec2e0()` 取用；核心查询引擎
`FUN_008283d0`（动作→动画帧序列选择，被渲染器 `FUN_00803900` 等调用，
输出 289 dword 部位帧表；`FUN_00805b10` 场景渲染族驱动）。

## 4. 导出

`tools/export_apci.py`：

| 批次 | 范围 | 输出 |
|---|---|---|
| B0 | `--limit-items 1000` | 1,000 item、598 blob、0.6 KB |
| 全量 | `--all` | 687,229 item、432,838 blob、1.77 MB blob 数据，14.1 s |

- `extracted/apci/perinfo/items.csv`（49.3 MiB）：逐 item 的
  group/record/entry/mid 索引、x/y/width/bpp/flags、blob 偏移与大小；
- `extracted/apci/perinfo/blobs.bin`（1.77 MiB）：按遍历顺序拼接的 blob；
- `extracted/apci/perinfo/records.csv`（10 KB）：每个 record 的
  sub/entry/mid/item/blob 计数；
- 清单 `manifests/batches/apci-b0.json`、`apci-perinfo.json`
  （含输入 SHA-256、分组统计、每 record 汇总）。

复现：

```bash
python tools/export_apci.py --limit-items 1000 \
    --out extracted/_batches/apci-b0 --manifest manifests/batches/apci-b0.json
python tools/export_apci.py --all \
    --out extracted/apci/perinfo --manifest manifests/batches/apci-perinfo.json
```

## 5. 待办

1. ~~解释 item blob 的语义~~：已闭合（逐方向×帧单元：1B 帧索引 / 12B 锚点组，见第 3 节）；
2. ~~确定 `entry.a`、`mid.header` 低 16 位与 `item.x/y` 各字段含义~~：已闭合（见第 3 节）；
3. 将 APCI 记录与 `player*/head`、`perinfo.dat` 及 APG 动作建立映射
   （消费引擎 `FUN_008283d0` 已按旧 SHA 定位；当前 SHA 的地址与调用链待复核，
   字段级对照可作为后续入口）；
4. blob 12B 锚点组 6×u16 的逐槽位语义（需在 `FUN_008283d0`/`FUN_008280a0`
   内追读点）；
5. `entry.a` 的区间判定：41.9% entry 不在 `FUN_007ed6d0` 的 4 个区间内，
   需找到该函数的实际调用者，确认哪些 record/entry 使用动作 ID、哪些使用
   其它 ID 空间；
6. 若后续发现其他 `APCI` 容器，可直接复用 `parse_apci` / `iter_items`。
