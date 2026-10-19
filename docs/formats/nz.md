# NZ 资源封装格式（已破解 0x0A/0x0B）

- 目标：`D:\software\dhsh\shdata\Game.exe`
  SHA-256 `f6bf66d24a3523bb6a30541589fabea36bbceec84355463d8e2c99e3b7f16be9`
- 日期：2026-10-19
- 方法：REA + Ghidra 静态反编译 + Capstone 定点反汇编；未运行游戏、未注入进程。

## 1. 外层结构

大部分 PAK 叶子资源以 NZ 头开始（小端序）：

```
0x00  2  "NZ"
0x02  2  type
0x04  4  size1      # 解压后大小（逻辑大小）
0x08  4  size2      # 存储大小（payload 字节数）
0x0C  N  payload
```

观察到的类型：

| type | 含义 | 状态 |
|---|---|---|
| `0x0000` | 原样存储 | ✅ 已验证 |
| `0x0001`–`0x0009` | 解码器不支持的保留类型 | — |
| `0x000A` | **16 位 LZW**（字典码 0x0101+） | ✅ 已破解（`0x8a6f60`） |
| `0x000B` | **LZO1X** 压缩 | ✅ 已破解（`0x8a6c90`） |

Game.exe 中的通用 NZ 解码器：

- 入口：VA `0x82d1f0`
- 校验 `cmp word ptr [esi], 0x5a4e`（"NZ"）：`0x82d234`
- 读取 type/size1/size2，分配 `size1 + 1` 字节
- 按 type 分派：跳转表 `0x82d494`，字节映射表 `0x82d4a8`
  - type 0x00 → `0x82d2eb`：直接 `memcpy(payload, out, size2)`
  - type 0x0A → `0x82d311` → 调用 `0x8a6f60`
  - type 0x0B → `0x82d37a` → 调用 `0x8a6c90`
  - 其他 → `0x82d43e`（错误）

## 2. type 0x0B = LZO1X

`0x8a6c90` 实现标准 LZO1X 解压：

- 入口参数：`(const uint8* src, uint32 src_len, uint8* dst)`
- 返回：输出字节数；失败返回 `-1`
- 控制码语义与 LZO1X 一致：
  - 首字节 > 17：初始字面量
  - `t >= 64` / `t >= 32` / `t >= 16` / `t < 16` 四种回溯
  - 结束标志：`m_pos == op`
- 关键常量：`0x0800`、`0x4000`、`0x400`、`0x801`，与 LZO1X 完全一致。

### 实现

`tools/dhsh/nz_lzo.py` 中提供纯 Python 版本：

```python
from dhsh.nz_lzo import lzo1x_decompress
out = lzo1x_decompress(payload, size1)
assert len(out) == size1
```

已验证：

- 压缩样本集：**133/133** 个 type=0x0B 文件解压长度等于 `size1`，magic/文本检查通过。
- B1 随机抽样：**100/100** 个 type=0x0B 叶子解压成功（`.apg` 98、`.scp` 1、`.a16` 1）。
- 解压结果示例：
  - `ib_main2.scp` → 可读 GBK UI 脚本；
  - `allbg01.a16` → 以 `AP` 开头的合法 A16；
  - `.apg` → `AG`；
  - `.tga` → TGA 头。

## 3. type 0x0A = 16 位 LZW

`0x8a6f60` 实现的是**按 16 位 token 读取的经典 LZW**（不是 LZO 变体）：

```
输入：小端序 uint16 码流（payload 长度按 2 字节对齐）

0x0000–0x00FF   字面量字节
0x0100          流结束（END）
0x0101–0xFFFF   动态字典码，按出现顺序递增分配
```

- 第一个码必须是字面量；`0x0100` 作为首码表示空输出。
- 每读入一个码 `cur`，输出其字符串，并向字典追加
  `prev_string + first_byte(cur_string)`（标准 LZW，含 KwKwK 情形）。
- 字典从 `0x0101` 开始增长，达到 `0xFFFF` 后冻结；后续码继续用现有字典解码。
- 输出长度必须严格等于 NZ 头 `size1`；这是最强的正确性校验。

### 实现

`tools/dhsh/nz_lzw10.py` 提供纯 Python 版本，并已接入 `tools/dhsh/nz.py`：

```python
from dhsh.nz_lzw10 import lzw10_decompress
out = lzw10_decompress(payload, size1)
assert len(out) == size1
```

已验证：

- 压缩样本集：**267/267** 个 type=0x0A 文件解码长度等于 `size1`。
- B1 随机抽样：**100/100**（APG 77、A16 11、TGA 6、PIF 4、CSV 2）。
- B2 随机抽样：**1,000/1,000**（含 801 个 APG、92 个 TGA、47 个 A16 等）。
- B3 定向验证：**3,213/3,213**（1,945 PIF、625 SCP、420 CSV、174 MLK、
  18 Lua、13 TXT、13 PSD、3 INI、2 BMP）。
- 全量校验：**59,222/59,222** 通过（18 种扩展名，0 失败），解码输出 3.34 GiB；
  8 进程约 709 秒，报告 `manifests/batches/nz0a-full.json`。
- 解压结果开头与扩展名一致：APG→`AG`、A16→`AP`、PIF→`PIF\0`、TGA→TGA 头、
  CSV/SCP→文本。

> `mlk`（以及部分 `size=0` 的 `.ini` 别名）解压后是 `lnk\0` 二进制地图跳转数据，
> 不是文本。文本类文件多为 GBK；用 GB18030 可覆盖 GBK 扩展区（如 `0xAAA1`），
> 少数新 UI 脚本为 UTF-8；CP936 的独立 `0x80`（欧元符号）需要单独处理。

## 4. 使用与边界

- 0x0A / 0x0B 解压只需要 `payload` 和 `size1`；输出长度必须严格等于 `size1`，否则视为失败。
- 解压后可能仍是 **A16/APG/TGA/SCP** 的自研子格式，需要继续按对应格式解析。
- 先用 10–100 个样本验证，再按批扩大；不要在未验证的情况下全量解码。
- 纯 Python LZW/LZO 足够用于样本与中等批量；全量 2.5/3.6 GiB 建议用 C 实现或分批 + 校验。
- 0x0A 的 `size2` 为偶数；`payload` 按小端 `uint16` 读取（多余奇数字节会被忽略）。

## 5. 证据

| 内容 | 地址/文件 |
|---|---|
| NZ 通用解码器 | `Game.exe` VA `0x82d1f0` |
| type 0x0A 解码器 | `Game.exe` VA `0x8a6f60` |
| type 0x0B 解码器 | `Game.exe` VA `0x8a6c90` |
| 分派表 | `0x82d494` / `0x82d4a8` |
| Python 实现 | `tools/dhsh/nz_lzw10.py`、`tools/dhsh/nz_lzo.py` |
| 验证工具 | `tools/probe_0a.py`、`tools/validate_0a.py` |
| 样本 | `extracted/samples/compressed/`（400 个） |
| 验证报告 | `manifests/batches/nz0a-b1.json`、`nz0a-b2.json`、`nz0a-b3-scripts-pif-v2.json`、`nz0a-full.json` |
