# 资源解包进度

- 更新：2026-10-23
- 数据源：`D:\software\dhsh`（新水浒Q传 **1.50.22**，2026-10-21 更新）
- 基线：`pak_inventory.json`（**523,213 条索引，含 189 个 size=0；非空 523,024**）；
  `Game.exe` 当前 SHA-256 `7a5e2c08…`（旧基线 `f6bf66d2…`）
- 工具：`tools/dhsh/`（PAK 解析、NZ 统计、按类型/扩展名导出）
- 格式文档：`docs/formats/nz.md`、`docs/formats/a16-cti.md`、`docs/formats/apg.md`、`docs/formats/pif.md`、`docs/formats/pg2.md`、`docs/formats/special-jpg.md`、`docs/formats/map.md`、`docs/formats/scp.md`、`docs/formats/apci.md`、`docs/formats/nonpak.md`

## 最新突破

- **非 PAK 资源索引（2026-10-23）**：新增 `tools/scan_nonpak.py`，把
  `sound/`（435 WAV + 21 MP3）、`font/`（4 TTF + 1 TTC + INI）、
  `animation/`（3 APG + 3 PIF + 1 MP3）、`update/skin/`（32 BMP +
  8 `BMP.rNNNN` + 1 JPG）纳入统一清单：
  **510 个文件、198,683,731 B（189.5 MiB）、0 错误**。逐文件记录
  SHA-256；WAV 解析 fmt（PCM/IMA ADPCM、声道/采样率/位深/时长），
  MP3 按帧头累计时长；fontTools 读取 TTF/TTC 的 family/字形数；
  APG/PIF 记录帧数；`*.bmp.r1571/.r1911` 实际仍是 BMP，按图像解析。
  14 个内容重复（13 组音效、`STXINGKA.TTF`=`ARKai_T.ttf`）。
  清单 `manifests/batches/nonpak-index.json` / `.csv`，详见
  `docs/formats/nonpak.md`。`save/` 为可变用户配置，未纳入。
  剩余：音效/音乐数字 ID 与 MAP/SCP 配置的消费端绑定。

- **MAN / MLK 闭合（2026-10-23）**：MAN 记录不是纯路径——每条 100 字节
  为 `path[64] + x + y + 6 个 int32`；地图加载器只读 path/x/y，并用
  `(*obj+0x2c)(x,y)` 定位。把 `YingTianFu/duchang/denglong1` 首帧按
  `(2628,421)` 叠到背景，带 alpha 模板匹配精确命中，确认 **x/y=左上
  像素坐标**。MLK 记录 108 字节：`value + target[64] + paired + 入口
  矩形 + dest + 3 个额外字段`；**`paired` = 目标图反向链路的 `value`
  （308/308 双向验证）**，入口矩形在预览中与店铺门口脚垫重合。
  全量扫描（`tools/scan_map_anim.py`）：292 个 INI → 283 个 MAN
  （9 个整图未导出，与缺背景图一致；`map\...` 需回退 `map1/`）、
  1,968 条记录、713 个去重动画目录、710/713 可解析、4,952 个帧引用；
  MLK 228 文件 / 313 条记录 / 311 个 target 存在 / 5 条无反向边。
  预览新增 `--man`（动画首帧）与 `--links`（入口框+落点），
  YingTianFu 103 条 MAN 绘制 93 条、24/24 条 link 绘出。清单
  `manifests/batches/map-anim-scan.json`、`map-anim-edges.csv`；
  详见 `docs/formats/map.md` §3/§5/§8。剩余：MAN `f50/f54/f58`、
  PIF `count=2` 延时分支、MLK `extra0..2` 与 `dest` 的运行时落点验证。

- **MAN / PIF / APCI 字段语义（2026-10-23）**：
  - **MAN**：`f48` = 深度排序 Y 偏移（getter `0x824740` 返回
    `obj+0x58 + y`，排序调用者 `0x7feba0` 可再加 10000）；
    `f58` = Q8.8 排序斜率（vtable `0x431290` 返回
    `y + f48 + (f58*arg>>8)`）；`f50` = 对象查找键（推断宿主/地图
    对象 ID，0=未绑定）；`f4c` = 启用/自动开始（语料恒 1）；
    `f5c/f60` = 定时重置基础间隔/随机范围（约 5–40 s + 0–50 s）；
    `f54` 三个 bit 在整份 `.text` 中**没有读取点**，疑为遗留。
  - **PIF**：行 `+0x04/+0x06` = x/y 偏移（`FUN_00431fc0` →
    `WObject::AddPos`），`+0x0a` = 帧延时（0→80 ms）；`count=2`
    的 24 字节行由 vtable `0x824ba0` 在加载时归一化为 28 字节
    （尾部补 0），无独立延时分支。
  - **APCI**：`entry.a` 按 record 家族分类（`tools/classify_apci_entry_a.py`
    → `manifests/batches/apci-entry-a-families.csv`）：`npc*` 100%
    使用 10k–39k ID 段，基础 `player\body/cloth/weapon` 约 99% 在
    100–9,999，编号 player 套装混用 20k/50k/60k/101k；41.9% “越界”
    是家族 ID 空间不同，不是解析错位。12B 单元 = PIF 行字段重排
    `[f0,f1,f4,f5,f2,f3]`（4,156 item / 11,834 单元），消费端逐槽
    取用仍待确认。
  详见 `docs/formats/map.md` §3、`docs/formats/pif.md` §3、
  `docs/formats/apci.md` §3。

- **CEL 图层语义闭合（2026-10-23 运行时）**：YingTianFu 玩家格
  `(128,125)` 实测 `map+0x34`（第一张位图，`param_5=1`）缓冲 `0x00`、
  override `0x00`（可走）；`map+0x38`（第二张，`param_5=0`）缓冲
  `0xff`、override `0x80`（阻挡）。**第一张 = 正常地面碰撞层，1=阻挡**。
  旧 `parse_cel`/`export_map_cel.py` 固定取第二张，已改为默认第一张；
  275 张 CEL PNG 重导（YingTianFu `blocked_ratio` 0.9589 → **0.4844**，
  0 错误）。另在 **YTF_WuQiDian** 实测：文件 CEL 64×40 被地图加载器
  **左上复制**到地图格 64×48，行 40–47 补 `0xff`；玩家格 `(33,29)` 与
  多个抽查格均与文件位一致，地图预览已对这类图按左上补齐绘制。
  截图 `pic/ytf_wuqi_1.png` 以偏移 `(120,167)` 与背景对齐后，CEL 可走
  轮廓贴合地板/楼梯；92% 阻挡主要来自画布黑边/墙体（实际可走约 6.5%）。
  另在 **TingYuan_1** 确认：背景非黑 bbox `(0,0)-(895,639)`，地图内容
  896×640 在 1024×768 背景的**左上角**（右侧/底部 128px 黑边），背景
  偏移为 **(0,0)**；截图 `pic/tingyuan.png` 的 `(-1,-26)` 只是窗口偏移，
  CEL 可走轮廓贴合草地/石径。证据
  `manifests/batches/cel-layer-runtime.json`，探测工具
  `tools/probe_cel_layers.py`。剩余：第二张辅助层用途、`JiaYuan_1_1`
  这类 CEL 大于地图格的异常图。

- **地图预览（2026-10-23）**：新增 `tools/map_preview.py`，把 special JPG
  解码背景与 MSK/core 遮罩合成为整图；静态遮罩洋红浅填充+2px 描边，
  group 成员默认取每组首帧（橙色）。画布以**背景原生尺寸**为准（INI
  W/H 可能只是可玩区域，如 TingYuan_3 896×640 vs 背景 1024×768、
  HFZLHaoZhai 3328×2304 vs 2816×2048）。CEL 默认不绘制：`0x4252e0`
  可在两张位图间选择，层级/极性未闭合，且 26 张图的 CEL 只是背景子区域。
  样本 B0 4 张、B1 10 张、B3 100 张、全量 292 张（283 渲染、9 张缺背景/
  遮罩跳过），0 错误，**306.7 MB / 52.5 s**（OpenCV 解码/编码 + 单通道
  标签混合 + `--jobs 6`；顺序基线 276.5 s，约 5.3×）；清单
  `manifests/batches/map-preview-*.csv`。

- **BangPai_1 group/frame 播放语义闭合（2026-10-23）**：全量扫描 300/300
  个 core/msk（16,114 放置、0 错误）只有 `BangPai_1` 两版含非静态项
  （50/300、55/238；groups 1–11、frames 1–5）。静态链：主分发器
  `0x7a2240` 的 case `0x3077`（调用点 `0x7a339c`）→ `FUN_00410000`
  （13×10B 建筑记录：byte0=state、byte9=type，`frame=(state+1)/2`）→
  `FUN_0041f2b0` → `FUN_00426980`（group 1–11、frame 0–6；同组匹配帧
  `+0x18 active=1`，其余 0）。**没有帧率/循环/时间动画**，完全由命令
  0x3077 的建筑状态驱动；运行时 `+0x18` 是 active，不是文件放置 flag。
  证据 `manifests/batches/bangpai-playback.json`，详见
  `docs/formats/map.md` §4.9。

- **2026-10-23 重新基线 + 纠错**：客户端 1.50.20 → **1.50.22**，
  `Game.exe` SHA `f6bf66d2…` → **`7a5e2c08…`**；重建 `pak_inventory.json`
  （523,213 条，含 189 个 size=0，非空 523,024）。4 个 PAK 随更新变化：
  `800config`（1 个 size）、`config`（1,330 条路径大小写 + 2 个内容）、
  `magic5`（10 个 APG size/内容）、`ui`（1 个 CSV + 3 个零长条目）。
  已重导 14 个变化叶子（`tools/reexport_changed.py`，0 错误）、刷新
  88 个 magic5 APG 帧、1,940 个 SCP JSON 与 include 图。MSK/core 消费端
  主链路在当前二进制复核：解析器 `0x4272d0`、ApplyMask `0x4266d0`、
  深度测试 `0x426756`、暗化 `0x4264b0`、group/frame `0x426980`。
  同时修正 overclaim：APCI `entry.a` 41.9% entry 越界、真实 PADDUSB 存在于
  颜色偏置路径、CTI 验证脚本默认筛灰度、FSM 名称 51 个；详见
  `docs/VERIFY-2026-10-23.md`。pinned 快照已导入新 Ghidra 项目
  `ghidra-proj/dhsh-7a5e`，并用 `kimi_ghidra_batch.py` 复核关键函数与
  vtable 0x9809f8（+0x18/+0x1c/+0x20 = 0x41e760/0x41e780/0x41e790）。

- **MSK/core 消费端语义（行为已闭合；地址已迁移到当前 SHA）**：
  ApplyMask 当前入口 `0x4266d0`——flag 检查 → 地线深度测试
  `y < ((f6*x)>>8)+sortY`（`0x426756`）→ 矩形相交 → RLE 逐行 → MMX 暗化
  （`psraw 3`），**遮挡 = 角色被盖部分变暗（ghost），非重画背景，不参与
  点击判定**；渲染调用点 `0x81ae3b`/`0x81b0c6` → vtable+0x18 包装
  `0x41e760`；flag/group/frame = 帮派建造阶段遮罩帧
  （`0x426980` ← `0x41f2b0` ← `0x410000` ← 主分发器 `0x7a2240`
  case `0x3077` @ `0x7a339c` ← 帮派 INI）；容器 `+0x40` 禁用开关无写入者（旧基线字节扫描）
  → **遮挡常开**；动态遮罩旧基线 `FUN_00427740`；f3/f5 解析器不读。
  运行时只读验证：2026-10-23 用修正版 `tools/run_mask_peek.py` 在 YingTianFu
  复跑 **PASS**（placement_count=563=vector、cap100/50=100/50、staging=0、
  disable=0，563 元素全部 active=1 group=0，7 个关键地址字节与 pinned 快照
  一致），明细 `manifests/batches/mask-peek-YingTianFu.json`；
  同 SHA 的 XingXiuCun 二次复跑（386/386，caps/disable/关键字节同样一致）
  见 `mask-peek-XingXiuCun.json`（`docs/formats/map.md` §4.7）。
