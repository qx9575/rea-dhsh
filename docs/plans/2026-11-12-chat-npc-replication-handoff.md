# 交接：聊天窗复刻 + NPC 位置/形象修正（2026-11-12）

## 任务

重启会话后处理两件事：

1. **聊天窗样式完全不对**：对照 `.rea/live_chat.png`，按原版 `FSChatMain`
   + `chatmain.scp` + 聊天皮肤资源重新复刻；不要手工拼近似 UI。
2. **NPC 显示/位置不对**：
   - 不该出现的人物已确认是**最早复刻时加入的测试 NPC**：
     `10009/10/3.apg`，实体位置 `(7000,2152)`，生成函数
     `client/src/scene/LocalEntityFactory.cpp:make_local_npc()`。
   - 用户误认成皇甫端；实际是测试 NPC 的形象。
   - NPC 位置整体需要**统一向上、向左**移动；需要逆向 NPC 绘制锚点，
     不是逐条改坐标。

用户要求：直接复用现有 SCP/Lua/资源，只逆向缺失细节；先方案、确认后再动手。

## 环境与安全

- 工作目录：`D:\ai-work-flow\rea-dhsh`，分支 `main`。
- 旧版资源只读：`D:\software\dhsh`；禁止修改/删除/重命名。
- 运行中的参照客户端：`D:\software\dhsh2\shdata\Game.exe`，
  PID 28816，版本 1.50.29，SHA
  `1c0704f9e31c21205accdf41e0c269340795022c3b7770c1bc8dcbb80dd96619`。
  **只读分析，不启动/关闭/修改其进程。**
- 不运行旧游戏或保护组件；不提交/推送，除非用户明确要求。
- 当前改动均**未提交**（最后提交仍是 `ce064ab`）。

## 当前未提交改动

```
M client/README.md
M client/src/CMakeLists.txt
M client/src/app/App.cpp
M client/src/scene/LocalEntityFactory.cpp/.h
M client/src/scene/MapScene.cpp/.h
?? client/src/platform/ScpChatWindow.cpp/.h
?? client/src/scene/NpcRoster.cpp/.h
?? docs/VERIFY-2026-11-12-npc-hud-runtime.md
?? tools/capture_npc_objects.py
?? manifests/batches/npc-objects.json
?? manifests/batches/npc-objects-now.json
```

## 一、聊天窗：已确认事实

- EXE 字符串：
  - SCP：`config/ui/main/chatmain.scp` @ `0x997C40`
  - 皮肤：`ui/chat/chatskin/skin%d.tga` @ `0x97F7B4`
  - 控件：`chatmain_txtlst_msg` @ `0x997CDC`、`chatmain_input_mes` @
    `0x997C74`、`chatmain_barwnd` @ `0x997C88`、`chatmain_allbtn` @
    `0x997CCC`、`chatmain_switchbtn` @ `0x997CB8`
  - 类 RTTI：`.?AVFSChatMain@@` @ `0xAae7E0`
- 皮肤加载点：`FSChatMain` 构造内 `0x4017D0`，循环拼
  `ui/chat/chatskin/skin%d.tga`（当前循环只试 `skin0`，是否另有 skin2
  由配置/代码决定，待确认）。
- 原版聊天窗口实测：
  - 顶层类 `fsgamehero0150`，HWND 独立窗口；
  - 客户区 **549×590**（窗口外框 565×629）；
  - 打开时游戏主窗口仍在旁边。
- SCP 版本未定：
  - `config.pak:ui/main/chatmain.scp`：根 `0,0,640,480`；
  - `800config.pak:ui/main/chatmain.scp`：根 `0,0,800,600`；
  - EXE 路径是 `config/ui/main/chatmain.scp`，但 800×600 模式下资源层可能
    优先解析 `800config.pak`。必须先确认实际加载哪个版本，再决定缩放/不缩放。
- 背景确认：`ui.pak:chat/chatskin/skin0.tga` 为 64×64 平铺纹理
  （深青色 + 小娃娃图案）；`skin2.tga` 为 640×480 整图。内存中同时出现
  `skin0.tga`/`skin2.tga` 字符串。
