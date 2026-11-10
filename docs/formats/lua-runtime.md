# Lua 运行时与宿主 ABI（静态证据）

- 分析对象：`D:\software\dhsh\shdata\Game.exe`
- 基线 SHA-256：`7a5e2c085ac79557ca104a2917e06f1a7e5f2827e4b68753bbaf1c754a4e78dc`
- 分析副本：`extracted/gameexe-snapshots/Game-7a5e2c08…exe`
- Ghidra 项目：`ghidra-proj/dhsh-7a5e`（程序 `Game-7a5e…exe`）
- 地址口径：镜像基址 `0x400000`，文中均为 VA；`.text` 文件偏移 = VA − `0x400000`
- 状态：Task 16 证据 + 最小宿主实现文档；只做只读静态分析，不运行游戏、不引用保护组件
- 修订：2026-11-10 按真实 `bulletin2.scp` 调用点修正 `RegMsgCall` 为单参数回调名（见 §3.2、§7）

本文区分三类结论：

- **Observed**：字节、反汇编或伪代码直接可见；
- **Inferred**：由调用形态或 Lua 5.1 源码模式推断，需后续验证；
- **Unknown**：当前证据不足。

---

## 1. 版本结论：Lua 5.1.4

**Observed。** `.rdata` 中存在完整的 Lua 标识串，位于 VA `0xa4751e`：

```text
00a47510  ... ... ... ... ... ... e0 bf 24 4c 75 61 3a 20 4c 75
00a47520  61 20 35 2e 31 2e 34 20 43 6f 70 79 72 69 67 68
00a47530  74 20 28 43 29 20 31 39 39 34 2d 32 30 30 38 20
00a47540  4c 75 61 2e 6f 72 67 2c 20 50 55 43 2d 52 69 6f
00a47550  20 24 0a 24 41 75 74 68 6f 72 73 3a 20 52 2e 20
```

对应 Lua 5.1 `lua.h` 的 `LUA_IDENT`：

```text
$Lua: Lua 5.1.4 Copyright (C) 1994-2008 Lua.org, PUC-Rio $
$Authors: R. Ierusalimschy, L. H. de Figueiredo & W. Celes $
$URL: www.lua.org $
```

补充证据：

| 证据 | 地址 | 说明 |
|---|---|---|
| `LUA_ERRRUN` / `LUA_ERRSYNTAX` / `LUA_ERRMEM` / `LUA_ERRERR` / `LUA_ERRFILE` | `0xa476cc`–`0xa47700` | Lua 5.1 `lua.h` 的 `LUA_ERR*` 常量名 |
| `lua_debug> ` | `0xa4f7e4` | Lua 5.1 解释器调试提示串 |
| `LUA_PATH` / `LUA_CPATH` | `0xa507a0` / `0xa507e8` | `luaopen_package` 的环境变量名 |
| `luaopen_package` 结构 | `0x933b40` | 与 Lua 5.1 `loadlib.c` 的 `luaopen_package` 逐项对应（见 §4） |

**结论**：需要的是 **官方 Lua 5.1.4 源码**，不是从 exe 抽取 VM 代码。版本字符串、错误常量和 `luaopen_package` 结构三者一致。

> 注意：`0xa4751e` 没有直接指针引用（不是被 `lua_version()` 之类函数取得的普通 C 字符串），但它是编译进二进制的 `LUA_IDENT` 常量；不影响版本结论。

---

## 2. VM 创建链

### 2.1 `luaL_newstate` = `0x8e8740`

**Observed。** 反汇编：

```asm
008e8740  push esi
008e8741  push 0
008e8743  push 0x8e86e0
008e8748  call 0x903d60          ; lua_newstate(alloc=0x8e86e0, ud=NULL)
008e874d  mov esi, eax
008e874f  add esp, 8
008e8752  test esi, esi
008e8754  je 0x8e8764
008e8756  push 0x8e8710
008e875b  push esi
008e875c  call 0x8c2be0          ; lua_atpanic(L, panic=0x8e8710)
008e8761  add esp, 8
008e8764  mov eax, esi
008e8766  pop esi
008e8767  ret
```

这与 Lua 5.1 `lauxlib.c` 的 `luaL_newstate` 完全同构：

