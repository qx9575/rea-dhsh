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

### 3.1 NPC 静态元数据与协议身份

`client/src/scene/NpcRoster.*` 只保留静态 CSV 元数据解析：

- `load_npc_roster(csv, map_file)`：解析 GBK CSV，按 `地图文件名` 筛选，
  生成 `NpcSpawn{id,name,cell_x,cell_y,category}`；名称转 UTF-8 仅用于诊断。
- CSV 不提供线上对象身份、模型或当前方向。Task 5 已删除 `kNpcModelHints`、
  name/cell 提示匹配、候选 shape 算术和通用模型 10009 回退，不再根据 132 行
  静态名单生成世界 NPC。

### 3.2 实体、动态加载与 NPC 绘制锚点

- `LocalEntityFactory::make_network_npc(object, resources)` 只使用协议对象的
  shape、位置和方向创建实体；方向资源回退只在同一 shape 内进行且候选方向去重。
- `MapScene` 以 `object_id` 键控网络 NPC。同 ID 更新先构造替代实体再替换，构造
  失败保留旧实体；删除只移除指定 ID，并重排其余索引。
- `MapScene::preload_visible_npcs`：每帧只为视口附近（±96px）且未尝试过的
  NPC 触发一次动画解码，按离视口中心距离排序，默认每帧最多 8 个；失败只尝试
  一次。`MapScene::advance` 不再推进尚未加载的 NPC。
- APG 原始帧头 `x/y` 继续按有符号 `int16_t` 解析。网络 NPC 不创建测试模型
  `10009`，也不为其保留 shape 回退。
- Task 4 已确认原版先转换 APG 锚点，再在绘制时做减法；旧的
  `position + APG anchor` 描述已废弃。运行时帧 `+4/+8` 是两个 dword，不是两个
  int16。完整公式见第七节，回归结果见 `.rea/task4_final_ctest.log`。

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

## 四、早期名单原型验证（Task 5 前）

- `cmake --build client/build --config Release --target dhsh_client` 通过。
- 当时启动日志：`NPC roster YingTianFu: configured=132 added=132 failed=0`；
  `NPCs=132`。该名单生成方式已由 Task 5 的协议对象驱动替代。
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
- 当时许多远端/活动 NPC 无运行时提示，会回退到候选模型或 10009；Task 5 已
  删除该行为。世界 NPC 头顶名称尚未绘制。
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
m1 日志见 `.rea/task1_m1_ctest.log`。该节保留 Task 1 当时的状态，其中 anchor
公式未知和 `position + anchor` 的描述已由 Task 4 结论取代。

## 七、Task 4：NPC 绘制锚点与测试 NPC 删除（2026-11-13）

### 逆向证据与公式

分析对象为已固定版本的原版客户端：
`extracted/gameexe-snapshots/Game-1c0704f9e31c21205accdf41e0c269340795022c3b7770c1bc8dcbb80dd96619.exe`，
SHA-256 为 `1c0704f9e31c21205accdf41e0c269340795022c3b7770c1bc8dcbb80dd96619`。
静态反汇编显示 `0x817b8c sub ebx, 0xa0`、`0x817b9b sub ecx, 0xd2`
计算后写入运行时帧 `+4/+8`；等价代码还见于 `0x81c187/0x81c196`。最终绘制
做减法的证据位于 `0x835de9`、`0x835e32`、`0x7f59fc`、`0x7f5a01`。
`0xa42a8c` 是 vtable 地址，不是函数起点。

两阶段公式为：

```text
runtime_anchor = (160 - APG.x, 210 - APG.y)
frame_origin   = logical_position - runtime_anchor
```

只读 RPM 对五个本地 APG 齐全的 shape 共 62 帧逐帧验证：
10093 为 12 帧，10040 为 10 帧，15026 为 8 帧，10051 为 16 帧，
20113 为 16 帧；每帧均满足 `APG.x + runtime.x = 160`、
`APG.y + runtime.y = 210`。Task 4 当时只索引 `npc.pak`，因此 26152 未纳入该数值证明；
Task 5 已在 `npc3.pak` 找到其 APG，但这里没有补做原进程 RPM 锚点验证。
证据与脚本保存在 `.rea/task4_compare_anchors.py`、
`.rea/task4_anchor_transform_confirmed_region.txt`、
`.rea/task4_actual_anchor_functions.txt` 和 `.rea/task4_live_npc_frames.json`。

### 实现与验证

- `AnimationFrame.anchor_x/y` 扩为 int32，以容纳转换后的值；APG 原始字段仍按
  int16 解析。角色画布仍使用 `(320,240)` 锚点。
- NPC APG 解码后逐帧转换为 `(160-source.anchor_x, 210-source.anchor_y)`；
  `Entity::frame_origin()` 饱和计算 `position - frame anchor`。