- 当前客户端实现（`client/src/platform/ScpChatWindow.*`）是近似：
  - 独立 Win32 窗口 549×590；
  - 加载 `config.pak + ui.pak`（避免 800config 覆盖）；
  - 用 config 640×480 `chatmain.scp` + skin0 平铺 + 缩放；
  - 消息列表 `chatmain_txtlst_msg` 是未支持的 `LISTVIEWEX`，内容区空白；
  - 底部输入条/输入框未实现。
  用户反馈样式仍完全不对，下一步应放弃近似缩放，按 `FSChatMain` C++ 逻辑
  和真实 SCP 布局重做。

### 聊天窗下一步

1. 静态确认 `FSChatMain` 构造/窗口创建：
   - 从 RTTI 和 `0x4017D0` 附近反查构造函数、vtable、窗口尺寸来源；
   - 确认实际 SCP 是 config 640×480 还是 800config 800×600；
   - 确认 549×590 的计算公式和是否缩放。
2. 反查 `chatmain_txtlst_msg` / `chatmain_input_mes` /
   `chatmain_barwnd` 的 C++ 绘制：
   - 消息区背景（skin0 平铺起点/对齐）、文字行颜色、滚动；
   - 底部输入条、输入框、底部按钮排。
3. 渲染直接用 SCP 控件坐标 + skin 资源，不再手工缩放/近似。
4. 验证：原版聊天窗与客户端聊天窗同尺寸截图对比。

## 二、NPC：已确认事实

### NPC 对象字段（运行时内存，只读）

- NPC 类 vtable `0x984554`（类大小 0x6A0，构造 `0x43DB50`）。
- 名字：`+0x04` 内联 GBK，NUL 结尾；shape：`u16 +0x28`；
  像素 X：`u16 +0x2A`；像素 Y：`u16 +0x2E`；方向：`u8 +0x32`。
- 动画路径：`npc.pak` 内为 `<shape>/10/<direction>.apg`（无 `npc/` 前缀）。

### 最近一次活跃对象（用户站在衙役/守门人之间时抓取）

| 名称 | shape | 像素 | 格子 | 方向 |
|---|---:|---|---|---:|
| 守门人 | 10051 | (7064,2152) | (441,134) | 1 |
| 应天府衙役 | 10040 | (6920,2088) | (432,130) | 1 |
| 庄园管理员 | 26152 | (6760,2088) | (422,130) | 1 |
| 房屋管理员 | 15026 | (6664,2040) | (416,127) | 1 |
| 金枼姿 | 10093 | (7304,2376) | (456,148) | 1 |
| 黄文炳 | 20113 | (7192,2248) | (449,140) | 1 |

- 世界 NPC 由服务器动态下发；`misc/npc.csv` 只是搜索/坐标表，不是当前
  活跃名单。客户端已临时用 `require_live_hint=true` 只生成这 6 个。
- 抓取工具：`tools/capture_npc_objects.py --pid 28816 --out ...`
  （只读扫描私有内存，不 attach）。
- 用户反馈：
  1. 不该出现的测试 NPC 是 `10009/10/3.apg` @ `(7000,2152)`，即
     `make_local_npc()` 创建的旧测试 NPC；确认它在最新构建中是否还被调用。
     当前 App 已不调用 `add_local_npc_if_available`，但需检查用户是否运行
     旧 EXE/是否有其它引用；正式处理时删除该测试 NPC 路径或仅保留测试。
  2. 当前显示的 NPC 位置整体都偏右下，需要**统一向上、向左**偏移。
  3. 庄园管理员看起来像皇甫端，可能是旧测试 NPC 或模型/名单绑定错误。

### NPC 渲染锚点线索

- `npc.pak:10051/10/1.apg` 第一帧 A16 头：`x=125, y=114, w=64, h=104`。
- 2026-11-12 复核当前代码：`ApgDecoder` 已把帧头 `x/y` 写入有符号
  `ApgFrame.anchor_x/anchor_y`，`AnimationClip` 已传递，`Entity::frame_origin()`
  当前按 `position + anchor` 计算。交接时“解析器未赋值/默认 0”已过时。
- 需要逆向 NPC 类 vtable `0x984554` 的绘制函数，确认：
  - `+0x2A/+0x2E` 是左上、脚底还是中心；
  - 原版是加、减还是另作变换使用 A16 帧头 `x/y`；
  - 是否存在其它统一固定偏移。不得再重复修改锚点解析器。
- 验证：玩家在 `(435,133)` 时，对照原版（`.rea/live_game_now.png`）和
  客户端同位置截图，按 NPC 包围盒算统一 delta，再在实体渲染统一应用。

