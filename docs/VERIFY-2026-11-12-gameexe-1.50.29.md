# Game.exe 1.50.29 基座重建（2026-11-12）

本文件记录 `D:\software\dhsh2` 客户端于 2026-11-12 更新后，为继续点击光效定位而重建的
PE 事实、地址映射与资源差异。所有结论分为“观察到的字节/结构”与“推断”，推断处已标注。

## 一、版本与环境（已观察）

| 项 | 值 |
|---|---|
| 运行实例 | PID 28816，`D:\software\dhsh2\shdata\Game.exe`，2026-11-12 10:41:05 启动 |
| 客户端版本 | **1.50.29**（`shdata/conf.ini`：`VERSION`/`NEWVERSION`；`shdata/ver.csv`：`1,50,29,release`） |
| exe SHA-256 | `1c0704f9e31c21205accdf41e0c269340795022c3b7770c1bc8dcbb80dd96619` |
| exe 大小 | 7,123,744 字节（mtime 2026-11-12 10:39:34） |
| pinned 副本 | `extracted/gameexe-snapshots/Game-1c0704f9…96619.exe` |
| 对比旧版 | 1.50.27，SHA `0dbc18…b519f`，同大小 7,123,744；pinned 副本 `Game-0dbc18…b519f.exe` |

注意：

- `help/pe.txt` 首行 SHA 是 0dbc18（1.50.27），**对本版已过期**，不能再用其中的
  RVA/导出地址直接挂钩。
- `shdata/save/update.txt` 同时出现 1.50.29 与 1.50.30 两段维护公告；已安装
  `conf.ini`/`ver.csv` 为 1.50.29。若再次自动更新，先重新计算 SHA 并复跑本文件工具。
- 今日更新的 PAK：`ui.pak`、`magic5.pak`、`config.pak`、`perinfo.pak`。

## 二、PE 事实（已观察）

完整 dump：`manifests/batches/gameexe-1.50.29.txt`、`gameexe-1.50.29.json`
（格式沿用 `help/pe.txt` 的 SHA256/Base/SECTION/IMPORT/EXPORT 行）。

- ImageBase `0x00400000`，Entry RVA `0x0053E4EA`（1.50.27 为 `0x0053E4BA`，+0x30），
  SizeOfImage `0x008C3000`（与旧版相同）。
- 节表：
  - `.text` RVA `0x1000`，VS `0x57D4BA`，RAW `0x1000`，SIZE `0x57E000`，entropy 6.49
  - `.rdata` RVA `0x57F000`，VS `0x12C59F`（旧 `0x12C57F`，+0x20），RAW `0x57F000`，SIZE `0x12D000`
  - `.data` RVA `0x6AC000`，VS `0x20A8B8`，RAW `0x6AC000`，SIZE `0x11000`，entropy 4.49（旧 4.40）
  - `.rsrc` RVA `0x8B7000`，RAW `0x6BD000`，SIZE `0x9000`
  - `.text`（第二段）RVA `0x8C0000`，RAW `0x6C6000`，SIZE `0x3000`
- 导出 426 个，名称集合与 1.50.27 **完全一致**（无新增/删除）；导入模块 17 个，与旧版一致。

## 三、构建差异（已观察，重要）

- 两版同大小，但 **5,927,691 字节不同（约 83.2%），共 12,077 个差异区间**。
  这不是小补丁式改动，而是整体重编译；**1.50.27 的绝对地址不能直接复用**。
- 导出函数 RVA 平移分布：`+0x60` 262 个、`+0x90` 90 个、`+0x30` 29 个、`+0x20` 7 个，
  其余 38 个不变。
- 完整差异与锚点复核：`manifests/batches/gameexe-1.50.27-to-1.50.29.json`。

## 四、关键地址映射（1.50.27 → 1.50.29）

“方法”列：`export` = 新导出表精确给出；`bytes` = 旧函数 48 字节在新镜像中唯一匹配且
逐字节相同；`disasm` = 指令逐条对应（调用目标按各自映射平移）；`same` = 同址且字节相同。

