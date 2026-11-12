# 交接：原版鼠标样式 + 点击光效（2026-11-11）

## 一、任务目标

给原生离线客户端（`client/`）补两项原版行为（参照截图 `pic/cursor.png`）：

1. **鼠标样式**：使用原版 `ui.pak` 的 `cursor/00000` 动画（24×24、6 帧、
   PIF 延时约 120ms/帧），而不是 Windows 默认箭头。原版共 19 套上下文光标
   （`cursor/00000..00018`），本任务先实现默认样式。
2. **点击光效**：左键点击**可走地面**时，在点击位置播放蓝色椭圆光环；
   - 绘制在**最高层**（树叶、建筑、甚至 UI 之上）；
   - 快速连续点击时，每个点击位置各出现一个独立实例（可并存）；
   - 只在可走区域出现（应与寻路/NavGrid 判定一致），点到 UI/不可走处不出。
3. **约束**：
   - 客户端继续使用旧 PAK 资源；新版仅作视觉参照；
   - 运行方式：`client\build\Release\dhsh_client.exe --shdata D:\software\dhsh\shdata`，
     编译 `client\build_release.cmd`；用户手动验证。

## 二、已完成的成果

### 1. 鼠标样式资源（已闭合）

- `ui.pak` 中 `cursor/00000..00018` 共 144 条（19 套），每套 =
  编号 `.a16` 帧 + `perinfo.dat`。
- `cursor/00000` 与截图左侧完全一致：24×24、6 帧，PIF 头后每帧 28 字节元数据，
  延时字段 0x78 = 120ms。
- 19 套已全部解码导出：`extracted/_probe_cursor/png/style_*.png`
  （另有大图 `style_00000_big.png`）。
- 新旧客户端 `ui.pak` 的 cursor 资源**完全一致**（均 144 条）；
  新版 ui.pak 仅多 `user/icon/137.apg`，与本任务无关。

### 2. 引擎侧静态证据（Game.exe 1.50.22，pinned 7a5e）

- Ghidra 项目：`ghidra-proj/dhsh-7a5e`；分析产物在 `.rea/cursor_*.json`、
  `.rea/cursor_mgr*.json`、`.rea/cursor_update.json`。
- `FSCursorManager` 单例：`FUN_004b3e30`；构造 `FUN_004b3dd0` 调
  `FUN_004b3d80("ui\\cursor")`。
- 资源加载 `FUN_004b35f0`：
  - `"%s\\%05d\\%s"` → `ui\cursor\<id>\perinfo.dat`
  - `"%s\\%05d\\%05d.%s"` → `ui\cursor\<id>\<frame>.a16`
  - 预加载 0..0x12（17 号复用 12 号资源）。
- 光标由引擎**在画面画布内自绘**（PrintWindow 截屏中能看到原版星形光标，
  而 PrintWindow 不包含系统光标），不是单纯 SetCursor。
  `FSCursorManager` 的消费位于 `FUN_00415bc0` 的绘制路径。
- 因此我们客户端也应把光标画进帧缓冲（软件渲染器 + D3D11 presenter），
  这样帧率和动画与游戏一致。

### 3. 点击光效的动态观测（已抓到实际渲染）

- 工具：`.rea/capture_printwindow.py`（PrintWindow + PW_RENDERFULLCONTENT，
  窗口被其他程序遮挡也能抓到）、`.rea/capture_click_effect.py`
  （约 30fps 连拍，检测左键按下，保存点击前后 ±110px 区域帧）。
- 产物：`.rea/effect_capture/`（`clicks.json` + `click_00..07/` 帧，
  以及 `montage_click*.png`、`ring_zoom_*.png`）。
- 实测外观与行为：
  - 蓝色椭圆光环 + 白色描边，约 30×16 px，位于点击点/光标下方；
  - **最高层**绘制（树叶、建筑、UI 之上）；
  - 生命周期约 0.5–1 秒，有扩散/淡出；
  - 快速点击会在每个位置各留一个实例（可叠加）；
  - 亮帧示例：`click_05` 的 f003–f005（之后镜头随角色移动）；
  - 该效果只在可走地面点击时出现（用户确认）。

