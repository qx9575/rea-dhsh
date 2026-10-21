MSK/core 遮罩格式：剩余“消费端语义”复核任务

目标：D:\software\dhsh\shdata\Game.exe
SHA-256 f6bf66d24a3523bb6a30541589fabea36bbceec84355463d8e2c99e3b7f16be9
只读静态分析，不要运行游戏或其保护组件；不要修改目标文件。

一、已确认的事实（可以直接复用，不必重复推导）

1. 加载链
   - 地图 INI 的 MASKFILE 写作 ...MSK；
   - 加载器 0x27aa0 用 0xb580 把路径扩展名“替换”成 .core（不是比较），
     然后调用解析器 0x274b0；
   - 因此当前客户端实际加载的是同目录 .core；.MSK 是旧格式。

2. 容器头（0x274b0）
   0x00 char[4] "MSK\0"; 0x04 u32 version=5; 0x08 u32 total_size;
   0x0c u32 flags=0; 0x10 u32 header_size=40; 0x14 u32 record_size;
   0x18 u32 record_count; 0x1c u32 data_offset; 0x20 u32 placement_size=24;
   0x24 u32 placement_count.
   data_offset = 40 + record_size * record_count.

3. 记录（core: record_size=28；MSK: 284 = 256 字节 BMP 名称 + 28 字节字段）
   7 个 u32：
   f0 width, f1 height, s32 f2 base_y, u32 f3 kind,
   u32 f4 blob 绝对文件偏移, u32 f5 blob 字节长度, s32 f6 slope(Q8.8).
   所有 blob 在放置表之后连续排列，末条到文件末尾。

4. 放置表（data_offset 起，24 字节/条）
   u32 record_index, s32 x, s32 y, u32 flag, u32 group, u32 frame.
   x/y = 遮罩左上角地图像素坐标。
   - flag=1：静态遮罩（16,009/16,114 条）；
   - flag=0：仅 BangPai_1 出现（105 条），group/frame 非 0。

5. 数据区 RLE（已完全破解并全量验证）
   u16 word = (covered << 15) | run_length
   - 逐行、不跨扫描行；每行 run 长度和 = width；总行数 = height；
   - covered=1 是前景物件轮廓（树、建筑、人物、浮空岛），与地图背景
     逐像素吻合。
   - 300/300 文件、12,755 记录、717,967,584 像素、11,421,604 runs，
     0 错误；已全量导出 PNG。

6. 运行时对象（YingTianFu，563/563 逐字段验证）
   容器（运行时）：
     +0x00 placement_count；+0x04 文件数据指针；
     +0x24 std::vector 对象；+0x28 begin；+0x2c end；+0x30 cap；
     +0x44 路径字符串（inline）。
   每个元素 44 字节：
     +0x00 x, +0x04 y, +0x08 width, +0x0c height,
     +0x10 y + f2 - (x*f6>>8), +0x14 f6,
     +0x18 flag, +0x1c group, +0x20 frame,
     +0x24 RLE 数据副本指针,
     +0x28 运行时递增 id（call [0x97d134] 的返回值，非文件字段）。

7. 数据侧强推断（待你确认）
   - f2/f6：obj+0x10 是 y 经过地线截距/斜率修正后的排序基准；
     f6 非零值 64.7% 聚集在 ±(128±48)，即 Q8.8 的 0.5 斜率，
     符合 2:1 等距视角；推断用于遮挡/深度排序（Y-sort）。
   - flag=0 的 BangPai_1 组：同 group 的帧 x 相同、底边对齐、高度递增，
     形态是从地面长出的动画（树/喷泉类）；group=组 id、frame=帧序号。
   - f3 以 1 为主（11,759/12,755），另有 2..10，语义未知。

二、需要你做的事（静态反汇编，给出证据；不要猜）

1. 找到消费容器 vector（+0x28/+0x2c）的绘制/更新函数，确认：
   - obj+0x10、obj+0x14（f2/f6）如何参与 Y-sort/遮挡/混合；
   - flag、group、frame 如何被使用；
   - 若存在动画播放逻辑，给出帧率、循环/触发条件、与脚本的关系。
   提示：容器是地图对象的成员（地图加载器 0x213a3 调用 0x27aa0）。
   对象没有 vtable，可能是纯数据 + 容器方法；元素大小 44 字节。

2. 确认 .core 遮罩在渲染中的具体作用：
   - 是把背景物件重新盖在角色上方（遮挡）？
   - 还是点击/拾取判定（IsCover/GetCoverPt）？
   - 或两者都有？给出调用链。

3. 复核 f3（kind）字段的取值分布与用途。

4. 如果发现我们上面的任何字段解释有误，请直接指出并给出反汇编证据。

三、参考文件（本仓库）

- tools/dhsh/formats/map.py            （当前解析/解码实现）
- tools/export_core_masks.py           （全量导出）
- docs/formats/map.md 第 4 节          （格式与验证记录）
- manifests/batches/core-mask-full.csv （12,755 条导出清单）
- extracted/core_masks/                （遮罩 PNG，可直接查看）
- extracted/_probe_core/               （叠加/动画面板调试图）

四、输出要求

- 消费函数地址、伪代码或关键反汇编片段；
- 每个字段/行为的证据链；
- 明确区分“已验证”“推断”“未知”；
- 如需脚本，请以 kimi 前缀独立命名，不要修改现有工具。
