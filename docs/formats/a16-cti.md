# A16 / CTI 图像格式

- 目标：`D:\software\dhsh\shdata\Game.exe`
  SHA-256 `f6bf66d24a3523bb6a30541589fabea36bbceec84355463d8e2c99e3b7f16be9`
- 日期：2026-10-19
- 方法：REA + Ghidra 静态反编译 + Capstone 定点反汇编；未运行游戏、未注入进程。
- 当前实现：`tools/dhsh/formats/a16.py`

## 1. A16 头

小端序：

```
0x00  char[2]  "AP"（或 "AL"）
0x02  uint16   header_size（实测 24）
0x04  int16    x offset
0x06  int16    y offset
0x08  uint16   width
0x0a  uint16   height
0x0c  uint16   flags / format hint（0xFF、0x20、0x10 等）
0x0e  uint8    pixel compression
0x0f  uint8    alpha mode
0x10  uint32   stream1 size
0x14  uint32   stream2 size
0x18  ...      stream1，随后 stream2
```

Game.exe 解析位置：

- 校验 `cmp ax, 0x5041`（"AP"）/ `0x4c41`（"AL"）：VA `0x89bff7`
- 读取 x/y/width/height：`0x89c04e`
- 按 `[+0x0e]` 选择像素解码器：跳转表 `0x89c744`
- 按 `[+0x0f]` 选择 alpha 解码器：跳转表 `0x89c774`

## 2. 像素压缩（[+0x0e]）

| 值 | 含义 | 状态 |
|---|---|---|
| 0 | 原始 RGB565（`width*height*2` 字节） | ✅ |
| 1 | 单色填充（stream1 前 2 字节为颜色） | ✅ |
| 2 | RLE16 | ✅（抽样中 99.3%） |
| 3/4 | 另一种处理（`0x89c127`，使用 `0x82d1f0`） | ❌（全量语料未出现） |
| 5 | stream1 为完整 JPEG（与 APG comp 5 相同），stream2 为 alpha | ✅（40 个） |
| 11 | `0x89c29f` | ❌（全量语料未出现） |
| 6/8/9/10 | 错误/不支持 | — |
| 7 | `0x89c351` | ❌ |

### RLE16（compression=2）

解码器：VA `0x8a7530`，算法：

```
while src < end and out_count < pixel_count:
    ctrl = *src++
    n = ctrl & 0x7f
    if ctrl & 0x80:
        color = *(uint16*)src; src += 2
        repeat color n times
    else:
        copy n uint16 pixels (2*n bytes) literally
```

颜色为 **RGB565**：

```python
r = ((c >> 11) & 0x1F) * 255 // 31
g = ((c >> 5)  & 0x3F) * 255 // 63
b = ( c        & 0x1F) * 255 // 31
```

## 3. Alpha（[+0x0f]）

| 值 | 含义 | 状态 |
|---|---|---|
| 0 | 无 alpha（不透明） | ✅ |
| 1 | 不支持/错误 | — |
| 2 | **RLE8**，输出 `width*height` 字节 | ✅（抽样 91.4%） |
| 3/4 | NZ 包装的压缩流（`0x89c550` → `0x82d1f0`） | ❌ |
| 5 | `0x89c5e9` → `0x8a7640` | ❌ |
| 6/7/8 | 错误/不支持 | — |

### RLE8（alpha_mode=2）

解码器：VA `0x8a75c0`：

```
while src < end and out_count < pixel_count:
    ctrl = *src++
    n = ctrl & 0x7f
    if ctrl & 0x80:
        value = *src++
        repeat value n times
    else:
        copy n literal bytes
```

观察到的值是 0..32（0=透明，32=不透明）。渲染时按 `value * 255 // 32`
缩放到 0..255，效果最好（B0 对比了 raw/binary/x8/invert 四种方案）。

## 4. CTI 调色板

文件固定 1556 字节（抽样 500/500）：