### 4. 相关既有提交

- 上一任务（遮挡 alpha）已提交：`4b1aa3c fix(client): fade covered sprite
  alpha to match core occlusion`；本任务未改动客户端代码。

## 三、未完成的成果

### 1. 点击光效的原始资源/资源 ID 尚未锁定

已排除的方向（均无命中）：

- 19 套 `cursor/*` 动画；`ui.pak` 的 40 个 perinfo A16 动画目录；
- `Game.exe` 资源路径字面量（cursor/click/point/ring/guang/highlight 等）；
- `config.pak` / `plugins.pak` 的 SCP/CSV/Lua 文本引用；
- 新旧客户端 PAK 清单差异（仅 `user/icon/137.apg`）；
- `ui.pak` 内 A16/APG/TGA 的椭圆形状自动扫描
  （蓝/白候选共数千张，均未命中；脚本 `.rea/scan_blue_sprites.py`、
  `.rea/scan_apg_blue.py`、`.rea/scan_tga_blue.py`、
  `.rea/scan_ellipse_ui.py`）；
- 运行中 1.50.27 客户端内存路径字符串扫描（788MB，只读，
  `tools/scan_runtime_mem.py`）未出现明显特效路径。

仍在追的线索：

- 覆盖层管理器：app+0xa20（全屏覆盖图）、app+0xa2c 起 16 个 0x24 字节槽位，
  `FUN_0040f150`（创建/更新覆盖动画）、`FUN_0040fdb0`（全屏覆盖 bitmap，
  地图加载里用于 ShuiLingLong 气泡/地图遮罩）。尚未找到“点击→覆盖层”的调用链。
- 其它可能：数字 ID/模板拼路径、`map.pak animate/*` 或 `magic.pak` 的特效、
  或引擎用 `magic.pak light.tga` / 阴影等现有图动态合成。
- 也可能使用**程序化绘制**（当前 exe 无 GDI Ellipse/Arc 导入，但不是定论）。

### 2. 客户端功能尚未实现

- 光标：未接入 `ui.pak cursor/00000` 帧动画（A16 + PIF 延时）到客户端渲染。
- 点击光效：未实现。
- 输入链路尚未改动：实现位置预计在
  `client/src/input/MapInput.cpp`（点击候选/寻路）、
  `client/src/navigation/NavGrid.cpp`（walkable 判定）、
  `client/src/app/App.cpp`（帧循环/相机/UI 门控）、
  `client/src/render/SoftwareRenderer.*`（顶层叠加绘制）。

### 3. 建议的后续路径（按优先级）

1. **连拍重建 + 模板匹配**：用 `.rea/effect_capture/` 的帧差/双底色解出光效
   的 RGB + alpha，再对所有 PAK 的候选精灵做模板匹配，锁定原文件；
2. **先实现**：以实测外观（蓝椭圆 + 白边、约 30×16、0.5–1s 扩散淡出、
   顶层、可叠加、仅 walkable）实现，后续找到原文件再替换；
3. **动态定位**：在运行中的 1.50.27 客户端里定位 app 指针/覆盖层槽位，
   点击时直接读槽位中的动画路径（需先解决新版地址映射）；
4. 光标可独立先做：`ui.pak` 已确定，不依赖光效资源。

## 四、关键文件与产物索引

| 类别 | 路径 |
|---|---|
| 光标导出 | `extracted/_probe_cursor/png/style_*.png` |
| 点击连拍 | `.rea/effect_capture/clicks.json`、`click_00..07/`、`montage_*`、`ring_zoom_*` |
| 连拍工具 | `.rea/capture_click_effect.py`、`.rea/capture_printwindow.py`、`.rea/capture_probe.py` |
| 资源扫描脚本 | `.rea/scan_blue_sprites.py`、`.rea/scan_apg_blue.py`、`.rea/scan_tga_blue.py`、`.rea/scan_ellipse_ui.py`、`.rea/scan_mem_paths.py` |
| 静态分析产物 | `.rea/cursor_*.json/.c`、`.rea/cursor_mgr*.json`、`.rea/cursor_update.json`、`.rea/exe_strings_effect.txt` |
| Ghidra | `ghidra-proj/dhsh-7a5e`（程序 `Game-7a5e…exe`） |
| 客户端接入点 | `client/src/input/MapInput.cpp`、`client/src/navigation/NavGrid.cpp`、`client/src/app/App.cpp`、`client/src/render/SoftwareRenderer.*`、`client/src/formats/A16.cpp`、`client/src/formats/Pif.cpp`、`client/src/resource/PakResourceProvider.*` |
| 运行中的参照客户端 | PID 17980，`D:\software\dhsh2\shdata\Game.exe`（1.50.27，只读） |