```c
LUA_API lua_State *luaL_newstate (void) {
  lua_State *L = lua_newstate(l_alloc, NULL);
  if (L) lua_atpanic(L, &panic);
  return L;
}
```

因此：

| 地址 | 推断 API | 置信度 |
|---|---|---|
| `0x903d60` | `lua_newstate` | High |
| `0x8e86e0` | `l_alloc`（Lua 默认分配器） | High |
| `0x8c2be0` | `lua_atpanic` | High |
| `0x8e8710` | Lua 默认 panic 函数 | Medium |
| `0x8e8740` | `luaL_newstate` | High |

### 2.2 `FSLuaSimple` 构造 = `0x909910`

**Observed。** 伪代码：

```c
iVar1 = FUN_008e8740();              // luaL_newstate
*param_1 = iVar1;
if (iVar1 != 0) {
  FUN_0092da90(iVar1);
  FUN_0092da70(*param_1);
  FUN_0092d140(*param_1);
  FUN_0092b540(*param_1);
  FUN_0092add0(*param_1);
  return param_1;
}
FUN_007f75b0(2, "FSLuaSimple : create luavm fail");
```

**Inferred**：创建的 5 个 `luaopen_*` 调用是 Lua 5.1 标准库打开函数。具体哪个函数对应 `base/table/string/math/io/os` 尚未逐一确认，记为 Unknown；不影响“官方 Lua 5.1.4”结论。

### 2.3 `FSLua` 构造 = `0x8c5720`

**Observed。** 关键序列：

```c
FUN_00909910();                          // FSLuaSimple 基类构造（创建 VM）
*param_1 = &PTR_FUN_00a477bc;            // FSLua vtable
...
FUN_008c33a0(state, &LAB_008c5410, 0);   // push C closure: FSLuaInstall
FUN_008c3720(state, 0xffffd8ee, "FSLuaInstall");
                                         // lua_setfield(L, LUA_GLOBALSINDEX, "FSLuaInstall")
FUN_008bb9b0(param_1, "FSLuaSystem");
FUN_008bb9b0(param_1, "FSLuaIntBits");
FUN_008bb9b0(param_1, "FSLuaUIData");
```

- `0xffffd8ee` = `-10002` = Lua 5.1 `LUA_GLOBALSINDEX`。
- `FUN_008c33a0` 在栈上写入 TValue，tag = 6（`LUA_TFUNCTION`），因此是 `lua_pushcclosure` 的封装（**High**）。
- `FUN_008c3720` 以伪索引 + 字符串名设置字段，对应 `lua_setfield`（**High**）。
- `FUN_008bb9b0` 通过 `FSLuaPluginsMng` 注册 Lua 可见类名（**Observed**；具体注册表结构见 §3.4）。

---

## 3. 宿主 API

### 3.1 `FSLuaWndScript` 方法表 = `0x8fc970`

**Observed。** `FUN_008fc970` 依次注册 9 个方法，每个方法先构造 std::string 名称，再在方法表里写入 C 函数指针：

| Lua 方法名 | C 实现地址 | 备注 |
|---|---|---|
| `RegMsgCall` | `0x8fc660` | 注册窗口消息回调（核心） |
| `RegAfterCreate` | `0x8fc6d0` | 创建后回调 |
| `RegBeforeRelease` | `0x8fc740` | 释放前回调 |
| `RegWinRun` | `0x8fc7b0` | 窗口运行回调 |
| `RegWinDrawThis` | `0x8fc820` | 窗口自绘回调 |
| `RegWinPreDraw` | `0x8fc890` | 绘制前回调 |
| `RegWinSysClose` | `0x8fc900` | 系统关闭回调 |
| `LexOutput` | `0x8fc430` | 词法/文本输出 |
| `LexGoto` | `0x8fc4b0` | 词法跳转 |

**Observed**：`0x8fc660` 在 Ghidra 中未被识别为独立函数（显示为 `LAB_008fc660`），本文用 Capstone 直接反汇编确认。

### 3.2 `RegMsgCall` 语义（2026-11-10 按原脚本修正）

