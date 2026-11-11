# SCP UI 脚本

- 日期：2026-10-23
- 目标：`D:\software\dhsh\shdata\Game.exe`
  SHA-256 **`7a5e2c085ac79557ca104a2917e06f1a7e5f2827e4b68753bbaf1c754a4e78dc`**
  （2026-10-21 更新；旧基线 `f6bf66d2…`）
- 当前实现：`tools/dhsh/formats/scp.py`、`tools/scan_scp.py`、`tools/export_scp_json.py`
- 状态：**1,940/1,940 个 SCP 解析并导出 JSON（0 解析错误）**；窗口树、
  属性、include、脚本块、预处理指令均已保留。2026-10-23 已按更新后的
  `config.pak`/`800config.pak` 重导（31,652 块、519 include、0 错误）。

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

### BGPIC 内联配置

`BGPIC` 的值为花括号块，已解析为结构化字段（`bgpic`），同时保留原始
`props` 行：

```text
BGPIC = { "BGTYPE" = "STATIC"; "SECTION" = "STATICHL"; TEXT = "存款"; FONTTYPE = 6 }
```

- 键统一大写、去掉引号；`;` 只在引号外分割；
- 已观察 7,435 个活动 BGPIC（另有 156 个被 `//` 注释），全部单行；
- 键分布：`BGTYPE` 7,435、`SECTION` 7,431、`TEXT` 1,428、
  `FONTTYPE` 640、`DRAWPARENT` 233、`FORCEWIDTH` 132、`XOFFSET` 128、
  `YOFFSET` 125、`HOFFSET` 118、`FORCEHEIGHT` 118、
  `TITLE1`–`TITLE9` 205、`TEXTCOLOR` 28、`YSPACE` 2；
- `BGTYPE` 常见值：`STATIC`、`BUTTON`、`EDIT`、`IMAGE`、`IMGLIST`、
  `STBTN`、`TEXTLIST`、`RICHTEXT`、`TITLEIMGCOPY`、`IMAGECOPY`。

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

## 3. include 图

`tools/scp_include_graph.py` 解析全部 1,940 个 SCP 的 `#include`，按
include 路径（相对 `config` 根）在三个语料中解析：

| 项 | 数值 |
|---|---:|
| include 边 | **519** |
| 同语料解析 | 183 |
| 跨语料解析 | 336 |
| 缺失 | **0** |
| 重复 include | 6 |
| 目标类型 | `.scp` 464、`.lua` 55 |
| 最大传递闭包 | 3 |
| 环 | 0 |

- 语料根：`easy=extracted/easy`、`0a=extracted/decoded`、
  `0b=extracted/decoded_0b`；include 写法为 `config\UI\...`（大小写、反斜杠不统一）；
- 解析顺序：先按原路径在**本语料**查找，再依次跨语料（0a → 0b → easy）；
  `config/UI` → `800config/ui` 的别名仅作最后回退（这 519 条边未用到）；
- 336 条跨语料边主要来自 0b/easy 引用 base 树中的 `config/UI/...`；
- 6 条重复 include（如 `task_main.scp` 重复 7 次同一文件）；
- 输出 `manifests/batches/scp-include-graph.json`（0.54 MiB）：每个节点含
  `includes`（目标/状态/命中副本）、`included_by` 反向边和 `closure`。

## 4. 已知告警（9 个）

均为条件编译/END 配对的边缘情况，不影响导出：

- `user_main.scp`（easy 的两个副本）未闭合 5 个块；
- `spirit_map.scp`、`assetlist.scp`（0x0A/0x0B）各未闭合 1 个块；
- `shiguangxuzhang.scp`、`farm_buymain.scp`、`farm_tree_state.scp`、
  `Ranking_list.scp` 各 1 个多余 `END`。

`#ifdef` 只解析 `WINWIDTH800` 分支（客户端使用 800×600 UI）；
另一分支的块可能与主分支不对称，产生上述告警。

## 5. 导出

`tools/export_scp_json.py`：

