# NZ 资源封装格式（已破解 0x0B）

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
| `0x000A` | 16 位 token / 字典压缩 | ❌ 未破解（`0x8a6f60`） |
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

## 3. type 0x0A（未破解）

`0x8a6f60` 使用 16 位 token 与 8 字节表项（`0xb30328`）的字典/回溯结构；
初步判断是“按 16 位符号的 LZO 变体 + 字典”，但仍需单独逆向。当前约 59,222
个叶子（11.3%）依赖它。

## 4. 使用与边界

- 0x0B 解压只需要 `payload` 和 `size1`；输出长度必须严格等于 `size1`，否则视为失败。
- 0x0B 解压后可能仍是 **A16/APG/TGA/SCP** 的自研子格式，需要继续按对应格式解析。
- 先用 10–100 个样本验证，再按批扩大；不要在未验证的情况下全量解码 214,623 个 0x0B 叶子。
- 纯 Python LZO 足够用于样本与中等批量；全量 3.6 GiB 建议用 C 实现或分批 + 校验。

## 5. 证据

| 内容 | 地址/文件 |
|---|---|
| NZ 通用解码器 | `Game.exe` VA `0x82d1f0` |
| type 0x0B 解码器 | `Game.exe` VA `0x8a6c90` |
| type 0x0A 解码器 | `Game.exe` VA `0x8a6f60` |
| 分派表 | `0x82d494` / `0x82d4a8` |
| Python 实现 | `tools/dhsh/nz_lzo.py` |
| 样本 | `extracted/samples/compressed/`（400 个） |
| 验证 | B0/B1：133 + 100 样本，0 失败 |