**Observed（原脚本调用点）。** 真实 `config.pak` 的 `UI/login/bulletin2.scp`
（`manifests/batches/nz0a-config-full.csv` 记录 NZ type `0x000A`、解码后
5,812 B、SHA-256 `c9d07f9e…`；`manifests/batches/scp-json-full.csv` 记录
1 个 `#script` 块、42 个窗口块）中，`#script` 以**单参数**形式注册回调：

```lua
FSLuaWndScript.RegMsgCall( "OnShowPolicy" )
```

按钮侧对应：

```text
MESSAGE FSM_BTN_MSG 1 CODE "OnShowPolicy"
```

因此参数 1 是 **Lua 全局函数名**，不是“消息名 + 函数”二元组；`CODE` 才是
消息触发键。

> 端到端复核：`dhsh_test_lua_host --real <shdata>` 通过 `PakResourceProvider`
> 只读挂载 `config.pak`，读取该路径的 NZ 数据并由 `NzDecoder` 解码；测试保留完整
> `#script`，构造真实窗口树，确认注册 3 个回调，再点击原坐标按钮并验证
> `OnShowPolicy` 对面的三个面板可见性与三个按钮启用状态的原脚本修改。

**Observed。** `0x8fc660` 反汇编（原有证据）：

```asm
008fc660  push ecx
008fc661  push esi
008fc662  mov esi, dword ptr [esp + 0xc]     ; esi = lua_State*
008fc666  push edi
008fc667  push esi
008fc668  call 0x8f4510                      ; 取当前 FSLuaWndScript*
008fc66d  mov edi, eax
008fc66f  add esp, 4
008fc672  test edi, edi
008fc674  je 0x8fc6b2                        ; 失败分支
008fc676  mov eax, dword ptr [edi + 0x114]
008fc67c  test eax, eax
008fc67e  je 0x8fc6b2                        ; 脚本未启用
008fc680  push 1
008fc682  push esi
008fc683  call 0x8c4280                      ; 按参数 1 构造回调对象（角色待重析）
008fc688  add esp, 8
008fc68b  test eax, eax
008fc68d  mov dword ptr [esp + 8], eax
008fc691  je 0x8fc6b2
008fc693  lea eax, [esp + 8]
008fc697  push eax
008fc698  lea ecx, [edi + 0x13c]
008fc69e  call 0x8fc620                      ; 挂入窗口回调链 +0x13c
008fc6a3  push 1
008fc6a5  push esi
008fc6a6  call 0x8b88b0                      ; 返回值/栈收尾
```

辅助函数：

- `0x8c4280(L, 1)`：读取参数 1 的字符串。**旧文**把它断言为“消息名”，
  并把 `0x8c4060(L, 2)` 断言为“参数 2 的函数引用”；该解释与真实调用点
  矛盾（原脚本没有第二个函数参数）。两者如何协作（按名称查全局函数、
  构造 0x18 回调对象、建立 registry ref）仍 **Unknown**；需在当前 SHA 上
  重新反汇编 `0x8c4280` / `0x8c4060` 才能闭合。
- `0x8fc620`：把回调对象插入 `FSLuaWndScript + 0x13c` 的双向链表。**Observed**。
- `0x8b88b0`：调用 `0x8c3440`；`0x8c3440` 反编译为写入 tag=1、值
  `(param_2 != 0)`，即 **`lua_pushboolean`**（High）。因此成功分支返回
  `true`；失败分支 `0x8b8630` 弹出 1 个栈值、不压 `false`，即返回 `nil`（Observed/Inferred）。

**Inferred（与原生实现一致的最小模型）**：

```text
RegMsgCall(callbackName:string) -> bool
  1. 取当前窗口脚本对象；不存在或未启用则失败；
  2. 在 Lua 全局表按 callbackName 取函数；
  3. 不是 function 则报错；
  4. 用 luaL_ref 捕获该函数引用；
  5. 记录 callbackName -> reference；
  6. 消息分发时用 SCP MESSAGE 的 CODE 查表并调用。
```

> **旧推断作废**：本文旧版写 `RegMsgCall(messageName:string, callback:function)`
> 并称 `0x8c4060(L, 2)` 捕获参数 2。真实 `bulletin2.scp` 只传一个字符串，
> 没有传函数；以原脚本调用点为准。原脚本回调声明为 `function(p1, p2)`，
> 但已观察函数体未使用参数，参数语义仍 Unknown；原生宿主目前以 0 参数调用。
>
> 原静态失败分支（无脚本对象/脚本未启用/回调对象分配失败）返回 `nil`；
> 原生宿主对“名字不存在或不是函数”选择 `luaL_error`，避免静默漏注册。
> 这一选择是 M1 的显式诊断行为，不是对原失败分支的逐字节复刻。