| 批次 | 文件 | 块 | 输出 | 耗时 |
|---|---:|---:|---:|---:|
| B0 | 10 | 95 | 0.04 MiB | 1.1 s |
| B1 | 100 | 1,512 | 0.68 MiB | 1.6 s |
| 全量 | **1,940** | **31,652** | **16.10 MiB** | **7.8 s** |

- 全量 0 错误；1,940 个 JSON 全部落盘；
- 每个窗口节点新增 `bgpic` 结构化字段（原始 `props` 仍保留）；
- 抽查 50 个 JSON，窗口计数与 manifest 全部一致；
- 输出 `extracted/scp_json/<corpus>/<logical path>.json`；
- 清单 `manifests/batches/scp-json-b0.csv`、`scp-json-b1.csv`、
  `scp-json-full.csv`。

## 6. 待办

1. ~~解析 `#include` 图并展开/去重~~（已完成，见第 3 节）；
2. ~~解析 `BGPIC = { ... }` 内联配置~~（已完成，见第 1 节）；
3. UI 树 → 可视化布局（可作为查看器的一部分，滞后）；
4. ~~脚本块（Lua 风格）语义与消息绑定~~（已闭合，见第 7 节）。

## 7. 消息绑定与 Lua 桥（2026-10-21，Ghidra 批次 51-57）

**`#script` 块就是标准 Lua**（引擎内嵌原版 Lua VM：二进制含
"loop in gettable"/"table index is NaN"/`__concat` 等 Lua 5.x 内部字符串，
及 "FSLuaSimple : create luavm fail"），并有 `luac`/`.lua`/`.out`
字符串表明支持预编译字节码。

**消息声明**：窗口定义中 `MESSAGE <消息名> <n> CODE "<Lua函数>"`：
- 消息名表内嵌于 Game.exe（旧基线 VA `0xa4c9ac` 附近；当前 SHA 含 51 个
  去重 `FSM_*` 字符串），包括：
  鼠标类 `FSM_MOUSE_DOWN/UP/MOVE/DBCLK/HOVER/LEAVE(/HOVEREX/LEAVEEX)`、
  键盘 `FSM_KEY_DOWN/UP`、`FSM_CHAR`、控件 `FSM_BTN_MSG`、`FSM_EDIT_MSG`、
  `FSM_LIST_SELECTED`、`FSM_COMBOBOX_SELECTED/OPEN_START/OPEN_END`、
  `FSM_SCROLL`、`FSM_DRAG`、`FSM_MOVE`、`FSM_SHOW`、`FSM_ENABLE`、
  `FSM_CHECKBOX(/_CHECK)`、`FSM_RADIOGROUP(/RADIOCANCHANGE)`、
  `FSM_DIALOG`、`FSM_MODALDIALOG`、`FSM_MENU`、`FSM_ANIMATE`、
  `FSM_TREEVIEW`、`FSM_IMAGELIST`、`FSM_LISTVIEWITEMTIP`、
  `FSM_IMGLISTITEMTIP`、`FSM_MOUSEWHEEL`、`FSM_SPIN_CHANGE`、
  `FSM_SETFOCUS`、`FSM_FOCUSCHANGED`、`FSM_WINCLICK`、`FSM_WINHELP`、
  `FSM_WINFLASHMSG`、`FSM_SWITCHSHOW`、`FSM_TIPWINMSG`、`FSM_LISTSCROLL`、
  `FSM_BRINGTOTOP`、`FSM_SENDTOBACK`，以及物品拖放
  `FSM_ITEMDOCK`、`FSM_ISITEMCANDRAG/CANDOCK/CANCHUCK`；
- SCP 解析器族 `FUN_0090c040`（旧基线 `0x90bf00`，117 KB）按名称表把消息名
  映射为内部 ID 挂到窗口对象；`CODE` 为 Lua 回调名；窗口类型名经
  `FSWndTypeNameMap`（旧基线 `FUN_009133f0`）解析。
  > **2026-11-10 修正**：真实 `bulletin2.scp` 中
  > `FSLuaWndScript.RegMsgCall( "OnShowPolicy" )` 是**单参数回调名**；
  > 引擎按该名字取 Lua 全局函数引用，`MESSAGE ... CODE "OnShowPolicy"`
  > 是触发键。旧版 `lua-runtime.md` 的“消息名 + 函数”双参数推断已作废，
  > 详见 `docs/formats/lua-runtime.md` §3.2。