- 删除硬编码测试 NPC 路径；网络 NPC 仅按世界对象 shape 创建，不回退到 10009。
- TDD 首先用真实 `10051/10/3` 验证 APG `(133,113)` 必须成为运行时 `(27,97)`，
  对象 `(7064,2152)` 对应绘制原点 `(7037,2055)`；旧实现按预期失败，转换后通过。
- 真实资源画面的已核对像素基线为 `0xa200d645cfe3166c`。最终
  `animation`、`entities`、`entities_real_resources`、`m1_acceptance`
  四项 Debug 测试全部通过，日志见 `.rea/task4_final_ctest.log`。
- 复刻客户端正常退出，日志 `fatal_error=none`、`exit_code=0`。实际查看
  `.rea/task4_entities_runtime_anchor.png`、`.rea/task4_client_frame_fixed.png` 和
  `.rea/task4_six_npc_comparison.png` 后，同相机六个 NPC 的位置已对齐，原先整体
  偏左上的问题消失。庄园管理员仍使用现有名单逻辑的回退模型 10009，故其外形不同；
  这属于后续名单/模型问题，不改变本次坐标对齐结论。

## 八、Task 5：DHR1 回放驱动 NPC（2026-11-13）

### 真实输入与实现

- `.rea/npc-login.dhr1` 从同一登录流的只读被动抓包提取，包含 4 条短版
  s2c `0x301e` NPC upsert：家具托管员 1326219 / 10001 / `(408,121)`、
  房屋管理员 1326218 / 15026 / `(416,127)`、庄园管理员 1326829 / 26152 /
  `(422,130)`、应天府衙役 1325962 / 10040 / `(432,130)`，方向均为 1。
  原始重组流没有可靠逐帧时间，因此 4 条记录均为 `elapsed_ms=0`，没有编造延迟。
- `dhsh_client --replay <DHR1>` 使用虚拟累计毫秒驱动 `NpcReplayDriver`；EOS 后
  仍完成可见 NPC 资源尝试、最终渲染和 present，再正常退出。非回放模式不再从
  静态 CSV 名单创建 NPC。
- `ObjectRemove` 到 store/scene 的路由由单元测试覆盖；真实 `0x3030` 删除协议
  仍未闭环，解码器不会把它或未知命令猜成删除。

### 验证结果

- 新测试先因缺少 `network/NpcReplayDriver.h` 编译失败（预期 RED），实现后键控
  创建/同 ID 更新/精确删除、失败预算和虚拟时钟测试全部通过。
- `protocol_replay`、`world_objects`、`animation`、`entities` 四项 Debug 回归通过；
  `entities_real_resources` 的真实视口哈希仍为 `0xa200d645cfe3166c`；真实资源
  `m1_acceptance` 全部子系统通过。日志为 `client/build/task5-regression.log`、
  `task5-entities-real.log` 和 `task5-m1-real.log`。
- 真实回放返回 0，汇总为 `messages=4 upserts=4 removals=0 ignored=0`；日志逐项
  记录上述对象 ID、shape、位置、方向和 revision。10001、15026、26152、10040
  均实际加载自己的 `10/1.apg`，没有 10009 或跨 shape 回退。前三者来自 `npc.pak`，
  26152 来自补挂的 `npc3.pak`。见 `client/build/task5-npc-replay.log`。
- `npc3.pak` 的部分 kind=1 嵌套目录声明 `size=0`，但该位置存在有效 `PAKF3001`
  子表。新增回归测试先复现 `archive entry is out of bounds`，随后仅对 size=0
  的嵌套目录使用父归档剩余范围；非零声明范围仍严格强制。全部 PAK 单元测试通过，
  真实 `npc3.pak` 成功索引 1811 个条目，包括 `26152/10/1.apg` 和
  `26152/101/0.apg`。
- 已实际查看 `.rea/task5_npc_replay.png`：当前玩家相机视口内可见 15026、26152、
  10040 三个回放 NPC，10001 在视口外；四个模型均有本地资源并实际加载。此次没有
  取得同一真实会话的六 NPC 回放样本，不把其他会话的 golden 拼接成六 NPC 证据。
- 补挂 `npc3.pak` 后的新 Release 客户端由 `tools/client_m1_acceptance.py` 直接验收，
  最终通过，6 个帧统计窗口及诊断预算均 PASS，结果见
  `client/build/task5-m1-app-Release-npc3-final.json`。同一二进制的前一次运行曾出现
  `Startup resources ms=2077.10` 短暂 OVER；功能步骤均完成且客户端 exit=0，但整体验收
  按失败保留在 `task5-m1-app-Release-npc3.json`，未改写为成功。
