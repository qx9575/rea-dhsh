# Input and Occlusion Fix Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

> 更正（2026-11-11 晚）：本文第 5/7 行及 Task 中“打包 16 位 RGB 暗化、保留 alpha”的
> 结论已被复核推翻；原版 ApplyMask 修改精灵 alpha 通道（`alpha >> 3`，RGB 不变）。
> 见 `docs/VERIFY-INPUT-OCCLUSION.md` 文末更正。以下保留为当时执行记录。

**Goal:** 分离左键寻路与右键拖图，平滑恢复相机跟随，并按原版 16 位像素算法修正角色遮挡颜色。

**Architecture:** Win32 只扩展事件与捕获；`MapInput` 统一拥有地图鼠标手势；App 只消费点击、拖动和跟随状态。core 掩码继续负责深度与 RLE，仅将覆盖像素的 RGBA 转换修正为打包 16 位位移语义，保留 alpha 和现有缓存。

**Tech Stack:** C++20、Win32、CMake/CTest、现有原生测试与 Python 窗口验收驱动。

参考设计：`docs/plans/2026-11-11-input-occlusion-fix-design.md`。全程使用 `D:/software/dhsh/shdata` 只读资源，不加载原 EXE/保护 DLL，不修改运行中的游戏。

---

### Task 1: Win32 右键事件与成对捕获

**Files:**
- Modify: `client/src/platform/Win32Window.h`
- Modify: `client/src/platform/Win32Window.cpp`
- Test: `client/tests/test_presenter.cpp`

**Step 1: 写失败测试**

在真实 `Win32Window` 测试中依次发送 `WM_RBUTTONDOWN`、`WM_MOUSEMOVE`、`WM_RBUTTONUP`，断言收到 `right_button_down`、带逻辑坐标的 `mouse_move`、`right_button_up`。增加左右键交错用例：左键按下后右键按下，左键松开时右键仍捕获，最后右键松开才释放捕获。先使用 `GetCapture() == window.handle()` 和事件序列断言。

**Step 2: 验证 RED**

```bash
cmake --build client/build --config Release --target dhsh_test_presenter
ctest --test-dir client/build -C Release -R '^presenter$' --output-on-failure
```

预期：因右键事件枚举/映射缺失而编译失败，或右键用例没有收到事件；不能通过删除事件断言取得通过。

**Step 3: 最小实现**

在 `WindowEvent::Type` 增加 `right_button_down/right_button_up`。在窗口类记录左右键捕获位；有效内容区的左右键按下均 `SetCapture`，对应松开先清除该位，仅在无剩余捕获键时 `ReleaseCapture`。`WM_CAPTURECHANGED`/`WM_KILLFOCUS` 清除捕获位并继续发出一次 `input_cancelled`。

**Step 4: 验证 GREEN**

重复 Step 2；预期 presenter 全部 PASS，既有异常跨越 Win32 边界测试仍通过。

**Step 5: 提交**

```bash
git add client/src/platform/Win32Window.h client/src/platform/Win32Window.cpp client/tests/test_presenter.cpp
git commit -m "fix(client): add right-drag window events"
```

---

### Task 2: 分离地图点击与右键拖动

**Files:**
- Modify: `client/src/input/MapInput.h`
- Modify: `client/src/input/MapInput.cpp`
- Modify: `client/src/app/App.h`
- Modify: `client/src/app/App.cpp`
- Modify: `client/tests/test_navigation.cpp`
- Modify: `client/tests/test_viewport.cpp`

**Step 1: 写失败测试**

在 `test_navigation.cpp` 添加：

```cpp
input.handle_event(mouse_event(Type::right_button_down, LogicalPoint{20, 30}), view);
check(input.camera_dragging(), "right down did not start camera drag");
check(!input.handle_event(mouse_event(Type::right_button_up, LogicalPoint{80, 90}), view)
        .has_value(), "right drag created a navigation target");
check(!input.camera_dragging(), "right up left camera drag active");
```

再覆盖：左键移动后松开仍不产生点击且不再提供相机拖动；右键期间左键松开不产生目标；UI 右键不拖动；`input_cancelled` 同时清除点击候选与右键状态。将 `InputState` 测试改为只验证键盘及取消，不再要求它跟踪鼠标。

**Step 2: 验证 RED**

```bash
cmake --build client/build --config Release --target dhsh_test_navigation dhsh_test_viewport
ctest --test-dir client/build -C Release -R '^(navigation|viewport)$' --output-on-failure
```

预期：新 API/事件行为缺失导致失败。

**Step 3: 最小实现**

`MapInput` 分离 `left_pressed_`、`left_dragged_`、`right_dragging_`，提供 `camera_dragging()`。左键移动只负责取消点击候选；右键移动不产生目标。右键拖动期间忽略左键点击候选，取消事件清空全部状态。删除 `InputState` 的重复鼠标拖动字段和接口，保留键盘行为。

App 在 `right_button_down` 且 `camera_dragging()` 时记录 `drag_anchor_x/y = camera + point/zoom`；仅右键 `mouse_move` 更新相机并挂起跟随；`right_button_up` 不改坐标。有效左键目标仍调用同一 `movement.set_destination`，不中断或重置角色路径以外的状态。

**Step 4: 验证 GREEN**

重复 Step 2；再运行 `ctest --test-dir client/build -C Release -R '^(navigation|viewport|m1_app_acceptance)$' --output-on-failure`，预期全部 PASS。

**Step 5: 提交**