| 锚点 | 1.50.27 | 1.50.29 | Δ | 方法 |
|---|---|---|---|---|
| `WWindow::WndProc` | `0x007ED580` | `0x007ED5E0` | +0x60 | export |
| WndProc 内消息表调用点（`call edx`） | `0x007ED654` | `0x007ED6B4` | +0x60 | bytes |
| 消息分发函数（交接文档称 vtable[0] 前置分发） | `0x004168D0` | `0x00416900` | +0x30 | bytes（48B 全同） |
| `WM_LBUTTONDOWN(0x201)` 目标 | `0x00416B45` | `0x00416B75` | +0x30 | bytes + disasm |
| `WM_LBUTTONUP(0x202)` 目标 | `0x00416B99` | `0x00416BC9` | +0x30 | disasm（首 16B 全同） |
| `WSurface::Blit2Screen` | `0x007F8A60` | `0x007F8AC0` | +0x60 | export |
| Blit2Screen 图层 blit 调用点 | — | `0x007F8B99`（`call dword ptr [edx+0x64]`；rect 参数 `0xAE7E10`） | — | disasm |
| `WCanvas::ViewPosDraw` | `0x007F8600` | `0x007F8660` | +0x60 | export + bytes |
| WCanvas::Draw 矩形读取点（`cmp edi,6` / `[esi+4]+0x20`） | `0x007FC161` | `0x007FC1C1` | +0x60 | bytes |
| `WAlphaBitmap::DrawShape` | `0x007F52C0` | `0x007F5320` | +0x60 | export + bytes |
| 透明混合包装（旧 `0x7f3940`） | `0x007F3940` | `0x007F39A0` | +0x60 | bytes |
| MMX 块拷贝（旧 `0x8345f1`） | `0x008345F1` | `0x00834651` | +0x60 | bytes |
| SSE 混合内核（旧 `0x836a3d`） | `0x00836A3D` | `0x00836A9D`（推断） | +0x60 | 推断：16B 模板不唯一，未逐字节确证 |
| `blit @0x3f5960` | `0x003F5960` | `0x003F5960` | 0 | same |
| `luaL_loadbuffer` | `0x008EA880` | `0x008EA880` | 0 | same |
| `script_file` | `0x008C6C80` | `0x008C6C80` | 0 | same |
| 动画加载函数（`tools/kimi_click_effect_hook.py` POINT） | `0x008C40B0` | `0x008C40B0` | 0 | same（48B 全同） |
| `WWindow` vtable（`??_7WWindow@@6B@`） | `0x00A42674` | `0x00A42694` | +0x20 | export；表项已随函数平移 |
| `WWindow::messageEntries` | `0x00AE780C` | `0x00AE780C` | 0 | export；位于 .data 未初始化区，静态文件读出为 0，运行时才填充 |
| `[0xAE7E08]`（交接文档提到的 DirectDraw 槽） | `0x00AE7E08` | `0x00AE7E08` | 0 | 同址；文件内为 BSS，运行时初始化，静态不可读值 |

常用导出地址（VA，按名称解析更稳妥）：

| 导出 | 1.50.27 | 1.50.29 | Δ |
|---|---|---|---|
| `?WndProc@WWindow@@SGJPAUHWND__@@IIJ@Z` | `0x7ED580` | `0x7ED5E0` | +0x60 |
| `?Blit2Screen@WSurface@@QAEKHH@Z` | `0x7F8A60` | `0x7F8AC0` | +0x60 |
| `?ViewPosDraw@WCanvas@@UAEXHHABVWObject@@K@Z` | `0x7F8600` | `0x7F8660` | +0x60 |
| `?Draw@WCanvas@@UAEKABVWObject@@K@Z` | `0x7FC0D0` | `0x7FC130` | +0x60 |
| `?DrawShape@WAlphaBitmap@@QAEKPAUXYUVVertex@1@IMMAAV1@@Z` | `0x7F52C0` | `0x7F5320` | +0x60 |
| `?Create@WAlphaBitmap@@UAEKHHPAX@Z` | `0x7F3600` | `0x7F3660` | +0x60 |
| `??0WAlphaBitmap@@QAE@HH@Z` | `0x40DBE0` | `0x40DC10` | +0x30 |
| `?Combine@WAlphaBitmap@@QAEKABV1@@Z` | `0x7F5D20` | `0x7F5D80` | +0x60 |
| `?Open@WFile@@QAEKPBD@Z` | `0x811930` | `0x8119C0` | +0x90 |

### 静态点击链复核

- `WndProc 0x7ED5E0` 通过 `GetWindowLong(-0x15)` 取窗口对象（`edi`），
  先调对象 vtable[0]（`call [edx]`，`0x7ED635`），再遍历 20 字节/条的消息表
  （`msg`、`handler`、`thisOff`），在 `0x7ED6B4` 执行 `call edx`。
- 静态 `WWindow` vtable[0] 是 `0x4013D0`（`xor eax,eax; ret 0x10` 桩）；
  交接文档动态观测到的 `vtable[0]=0x4168D0` 应来自运行时派生窗口对象，二者不矛盾。
- 本版 `WM_LBUTTONDOWN` 处理入口为 `0x416B75`，`WM_LBUTTONUP` 为 `0x416BC9`；
  其内部调用目标分别按 +0x30/+0x60 平移（如 `call 0x41c080→0x41c0b0`、
  `call 0x7ee370→0x7ee3d0`）。
