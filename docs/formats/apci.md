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
| `entry.a` | **模型 ID**（资源目录第一段，2026-10-26 全量验证：22,257/22,341 = **99.62%** 命中 PAK 资源树） | 常见 200/250/201/251、101500+；按 (name,entry) 去重 22,305 个 entry 中 **9,355（41.94%）**不在 `FUN_007ed6d0` 的 4 个区间——那是模型 ID 空间差异，不是校验范围；见下方家族分类与 lookup 表 |
| `entry.b` 高 16 位 | 保留（全 0） | 解析器只取低 16 位 |
| `entry.b` 低 16 位 | mid 数（= 动作数） | 解析器 `0x82a460` |
| `mid.header` 高 16 位 | **方向数** | 按 item 加权 **683,352/687,229 = 99.44%** 为 8；按 mid 计数 85,419/87,676 = 97.43%；按 entry（首个 mid）91.56%。旧文档 99.4% 是 item 加权口径 |
| `mid.header` 低 16 位 | **动作 ID**（= 资源目录 `<action>/`，2026-10-26 修正：旧文档误写"每方向帧数"，当时看到的 10/30/90/100/101 频次其实是动作 ID 分布） | 同一 entry 的 mid 低 16 位集合 == 该模型动作目录集合，player/npc 家族 20,487/20,570 = **99.60%** 子集成立；mid 按动作 ID 字典序排列 |
| `item.x` 低 16 位 | 矩阵布局时的行数（= 方向数） | 8 最多 |
| `item.x` 高字节 | 每单元字节数 | 1 = 帧索引字节（579,741 item）；12 = 6×u16 参数记录（107,407）；另有 81 个 item 为 0（空/边界记录） |
| `item.y` 字节 2 | 布局标志：1 = 一维 blob；0 = 矩阵 blob | 全部 item：1=424,876、0=262,353；有 blob 的 432,838 item：1=424,876、0=7,962 |
| blob | 逐（方向×帧）单元数据 | 大小公式 100% 验证（687,229/687,229）：`y_hi8==1 → x_hi8`；否则 `x_hi8 × x_lo16` |

**blob 内容**：1 字节单元 = 帧索引（如 `0b`）；12 字节单元 =
**6×u16 参数记录**。数据统计（4,156 个 item、11,834 个单元）表明它是
PIF 行字段的**重排**，不是三个任意坐标点：

```
PIF 行:      f0, f1, f2, f3, f4, f5
APCI 12B:    f0, f1, f4, f5, f2, f3
```

- 最常见单元 `(160,160,5,80,160,210)` = PIF 行
  `(160,160,160,210,5,80)` 的重排；
- 槽位 4/5 = PIF `f2/f3`（x/y 偏移，常见 160/210）；
- 槽位 3 = PIF `f5`（常见 80/100/120/140，观测 20–300）；
- 槽位 2 = PIF `f4`（常见 5/6/0），槽位 0/1 = PIF `f0/f1`（常见 160/160）；
- 4,156 个 item 中 3,458 个所有单元相同，698 个仅该槽变化。

**消费端读到（2026-10-25；2026-10-26 审计修正）**：`FUN_00827be0` 把
APCI 记录**逐字**复制成 12 字节数组（3 dwords）；角色/装备动画加载器
`FUN_0043f380`：

- `+0x08` → 加到每帧位图的 x（`0x43f882`/`0x43f929`）；
- `+0x0a` → 加到每帧位图的 y（`0x43f87d`/`0x43f920`）；
- `+0x06` → **被读取**：`*(u16*)(unit + i*0xc + 6) * 10` 逐帧写入
  时序数组 `param_1[0x45]`。

2026-10-26 审计修正：此前写的“`+0x06` 在该路径未被读取、时序值来自
`+0x0a`”**不成立**——那是把 fallback 重建分支（缺单元时
`dst+6 = src+10`）误当成了主路径。主路径单元由 `FUN_00827be0` 逐字
拷贝，时序读的就是单元自身的 `+6`。按布局 `[f0,f1,f4,f5,f2,f3]`，
`+6` = 槽位 3 = **PIF `f5` 帧延时**（top 值 80/100/120 与 PIF 延时
分布一致），时序换算为 `×10`。至此 `+6` 的待定项闭合。
“12B 锚点组”更准确的叫法是**逐帧参数记录**。统计证据：
`manifests/batches/apci-12b-units.json`（其中 field_use 的 `+0x06`
条目已按本结论修正）。

**12B 单元的全部消费点（2026-10-26 枚举闭合）**：`FUN_00827be0`
（构建 12B 数组）共 8 个调用点：

| 调用点 | 所在函数 | 用途 |
|---|---|---|
| `0x43f50c` | `FUN_0043f380` | 角色/装备动画加载（主路径，+8/+a=x/y、+6=延时×10） |
| `0x81a9cd` | `FUN_0081a950` | 场景/UI 对象动画（参数取 `obj+0xe` 的 u16） |
| `0x8c21fd` | `FUN_008c21b0` | 通用 APG 装载（带 `%s.apg` 拼路径） |
| `0x52e619` | `FUN_0052e3b0` | 苹果机/抽奖机类 UI（`config/ui/increment/applemach.csv`） |
| `0x52f779` | `FUN_0052f510` | 同上（同族 5 份副本） |
| `0x530629` | `FUN_005303c0` | 同上 |
| `0x5312a9` | `FUN_00531040` | 同上 |
| `0x53240a` | `FUN_00532280` | 同上（同族变体，5,646 字符） |

即 12B 逐帧参数记录服务于：角色动画 + 通用 APG 装载 + 抽奖机族 UI
动画，字段用法一致（x/y 偏移 + 帧延时）。各 record 家族的 lookup 表
仍开放（`entry.a` 家族分类见下，消费引擎统一入口 `FUN_00828510`）。

