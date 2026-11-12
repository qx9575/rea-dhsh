# 应天府 NPC 与基础 HUD 接入（2026-11-12，1.50.29 运行时）

本文记录原版 `Game.exe` 1.50.29 的 NPC 对象布局、动态加载行为、配置表解析，
以及原生客户端 `client/` 已接入的 NPC 与基础 HUD 面板。所有动态读取均为
只读；未修改 `D:\software\dhsh2` 进程或文件。

## 一、NPC 配置表

- 资源：`config.pak:misc/npc.csv`（44,846 B，GBK 纯文本，非 NZ 封装）。
- 列：`NPCID, 名称, 地图名, 地图文件名, 坐标X, 坐标Y, 显示地图, NPC信息,
  NPC描述, 名字分类(0/1/2/3), 室外X, 室外Y, 简述`。
- 坐标是 **格子坐标**；其实测像素坐标字段数值与 `cell * 16 + 8` 对应，
  但该数值对应不提前确定原版绘制锚点语义。
- 应天府（`YingTianFu`）共 132 行（按地图文件列筛选，含重复/活动 NPC）。
- 该表只提供位置/名称/分类，**不包含模型 ID**；模型由服务器下发到 NPC 对象。

## 二、NPC 世界对象（运行时内存，只读）

在运行中的 1.50.29 客户端内存中扫描对象 vtable（模块基址 0x400000）：

| 项 | 值 |
|---|---|
| NPC 类 vtable | `0x984554`（类大小 0x6A0，构造 `0x43DB50`） |
| 名字 | 对象 `+0x04` 起，内联 GBK，NUL 结尾 |
| 模型/shape | `u16 @ +0x28` |
| 像素坐标字段 X | `u16 @ +0x2A` |
| 像素坐标字段 Y | `u16 @ +0x2E`（注意字段间有间隔） |
| 当前方向 | `u8 @ +0x32`（1/3 等，对应 APG 方向） |

实测样例（2026-11-12，PID 28816）：

| 名称 | NPCID | shape | 格子 | 实测像素值 |
|---|---:|---:|---|---|
| 守门人 | 1039 | 10051 | (441,134) | (7064,2152) |
| 应天府衙役 | 1004 | 10040 | (432,130) | (6920,2088) |
| 落魄僧人 | 1014 | 20044 | (464,138) | (7432,2216) |
| 黄文炳 | 1083 | 20113 | (449,140) | (7192,2248) |
| 星神使者 | 8342 | 15025 | (462,158) | (7400,2536) |
| 金枼姿 | 8377 | 10093 | (456,148) | (7304,2376) |
| 庄园管理员 | 8348 | 26152 | (422,130) | (6760,2088) |
| 房屋管理员 | 4001 | 15026 | (416,127) | (6664,2040) |

- 运行时动画路径形如 `npc/<shape>/10/<direction>.apg`；`npc.pak` 内实际
  归档路径为 `<shape>/10/<direction>.apg`（无 `npc/` 前缀）。
- 世界 NPC 是**动态加载**的：只扫描到玩家附近已创建的 7 个对象（2026-11-12 在衙役/守门人之间复核为 6 个，房屋管理员新增）；全部 132 行
  不会同时存在。因此不能把一次扫描当作完整名单。

## 三、客户端实现

### 3.1 NPC 名单

新增 `client/src/scene/NpcRoster.*`：

- `load_npc_roster(csv, map_file)`：解析 GBK CSV，按 `地图文件名` 筛选，
  生成 `NpcSpawn{id,name,cell_x,cell_y,category}`；名称转 UTF-8 仅用于诊断。
- `resolve_npc_model(spawn, npc_resources)`：
  1. 先用运行时抓到的 name/cell 提示（`kNpcModelHints`）匹配 shape；
  2. 否则尝试 `10000+npcid`、`10000+(npcid%1000)`、`10000+(npcid%100)`，
     仅接受 `npc.pak` 中实际存在的 `<shape>/10/1.apg` 或 `<shape>/10/3.apg`；
  3. 仍无则回退到通用模型 `10009`。
- 提示表来自 `tools/capture_npc_objects.py`（只读扫描 vtable），输出
  `manifests/batches/npc-objects.json`；继续走图后可重新抓取并补充提示。

### 3.2 实体与动态加载

- `LocalEntityFactory::add_local_npcs_if_available`：读取 `config.pak`
  的 `misc/npc.csv`，为每行创建 `Entity`（位置 = cell*16+8），安装懒加载器，
  放入 `MapScene::npcs()`。