- `Blit2Screen 0x7F8AC0` 的图层循环：`ebx` 为层/表面对象，`edx=[ebx]` 为 vtable，
  `0x7F8B99` 调 `[edx+0x64]`，并压入矩形 `0xAE7E10`。这是下一步动态抓光圈的重点
  调用点；交接文档写的 `[0xae7e08]` 是同一数据区附近的运行时槽，静态文件为 0。

## 五、资源差异（1.50.22 → 1.50.29）

工具：`tools/diff_pak.py`；产物：
`manifests/batches/pak-diff-1.50.22-to-1.50.29.json`、
`pak-diff-magic5-1.50.22-to-1.50.29.json`。

### ui.pak（509,587,338 → 509,605,655 字节）

- 叶子 33,502 → 33,505：新增 3 个，无删除：
  - `icon/big/75161.a16`（14,564）
  - `icon/small/75161.a16`（2,412）
  - `user/icon/137.apg`（431）
- 大小变化 1 个：`item/itemSpecData.csv` 1,145,484 → 1,146,250（+766）。
- **`cursor/*` 144 条完全不变**（路径、大小全同）。
  → 点击光圈不是 cursor 动画/资源；1.50.29 没有新增光标/光圈资源。

### magic5.pak（173,987,977 → 174,129,558 字节）

- 叶子 306 → 308，新增 2 个 APG，无大小变化：
  - `1440/0/back.apg`：10 帧，白色扩散特效，首帧 8×16，最大 40×32；
  - `4187/0/front.apg`：8 帧，136×104 橙色特效。
- 两者均**不是**约 30×16 的蓝白椭圆光圈，排除。

### config.pak / 800config.pak

- config.pak：1330 增 / 1330 删，实为嵌套目录名大小写 `ui` → `UI`（路径与大小一一对应）；
  另 `GraphTitleData.csv` 18,640→18,698、`magic/Skill.csv` 124,966→124,964。
- 800config.pak：仅 `ui/ib/ib_main3.scp` 7,820→7,806。

## 六、工具与产物

新增/使用：

- `tools/pe_facts.py`：PE32 dump / 两版对比（节表、导入、导出、字节差异、锚点掩码/精确搜索）。
- `tools/diff_pak.py`：两个安装目录的 PAK 叶子清单差异（新增/删除/大小变化/cursor 专列）。
- `manifests/batches/gameexe-1.50.29.txt` / `.json`
- `manifests/batches/gameexe-1.50.27-to-1.50.29.json`
- `manifests/batches/pak-diff-1.50.22-to-1.50.29.json`
- `manifests/batches/pak-diff-magic5-1.50.22-to-1.50.29.json`
- `.rea/anchors_1_50_27.json`：本次锚点清单（含旧字节模板）。

## 七、对点击环任务的影响与下一步

1. **旧地址全部失效**：动态脚本若仍用 `0x7f8600`、`0x7fc161`、`0x7f52c0`、`0x4168d0`
   等 1.50.27 地址，必须换成本文件第四节的新地址；优先用
   `Process.findModuleByName('Game.exe').enumerateExports()` 按导出名解析
   （Frida 17 已移除 `Module.enumerateExports`，且 `Process.mainModule` 可能误指
   `dwmapi.dll`，不要用）。
2. **下一步动态抓取的重点**：`WSurface::Blit2Screen 0x7F8AC0` 的层循环，
   尤其 `0x7F8B99` 的 `call [edx+0x64]` 与图层对象 `ebx`；按目标表面/坐标过滤，
   确认点击环是否作为 DirectDraw 层/表面绘制。
3. 或沿 `0x416BC9`（左键抬起）→ 输入管理器继续静态跟踪创建代码；
   `0x416900` 消息分发可作为低频条件断点入口（仍须遵守“单次、清断点、立即 detach”）。
4. 光标任务不依赖本文件：`cursor/*` 资源 144 条未变，可直接继续接入客户端；
   点击光效的原始资源仍未锁定。
5. Ghidra：`ghidra-proj/dhsh-7a5e` 仍是 1.50.22 旧基线；本版尚未建 Ghidra 项目。
   深静态分析前需为 `1c0704f9…` 新导入项目，不要把 1.50.22 地址当本版地址。

## 八、剩余未知

- 点击环的原资源/生成代码仍未定位；本版 `ui.pak` 无新增候选，`magic5.pak` 新增两例
  已排除。
- `[0xAE7E08]` 运行时值未读取；其数据类型与初始化来源未验证。
- SSE 混合内核 `0x836A9D` 是按 +0x60 推断，尚未用唯一模板逐字节确证。
- `WWindow::messageEntries` 运行时内容未抓取；静态文件为 BSS，需运行期读。