## 五、待确认问题

1. 点击光效是“世界坐标锚定在点击格”还是“屏幕坐标跟随鼠标”？
   用户观察：快速点击每个位置各一个，光圈固定在地面点击点 → 更像世界/格锚定，
   但需要连拍进一步确认镜头移动时的表现。
2. 光效是否需要与角色移动目标绑定（点一次播一次，还是移动中重复）？
3. 光标热点（热点像素）与是否屏蔽系统光标（原版为画布自绘）。
4. 19 套上下文光标（攻击/拾取/看/禁止等）是否也要在本任务实现，
   还是只做默认样式。

## 六、本轮动态复核（新版 1.50.27，尚未闭合）

### 已证实

- `tools/kimi_click_effect_draw_hook.py` 使用 Frida 17，在 `ViewPosDraw 0x7f8600`
  与 `WCanvas::Draw` 矩形读取点 `0x7fc161` 做点击后的短时采样。新版宽、高分别位于
  `EBP-0x24`、`EBP-0x20`；最初使用的 `EBP-0x28` 是错误偏移。
- 热路径不能持续挂钩：实测约 15 万次调用/秒。当前脚本按左键触发，每个片段最多
  3000 次调用便卸载，并在期限到达时主动卸载两个挂钩。
- 坐标日志 `.rea/click_effect_draw_positions.jsonl` 表明：点击点附近只有 24×24
  的星形光标对象；没有看到独立的约 30×16 光圈 WObject。
- 曾抓到固定 32×16 `WAlphaBitmap`（当时对象 `0x14d84be8`、像素主色 `0x2a91`），
  但旧版调用链 `0x4869c0 -> 0x4867a0` 明确来自接收字符串的 `0x4876b0`，
  是文字位图生成器；坐标也固定为 `(20,584)`。**它是已排除的文字假候选，不是光圈。**
- `WAlphaBitmap::DrawShape` 旧版实际入口 `0x7f32a0`、新版 `0x7f52c0`，是
  XYUV 顶点/纹理多边形填充。`.rea/hook_draw_shape.py` 在 28 次点击中零调用，
  排除该入口作为本轮光圈生成路径。
- 主画布曾动态读取为 `canvas=0x14661c60`、位图数据 `0x1822d280`、
  896×640、pitch=1792、像素 `0x58a7020`；导出 `.rea/canvas_framebuffer.png`。
  这些堆地址只适用于当次进程，重启后必须重新获取。
- 对点击区域做内存写监听（`.rea/monitor_click_pixels.py`），命中
  `0x8345f1`（MMX 块拷贝）和 `0x836a3d`（SSE 透明混合/拷贝）。二者均为通用
  像素例程，不是椭圆数学；旧版透明混合内核为 `0x83468a`，由 `0x7f3940` 包装。

### 当前结论与下一步

- 尚未找到实际光圈的资源或生成代码；不能宣称它是程序生成，也不能使用上述
  文字位图代替。普通 `ViewPosDraw` 对象路径和 `DrawShape` 路径均未命中。
- 下一次有效点击使用 `.rea/hook_click_blend.py`，在透明混合写指令处记录点击附近的
  源像素、alpha、目的坐标和调用者。最近一次 30 秒窗口未检测到点击，因此该脚本
  尚无有效业务数据。
- 所有本轮挂钩均已卸载；曾发生强制结束采集端导致 `0x7fc161` 残留 5 字节跳转，
  已恢复原码并复核 16 字节签名。后续不要直接杀仍挂热路径的采集进程，应先让脚本
  到期自动卸载。