- **当前 SHA 复核**：`FUN_008fc970` 反编译可见
  `FUN_004029f0("RegMsgCall",10)` 与回调表 `LAB_008fc660`；
  `FUN_008c5720` 为 FSLua 脚本对象构造（vtable `PTR_FUN_00a477bc`）；
  `FUN_0090c040` 为 117 KB 的 SCP 解析器族。其余旧地址按本模块的
  +0x140 偏移迁移，未逐项复核。

**Lua C 桥**（注册表在 VA `0xa47108` 一带）：
- `FSLuaWndScript`：窗口脚本 API，注册器 `FUN_008fc830` 登记
  `RegMsgCall`（C 实现 `0x8fc520`：解析 Lua 函数引用 `FUN_008c4140`，
  挂入窗口脚本对象的回调链表 `obj+0x13c`）、`RegAfterCreate`、
  `RegBeforeRelease`、`RegWinRun` 等生命周期钩子；
- `FSLuaSystem`：系统 API（`GetRootWindow` 等），注册器
  `FUN_008bb410`/`0x8bb230`/`0x8bb260`/`0x8bb290`/`0x8bb2c0`；
- `FSLuaInstall("模块")`（C 实现 `FUN_008c55e0`）：加载
  `config/lua/*` 模块（如 `FSGameExchange`、`FSLuaBaseEx`、`scdefine`）。

**CSV 配置**（457 个，Skill.csv/magicFormulas.csv 等）：Game.exe 内
**无任何硬编码引用**（"Skill.csv"、".csv" 后缀均 0 xref）——
CSV 完全数据驱动加载（pak 枚举 + Lua/配置层消费），语义属于游戏数据层，
不构成格式逆向问题。

## 8. 消息名称 → ID 全表（当前 SHA 复核）

> §7 中的 `0xa4c9ac`、`0x8fc520`、`0x8c55e0` 等属于旧 SHA，仅作历史记录；
> 当前 `7a5e…` 以本节和 `lua-runtime.md` 为准。

- 基线 SHA-256：`7a5e2c085ac79557ca104a2917e06f1a7e5f2827e4b68753bbaf1c754a4e78dc`
- 构造函数：`FUN_0090c040`（Observed，117 KB，SCP 解析器族）
- 证据：`extracted/_batches/lua-task16/evidence7.json`

### 8.1 表项布局

每个名称对应一个 0x44 字节对象：

```c
struct MessageNameEntry {   // 0x44 bytes
    uint32_t id;            // +0x00
    char     name[0x40];    // +0x04，strncpy(..., 0x3f)
};
```

构造伪代码（Observed，逐项展开）：

```c
puVar3 = operator_new(0x44);
zero(puVar3, 0x44);
*puVar3 = <ID>;
_strncpy((char *)(puVar3 + 1), "<NAME>", 0x3f);
FUN_00402b40(puVar3 + 1);       // 构造 std::string 键
FUN_004206a0(...);              // 插入名称→对象映射
```

### 8.2 `FSM_*` 完整映射

以下 51 项在初始化代码中连续赋值，ID 为 `0x00–0x32`：

