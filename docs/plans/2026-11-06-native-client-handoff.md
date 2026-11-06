# 原生离线客户端阶段交接

日期：2026-11-06。适用于在 Kimi Work 或其他会话继续开发。

## 1. 当前目标与边界

目标是复刻《新水浒Q传》原生离线客户端。M1 为“应天府单地图离线探索”：原生窗口、地图/音乐、八方向角色、CEL + A* 点击移动、相机、排序/ghost 遮挡、NPC/MAN 动画、最小 SCP 控件及至少一个真实 Lua 回调。

技术栈：C++20、CMake、Win32/D3D11；CPU 合成画面，D3D11 只负责显示。原 Lua/SCP/CSV/PAK 尽量原样复用，C++ 按反汇编和 Python 参考实现移植。

禁止改变边界：不加载原 EXE/保护 DLL，不联网，不运行游戏作为实现依赖，不修改 `D:\software\dhsh`。真实资源只读；缓存和截图只写构建目录。M1 尚未完成，当前不能称为完整可玩客户端。

## 2. 工作区与版本

- 主工程：`D:\ai-work-flow\rea-dhsh`
- 开发工作区：`D:\ai-work-flow\rea-dhsh\.worktrees\native-client`
- 分支：`feat/native-client`
- 交接时代码 HEAD：`c77dae0186270be0368b56b26658fe55c38e824f`
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
| Task 12：角色/NPC 实体 | **代码和规格审查完成，质量审查未通过** | 已上屏、18/18 测试通过，但有下节 6 项 Important，不能标记完成。 |
| Task 13：CEL A* 与点击移动 | 未开始 | 下一步应在 Task 12 修复复审后进行。 |
| Task 14：地图音乐 | 未开始 | 音频失败隔离。 |
| Task 15：SCP | 未开始 | 控件树和消息分发。 |
| Task 16：Lua | 未开始 | 必须先确认精确 Lua 版本和宿主 ABI，可用 dhsh2 做动态对比。 |
| Task 17：M1 集成 | 未开始 | 联调诊断。 |
| Task 18：M1 验收 | 未开始 | 端到端、性能和记录。 |

除 Task 12 外，上述完成任务均已通过“规格审查 → 代码质量审查”，发现的问题已修复并复审。

## 4. 当前验证状态

最后一次主代理实际执行：

```text
18/18 CTest PASS，0 failed
总耗时约 156 秒
```

包含真实资源测试：

- `avatar_real_resources`：152 帧与 Python 查看器严格对拍。
- `scene_ying_tian_fu_static`：静态视口逐像素对拍。
- `scene_ying_tian_fu_animation`：真实 MAN 动画前后帧对拍。
- `map_formats_ying_tian_fu`：真实地图格式和 486 个 core mask。
- `entities_real_resources`：真实出生点、角色/NPC 资源可解码；它不是完整 App 集成测试。

真实窗口当前能显示应天府、103 个 MAN 动画、body/200 + head/105 角色和一个显式本地 NPC。角色出生 `(6968,2136)`，NPC 为 `npc.pak:10009/10/3.apg`、位置 `(7000,2152)`；该 NPC 是 M1 展示预设，不是原 NPC 放置表还原，见 `docs/plans/2026-11-06-native-client-task12-local-presets.md`。

性能尚未达标：真实窗口曾记录约 49 秒冷启动、约 147–187 ms/frame，需要在 Task 18 前处理，不能把 60 FPS 写成既成事实。

## 5. Task 12 必须先修复的问题

质量审查提交范围为 `6a52cb6..c77dae0`，结论 CHANGES REQUIRED：

1. `client/src/app/App.cpp:235-245` 只装配出生方向的 Idle/Walk；`Entity.cpp:148-154` 转向后选空 clip，角色会消失。需按方向加载/缓存八方向，并加转向测试。
2. `App.cpp:205-215` 指定 `YTF_YaMen.map` 出生记录无效时回退任意 MLK 链接。明确预设失效应报错，不能换门。
3. `App.cpp:332-333` 在 NPC 的 try/catch 前同步必需加载 `npc.pak`；PAK 缺失/损坏会致命退出，破坏单 NPC 失败隔离。
4. `client/src/animation/AnimationClip.cpp:11-13,42-54` 公开构造允许全零时长，`advance` 对 `total_ms==0` 取模。统一把零时长归一化为 80ms，并加回归。
5. 启动和帧耗没有阶段记录及预算。先增加计时和按需/有界缓存，避免八方向加载进一步放大冷启动。
6. `entities_real_resources` 复制 App 的选择/装配逻辑而非执行 App 集成路径，无法发现上述问题。需复用生产逻辑并增加转向、非法出生、NPC 失败隔离和最终像素断言。

次要问题：饱和移动后方向可能与实际位移不一致；负出生坐标除法可能误判格号；实体每帧先复制完整 RGBA 再遍历全部 mask；真实烟测必须校验退出码，NPC 打印的 `current_frame_index()+1` 不是总帧数。

修复要求：恢复原 Task 12 实现者或新实现代理，严格 TDD；修复后先让质量审查者复审，通过才能标记 Task 12 完成并开始 Task 13。

## 6. 构建、测试与运行

在 `D:/ai-work-flow/rea-dhsh/.worktrees/native-client` 执行：

```bash
DHSH_SHDATA='D:/software/dhsh/shdata' \
DHSH_MAP_CACHE='D:/ai-work-flow/rea-dhsh/.worktrees/native-client/client/build/map-cache' \
cmake -S client -B client/build -G "Visual Studio 17 2022" -A Win32 \
  -DPython3_EXECUTABLE='D:/software/anaconda3/python.exe'

cmake --build client/build --config Debug
ctest --test-dir client/build -C Debug --output-on-failure
```

CMake 默认找到的 Python 可能缺少 NumPy/Pillow，所以显式使用 Anaconda。真实资源测试只有在对应环境变量和资源存在时注册；配置日志会明确说明，不能把 7 个合成测试通过误写成全部真实测试通过。

若缓存不存在，只生成选定应天府背景：

```bash
python tools/client_map_cache.py \
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

运行不需要 Python 进程；Python 只用于缓存生成和参考对拍。窗口验证后必须正常关闭，不留后台进程。

## 7. 协作与审查纪律

- 每任务新实现者；完整任务文本由主代理提供，不让子代理自行读完整计划。
- 严格 TDD：先写测试并实际观察 RED，再最小实现到 GREEN。
- 实现后先规格审查，再代码质量审查；问题修复后必须复审，未通过不能进入下一任务。
- 真实资源有限采样、只读；不全量导出，不把 build 产物提交。
- 不推送远端，不合并 main，除非用户明确要求。

Claude CLI 已实测可辅助：

```bash
claude -p --model 'deepseek-v4-flash[1m]' --effort max \
  --permission-mode acceptEdits --tools 'Read,Write,Edit,Grep,Glob' \
  --no-session-persistence --output-format json "具体任务"
```

外部 CLI 不负责构建、测试和 git；主代理必须验证。其配置位于 `C:\Users\Administrator\.claude\settings.json`，含认证信息，不打印或传播。内部子代理可能遇到 5 小时额度限制，已用该 CLI 完成过实现和审查。

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

交接文档提交后的新 HEAD 以 `git log -1 --oneline` 为准。
