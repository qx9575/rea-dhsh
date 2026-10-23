# CLAUDE.md

本文件为 Claude Code 在 `rea-dhsh` 分析工作区中提供指导。

## 项目定位

本目录是《新水浒Q传》（dhsh）客户端资源的逆向分析工作区，目标是识别、提取并解析 `D:\software\dhsh` 下的游戏资源。

- **分析对象（只读）**：`D:\software\dhsh`（约 9.5 GiB）
- **禁止**修改、删除或重命名该目录下的任何文件；所有解包/转码输出写入本目录。
- 本目录约定：
  - `docs/PLAN.md` — 由简入难的阶段计划（S0–S10）
  - `docs/PROGRESS.md` — 哪些资源已导出/已解析、哪些仍被 codec 或格式阻塞
  - `docs/ROADMAP.md` — 技术路线与原理说明
  - `docs/REA-STATUS.md` — REA/Ghidra 环境检查结果与启用方式
  - `pak_inventory.json` — 已生成的全量 PAK 叶子资源索引（523,213 条，含 189 个 size=0，约 87 MB）
  - `tools/dhsh/` — PAK 解析、NZ 统计、按类型/扩展名导出 CLI（已可用）
  - `extracted/` — 已导出资源（当前为非 APG 的 type=0 全量 + 样本）
  - `manifests/` — NZ 统计、导出清单（CSV/JSON）
  - `notes/` — 格式笔记（待建立）

## 执行安全规则（重要）

资源规模为 523,213 条索引（非空 523,024）、约 9 GiB。**禁止一次性全量解包、解码、解析或转码**；一旦逻辑有误，会把错误放大成数小时的重复工作。必须遵循：

1. **样本先行**：新格式/新 codec 先取 10–50 个代表性样本，人工/自动验证后再扩大。
2. **小批量递进**：10 → 100 → 1,000 → 按 PAK/类别全量；每批保留独立 manifest 与日志。
3. **按维度分批**：一次只处理一个 PAK、一个扩展名或一个 NZ type；使用 `--pak`、`--exts`、`--types`。
4. **强制限额**：批量命令必须带 `--per-pak-limit` / `--limit` / `--max-size`；先跑 `--dry-run`。
   `extract` 在没有过滤条件时会拒绝执行（防止误导出 52 万文件）。
5. **自动校验**：每批检查数量、输出大小、magic/图片可打开/文本可解码/codec 输出长度；
   失败率超过阈值（建议 1%）立即停止并修正。
6. **可恢复**：批次输出与 manifest 分开命名；重复运行依赖“已存在则跳过”，不要覆盖上一批结果。
7. **长任务**：后台运行并写日志，分批检查；不要在前台一次跑完。
8. **先验证再合并**：确认批次正确后，才把结果合并到正式目录或继续下一批。

## 客户端标识（已确认）

| 项 | 值 |
|---|---|
| 游戏 | 新水浒Q传（内部产品名亦作“大话水浒”） |
| 引擎 | Hero2D（`Game.exe` 版本信息：Hero2D Application） |
| 运营 | 搜狐畅游 / 火石软件 |
| 客户端版本 | 1.50.22（`shdata/conf.ini`、`shdata/ver.csv`；2026-10-21 更新） |
| 官网 | http://dhsh.changyou.com |
| 更新服 | update.dhsh.changyou.com |
| 最近更新公告 | 2026-10-21（`shdata/save/update.txt`，版本 1.50.22） |

可执行文件角色（来自版本信息/文件名观测）：

| 文件 | 角色 |
|---|---|
| `launch.exe` | 启动器（“启动游戏程序”） |
| `shdata/Game.exe` | 主客户端，Hero2D Application，产品名“新水浒Q传客户端” |
| `shdata/Main.exe` | 产品名“大话水浒” v1.2.0.6 |
| `shdata/LaunchHall.exe` / `LaunchHallSk.exe` | 水浒启动检查程序 |
| `shdata/gengxinhero.exe` | 自动更新程序 v3.0.0.3 |
| `shdata/CProtect.exe` / `CProtect1.exe` | CYOU Game Protector 反外挂 |
| `shdata/NEP2.dll`、`NEPDaemon.exe`、`CyouProtect64.sys`、`dhsh.sys`、`dhsken.sys`、`grendsken.sys`、`MemDog.dll` | 保护/反调试组件（推断） |
| `shdata/necrashpad*.dll/exe`、`bugrpt.exe`、`KernelDumpAnalyzer.exe` | 崩溃上报（推断） |

