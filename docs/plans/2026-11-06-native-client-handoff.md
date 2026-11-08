# 原生离线客户端阶段交接

日期：2026-11-08。适用于切换到 Kimi Code 后继续开发。

## 1. 当前目标与边界

目标是复刻《新水浒Q传》原生离线客户端。M1 为“应天府单地图离线探索”：原生窗口、地图/音乐、八方向角色、CEL + A* 点击移动、相机、排序/ghost 遮挡、NPC/MAN 动画、最小 SCP 控件及至少一个真实 Lua 回调。

技术栈：C++20、CMake、Win32/D3D11；CPU 合成画面，D3D11 只负责显示。原 Lua/SCP/CSV/PAK 尽量原样复用，C++ 按反汇编和 Python 参考实现移植。

禁止改变边界：不加载原 EXE/保护 DLL，不联网，不运行游戏作为实现依赖，不修改 `D:\software\dhsh`。真实资源只读；缓存和截图只写构建目录。M1 尚未完成，当前不能称为完整可玩客户端。

## 2. 工作区与版本

- 主工程：`D:\ai-work-flow\rea-dhsh`
- 开发工作区：`D:\ai-work-flow\rea-dhsh\.worktrees\native-client`
- 分支：`feat/native-client`
- 本轮交接前 HEAD：`7c3d882`（`docs: hand off native client milestone progress`）；本轮提交后以 `git log -1 --oneline` 为准。
- 设计与 18 任务计划：`docs/plans/2026-11-06-native-client-design.md`、`docs/plans/2026-11-06-native-client.md`
- 新代码均在 `client/`；验证工具在 `tools/`。
- 旧版只读基线：`D:\software\dhsh`
- 用户已启动的更新版：`D:\software\dhsh2`，只用于新版动态对比，不能静默替换旧版 PAK 基线。
- `D:\tmp` 资料索引：`docs/analysis-artifacts.md`。

## 3. 阶段进度

| 项目 | 状态 | 结果 |
| --- | --- | --- |
| Task 1：工程/窗口/CTest | 完成 | Win32 工程、烟测和真实窗口关闭。 |
| Task 2：PAKF | 完成 | 安全索引、嵌套边界与真实 config.pak 2470 叶验证。 |
| Task 3：资源接口 | 完成 | 逻辑路径、挂载顺序与错误契约。 |
| Task 4：NZ | 完成 | 0/0A/0B；60 个真实样本逐字节一致。 |
| Task 5：A16/CTI/WIC | 完成 | 48 个 A16、4 个 CTI × 2 表对拍；AI、预算和格式白名单已修复。 |
| Task 6：APG/PIF | 完成 | AG delta、AI、JPEG、延时与跨帧；真实 45 帧验证。 |
| Task 7：地图格式 | 完成 | MAP/CEL/MAN/MLK/core；真实应天府 486 个 mask 全解码。 |
| Task 8：静态场景 | 完成 | 背景缓存、MAN 首帧、CPU 混合；800×600 共 480000 像素与 Python 完全一致。 |
| Task 9：D3D11/输入 | 完成 | 等比黑边、滚动缩放、resize/设备恢复/焦点和 Alt+F4。 |
| Task 10：MAN 动画/遮挡 | 完成 | 稳定排序、PIF、core ghost、共享动画缓存；真实动画逐帧对拍。 |
| Task 11：APCI/纸娃娃 | 完成 | body/200 + head/105；16 序列 152 帧 RGBA/锚点/延时/层叠一致。 |
| Task 12：角色/NPC 实体 | **修复与自动验证完成，最终复审待收尾** | 18/18 CTest、真实像素和 25 秒窗口烟测通过；渲染小组复审 PASS，实体/App 最终外部复审超时。 |
| Task 13：CEL A* 与点击移动 | **未开始** | 等 Task 12 剩余复审完成后再开始。 |
| Task 14：地图音乐 | 未开始 | 音频失败隔离。 |
| Task 15：SCP | 未开始 | 控件树和消息分发。 |
| Task 16：Lua | 未开始 | 必须先确认精确 Lua 版本和宿主 ABI，可用 dhsh2 做动态对比。 |
| Task 17：M1 集成 | 未开始 | 联调诊断。 |
| Task 18：M1 验收 | 未开始 | 端到端、性能和记录。 |

除 Task 12 外，上述完成任务均已通过“规格审查 → 代码质量审查”，发现的问题已修复并复审。

## 4. 当前验证状态

最终代码重建后，主代理实际执行完整 CTest：

```text
18/18 CTest PASS，0 failed
Total Test time (real) = 126.30 sec
```

其中实体、动画、场景合成及新增遮挡缓存回归均通过。真实资源测试包括：

