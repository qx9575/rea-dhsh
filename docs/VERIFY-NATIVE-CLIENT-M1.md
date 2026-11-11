# 原生离线客户端 M1 验收记录

日期：2026-11-11。工作区：`.worktrees/native-client`，分支 `feat/native-client`。

## 结论与边界

本记录区分真实 PAK 子系统验收和真正的 Win32/D3D11 客户端验收，不用单元测试代替原生窗口结果。

最终完整回归：Release **27/27 PASS、0 failed、真实 M1 两项均未 SKIP**，144.60 秒；Debug **26 PASS + 1 个 Release-only 明确 SKIP、0 failed**，183.17 秒。Release ASan 完整 M1 驱动退出 0。Task 18 规格、UAF 专项及最终 Win32 异常/F8 重开复审均由 Claude/DeepSeek 返回 APPROVED。

- 资源基线固定为旧版只读 `D:\software\dhsh`，运行时使用专用 `client/build/map-cache`。
- 新版 `D:\software\dhsh2` 仅作用户已启动实例的动态视觉参照，不替换旧版 PAK，不关闭或修改其进程。
- 不加载原 EXE/保护 DLL，不联网；缓存、日志和截图只写构建目录。
- 正常模式不加载登录公告。`--acceptance` 是明确诊断模式，从专用 `config.pak` provider 加载真实 `ui/login/bulletin2.scp`，避免 `800config.pak` 同名空壳遮蔽；F8 隐藏/重开公告。

## 构建与验收命令

```bash
DHSH_SHDATA='D:/software/dhsh/shdata' \
DHSH_MAP_CACHE="$PWD/client/build/map-cache" \
cmake -S client -B client/build -G 'Visual Studio 17 2022' -A Win32 \
  -DPython3_EXECUTABLE='D:/software/anaconda3/python.exe'
cmake --build client/build --config Debug
cmake --build client/build --config Release
ctest --test-dir client/build -C Release --output-on-failure
```

单独运行两级 M1 验收：

```bash
ctest --test-dir client/build -C Release -R m1_ --output-on-failure
```

`m1_acceptance` 缺真实参数时打印 SKIP 并返回 77；CTest 使用 `SKIP_RETURN_CODE=77`。真实资源齐全时该测试必须显示 Passed，不允许把 SKIP 当作验收通过。`m1_app_acceptance` 只执行 Release；其他配置明确 SKIP，因为计划要求真实 Release 验收，且 Debug 启动预算已有已知波动。

## 真实 PAK 子系统验收

证据：`client/build/task18-m1-subsystem-final.log`。

```text
M1 scene PASS background=7808x5760 objects=103 masks=563 depth_sorted=true
M1 spawn PASS position=6968,2136 facing=left_down npc=10009/10/3.apg cover_tests=1126
M1 navigation PASS blocked=goal_blocked unreachable=395,114 target=6808,1896 samples=95 blocked_samples=0 walk_frame_changes=66
M1 eight_directions PASS turns=8 walk_playbacks=8 idle_playbacks=8
M1 animation PASS npc_advanced=true man_frame_changes=1009
M1 occlusion PASS mask=70 position=6536,1368 pixel=413,227 covering_masks=464 sprite_copies=2
M1 lua PASS source=ui/login/bulletin2.scp registered=3 calls=1 cleanup=unsubscribed
M1 cleanup PASS lua_unsubscribed=true
M1_SUBSYSTEM PASS real_pak=true map=YingTianFu
```

覆盖内容：

- 生产 `MapScene`/MAN/core 加载、MLK 确认出生点、固定角色和显式 NPC 预设。
- `MapInput` 按下/抬起转换为地图目标；阻挡格返回 `goal_blocked`，另用 CEL flood-fill 找到可走但不连通的 `(395,114)` 并断言 `unreachable`。
- 可达路径 95 次模拟采样均不进入阻挡格，Walk 帧实际变化 66 次，最终精确到达 `(6808,1896)` 并回 Idle。
- 八个方向的 Walk/Idle 都实际推进帧，不只是检查资源存在。
- core 不仅调用 `CoverMask::apply`，还对 `MapScene::render` 最终像素做精确比较，并排除后续 MAN/NPC 非透明像素覆盖。
- 原 `bulletin2.scp` 的 NZ 数据、3 个回调注册、按钮消息、`OnShowPolicy` 和窗口状态变化；窗口销毁后消息订阅已注销。

## 真实 Release App 验收

`m1_app_acceptance` 通过 `tools/client_m1_acceptance.py` 启动真正的 `dhsh_client.exe`，发送 Win32 鼠标/键盘消息并正常 WM_CLOSE。证据为 `client/build/tests/m1-app-Release.json`、`.log`、`.stdout.log`、`.stderr.log` 和 `.ppm`。

最终完整 Release 回归中的通过记录：

