# REA 状态检查

- 检查时间：2026-10-19
- 检查对象：REA 6.7.0（`rea-agents@6.7.0`）

## 结论

| 项目 | 状态 |
|---|---|
| REA MCP 注册（Claude Code） | ✅ `C:\Users\Administrator\.claude.json` 中已配置，`claude mcp list` 显示 `rea` 已连接 |
| 本会话的 REA MCP 工具 | ✅ 已加载；`binary_session` 显示 207 个工具可用，`open_binary` 等原生工具在打开目标后可用 |
| REA CLI | ✅ `npx -y rea-agents@6.7.0` 可用（153 个命令） |
| Node.js | ✅ v24.19.0 |
| REA skill | ✅ v42，与 CLI 6.7.0 对齐 |
| Ghidra | ✅ 已安装：`D:\software\ghidra_12.1.2_PUBLIC`；`GHIDRA_INSTALL_DIR` 已写入 MCP `env`，`binary_session` 显示 ghidra provider available（12.1.2，Java 25.0.2） |
| Hopper / IDA / JADX / Kuna | ❌ 未配置（Windows PE 分析用 Ghidra 即可，不影响） |

`doctor` 摘要：`healthy: true`，`environment_healthy: false`（原因是缺少 Hopper/IDA 等引擎）。
`binary_session` 已确认活动 MCP server：`rea-agents` 6.7.0，启动于 2026-10-19T10:16:09Z，
Ghidra provider 候选为 available，install_dir 解析正确。

## 已完成的实测

设置 `GHIDRA_INSTALL_DIR` 后，REA 能正常调用 Ghidra 12.1.2：

```bash
GHIDRA_INSTALL_DIR="D:/software/ghidra_12.1.2_PUBLIC" \
npx -y rea-agents@6.7.0 inspect "D:/software/dhsh/launch.exe" \
  --provider ghidra \
  --snapshot "D:/ai-work-flow/rea-dhsh/.rea/launch" \
  --format json
```

结果：

- 成功返回 Evidence（`evidence_id: ev_3ad053bf286ff2e0c3e1453d4cdab6917d5a8573645076060457382b46be3bbe`）
- Provider：Ghidra 12.1.2
- `launch.exe`：x86 PE，158 个过程，124 个字符串，6 个段
- 输出：`.rea/launch-inspect.json`

> 注意：REA 结构化输出不能与 `--token-limit/--token-offset` 同时使用；
> 需要控制大小时改用 `--filter-output`、分页或 `inspect-analysis-view`。

## 让 REA MCP 工具在本会话可用

当前 MCP 进程已带上 `GHIDRA_INSTALL_DIR`，REA 工具在本会话可用。**已完成**：

- 在 `C:\Users\Administrator\.claude.json` 的 `mcpServers.rea` 中加入：

```json
"env": {
  "GHIDRA_INSTALL_DIR": "D:\\software\\ghidra_12.1.2_PUBLIC"
}
```

- 修改前备份：`C:\Users\Administrator\.claude.json.bak-rea-20261019-181512`
- `claude mcp get rea` 显示 User config / Connected。
- 活动 MCP server 已自动重连并识别 Ghidra；无需额外重启即可使用。

**MCP 实测**：

- `open_binary(D:\software\dhsh\launch.exe, provider=ghidra)` → 成功
- `binary_overview` → 158 个过程、124 个字符串、6 个段，
  Evidence `ev_3ad053bf286ff2e0c3e1453d4cdab6917d5a8573645076060457382b46be3bbe`
- `close_binary` → 已清理会话

如果后续客户端重启导致配置被覆盖，可重新执行上述 `env` 配置，或改用
`claude mcp remove rea -s user` + 带 `-e GHIDRA_INSTALL_DIR=...` 的 `claude mcp add`。

CLI 方式仍可备用（带 inline `GHIDRA_INSTALL_DIR`）。

## 对 dhsh 分析的意义

- 原生 PE（`Game.exe`、`Main.exe`、`NEP2.dll` 等）可以用 Ghidra 做静态分析，
  用于定位 PAKF/NZ/APG 读取与解压函数；不需要 IDA/Hopper。
- 资源格式解析仍以 Python 工具链为主；REA 负责原生代码证据与函数级分析。
- 建议把 Ghidra 分析快照放在 `.rea/` 下，并把关键 Evidence ID 记录到
  `docs/formats/*.md`。