## 七、`help/` 目录验证结果（2026-11-12，新版 1.50.27）

结论：**PE 清单有用，Frida Lua/网络观察器对“点击光效定位”没有直接帮助。**

### 已证实

- `help/pe.txt` 首行 SHA256 `0dbc18…b519f` 与运行中的
  `D:\software\dhsh2\shdata\Game.exe`（1.50.27）完全一致，导出/导入/RVA 表对该版本有效。
  本轮用其导出表成功挂钩 `WFile::Open`、`WAlphaBitmap::ctor/Create` 等 7 个地址，
  均正常安装、正常卸载。
- `help/game_observe.js` 的 `luaL_loadbuffer @0x4ea880` 16 字节签名匹配新版，可挂钩。
- `help/game_observe.js` 的 `script_file @0x4c6c80` 期望签名有笔误：
  `...33c05c20c00` 只有 31 个十六进制字符（少一个 `0`），正确为
  `558be98b450485c0750633c05dc20c00`；原 `checkedHook` 会永远判定不匹配并跳过。
- Frida 17.24.3 已移除 `Module.enumerateExports`，需改用模块实例
  `Process.findModuleByName('Game.exe').enumerateExports()`；该版本
  `Process.mainModule` 还会误指向 `dwmapi.dll`，不要用它取路径。

### 动态观察（8s 自检 + 90s 点击窗口，PID 18820）

- 窗口内记录 32 次左键点击，其中 19 次位于游戏窗口内（含在可走地面连续快速点击）。
- `lua_chunk = 0`、`script_file = 0`、`WFile::Open = 0`：点击期间没有 Lua 块、
  脚本文件或 `WFile` 资源打开事件。
- `WAlphaBitmap::Create/ctor` 共 651 条（4×4 至 200×144），但调用链集中在
  `0x404b57 -> 0x577bbe -> 0x577cc6`，且两次点击之间 10 秒空档仍有 18 条、
  距任意点击>6 秒的有 282 条，说明它们是常驻 UI/动画位图创建，不是点击光效。
  未出现约 30×16 的独立“光圈”创建。
- 因此光圈更可能是**预先创建/已有位图或对象，在点击时复用、缩放、旋转或淡出绘制**；
  不是点击时从 PAK/脚本/Lua 现场加载。

### 本轮新增产物

- `help/`：待验证素材（`game_observe.js`、`observe_game.py`、`lua_strings.txt`、`pe.txt`）。
- `.rea/help_verify.js`、`.rea/help_verify_observe.py`：修正后的只读观察脚本。
- `.rea/help_verify.jsonl`：本轮 90 秒观察日志（点击、位图创建、挂钩状态）。

### 崩溃说明

- 本轮验证开始前游戏曾自行崩溃；`dhsh2` 安装目录未找到当日 `.dmp`。
- 崩溃前本轮只做了文件读取与 SHA256 计算，未 attach；之后两次只读 attach
  （8s、90s）均正常卸载，未再崩溃；磁盘 `Game.exe` 哈希与 `help/pe.txt` 一致。
- 可确认的事实仅止于此，不能据此断言当时崩溃的根因。

## 八、2026-11-12 点击链与渲染路径复核（新版 1.50.27）

### 已排除

- **48×32 星芒不是点击环**。在用户确认蓝色光圈出现后，完整抓取点击后
  1.6 秒内全部 `0x3f5960` blit（178/157 帧）——没有任何 48×32 光圈帧；
  48×32 星芒来自 `WAlphaBitmap::Combine` 角色/装备合成链。
- 点击环不走 6 个形状/椭圆绘制函数（新 RVA `0x422f00/0x423cf0/0x423e10/
  0x423b60/0x424450/0x4244b0`）：有效点击期间零调用。
- 游戏不轮询 `GetAsyncKeyState(1)`（4 秒零调用），点击是窗口消息驱动。

### 已定位的点击消息链（新版绝对地址）

- `WWindow::WndProc @0x7ed580`；消息表在运行时为 20 字节/条
  （`+0 msg`、`+4 handler`、`+8 thisOff`），并沿表首 dword 链继续。
