# 输入、遮挡与打包验证

日期：2026-11-11

## 范围

本次修正原生离线 M1 的左键寻路/右键拖动冲突、相机恢复跟随跳变、core 遮挡像素格式，以及 Windows 打包。不是完整游戏客户端或联机功能验收。

资源只读使用 `D:/software/dhsh/shdata`。运行中的新版游戏仅作视觉参照；未注入、未修改原资源，也未把旧基线地址套用到新版进程。

## 实现

- 左键只产生点击寻路，右键按住才拖动地图；UI、黑边、取消和左右键交错有独立状态与捕获处理。
- 右键拖动期间角色继续移动，松开保持相机；下一次有效路径通过逐帧指数趋近恢复跟随。
- core 覆盖像素按 RGB565 量化、16 位有符号 `>>3`、`0x1f1f`、还原 RGBA 的原版算法处理，alpha 不变。
- `tools/package_client.py` 生成 EXE、README、启动脚本、地图缓存和 ZIP；已有输出默认拒绝覆盖。

## 自动验证

工作树：`.worktrees/input-occlusion-fix`，Win32 Release。

```text
cmake --build client/build --config Release
ctest --test-dir client/build -C Release --output-on-failure
27/27 PASS，0 failed，166.93 秒
```

两项 M1 均为真实 Passed，不是 SKIP。`m1_app_acceptance` 额外验证移动中右键拖动：

- 角色继续移动：true；
- 右键松开相机稳定：true；
- 再次寻路后的跟随误差由 74 降为 31；
- 最终收敛到相机 `(6567,1836)`；
- 正常退出：0；
- 40 个性能统计窗口均通过诊断预算。

打包测试：

```text
D:/software/anaconda3/python.exe tools/test_package_client.py
2 tests PASS
```

实际生成：

- `client/dist/dhsh-client/`
- `client/dist/dhsh-client.zip`

打包后的 EXE 使用包内地图缓存完成真实启动和烟测：`WINDOW_SMOKE PASS exit=0`。25 秒采样用于覆盖约 10 秒启动时间；启动后各渲染窗口平均约 1.7–2.0 ms，均低于 16.67 ms 预算。较短采样会在启动完成前结束，不能据此判断帧循环故障。

## 视觉验证

真实客户端移动至地图 cell `(424,117)`、像素 `(6792,1880)`，截图：

`client/build/occlusion-424-117.png`

柱子和屋檐区域的角色显示保留颜色的 core ghost，不再出现此前 RGBA 逐通道右移造成的全黑轮廓；角色与 MAN 深度顺序保持。该次移动中两个统计窗口的 `render_avg_ms=1.87`，均为 PASS。

树木、柱子和建筑共用同一 core 像素路径，并有独立 16 位参考像素测试与真实 PAK 验收；本次没有把不同角色、不同坐标的 `mapdeep.png` 左右区域当作逐像素相等样本。

## 边界

- 平滑跟随是本次明确选择的原生交互行为，不声称已逐帧复刻原版相机。
- 遮挡颜色按旧基线已确认的打包 16 位指令复刻；新版运行游戏地址变化，不使用旧 VA 读取其内存。
- 包仍需要用户提供的原 `shdata`；不包含原 PAK，也不提供联机能力。
