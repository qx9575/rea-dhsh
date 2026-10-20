# 特殊 JPG 独立复核（kimi）

- 日期：2026-10-21
- 对象：`Game.exe`（SHA-256 `f6bf66d2…`，VA 基准 0x400000——注意 PE 头里
  的 image_base 是 VMProtect 的 0x57c000，**文件偏移 = dump VA − 0x400000**，
  .text/.rdata 均未加密，常量可直接读）
- 依据：`manifests/batches/special-jpg-code-dump.txt`（已用 capstone 对原文件
  字节逐条核对，439/439 条与 dump 完全一致）
- 工具：`tools/kimi_dump_consts.py`（常量提取）、`tools/kimi_idct_sim.py`
  （439 条 MMX 指令的逐位精确解释器 + 数值验证）、`tools/kimi_verify_slots.py`
  （v2 DC 槽位实证）、`tools/kimi_color_check.py`（颜色转换对拍）

## 问题 1：IDCT（0x8a8780）的缩放与舍入

**结论：现行 Python 的 `raw_sum/64` 缩放是对的**；错在舍入方式
（`np.rint` 四舍五入）以及定点内部截断无法被 float 复现。

实测（解释器驱动）：

- 相对标准公式 `f = (1/4)Σ C(u)C(v)F·cos·cos`，**真实倍数 = 1/16**
  （即 raw_sum/64）。DC 实测：F0=512 → 输出 4，F0=2048 → 16，负值对称，
  全局 LSQ 拟合 S = 0.06249782 ≈ 1/16；
- `packsswb` 之前只有 **`psraw 4`（算术右移 4 = 对负数向下取整 floor），
  没有任何加性舍入偏置**；`packsswb` 饱和到 **[-128, 127]**（有符号字节，
  无 +128 偏移——level shift 在颜色转换里做）；
- 全块统计（随机/稀疏系数各 3000 块）：相对 `floor(ref/16)`，62–74% 像素
  完全一致，其余几乎全为 ±1（内部 pmulhw/psraw 截断所致），与
  `rint(ref/16)` 不符率反而更高 → **float 近似应把 `np.rint` 改成
  `np.floor`**；要像素级精确必须用整数流程（见 `kimi_idct_sim.py`）；
- 一个真实怪癖：pass-1 右半列组（v=4..7 通道）奇数部有
  `paddw mm4, [0xab6320]`，常量 = **[1,1,2,5]**（按 lane），经 `psraw 2`
  后仅 v=7 lane 生效（+1 偏置）——全零输入也会在该通道产生 ±1 杂散。

算法结构（与 FFmpeg `simple_idct` 同族）：

- 预缩放表 `0xab6350`（64×u16，与系数缓冲一一对应）：
  `row0[u] = round(√2·cos(uπ/16)·2^14)` = 16384, 22725, 21407, 19266, 16384,
  12873, 8867, 4520；`T[u][v] ≈ row0[u]·row0[v]/2^14`，第 5–7 行部分列带
  ×2/×4 修正（补偿 pass-1 两个半列组不同的移位结构）；
- 旋转常量（均 4×u16 重复）：K0(0xab6330)=23170≈√2/2，
  K1(0xab6338)=21407，K2(0xab6340)=17734≈0.541196（AAN FIX_0_541196100），
  K3(0xab6348)=25080≈0.765367（AAN FIX_0_765366865）；
- 流程：表乘（pmulhw）→ pass 1 沿 u（列 0–3、4–7 两段）→ 原位 8×8 转置
  （punpck + 4 个 scratch qword `0xcb0368–0xcb0380`）→ pass 2 沿 v（不再乘表）
  → `psraw 4` → `packsswb` 原地写回前 64 字节；
- 输入缓冲布局：**coeff[i]，i = u·8 + v**（与现行 Python 一致）；输出 64 字节
  **行主序像素 byte = y·8 + x**（也与现行一致，无需改动）。

`kimi_idct_sim.py::idct_mmx(coeff64)` 是按 dump 逐条解释的位精确参考实现
（439 条直线指令，无分支），可直接作为“独立伪代码”。