- **语义深化（2026-10-21，含 2026-10-23 修正）**：
  **AG offset** = 帧数据 delta 长度（迭代器 `FUN_0082cd70` 累加，非指针；
  数据侧已用真实 APG 前 8 帧 34B 累加=header 272 复核）；
  **CTI table2** = 256×{H,S,V,pad} HSV 染色主表，table1 = 默认 RGB565，
  选择链 `_UseColorTable`→`0x825630/0x8255c0/0x824e00/0x825400`（当前 SHA，旧基线 +0x140）；
  灰度 CTI（player6 harmor*，S=0）`hue0(table2)==table1` **255/255**，
  `kimi_verify_cti.py` 默认只筛灰度（`--all` 才随机全量）；
  **混合模式** = 精灵逐像素 alpha 合成，**未观察到层间加法混合**；
  真实 `PADDUSB` 的调用者 `0x820050` 已复核为纯色填充/偏置路径，
  不是层间加法混合；旧“全二进制无 PADDUSB”表述已修正；
  **APCI 字段** = 布局/公式已验证；`entry.a` 仅部分 entry 落在
  `FUN_007ed6d0` 的 4 个区间（41.9% entry/30.7% item 越界），需按 record
  类型区分；`mid.header` 方向数按 item 加权 99.44% 为 8；blob 几何公式
  687,229/687,229 全量验证；消费引擎当前 SHA `0x828510` 已复核（解析器/加载器 `0x82a5a0/0x82a810`）；
  **AI 锚点** x/y = 绘制偏移（当前 SHA：装载 `0x7f8330` 取负存位图 +4/+8，
  绘制 `0x81a140` 计算 pos+anchor）；
  **SCP 消息/Lua** = `#script` 为标准 Lua（内嵌 Lua 5.x VM），当前 SHA
  含 **51 个**去重 `FSM_*` 名称（旧文档 38 个），`FSLuaWndScript.RegMsgCall` 当前 SHA `0x8fc970` 注册、C 实现挂窗口回调，
  FSLua 对象 `0x8c5720`、SCP 解析器 `0x90c040`；
  **457 个 CSV** 零硬编码引用 = 纯数据驱动（旧基线，未在本次独立复核）。

- **NZ type 0x0A = 16 位 LZW**：通过 REA + Ghidra 定位 type 0x0A 解码器
  （VA `0x8a6f60`），实现 `tools/dhsh/nz_lzw10.py` 并接入 `nz.py`。
  B1 **100/100**、B2 **1,000/1,000**、B3 定向 **3,213/3,213**、全量
  **59,222/59,222** 全部通过（0 失败，解码输出 3.34 GiB）。
- **config.pak 0x0A 首批导出**：B0 12/12、B1 100/100、全量 **1,253/1,253**
  通过，输出 14.5 MiB（`extracted/decoded/config/`）。
- **map.pak / map1.pak 0x0A 导出**：B0 25/25、B1 100/100、全量 **365/365**
  通过，输出 69.7 MiB（`extracted/decoded/map/`、`map1/`）。
- **0x0A 解码性能优化**：字典表复用 + 预分配输出 + 可选 numba JIT。
  全量校验 **709.2 s → 25.8 s**，config.pak 端到端导出 **21.7 s → 7.36 s**，
  解码吞吐 1.35 → 77.6 MiB/s。
- **ui.pak 0x0A 导出**：B0 14/14、B1 100/100、全量 **9,375/9,375**
  通过，输出 438.9 MiB（`extracted/decoded/ui/`）；numba 下全量仅 **31.9 s**。
- **npc.* / magic.* 0x0A 导出**：npc 9 个 PAK **4,232/4,232**（589.0 MiB）、
  magic 4 个 PAK **1,062/1,062**（565.1 MiB），全部 0 失败，合计 43.8 s。
- **player.* 0x0A 导出 + 0x0A 全量完成**：player.pak + player1-15
  **42,928/42,928**（APG 41,182、PIF 1,598、TGA 128、CTI 20，1,740.5 MiB，
  162.1 s）；另补齐 800config/plugins 7 个。至此 **0x0A 59,222/59,222 全部导出**。
- **0x0A APG 全量解析扫描**：47,049 个文件、449,011 帧，0 解析错误；
  AP 79,197 / AI 369,814；压缩只用 0/1/2、alpha 只用 0/2；
  **AG flags=1 ⇔ 含 AI 帧**（38,714/38,714）；CTI 覆盖 **100%**
  （369,814 个 AI 帧，0 缺失）；分层 886 帧解码 0 失败。
- **0x0A APG 批量转帧**：47,049 个 APG → **449,011/449,011 张 PNG**，
  3,175.6 MiB，0 错误，**838.6 s**（8 进程，约 535 帧/s）。NumPy 快速路径
  与参考解码器 886/886 帧逐像素一致（解码 4.8×）；PNG level 3。
- **0x0B 全量导出**：LZO1X 增加可选 numba JIT（**171 MiB/s**，约 150×），
  分组并行导出 **214,623/214,623** 个 0x0B 叶子，0 失败，解码 **4,428.5 MiB**；
  player.* 186,487 个 / 1,984.4 MiB / 528.8 s。
- **0x0B APG 批量转帧**：204,137 个 APG → **1,824,600/1,824,600 张 PNG**，
  4,748.0 MiB，0 错误，**1,977.8 s**（约 923 帧/s）；CTI 覆盖 100%
  （40 个 `Lcuff/521` 用 `Rcuff/521` sibling-palette 回退）。
- **type 0 APG 导出与转帧**：`pak_inventory.json` 全量校验通过（2026-10-23 重建后 523,213 条，非空 523,024；
  旧批次为 523,212 可读叶子口径）；inventory 驱动的 8 进程导出
  **207,811/207,811**（2,208.4 MiB，160.8 s）；扫描 1,647,239 帧、0 错误，
  CTI 覆盖 100%；转帧 **1,647,239/1,647,239 PNG**（3,064.1 MiB，0 错误，1,817.9 s）。
- **A16 全量转 PNG**：三语料扫描 29,716 个 A16（t0 20,990 / 0x0A 3,408 /
  0x0B 5,318），0 头部/尺寸错误；模式只有 comp 0/1/2/5、alpha 0/2，comp 5
  （stream1 = JPEG，40 个）已实现。`a16_fast` 与参考解码器在 89 个分层样本上
  逐字节一致；B0 40/40、B1 400/400、B1b 3,000/3,000、全量
  **29,716/29,716 PNG**（405.8 MiB，0 错误，130.6s）。
- **TGA/BMP/PSD 全量转 PNG**：三语料 **6,646/6,646**（TGA 6,549、BMP 84、
  PSD 13），0 错误，31.1 MiB，12.3s；alpha 保留（RGBA 自动判定）。
- **特殊 JPG 解码（结构层完成，已复核修正）**：510 个 `FF D8 FF C8` 文件不是标准 JPEG。
  已定位 `0x823240` MCU 驱动、`0x8a1fe0` 块熵解码、`0x8a8780` MMX IDCT、
  `0x8a32d0/0x8a38d0` RGB565 颜色路径；实现 `tools/dhsh/formats/special_jpg.py`。
  6400×6400 v2 文件 **160,000 MCU / 960,000 块 0 Huffman 错误**；v3 样本同样
  0 错误。独立复核（`docs/formats/special-jpg-kimi-review.md`）确认 IDCT 缩放
  `raw/64` + floor、颜色查表 `trunc(K·v+1)>>1` + RGB565，并修正三处块级 bug：
  v2 的 4 个 Y 块全部使用 DC slot0、AC zigzag 反序（`ZIGZAG[64-k]`）、
  IDCT 块输出需转置。修正后 v2 与复核输出平均差 1.10（61.2% 逐像素一致），
  真机世界地图并排图一致。numba 快路径（熵解码 JIT）将 6400×6400 解码从
  157 s 降到 20 s（7.75×），快/慢输出逐字节一致。**已全量导出 510/510 个
  PNG**（1,551,886,272 px，1,628.3 MiB，0 错误，198.8 s / 8 进程）。
- **PG2 容器解析与全量导出**：150 个 PG2（t0 38 / 0x0A 106 / 0x0B 6），
  头部 20 字节 + 40 字节帧表项；每帧是完整 `AP` 块，复用 A16 解码器。
  **1,606/1,606 帧导出为 PNG**（81,366,848 px，66.1 MiB，0 错误，16.1s）；
  fast 与 reference 46 个分层样本逐字节一致（`docs/formats/pg2.md`）。
- **PIF 语义与 GIF 导出**：PIF 行（28 字节）与 PG2 帧表项前 28 字节同构；
  `VA 0x824a20` 读取 `row+0x0a` 作为**帧延时（ms，默认 80）**，
  animation/1-3 与 PIF 逐帧延时完全对应。新增 `tools/export_gif.py`：
  B0 10 个动画/80 帧、B1 50 个/413 帧、animation 1-3（90 帧，3/3 使用
  PIF 延时）全部 0 错误（`docs/formats/pif.md` 第 3、7 节）。
- **S5 地图侧格式（第一段）**：解析 MAP INI、CEL 碰撞位图、MAN 动画路径表、
  MSK/core 记录容器、MLK 跳转表；**563+283+539 个文件全部通过校验，
  0 错误**。CEL 位序确认为 MSB-first、1=阻挡（边界置 1 比例 98.6% vs 91.6%）；
  **275/275 CEL 导出 PNG**（5,573,952 格，0.32 MiB，0 错误；
  `docs/formats/map.md`）。
- **S5 地图侧格式（第二段）：MSK/core 数据区破解**：数据区是 1 位前景遮罩的
  RLE，`u16 = (covered<<15) | run`，逐行不跨行；放置表 24 字节
  `record/x/y/flag/group/frame`，x/y 有符号，`f4` 为绝对偏移。加载器
  `0x27aa0` 会把 `MASKFILE` 扩展名改成 `.core`，故 `.core` 是活动格式、
  `.MSK` 是旧格式。**300/300 文件、12,755 遮罩、717,967,584 像素、
  11,421,604 runs 全量解码，0 错误**；运行时 YingTianFu 563/563 对象逐字段
  验证 `sortY = y + f2 - (x*f6>>8)`。全量导出 **12,755 张 PNG**
  （15.85 MiB，0 错误，21.3s；`tools/export_core_masks.py`）。
- **SCP UI 脚本解析**：支持 GB18030/UTF-8、`WINDOW`/`FIXEDPART` 嵌套、
  `#include`、`#ifdef/#else/#endif`（WINWIDTH800）、`#script` 块、跨行字符串。
  **1,940/1,940 文件解析并导出 JSON**（31,652 个块、519 个 include、
  16.10 MiB，0 解析错误，7.8s）；窗口类型以 BUTTON 10,035、STATIC 8,116、
  WIN 4,962 为主。`BGPIC { ... }` 已结构化（7,435 个活动块，键
  BGTYPE/SECTION/TEXT/FONTTYPE/TITLE1-9 等）；**include 图 519 条边全部解析
  （183 同语料、336 跨语料、0 缺失、6 重复、0 环，最大闭包 3）**
  （`docs/formats/scp.md`）。
