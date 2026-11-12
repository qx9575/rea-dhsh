# 点击光圈 x32dbg 单次采集 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 用一次低频条件断点取得原版左键点击入口的寄存器与调用栈，并自动清理、脱离。

**Architecture:** 离线旧基线提供新版地址签名；x32dbg 通过 `-p PID -cf script` 附加并执行单次断点脚本。脚本命中后记录寄存器和调用栈，清除断点并脱离，游戏继续运行。

**Tech Stack:** x32dbg 2026.05.27、x32dbg 脚本、Python 3、Windows 进程与窗口读取。

---

### Task 1: 准备单次断点脚本

**Files:**
- Create: `tools/x64dbg/click_once.script`
- Reference: `docs/plans/2026-11-12-cursor-click-effect-x64dbg-design.md`

**步骤:**
1. 在 `0x4168d0` 设置断点，条件为 `[esp+4]==0x201`。
2. 命中后使用 `log` 输出 EIP、通用寄存器和栈参数，使用 `printstack` 输出调用栈。
3. 使用 `bc 4168d0` 清除断点，再执行 `detach`。
4. 不加入全量 Trace、硬件页监听或循环断点。

### Task 2: 分离启动调试器并执行

**Files:**
- Use: `tools/x64dbg/release/x32/x32dbg.exe`
- Use: `tools/x64dbg/click_once.script`

**步骤:**
1. 由用户恢复游戏并进入可走地图；外部只读枚举得到 `Game.exe` PID。
2. 用 Windows `DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP` 启动：
   `x32dbg.exe -p <PID> -cf <绝对脚本路径>`，启动端立即退出，不设置调试器运行超时。
3. 用户在地面只点击一次；若未命中，不连续点击。

### Task 3: 验证采集与清理

**Files:**
- Read: x32dbg 日志窗口/保存日志
- Update: `docs/plans/2026-11-11-cursor-click-effect-handoff.md`

**步骤:**
1. 读取命中日志和 `printstack` 输出，确认消息参数为 `0x201`。
2. 核对断点已清除、调试器已脱离、游戏仍可正常显示与操作。
3. 单次结果只作为消息入口证据；结合旧基线静态分析决定下一定点，不宣称已找到光圈。
4. 将实际地址、日志路径、异常与剩余未知写入交接文档。不提交 Git，除非用户另行要求。