## 问题 2：YCbCr→RGB（0x8a3b30 / 0x8a3590）

建表（0x8a3d46 循环，x87；`0x939730` = `__ftol2`，fistp 走 FPU 控制字。
**已在运行进程内转储四张表并与预测逐字节对比（256/256 全中）：游戏把
FPU 设为截断模式，转换 = 向零截断**）。四个 double 常量的精确值：
**K1=1.402、K2=0.34414、K3=0.71414、K4=1.772、B=1.0**（不是 0.5、不是 128）。
表按无符号字节索引、按有符号 int8 建立（v = (int8)i）：

```text
T_R [i] = trunc(1.402  * v + 1.0) >> 1    ; 0xb0ced0，查 Cr
T_Gb[i] = trunc(1.0 - 0.34414 * v) >> 1   ; 0xb0cdc8，查 Cb
T_Gr[i] = trunc(1.0 - 0.71414 * v) >> 1   ; 0xb0cbc8，查 Cr
T_B [i] = trunc(1.772  * v + 1.0) >> 1    ; 0xb0ccc8，查 Cb
```

**运行时选择（已从进程内存确认）**：`[0xb0cfd8]=0x8a32d0`（MMX **RGB565**），
`[0xb0cbc0]=0x8a38d0`，`[0xb0c9b8]=0x8a8780`（MMX IDCT），掩码
0xf800f800/0x07e007e0 —— 即本机实际走 **RGB565 + MMX** 路径；dump 里的
0x8a3590 是 555 变体，并非当前活动路径（565 版 0x8a32d0 未在 dump 中，
结构应相同、仅移位/掩码不同）。

像素合成（标量 helper 0x8a3840/0x8a38d0；MMX v2 0x8a3590 用
paddsb/psubusb 做同样语义）：

```text
t  = (Y + 0x80) >> 1                       ; sar，floor；Y 为 IDCT 的有符号输出
R7 = clamp(T_R[Cr] + t, 0, 127)
G7 = clamp(T_Gb[Cb] + T_Gr[Cr] + t, 0, 127)  ; 标量路径是 8 位回绕 add，值域内不触发
B7 = clamp(T_B[Cb] + t, 0, 127)
RGB555（0x8a3840 / MMX 0x8a3590，掩码 0xfc00/0x03e0）:
    px = (R7>>2)<<10 | (G7>>2)<<5 | (B7>>2)
RGB565（0x8a38d0 / 0x8a32d0，掩码 0xf800/0x07e0）:
    px = (R7>>2)<<11 | (G7>>1)<<5 | (B7>>2)
```

要点：

- **整条管线是半精度（7 位）**：Y 先 `(Y+128)>>1` 丢失 LSB，色度项量化到
  半步长；7 位结果直接取高 5/6 位，不做任何舍入；
- **level shift 只有 Y 的 +128**（色度本来就有符号）；等效 8 位值 = 2×v7，
  永远到不了 255；
- 输出格式两种都支持，运行时由 `0x8a3c00(arg0, arg1)` 选择
  （arg0=0 → 565；arg1=0 → 无 MMX 的标量路径 0x8a3960 + IDCT 0x8a8630）；
  **本机实测 = RGB565 + MMX**；
- v2 的 4:2:0 上采样（0x8a3590 内层 0x8a367e）：**最近邻 2×2**——色度项经
  `punpcklbw` 横向复制，两个输出行共用同一行色度；Y 平面 = 4 个块按
  128 字节步长排列（TL=+0, TR=+0x80, BL=+0x100, BR=+0x180，即
  Y0=TL、Y1=TR、Y2=BL、Y3=BR，与现行 Python 的块排布一致）；
- 与标准 full-range BT.601 对拍（全定义域网格）：最终 5/6 位通道最大差
  **±1**，0 差占 88–96%（`tools/kimi_color_check.py`）。现行 Python 的
  标准公式 + rint 在视觉上等价，但不能逐像素复现游戏画面。

## 问题 3：MCU 描述符与 v2 DC 槽位 —— **发现一处真 bug**

描述符字段（0x8a3dd0 逐条核对，6 字节 = 48 位无浪费）：