```
0x00  char[2]  "CT"
0x02  uint16   header_size = 20
0x04  uint16   count = 256
0x06  uint16   bpp = 16
0x08  uint32   format = 1
0x0c  uint32   table1_size = 512
0x10  uint32   table2_size = 1024
0x14  ...      table1：256 × uint16（疑似 RGB565）
0x214 ...      table2：256 × uint32（疑似 32 位色/其他表）
```

Game.exe 解析位置：

- 校验 `cmp word ptr [ebx], 0x5443`（"CT"）：VA `0x82661b`
- 复制 table1/table2：`0x826660`
- table1 复制到对象字段，table2 复制到对象 +0x3f0

已验证：20/20 个 CTI 头部/尺寸正确。table1/table2 的语义（尤其是 table2
与 table1 颜色不匹配）仍未完全确认；**APG 的 AI 索引帧使用 table1 后颜色正确**
（宠物 36、头像 24962），说明 table1 至少是一套可用的主调色板；table2 会得到
明显不同的颜色变体（蓝↔黄、紫↔红），选择逻辑待定。

## 5. B0 / B1 结果

- **B0**：23 个 A16 样本全部解码成功（comp 2；alpha 0/2），保存为 PNG；
  `extracted/_batches/s3-b0/png/`，拼图 `a16_contact_b0.png`。
- **B1**：100 个随机 A16 样本全部通过（comp 2；alpha 2=92、alpha 0=8）；
  像素数/alpha 长度校验通过。
- CTI：B0 20/20 头部验证；B1 未单独扩大。
- 样例视觉检查：角色头像、宠物、装备、UI 图标、加载界面均正确，透明背景与边缘正常。

## 6. 待办

1. ~~compression 3/4/5/7/11 的完整解码~~：全量扫描显示只存在 0/1/2/5，
   5 已按 JPEG 实现；3/4/7/11 在 29,716 个文件中均未出现；
2. ~~alpha_mode 1/3/4/5 的完整解码~~：全量扫描只有 0/2，均已实现；
3. CTI table1/table2 语义与索引图的关联；
4. ~~全量 A16 → PNG 导出~~：已完成（见第 7 节）。

## 7. 全量扫描与转 PNG（2026-10-20）

三份导出语料合并扫描（`tools/scan_a16.py`）：

- `extracted/images-proprietary`（type 0）20,990
- `extracted/decoded`（0x0A）3,408
- `extracted/decoded_0b`（0x0B）5,318
- 合计 **29,716 个 A16**，527.8 MiB，875,333,120 像素，头部/尺寸错误 **0**。

模式分布：

| compression | 文件数 |
|---:|---:|
| 0 原始 RGB565 | 131 |
| 1 单色 | 14 |
| 2 RLE16 | 29,531 |
| 5 JPEG | 40 |

| alpha_mode | 文件数 |
|---:|---:|
| 0 不透明 | 1,863 |
| 2 RLE8 | 27,853 |

- 全部文件 `header_size + stream1_size + stream2_size == 文件大小`，magic 均为 `AP`。
- 40 个 comp 5 全部位于 `ui/clan/homeRevision/viewbuilding/*.a16`，stream1 可被
  Pillow 直接作为 JPEG 打开；alpha 仍为 RLE8（0..32 → 0..255）。
- 新增 `tools/dhsh/formats/a16_fast.py`（NumPy RLE + RGB565 LUT），
  与参考解码器在 89 个分层样本（覆盖全部 7 种 comp/alpha 组合）上
  **逐字节一致**（`manifests/batches/a16-fast-verify.json`）。
- `tools/export_a16.py` 输出 `<out>/<corpus>/<logical path>.png`：
  B0 **40/40**（1.7s）、B1 **400/400**（3.0s）、B1b **3,000/3,000**（12.8s）、
  全量 **29,716/29,716**（0 错误，130.6s，约 227 文件/s）。
- 全量输出 `extracted/a16_png/`，405.8 MiB；清单
  `manifests/batches/a16-full.csv`；抽查 100 个 PNG 尺寸全部匹配、0 缺失。
