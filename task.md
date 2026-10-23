● B1 通过：100/100（含 77 个 APG、4 个 PIF）。继续 B2（1,000     
  随机）和稀有扩展名定向验证。

● 全部 1,945 个 0x0A PIF 与 13 个 PSD
  解码成功。剩余“失败”只是文本启发式检查：.mlk 本身是 lnk        
  二进制，不是文本；少数 CSV/SCP 需确认真实编码。先分析这几类。  


  ✅ 反汇编并理解 0x8a6f60（NZ type 0x0A 解码器）
  ✅ 用 0x0A 样本验证解码假设（B1 100/100）
  ✅ 实现 Python 0x0A 解码器并批量验证（B2 1,000；B3 3,213）
  ✅ 更新文档并提交（57ab60c）
  ✅ 全量 59,222 个 0x0A 长度校验（59,222/59,222，0 失败，3.34 GiB 输出）
  ✅ config.pak 0x0A 导出（B0 12/12、B1 100/100、全量 1,253/1,253，14.5 MiB）
  ✅ map.pak/map1.pak 0x0A 导出（B0 25/25、B1 100/100、全量 365/365，69.7 MiB）
  ✅ 0x0A 解码性能优化（字典复用 + 可选 numba；全量校验 709s→25.8s）
  ✅ ui.pak 0x0A 导出（B0 14/14、B1 100/100、全量 9,375/9,375，438.9 MiB）
  ✅ npc.* 0x0A 导出（B0 6/6、B1 100/100、全量 4,232/4,232，589.0 MiB）
  ✅ magic.* 0x0A 导出（B0 6/6、B1 100/100、全量 1,062/1,062，565.1 MiB）
  ✅ player.* 0x0A 导出（B0 6/6、B1 100/100、全量 42,928/42,928，1,740.5 MiB）
  ✅ 0x0A 全量导出完成（59,222/59,222，解码输出 3.34 GiB）
  ✅ 0x0A APG 全量扫描（47,049 文件/449,011 帧，模式/CTI 覆盖 100%）
  ✅ 0x0A APG 批量转帧（449,011/449,011 PNG，3.18 GiB，0 错误）
  ✅ 0x0B 全量导出（214,623/214,623，4,428.5 MiB，0 失败）
  ✅ 0x0B APG 批量转帧（1,824,600/1,824,600 PNG，4,748.0 MiB，0 错误）
  ✅ inventory 全量校验（523,212 可读叶子，0 错误）
  ✅ type 0 APG 导出/转帧（207,811 APG；1,647,239 PNG，3,064.1 MiB，0 错误）
  ✅ A16 扫描（29,716 文件，comp 0/1/2/5 + alpha 0/2，0 错误）
  ✅ A16 全量转 PNG（29,716/29,716，405.8 MiB，0 错误，130.6s）
  ✅ TGA/BMP/PSD 全量转 PNG（6,646/6,646，31.1 MiB，0 错误，12.3s）
  ✅ 特殊 JPG（510）容器结构解析（熵解码待移植，docs/formats/special-jpg.md）
  ✅ PG2 容器解析与全量转 PNG（150 文件 / 1,606 帧，66.1 MiB，0 错误）
  ✅ PIF f5 = 帧延时（ms，默认 80）确认
  ✅ GIF 导出（B0 10/80 帧、B1 50/413 帧、animation 1-3 共 90 帧，0 错误）
  ✅ 地图侧格式解析（MAP/CEL/MAN/MSK/core/MLK，1,385 文件，0 错误）
  ✅ CEL 全量转 PNG（275/275，5,573,952 格，0.32 MiB，0 错误）
  ✅ SCP UI 脚本解析（1,940/1,940，31,652 块，0 错误）
  ✅ SCP BGPIC 结构化（7,435 个活动块）
  ✅ SCP include 图（519 边，0 缺失，0 环）
  ✅ APCI perinfo.pak 完整解析（20 group / 219 record / 687,229 item，0 错误）
  ✅ APCI 全量导出（items.csv 49.3 MiB + blobs.bin 1.77 MiB）
  ✅ 特殊 JPG 解码（结构/颜色/IDCT 复核）
  ✅ 特殊 JPG 性能优化（numba 快路径，6400×6400 157s→20s）
  ✅ 特殊 JPG 全量导出（510/510 PNG，1,628.3 MiB，0 错误）
  ✅ MSK/core 数据区 RLE 破解（300/300 文件，12,755 遮罩，717,967,584 px，0 错误）
  ✅ MSK/core 遮罩全量导出（12,755 PNG，15.85 MiB，0 错误）
  ✅ MSK/core 消费端语义（f2/f6 地线、flag/group/frame 动画）→ docs/formats/map.md §4.7（行为在当前 SHA 复核；运行时脚本已修正，待游戏启动后复跑）
  ✅ 资源查看器 kimi_viewer（PyQt5/PySide6 兼容，可插拔格式插件，52 万条目秒级过滤）→ tools/kimi_viewer/
  ✅ 重新基线：客户端 1.50.22 / Game.exe 7a5e… / inventory 523,213 条；14 个变化叶子重导，88 个 magic5 APG 帧 + SCP JSON/include 图刷新
  ✅ 修正 overclaim：APCI entry.a 部分越界、真实 PADDUSB（颜色偏置）、CTI 脚本筛灰度、FSM 51 个、文档口径
  ◻ 下次启动游戏：复跑 `python tools/run_mask_peek.py`（或 `kimi_peek_masks.py`，包含 SHA 快照/暂存栈/disable/count）
  ✅ 新 SHA 复核：AI 锚点（0x7f8330/0x81a140）、SCP/Lua（0x8fc970/0x8c5720/0x90c040）、APCI 引擎（0x828510）、CTI（0x825630/0x824e00/0x825400）、混合=纯色填充偏置（0x820050）
  ◻ 加密/未知数据块（ACDC.dat、ccore.dat、cltgp.dat、sl.dat、001.cpm）
  ◻ 非 PAK 资源索引（sound/font/animation/update skin）
  ◻ 查看器收尾（MSK/core 与 SCP 插件）
  ✅ 语义深化（AG offset=delta、CTI 灰度 hue0、SCP 消息/Lua；混合表述已修正）
  ◻ 语义深化剩余（APCI entry.a 类型区分、12B 锚点槽位）