```text
f0 = dword & 0x3ff                       ✓ 10 位字节增量
f1 = byte[4] & 0x1f                      ✓ 位偏移
f2 = (s16)word[4] >> 5                   ✓ 11 位有符号（sar ax,5）
f3 = (s32)(dword << 11) >> 21            ✓ 11 位有符号（第 10–20 位）
f4 = (s32)dword >> 21                    ✓ 11 位有符号（第 21–31 位）
```

- 累加方式：循环里 `rec[i].f0 += rec[i-1].f0`（累加），随后给 **前一个**
  记录加 data_start，最后一个记录在循环外加 → 净效果
  `ptr[i] = data_start + Σ_{j≤i} f0[j]`，与现行 Python 完全一致 ✓；
- 内存记录 12 字节：+0 指针、+4 位偏移、+6 f2、+8 f3、+10 f4 ✓；
- **v2 槽位顺序复核结果：文档/现行代码的 `0/1/2/0/1/2` 是错的。**
  驱动 0x823240 v2 分支六次调用的 DC 种子指针（对 12 字节栈记录的
  esp 符号跟踪，见下表）：

```text
call1 Y0 (comp0, buf TL) : seed = copy+6  = f2   slot 0
call2 Y1 (comp0, buf TR) : seed = copy+6  = f2   slot 0
call3 Y2 (comp0, buf BL) : seed = copy+6  = f2   slot 0
call4 Y3 (comp0, buf BR) : seed = copy+6  = f2   slot 0
call5 Cb (comp1)         : seed = copy+8  = f3   slot 1
call6 Cr (comp2)         : seed = copy+10 = f4   slot 2
```

  即 **4 个 Y 块共享同一条 DC 链（slot 0）**，Cb/Cr 各占 slot 1/2。
  语义上也合理：Y 块按光栅顺序链接（Y3 的 DC 传给下一 MCU 的 Y0——
  这正是“f2 与上一 MCU 解出 DC 精确相等”验证过的链）。注意该验证在
  错误 scheme 下同样通过（边界值相同），所以之前没暴露。
- 实证（`tools/kimi_verify_slots.py`，v2 样本 240×128）：
  错误 scheme 的 Y-DC 平面相邻差均值 **289**、MCU 内 4 块 DC 极差中位数
  **341**；修正后 **28 / 22**——差 10 倍，证实现行 v2 解码的 Y1/Y2 块
  DC 被污染自色度链，导出图应有块级亮度错误。
- **修正**：`special_jpg.py` 的 v2 `order` 改为
  `[(0,0),(0,0),(0,0),(0,0),(1,1),(2,2)]`。
- v3 分支复核无误：Y=f2、Cb=f3、Cr=f4 ✓；块缓冲 +0x30/+0x230/+0x2b0
  对应转换器平面 +0x18/+0x218/+0x298 ✓。

## 问题 4：0x8a1fe0 位序 / Huffman 比较 / 符号扩展

全部与现行 Python 一致，细节确认如下：

- 位流：`lodsd + bswap` 按 **32 位大端字**加载，`shld ebx, eax, cl` 拼成
  64 位滑动窗口，**MSB-first**（等价于字节流 MSB-first，Python BitReader
  正确）；`cl ≥ 0x20` 时 `sub cl,0x20 + lodsd`  refill；块尾写回
  `ptr = esi − 4`（lodsd 预取补偿）与位偏移 ✓；
- 描述符位偏移 f1 直接作为窗口初始左移量 ✓；
- Huffman 比较：`shld eax, ebx, cl`（cl=len）取窗口**高 len 位**右对齐后与
  条目 `code`（u16，低位对齐）比较，**按文件顺序线性扫描、首个匹配命中**，
  要求表前缀自由（已验证）；Python 的 LUT 与之等价 ✓；
- DC/AC 差分符号扩展：`bt eax, ecx`（测第 cat−1 位）→ 若 0 则
  `eax = v + 1 − 2^cat`，即标准 JPEG `extend()` ✓；