- `avatar_real_resources`：152 帧与 Python 查看器严格对拍。
- `scene_ying_tian_fu_static`：静态视口逐像素对拍，覆盖最终背景快速路径。
- `scene_ying_tian_fu_animation`：真实 MAN 动画前后帧对拍。
- `map_formats_ying_tian_fu`：真实地图格式和 486 个 core mask。
- `entities_real_resources`：复用生产实体装配，验证出生点、八方向、NPC 和最终像素；RGB FNV-1a64 保持 `fc5b7bc888bd4c91`，八方向转向计数为 8。它仍不是完整 App 集成测试。

真实窗口当前能显示应天府、103 个 MAN 动画、body/200 + head/105 角色和一个显式本地 NPC。角色出生 `(6968,2136)`，NPC 为 `npc.pak:10009/10/3.apg`、位置 `(7000,2152)`；该 NPC 是 M1 展示预设，不是原 NPC 放置表还原，见 `docs/plans/2026-11-06-native-client-task12-local-presets.md`。

连续采样修复后的性能基线仍为三次真实 OVER；最终基线日志 `task12-continuous-window-smoke-final.log` 含 88 个统计窗口，`render_avg_ms` 峰值 17.96 ms、`render_max_ms` 峰值 60.05 ms。其 Scene stats 均值为 `entity_mask=4.278 ms`、`entity_draw=4.123 ms`、每次脏帧 `cover_tests=1126`、`covering_masks=774`、`sprite_copies=2`。首次只加候选缓存的实测仍有 57 窗、18 个 OVER，确认完整画布绘制是另一热点，未据此宣称完成。

加入精确遮挡状态键、每实体候选/帧缓存及非透明区域裁剪后，连续三次 25 秒真实烟测均为 `WINDOW_SMOKE PASS exit=0`，分别含 99、101、90 个统计窗口且全部 0 OVER。三次 `render_avg_ms` 均值为 9.098/8.763/9.435 ms，峰值为 15.46/14.33/13.73 ms；`entity_mask` 均值为 1.727/1.617/1.755 ms，`entity_draw` 均值为 0.519/0.473/0.512 ms。对应日志为 `client/build/task12-occlusion-perf-2.log`、`task12-occlusion-perf-3.log`、`task12-occlusion-perf-4.log`。没有放宽 16.67 ms 预算、删除连续/OVER 断言或降低画质。

最小化真实回归先从 `stats_delta=11 title_still=False` 复现失败，修复后得到 `WINDOW_MINIMIZE_PASS minimized_stats_delta=0 title_still=True restore_frame_ms=1.97 exit=0`。采样/诊断单元测试本轮从 4 项失败转为 6 项全部通过，覆盖干净提前退出、存活但不产出帧、最小化、完整采样时长、wall-time 字段和 OVER 拒绝。

## 5. Task 12 本轮修复与交接状态

此前质量审查的 6 项 Important 和次要问题已处理：

1. 角色使用固定 2×8 动画槽，出生方向预装，其余方向 lazy load；加载抛异常时，位置、方向、Idle/Walk 和旧帧保持不变。
2. 仅接受 `YTF_YaMen.map` 出生记录；无效朝向、不可走格、无确认链接均报错，不回退其他门。饱和移动按实际位移定向，负坐标格号向下取整。
3. NPC 独立挂载 `npc.pak`，挂载或解码失败只跳过 NPC，不污染已有实体。
4. 公开 `AnimationClip` 构造把 0 ms 帧归一化为 80 ms。
5. 新增 `LocalEntityFactory` 复用生产装配，补齐异常状态、三种出生失败、NPC 坏帧和真实最终像素测试。
6. PIF 有有效行数时限制 A16 探测；无 PIF、坏 PIF、行数不匹配和超过 1000 行均有回退或上限测试。增加 PAK、场景加载和渲染阶段诊断。
7. 渲染器对全不透明行使用 `memcmp`/`memcpy`，全透明行跳过，混合 alpha 行走原逐像素路径。背景只在加载时验证一次不透明性，已验证背景使用 `draw_opaque`；完整覆盖视口时跳过冗余清屏。
8. CoverMask 预过滤与 `apply` 共用 int64 矩形相交逻辑，不相交时不再复制精灵；另补空精灵直调保护。烟测校验完整日志、退出码、控制异常和任何 `render_budget_ms=16.67 OVER`；不支持的 `--map` 立即退出 1。
9. 最终 App 生命周期复审修复：0×0 最小化客户端每 16 ms 睡眠等待，不推进动画、不输出统计、不改标题；恢复时重置帧时钟与动画余量。窗口烟测在完整采样期间持续检查进程和窗口存活，干净提前退出同样失败。整帧统计明确命名为 `frame_to_present_*` wall time。
10. 连续采样复审修复：stdout/stderr 改为子进程直接写临时文件，采样器用独立句柄读取，消除 Windows PIPE 写满死锁。除进程和窗口存活外，还要求非最小化且 Frame stats 计数较采样起点增加；结束后仍解析完整日志并拒绝任何 OVER。
11. 实体遮挡性能修复：按位置、位图原点/尺寸、动画代次和精确 group active 位图缓存候选 mask；每实体最多缓存 4 个裁剪后的遮挡帧。同帧不重复深度测试或复制，新帧只处理非透明区域；移动、换帧、换动画和 group 切换均有像素回归。