- 窗口对象 vtable[0] 前置分发 `0x4168d0`；
  `WM_LBUTTONDOWN(0x201)` 跳转目标 = **`0x416b45`**（校验
  `GetCapture`/`GetFocus`，随后调 `0x41c080` 单例和 `0x41b830` 计时器方法）；
  `WM_LBUTTONUP(0x202)` = **`0x416b99`**（更可能承载真正点击动作）。
- 消息主表无 0x201，左键按下由 vtable[0] 处理，而非消息表。

### 渲染路径新线索

- `WSurface::Blit2Screen @0x7f8a60` 使用：
  - `[surface+4]` 的窗口/位图描述（宽高字段读出的 0x553/0x356 恰为窗口
    right/bottom）；
  - `[surface+8]` 对象的**图层数组**：`+0x34` 数组、`+0x38` 层数，每层
    0x2c 字节，逐层经 DirectDraw 风格 `[0xae7e08]` 调用 Blt。
- 点击环很可能作为 DirectDraw 图层/表面直接绘制，绕过软件 blit 挂钩；
  这与“位图和形状函数都抓不到环”一致。
- 光标系统仍是强候选：用户原始 `cursor.png` 就是“鼠标 + 点击光效”，
  且光圈只在可走光标上下文出现；19 套 cursor 资源已导出但尚未按帧切片
  比对点击环。

### 建议的下一步（按优先级）

1. 在有效点击的 1.6 秒窗内挂钩 `Blit2Screen` 的层 blit `[0xae7e08]`
   和/或 DirectDraw `Blt`/`Lock`，按目标表面/坐标过滤，抓环所在表面与调用者。
2. 或沿 `0x416b99`（左键抬起）→ 输入管理器虚拟调用继续静态追创建代码。
3. 光标系统：按 PIF 帧切片 19 套 cursor，匹配蓝白环/星芒帧，并检查
   FSCursorManager 的点击动画创建逻辑。

## 九、1.50.29 基座重建（2026-11-12，已完成）

- 运行实例已更新为 **1.50.29**（SHA `1c0704f9…d96619`，7,123,744 B；
  2026-11-12 10:39 更新，PID 28816）。`help/pe.txt`（`0dbc18…`，1.50.27）
  及 §6–8 的**所有地址对本版失效**；两版虽同大小，但约 83% 字节不同（整体重编译）。
- 已重建 PE 事实、导出表与锚点映射：详见
  `docs/VERIFY-2026-11-12-gameexe-1.50.29.md`。关键新地址：
  - `WWindow::WndProc 0x7ED5E0`；`WSurface::Blit2Screen 0x7F8AC0`
    （图层循环 blit 调用点 `0x7F8B99`，`call dword ptr [edx+0x64]`，rect `0xAE7E10`）；
  - `WCanvas::ViewPosDraw 0x7F8660`；`WCanvas::Draw` 矩形读取 `0x7FC1C1`；
    `WAlphaBitmap::DrawShape 0x7F5320`；
  - 消息分发 `0x416900`；`WM_LBUTTONDOWN → 0x416B75`；`WM_LBUTTONUP → 0x416BC9`；
  - 未变：`luaL_loadbuffer 0x8EA880`、`script_file 0x8C6C80`、动画加载 `0x8C40B0`、
    `blit 0x3F5960`、`[0xAE7E08]` 槽地址（文件内为 BSS，运行时初始化）。
- 资源差异：`ui.pak` 的 `cursor/*` 144 条**完全不变**；仅新增 3 个图标
  （`icon/*/75161.a16`、`user/icon/137.apg`），`item/itemSpecData.csv` +766；
  `magic5.pak` 新增 2 个特效 APG（`1440/0/back` 白色 8→40px、
  `4187/0/front` 136×104 橙色），均已排除为点击环。
- 下一步：用上述新地址做 Blit2Screen 层 blit 的动态观察；旧 Frida/x64dbg
  脚本不得直接复用 1.50.27 地址，优先按导出名解析。

## 十、2026-11-12 1.50.29 点击光圈动态定位（有突破，仍需收尾）

### 已证实