- **perinfo.pak / APCI**：完整解析。旧基线加载器 `0x82a6d0`、解析器 `0x82a460`
  （当前 SHA `0x82a810`/`0x82a5a0`）；
  group(u16 id + u16 count) → record(36B) → entry(8B: u32 a/u32 b) →
  mid(4B header) → item(u32 x/u32 y + blob)。blob 大小按
  `y` 高字节分支并对乘积做 16 位截断；**20 个 group、219 record、
  687,229 item、432,838 blob 恰好解析到文件末字节**，全量导出
  `extracted/apci/perinfo/`（items.csv 49.3 MiB、blobs.bin 1.77 MiB），
  0 错误（`docs/formats/apci.md`）。
- **NZ type 0x0B = LZO1X**：通过 REA + Ghidra 定位 `Game.exe` 的 NZ 解码器
  （VA `0x82d1f0`）与 type 0x0B 解码器（VA `0x8a6c90`），实现
  `tools/dhsh/nz_lzo.py`。样本验证 **133/133**、B1 随机验证 **100/100**。
- **A16 图像解码**：RGB565 + RLE16（compression 2）+ RLE8 alpha（alpha_mode 2）
  已实现（`tools/dhsh/formats/a16.py`）。B0 **23/23**、B1 **100/100** 通过，
  已生成带透明通道的 PNG 样例。
- **CTI**：20/20 头部/尺寸验证；table1/table2 语义待定，但不影响 A16 comp 2。

## 快速状态

- ✅ **已完成**：PAK 清单；NZ 类型全量统计；全部非 APG 的 type=0 资源导出；Lua/SCP/MAP/INI 文本验证；118 个 TGA 与 34 个标准 JPG 可打开；A16 扫描 29,716/29,716 且**全量转 PNG 29,716/29,716**；**TGA/BMP/PSD 全量转 PNG 6,646/6,646**；NZ 0x0A LZW 解码器全量 59,222/59,222 并全量导出；NZ 0x0B LZO1X 全量导出 214,623/214,623（解码 4,428.5 MiB）；**三种 APG 语料导入/转帧完成（3,920,850 PNG）**；2026-10-23 重建 `pak_inventory.json`（523,213 条，非空 523,024）；SCP 1,940 全量 JSON + BGPIC + include 图；**perinfo.pak APCI 完整解析与导出**；510 个特殊 JPG 全量解码/导出；MSK/core 遮罩 RLE 破解并全量导出；**MSK/core 消费端语义（行为与运行时数字均已复核：修正版 peek PASS）**；**语义深化（含修正）：AG delta、CTI 灰度 hue0、SCP 消息/Lua 桥、CSV（数据驱动）；APCI entry.a/AI 锚点/混合模式仍有边界项**；**资源查看器 kimi_viewer（可插拔格式 + mmap/LRU/后台解码性能优化）**；**资源统计工具 kimi_res_stats.py（41 PAK / 523,213 条索引全量分布）**；**网络协议破解：s2c 全离线解密（静态密钥表 0xa43eb8 + 逐帧自包含算法，1031 帧实测），c2s 会话密钥加密 + Frida 双向明文 hook，10-21 更新致代码段 +0x120 平移已用字节签名自动校准 → docs/formats/net-protocol.md**。
- 🟡 **部分完成**：PIF x/y 偏移、帧延时与 count=2 归一化均已确认；MAN `f48/f4c/f50/f58/f5c/f60` 已闭合（排序偏置/斜率、启用、对象键、定时重置），`f54` 未发现读取点；MLK `paired`/入口框已闭合并进入预览，`extra0..2`/`dest` 运行时待确认；APCI `entry.a` 已按 record 家族分类，12B 单元布局已闭合，消费端逐槽取用待确认。
- ❌ **未完成**：加密/未知数据块（`ACDC.dat`、`ccore.dat`、`cltgp.dat`、`sl.dat`、`001.cpm`，已评估为反外挂/小配置，不影响还原）；1 个 ZIP 未展开（标准容器，可解）。

## 总览

| 指标 | 数值 |
|---|---:|
| 顶层 `.pak` | 41 个（40 个 PAKF + `perinfo.pak` APCI；另有 4 个更新补丁包） |
| PAKF 归档 | 40 个 |
| PAK 叶子资源 | 523,213（含 189 个 size=0；非空 523,024） |
| NZ type 0（可直接导出） | 249,185（47.6%），约 2.85 GiB |
| NZ type 0x0A（16 位 LZW） | 59,218（11.3%），约 2.51 GiB |
| NZ type 0x0B（LZO1X） | 214,620（41.0%），约 3.60 GiB |
| 非 NZ / 零长度 | 1 个非 NZ（perinfo.pak）；189 个 size=0 |
| 已导出/解码文件（含 PNG） | 4,498,778 个，约 23.4 GiB |
| 已解码导出的 type 0 叶子 | 249,361 / 249,361（**100%**） |
| 其中非 APG 的 type 0 | 41,550 / 41,550（**100%**） |
| 其中 APG 的 type 0 | 207,811 / 207,811（**100%**） |
| 0x0A 全量校验 | **59,222/59,222**，0 失败；解码输出 3.34 GiB（8 进程 25.8s，numba；纯 Python 为 709s） |
| 0x0B 全量导出 | **214,623/214,623**，0 失败，解码 4,428.5 MiB |
| A16 全量转 PNG | **29,716/29,716**，0 失败，405.8 MiB |
| TGA/BMP/PSD 转 PNG | **6,646/6,646**（TGA 6,549、BMP 84、PSD 13），0 失败，31.1 MiB |
| PG2 转 PNG | **1,606/1,606 帧**（150 文件），0 失败，66.1 MiB |
| GIF 样本 | 63 文件 / 583 帧（B0 10、B1 50、animation 1-3），0 失败 |
| 地图 CEL 转 PNG | **275/275**，5,573,952 格，0 失败，0.32 MiB |
| MSK/core 遮罩转 PNG | **12,755/12,755**（300 文件），717,967,584 px，0 失败，15.85 MiB |
| SCP 解析 | **1,940/1,940**，31,652 块，519 include，0 解析错误，16.10 MiB JSON |
| APCI（perinfo.pak） | **20 group / 219 record / 687,229 item / 432,838 blob**，恰好到文件末尾，0 错误 |
| APG B0/B1/B2 | B0 13 动画/181 帧；B1 53 动画/470 帧；B2 134 动画/1,000 帧；全部解码；B2 含 AI 257 帧，34/34 CTI 解析成功 |
| 仍被 codec 阻塞的叶子 | 0（0x0A/0x0B 均已具备解码能力） |

> 结论：**所有非 APG 的 type=0 资源已全部导出**；**type 0x0A（16 位 LZW）与
> type 0x0B（LZO1X）编解码均已破解并全量导出**；三种 APG 语料共产生
> 3,920,850 张帧 PNG，A16 全量转 PNG（29,716）与 TGA/BMP/PSD（6,646）完成。

## NZ 类型总览

| 类型 | 叶子数 | 占比 | 字节 | 状态 |
|---|---:|---:|---:|---|
| `0x0000` 原样存储 | 249,185 | 47.6% | 约 2.85 GiB | 已可导出；非 APG 已全部导出 |
| `0x000A` 16 位 LZW | 59,218 | 11.3% | 约 2.51 GiB | ✅ 已破解（`0x8a6f60`），B1/B2/B3 全通过 |
| `0x000B` LZO1X | 214,620 | 41.0% | 约 3.60 GiB | ✅ 已破解（`0x8a6c90`），B1 100/100 |
| 非 NZ | 1 | <0.1% | 7.4 MiB | `perinfo.pak`（APCI） |
| size=0 | 189 | <0.1% | 0 | 空/占位条目，单列统计 |

> 2026-10-23 重建 inventory 后，旧的 249,361/59,222/214,623/6 口径不再直接可比：
> 旧计数把部分 size=0/占位记录计入 NZ 类型，新索引将其单列。非空叶子为 523,024。

## 分类进度

| 类别 | 叶子总数 | type 0 | type 0A | type 0B | 已导出 type 0 | type 0 剩余 |
|---|---:|---:|---:|---:|---:|---:|
| 角色 | 445,999 | 216,578 | 42,928 | 186,487 | 18,579 | 197,999（APG） |
| UI/图标 | 33,502 | 21,610 | 9,375 | 2,517 | 21,081 | 529（APG） |
| NPC | 30,915 | 8,697 | 4,232 | 17,986 | 258 | 8,439（APG） |
| 地图 | 5,893 | 872 | 365 | 4,652 | 872 | 0 |
| 配置/数据/脚本 | 3,159 | 1,103 | 1,256 | 621 | 1,278 | 0（旧口径含 alias） |
| 技能/特效 | 2,695 | 309 | 1,061 | 1,325 | 121 | 188（APG） |
| UI 背景 | 658 | 5 | 0 | 653 | 5 | 0 |
| 表情动画 | 226 | 9 | 0 | 217 | 9 | 0 |
| 插件/UI | 165 | 2 | 1 | 162 | 2 | 0 |
| **合计** | **523,213** | **249,185** | **59,218** | **214,620** | **42,205** | **206,980** |

> type 0/0A/0B 计数为 2026-10-23 重建 inventory 后的值；`已导出 type 0` 列
> 仍是旧语料统计（含 size=0/alias），两者口径不同，待按新 PAK 重导后统一。

## 格式进度

> 0x0A（16 位 LZW）与 0x0B（LZO1X）均已具备解码能力，但尚未批量导出；
> “已导出”列只统计实际导出文件。0x0A 定向验证（B3）覆盖全部 1,945 个 PIF、
> 625 个 SCP、420 个 CSV、174 个 MLK、13 个 PSD 等稀有脚本/数据扩展名。