```bash
git add client/src/input client/src/app client/tests/test_navigation.cpp client/tests/test_viewport.cpp
git commit -m "fix(client): separate click movement from camera drag"
```

---

### Task 3: 平滑恢复相机跟随

**Files:**
- Modify: `client/src/app/App.h`
- Modify: `client/src/app/App.cpp`
- Test: `client/tests/test_viewport.cpp`
- Modify: `tools/client_m1_acceptance.py`

**Step 1: 写失败测试**

为公开纯函数添加用例：

```cpp
const double first = dhsh::approach_camera_coordinate(100.0, 500.0, 16.0);
check(first > 100.0 && first < 500.0, "follow resumed with a camera jump");
check(dhsh::approach_camera_coordinate(100.0, 500.0, 0.0) == 100.0,
    "zero-duration frame moved camera");
check(dhsh::approach_camera_coordinate(499.999, 500.0, 16.0) == 500.0,
    "camera did not converge to its target");
```

窗口驱动增加真实序列：启动有效寻路，右键拖动后断言相机/角色坐标均符合预期，右键松开一帧内不跳变，再次有效点击后采样标题，确认相机逐步趋近而非一次回中。

**Step 2: 验证 RED**

```bash
cmake --build client/build --config Release --target dhsh_test_viewport
ctest --test-dir client/build -C Release -R '^viewport$' --output-on-failure
```

预期：平滑函数缺失或当前跟随仍立即居中导致失败。

**Step 3: 最小实现**

实现 `weight = 1 - exp(-elapsed_ms/120)` 的逐帧趋近，非有限/非正时间步返回当前值，差值足够小时返回目标。App 的跟随分支分别计算目标 x/y、边界截断，再调用该函数；角色 `movement.advance` 不因右键拖动暂停。

**Step 4: 验证 GREEN**

运行 viewport 与真实 App 驱动；预期单元测试和新增窗口序列 PASS，旧 M1 寻路/阻挡/退出仍 PASS。

**Step 5: 提交**

```bash
git add client/src/app client/tests/test_viewport.cpp tools/client_m1_acceptance.py
git commit -m "fix(client): smooth camera follow after drag"
```

---

### Task 4: 按原版 16 位语义修正 core 遮挡

**Files:**
- Modify: `client/src/scene/CoverMask.cpp`
- Test: `client/tests/test_scene_layers.cpp`
- Modify: `client/tests/test_scene.cpp`
- Modify: `client/tests/test_m1_acceptance.cpp`
- Modify: `docs/formats/map.md`

**Step 1: 写独立参考失败测试**

在 `test_scene_layers.cpp` 用独立辅助函数计算 RGB565 往返和 `(int16_t(pixel) >> 3) & 0x1f1f`，至少断言 `(80,72,64)`、`(120,117,109)`、`(0,128,255)` 和不透明/半透明 alpha。参考结果必须不同于旧逐通道 `>>3`，并确认原 `test_core_depth_and_rle_ghost_darkening` 因颜色断言失败。保留深度边界、矩形相交和未覆盖像素断言。

**Step 2: 验证 RED**

```bash
cmake --build client/build --config Release --target dhsh_test_scene_layers dhsh_test_scene dhsh_test_m1_acceptance
ctest --test-dir client/build -C Release -R '^(scene_layers|scene|m1_acceptance)$' --output-on-failure
```

预期：新像素用例及旧错误暗化断言明确显示 RGB 差异；不得通过放宽相等检查处理。

**Step 3: 最小实现**

仅在 `CoverMask.cpp` 增加内部 RGB565 量化、算术位移和还原辅助逻辑，替换 `previous >> 3`。alpha 不改，掩码、深度、候选缓存和裁剪逻辑不改。将旧测试和诊断文字从“逐通道右移”更新为“打包 16 位像素位移”，`docs/formats/map.md` 补充原生 RGBA 适配规则。

**Step 4: 验证 GREEN**

重复 Step 2；预期全部 PASS，包括真实 PAK core 遮挡验收。运行 `scene_ying_tian_fu_static/animation` 确认场景没有回归。

**Step 5: 提交**

```bash
git add client/src/scene/CoverMask.cpp client/tests/test_scene_layers.cpp client/tests/test_scene.cpp client/tests/test_m1_acceptance.cpp docs/formats/map.md
git commit -m "fix(client): match packed16 core occlusion"
```

---

### Task 5: 整体验证与交接

**Files:**
- Modify: `client/README.md`
- Create: `docs/VERIFY-INPUT-OCCLUSION.md`

**Step 1: 独立审查**

让 Claude CLI/DeepSeek 只读审查输入状态、Win32 捕获、相机数值稳定性和 16 位转换；主代理复核所有发现，不直接接受未经验证的建议。

**Step 2: 完整自动验收**

```bash
cmake --build client/build --config Release
ctest --test-dir client/build -C Release --output-on-failure
```

预期：27/27 或新增后的全部测试 PASS，0 failed，两项 M1 均非 SKIP。

**Step 3: 真实场景验收**

运行原生客户端，覆盖长距离点击、移动中右键拖动、松开、再次点击、UI 区域、滚轮和失焦；对比树木与建筑遮挡，记录前后截图和 `Frame stats`，确认无黑色错误轮廓、无相机瞬跳且渲染预算不回归。

**Step 4: 文档与提交**

在 README 写明左键移动/右键拖动，验收文档记录命令、测试数、真实截图与剩余限制，只提交相关文件：

```bash
git add client/README.md docs/VERIFY-INPUT-OCCLUSION.md
git commit -m "docs: verify input and occlusion fixes"
```