- `MapScene::preload_visible_npcs`：每帧只为视口附近（±96px）且未尝试过的
  NPC 触发一次动画解码，按离视口中心距离排序，默认每帧最多 8 个；失败只尝试
  一次。`MapScene::advance` 不再推进尚未加载的 NPC。
- 这样既不会一次解码 132 个 APG 卡住首帧，也符合原版“只加载附近 NPC”的
  动态行为。
- 2026-11-12 Task 1 复核：`client/src/formats/Apg.cpp:191-192` 已按
  `int16_t` 解析帧头 `x/y` 到 `ApgFrame.anchor_x/anchor_y`，
  `client/src/animation/AnimationClip.cpp:31-32` 已传递，
  `client/src/scene/Entity.cpp:133-141` 的 `Entity::frame_origin()` 当前执行
  `position + anchor`。任何“anchors 未赋值/默认 0”的旧描述均已过时。
- 剩余未知是原版 NPC 绘制公式：帧头 anchor 应当相加、相减还是配合固定偏移，
  以及 `+0x2A/+0x2E` 在原版绘制中代表左上、脚底还是中心；不得再重复修改
  anchor 解析与传递代码。

### 3.3 基础 HUD 面板

按 1.50.29 的 HUD 创建调用直接加载对应 SCP（未手工拼布局）：

| SCP | 原版创建点 |
|---|---|
| `ui/main/headwnd.scp` | `0x5E4A50` getter，init `0x411148` |
| `ui/main/minimap.scp` | `0x42B250` getter，init `0x41100F` |
| `ui/main/smallmap.scp` | `0x42F640` getter，init `0x411031` |
| `ui/main/bar.scp` | `0x468210` getter |
| `ui/main/chat.scp` | `0x485C80` getter，map-enter `0x41286C` |
| `ui/main/daynight.scp` | `0x46AE90` getter，init `0x411174` |
| `ui/main/fucnbtn_shortcut.scp` | 既有 M1 面板 |

`ui/main/chatmain.scp` 是原版的**分离聊天窗口**，默认不加载。点击 `chat.scp`
左侧竖排第一个按钮（`chat_switch`，控件矩形 1,466,16,16）时，客户端新建一个
独立 Win32 窗口“聊天窗口”（549×590，放在主窗口右侧）。运行时抓取结果：
窗口类 `fsgamehero0150`，客户区 549×590；使用 `config.pak` 的 640×480
`chatmain.scp` 缩放布局，背景是 `ui.pak:chat/chatskin/skin0.tga` 64×64
平铺纹理。客户端现在按此实现：独立窗口加载 config.pak+ui.pak、平铺 skin0、
将 640×500 源布局缩放到 549×590，并显示频道顶排和底部横排。关闭窗口即销毁。

当前 `ScpWindow` 已能解析并绘制这些面板的静态部分；日志会逐条列出尚未支持的
控件（RATEBAR/COMBOBOX/TXTLIST/SCROLL/RICHEDIT/LISTVIEWEX 等），这些不影响
其余面板加载。

## 四、验证

- `cmake --build client/build --config Release --target dhsh_client` 通过。
- 启动日志：`NPC roster YingTianFu: configured=132 added=132 failed=0`；
  `NPCs=132`。
- 45 秒窗口烟测：120 帧/秒，`render_avg≈3.4–4.4 ms`，
  `frame_to_present≈16.4 ms`，PASS。
- 首帧截图 `client/build/npc_panels2.png`：可见 headwnd、右上小地图控件、
  bar、聊天栏、daynight 与玩家附近 NPC；守门人/衙役/落魄僧人/黄文炳等使用
  运行时匹配模型，未再回退 10009。
- 默认 HUD 截图 `client/build/npc_panels3.png`：`综/当/队…` 顶排与底部
  F1–F8 横排已不再出现。
- 点击 `chat_switch` 后，独立窗口测试截图 `.rea/chat_window.png`：
  549×590 的“聊天窗口”在右侧创建，显示顶部频道按钮排和底部横排按钮；
  聊天消息列表 `chatmain_txtlst_msg` 仍为未支持的 LISTVIEWEX，内容区暂时只显示 skin0 平铺背景，尚无聊天文本。
- 注意：许多远端/活动 NPC 仍无运行时提示，暂时回退到候选模型或 10009；
  世界 NPC 头顶名称尚未绘制。