| 扩展名 | 总数 | type 0 | type 0A | type 0B | 已导出 | 已解析/验证 | 状态 |
|---|---:|---:|---:|---:|---:|---|---|
| `.apg` | 458,997 | 207,811 | 47,049 | 204,137 | 655（样本） | AG/AP/AI；AI+CTI；AG offset=delta 长度；锚点=绘制偏移 | 🟢 三语料全量转帧 3,920,850 PNG；语义已闭合 |
| `.a16` | 29,716 | 20,990 | 3,408 | 5,318 | 20,990 | comp2/alpha0,2 B1 100/100 | 🟢 可转 PNG；comp3/4/5/7 待做 |
| `.cti` | 18,387 | 18,292 | 20 | 75 | 18,292 | 头部 20/20；table1=默认 RGB565、table2=HSV 染色主表 | 🟢 语义已闭合（hue/sat/val 选择链 + 实证） |
| `.tga` | 6,549 | 118 | 5,317 | 1,114 | 118 | 全量转 PNG（含 0A/0B） | 🟢 6,549/6,549 |
| `.dat` | 4,642 | 54 | 1,945 | 2,643 | 54 | PIF 解析（type 0/0x0A/0x0B）；f5=帧延时 | 🟡 GIF 样本导出完成；其余字段待定 |
| `.scp` | 1,940 | 732 | 625 | 583 | 732 | 1,940/1,940 JSON；BGPIC/include 图；消息绑定/Lua 桥已闭合 | 🟢 语义已闭合（FSM_* 38 消息 + FSLua* 桥） |
| `.jpg` | 544 | 544 | 0 | 0 | 544 | 34 标准 + 510 特殊头 | 🟢 特殊 JPG 已全量导出 |
| `.csv` | 457 | 0 | 420 | 37 | 0 | 420/420 文本解码 | 🟢 0x0A/0x0B 可解码；GBK/GB18030/UTF-8 混杂 |
| `.map` | 305 | 292 | 0 | 13 | 292 | 文本 INI 验证 | 🟢 单图可用；0x0B 可解码 |
| `.man` | 277 | 172 | 20 | 85 | 172 | 100 字节记录：path/x/y/6 字段 | 🟡 位置/预览已闭合；其余字段待定 |
| `.cel` | 275 | 0 | 57 | 218 | 0 | MSB-first 碰撞位图 | 🟢 275/275 全量转 PNG |
| `.core` | 274 | 32 | 24 | 218 | 32 | `MSK` 头 | 🟢 RLE + 消费端语义闭合（ghost 暗化/深度测试/帮派帧；含运行时验证） |
| `.ini` | 231 | 183 | 3 | 45 | 183 | 文本可读 | 🟢 0x0A 3/3（2 个为 `lnk` 别名） |
| `.mlk` | 228 | 52 | 174 | 2 | 52 | 108 字节跳转记录 | 🟡 `value`/`paired`/入口框已闭合；`extra`/`dest` 待定 |
| `.pg2` | 150 | 38 | 106 | 6 | 38 | `PG` 头 + 40B 帧表 | 🟢 1,606/1,606 帧全量转 PNG |
| `.bmp` | 84 | 0 | 2 | 82 | 0 | 全量转 PNG（含 0A/0B） | 🟢 84/84 |
| `.lua` | 46 | 7 | 18 | 21 | 7 | GBK 源码解码 | 🟢 0x0A/0x0B 可解码；18 个 0x0A 文本验证 |
| `<none>` | 45 | 28 | 0 | 17 | 28 | 多为 CVS 元数据 | 🟢 0x0B 可解码 |
| `.msk` | 26 | 15 | 8 | 3 | 15 | `MSK` 头 | 🟢 旧格式；RLE 同 core，语义同 core 闭合 |
| `.txt` | 22 | 0 | 13 | 9 | 0 | 13/13 文本解码 | 🟢 0x0A/0x0B 可解码 |
| `.psd` | 13 | 0 | 13 | 0 | 0 | `8BPS` | 🟢 已随 TGA/BMP/PSD 批次全量转 PNG（6,646 含） |
| `.db` | 2 | 0 | 0 | 2 | 0 | `Thumbs.db` | 🟢 Windows 缩略图缓存，非游戏资源 |
| `.gal` | 1 | 0 | 0 | 1 | 0 | `AL`/A16，`stream1=0` 纯 alpha | 🟢 RLE8 alpha 解出（4736×2944，云/雾遮罩） |
| `.zip` | 1 | 1 | 0 | 0 | 1 | 有效 ZIP（cloth/907，APG/CTI） | 🟡 标准容器可解；未展开导出 |

另：`perinfo.pak`（7.4 MB）不是 PAKF，而是 `APCI` 容器，已完整解析并导出
（`docs/formats/apci.md`）。

## 已导出目录

| 目录 | 文件数 | 内容 |
|---|---:|---|
| `extracted/easy/` | 1,540 | type 0 文本/配置/脚本（Lua/SCP/INI/MAP/MLK/MAN/CORE/MSK/DAT/ZIP） |
| `extracted/images-standard/` | 662 | TGA/JPG（501.3 MiB） |
| `extracted/images-proprietary/` | 39,282 | type 0 A16/CTI（185.8 MiB） |
| `extracted/samples/apg/` | 655 | type 0 APG 样本（20.4 MiB） |
| `extracted/samples/compressed/` | 400 | 0x0A/0x0B 原始 NZ，供 codec 分析（25.5 MiB） |
| `extracted/decoded/config/` | 1,253 | config.pak 的 0x0A 全量解码（CSV/SCP/Lua/TXT/MLK/PIF/APG/A16/INI，14.5 MiB） |
| `extracted/decoded/map/` + `map1/` | 365 | map.pak/map1.pak 的 0x0A 全量解码（CEL/MAN/core/MSK/TGA/A16/PIF/APG，69.7 MiB） |
| `extracted/decoded/ui/` | 9,375 | ui.pak 的 0x0A 全量解码（TGA/A16/APG/PIF/PG2/PSD/CSV/BMP/TXT/SCP，438.9 MiB） |
| `extracted/decoded/npc*/` | 4,232 | npc.pak + npc1-9 的 0x0A 全量解码（APG/TGA/PIF，589.0 MiB） |
| `extracted/decoded/magic*/` | 1,062 | magic.pak + magic1-5 的 0x0A 全量解码（APG/PIF，565.1 MiB） |
| `extracted/decoded/player*/` | 42,928 | player.pak + player1-15 的 0x0A 全量解码（APG/PIF/TGA/CTI，1,740.5 MiB） |
| `extracted/decoded/800config/` + `plugins/` | 7 | 0x0A 收尾（SCP 6、Lua 1） |
| `extracted/apg_frames/` | 449,011 | 0x0A APG 帧 PNG（449,011 帧，3,175.6 MiB） |
| `extracted/decoded_0b/` | 214,623 | 0x0B 全量解码（APG 204,137、A16 5,318、PIF 2,643、TGA 1,114 等，4,428.5 MiB） |
| `extracted/apg_frames_0b/` | 1,824,600 | 0x0B APG 帧 PNG（1,824,600 帧，4,748.0 MiB） |
| `extracted/decoded_t0/` | 207,811 | type 0 APG 原始导出（207,811 个，2,208.4 MiB） |
| `extracted/apg_frames_t0/` | 1,647,239 | type 0 APG 帧 PNG（1,647,239 帧，3,064.1 MiB） |
| `extracted/misc/` | 66 | type 0 PG2 + 无扩展名文件（23.8 MiB） |
| `extracted/a16_png/` | 29,716 | 三语料 A16 全量转 PNG（t0/0a/0b，405.8 MiB） |
| `extracted/std_png/` | 6,646 | 三语料 TGA/BMP/PSD 全量转 PNG（31.1 MiB） |
| `extracted/pg2_png/` | 1,606 | 三语料 PG2 全量转 PNG（150 文件，66.1 MiB） |
| `extracted/map_cel_png/` | 275 | 地图 CEL 碰撞位图 PNG（5,573,952 格，0.32 MiB） |
| `extracted/scp_json/` | 1,940 | SCP UI 脚本解析 JSON（31,652 块，16.10 MiB） |
| `extracted/apci/perinfo/` | 219 | APCI 记录/blob（items.csv + blobs.bin 1.77 MiB） |
| `extracted/special_jpg_png/` | 510 | 特殊 JPG 全量 PNG（1,551,886,272 px，1,628.3 MiB） |
| `extracted/core_masks/` | 12,755 | MSK/core 前景遮罩 PNG（717,967,584 px，15.85 MiB） |
| **合计** | **4,498,778** | **约 23.4 GiB 有效载荷** |

对应清单：

- `manifests/easy-extract.csv`
- `manifests/images-standard.csv`
- `manifests/images-proprietary.csv`
- `manifests/samples-apg.csv`
- `manifests/samples-compressed.csv`
- `manifests/misc.csv`
- `manifests/batches/nz0a-config-b0.csv`、`nz0a-config-b1.csv`、`nz0a-config-full.csv`
- `manifests/batches/nz0a-map-b0.csv`、`nz0a-map-b1.csv`、`nz0a-map-full.csv`
- `manifests/batches/nz0a-ui-b0.csv`、`nz0a-ui-b1.csv`、`nz0a-ui-full.csv`
- `manifests/batches/nz0a-npc-b0.csv`、`nz0a-npc-b1.csv`、`nz0a-npc-full.csv`
- `manifests/batches/nz0a-magic-b0.csv`、`nz0a-magic-b1.csv`、`nz0a-magic-full.csv`
- `manifests/batches/nz0a-player-b0.csv`、`nz0a-player-b1.csv`、`nz0a-player-full.csv`
- `manifests/batches/nz0a-misc-full.csv`
- `manifests/batches/apg-frames-b0.csv`、`apg-frames-b1.csv`、`apg-frames-b1b.csv`、`apg-frames-full.csv`
- `manifests/batches/nz0b-b0-*.csv`、`nz0b-b1-*.csv`、`nz0b-*-full.csv`
- `manifests/batches/apg-0b-frames-b0.csv`、`apg-0b-frames-b1.csv`、`apg-0b-frames-b1b.csv`、`apg-0b-frames-full.csv`
- `manifests/batches/inventory-validation.json`
- `manifests/batches/nzt0-b0.csv`、`nzt0-b1.csv`、`nzt0-full.csv`
- `manifests/batches/apg-t0-frames-b0.csv`、`apg-t0-frames-b1.csv`、`apg-t0-frames-b1b.csv`、`apg-t0-frames-full.csv`
- `manifests/batches/a16-scan.json`、`a16-fast-verify.json`
- `manifests/batches/a16-b0.csv`、`a16-b1.csv`、`a16-b1b.csv`、`a16-full.csv`
- `manifests/batches/std-b0.csv`、`std-b1.csv`、`std-b1b.csv`、`std-full.csv`
- `manifests/batches/pg2-b0.csv`、`pg2-b1.csv`、`pg2-full.csv`
- `manifests/batches/gif-b0.csv`、`gif-b1.csv`、`gif-anim.csv`
- `manifests/batches/map-scan.json`
- `manifests/batches/map-cel-b0.csv`、`map-cel-b1.csv`、`map-cel-full.csv`
- `manifests/batches/scp-scan.json`
- `manifests/batches/scp-json-b0.csv`、`scp-json-b1.csv`、`scp-json-full.csv`

## 已验证内容