**资源 lookup 表（2026-10-26 闭合）**：`tools/kimi_apci_lookup.py`
生成并全量验证（`manifests/batches/apci-lookup.csv` / `.json`）：

```
group = 部位类别: 0=head 1=body/npc 2=weapon 3=pet 4=magic 7=cloth
        8=effect 9=mask 10=Lcuff 11=shoe 12=Rcuff 13=horse
        14=harmorbody 15=harmorhead（5/6/16-19 空）
record 名 = <pak 家族>\<部位>（player\head → player.pak:head/；
        npc3 → npc3.pak；magic2 → magic2.pak）
entry.a = 模型 ID → <部位>/<模型>/
mid     = 一个动作：header 高 16 位 = 方向数（8），低 16 位 = 动作 ID
          → <动作>/；mid 按动作 ID 字典序排列
item    = 一个方向 → <部位>/<模型>/<动作>/<方向>.apg
blob    = 该方向逐帧数据（12B 逐帧参数 / 1B 字节，语义见上）
```

验证率：player/npc 家族 20,570 entry 中模型命中 99.64%、动作子集
99.60%；magic 家族 1,766 entry 中模型命中 99.8%（`1344/0/front.apg` 等
`<模型>/<变体>/{front,back}.apg` 结构，mid 低 16 位对 87% 等于变体号，
其余为 0）。残余 miss 为无美术资源的条目（下架/纯参数模型）。
magic 的 item 记录 byte4/byte5 = back/front 分层标志（`FUN_00827be0`
据此拼 `%s/%d/%d/back|front/` 路径），需要居中的模型
（`FUN_0082c550` 门控）会把 12B 单元 +4/+6 按分辨率差值平移
（`(W-640)/2`、`(H-480)/2`）。

每模型每动作还有独立的 `<部位>/<模型>/<动作>/perinfo.dat`（**PIF 格式**，
非 APCI；8 行=8 方向），与 APCI 12B 单元互为同一参数的两种存储。

**entry.a 家族分类（2026-10-23）**：`tools/classify_apci_entry_a.py`
按 record 名统计（`manifests/batches/apci-entry-a-families.csv`）：

- `npc`/`npc1-9`：**100% 越界**，主流带宽 10,000–19,999 /
  20,000–29,999 / 30,000–39,999 —— NPC 使用自己的动作/资源 ID 段，
  不应拿玩家 `FUN_007ed6d0` 的 4 个区间校验。
- 基础 `player\body/cloth/weapon/Lcuff/Rcuff/shoe/mask`：约 99% 落在
  100–9,999（少量 <100 的边界值）。
- 编号 `playerN\head/mask/body/...`：混用 20k–29k、50k–59k、
  60k–64k、101k 等段；越界比例 0–90% 不等，取决于套装。
- 因此“41.9% 越界”不是解析错位，而是**不同 record 家族使用不同 ID
  空间**；需要按家族/调用者选择对应的动作表，而不是一个全局白名单。

**解析器→内存布局**（当前 SHA：解析器 `0x82a5a0`、加载器 `0x82a810`；
旧基线 `0x82a460`/`0x82a6d0`，本模块 +0x140）：
record 0x2c 内存步长（36B 文件头 + 子表指针）；entry 16B（a, b, mid 表指针）；
mid 12B（header, item 表指针）；item 16B（x, y, blob 指针，blob 就地截断 & 0xffff）。
当前解析器反编译仍以 `cmp [data],0x49435041` + version `0x101` 开头。

**消费链**：`FUN_007ed8a0` 启动加载 "perinfo.pak" → 全局 `DAT_00ae4b08`
（0xac 字节对象）→ 21 个消费者经 `FUN_007ec2e0()` 取用；核心查询引擎
`FUN_00828510`（旧基线 `0x8283d0`；当前 SHA 已复核该函数及 6 个调用点，
输出部位帧表；`FUN_00805b10` 场景渲染族驱动）。

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
3. ~~将 APCI 记录与 `player*/head`、`perinfo.dat` 及 APG 动作建立映射~~
   **已闭合（2026-10-26）**：lookup 链 = record 名→(pak 家族, 部位)、
   `entry.a`→模型 ID、`mid.header` 低 16 位→动作 ID、item→方向、
   blob→逐帧参数；模型命中 99.62%、动作子集 99.60%
   （`tools/kimi_apci_lookup.py` → `manifests/batches/apci-lookup.csv`）。
   每模型动作的 `perinfo.dat` 确认为 **PIF 格式**（8 行=8 方向）。
   剩余：1B blob 单元的精确语义（既非帧数也非帧索引，疑为播放节拍数）；
4. ~~blob 12B 单元结构~~ **布局与主消费链已闭合（2026-10-25；
   +6 于 2026-10-26 审计闭合）**：6×u16 = PIF 行字段重排
   `[f0,f1,f4,f5,f2,f3]`；`FUN_00827be0` → `FUN_0043f380` 确认
   `+8/+a` = x/y 偏移，`+6` = PIF `f5` 帧延时（×10 写入
   `param_1[0x45]` 时序数组）。
5. ~~`entry.a` 的区间判定~~ **已解释（家族分类，2026-10-23）**：
   越界集中在 `npc*`（100%）与编号 player 套装；基础 player 约 99%
   在 100–9,999。~~仍需为每个家族定位具体 lookup 表/调用者~~
   **lookup 表 2026-10-26 已闭合（见第 3 节）**；`FUN_007ed6d0` 实为
   clamp 辅助函数（范围来自 `FUN_007ed320`），并非 entry.a 校验器。
6. 若后续发现其他 `APCI` 容器，可直接复用 `parse_apci` / `iter_items`。
