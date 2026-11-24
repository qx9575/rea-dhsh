# 在线角色选择与图形验证码 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 让复刻客户端独立走完账号认证、角色列表选择、图形验证码、`0x1002` 角色选择和 `0x2002` 重定向，再进入既有限定在线聊天。

**Architecture:** `GameSession` 负责有状态网关时序，`OnlineChatWorker` 负责后台协议与 UI 事件桥接，Win32 主线程负责角色选择和图片验证码对话框。初次对话框不再接收角色字段；角色与图片只在对应服务器帧到达后展示，socket 始终不进入 UI 线程。

**Tech Stack:** C++17、Win32、现有 `GameSession`/`OnlineChatWorker`、CTest loopback fixtures、Python `.rea` 抓包分析工具。

---

设计依据：`docs/plans/2026-11-24-online-role-graphkey-design.md`。当前 HEAD 为 `17ad752`；另有接收超时修复待独立提交。禁止 merge/rebase/push，禁止把真实账号、动态畅游码、图形码、聊天正文或抓包写入提交。

### Task 0: 提交已验证的接收超时修复

**Files:**
- Modify: `client/src/network/Win32TcpStream.cpp`
- Modify: `client/src/network/GameSession.cpp`
- Modify: `client/src/network/GameSession.h`
- Modify: `client/src/network/OnlineChatWorker.h`
- Test: `client/tests/test_game_session.cpp`

**Step 1: 主 Agent 核对暂存 diff**

确认只包含长度头进度记账、partial-frame timeout 终端处理、脱敏定位元数据、1 秒有界接收期限及回归测试，不包含 HUD/NPC WIP。

**Step 2: 运行回归**

```bash
"D:/software/CMake/bin/ctest.exe" --test-dir client/build -C Debug -R '^(online_credentials|game_session|game_crypto|chat|protocol_replay|scp_ui|smoke)$' --output-on-failure
"D:/software/CMake/bin/ctest.exe" --test-dir client/build -C Release -R '^(online_credentials|game_session|game_crypto|chat|protocol_replay|scp_ui|smoke)$' --output-on-failure
```

Expected: 两个配置均 7/7 通过。

**Step 3: 独立审查并提交**

由未实现该修复的 coder 审查实际暂存区，主 Agent 验证快照后提交：

```bash
git commit -m "fix(network): preserve framed reads across polling timeouts"
```

### Task 1: 角色记录解析与选择时序

**Files:**
- Modify: `client/src/network/GameSession.h`
- Modify: `client/src/network/GameSession.cpp`
- Modify: `client/src/network/OnlineChatWorker.h`
- Modify: `client/src/network/OnlineChatWorker.cpp`
- Test: `client/tests/test_game_session.cpp`

**Step 1: 写失败的角色记录测试**

使用合成的 14B 头 + 两个 0x85B 角色记录构造 `0x2000`，断言解析出两个不同角色 ID 和 GBK 名称；覆盖空记录、非整记录、空名称和多个 `0x2000` 累计。真实角色记录只放 `.rea`，提交使用 SYNTH 字段。

**Step 2: 运行测试确认 RED**

```bash
"D:/software/CMake/bin/cmake.exe" --build client/build --config Debug --target dhsh_test_game_session
client/build/tests/Debug/dhsh_test_game_session.exe
```

Expected: 角色事件/解析 API 尚不存在或当前 `start()` 提前发送 `0x100b/0x1002` 导致断言失败。

**Step 3: 实现角色记录与显式选择 API**

`GameSession::start()` 仅发送 `0x1005/0x1000/0x1500`。解析每个完整 `0x2000` 中一个或多个 0x85B 记录，通过新 `SessionEvent` 类型向 worker 提供角色 ID/GBK 名称，不向日志输出角色内容。新增仅 worker 可调用的角色选择方法，保存所选记录并发送 `0x100b`；未选择、未知 ID 或重复提交必须 fail closed。

**Step 4: 写并运行角色选择后时序测试**

证明收到角色事件前只有三帧；调用选择后才发送 `0x100b`；收到 `0x2005` 前不得发送 `0x1002`。运行 Debug/Release `game_session`，Expected: PASS。

**Step 5: 审查并提交**

```bash
git commit -m "feat(network): defer role selection until role records arrive"
```

### Task 2: 闭合 `0x2005` 图片解码

**Files:**
- Create: `client/src/network/GraphKeyDecoder.h`
- Create: `client/src/network/GraphKeyDecoder.cpp`
- Create: `client/tests/test_graphkey.cpp`
- Modify: `client/src/CMakeLists.txt`
- Modify: `client/tests/CMakeLists.txt`
- Evidence: `.rea/graphkey_*`（不提交）

**Step 1: 对原版 `0x2005` 做最小静态/抓包分析**