### 3.3 `FSLuaInstall` 全局函数 = `0x8c5410`

**Observed。** 入口先取 `FSLuaPluginsMng`（`0x8bb8f0`），构造参数字符串，查类名：

```c
FUN_008c5410(state):
  mgr = FUN_008bb8f0();                       // 单例
  FUN_00817460(...);                          // 取 Lua 参数字符串
  if (arg == NULL) return;
  plugin = FUN_008c5140(...);                 // 按名称查找插件/类
  if (plugin == NULL) {
      FUN_007f75b0(3, 0xa47878);              // "非注册脚本启用脚本功能失败。"
      ...
      return;
  }
  if (状态 == 2) {
      FUN_008b8920(state, arg, 0, 0);         // 推断：再次加载/启用
  } else {
      FUN_008c5040(plugin, arg, 1);           // 设置插件状态
      cls = FUN_008baf60(name);               // 取类描述
      ...
  }
```

相关诊断串（GBK，位于 `.rdata`）：

| VA | 文本 |
|---|---|
| `0xa47300` | `调用注册函数“%s”，但系统没有注册对应处理` |
| `0xa47838` | `脚本调用注册函数“%s”，但此处理属于递归调用或上次调用发生异常` |
| `0xa47878` | `非注册脚本启用脚本功能失败。` |

**Inferred 签名**：`FSLuaInstall(classOrPluginName: string)`，用于按名称激活已注册的 Lua 插件类（`FSLuaSystem` / `FSLuaIntBits` / `FSLuaUIData` 等）。返回值和“非注册脚本”分支的完整语义仍 Unknown。

### 3.4 `FSLuaPluginsMng` 单例 = `0x8bb8f0`

**Observed。** `FUN_008bb8f0` 是懒加载单例（`DAT_00cb0470`）：

```c
if (DAT_00cb0470 == 0) {
  if (DAT_00cb0474 != 0) FUN_007f75b0(1, "InstanceErrorFSLuaPluginsMng", ...);
  DAT_00cb0474++;
  pv = operator_new(0xa8);
  DAT_00cb0470 = FUN_008ba3d0(pv);            // 构造管理器
  FUN_007ee580("FSLuaPluginsMng", &LAB_008bb8c0);
  FUN_008bb550(); FUN_008bb760(); FUN_008bb6b0();
  FUN_008bb600(); FUN_008bb810();             // 注册内置插件类
}
return DAT_00cb0470;
```

`FUN_008bb9b0(manager, name)` 按名称把类注册进管理器；`FSLua` 构造时注册 `FSLuaSystem`、`FSLuaIntBits`、`FSLuaUIData`。**Observed**。

---

## 4. 脚本加载与搜索路径

### 4.1 `luaopen_package` = `0x933b40`

**Observed。** 伪代码与 Lua 5.1 `loadlib.c` 的 `luaopen_package` 逐项对应：

```c
FUN_008e7f50(L, "_LOADLIB");                  // luaL_findtable / 取 _LOADLIB
FUN_008c33a0(L, &LAB_00933260, 0);            // push loadlib C function
FUN_008c3720(L, 0xfffffffe, &DAT_00a4c1d0);   // setfield(package, ...)
FUN_008e8ea0(L, "package", &PTR_s_loadlib_00a504fc);  // luaL_register(L,"package",pk_funcs)
...
if (getenv("LUA_PATH") == NULL)
    lua_pushstring(L, ".\\?.lua;!\\lua\\?.lua;!\\lua\\?\\init.lua;!\\?.lua;!\\?\\init.lua");
...
if (getenv("LUA_CPATH") == NULL)
    lua_pushstring(L, ".\\?.dll;!\\?.dll;!\\loadall.dll");
...
FUN_008e80a0(L, 0xffffd8f0, "_LOADED", 2);    // luaL_findtable(_LOADED)
...
```

默认 `package.path`：

