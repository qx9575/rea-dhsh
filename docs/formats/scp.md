# SCP UI 脚本

- 日期：2026-10-20
- 目标：`D:\software\dhsh\shdata\Game.exe`
  SHA-256 `f6bf66d24a3523bb6a30541589fabea36bbceec84355463d8e2c99e3b7f16be9`
- 当前实现：`tools/dhsh/formats/scp.py`、`tools/scan_scp.py`、`tools/export_scp_json.py`
- 状态：**1,940/1,940 个 SCP 解析并导出 JSON（0 解析错误）**；窗口树、
  属性、include、脚本块、预处理指令均已保留。

## 1. 编码与语法

- 编码：GB18030 **1,720** 个，UTF-8（带/不带 BOM）**220** 个；
  未发现解码失败。
- 行注释：`//`（引号内的 `//` 不截断）；块注释 `/* ... */`。
- 预处理：`#ifdef WINWIDTH800` / `#else` / `#endif`、`#define`、
  `#include "..."`、`#script ... #endscript`。
- 块：

```text
WINDOW TYPE = WIN
    NAME = "barwnd"
    RECT = 0,582,800,600
    BMP  = "ui/bar/inputbg_800.a16"
    WINDOW TYPE = BUTTON
        NAME = "bar_close"
        MESSAGE FSM_BTN_MSG 1 CODE "OnChatClick"
    END
    FIXEDPART TYPE = EDIT
        ...
    END
END
```

- `WINDOW` 与 `FIXEDPART` 都可嵌套，`END` 关闭最近块；
- 其它行为 `KEY = value` 或 `KEY token ...`（如 `MESSAGE`）；
- 属性值可以是单行、带引号、跨行拼接的字符串（连续引号行会追加到前一属性）；
- `#script` 块原样保存，不按 UI 语法解析（内部是 Lua 风格脚本）；
- 纯文本行保存在 `free_text`，不算解析错误。

解析结果结构：

```json
{
  "encoding": "gb18030",
  "includes": ["config/ui/chat/messagefind.scp"],
  "preproc": [["#ifdef", "WINWIDTH800"], ["#else", ""], ["#endif", ""]],
  "scripts": ["..."],
  "free_text": [],
  "props": [],
  "windows": [
    {"kind": "WINDOW", "type": "WIN", "line": 3,
     "props": [["NAME", "\"barwnd\""], ["RECT", "0,582,800,600"]],
     "children": [ ... ]}
  ],
  "warnings": []
}
```

## 2. 语料统计

`tools/scan_scp.py`：

| 项 | 数值 |
|---|---:|
| SCP 文件 | **1,940**（easy 732、0x0A 625、0x0B 583） |
| 行数 | 268,195 |
| `WINDOW` 块 | 27,698 |
| `FIXEDPART` 块 | 3,954 |
| `#include` | 519 |
| 编码 | GB18030 1,720 / UTF-8 220 |

控件类型（Top 10）：

`BUTTON` 10,035；`STATIC` 8,116；`WIN` 4,962；`IMGLIST` 2,330；
`EDIT` 2,124；`SCROLL` 873；`TWOSTBTN` 873；`CHECKBOX` 832；
`TXTLIST` 309；`LISTVIEW` 305；`RATEBAR` 249；另有 `RICHTEXT`、
`RADIOGROUP`、`COMBOBOX`、`SPIN`、`LISTVIEWEX`、`TREEVIEW`、`RICHEDIT`。

高频属性：`RECT` 31,187、`NAME` 27,940、`WINDOW` 27,845、`MESSAGE` 13,666、
`BGPIC` 7,435、`TEXTCOLOR` 5,871、`BMPPATH` 4,385、`CLIENTRECT` 3,993、
`FIXEDPART` 3,982、`TIPSDELAY` 3,860、`BMP` 3,343、`TEXT` 3,165。

报告：`manifests/batches/scp-scan.json`。

## 3. 已知告警（9 个）

均为条件编译/END 配对的边缘情况，不影响导出：

- `user_main.scp`（easy 的两个副本）未闭合 5 个块；
- `spirit_map.scp`、`assetlist.scp`（0x0A/0x0B）各未闭合 1 个块；
- `shiguangxuzhang.scp`、`farm_buymain.scp`、`farm_tree_state.scp`、
  `Ranking_list.scp` 各 1 个多余 `END`。

`#ifdef` 只解析 `WINWIDTH800` 分支（客户端使用 800×600 UI）；
另一分支的块可能与主分支不对称，产生上述告警。

## 4. 导出

`tools/export_scp_json.py`：

| 批次 | 文件 | 块 | 输出 | 耗时 |
|---|---:|---:|---:|---:|
| B0 | 10 | 95 | 0.04 MiB | 1.1 s |
| B1 | 100 | 1,512 | 0.68 MiB | 1.6 s |
| 全量 | **1,940** | **31,652** | **14.90 MiB** | **6.4 s** |

- 全量 0 错误；1,940 个 JSON 全部落盘；
- 抽查 50 个 JSON，窗口计数与 manifest 全部一致；
- 输出 `extracted/scp_json/<corpus>/<logical path>.json`；
- 清单 `manifests/batches/scp-json-b0.csv`、`scp-json-b1.csv`、
  `scp-json-full.csv`。

## 5. 待办

1. 解析 `#include` 图并展开/去重（519 条边，多为 `800config` 转发）；
2. 解析 `BGPIC = { ... }` 内联配置（BGTYPE/SECTION/TEXT/FONTTYPE）；
3. UI 树 → 可视化布局（可作为查看器的一部分，滞后）；
4. 脚本块（Lua 风格）语义与消息绑定。
