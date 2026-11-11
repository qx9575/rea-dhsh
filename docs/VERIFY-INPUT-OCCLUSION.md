# 输入、遮挡与打包验证

日期：2026-11-11

## 范围

本次修正原生离线 M1 的左键寻路/右键拖动冲突、相机恢复跟随跳变、core 遮挡像素格式，以及 Windows 打包。不是完整游戏客户端或联机功能验收。

资源只读使用 `D:/software/dhsh/shdata`。运行中的新版游戏仅作视觉参照；未注入、未修改原资源，也未把旧基线地址套用到新版进程。

## 实现

- 左键只产生点击寻路，右键按住才拖动地图；UI、黑边、取消和左右键交错有独立状态与捕获处理。
- 右键拖动期间角色继续移动，松开保持相机；下一次有效路径通过逐帧指数趋近恢复跟随。
- core 覆盖像素只修改精灵 **alpha 通道**（`alpha >>= 3`，不透明 255→31），RGB 不变；
  见文末 2026-11-11 晚更正。
- `tools/package_client.py` 生成 EXE、README、启动脚本、地图缓存和 ZIP；已有输出默认拒绝覆盖。

## 自动验证

工作树：`.worktrees/input-occlusion-fix`，Win32 Release。

```text
cmake --build client/build --config Release
ctest --test-dir client/build -C Release --output-on-failure
28/28 PASS，0 failed，291.61 秒
```

两项 M1 均为真实 Passed，不是 SKIP。`m1_app_acceptance` 额外验证移动中右键拖动：

- 角色继续移动：true；
- 右键松开相机稳定：true；
- 再次寻路后的跟随误差由 79 降为 27；
- 最终收敛到相机 `(6567,1836)`；
- 正常退出：0；
- 6 个性能统计窗口均通过诊断预算；
- 右键拖过底部 SCP UI 仍按固定锚点连续移动，不会冻结后跳变。

DeepSeek 初审发现 UI 拖动门控、正常鼠标释放误清键盘、ZIP 覆盖测试缺口。
已分别增加真实 App/Win32 回归和 ZIP 单独存在用例，并将打包测试接入 CTest。
正常鼠标松开不再等同于输入取消；真正失焦或捕获被其他窗口夺走仍会取消输入。
修复后的 DeepSeek 复审结论为 APPROVED，无剩余 Critical/Important。

打包测试：

```text
D:/software/anaconda3/python.exe tools/test_package_client.py
3 tests PASS
```

实际生成：

- `client/dist/dhsh-client/`
- `client/dist/dhsh-client.zip`

打包后的 EXE 使用包内地图缓存完成真实启动和烟测：`WINDOW_SMOKE PASS exit=0`。25 秒采样用于覆盖启动时间；启动后各渲染窗口平均约 1.7–2.3 ms，均低于 16.67 ms 预算。较短采样会在启动完成前结束，不能据此判断帧循环故障。

## 视觉验证

真实客户端移动至地图 cell `(424,117)`、像素 `(6792,1880)`，截图：

`client/build/occlusion-424-117.png`

柱子和屋檐区域的角色显示保留颜色的 core ghost，不再出现此前 RGBA 逐通道右移造成的全黑轮廓；角色与 MAN 深度顺序保持。该次移动中两个统计窗口的 `render_avg_ms=1.87`，均为 PASS。

树木、柱子和建筑共用同一 core 像素路径，并有独立 16 位参考像素测试与真实 PAK 验收；本次没有把不同角色、不同坐标的 `mapdeep.png` 左右区域当作逐像素相等样本。

## 边界

- 平滑跟随是本次明确选择的原生交互行为，不声称已逐帧复刻原版相机。
- 遮挡颜色/透明度按旧基线已确认的指令复刻；新版运行游戏地址变化，不使用旧 VA 读取其内存。
- 包仍需要用户提供的原 `shdata`；不包含原 PAK，也不提供联机能力。

## 更正（2026-11-11 晚）：遮挡是 alpha 淡化，不是 RGB 暗化

`pic/mapdeep.png` 右侧原版参照中，被前景遮住的角色不是变黑，而是半透明
ghost。复核旧基线 `Game.exe` 调用链后确认此前的 RGB565 暗化结论有误：

- 渲染调用点 `0x81ae3b`/`0x81b0c6` 传给 ApplyMask（`0x4266d0`）的第四参数是
  `WAlphaBitmap::GetChannel`，即精灵的 **8 位 alpha 通道**；`0x4267aa-0x4267c3`
  的暂存拷贝大小是 w×h 字节（每像素 1 字节），不是 2 字节 RGB565。
- `0x4264b0` 的 `psraw 3; pand 0x1f1f…` 作用在 16 位 lane 上，但对 8 位 alpha
  逐字节等价：两个相邻 alpha 字节组成的 lane 右移 3 位后，低 5 位恰好分别是
  各自字节的 `>>3`（20 万随机字节对验证 0 差异）。结果：不透明 255→31
  （约 12% 不透明度），RGB 完全不变。
- 原生 `CoverMask::apply` 已改为仅对 RLE covered 像素执行 `alpha >>= 3`；
  单元/接受测试同步改为断言 alpha 淡化与 RGB 不变，并在真实 PAK 上以
  “无玩家目的地图素 + source-over 混合”复算最终像素。
- `docs/formats/map.md` §4.7 已更新；旧设计文档中“RGB565 暗化、alpha 不变”
  的表述属于已被本次更正取代的历史结论。