```text
.\?.lua;!\lua\?.lua;!\lua\?\init.lua;!\?.lua;!\?\init.lua
```

默认 `package.cpath`：

```text
.\?.dll;!\?.dll;!\loadall.dll
```

`!` 是原客户端自定义的根路径占位符，具体展开规则 Unknown；M1 不需要动态加载 DLL，可忽略 `cpath`。

### 4.2 模块路径构造 = `0x8c47c0`

**Observed。** 先拼 `%s%s.lua`，失败后读取全局 `fspath`（`lua_getfield(L, LUA_GLOBALSINDEX, "fspath")`），按 `;` 分段，再拼 `%s/%s%s.lua`，逐段调用文件加载器：

```c
snprintf(buf, 0x3ff, "%s%s.lua", dir, module);
if (FUN_0080f9f0(buf, out) != 0) return;
FUN_008c34f0(state, -10002, "fspath");         // lua_getfield(L, GLOBALSINDEX, "fspath")
if (lua_type(L, -1) == 4) {                   // LUA_TSTRING
    fspath = lua_tostring(L, -1);
    for (seg in split(fspath, ';')) {
        snprintf(buf, 0x3ff, "%s/%s%s.lua", seg, dir, module);
        if (FUN_0080f9f0(buf, out) != 0) return;
    }
}
```

- `%s%s.lua` 位于 `0xa47758`；`%s/%s%s.lua` 位于 `0xa47744`。
- `FUN_0080f9f0` 是引擎通用文件加载器：先 `FUN_0080f880(path)` 判断存在，再 `FUN_008317d0(path, out, 1, 0)` 读取。
- 读取到的字节如何交给 Lua：见 §4.4，已定位 `luaL_loadbuffer` / `lua_pcall`。

### 4.3 具体脚本入口（Observed）

| 脚本/配置 | 加载点 | 说明 |
|---|---|---|
| `plugins\plugins.lua`、`plugins\plugins2.lua` | `0x410ac0` | 字符串位于 `0x97ed50` / `0x97ed38` |
| `config/taskhelp.lua` | `0x5df160` | 字符串位于 `0x9de2e8` |
| `config/lua/LuaWindowMap.csv` | `0x8ee370` | CSV 逐行读取；条目经 `0x8ee1c0` 注册（窗口名→ID/序号，具体字段 Unknown） |
| `.lua` / `luac` 扩展名处理 | `0x916480` | 检查引号包裹的参数与扩展名；`#include` 错误串 `0xa4d650` |

`#include` 相关诊断（Observed）：

```text
#include 后面的字符串[%s]太短了.
```

### 4.4 脚本字节 → `luaL_loadbuffer` → `lua_pcall`（当前 SHA 已闭合）

**Observed。** 引擎脚本加载链如下：

```text
引擎文件加载器 0x80f9f0（读 PAK/磁盘，得到 buffer + size）
        │
        ├─ FSLuaSimple::DoBuffer 0x8c4620(this,buf,size,runFlag,chunkName)
        │       ├─ luaL_loadbuffer 0x8e86b0
        │       │       └─ lua_load 0x8c3a80
        │       └─ lua_pcall 0x8c3a10
        │               └─ luaD_pcall 0x903950
        │
        ├─ plugins 加载器 0x7a7dd0（chunkName="FSLuaPluginsMain"）
        └─ 另一处脚本加载器 0x4b88d0
```

Lua API 地址（当前 SHA，置信度 High）：

| API | 地址 | 判定依据 |
|---|---|---|
| `luaL_loadbuffer` | `0x8e86b0` | 设 `LoadS{buf,size}`，调用 `lua_load(L, reader=0x8e8690, &s, name)` |
| `luaL_loadfile` | `0x8e8450` | `loader_Lua`（`0x9334c0`）调用；内部 `lua_load` |
| `lua_load` | `0x8c3a80` | 被 `0x8e8450` / `0x8e86b0` / Lua `load` 调用 |
| `lua_pcall` | `0x8c3a10` | 四参数 `(L,nargs,nresults,errfunc)`；调用 `luaD_pcall`（`0x903950`）；处理 `LUA_MULTRET=-1` |
| `lua_pushboolean` | `0x8c3440` | TValue tag=1，值 `(arg != 0)` |
| `luaL_newstate` | `0x8e8740` | 见 §2.1 |