复审状态必须如实保留：渲染/遮挡小组对背景缓存、`draw_opaque` 和矩形预过滤返回 PASS；实体和 App 此前的并行外部审查超时，不能记为外部 PASS。随后 Claude App 复审提出最小化空转、烟测干净提前退出误判和 wall-time 误命名三项 CHANGES REQUIRED，本轮已按 TDD 修复并完成上述真实回归。

Task 12 的指定最终质量修复已经闭合；不要把此表述扩展为其他小组或全功能审查 PASS。当前不要进入音乐、SCP、Lua 或 M1 联调。

## 6. 构建、测试与运行

在 `D:/ai-work-flow/rea-dhsh/.worktrees/native-client` 执行：

```bash
DHSH_SHDATA='D:/software/dhsh/shdata' \
DHSH_MAP_CACHE='D:/ai-work-flow/rea-dhsh/.worktrees/native-client/client/build/map-cache' \
'D:/software/CMake/bin/cmake.exe' -S client -B client/build \
  -G "Visual Studio 17 2022" -A Win32 \
  -DPython3_EXECUTABLE='D:/software/anaconda3/python.exe'

'D:/software/CMake/bin/cmake.exe' --build client/build --config Debug
'D:/software/CMake/bin/ctest.exe' --test-dir client/build -C Debug --output-on-failure
```

CMake 默认找到的 Python 可能缺少 NumPy/Pillow，所以显式使用 Anaconda。真实资源测试只有在对应环境变量和资源存在时注册；配置日志会明确说明，不能把少量合成测试通过误写成全部真实测试通过。

若缓存不存在，只生成选定应天府背景：

```bash
'D:/software/anaconda3/python.exe' tools/client_map_cache.py \
  --shdata 'D:/software/dhsh/shdata' \
  --map YingTianFu \
  --cache client/build/map-cache --png-level 1
```

运行原生窗口：

```bash
client/build/Debug/dhsh_client.exe \
  --shdata 'D:/software/dhsh/shdata' \
  --map YingTianFu \
  --cache client/build/map-cache
```

最终 25 秒自动窗口烟测：

```bash
'D:/software/anaconda3/python.exe' tools/client_window_smoke.py \
  --client 'D:/ai-work-flow/rea-dhsh/.worktrees/native-client/client/build/Debug/dhsh_client.exe' \
  --shdata 'D:/software/dhsh/shdata' \
  --cache 'D:/ai-work-flow/rea-dhsh/.worktrees/native-client/client/build/map-cache' \
  --sample-seconds 25
```

运行不需要常驻 Python 进程；Python 只用于缓存生成、参考对拍和自动烟测。窗口验证后必须正常关闭，不留后台进程。启动阶段可能保留 OVER；烟测会检查脏帧预算、退出码和控制异常。

## 7. 协作与审查纪律

- 每任务新实现者；完整任务文本由主代理提供，不让子代理自行读完整计划。
- 严格 TDD：先写测试并实际观察 RED，再最小实现到 GREEN。
- 实现后先规格审查，再代码质量审查；问题修复后必须复审，未通过不能进入下一任务。
- 真实资源有限采样、只读；不全量导出，不把 build 产物提交。
- 不推送远端，不合并 main，除非用户明确要求。

Claude CLI 可用于小组只读复审：

```bash
'C:/Users/Administrator/.local/bin/claude.exe' -p \
  --model 'deepseek-v4-flash[1m]' --effort high \
  --permission-mode acceptEdits --tools 'Read,Grep' \
  --no-session-persistence --output-format text "具体小组审查任务"
```

本轮三组并行审查均超时，随后单独的小组审查成功返回。后续一次只审一个小组，不做宽泛审查，不把超时记为 PASS。外部 CLI 不负责构建、测试和 git；主代理必须验证。其配置位于 `C:\Users\Administrator\.claude\settings.json`，含认证信息，不打印或传播。

## 8. 关键提交

- `d6105a2` 静态应天府与缓存/像素对拍
- `0e267a9` 原生 D3D11 视口
- `d851c2d` 窗口生命周期与设备恢复
- `9e80622` MAN 动画与遮挡
- `ffd30cd` 动画共享缓存、边界和真实动画测试
- `254aca8` APCI 与纸娃娃
- `6a52cb6` 角色预算与 152 帧自动对拍
- `62919fd` 外部分析资料索引
- `c77dae0` 本地角色/NPC（规格通过，质量待修复）
- `7c3d882` 上一阶段交接文档

本轮 Task 12 质量修复和交接提交后的新 HEAD 以 `git log -1 --oneline` 为准。