## 目录总览

### 安装根目录 `D:\software\dhsh`

| 条目 | 大小 | 说明 |
|---|---:|---|
| `launch.exe` | 78 KB | 启动器 |
| `uninst.exe` | 724 KB | 卸载程序（产品名“大话水浒” v1.50.18.1） |
| `VMProtectSDK32.dll` | 62 KB | VMProtect SDK 运行库 |
| `CYFlag.flag` | 40 B | 畅游平台标记文件 |
| `shdata/` | 9.5 GiB | 全部游戏数据 |

### `shdata/` 一级目录

| 目录/文件 | 大小 | 内容 |
|---|---:|---|
| `*.pak`（共 45 个） | 约 9.0 GiB | 资源归档：41 个顶层（40 个 PAKF + `perinfo.pak` APCI），另有 4 个更新补丁包（`update/_tmp`、`update/update`） |
| `sound/` | 128 MB | 435 个 `.wav` + 21 个 `.mp3`，数字 ID 命名（如 `18xxx.wav`、`20120220.mp3`） |
| `update/` | 138 MB | 更新器、`skin/hero1` 更新界面 BMP（41 个文件），`_tmp/` 待应用补丁（含 `Game.exe`、`NEP2.dll`、`ui.pak`、`config.pak`） |
| `font/` | 35 MB | `simsun.ttc`、`SIMYOU.TTF`、`STFANGSO.TTF`、`STXINGKA.TTF`、`ARKai_T.ttf` + `fontinfo.ini` |
| `animation/` | 25 MB | `1/2/3.apg` + 对应 `.dat`（PIF）+ `1.mp3`，推测为开场/登录动画 |
| `save/` | 202 KB | 本地设置 `config.ini`、服务器列表 `svrlist.cfg`、`zoneinfo/*.cfg`、协议/公告文本 |
| `screen/` | 0 | 空目录（运行时截图输出位） |
| `userpic/` | 0 | 空目录（运行时头像输出位） |

`shdata/` 根目录还包含：主程序 `Game.exe`、`Main.exe`、更新器、反外挂/崩溃上报组件、FFmpeg 运行库（`avcodec-52.dll`、`avformat-52.dll` 等）、`fmodex.dll`、配置 `conf.ini`/`ver.csv`，以及若干未解析数据块（`001.cpm`、`ccore.dat`、`cltgp.dat`、`ACDC.dat`(gzip)、`sl.dat`、`rdm.bin`(PE)、`EncryptPara.dll`(PE)、`cltax.xmf` 等）。

### PAK 资源分类

| 类别 | 归档 | 大小 | 叶子资源数 | 主要内容 |
|---|---|---:|---:|---|
| 角色 | `player.pak` + `player1-15.pak` | 4870 MB | 445,999 | `body/head/cloth/weapon/mask/horse` 等 `.apg` 动画 |
| NPC | `npc.pak` + `npc1-9.pak` | 1998 MB | 30,915 | 按 NPC ID 分目录的 `.apg` |
| 技能/特效 | `magic.pak` + `magic1-5.pak` | 1187 MB | 2,695 | 技能特效 `.apg` |
| 地图 | `map.pak`、`map1.pak` | 642 MB | 5,893 | `.map/.man/.cel/.core/.msk` + 地图动画 `.apg` + 背景图 |
| UI/配置/图标 | `ui.pak` | 486 MB | 33,502 | `icon/`、`user/head`、`home/`、`activity/` 等 |
| 配置/数据 | `config.pak` | 10 MB | 2,470 | `.csv/.scp/.ini/.lua` 及嵌套 UI 配置 |
| 头像信息 | `perinfo.pak` | 7.4 MB | 1 | `APCI` 容器（1 个 PAK 级条目；219 条 record、687,229 个 item；已解析） |
| UI 背景 | `cmbgui.pak` | 4.2 MB | 658 | `.tga/.a16/.ini` |
| 插件/UI | `plugins.pak` | 2.6 MB | 165 | `.lua/.scp/.tga` |
| 表情动画 | `emotions.pak` | 0.7 MB | 226 | `.apg/.a16` |
| UI 脚本 | `800config.pak` | 0.5 MB | 689 | `.scp/.txt/.csv` |