`FUN_008c4620` 的行为（Observed）：

```c
DoBuffer(this, buf, size, runFlag, chunkName):
  state = this->state;                       // +0x04
  if (!state || !buf || !size) return 0;
  name = chunkName ? chunkName : "FSLuaScriptBase";
  status = luaL_loadbuffer(state, buf, size, name);
  if (status != 0) { report_error(state, name, status); return 0; }

  if (runFlag) {
      if (chunkName && *chunkName)
          lua_pushstring(state, chunkName);
      status = lua_pcall(state,
                         chunkName ? 1 : 0,
                         chunkName ? 1 : LUA_MULTRET,
                         0);
      if (status != 0) { report_error(state, name, status); return 0; }
  }
  // 诊断：把前 0x3c 字节写成 "MEM:xx xx ..." 存入 this+0x14
  return 1;
```

- `0xffffffff` 作为 `nresults` 即 Lua 5.1 `LUA_MULTRET`。
- 错误报告函数 `0x8c4370` 按状态码 2–6 输出 `LUA_ERRRUN` / `LUA_ERRSYNTAX` / `LUA_ERRMEM` / `LUA_ERRERR` / `LUA_ERRFILE`，与 §1 的常量串一致。
- `0x7a7dd0` 以 `chunkName="FSLuaPluginsMain"` 调同一 `luaL_loadbuffer`，随后 `lua_pcall(0, LUA_MULTRET, 0)`；`0x4b88d0` 结构相同，chunk 名由调用方传入。
- `loader_Lua`（`0x9334c0`）走 `findfile` + `luaL_loadfile`，供 Lua `require` 使用，与引擎直接 loadbuffer 是两条并行路径。

---

## 5. 对原生客户端实现的含义

1. **依赖选择**：使用官方 Lua 5.1.4 源码（或等价的 5.1.4 ABI 实现），不要抽取 `Game.exe` 内的 VM 机器码。
2. **必须实现的宿主面（M1 最小集）**：
   - `FSLuaWndScript.RegMsgCall`：回调名 → Lua 全局函数引用 → 窗口回调链；
   - `FSLuaWndScript` 其余 8 个注册方法可先提供显式“未实现”错误；
   - 全局 `FSLuaInstall`：识别已知名称；未知名称必须显式报错，不能静默成功；
   - `FSLuaSystem` / `FSLuaIntBits` / `FSLuaUIData`：当前未提供；只在真实脚本命中时按实际方法补，不允许静默成功。
3. **脚本来源**：原 `.lua` 从 PAK/NZ 解出后按原路径喂给 Lua；`package.path` 的 `!` 根路径在原生客户端里映射到资源根，不需要复刻 Windows 工作目录语义。
4. **SCP 桥接**：SCP `#script` 的函数注册走 `RegMsgCall`；消息分发时按 `MESSAGE ... CODE` 的回调名查 `+0x13c` 回调链。`FSM_*` 名称到消息 ID 的映射在 SCP 解析器族（`0x90c040`）中，属于 Task 15。
5. **验证方式**：先用一个原样 Lua 片段调用 `RegMsgCall`，断言回调被挂入链；再由 SCP 按钮消息触发，断言 Lua 函数被调用。`luaL_error` 必须带脚本名和行号。

---

## 6. 未决项（Unknown / 待验证）