- **两处与 Python 不同的整数边界行为**（现行代码用 int32，建议修正）：
  - DC：`coeff[0] = (dc·quant[0]) 的低 16 位`（imul 后只存 ax，**回绕**）；
  - AC：`val·quant[k]` 做 **int16 饱和**（dx≠0 且 dx≠−1 时钳到
    0x7fff/0x8000，见 0x8a215d–0x8a2170）；
- run 跳过的槽位显式写 0、EOB 后剩余槽位也写 0 → 每块 64 项全部重写，
  与 `np.zeros(64)` 等价 ✓；量化表按**扫描序**索引（edi 随 run 前进）✓；
  ~~系数按私有 zigzag 写入自然位置~~ —— **zigzag 方向后来经截图对拍
  证伪，见下文 bug B**（计数器从 63 倒序索引该表）。

## 运行时 + 截图验证（2026-10-21 补充）

游戏客户端实测（`tools/kimi_probe_runtime.py`，只读进程内存）：

- 活动路径 = **MMX IDCT 0x8a8780 + MMX RGB565 转换器 0x8a32d0**
  （helper 0x8a38d0，掩码 0xf800f800/0x07e007e0）——dump 里的 0x8a3590
  是 555 变体，非本机活动路径；
- 四张颜色表逐字节转储，与 `trunc(K·v+1.0)>>1` 预测 **256/256 全中**
  → 游戏把 FPU 设为截断模式，建表是**向零截断**（不是最近舍入）；
- 内存中的 IDCT 预缩放表与文件提取值逐字节一致 ✓。

截图对拍（世界地图界面 = `ui/minimap/map/BangPai_1_q.jpg`，256×224 v3）：
修正全部三处 bug（见下）后解码与游戏画面结构、颜色、文字标签完全一致
（`extracted/easy/kimi_side_by_side.png` 并排对比；残差全部来自界面缩放
重采样，不是解码错误）。

## 截图对拍又抓出两个块级 bug（v2/v3 都中招）

之前的“960,000 块 0 错误”只验证了位流消耗量，没验证系数落位——
用真机画面对拍后暴露：

### bug B：私有 zigzag 表方向反了

`0x8a1fe0` 的 AC 循环里位置计数器 **从 63 往下数**（`mov cx, 0x3f`；
`dec cx; jne`；EOB 时零填充 `table[cx..1]`）。因此扫描第 k 个 AC 系数
（k=1 起）落在 **`table[64-k]`**，不是 `table[k]`。表倒过来读正好是
从低频到高频的正常之字序（1,0)→(0,1)→(0,2)…——熵编码语义也合理。
现行 Python 用 `ZIGZAG[k]`，等价于把 AC 系数按频率倒序放，DC 不受影响
（所以 DC 链验证照过）。

### bug C：IDCT 输出块需逐块转置

硬件 IDCT 输出字节序是 **y·8+x**（行主序像素）；`idct_blocks` 的
`(_T @ f @ _TT).reshape(-1)` 是 **x·8+y**。用位精确模拟器
（`kimi_idct_sim.py`，直接执行真实 MMX 指令流）对同一系数块对拍：
sim 输出与 `idct_blocks(...).T` 差 ≤±2，与未转置版差 118——确认需要转置。

## 修正清单（供 special_jpg.py 采纳）

1. **v2 `order`**：`(0,0),(0,1),(0,2),(0,0)` → 全 slot 0（否则 v2 颜色错误）；
2. **ZIGZAG 反向**：AC 第 k 个系数写 `ZIGZAG[64-k]`（DC 仍是 0）；
3. **逐块转置** IDCT 输出（或在装配平面时转置每个 8×8 块）；
4. IDCT 舍入：`np.rint` → `np.floor`（像素级精确用 `tools/kimi_idct_sim.py`）；
5. `decode_block`：DC 乘积取 int16 回绕、AC 乘积 int16 饱和；
6. 颜色转换如需对拍游戏截图：查表管线（**trunc**(K·v+1.0)>>1，半精度 7 位，
   本机 RGB565）。

修正 1+2+3 后的效果：`BangPai_1_q.jpg`（v3）与 `BangPai_1m.jpg`（v2）
解码均与游戏内画面一致（见 `extracted/easy/kimi_bp_*_fullfix.png`）。