- **Lua**：7 个 type=0 源码均可按 GBK 解码（如 `magic/magicFormulas.lua`、`UI/main/activebtn.lua`）。
- **MAP**：292 个文本 INI，含 `[base]`/`WIDTH`/`HEIGHT`/`BACKGROUND`。
- **SCP**：731/732 个 type=0 文件可读（`WINDOW TYPE`、`#include`、`//` 注释等）。
- **TGA**：118/118 可被 Pillow 打开。
- **JPG**：34 个标准 JPEG 可打开；510 个特殊头 JPG（地图/小地图/系统等）使用 `FF D8 FF C8` 私有头，Pillow 无法识别。
- **A16/CTI**：20,990 个 A16 全部以 `AP` 开头；18,292 个 CTI 全部以 `CT` 开头。
- **A16 解码**：compression 0/1/2/5 + alpha_mode 0/2 已实现；B0 23/23、B1 100/100 通过；RGB565 颜色、透明背景与边缘均正确；样例在 `extracted/_batches/s3-b0/`。
- **A16 全量**：三语料 29,716 个文件、875,333,120 像素，0 头部/尺寸错误；comp 0=131、1=14、2=29,531、5=40（JPEG），alpha 0=1,863、2=27,853；fast 与 reference 89/89 逐字节一致；全量转 PNG **29,716/29,716**（405.8 MiB，130.6s）。
- **标准图片**：TGA **6,549**、BMP **84**、PSD **13**（合计 6,646）全部由 Pillow 打开并转 PNG，0 错误，31.1 MiB；TGA 含 type 2/10，PSD 为 8BPS；alpha 视源图自动保留 RGBA/RGB。
- **PG2**：150 个文件（38/106/6）、1,606 帧，头部/帧表 0 错误；帧块全部为 `AP`（comp 1=6、2=1,165、5=435，alpha 全为 2）；`data_size == 24+s1+s2` 全部成立；全量转 PNG 0 错误。
- **PIF/GIF**：PIF 行 28 字节 = 14 个 u16，`row+0x0a`（f5）= 帧延时 ms，默认 80（VA `0x824a20`）；animation/1-3 的 GIF 总时长与 PIF 完全一致；`tools/export_gif.py` 按 APG 帧头 x/y 合成画布，B0/B1/动画样本全部 0 错误。
- **地图侧格式**：`.map` 305、`.cel` 275、`.man` 277、`.msk` 26、`.core` 274、`.mlk` 228，全部通过头部/尺寸/表结构校验（0 错误）；CEL 位序 MSB-first、1=阻挡（边界置 1 98.6% vs 91.6%）；275/275 CEL 转 PNG。
- **SCP**：1,940 个文件（GB18030 1,720、UTF-8 220），268,195 行；27,698 个 WINDOW、3,954 个 FIXEDPART、519 个 include；支持 `#ifdef WINWIDTH800`、`#script`、跨行字符串；全量导出 JSON 0 解析错误（9 个条件编译/END 告警）。
- **NZ 0x0B**：LZO1X 解码器已实现（`tools/dhsh/nz_lzo.py`）；样本 133/133、B1 100/100 通过；解压后可得到 A16/APG/TGA/SCP 等原始内容。
- **NZ 0x0A**：16 位 LZW 解码器已实现（`tools/dhsh/nz_lzw10.py`），接入 `nz.py`；B1 100/100、B2 1,000/1,000、B3 3,213/3,213 通过；全量 59,222/59,222 长度/格式校验通过，0 失败，并已全量导出（解码输出 3.34 GiB）。
- **文本编码**：0x0A 文本以 GBK 为主，GB18030 覆盖其扩展区；个别新 UI SCP 为 UTF-8；CP936 独立 `0x80`（欧元）已单独处理；`.mlk`/部分 `.ini` 别名是 `lnk\0` 二进制。
- **CTI**：20/20 头部/尺寸验证；table1=512B、table2=1024B；语义待定。
- **PAK inventory**：2026-10-23 重建后 **523,213** 条（189 个 size=0；非空 523,024），offset/head/size/path 由当前 PAK 重新生成；旧校验记录的 523,212/55 口径见历史条目（`manifests/batches/pak-update-2026-10-21.json`）。
- **APG**：655/655 样本以 `AG` 开头；三种语料（type 0/0x0A/0x0B）全部解析，模式/CTI 覆盖 100%，共 3,920,850 张帧 PNG。
- **ZIP**：1 个有效 ZIP。

## 已知问题

1. **特殊 JPG 已完成**：510/510 解码并导出 PNG；IDCT/颜色已按 MMX 与运行时表复核，剩余可选项为位精确 MMX IDCT（`docs/formats/special-jpg.md`）。
2. **0x0A/0x0B 已全量导出**：旧口径 0x0A 59,222 + 0x0B 214,623，0 失败；2026-10-23 重建索引后为 0x0A 59,218 / 0x0B 214,620（size=0 单列），实际内容未减少；三种 APG 语料亦已全部转帧，剩余工作为语义解析、标准图片转码与容器格式。
3. **A16 模式已闭合**：全量扫描只出现 compression 0/1/2/5 与 alpha_mode 0/2，均已实现；3/4/7/11 与 alpha 1/3/4/5 在 29,716 个文件中不存在。
4. **APG 三种语料已全部导出并转帧**：type 0 207,811 + 0x0A 47,049 + 0x0B 204,137，共 **3,920,850 张 PNG**；~~AG offset、CTI table2、混合模式~~语义已闭合（2026-10-21）。
5. **perinfo.pak APCI 已解析并赋予字段语义**：group(4B)/record(36B)/entry(8B)/mid(4B)/item(8B+blob) 全部对齐；`mid.header=方向数<<16|每方向帧数`（item 加权方向 8 占 99.44%）、blob 几何公式全量验证；消费引擎 `FUN_008283d0` 已按旧 SHA 定位。**修正**：`entry.a` 并非统一动作 ID——按 (name,entry) 去重 22,305 个 entry 中 9,355（41.94%）不在 `FUN_007ed6d0` 的 4 个区间内；2026-10-23 家族分类显示 `npc*` 100% 使用 10k–39k 段、基础 player 约 99% 在 100–9,999、编号 player 套装混用 20k/50k/60k/101k（`tools/classify_apci_entry_a.py`、`manifests/batches/apci-entry-a-families.csv`；`docs/formats/apci.md`）。剩余：12B 锚点组逐槽位语义、各家族具体 lookup 表。
6. **PAK size=0 别名**：部分条目（如 `.ini`）的 `size=0`，实际数据与同偏移的 `.map` 条目共享；已按 NZ 头 `size2` 解析长度。参见 `tools/dhsh/cli.py` 的 `_leaf_info`。
7. **文本解析**：SCP/Lua/MAP 只是导出与初步验证，尚无完整语义/UI 树解析。
8. **非 PAK 资源已建立索引**：`tools/scan_nonpak.py` 覆盖 `sound/`、
   `font/`、`animation/`、`update/skin/`，510 文件 / 189.5 MiB /
   0 错误；逐文件 size/SHA/音频/字体/图像元数据，详见
   `docs/formats/nonpak.md`。剩余：音效/音乐 ID 与 MAP/SCP 的绑定。
   `save/` 为可变用户配置，未纳入索引。
9. **非 ASCII 资源名**：PAK 名字段按 Latin-1 解码，GBK 文件名（如 `说明.txt`、`全服梦想奖励.psd`）在导出路径中显示为乱码；文件内容正常。后续可在 `tools/dhsh/pak.py` 用 GBK→Latin-1 fallback 修正并重导受影响的批次。
10. **PG2 已完成**：150 个文件、1,606 帧全部解析并转 PNG；帧表前 12 字节语义待定，不影响导出。
11. **MSK/core 数据区与消费端均已闭合（行为）**：`u16=(covered<<15)|run` 行内 RLE，300/300 文件、12,755 遮罩全量导出 PNG；当前 SHA `7a5e…` 复核 ApplyMask `0x4266d0`（ghost 暗化 + 地线深度测试 `0x426756`）、group/frame `0x426980`；修正版 `run_mask_peek.py` 已于 2026-10-23 运行时复验 **PASS**（563/563、cap100/50、staging=0、disable=0、7/7 关键字节一致；`docs/formats/map.md` §4.7）。放置表全量静态扫描（`tools/scan_core_placements.py` → `manifests/batches/core-placement-scan.json`，300/300 文件、16,114 放置、0 错误）显示只有 `BangPai_1` 两个版本含 `flag=0/group/frame` 播放项（共 105 个），其余 298 个文件全部静态；播放语义已闭合：不是时间动画，`group`=建筑类型、`frame`=状态等级，由命令 `0x3077` 驱动（§4.9）。MAN x/y、MLK `paired`/入口框已闭合并进入预览（`--man`/`--links`）；剩余 MAN `f54`（疑遗留）、MLK `extra`/`dest`；PIF 与 12B 布局已闭合。
12. **SCP 深层语义已闭合**：窗口树/属性/BGPIC/include 图已结构化为 JSON；
    `#script` = 标准 Lua（内嵌 Lua 5.x VM），当前 SHA 含 51 个去重 `FSM_*`
    消息名（旧文档写 38 个），`FSLuaWndScript/FSLuaSystem/FSLuaInstall` C 桥
    已按旧 SHA 定位（`docs/formats/scp.md` §7）；剩余可选项为 UI 布局渲染
    （查看器的一部分）。
13. **2026-10-21 客户端更新（1.50.20 → 1.50.22）**：`Game.exe` SHA
    `f6bf…` → `7a5e…`；4 个 PAK 变化（`config` 1,330 条路径大小写、
    `magic5` 10 个 APG、`ui` 1 个 CSV、`800config` 1 个 SCP）。
    已重建 `pak_inventory.json`（523,213 条）、重导 14 个变化叶子、刷新
    88 个 magic5 APG 帧与 SCP JSON/include 图。**Game.exe 可能在每次运行后
    被启动器/保护组件改写**：已新增 `tools/snapshot_gameexe.py`，当前
    `7a5e…` 已固化到 `extracted/gameexe-snapshots/`；以后每次运行后先跑
    该工具，比对字节/关键地址再判断静态地址是否仍可用。旧文档中的静态
    地址/运行数字若未迁移，以 `docs/VERIFY-2026-10-23.md` 为准。

## 下一批工作（按 `docs/PLAN.md`）

> 严格遵守 `docs/PLAN.md` 的“分批执行规范”：先 10–20 个样本，再 100 → 1,000 → 全量；任何失败率超标立即停止。

1. A16（29,716）、TGA/BMP/PSD（6,646）、PG2（1,606 帧）已全部转 PNG；PIF 延时与 GIF 导出完成；地图 CEL（275）与 MSK/core 遮罩（12,755）已转 PNG；SCP 1,940 个已解析为 JSON；APCI 已完整解析导出。
2. **特殊 JPG 已完成**：510/510 PNG 全量导出（`extracted/special_jpg_png/`）；可选位精确 IDCT。
3. ~~MSK/core 数据区/消费端语义~~（行为已闭合；当前 SHA 主链路已复核，修正版运行时复跑 PASS）；~~MAN/MLK 与地图渲染关联~~ **已完成**（`--man`/`--links`，`docs/formats/map.md` §3/§5/§8）；剩余 MAN 字段/perinfo 锚点、MLK `extra0..2`/`dest`。
4. S10：加密/未知数据块（`ACDC.dat`、`ccore.dat`、`cltgp.dat`、`sl.dat`、`001.cpm`）。
5. 语义深化：AG offset、CTI 灰度 hue0、SCP 消息/Lua 桥、APCI `entry.a` 家族分类、PIF 行布局、MAN 排序/定时字段均已闭合；剩余 APCI 12B 单元消费端逐槽取用、各家族具体 lookup 表、AI 锚点在新 SHA 上的复核、混合模式证据表述修正。
6. ~~非 PAK 资源索引~~ **已完成**：510 文件 / 189.5 MiB / 0 错误
   （`tools/scan_nonpak.py`，`docs/formats/nonpak.md`）；剩余音效/音乐
   ID 与 MAP/SCP 的消费端绑定。
7. 资源查看器 `kimi_viewer` 已可用（52 万条目、插件、批量导出、索引自愈）；剩余：忽略/清理生成缓存、更新 `优化.md`、补 MSK/core 与 SCP 插件。
8. CEL 地面层、左上补齐与 `TingYuan_*` 的 (0,0) 背景偏移均已确认并落入预览；
   剩余：第二张辅助层用途、`JiaYuan_1_1` 的 CEL>地图格异常图。
9. ~~**`FuXianDao.gal`（唯一 `.gal`）**~~ **已解**：`AL`/A16 头、
   `stream1=0` 表示纯 alpha（comp 7 不参与），`stream2=693,184 B` 按
   RLE8 解出 13,942,784 字节（4736×2944，77.88% 不透明）；导出
   `extracted/gal_png/`，工具 `tools/export_gal.py`，清单
   `manifests/batches/gal-scan.csv`（1/1，0 错误）。详见
   `docs/formats/a16-cti.md` §2。