复用 `.rea/task8_ghidra*` 和 `.rea/original_login*_20261124.pcapng`，定位 `graphkey.scp` 图片更新链、`0x2005` 混淆/压缩格式、尺寸与像素通道。保存地址、算法、边界和失败证据到 `.rea/graphkey_decode.json`，不得只凭文件名猜测。

**Step 2: 写脱敏 golden 与失败测试**

用已闭合的合成小图验证像素、尺寸、通道和截断/异常输入；真实图片仅放 `.rea` 做额外验收。先运行 `dhsh_test_graphkey` 确认 RED。

**Step 3: 实现有界解码器**

API 返回结构化的 RGBA 像素、宽、高和脱敏错误；限制分配大小、尺寸和输入长度，不依赖原 EXE/DLL，不写图片内容日志。

**Step 4: 运行双配置测试**

```bash
"D:/software/CMake/bin/ctest.exe" --test-dir client/build -C Debug -R '^(graphkey|game_session)$' --output-on-failure
"D:/software/CMake/bin/ctest.exe" --test-dir client/build -C Release -R '^(graphkey|game_session)$' --output-on-failure
```

Expected: 全部通过，真实 `.rea` 图片与原版截图一致。

**Step 5: 审查并提交**

```bash
git commit -m "feat(network): decode gateway graph-key challenges"
```

### Task 3: 图形码提交完成网关状态机

**Files:**
- Modify: `client/src/network/GameSession.h`
- Modify: `client/src/network/GameSession.cpp`
- Modify: `client/src/network/OnlineChatWorker.h`
- Modify: `client/src/network/OnlineChatWorker.cpp`
- Test: `client/tests/test_game_session.cpp`

**Step 1: 写失败的图形码时序测试**

loopback 在角色选择后发送合成 `0x2005`，断言产生图片事件且尚未发送 `0x1002`；调用图形码提交 API 后验证 41B payload、角色名和最多 7B 输入。覆盖空输入、超长、非 ASCII、重复提交、取消和超时。

**Step 2: 运行确认 RED**

Expected: 当前实现仍提前发送角色帧或没有图片事件/提交 API。

**Step 3: 实现 worker 双向有界命令**

UI 的角色选择和图形码输入只能进入 worker 队列；worker 验证后调用 `GameSession`。新事件仅携带角色显示数据、解码图片和脱敏错误，不携带账号、动态畅游码或其他网关 payload。

**Step 4: 运行完整网络回归**

Debug/Release 执行 `game_session|graphkey|game_crypto|chat|protocol_replay|smoke`，Expected: 全部通过。

**Step 5: 审查并提交**

```bash
git commit -m "feat(network): submit graph-key after user confirmation"
```

### Task 4: Win32 角色与图形码对话框

**Files:**
- Modify: `client/src/platform/OnlineCredentials.h`
- Modify: `client/src/platform/OnlineCredentials.cpp`
- Modify: `client/src/app/App.cpp`
- Test: `client/tests/test_online_credentials.cpp`
- Modify: `client/README.md`
- Modify: `docs/VERIFY-NATIVE-CLIENT-M1.md`

**Step 1: 写失败的非交互结构测试**

初次对话框不再含角色字段；角色对话框支持列表选择；图形码对话框包含图片和最多 7 字符输入。使用模板/控件结构测试，不自动操作真实账号。

**Step 2: 运行确认 RED 后实现**

主线程处理 worker 角色列表和图片事件，以简单 Win32 对话框完成选择/输入；modal 期间 worker 继续接收。取消走统一 stop/join 和非零会话失败路径，不自动选第一个角色、不跳过图片。

**Step 3: 运行回归**

Debug/Release 执行 `online_credentials|game_session|graphkey|chat|protocol_replay|scp_ui|smoke`，再运行 `tools/client_chat_acceptance.py` 和窗口烟测。Expected: 全部通过，默认离线行为不变。

**Step 4: 更新文档并提交**

文档删除角色 ID 手填说明，记录角色列表与图片输入流程、取消/超时语义和尚未完成的限制。

```bash
git commit -m "feat(app): add role and graph-key login dialogs"
```

### Task 5: 真实双客户端验收

**Files:**
- Evidence only: `.rea/online_live_*`
- Modify if verified: `docs/formats/net-protocol.md`
- Modify if verified: `docs/VERIFY-NATIVE-CLIENT-M1.md`

**Step 1: 实网登录**

用户在复刻端完成账号、动态畅游码、角色选择和图形码。确认角色记录、`0x100b`、图片、`0x1002`、`0x2002` 与游戏服建立的顺序；抓包仅限网关和实证游戏服，敏感内容仅留 `.rea`。

**Step 2: 双向聊天**

第二个原客户端接收复刻端唯一测试消息，再向复刻端发送世界和当前频道消息。检查 GBK、频道、主线程 ChatModel、滚动与断线行为，不回显伪造成功。

**Step 3: 最终回归与提交**

独立审查实际 diff，Debug/Release 相关测试全过后提交协议事实与验收状态；任何未闭合项明确保留，不宣称完整在线世界。