| ID | 名称 | ID | 名称 |
|---:|---|---:|---|
| `0x00` | `FSM_SHOW` | `0x1a` | `FSM_CHECKBOX` |
| `0x01` | `FSM_ENABLE` | `0x1b` | `FSM_RADIOGROUP` |
| `0x02` | `FSM_KEY_DOWN` | `0x1c` | `FSM_MODALDIALOG` |
| `0x03` | `FSM_KEY_UP` | `0x1d` | `FSM_MENU` |
| `0x04` | `FSM_MOUSE_DOWN` | `0x1e` | `FSM_ANIMATE` |
| `0x05` | `FSM_MOUSE_UP` | `0x1f` | `FSM_TOOLSBAR` |
| `0x06` | `FSM_MOUSE_MOVE` | `0x20` | `FSM_CHECKBOX_CHECK` |
| `0x07` | `FSM_MOUSE_DBCLK` | `0x21` | `FSM_IMAGELIST` |
| `0x08` | `FSM_MOUSE_HOVER` | `0x22` | `FSM_TREEVIEW` |
| `0x09` | `FSM_MOUSE_LEAVE` | `0x23` | `FSM_IMGLISTITEMTIP` |
| `0x0a` | `FSM_CHAR` | `0x24` | `FSM_LISTVIEWITEMTIP` |
| `0x0b` | `FSM_MOVE` | `0x25` | `FSM_MOUSEWHEEL` |
| `0x0c` | `FSM_SCROLL` | `0x26` | `FSM_RADIOCANCHANGE` |
| `0x0d` | `FSM_DRAG` | `0x27` | `FSM_SETFOCUS` |
| `0x0e` | `FSM_BTN_MSG` | `0x28` | `FSM_WINFLASHMSG` |
| `0x0f` | `FSM_EDIT_MSG` | `0x29` | `FSM_SWITCHSHOW` |
| `0x10` | `FSM_LIST_SELECTED` | `0x2a` | `FSM_MOUSE_HOVEREX` |
| `0x11` | `FSM_COMBOBOX_OPEN_START` | `0x2b` | `FSM_MOUSE_LEAVEEX` |
| `0x12` | `FSM_COMBOBOX_OPEN_END` | `0x2c` | `FSM_TIPWINMSG` |
| `0x13` | `FSM_COMBOBOX_SELECTED` | `0x2d` | `FSM_LISTSCROLL` |
| `0x14` | `FSM_SPIN_CHANGE` | `0x2e` | `FSM_WINCLICK` |
| `0x15` | `FSM_ITEMDOCK` | `0x2f` | `FSM_WINHELP` |
| `0x16` | `FSM_ISITEMCANDRAG` | `0x30` | `FSM_BRINGTOTOP` |
| `0x17` | `FSM_ISITEMCANDOCK` | `0x31` | `FSM_SENDTOBACK` |
| `0x18` | `FSM_ISITEMCANCHUCK` | `0x32` | `FSM_FOCUSCHANGED` |
| `0x19` | `FSM_DIALOG` | | |

校验示例：`MESSAGE FSM_BTN_MSG 1 CODE "OnChatClick"` 中
`FSM_BTN_MSG` → ID `0x0e`。

### 8.3 同函数内的其他名称表

`FUN_0090c040` 还构造了两组名称表，均使用相同的 0x44 字节表项：

| 组 | 数量 | ID 范围 | 示例 |
|---|---:|---|---|
| SCP 属性关键字 | 144 | `0x3d–0xcd`（有空洞） | `SPIN` `0x3d`、`RECT` `0x4a`、`MESSAGE` `0x4b`、`BGPIC` `0x7b`、`FIXEDPART` `0xad`、`NEEDONE` `0xcd` |
| 窗口命令/消息名 | 25 | `0x00–0x18` | `OPEN` `0x00`、`CLOSE` `0x01`、`MESSAGE` `0x02`、`SCRIPT` `0x03`、`CODE` `0x04`、`CREATEWINDOW` `0x05`、`RFINDOPEN` `0x18` |

完整 220 条名称→ID 清单由 evidence7 的
`*puVar3 = <ID>; _strncpy(..., "<NAME>", 0x3f)` 模式直接提取；上表只列
Task 15 必需的 `FSM_*` 全量。

**未知**：名称→ID 映射在解析时的具体查找调用点（map::find 包装函数）尚未
单独定位；表构造和表项布局已确认。窗口类型名 `FSWndTypeNameMap`
（`0x9133xx` 邻域）是另一张表，未在本次展开。
