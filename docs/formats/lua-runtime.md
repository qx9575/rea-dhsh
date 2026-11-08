# Lua 运行时与宿主 ABI（静态证据）

- 分析对象：`D:\software\dhsh\shdata\Game.exe`
- 基线 SHA-256：`7a5e2c085ac79557ca104a2917e06f1a7e5f2827e4b68753bbaf1c754a4e78dc`
- 分析副本：`extracted/gameexe-snapshots/Game-7a5e2c08…exe`
- Ghidra 项目：`ghidra-proj/dhsh-7a5e`（程序 `Game-7a5e…exe`）
- 地址口径：镜像基址 `0x400000`，文中均为 VA；`.text` 文件偏移 = VA − `0x400000`
- 状态：Task 16 Step 1 证据文档；只做只读静态分析，不运行游戏、不引用保护组件

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

### 3.2 `RegMsgCall` 语义

**Observed。** `0x8fc660` 反汇编：

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
008fc683  call 0x8c4280                      ; 创建回调对象（名称 + Lua 函数引用）
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

结合辅助函数：

- `0x8c4280(L, 1)`：用 `0x8b7de0` 读取 Lua 参数 1 的字符串（推断 `lua_tolstring` / `luaL_checkstring`），分配 0x18 字节回调对象，用 `0x8c4060(L, 2)` 捕获参数 2 的函数引用（推断 `luaL_ref(L, LUA_REGISTRYINDEX)`）。**Inferred**。
- `0x8fc620`：把回调对象插入 `FSLuaWndScript + 0x13c` 的双向链表。**Observed**。
- `0x8b88b0`：调用 `0x8c3440(L, 1)`；后者形态像是 `lua_pushboolean` 或 `lua_settop`，未最终确认。**Unknown**。

**可复刻的最小行为**：

```text
RegMsgCall(messageName:string, callback:function) -> 注册
  1. 取当前窗口脚本对象；不存在或未启用则失败；
  2. 读取参数 1 作为消息名；
  3. 捕获参数 2 的 Lua 函数引用；
  4. 以消息名/引用构造回调节点，挂到窗口回调链；
  5. 消息分发时按消息名找到节点，调用 Lua 函数。
```

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
- 读取到的字节如何交给 Lua（`luaL_loadbuffer` / `luaL_loadfile` / `lua_pcall`）尚未定位，**Unknown**。

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

---

## 5. 对原生客户端实现的含义

1. **依赖选择**：使用官方 Lua 5.1.4 源码（或等价的 5.1.4 ABI 实现），不要抽取 `Game.exe` 内的 VM 机器码。
2. **必须实现的宿主面（M1 最小集）**：
   - `FSLuaWndScript.RegMsgCall`：消息名 + Lua 函数引用 → 窗口回调链；
   - `FSLuaWndScript` 其余 8 个注册方法可先提供显式“未实现”错误；
   - 全局 `FSLuaInstall`：至少识别并报错，不能静默成功；
   - `FSLuaSystem` / `FSLuaIntBits` / `FSLuaUIData`：先注册类名，方法按实际脚本命中逐个补。
3. **脚本来源**：原 `.lua` 从 PAK/NZ 解出后按原路径喂给 Lua；`package.path` 的 `!` 根路径在原生客户端里映射到资源根，不需要复刻 Windows 工作目录语义。
4. **SCP 桥接**：SCP `#script` 的函数注册走 `RegMsgCall`；消息分发时按消息名查 `+0x13c` 回调链。`FSM_*` 名称到消息 ID 的映射在 SCP 解析器族（`0x90c040`）中，属于 Task 15。
5. **验证方式**：先用一个原样 Lua 片段调用 `RegMsgCall`，断言回调被挂入链；再由 SCP 按钮消息触发，断言 Lua 函数被调用。`luaL_error` 必须带脚本名和行号。

---

## 6. 未决项（Unknown / 待验证）

| 项 | 状态 | 下一步 |
|---|---|---|
| 5 个 `luaopen_*` 的具体库 | Unknown | 逐个反编译 `0x92da90` / `0x92da70` / `0x92d140` / `0x92b540` / `0x92add0`，对照 Lua 5.1 `linit.c` |
| `RegMsgCall` 的返回语义（`0x8c3440`） | Unknown | 反编译 `0x8c3440`，确认是 `lua_pushboolean` 还是 `lua_settop` |
| 回调对象 0x18 字节布局 | Partially observed | 反编译 `0x8c3ed0` / `0x8c4060`，确认 `+0x10/+0x14` 是 ref 还是 upvalue |
| 脚本字节到 `luaL_loadbuffer` 的调用点 | Unknown | 从 `0x8317d0` 返回路径继续追 `lua_load` / `lua_pcall` |
| `package.path` 中 `!` 的展开 | Unknown | 搜索 `!` 处理逻辑或 `fspath` 赋值点 |
| `LuaWindowMap.csv` 字段 | Partially observed | 读取真实 CSV 样本 + 反编译 `0x8ee1c0` |
| `FSLuaSystem` / `FSLuaIntBits` / `FSLuaUIData` 方法面 | Unknown | 反编译对应类注册函数 |
| `#include` 与 `.lua`/`luac` 的调度关系 | Unknown | 反编译 `0x916480` 的调用者 |

---

## 7. 证据文件

- `extracted/_batches/lua-task16/evidence1.json` — 旧地址初查（无函数）
- `extracted/_batches/lua-task16/evidence2.json` — `0x8fc970` / `0x8c5720` / `0x90c040` / `0x8bb400` + 字符串 xref
- `extracted/_batches/lua-task16/evidence3.json` — `0x8c33a0` / `0x8c3720` / `0x8bb9b0` / `0x8bb8f0` / `0x909910`
- `extracted/_batches/lua-task16/evidence4.json` — Lua 路径字符串 xref
- `extracted/_batches/lua-task16/evidence5.json` — `0x8c47c0` / `0x933b40` / `0x8ee370` / `0x916480`
- `extracted/_batches/lua-task16/evidence6.json` — `0x8fc620` / `0x8c4280` / `0x8b88b0` / `0x8b8630` / `0x8f4510` / `0x80f9f0` / `0x8c5040`

地址迁移提醒：旧文档中的 `0x8fc520`（RegMsgCall）、`0x8c55e0`（FSLuaInstall）、`0x8fc830`（FSLuaWndScript）属于旧 SHA；当前 `7a5e…` 的正确入口见本文。
