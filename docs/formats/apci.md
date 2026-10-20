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

字段观察（尚未赋予确定语义）：

| 字段 | 观察 |
|---|---|
| `entry.a` | 100/200/250/350…，多为 50 的倍数 |
| `entry.b` 高 16 位 | 全部为 0 |
| `mid.header` 低 16 位 | 10、30、20、60、80、90… |
| `item.x` 低 16 位 | 8（278,643）、0（254,391）、12、10、6… |
| `item.x` 高字节 | 1（579,741）、12（107,407）、0（81） |
| `item.y` | `0x10000`（419,041）、0（259,483）、`0x10100`… |
| blob 大小 | 1 B（365,561）、12 B（59,789）、96 B、144 B、24 B… 共 54 种 |

推断（非定论）：`x` 低 16 位像“行宽/列数”，高字节像“每像素字节数”，
`y` 高字节像标志位；blob 可能是逐帧的时序/索引数据。group 8
（`player\effect`）与 group 12（`player\Rcuff`）的 item 全部无 blob。

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

1. 解释 item blob 的语义（时序？索引？与 APG 动画帧的关联）；
2. 确定 `entry.a`、`mid.header` 低 16 位与 `item.x/y` 各字段含义；
3. 将 APCI 记录与 `player*/head`、`perinfo.dat` 及 APG 动作建立映射；
4. 若后续发现其他 `APCI` 容器，可直接复用 `parse_apci` / `iter_items`。