## 更新记录

- 2026-10-19：完成 PAK/NZ 统计（523,212 叶子）；导出全部非 APG 的 type=0 资源；建立 400 个压缩样本；验证文本/图片/动画头；REA + Ghidra 可用。
- 2026-10-19 20:40：破解 NZ 0x0B（LZO1X），实现 `nz_lzo.py`，B1 100/100；实现 A16 RGB565/RLE16/RLE8 alpha 解码，B0 23/23、B1 100/100；CTI 头部 20/20；新增 `docs/formats/nz.md`、`docs/formats/a16-cti.md`。B1 报告：`manifests/batches/s3-b1-validation.json`。
- 2026-10-19 21:10：B2 通过：A16 1,000/1,000、0x0B 1,000/1,000（`manifests/batches/b2-validation.json`）；APG B0 结构通过：13 个动画、181/181 图像块（`manifests/batches/s4-apg-b0.json`、`docs/formats/apg.md`）。APG 的 metadata/delta/blend 语义仍待解析。
- 2026-10-19 21:40：APG B1 通过：53 个动画、470/470 帧；发现 `AI` 块 = RLE8 索引 + CTI，13/13 AI 样本成功解析调色板（`manifests/batches/s4-apg-b1.json`）；特效帧的混合模式、AG metadata、CTI table2 选择仍待解。
- 2026-10-19 22:00：APG B2 通过：134 个动画、1,000/1,000 帧；AP=743、AI=257；34/34 AI 样本 CTI 解析成功（`manifests/batches/s4-apg-b2.json`）。
- 2026-10-19 22:15：APG AG 帧索引结构确认：`u16 frame_id + u16 flags + u32 offset`；offset 指向共享 alpha/命令流位置（可被多帧复用）；已定位 AG 帧迭代 `0x82cd70`、APG 加载器 `0x7f8210`。混合模式与命令流语义仍待解。
- 2026-10-19 22:35：验证 AG offset **不是**每帧 alpha 起点（用它解码会出现拼接/黑块）；每个图像块自己的 s2 起点才是正确 alpha。index flags 全为 0；混合模式很可能来自外部技能/动作配置，而非 APG 文件。静态导帧逻辑保持不变，B1 回归 470/470。
- 2026-10-19 23:00：PIF 解析完成（type 0/0x0B）：standalone 1-3.dat width=44/26/20 与 APG 帧数一致；perinfo.dat width=同目录 APG 动作数（8/2/1 验证）；报告 `manifests/batches/pif-b0.json`，文档 `docs/formats/pif.md`。
- 2026-10-19 23:55：**破解 NZ 0x0A（16 位 LZW）**：实现 `tools/dhsh/nz_lzw10.py` 并接入 `nz.py`；B1 100/100、B2 1,000/1,000、B3 定向 3,213/3,213 通过（含全部 1,945 个 PIF、625 个 SCP、420 个 CSV、174 个 MLK、13 个 PSD）。修正文本校验：GB18030 覆盖 GBK 扩展区、UTF-8 SCP、CP936 独立 0x80、`lnk` 二进制识别。报告 `manifests/batches/nz0a-b1.json`、`nz0a-b2.json`、`nz0a-b3-scripts-pif-v2.json`。
- 2026-10-20 00:15：**config.pak 0x0A 全量导出**：B0 12/12、B1 100/100、全量 1,253/1,253，0 失败，14.5 MiB；输出 `extracted/decoded/config/`，清单 `manifests/batches/nz0a-config-b0.csv`、`nz0a-config-b1.csv`、`nz0a-config-full.csv`。CSV/SCP/Lua/TXT 文本、MLK/INI 的 `lnk` 二进制、APG/A16/PIF 均校验通过。
- 2026-10-20 00:20：**0x0A 全量校验通过**：59,222/59,222 个叶子解码长度等于 `size1`，0 失败；18 种扩展名全部通过（APG 47,049、TGA 5,317、A16 3,408、PIF 1,945、SCP 625、CSV 420 等），解码输出 3.34 GiB；8 进程约 709 秒。报告 `manifests/batches/nz0a-full.json`，工具 `tools/validate_0a.py`。
- 2026-10-20 00:30：**map.pak / map1.pak 0x0A 全量导出**：B0 25/25、B1 100/100、全量 365/365，0 失败，69.7 MiB；输出 `extracted/decoded/map/`、`map1/`，清单 `nz0a-map-b0.csv`、`nz0a-map-b1.csv`、`nz0a-map-full.csv`。magic 规律：`.cel→CEL\0`、`.man→MAN\0`、`.core/.msk→MSK\0`；TGA 141 个 type 2、28 个 type 10（RLE）。
- 2026-10-20 00:40：**0x0A 解码性能优化**：复用模块级 LZW 字典/长度表、按 `size1` 预分配输出并直接倒写，新增可选 numba JIT（`cache=True`，无 numba 时回退纯 Python）。config.pak 纯解码 10.75s → 0.19s（77.6 MiB/s）；全量校验 709.2s → 25.8s；config 端到端导出 21.7s → 7.36s。回归：config+map 1,618 个文件逐字节一致，全量 59,222/59,222 仍 0 失败。
- 2026-10-20 00:50：**ui.pak 0x0A 全量导出**：B0 14/14、B1 100/100、全量 9,375/9,375，0 失败，438.9 MiB，31.9s；输出 `extracted/decoded/ui/`，清单 `nz0a-ui-b0.csv`、`nz0a-ui-b1.csv`、`nz0a-ui-full.csv`。扩展名：TGA 4,596、A16 3,346、APG 1,105、PIF 201、PG2 106、PSD 13、CSV 4、BMP 2、TXT 1、SCP 1。发现非 ASCII 文件名按 Latin-1 呈现的乱码问题（内容正常）。
- 2026-10-20 01:00：**npc.* / magic.* 0x0A 全量导出**：npc 9 个 PAK 4,232/4,232（APG 3,684、TGA 424、PIF 124，589.0 MiB，23.1s）；magic 4 个 PAK 1,062/1,062（APG 1,061、PIF 1，565.1 MiB，15.1s）。B0 各 6/6、B1 各 100/100，全量校验 5,294/5,294 全部通过；输出 `extracted/decoded/npc*/`、`magic*/`，清单 `nz0a-npc-*`、`nz0a-magic-*`。
- 2026-10-20 01:15：**player.* 0x0A 全量导出 + 0x0A 完成**：player.pak + player1-15 42,928/42,928（APG 41,182、PIF 1,598、TGA 128、CTI 20，1,740.5 MiB，162.1s）；补齐 800config/plugins 7 个。B0 6/6、B1 100/100，全量校验 42,928/42,928 全部通过；至此 0x0A 59,222/59,222 全部导出。清单 `nz0a-player-*`、`nz0a-misc-full.csv`。
- 2026-10-20 01:30：**0x0A APG 全量解析扫描**：`tools/scan_apg.py` 扫描 47,049 个 0x0A APG、449,011 帧，0 解析错误；压缩只出现 0/1/2、alpha 只出现 0/2；AP 79,197、AI 369,814；**AG flags=1 ⇔ 含 AI**（38,714/38,714）。`tools/check_apg_cti.py` 验证 369,814 个 AI 帧 CTI **100%** 覆盖（0 缺失）；player/npc/magic/ui 分层 100 文件/886 帧解码 0 失败。报告 `manifests/batches/apg-0a-scan.json`、`apg-cti.json`，文档 `docs/formats/apg.md` 第 7 节。
- 2026-10-20 02:00：**0x0A APG 批量转帧完成**：`tools/export_apg.py` + `tools/dhsh/formats/apg_fast.py`（NumPy RLE/LUT、共享 CTI 索引、8 进程、PNG level 3）导出 47,049 个 APG 的 **449,011/449,011 张 PNG**，3,175.6 MiB，0 错误，838.6s（约 535 帧/s）。快速路径与参考解码器 886/886 帧逐像素一致（4.8×）；B0 20/186、B1 200/1,910、B1b 1,000/9,615 均 0 失败；全量尺寸抽查 304 帧全部匹配。输出 `extracted/apg_frames/`，清单 `apg-frames-full.csv`。
- 2026-10-20 02:40：**0x0B 全量导出完成**：`nz_lzo.py` 增加可选 numba JIT（171 MiB/s，约 150×；numba 缺失时回退纯 Python），分组并行导出 **214,623/214,623** 个 0x0B 叶子，0 长度不匹配、0 失败，解码 **4,428.5 MiB**；其中 player.* 186,487 个/1,984.4 MiB（528.8s），npc 17,986、map 4,655、ui 2,517、magic 1,325、other 1,495、config 158。输出 `extracted/decoded_0b/`，清单 `nz0b-*-full.csv`；B0 35/35、B1 700/700、采样 1,000/1,000 校验通过。
- 2026-10-20 12:46：**0x0B APG 批量转帧完成**：`scan_apg`/`check_apg_cti`/`export_apg` 支持自定义 APG/CTI 根；0x0B 语料 204,137 APG/1,824,600 帧（0 解析错误），CTI 覆盖 100%（40 个 `Lcuff/521` 使用 `Rcuff/521` sibling-palette 回退）。B0 175 帧、B1 1,678、B1b 8,770 均 0 失败；全量导出 **1,824,600/1,824,600 张 PNG**，4,748.0 MiB，0 错误，1,977.8s（约 923 帧/s）。输出 `extracted/apg_frames_0b/`，清单 `apg-0b-frames-full.csv`。
- 2026-10-20 15:21：**inventory 校验 + type 0 APG 完成**：`tools/validate_inventory.py` 校验 523,267 条 inventory 记录，523,212 个可读叶子全部通过（55 个 size=0 容器记录排除）。新增 `tools/export_nz.py`（inventory 驱动、8 进程）导出 type 0 APG **207,811/207,811**（2,208.4 MiB，0 失败，160.8s）；扫描 1,647,239 帧、0 错误，CTI 100%（12 sibling + 16 neighbor `mask/53359`）；转帧 **1,647,239/1,647,239 PNG**（3,064.1 MiB，0 错误，1,817.9s）。输出 `extracted/decoded_t0/`、`extracted/apg_frames_t0/`，清单 `nzt0-*`、`apg-t0-frames-*`。三种 APG 语料累计 3,920,850 张 PNG。
- 2026-10-20 17:40：**A16 全量转 PNG 完成**：新增 `tools/scan_a16.py`、`tools/dhsh/formats/a16_fast.py`、`tools/export_a16.py`。三语料 **29,716 个 A16**（t0 20,990 / 0x0A 3,408 / 0x0B 5,318）、875,333,120 像素，0 头部/尺寸错误；模式只有 comp 0/1/2/5（5=JPEG，40 个，已实现）与 alpha 0/2。fast 与 reference **89/89 逐字节一致**；B0 40/40、B1 400/400、B1b 3,000/3,000、全量 **29,716/29,716 PNG**（405.8 MiB，0 错误，130.6s）。输出 `extracted/a16_png/`，清单 `a16-*`。
- 2026-10-20 18:05：**TGA/BMP/PSD 全量转 PNG**：新增 `tools/export_std_images.py`，使用 Pillow 打开三语料 **6,646** 个标准图片（TGA 6,549、BMP 84、PSD 13），alpha 自动保留；B0 50/50、B1 500/500、B1b 2,000/2,000、全量 **6,646/6,646 PNG**，0 错误，31.1 MiB，12.3s。输出 `extracted/std_png/`，清单 `std-*`。抽查 120 个 PNG 尺寸全部匹配。
- 2026-10-20 18:35：**特殊 JPG（FF D8 FF C8）格式定位**：510 个文件不是标准 JPEG。定位加载分支 `0x8238d0`（FF D8 + FF C8 → 自定义解码器 `0x8a3dd0`），解析出固定头（version 2/3、宽高、16 位量化表）、canonical Huffman 表（DC 10/9、AC 49/36）、每 MCU 6 字节描述符、u32 数据长度与熵数据；**510/510 容器结构校验通过**。熵编码为私有码流（无 SOS/EOI、无 FF00 转义），标准 JPEG 重建失败；文档 `docs/formats/special-jpg.md`，解码器移植作为独立任务暂缓。
- 2026-10-20 19:00：**PG2 容器解析与全量导出**：新增 `tools/dhsh/formats/pg2.py`、`tools/export_pg2.py`。150 个文件（t0 38/0x0A 106/0x0B 6）、1,606 帧，头部/帧表 0 错误；每帧为完整 `AP` 块（comp 1=6、2=1,165、5=435，alpha 全 2），复用 A16 解码。B0 102/102、B1 381/381、全量 **1,606/1,606 PNG**（81,366,848 px，66.1 MiB，0 错误，16.1s）；fast/reference 46 个分层样本逐字节一致。输出 `extracted/pg2_png/`，清单 `pg2-*`，文档 `docs/formats/pg2.md`。
- 2026-10-20 19:30：**PIF 帧延时语义 + GIF 导出**：确认 PIF 行 28 字节与 PG2 帧表项同构，`VA 0x824a20` 读取 `row+0x0a` 作为帧延时（ms，<1 时默认 80）；animation/1-3 GIF 总时长 8,800/2,080/1,600 ms 与 PIF 完全一致。新增 `tools/export_gif.py`（APG x/y 合成画布、CTI 复用、GIF 透明+disposal）；B0 10 文件/80 帧、B1 50/413、animation 1-3 共 90 帧，全部 0 错误。文档 `docs/formats/pif.md` 第 3、7 节，清单 `gif-*`。
- 2026-10-20 20:10：**S5 地图侧格式第一段**：新增 `tools/dhsh/formats/map.py`、`tools/scan_maps.py`、`tools/export_map_cel.py`。解析 MAP INI（width/height/background）、CEL 碰撞位图（MSB-first，1=阻挡，flags bit0 额外图层）、MAN 100 字节路径表、MSK/core 记录容器（record_size 284/28）、MLK 108 字节跳转记录。扫描 305 `.map`、275 `.cel`、277 `.man`、26 `.msk`、274 `.core`、228 `.mlk`，**0 错误**；CEL 位序用边缘置 1 比例验证（98.6% vs 91.6%）。全量 **275/275 CEL 转 PNG**（5,573,952 格，0.32 MiB，0 错误）。文档 `docs/formats/map.md`，清单 `map-scan.json`、`map-cel-*`。
- 2026-10-20 21:00：**SCP UI 脚本解析**：新增 `tools/dhsh/formats/scp.py`、`tools/scan_scp.py`、`tools/export_scp_json.py`。支持 GB18030/UTF-8、WINDOW/FIXEDPART 嵌套、`#include`、`#ifdef WINWIDTH800/#else/#endif`、`#script` 块、块注释、跨行引号字符串与 free text。扫描 **1,940** 文件、268,195 行、27,698 WINDOW、3,954 FIXEDPART、519 include；全量导出 JSON **1,940/1,940**（31,652 块，14.90 MiB，0 解析错误，9 个条件编译/END 告警）。输出 `extracted/scp_json/`，清单 `scp-scan.json`、`scp-json-*`，文档 `docs/formats/scp.md`。
- 2026-10-20 21:20：**APCI（perinfo.pak）解析器定位**：REA/Ghidra 在本机对 7.1 MB `Game.exe` 多次 330s 启动超时（未获得可用反汇编会话），改用 Capstone/字节级静态分析。定位文件加载器 `0x82a6d0`（以 mode=0 加载 `perinfo.pak`）与解析器 `0x82a460`；确认 group(4B)/record(36B)/subentry(4B)/mid(4B header)/item(8B+blob) 层级与 `0x826e70` 分配器。直接移植可解析约 7.25/7.81 MB，剩余段落出现 `player1\Rcuff` 等内联字符串，说明仍有未识别层级；文档 `docs/formats/apci.md`，后续优先用 Unicorn 局部仿真。
- 2026-10-20 22:10：**SCP 续：BGPIC + include 图**：`BGPIC = { ... }` 解析为结构化 `bgpic` 字段（7,435 个活动块；BGTYPE/SECTION/TEXT/FONTTYPE/TITLE1-9/DRAWPARENT/OFFSET 等）。新增 `tools/scp_include_graph.py`：519 条 include（.scp 464、.lua 55），183 同语料、336 跨语料、**0 缺失**、6 重复、0 环、最大闭包 3；输出 `manifests/batches/scp-include-graph.json`（0.54 MiB，含反向边与闭包）。SCP JSON 重导 **1,940/1,940**（31,652 块，16.10 MiB，0 错误，7.8s）。
- 2026-10-20 22:20：**APCI 完整解析与全量导出（S9 完成）**：修正为 8 字节 entry（`u32 a`/`u32 b`，`b&0xffff`=mid 数），blob 大小按 `(y>>16)&0xff==1 ? x>>24 : ((x>>24)*width)&0xffff`。新增 `tools/dhsh/formats/apci.py`、`tools/export_apci.py`。**20 group / 219 record（203 名称）/ 22,341 entry / 87,676 mid / 687,229 item / 432,838 blob**，解析恰好消耗 7,808,665 字节；全量导出 `extracted/apci/perinfo/`（items.csv 49.3 MiB、blobs.bin 1.77 MiB、records.csv），0 错误，14.1s；B0 `--limit-items 1000` 通过。`player1\Rcuff` 等字符串实为 record 名称，旧移植的 4 字节 entry 假设与缺少 16 位截断是错位根因。
- 2026-10-20 23:10：**特殊 JPG 解码结构层完成**：新增 `tools/dhsh/formats/special_jpg.py`、`tools/probe_special_jpg.py`。解析头部（组件 q/ac/dc 记录、3×量化表、8 个 Huffman 槽）、6 字节 MCU 描述符（f0 字节增量、f1 字内位偏移、f2/f3/f4 DC 槽）与 data_size；块解码按大端 32 位字/MSB-first、私有 zigzag（VA 0xab62c8）实现。**6400×6400 v2 文件 160,000 MCU / 960,000 块 0 错误**，v3 样本 0 错误；DC 链 f2 = 上一 MCU 最终 DC 逐个吻合。IDCT 暂用 float 近似（raw/64）、颜色用标准 BT.601，待精确复核；反汇编汇总 `manifests/batches/special-jpg-code-dump.txt`（8 函数，2,020 行）。
- 2026-10-21 00:10：**特殊 JPG 独立复核与修正**：外部复核提交 `70969a2`（`docs/formats/special-jpg-kimi-review.md`、`tools/kimi_*`）。用位精确 MMX 解释器确认 IDCT 缩放 = 教材公式的 1/16（raw/64）、末级 `psraw 4` floor、无偏置；颜色表 `trunc(K·v+1)>>1` 与运行时内存 256/256 吻合，活动路径为 MMX RGB565（0x8a32d0/0x8a38d0）；真机截图对拍抓出并修正三处块级 bug：v2 四个 Y 块全部使用 DC slot0（原 0/1/2/0 错误，Y-DC 粗糙度 289→28）、AC zigzag 反序（`ZIGZAG[64-k]`）、IDCT 输出块需转置。已同步修正 `special_jpg.py`（含 floor、int16 回绕/饱和、查表 RGB565）。v2 复核图平均绝对差 1.10、61.2% 逐像素一致。
- 2026-10-21 01:00：**特殊 JPG 解码性能优化**：新增 `tools/dhsh/formats/special_jpg_numba.py`（numba JIT 熵解码/MCU 驱动，IDCT/颜色仍 numpy 向量化）。1024×768 v2 **3.03 s → 0.36 s（8.4×）**，6400×6400 v2 **157.3 s → 20.3 s（7.75×）**；快路径与纯 Python 输出逐字节一致。顺带修复纯 Python `BitReader` 在文件末尾不足 8 字节时未补零的问题（仅影响最后一个 MCU 末块）。510 个文件按 8 进程预计约 2–3 分钟完成。
- 2026-10-21 01:40：**特殊 JPG 全量导出完成**：新增 `tools/export_special_jpg.py`（多进程、`--sample/--all`、已存在跳过、PNG compress_level 可调）。B0 10/10（79.3 MP，84.7 MiB）、B1 50/50（127.9 MP，138.8 MiB）、全量 **510/510**（**1,551,886,272 px，1,628.3 MiB，0 错误，198.8 s / 8 进程**）。输出 `extracted/special_jpg_png/`，清单 `special-jpg-b0.csv`、`special-jpg-b1.csv`、`special-jpg-full.csv`；全部 PNG 尺寸与头部一致（v2 427、v3 83）。
- 2026-10-21 02:10：**MSK/core 数据区破解并全量导出**：地图加载器 `0x27aa0` 把 `MASKFILE` 扩展名改成 `.core` 后调用解析器 `0x274b0`，故 `.core` 是活动格式、`.MSK` 是旧格式。确认头部/记录/24 字节放置表/数据区：`u16=(covered<<15)|run` 行内 RLE；放置 `x/y` 有符号，`f4` 为绝对偏移；`obj+0x10 = y + f2 - (x*f6>>8)`。运行时 YingTianFu 容器与 **563/563** 个 44 字节对象逐字段验证（含 146 个 f6≠0）。**300/300 文件、12,755 遮罩、717,967,584 px、11,421,604 runs 全量解码 0 错误**；新增 `tools/export_core_masks.py`，B0 10 文件/564 记录（0.50 MiB）、B1 50/2,165（3.04 MiB）、全量 **12,755/12,755 PNG（15.85 MiB，0 错误，21.3 s）**。输出 `extracted/core_masks/`，清单 `core-mask-b0.csv`、`core-mask-b1.csv`、`core-mask-full.csv`；文档 `docs/formats/map.md` 第 4 节。遮罩与地图背景叠加逐像素吻合（树/建筑/人物/浮空岛）；`flag=0` 的 BangPai_1 组为底边对齐的生长动画帧，f2/f6 为地线截距/Q8.8 斜率（强推断，消费函数待确认）。
- 2026-10-21 04:00：**MSK/core 消费端语义闭合（S 段完成）**：建立持久 Ghidra 项目 + `tools/kimi_ghidra_batch.py`（PyGhidra 批量查询，约 35 s/轮）。定位主消费函数 `FUN_004268b0`（ApplyMask）：flag 检查 → 地线深度测试 `y < ((f6*x)>>8)+sortY` → 矩形相交 → RLE 逐行 → MMX 暗化（`psraw 3` & 0x1f1f…，DAT_00aa9e68）——**遮挡 = 角色被盖部分变暗（ghost），非重画背景，不参与点击判定**。渲染调用点 `FUN_0081ac30`（blit 前 map vtable+0x18 = 0x41e940 → 0x4268b0，绘制后 vtable+0x1c 恢复）；容器 `map+0x40 = *(app+0x88)`，`vector<Elem*>`；flag/group/frame = 帮派建造阶段遮罩帧（`FUN_00426b60` ← 主命令分发器 `FUN_007a2120` case 0x3077 ← `FUN_00410000` 读帮派 INI）；动态遮罩 `FUN_00427740`（角色/动画 alpha≥0x10 实时注册）；f3/f5 运行时零消费。提交 `2ec48e4`。
- 2026-10-21 05:30：**消费端遗留项全闭合 + 运行时只读验证**：暂存栈协议闭合（容器+0x08 内嵌 {cap 100/50, count@+0x10, begin@+0x14}，push `0x4265f0`/恢复 vtable+0x1c→`0x426080`/清空 vtable+0x20→`0x425f30`/未命中 `0x426b4a: DEC [+0x10]`）；容器+0x40 禁用开关字节级扫描（`89 /r`、`C7 /0` + 掩码通配）3 命中均为无关类 → **遮挡常开**；`FUN_007a2120` 确认为主命令分发器。新增 `tools/kimi_peek_masks.py`（ReadProcessMemory 零注入）对运行中游戏实测：容器/563 元素/disable=0/count 空闲=0/f6∈±128/sortY 升序全部吻合。提交 `46dc97a`。
- 2026-10-21 08:00：**语义深化第一段（APG 渲染族）**：**AG offset** = 帧数据 delta 长度（迭代器 `FUN_0082cd70` 的 `obj+0x10` 累加器；`FUN_007f80f0` 仅作 >0 校验）——旧"alpha 起点"误判澄清；**CTI table2** = 256×{H,S,V,pad} HSV 染色主表（色相轮 252=6×42），选择链 `FSAIndexBitmap::_UseColorTable`→`FUN_008254f0`（快路径 `FUN_00825480`/`FUN_00824cc0` 纯色相旋转；变体 `FUN_008252c0` hue+sat+val，`c3==0x81` 灰度），新增 `tools/kimi_verify_cti.py` 实证：可染色装备（player6 harmor*）table2 全 S=0 灰阶且 `hue0(table2)==table1` 255/255 精确成立——**可染色装备底稿为灰阶图**；**混合模式**：渲染管线 `FUN_00805b10`→`FUN_008035c0`/`0x81ad70`→`FUN_0081ac30`→`WCanvas::Draw`（vtable+0x40=`0x7f9f70`）→tile 合成→GDI Blit2DC，mode 0/1/4/6 为缩放/脏标记非混合，合成 = WAlphaBitmap 逐像素 alpha——**未观察到层间加法混合**；当时“全二进制无 PADDUSB”的表述已在 2026-10-23 修正（存在颜色偏置用 PADDUSB）。提交 `cbd0ff9`。
- 2026-10-21 10:00：**APCI 字段语义闭合**：解析器 `FUN_0082a460`（APCI v0x101，8B entry）内存布局 record(0x2c)/entry(16B)/mid(12B)/item(16B)；`entry.a` = 动作/动画 ID（当时判为与 `FUN_007ed6d0` 区间吻合；2026-10-23 修正：41.9% entry 越界，需按 record 类型区分）；`mid.header` = **方向数<<16 | 每方向帧数**（item 加权 99.44%）；blob 几何公式 `y_hi8==1 → x_hi8` 否则 `x_hi8×x_lo16` **687,229/687,229 全量验证**；blob 1B=帧索引、12B=6×u16 锚点组；消费链 `FUN_007ed8a0`→`DAT_00ae4b08`→21 消费者，核心查询引擎 `FUN_008283d0`（动作→帧序列）。**AI 锚点**：x/y=绘制偏移，装载取负存位图 +4/+8，绘制 `pos+anchor`（`FUN_007f8210`/`FUN_0081a000`）。提交 `67b57cd`、`38c67b4`。
- 2026-10-21 12:00：**SCP 消息绑定与 Lua 桥闭合**：`#script` = 标准 Lua（二进制含 Lua 5.x VM 内部字符串与 luac/.out 字节码支持）；38 个 `FSM_*` 消息名表（VA `0xa4c9ac` 邻域；2026-10-23 修正：当前 SHA 去重 51 个）；SCP 解析器族 `FUN_0090bf00`（117 KB）按名映射消息 ID，窗口类型经 `FSWndTypeNameMap`（`FUN_009133f0`）；`FSLuaWndScript` 注册器 `FUN_008fc830`（`RegMsgCall` C 实现 `0x8fc520` → Lua 函数引用挂入窗口回调链 `obj+0x13c`；`RegAfterCreate`/`RegBeforeRelease`/`RegWinRun`）；`FSLuaSystem`（`FUN_008bb410` 等）、`FSLuaInstall`（`FUN_008c55e0`，加载 config/lua 模块）；**457 个 CSV 零硬编码引用 = 纯数据驱动**。提交 `00a5610`。
- 2026-10-23：**重新基线 + overclaim 纠错（评估见 `docs/VERIFY-2026-10-23.md`）**：客户端 1.50.20→1.50.22，Game.exe SHA `f6bf…`→`7a5e…`；重建 `pak_inventory.json`（523,213 条，零长 189，非空 523,024）；4 个 PAK 更新（config 1,330 条路径大小写 + 2 内容、magic5 10 APG、ui 1 CSV + 3 零长、800config 1 SCP）。新增 `tools/reexport_changed.py`，重导 14 个变化叶子 0 错误；刷新 88 个 magic5 APG 帧（736/736）、SCP JSON 1,940/1,940、include 图 519 边。当前二进制复核 MSK/core：解析器 `0x4272d0`、ApplyMask `0x4266d0`、深度测试 `0x426756`、暗化 `0x4264b0`、group/frame `0x426980`。修正：APCI `entry.a` 41.9% entry 越界；PADDUSB 实存于颜色偏置路径（旧“全二进制无”错误）；CTI 脚本默认筛灰度；FSM 51 个；`kimi_peek_masks.py` 字段偏移修正（待下次运行复验）。
- 2026-10-23 晚：**网络协议双向全破解（c2s 闭合）**：c2s = 首帧 Blowfish（静态主密钥，自定义 F 函数与密钥编排，磁盘表 0x643e70/0x643eb8/0x644eb8）+ 后续全会话标准 RC4（密钥 = 93B 凭据块 + 其 MD5 大写 hex 32B = 125B，凭据块 = 账号/密码/会话值/MAC，登录帧明文自带）+ 4B MD5 尾校验。登录帧 0x4004 经 `FUN_007a1590`（游戏服）/`FUN_007a1370`（网关）构建，重定向连接 `FUN_00410810`。登录抓包 62/62 帧全解：0x40d0×4(7/2/1/3) 与二进制调用序列逐一对应、0x4210 内嵌会话值、聊天帧还原测试消息。新增 `tools/kimi_mem_read.py`（RPM 任意地址只读转储）；`kimi_msg_crypto.py` 增 Blowfish/RC4/`decode_c2s_stream`；`kimi_msg_decode.py` 增 `--c2s`（凭据脱敏）。未确定：序号种子哈希移植（不影响解密）、登录期 s2c 加密方案（疑凭据 Blowfish，待哈希修正后验证）。文档 `docs/formats/net-protocol.md` §4。