- `WSurface::Blit2Screen` 不是光圈的直接绘制点：它先做一次 DDRAW 表面 blt，
  再遍历 `[surface+8]+0x34` 的 20 项数组、经 `[0xAE7E08]`（运行时
  `Game.exe:0x89D556`）逐项软件 blit。该数组是瓦片网格，不是独立 UI 层。
- 光圈走**软件 alpha 混合**：绘制函数入口 `Game.exe:0x83699A`（新地址；
  1.50.27 为 `0x83693A`），像素热循环在 `0x836A9D`。入口参数：
  - `[ebp+8]=dest`、`[ebp+0xc]=src(RGB565)`、`[ebp+0x10]=alpha`、
    `[ebp+0x14]=width`、`[ebp+0x18]=height`；
  - `[ebp+0x1c]=dest 行距`（观测 0x700）、`[ebp+0x20]=src 行距`
    （观测 0x120=144px）、`[ebp+0x24]=alpha 行距`（观测 0x90=144px）。
- 光圈按 **64px 宽条带**绘制；一次点击可出现 5 条/行，总宽约 320px。
  源色只有两种：白 `0xFFFF`（外圈）和蓝 `0x3BD5`（填充，RGB888≈57,121,172）；
  形状由 alpha 遮罩决定。条带高度随时间变化（外圈 24→1），持续时间约 1.2s。
- 条带的 `src`/`alpha` 指针在进程堆上（本轮 `0x11D35024..` / `0x113AEC76..`），
  随点击世界坐标移动；同一屏幕位置会重复绘制（多实例、镜头移动时跟随）。
- 30 秒窗口（`--dump-ring`）在点击后 43ms 命中首个条带，已 dump：
  - `.rea/ring_src.bin`（0x6000 B，源像素）
  - `.rea/ring_alpha.bin`（0x6000 B，alpha 遮罩）
  - `.rea/ring_tile_src.png`、`ring_tile_alpha.png`、`ring_tile_comp.png`
    （按 64×24、源行距 0x120、alpha 行距 0x90 渲染）
- 首个条带是光圈**上弧**：源为蓝色矩形+白顶边，alpha 大部分透明，
  说明不是一次性完整椭圆贴图，而是按条带裁剪绘制。

### 排除/安全事项

- 光圈**不是** DDRAW 独立层、不是 `WAlphaBitmap::DrawShape`、不是 cursor 动画；
  1.50.29 的 `ui.pak` 仅新增 3 个图标，`cursor/*` 144 条不变；`magic5.pak`
  新增两例特效 APG 已排除。
- **不要**在 `0x836A9D` 像素热循环上持续挂钩：本轮 1.6s 像素级捕获期间
  出现过角色暂时变黑，脚本结束后自行恢复。后续统一使用入口 `0x83699A`
  （每张位图一次，频率低），并在 JS 内自卸挂钩。

### 产物

- `.rea/hook_drawalpha_entry_click.py`（入口采集，`--dump-ring` 实时 dump）
- `.rea/hook_click_blend_1_50_29.py`（像素级捕获，仅作证据，勿再长挂）
- `.rea/drawalpha_ringdump.jsonl`、`.rea/drawalpha_click.jsonl`
- `.rea/ring_src.bin`、`.rea/ring_alpha.bin`、`.rea/ring_*.png`
- `.rea/click_blend_1_50_29.jsonl`（含两次点击的混合写行）

### 未闭合

- 光圈源位图/alpha 遮罩的**最初生成者**仍未锁定：可能是运行时按椭圆公式
  生成，或由 PAK 资源解压到堆后缓存。当前只证明最终绘制走 `0x83699A`。
- 绘制回调已定位到 `Game.exe:0x7F59C0`（`WCanvas::Update 0x7FB5D0` 经
  图层节点 `[entry+0x24]` 间接调用）。该回调对象布局：
  `+0x0c=宽、+0x10=高、+0x14=源行距、+0x18=源像素、+0x24=alpha`；
  光圈条带为 64px 宽、源行距 0x120、alpha 行距 0x90。
- 但 `0x7F59C0` 入口挂钩在有效点击窗口内未命中 `src=0x11D3xx/alpha=0x113Axx`
  的对象；基线窗口反而捕获到 14 条同源条带（可能是上一次光圈残留），
  说明光圈节点可能是 `WCanvas::Update` 过程中临时加入的，或经由不同的
  回调入口/画布，不能仅靠入口快照归属。