```json
{
  "spawn": [6968, 2136],
  "lua_callback": "OnShowPolicy",
  "ui_click_preserved_player": true,
  "reopen_callback": "OnShowPolicy",
  "reachable": {"position": [6808, 1896], "positions": 66, "seconds": 4.604},
  "blocked": "goal_blocked",
  "camera_drag": [6208, 1596],
  "unreachable": {"cell": [395, 114], "status": "unreachable"},
  "exit": 0,
  "startup": "Startup total ms=4565.22",
  "frame_windows": 31,
  "diagnostic_budgets": "PASS"
}
```

该流程先点击真实公告按钮，确认原 Lua 回调和 `policy_visible=true bulletin_visible=false license_visible=false`，并等待 300 ms 后确认地图角色未移动；随后实际完成 F8 隐藏→重开→第二次 `OnShowPolicy`→再次隐藏，见 `task18-app-acceptance-reopen.log`。之后完成地图寻路。相机精确拖动 200 像素后，点击非连通目标仍不移动。启动报告为 `success=115 cache=1 skipped=0 failure=0`、`fatal_error=none`、`exit_code=0`，stderr 为空。

## 截图与对拍

- 原生正常模式首帧：`client/build/task18-release-baseline.png`。
- 原生验收模式首帧：`client/build/task18-app-acceptance.png`。
- 用户已运行新版客户端的只读 `PrintWindow` 捕获：`client/build/task18-original-new-version-printwindow.png`。
- 新版标题显示 1.50.27，角色位置、在线实体和 UI 状态均不同；该图只作视觉参照，不做整帧像素相等声明。最初屏幕区域截图被编辑器遮挡，已弃用，不作证据。
- 位精确证据仍由 `scene_ying_tian_fu_static`、`scene_ying_tian_fu_animation`、`avatar_real_resources` 和 `entities_real_resources` 提供；它们与 Python 参考实现/已确认基线对拍。验收模式只验证公告状态和回调，不声称完整公告图片或在线场景一致。

## 性能与稳定性记录

- Task 17 最新正常 Release 25 秒烟测：启动 4891.22 ms，12 个帧统计窗口，所有启动/帧预算 PASS，正常退出 0。
- Task 18 最终 App 完整验收：启动 4565.22 ms，31 个帧统计窗口，所有诊断预算 PASS。
- 其他运行曾出现资源启动 2022.61 ms（预算 2000 ms）以及高负载下总启动 37.52/43.32 秒；这些记录保留，不用最快值掩盖启动波动。
- 真实 App 运行中的普通帧绘制仍是约 2 ms 量级；最终统计以对应 stdout 的 `Frame stats` 为准，不把启动耗时混入帧均值。

## UAF 定位与修复

完整驱动曾间歇出现 `Bulletin_win00` 初始可见性错误；独立 `lua_host_real` 连续 50 次通过，但完整驱动约每 5 次可复现一次。Win32 ASan 定位为：

```text
heap-use-after-free
Widget::visible
ScpWindow::build_node
widgets_.emplace_back 扩容后继续使用失效的 Widget&
```

新增 96 个子控件的扩容回归，在修复前由 ASan 明确报 UAF。修复为在进入递归前保存 `child_parent_visible`，不再跨子控件扩容保存父控件引用。修复后该 ASan 用例退出 0，普通 Release 完整 M1 驱动连续 10/10 通过，Release ASan 完整驱动也退出 0。证据：`task18-scp-growth-asan-red.log`、`task18-scp-growth-asan-green.log`、`task18-m1-fixed-loop-*.log`、`task18-asan-release.log`。

运行期事件异常边界也已补齐：`Win32Window::emit` 在 user32 回调内保存首个异常并请求退出，App 在 WM_QUIT 中停止音频后重新抛出，由既有致命报告路径返回非零。`presenter` 的真实 Win32 异常用例先因接口缺失 RED，修复后 5/5 PASS。F8 隐藏/重开及两次原 Lua 回调由真实 Release App 验收覆盖。

## 原资源完整性

七类原 PAK 在本轮完整回归前后计算 SHA-256：`800config.pak`、`config.pak`、`ui.pak`、`map.pak`、`player.pak`、`npc.pak`、`perinfo.pak`。结果 `PAK_SHA256_MATCH`，前后文件为 `task18-pak-sha256-before.json` 和 `task18-pak-sha256-after.json`。

## 已知限制

- M1 仍不包含登录认证、真实网络、地图切换、NPC 对话、战斗、任务、背包、存档或完整 SCP 控件集。
- 角色为 body/200 + head/105 固定预设，NPC 为显式 `10009/10/3.apg` 预设，不是原地图完整 NPC 放置表。
- 正常模式主界面根 SCP 没有 Lua 脚本；原 Lua 链路只在明确 `--acceptance` 模式加载公告。`.lua` include 和 SCP include 内脚本仍未实现。
- 隐藏控件按需图片加载、父子动态可见性级联和完整公告视觉仍有限制。
- 动态帮派 group/frame 不在应天府数据中，不能由本图验收。