全量索引条目 523,213（2026-10-21 更新后重建；含 189 个 size=0 条目，
非空叶子 523,024；`perinfo.pak` 以 1 个 PAK 级条目计入）：

`.apg` 458,997；`.a16` 29,716；`.cti` 18,387；`.tga` 6,549；`.dat` 4,642；`.scp` 1,940；`.jpg` 544；`.csv` 457；`.map` 305；`.man` 277；`.cel` 275；`.core` 274；`.ini` 231；`.mlk` 228；`.pg2` 150；`.bmp` 84；`.lua` 46；无扩展名 45；`.msk` 26；`.txt` 22；`.psd` 13；`.pak` 1；其余少量（`.zip/.db/.gal` 等）。

## 已确认格式

### 1) PAKF3001 归档（已验证）

小端序，结构如下：

```c
struct PakHeader {          // 16 bytes
    char     magic[8];      // "PAKF3001"
    uint32_t checksum;      // 作用未知（每包不同）
    uint32_t entry_count;   // 索引条目数
};

struct PakEntry {           // 48 bytes
    char     name[32];      // ASCII 文件名，NUL 填充
    uint32_t offset;        // 相对于该归档起始位置的偏移
    uint32_t size;          // 数据大小
    uint32_t type;          // 0 = 原始文件；1 = 嵌套 PAKF3001 归档
    uint32_t reserved;      // 0
};
// 数据区起点 = 0x10 + entry_count * 48
```

- 索引紧跟 16 字节头；第 i 条数据的绝对位置 = `归档基址 + offset`。
- `type=1` 的条目数据仍是 PAKF3001，可递归解析；已观察到 3–4 层嵌套（如 `ui.pak/item/...`）。
- `type=0` 的条目是叶子资源；其数据通常再包一层 NZ（见下）。
- 40 个顶层 PAKF 归档解析后共 523,213 条索引条目（含 189 个 size=0；非空 523,024），索引已写入 `pak_inventory.json`；`perinfo.pak` 以 1 个 PAK 级条目计入该数量。
- 解析注意：`offset` 相对当前归档基址，不是整个文件的绝对偏移；递归时必须传递新的 base。

### 2) NZ 资源封装（大部分叶子资源）

```
0x00  2  "NZ"
0x02  2  type      // 0x0000 / 0x000A / 0x000B ...
0x04  4  size1     // 原始/逻辑大小
0x08  4  size2     // 存储大小（payload 字节数）
0x0C  N  payload
```

- `type=0x0000`：payload 原样存储，`size1 == size2`，payload 以真实文件 magic 开头（如 `PK`、`PG`、`CT`、`MSK`，或纯文本 INI/CSV）。
- `type=0x000B`：**LZO1X 压缩，已破解**（Game.exe VA `0x8a6c90`；实现 `tools/dhsh/nz_lzo.py`）。样本 133/133、B1 100/100 通过。
- `type=0x000A`：16 位 LZW 压缩，**已破解**（Game.exe VA `0x8a6f60`；实现 `tools/dhsh/nz_lzw10.py`），占 59,222 个叶子，已全量导出。
- 文本编码（已验证）：CSV/脚本中的 ASCII 按 UTF-16LE 存储；中文的 GBK 双字节被逐字节扩成 16 位（每个 16 位单元的低字节依次组成 GBK 串）。解析时应取每个 16 位字的低字节再按 GBK 解码，并结合 `size1` 截断。
- 详见 `docs/formats/nz.md`。

### 3) 叶子格式观察（部分）