| 项 | 状态 | 下一步 |
|---|---|---|
| 5 个 `luaopen_*` 的具体库 | Unknown | 逐个反编译 `0x92da90` / `0x92da70` / `0x92d140` / `0x92b540` / `0x92add0`，对照 Lua 5.1 `linit.c` |
| `RegMsgCall` 参数语义 | **Resolved（以原脚本为准）** | 单参数回调名；`CODE` 触发，见 §3.2 |
| `RegMsgCall` 的返回语义 | **Resolved** | `0x8c3440` = `lua_pushboolean`；成功 `true`，失败 `nil`（见 §3.2） |
| 回调对象 0x18 字节布局 | Partially observed | 在单参数语义下重新反编译 `0x8c3ed0` / `0x8c4060`，确认 `0x8c4280` 如何按名称取全局函数、`+0x10/+0x14` 的 ref/upvalue 角色 |
| 脚本字节到 `luaL_loadbuffer` / `lua_pcall` | **Resolved** | 见 §4.4：`0x8c4620` → `0x8e86b0` → `0x8c3a10` |
| `package.path` 中 `!` 的展开 | Unknown | 搜索 `!` 处理逻辑或 `fspath` 赋值点 |
| `LuaWindowMap.csv` 字段 | Partially observed | 读取真实 CSV 样本 + 反编译 `0x8ee1c0` |
| `FSLuaSystem` / `FSLuaIntBits` / `FSLuaUIData` 方法面 | Unknown | 反编译对应类注册函数 |
| `#include` 与 `.lua`/`luac` 的调度关系 | Unknown | 反编译 `0x916480` 的调用者 |
| 回调参数 `p1,p2` 语义 | Unknown | 原脚本函数体未使用；需找到真实消费点或运行证据 |

---

## 7. 原生宿主实现状态（Task 16，2026-11-10）

**已实现。**

- 官方 Lua 5.1.4（`client/third_party/lua-5.1.4`），打开 base/table/string/math；
- 逐块执行 SCP `#script`，chunk 名为 SCP 源文件名；
- 全局 `FSLuaWndScript.RegMsgCall(callbackName)`：按名称捕获 Lua 全局函数引用；
- 按 `LUACONTROL` 把控件暴露为 Lua 全局 userdata，提供 `SetShow(bool)`、`Enable(bool)`；
- 按 `MESSAGE ... CODE` 订阅 `MessageBus`，消息触发对应回调；
- `RegAfterCreate` 等其余 8 个 `FSLuaWndScript` 方法、未知 `FSLuaInstall` 均抛带源文件/行号的 Unsupported 错误；
- `FSLuaInstall("FSLuaFSButton")` / `FSLuaInstall("FSLuaFSWindow")` 作为宿主已直接提供的控件功能接受（no-op），其余名称不静默成功。

**未实现 / 限制。**

- 回调参数语义 Unknown，当前 0 参数调用；
- `FSLuaSystem` / `FSLuaIntBits` / `FSLuaUIData`、模块加载、生命周期钩子未实现；
- Lua 回调运行期错误会经 `MessageBus` 重新抛出；App 的事件回调/WndProc 路径
  尚未做异常隔离或诊断落盘（测试未覆盖运行期 Lua 错误）。
- `SetShow` 只改可见位；构造时因隐藏而未加载的图片不会按需补载；
- 父子可见性没有动态级联，仍以控件构造时的可见性为基础；
- 语法错误行号相对 `#script` 内提取出的脚本内容（块首空行会被压缩），不是 SCP 文件绝对行号。

**App 接入状态（检查结论）。**

- `App::run` 当前只加载 `ui/main/bar.scp` 和 `ui/main/fucnbtn_shortcut.scp`，
  没有加载 `config.pak` 的真实 `UI/login/bulletin2.scp`，也没有登录公告流程。
- `manifests/batches/scp-json-full.csv` 中对应的根 SCP 行均为 `scripts=0`
  （`800config/ui/main/bar.scp`：44 窗口/0 脚本；`config/UI/main/fucnbtn_shortcut.scp`：
  9 窗口/0 脚本，虽有 3 个 include）。`ScpWindow` 只在 `document.scripts`
  非空时创建 `FsLuaHost`，因此**当前 App 实际不会执行任何真实 SCP Lua 回调**。
- 事件链本身已通：`ScpWindow::handle_event` → `MessageBus::dispatch` →
  `FsLuaHost` 订阅 → Lua。因而只要被加载的 SCP 含 `#script`（注册回调）
  和匹配 `CODE` 的 `MESSAGE`，回调会真实执行；回调操作控件还需要
  `LUACONTROL` 提供 Lua 全局名。
- 另一个缺口：`ScpParser::load` 目前只合并 `.scp` include 的窗口/警告，**不合并
  被 include 文件的 `#script` 块**；而 `fucnbtn_shortcut.scp` 的 3 个 include
  全是 `.lua`（`activebtn.lua` / `CrossSrvFBbtn.lua` / `SeriesFestivalBtn.lua`），
  `ScpParser` 会直接忽略非 `.scp` include。这些 Lua 目前都不会执行。
  真实 `bulletin2.scp` 的脚本在根文件，直接加载它不受影响，但依赖
  include Lua 的窗口会静默缺少回调。
