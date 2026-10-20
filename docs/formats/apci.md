# perinfo.pak / APCI 容器（进行中）

- 日期：2026-10-20
- 目标：`D:\software\dhsh\shdata\Game.exe`
  SHA-256 `f6bf66d24a3523bb6a30541589fabea36bbceec84355463d8e2c99e3b7f16be9`
- 状态：**格式定位与解析器层级已确认；完整遍历仍差最后一段对齐，暂缓。**

## 1. 文件头

`shdata/perinfo.pak`（7,808,665 字节）以 `APCI` 开头：

```
0x00  char[4]  "APCI"
0x04  uint32   version = 0x101
0x08  ...      数据（group 序列）
```

`Game.exe` 中：

- 文件加载器 VA `0x82a6d0`：以路径 `perinfo.pak`、模式 `0` 调用解析器，
  失败可重试 4 次；
- 解析器 VA `0x82a460`：校验 `APCI` 与 version 0x101，然后遍历 group。

## 2. 解析器层级（VA 0x82a460）

`mode=0`（游戏实际调用值）下的读取顺序：

```
group:
    u16 group_id
    u16 record_count
    record × record_count:
        char[36] struct        // +0 u16 id, +2 u16 sub_count, +4 char[32] name
        subentry × sub_count:  // mode 0：u16 a, u16 b
            mid × b:           // mid header: u32, count = high16
                item × count:
                    u32 x, u32 y
                    if (x & 0xffff) != 0:
                        size = ...
                        blob[size]
```

- 组数上限 20；`record` 在内存中占 44 字节（36 + 指针 + 对齐）；
- 分配器为 VA `0x826e70`；
- `subentry` 的 `b` = mid 个数；`mid` 头低 16 位用途待定，
  高 16 位为 item 个数；
- item 的 blob 大小分支（VA `0x82a601` / `0x82a60c`）：
  - `byte[y+2] == 1` 时取 `byte[x+3]`；
  - 否则为 `byte[x+3] * ecx`（ecx 由 `x` 低 16 位与 `y` 高 16 位组合）。

首个组：group_id=0、record_count=17，第一条记录名为 `player\head`，
sub_count=304——与“头像/头部信息”用途一致。

## 3. 当前阻塞

按上述层级直接移植后：

- 能解析到约 **7.25 MB / 7.81 MB**，已读出 7 个 group、2 万+ item；
- 在 group 7 的一段数据中落入字符串 `player1\Rcuff` / `player1\effect`，
  说明某处 `blob` 大小或 mid 层计数仍有偏差；
- 字符串（如 `player1\Rcuff`、`player1\effect`）看起来是内联名称/表数据，
  但现有解析器代码路径中没有显式名称读取，说明还存在未识别的层级
  或字段组合。

## 4. 后续方案

1. **Unicorn 局部仿真**（首选）：加载 `0x82a460`、桩掉 `0x826e70`
   （返回 bump 堆指针），执行后直接遍历内存中的组/记录/子项/blob，
   反推准确层级与字段语义；仅做静态代码仿真，不运行游戏本体。
2. 继续静态反汇编，重点核对 `0x82a601`–`0x82a64a` 的 blob 大小与
   `0x82a5b5` 的 mid 计数/高 16 位语义。
3. 完整解析后导出 `perinfo.pak` 的名称树与 blob（头像/头部信息），
   并与 `player*/head`、`perinfo.dat` 关联。

在此之前，`perinfo.pak` 保持原始导出，不参与批量转换。