- 需要进一步确认：谁填充 `0x11D3....` 源缓冲与 `0x113A....` alpha 缓冲；
  可在入口钩子中对这两个缓冲设一次性读/写观察（低频、短窗口），
  或对 `0x83699A` 的上层 caller `0x7F5B15` 所在函数做静态回溯。
- 客户端尚未实现；现在已可按实测参数（320px 宽白边蓝椭圆、~1.2s、
  世界锚定、多实例、顶层、仅 walkable）先实现，再决定是否等原资源。

### 2026-11-12 追加：回调与对象布局证据

- `WCanvas::Update 0x7FB5D0` 遍历 `[canvas+0x34]` 的 0x2c 字节图层项，
  经 `+0x24`/`+0x28` 的链表节点 `call [edi]`；节点 `+4` 为绘制对象，
  `+8` 为下一个节点。
- 回调 `0x7F59C0` 是通用“带裁剪的 alpha 位图绘制”：
  `esi = 对象`，对象 `+0xc=宽、+0x10=高、+0x14=源行距、+0x18=源像素、
  +0x24=alpha`；`edi = 绘制上下文`（`+0x14=目标行距`）。
  内部按 64px 条带调用 `0x83699A`（透明混合）/`0x8366A0`（不透明拷贝）。
- 光圈条带的 `src=0x11D350A4`、`alpha=0x113AECB6`，`w=64`、`h=24/14/10`
  与对象字段可对应；但当前进程里该对象/缓冲可能是复用的，需在点击瞬间
  抓对象归属。
- 新增脚本：`.rea/hook_ring_object.py`、`.rea/hook_ring_current_obj.py`、
  `.rea/hook_canvas_callbacks.py`；日志 `.rea/ring_object.jsonl`、
  `ring_current_obj.jsonl`、`canvas_callbacks.jsonl`。

## 十一、客户端实现（2026-11-12，已完成首版）

按截图参考先实现小号光圈；动态抓到的 ~320px 蓝色条带视为“大范围地面高亮”，
本轮不并入小光圈。

### 光标

- 从 `ui.pak` 加载 `cursor/00000/perinfo.dat`（PIF，6 行，延时 row[5]=0x78=120ms，
  热点 row[0..1]=(7,6)）与 `cursor/00000/00000..00005.a16` 共 6 帧。
- 每帧按 120ms 轮换；在软件渲染器最后绘制（高于 SCP UI）。
- `Win32Window` 处理 `WM_SETCURSOR`，客户区内隐藏系统箭头；非客户区保留系统箭头。
- 启动输出：`Cursor frames=6 hotspot=7,6`。

### 点击光效

- 左键点击可走地面时，按 `NavGrid::walkable(cell)` 生成一个实例；UI 点击、
  拖拽、不可走格不生成。
- 实例保存地图坐标，绘制时按 `(map - camera) * zoom` 投影，支持多实例、跟随镜头。
- 外观：蓝色 `(57,121,172)` 椭圆环 + 白色外圈；半径从 `8x4` 扩张到 `32x16`
  （逻辑 800×600，缩放随 zoom），约 1s 内淡出。
- 绘制在 SCP 窗口之后（最高层），光标之前。

### 改动文件

- `client/src/app/App.cpp`（光标加载/动画、点击实例、渲染顺序）
- `client/src/render/SoftwareRenderer.h/.cpp`（`draw_ellipse_ring`）
- `client/src/platform/Win32Window.h/.cpp`（`set_cursor_hidden` + WM_SETCURSOR）

### 验证

- `cmake --build client/build --config Release --target dhsh_client` 通过。
- 实机启动：`Cursor frames=6 hotspot=7,6`；resources/scene/avatar/npc/scp_ui/
  window_presenter 全部 PASS；帧统计 render_avg≈1.7ms PASS；90 秒运行 +
  多次导航点击无崩溃。
- 剩余需目视调参：光标热点、光圈大小/下移偏移、是否追加 320px 大范围高亮。
