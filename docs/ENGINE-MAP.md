# Engine Map（客户端静态分析索引）

- 对象：`D:\software\dhsh\shdata\Game.exe`（Hero2D，32 位）
- 基线 SHA-256：`7a5e2c085ac79557ca104a2917e06f1a7e5f2827e4b68753bbaf1c754a4e78dc`
- 分析副本：`extracted/gameexe-snapshots/Game-7a5e2c08…exe`
- 口径：VA（镜像基址 `0x400000`）；`.text` 文件偏移 = VA − `0x400000`
- 原则：只记录**当前 SHA 上复核过**的地址；旧 SHA 结论单独标注，不混用

本文件只做索引和边界说明，具体格式见 `docs/formats/`。`Game.exe` 会被启动器/保护组件改写，磁盘 SHA 不是稳定基线；做静态分析前先 `python tools/snapshot_gameexe.py`，以 pinned 副本为准。

---

## 1. 可执行文件结构（Observed）

| 项 | 值 |
|---|---|
| 架构 | x86（PE32，Machine `0x14c`） |
| 大小 | 7,111,456 字节 |
| 镜像基址 | `0x400000` |
| 段 | `.text` VA `0x1000` / 5.5 MiB；`.rdata` `0x57d000`；`.data` `0x6a9000`；`.rsrc` `0x8b4000`；第二 `.text` `0x8bd000` |
| 导入 DLL | `WS2_32`、`fmodex`、`DDRAW`、`GDI32`、`GDI+`、`USER32`、`KERNEL32`、`WININET`、`NEP2` 等 |
| Lua | 静态链接 **Lua 5.1.4**，无 `lua5x.dll` 导入 |

`NEP2.dll` 的导入表没有具名函数（0 个 named import），属于保护/反调试组件；本工作区不分析、不加载、不运行。

---

## 2. 资源加载

### 2.1 原生客户端侧（已实现）

原生客户端的资源层已经不再依赖 `Game.exe`，实现位于 worktree：

| 模块 | 文件 | 验证 |
|---|---|---|
| PAKF 索引 | `client/src/resource/PakArchive.cpp` | `config.pak` 2,470 叶子 |
| NZ 0/0A/0B | `client/src/resource/NzDecoder.cpp` | 60 个真实样本逐字节一致 |
| 统一资源接口 | `PakResourceProvider.cpp` / `CachedImageProvider.cpp` | 挂载顺序、错误契约 |
| A16/CTI | `client/src/formats/A16.cpp` / `Cti.cpp` | 48 A16、4 CTI × 2 表 |
| APG/PIF | `client/src/formats/Apg.cpp` / `Pif.cpp` | 45 帧真实动画 |
| 地图 | `client/src/formats/MapFormats.cpp` | 应天府 486 个 core mask |

### 2.2 原作静态入口（当前 SHA 已复核）

| 功能 | 当前地址 | 证据 |
|---|---|---|
| 地图加载器（MASKFILE→`.core`） | `0x4278c0` | `docs/VERIFY-2026-10-23.md` §2 |
| MSK/core 解析器 | `0x4272d0` | 同上 |
| ApplyMask 入口 | `0x4266d0` | 同上 |
| 地线深度测试 | `0x426756` | 同上 |
| MMX 暗化 | `0x4264b0` | 同上 |
| group/frame 切换 | `0x426980` | 同上 |
| 地图对象加载链 | `0x213a3` → `0x27aa0`（旧 SHA） | `docs/formats/map.md`；新 SHA 需迁移 |
| NZ 0x0B LZO1X 解码 | `0x8a6c90`（旧 SHA） | `docs/formats/nz.md`；新 SHA 需迁移 |
| NZ 0x0A LZW 解码 | `0x8a6f60`（旧 SHA） | 同上 |