| 扩展名 | 数量 | 观察/推断 |
|---|---:|---|
| `.apg` | 458,997 | Hero2D 动画包。`AG` 头 + `AP`/`AI` 图像块；`AP`=RGB565/JPEG，`AI`=RLE8 索引+CTI；B2 134 动画/1,000 帧全部解码；metadata/blend 待解；详见 `docs/formats/apg.md` |
| `.a16` | 29,716 | 16 位位图。头 `AP`，RGB565 + RLE16 + RLE8 alpha；解析器已实现（`tools/dhsh/formats/a16.py`），B1 100/100；详见 `docs/formats/a16-cti.md` |
| `.cti` | 18,387 | 调色板：20 字节头 + 256×u16 + 256×u32；头部已验证，table 语义待定 |
| `.tga` | 6,549 | TGA 纹理（UI/插件） |
| `.dat` | 4,642 | PIF 容器：20 字节头 + width×height 字节；type 0/0x0B 已解析（`tools/dhsh/formats/pif.py`）；width=帧数或同目录 APG 动作数；字段语义待定；详见 `docs/formats/pif.md` |
| `.scp` | 1,940 | UI 脚本/布局；已解析为 JSON（WINDOW/FIXEDPART 树、BGPIC、include 图）；详见 `docs/formats/scp.md` |
| `.map` | 305 | 纯文本 INI（`WIDTH`/`HEIGHT`/`BACKGROUND` 等） |
| `.man`/`.cel`/`.core` | 277/275/274 | `MAN` 动画路径 / `CEL` 碰撞位图 / `MSK` 容器前景遮罩；`.core` 是活动格式，遮罩 RLE 已破解并全量导出；详见 `docs/formats/map.md` |
| `.mlk` | 228 | 地图跳转/链接脚本，payload 以 `lnk` 开头 |
| `.pg2` | 150 | PG 图片容器（大图），payload 以 `PG` 开头 |
| `.csv/.ini/.lua/.txt` | 457/231/46/22 | 配置与脚本 |
| `.msk` | 26 | 旧版地图遮罩（同名 `.core` 才是当前客户端加载的格式）；`MSK` 头 + 名称 + RLE 遮罩，已解析 |

### 4) 其他容器

- `perinfo.pak`：`APCI` 头（非 PAKF），头像/装备图集索引；布局为
  group(u16 id + u16 count) → record(36B) → entry(8B) → mid(4B) →
  item(u32 x/u32 y + 可选 blob)；已完整解析并导出（`docs/formats/apci.md`）。
- `animation/*.dat`：`PIF` 头，与同名 `.apg` 配对，记录帧尺寸/锚点等元数据。
- `ACDC.dat`：gzip 流（`1F 8B`）。
- `rdm.bin`、`EncryptPara.dll`：PE 文件（MZ）。
- `001.cpm`、`ccore.dat`、`cltgp.dat`、`sl.dat`：高熵/加密数据块，待定位用途。

## 环境与工具约定

- 工作目录：`D:\ai-work-flow\rea-dhsh`；平台 Windows 10，shell 为 Git Bash。
- 当前客户端 1.50.22，`shdata/Game.exe` 当前 SHA-256 `7a5e2c08…`；
  旧分析基线为 `f6bf66d2…`。**注意：Game.exe 可能在每次运行后被启动器/
  保护组件改写**，磁盘 SHA 不是稳定基线；运行后先执行
  `python tools/snapshot_gameexe.py`，以 `extracted/gameexe-snapshots/`
  中的 pinned 副本和差异报告为准，再判断静态地址是否仍可用。
  静态地址迁移与 overclaim 修正见 `docs/VERIFY-2026-10-23.md`。