## 复现命令

```bash
cd tools
# 全量类型统计
python -m dhsh stats --out ../manifests/nz-stats.json --summary ../manifests/nz-summary.csv
# 非 APG 的 type=0 文本/数据
python -m dhsh extract --out ../extracted/easy --types 0 \
  --exts .ini .lua .scp .map .mlk .man .core .msk .dat .zip .csv .txt .db \
  --strip-nz --hash --overwrite --manifest ../manifests/easy-extract.csv
# 标准图片
python -m dhsh extract --out ../extracted/images-standard --types 0 \
  --exts .jpg .tga .bmp --strip-nz --overwrite --manifest ../manifests/images-standard.csv
# A16/CTI
python -m dhsh extract --out ../extracted/images-proprietary --types 0 \
  --exts .cti .a16 --strip-nz --overwrite --manifest ../manifests/images-proprietary.csv
# APG 样本与压缩样本
python -m dhsh extract --out ../extracted/samples/apg --types 0 --exts .apg \
  --per-pak-limit 20 --max-size 500000 --strip-nz --overwrite --manifest ../manifests/samples-apg.csv
python -m dhsh extract --out ../extracted/samples/compressed --types 0x0a 0x0b \
  --exts .apg .a16 .tga .scp .lua .csv .cti --per-pak-limit 10 --max-size 300000 \
  --overwrite --manifest ../manifests/samples-compressed.csv
# 0x0A LZW 验证（B1/B2/B3/全量）
python ../tools/probe_0a.py --count 100 --seed 1 --batch b1
python ../tools/probe_0a.py --count 1000 --seed 2 --batch b2
python ../tools/probe_0a.py --count 100000 \
  --exts .csv,.scp,.mlk,.dat,.lua,.txt,.ini,.psd,.bmp --batch b3-scripts-pif-v2
python ../tools/probe_0a.py --count 100000 --types 0x0a --batch full
# A16 扫描与全量转 PNG
python ../tools/scan_a16.py --jobs 8 --out ../manifests/batches/a16-scan.json
python ../tools/export_a16.py --all --jobs 8 --out ../extracted/a16_png \
  --manifest ../manifests/batches/a16-full.csv
# TGA/BMP/PSD 全量转 PNG
python ../tools/export_std_images.py --all --jobs 8 --out ../extracted/std_png \
  --manifest ../manifests/batches/std-full.csv
# PG2 全量转 PNG
python ../tools/export_pg2.py --all --jobs 8 --out ../extracted/pg2_png \
  --manifest ../manifests/batches/pg2-full.csv
# GIF 样本（B0/B1/开场动画）
python ../tools/export_gif.py --sample 10 --seed 1 \
  --out ../extracted/_batches/gif-b0 --manifest ../manifests/batches/gif-b0.csv
python ../tools/export_gif.py --sample 50 --seed 2 \
  --out ../extracted/_batches/gif-b1 --manifest ../manifests/batches/gif-b1.csv
python ../tools/export_gif.py --root "anim=D:/software/dhsh/shdata/animation" --all \
  --out ../extracted/_batches/gif-anim --manifest ../manifests/batches/gif-anim.csv
# 地图侧文件扫描与 CEL 导出
python ../tools/scan_maps.py --jobs 8 --out ../manifests/batches/map-scan.json
python ../tools/export_map_cel.py --all --jobs 8 --out ../extracted/map_cel_png \
  --manifest ../manifests/batches/map-cel-full.csv
# 地图预览（背景 + MSK/core 遮罩）
python ../tools/map_preview.py --sample 10 --seed 1 --tag b1 --allow-missing
python ../tools/map_preview.py --all --allow-missing
# SCP 扫描与 JSON 导出
python ../tools/scan_scp.py --jobs 8 --out ../manifests/batches/scp-scan.json
python ../tools/export_scp_json.py --all --jobs 8 --out ../extracted/scp_json \
  --manifest ../manifests/batches/scp-json-full.csv
python ../tools/scp_include_graph.py --out ../manifests/batches/scp-include-graph.json
# APCI（perinfo.pak）解析与导出
python ../tools/export_apci.py --limit-items 1000 --out ../extracted/_batches/apci-b0 \
  --manifest ../manifests/batches/apci-b0.json
python ../tools/export_apci.py --all --out ../extracted/apci/perinfo \
  --manifest ../manifests/batches/apci-perinfo.json
```