### NPC 下一步

1. 移除旧测试 NPC `10009/10/3.apg`（确认运行时无调用；删除/隔离
   `make_local_npc` 的客户端路径，测试可保留）。
2. 逆向 NPC 绘制锚点，应用统一 up/left 偏移；不逐 NPC 改坐标。
3. 活跃名单精确匹配：按“运行时名称 + 像素坐标 + shape”绑定，避免
   庄园管理员/皇甫端错位；继续用抓取工具生成数据，不在代码里写死 132 条。
4. 验证 6 个活跃 NPC 的名称/模型/位置/方向与原版一致。

### 2026-11-12 补充发现与已确认路线

- `config.pak:npc.csv` 与 `misc/npc.csv` 均为搜索/坐标表，不含通用 shape；
  `monster/npchead.csv` 只有 8 行特殊头像映射。`misc/npcdlg.csv` 是对话按钮，
  `mapnpclink.csv` 是 NPC 地图跳转，都不能生成当前动态 NPC 名单。
- 现有 `tools/kimi_msg_*` 已能离线解密聊天和对象消息：聊天广播 `0x303f`、
  发送 `0x4050`；对象同步候选 `0x301e/0x301f/0x30a0/0x300d`。C++ 客户端尚无
  网络层。用户已确认“协议优先，先回放后实网”，不再扩大 NPC 硬编码提示表。
- config/800config 两份 `chatmain.scp` 都有 50 个控件节点，根矩形分别为
  640×480/800×600；实际加载和 549×590 尺寸计算仍以 `FSChatMain` 为准。
  原 SCP 已包含消息 LISTVIEWEX、输入 RICHEDIT、频道 COMBOBOX 和隐藏输入条。
- `fucnbtn_shortcut.scp` 声明串联活动、你画我猜、关系、阵营战排行、家族活动、
  跨服副本、活动链接、帮助 8 个入口；显示与排布还受 Lua/C++ 控制。用户确认
  点击后须打开真实面板并支持通用交互，无数据时保持真实空状态。
- 已确认设计：`docs/plans/2026-11-12-chat-npc-replication-design.md`。

## 三、构建/运行/验证

```bash
cmake --build client/build --config Release --target dhsh_client
client/build/Release/dhsh_client.exe --shdata D:/software/dhsh/shdata \
  --cache client/build/map-cache --log client/build/startup.log
```

- 最新启动日志：`NPC roster YingTianFu: configured=132 added=6 failed=0`，
  `NPCs=6`。
- 40 秒窗口烟测 PASS（约 120 帧/秒，render 2–4ms）。
- 截图：
  - `.rea/live_game_now.png`：原版主界面，玩家 应天府 435,133；
  - `.rea/live_chat.png`：原版聊天窗口；
  - `client/build/npc_panels4.png`：当前客户端 6 NPC 版本；
  - `.rea/chat_window.png`：当前客户端近似聊天窗口。
- 窗口/截图工具：`.rea/capture_hwnd.py`、`.rea/snap_client.py`、
  `.rea/chat_window_test.py`（均在 `.rea/`，不提交）。

## 四、关键文件

| 用途 | 路径 |
|---|---|
| NPC 解析/模型提示 | `client/src/scene/NpcRoster.*` |
| NPC 实体生成 | `client/src/scene/LocalEntityFactory.*` |
| 场景/NPC 工具 | `client/src/scene/MapScene.*` |
| 客户端主流程/HUD | `client/src/app/App.cpp` |
| 聊天独立窗口 | `client/src/platform/ScpChatWindow.*` |
| 运行时 NPC 抓取 | `tools/capture_npc_objects.py` |
| NPC 清单 | `manifests/batches/npc-objects*.json` |
| 已记录分析 | `docs/VERIFY-2026-11-12-npc-hud-runtime.md` |

## 五、注意事项

- 不要把 `misc/npc.csv` 的 132 行全部当成当前地图 NPC；服务器只刷附近子集。
- 不要手工拼聊天 UI；必须用 `chatmain.scp` + skin 资源 + FSChatMain 逻辑。
- 不要用模糊 name/cell 匹配活跃 NPC；必须精确绑定运行时对象。
- 所有截图/日志/临时脚本放 `.rea/` 或 `client/build/`，不提交。
- 先修 NPC 锚点/测试 NPC（可快速目视验证），再做聊天窗 C++ 复刻。