- 尚缺：① 加载公告窗口（真实文件 42 个窗口块；测试夹具只覆盖三按钮行为，
  不覆盖完整文件）；
  ② UI 状态变更后的重绘通知（Lua `SetShow`/`Enable` 不会设置 `App::dirty`）；
  ③ 隐藏控件的按需图片加载；④ 真实脚本命中的其余 API。未提前扩大实现。
- 因此当前测试证明的是**夹具上的消息 → Lua → 控件状态链**，不是完整
  `bulletin2.scp` 的端到端验证；App 也没有执行真实回调。

---

## 8. 证据文件

- `extracted/_batches/lua-task16/evidence1.json` — 旧地址初查（无函数）
- `extracted/_batches/lua-task16/evidence2.json` — `0x8fc970` / `0x8c5720` / `0x90c040` / `0x8bb400` + 字符串 xref
- `extracted/_batches/lua-task16/evidence3.json` — `0x8c33a0` / `0x8c3720` / `0x8bb9b0` / `0x8bb8f0` / `0x909910`
- `extracted/_batches/lua-task16/evidence4.json` — Lua 路径字符串 xref
- `extracted/_batches/lua-task16/evidence5.json` — `0x8c47c0` / `0x933b40` / `0x8ee370` / `0x916480`
- `extracted/_batches/lua-task16/evidence6.json` — `0x8fc620` / `0x8c4280` / `0x8b88b0` / `0x8b8630` / `0x8f4510` / `0x80f9f0` / `0x8c5040`
- `extracted/_batches/lua-task16/evidence7.json` — `FSM_*` 名称→ID 全表（`0x90c040` 初始化代码）
- `extracted/_batches/lua-task16/evidence8.json` — `loader_Lua` / `loader_preload` / `require`
- `extracted/_batches/lua-task16/evidence9.json` — `luaL_loadfile`（`0x8e8450`）与 lauxlib 邻域函数
- `extracted/_batches/lua-task16/evidence10.json` — `lua_load` / `luaL_loadfile` 的调用者
- `extracted/_batches/lua-task16/evidence11.json` — `luaL_loadbuffer`（`0x8e86b0`）与 Lua `load`/`loadfile`
- `extracted/_batches/lua-task16/evidence12.json` — `luaL_loadbuffer` / Lua 调用辅助函数的 xref
- `extracted/_batches/lua-task16/evidence13.json` — 引擎 `DoBuffer`（`0x8c4620`）/ plugins 加载器（`0x7a7dd0`）
- `extracted/_batches/lua-task16/evidence14.json` — `lua_pcall`（`0x8c3a10`）/ 错误报告（`0x8c4370`）/ `lua_load`（`0x8c3a80`）
- `manifests/batches/nz0a-config-full.csv` — 真实 `config.pak:UI/login/bulletin2.scp`：NZ type `0x000A`、解码后 5,812 B、SHA-256 `c9d07f9e…`
- `manifests/batches/scp-json-full.csv` — 同上 SCP：1 个 `#script` 块、42 个窗口块
- `manifests/batches/scp-json-full.csv` — App 当前加载的 `ui/main/bar.scp`
  与 `ui/main/fucnbtn_shortcut.scp` 对应根 SCP 行均为 0 个根 `#script` 块（见 §7 App 结论）
- `manifests/batches/scp-include-graph.json` — `fucnbtn_shortcut.scp` 的 3 个
  include 均为 `.lua`（`activebtn.lua` / `CrossSrvFBbtn.lua` / `SeriesFestivalBtn.lua`）
- `client/tests/test_lua_host.cpp` — 三按钮状态切换的最小复现夹具（不是完整原文件；验证单参数 `RegMsgCall` 行为）
- `docs/formats/scp.md` §7 — SCP 消息绑定与旧桥接说明（已加单参数修正指引）

地址迁移提醒：旧文档中的 `0x8fc520`（RegMsgCall）、`0x8c55e0`（FSLuaInstall）、`0x8fc830`（FSLuaWndScript）属于旧 SHA；当前 `7a5e…` 的正确入口见本文。