- `minimap.scp`/`smallmap.scp` 已加载；其 `minimap_map`/`sys_smallmap_win`
  是原版由代码绘制地图内容的空白窗口，客户端尚未实现这块自定义绘制
  （当前可见的是窗口框架，而不是缩放地图）。

## 五、产物

| 类别 | 路径 |
|---|---|
| 运行时抓取工具 | `tools/capture_npc_objects.py` |
| NPC 对象清单 | `manifests/batches/npc-objects.json` |
| 客户端 NPC 解析/模型 | `client/src/scene/NpcRoster.*` |
| 客户端实体接入 | `client/src/scene/LocalEntityFactory.*`、`MapScene.*` |
| 客户端 HUD 接入 | `client/src/app/App.cpp` |
| 首帧截图 | `client/build/npc_panels2.png`（构建产物，不入库） |

## 六、Task 1 视觉基线（2026-11-12）

### 实际执行与产物

在 worktree 根目录实际成功运行：

```bash
set -o pipefail; python .rea/task1_capture_baseline.py 2>&1 | tee .rea/task1_capture_baseline.log
```

脚本只启动/关闭自己创建的 `dhsh_client` 进程；成功的客户端调用为：

```bash
D:/ai-work-flow/rea-dhsh/client/build/Release/dhsh_client.exe \
  --shdata D:/software/dhsh/shdata \
  --cache D:/ai-work-flow/rea-dhsh/client/build/map-cache \
  --log D:/ai-work-flow/rea-dhsh/.worktrees/chat-npc-replication/client/build/task1-baseline.log
```

客户端由脚本正常关闭，返回码 0；日志含 `SCP chatmain window=new`。真实截图：

- `.rea/task1_baseline_main.png`：主窗口外框 816×639（客户区 800×600）。
- `.rea/task1_baseline_chat.png`：分离聊天窗外框 565×629（客户区 549×590）。
- 运行日志：`client/build/task1-baseline.log`；捕获控制台日志：
  `.rea/task1_capture_baseline.log`。对照图只读使用主工作区
  `.rea/live_game_now.png` 与 `.rea/live_chat.png`。

指定的主工作区 Debug 路径也以相同参数实际尝试过，但其文件时间为
2026-11-11 17:44，启动日志只有 `NPCs=1` 且只加载 `bar.scp`，无法打开当前
分离聊天窗。主工作区 Release 文件时间为 2026-11-12 16:07，启动日志为
`NPC roster ... added=6 failed=0`、`NPCs=6`，因此以上成功基线改用该现有
Release；Task 1 未运行 cmake 配置或任何编译命令。

### 已观察事实

- NPC：源码事实如第 3.2 节所述，当前绘制原点执行 `position + anchor`；
  主窗口截图可见当前 NPC 与原版对照的位置关系仍不正确。此次未做同相机
  包围盒测量，不定量宣称 delta。
- 聊天：分离窗口能创建，但截图只有频道/底部按钮和背景，没有消息文本或
  输入区；运行日志仍有 LISTVIEWEX/RICHEDIT 等未支持控件。
- 右侧 UI：小地图只有框架/控件，没有地图内容；`fucnbtn_shortcut.scp`
  的 8 个入口仍报告 unresolved layout，点击与真实面板交互不完整。

### 构建与测试结果（主代理）

原版 NPC 对 anchor 使用加法、减法还是固定偏移仍待逆向。

主代理已在 worktree 完成 Debug 构建，以下命令成功，exit 0：

```bash
cmake --build client/build --config Debug --target dhsh_test_entities dhsh_test_scp_ui dhsh_client
```

实际运行：

```bash
ctest --test-dir client/build -C Debug -R '^(entities|scp_ui|m1_)$' --output-on-failure
```

该正则末尾的 `$` 使 `m1_` 无法匹配两个 m1 测试；实际只匹配并运行
`entities`、`scp_ui`，二者均 Passed。日志见 `.rea/task1_ctest.log`。

初次另跑 `ctest -R '^m1_'` 时，`m1_acceptance` 因尚未构建
`dhsh_test_m1_acceptance` 而 Not Run，`m1_app_acceptance` 因 Debug 配置跳过。
随后成功构建该目标并重跑，最终 `m1_acceptance` Passed（39.14s）；
`m1_app_acceptance` 输出 `SKIP: M1 App acceptance is Release-only, got Debug`。
这是脚本明确的 Release-only 行为，不计为 Passed，仍留待 Release 验收。
m1 日志见 `.rea/task1_m1_ctest.log`。