- Python 3.13 可用，但命令名是 `python`；`python3` 是 Windows Store 存根（exit 49），不要使用。
- 控制台默认 GBK，输出中文时用 `PYTHONIOENCODING=utf-8`，避免乱码；外部 `.ini/.cfg/.txt` 多为 GBK，读取时用 `iconv -f GBK -t UTF-8`。
- 已生成 `pak_inventory.json`：按 pak 分组的叶子资源列表，字段 `name/path/offset/size/depth/head`（`offset` 为文件内绝对偏移，`head` 为前 16 字节 hex）。
- 工具：`tools/dhsh/nz_lzo.py`（NZ 0x0B LZO1X）、`tools/dhsh/nz_lzw10.py`（NZ 0x0A）、`tools/dhsh/formats/a16.py`（A16/CTI）、`tools/dhsh/formats/apg.py`（APG）、`tools/dhsh/formats/pif.py`（PIF）、`tools/dhsh/formats/pg2.py`（PG2）、`tools/dhsh/formats/map.py`（地图侧，含 MSK/core 遮罩 RLE 解码）、`tools/dhsh/formats/scp.py`（SCP UI）、`tools/dhsh/formats/apci.py`（APCI）、`tools/dhsh/formats/special_jpg.py`（特殊 JPG）；导出器 `tools/export_*.py`（含 `export_core_masks.py`）；更新重导 `tools/reexport_changed.py`；Game.exe 漂移快照 `tools/snapshot_gameexe.py`。
- Python 依赖：Pillow 11.1.0（图像验证/导出）、Capstone 5.0.7（定点反汇编）；numpy 2.1.3 + numba 0.61.0 为可选依赖，用于 0x0A LZW 的 JIT 加速（缺失时自动回退纯 Python）。
- 格式文档：`docs/formats/nz.md`、`docs/formats/a16-cti.md`、`docs/formats/apg.md`、`docs/formats/pif.md`、`docs/formats/pg2.md`、`docs/formats/map.md`、`docs/formats/scp.md`、`docs/formats/apci.md`、`docs/formats/special-jpg.md`。
- REA 6.7.0 已注册到 Claude Code（`C:\Users\Administrator\.claude.json`），`claude mcp list`
  显示 `rea` 已连接；MCP 配置含 `env.GHIDRA_INSTALL_DIR`，本会话已加载 REA 工具。
  已实测 `open_binary` / `binary_overview` / `close_binary`（launch.exe，Ghidra 12.1.2）。
  详见 `docs/REA-STATUS.md`。
- Ghidra 12.1.2 已安装于 `D:\software\ghidra_12.1.2_PUBLIC`；REA 原生分析可直接使用，
  会话快照放在 `.rea/`。CLI 备用方式：
  `GHIDRA_INSTALL_DIR="D:/software/ghidra_12.1.2_PUBLIC" npx -y rea-agents@6.7.0 <command>`。
- 当前 SHA 的 pinned Ghidra 项目：`ghidra-proj/dhsh-7a5e`（程序
  `Game-7a5e…exe`，由 `analyzeHeadless` 导入快照）。批量查询：
  `GHIDRA_PROJ_NAME=dhsh-7a5e GHIDRA_PROGRAM=Game-7a5e…exe python tools/kimi_ghidra_batch.py cmd.json`
  （Git Bash 下需 `MSYS_NO_PATHCONV=1`，程序名不要带前导 `/`）。

## Git 管理

- 远程：`git@github.com:qx9575/rea-dhsh.git`（`origin`，分支 `main`）
- SSH key：`C:/Users/Administrator/.ssh/id_rsa_qx/id_rsa`，已写入本仓库本地配置：

```text
core.sshCommand = ssh -i C:/Users/Administrator/.ssh/id_rsa_qx/id_rsa -o IdentitiesOnly=yes -o BatchMode=yes
```

- 已忽略（不提交）：`extracted/`、`pak_inventory.json`、`.rea/`、`.claude/`、`__pycache__/`、`*.log`
- 提交内容：`docs/`、`tools/`、`manifests/`、`CLAUDE.md`、`.gitignore`、`.vscode/settings.json`
- 常用命令：`git status` → `git add .` → `git commit -m "..."` → `git push`
- 不要把 `D:\software\dhsh` 原始资源或 `extracted/` 大文件加入 Git；大文件需要版本化时使用 Git LFS 或外部存储。

## 下一步建议

1. **MSK/core 消费端语义**（f2/f6 地线遮挡排序、flag/group/frame 动画播放）；MAN/MLK 与地图渲染关联。
2. **加密/未知数据块**：`ACDC.dat`(gzip)、`ccore.dat`、`cltgp.dat`、`sl.dat`、`001.cpm`。
3. 语义深化：AG offset、CTI table2、混合模式、Lua/CSV、SCP 消息绑定、APCI 字段/blob。
4. 非 PAK 资源索引：`sound/`（456）、`font/`、`animation/`、`update/skin/`。
5. 资源查看器（TGA/PG2/A16/APG 自动匹配，单文件/目录浏览）——滞后。

## 重要约束

- 分析结论必须区分“观察到的字节/结构”与“推断”；不要把猜测写成格式定论。
- 不要运行游戏或其保护组件；静态分析优先。
- `D:\software\dhsh` 为只读目标，任何写操作限制在 `D:\ai-work-flow\rea-dhsh`。