**结论**：资源加载的“行为”已经由 Python 参考实现和 C++ 客户端双重验证；剩余工作是把旧 SHA 的 PAK/NZ 地址迁移到 `7a5e…`，仅在需要解释原作行为时使用。

---

## 3. Lua / SCP / UI

专项文档：`docs/formats/lua-runtime.md`（本次新增）。

当前 SHA 已复核入口：

| 功能 | 地址 | 置信度 |
|---|---|---|
| `luaL_newstate` | `0x8e8740` | High |
| `lua_newstate` / `lua_atpanic` | `0x903d60` / `0x8c2be0` | High |
| `luaL_loadbuffer` | `0x8e86b0` | High |
| `luaL_loadfile` | `0x8e8450` | High |
| `lua_load` | `0x8c3a80` | High |
| `lua_pcall` | `0x8c3a10` | High |
| `lua_pushcclosure` | `0x8c33a0` | High |
| `lua_setfield` | `0x8c3720` | High |
| `lua_pushboolean` | `0x8c3440` | High |
| `luaopen_package` | `0x933b40` | High |
| `FSLuaSimple` 构造（创建 VM） | `0x909910` | Observed |
| `FSLua` 构造（注册宿主类） | `0x8c5720` | Observed |
| `FSLuaSimple::DoBuffer`（loadbuffer + pcall） | `0x8c4620` | Observed |
| `FSLuaWndScript` 方法注册 | `0x8fc970` | Observed |
| `RegMsgCall` C 实现 | `0x8fc660` | Observed（Ghidra 未识别为函数，Capstone 确认） |
| `FSLuaInstall` 全局函数 | `0x8c5410` | Observed |
| `FSLuaPluginsMng` 单例 | `0x8bb8f0` | Observed |
| SCP 解析器族 | `0x90c040` | 已定位（117 KB 函数） |
| SCP 消息名 → ID 表 | `0x90c040` 内 | `docs/formats/scp.md` §8，51 个 `FSM_*` |
| 模块路径构造 | `0x8c47c0` | Observed |
| LuaWindowMap CSV | `0x8ee370` | Observed |

---

## 4. 通信

专项文档：`docs/formats/net-protocol.md`。

已离线闭合的部分：

- 帧格式：`[u16 长度][加密载荷]`；
- c2s 首帧：Blowfish-ECB 静态主密钥；后续：标准 RC4，密钥来自 93B 凭据块 + MD5 大写 hex；
- s2c：静态密钥表 `0xa43eb8` + 逐帧自包含位置算法；
- 登录网关 `101.42.255.186:8000` 全程明文；游戏服 s2c 从首帧起即加密。

注意：该文档中的函数地址属于旧 SHA（`0x819610` 等），当前 `7a5e…` 需要按字节签名重新校准；算法本身与 SHA 无关，可直接移植。M1 不联网，通信属于 M2。

---

## 5. 保护/未知组件（不做实现依赖）

| 组件 | 处理 |
|---|---|
| `NEP2.dll` / `NEPDaemon.exe` | 不加载、不分析运行时行为 |
| `CProtect.exe` / `CProtect1.exe` / `*.sys` | 不运行 |
| `VMProtectSDK32.dll` | 仅记录存在 |
| `ACDC.dat`(gzip)、`ccore.dat`、`cltgp.dat`、`sl.dat`、`001.cpm` | 高熵/加密块，待定位用途 |

---

## 6. 证据与迁移规则

1. 只把**当前 SHA 复核过**的地址写进实现或测试；旧 SHA 地址必须标注并迁移。
2. 地址结论必须附字节/反汇编或 Ghidra Evidence，不写“猜的地址”。
3. 行为结论区分 Observed / Inferred / Unknown，详见各格式文档。
4. `Game.exe` 可能被改写：先快照，再用 `extracted/gameexe-snapshots/` 的 pinned 副本分析。
5. 不把反编译伪代码当可编译源码；不抽取固定地址机器码；不把原作保护环境作为新客户端底座。
