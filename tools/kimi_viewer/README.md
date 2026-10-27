# kimi_viewer — 新水浒Q传 PAK 资源查看器

启动（仓库根目录）：

```
python tools/kimi_viewer.py                      # 默认读 D:\software\dhsh\shdata
python tools/kimi_viewer.py --shdata <PAK目录>   # 指定其他目录
```

- 左侧：PAK / 格式过滤 + 路径子串搜索（250ms 防抖），点击条目即解码预览；
- 右侧：预览页（图像/文本/hex）+ 信息面板（NZ 信息、格式元数据、解码耗时）；
- 工具栏：
  - 「导出当前资源」：单个选中资源（图像 → PNG，多帧导出一个目录；文本 → .txt；其他 → 原始字节）；
  - 「批量导出」：按范围批量导出——当前过滤结果 / 指定 PAK / 指定格式 /
    PAK+格式，后台线程执行，进度条可取消，输出目录按 `<pak>/<路径>` 布局；
  - 「插件管理」：查看全部插件及其负责的扩展名，可勾选启用/禁用
    （禁用后该格式降级 hex 预览），配置持久化到 `manifests/kimi_viewer_config.json`；
- APG/PG2 多帧：播放/暂停、FPS、拖动进度条；滚轮缩放，「适应」/「1:1」切换；
- 大图平移：**右键按住拖动**（图像页与场景页通用，`pan.py` 的
  `RightDragPan`），滚动条随拖动同步；滚轮缩放**以鼠标位置为中心**
  （`pan.py::zoom_at`，光标下的内容点保持不动）；

## 纸娃娃（工具栏「纸娃娃」）

按游戏内规则离线装配角色（`avatar.py` + `avatar_view.py`，全部 PAK 直读）：

- 装配规则与 `FUN_008281e0` 一致：13 个部位按 **APCI 1B 层叠键升序**绘制，
  锚点与帧延时取动作目录 `perinfo.dat`（PIF f2/f3/f5），帧图像复用 APG
  解码（含 AI 调色板）；首次装配建立 APCI 索引，之后部件动画有缓存。
- **六类别**：人物 / NPC / 坐骑 / 装备 / 宠物 / 召唤兽，均不使用家族/pak
  选择器。模型统一显示 `名称(ID)`；没有可靠名称时显示 `待确认(ID)`，不猜测。
  同一模型的别名与各自来源保留在映射层和覆盖 CSV 中；召唤兽只列带名模型。
- **人物职业联动**：职业（17 类）+ 进阶（职业/英雄/星将）+ 性别联动装备、
  坐骑、技能列表与画面；「应用职业」可填入默认预设。装备候选为
  `itemSpecData.csv` 职业权限与真实模型资源的交集，再额外纳入**当前 viewjob
  有效预设**（职业标志全 False 为通用），不改变装备类别的职业浏览过滤；
  **body/head 不过滤**。坐骑按职业/进阶/性别的有效关系筛选，骑甲按坐骑 ID
  及 +1/+2 匹配。合法旧选择与显式「无」保留，失效模型选首个有效项；预设
  资源不存在时置「无」并明确报告资源缺失，不静默保留旧值。每次职业切换/
  应用只在批量更新后进行一次有效装配。人物与坐骑按底心对齐。
- **装备类别**：物品库支持类型、职业、名称过滤及图标详情。可装配物品按
  严格槽位换算 `模型 ID = 物品 ID − 槽位×100`，且目标资源必须存在，然后播放
  对应部位 APG（共享动作/方向、perinfo 帧延时与自动播放 timer）。无模型时
  保留详情并按结果降级：图标可用才显示「显示静态图标」；否则状态与资源
  明细一致显示「图标缺失」或「图标解码失败」。装配异常清空旧帧，资源明细
  更新为当前项。
- **坐骑无技能**：坐骑类别不显示、不叠加技能光效，资源明细也不包含技能。
  人物技能下拉在对话框构造时即填充；人物/宠物技能按 `Skill.csv` ID 查找，
  支持职业过滤、空光效类型回退及 magic front/back 双层；全部件为「无」时
  可纯特效播放。
- **映射来源**：装备与骑甲来自 `ui.pak:item/itemSpecData.csv`；坐骑来自
  `horseinfo.csv` 与 `viewjob.csv` 的职业/进阶/性别/资源 ID 交叉关系，不使用
  旧的「怪物 ID−30800」通用公式。NPC/别名来自 `Monsters.csv`、
  `monstersound.csv`、`simmonstersound.csv`、`npchead.csv` 和 `horseinfo.csv`；
  `bodyidequal.csv + Monsters.csv` 仅提供有来源的人物部件别名。运行时调查
  未新增映射，证据不足的 ID 保持待确认。
- 多帧自动播放；方向缺失时回退到有帧方向，类别/选项切换清空旧帧并刷新。
  「资源明细」可复制 APG、perinfo.dat、技能前后层与 APCI/PIF 来源；隐藏坐骑
  时骑甲自动禁用且不装配。支持 CTI 染色、播放/暂停、FPS、缩放、右键平移和
  导出当前帧。画布背景可在棋盘格/黑/白间切换以检查 alpha。

### 模型名称覆盖清单

从**仓库根目录**运行：

```bash
python tools/kimi_viewer_model_coverage.py
```

生成器默认读取**当前工作目录**的 `kimi_viewer_data/pak_inventory.json`（缓存
也在该目录），直接读取真实 PAK，输出 `manifests/kimi_viewer_model_coverage.csv`。
当前 CSV 共 **20,530 条数据行**（含表头 20,531 行），状态为
**confirmed 2,128 / pending 18,402**。分类统计如下，与生成器 `stats()` 和 CSV
实数一致：

| 类别 | confirmed | pending | 合计 |
|---|---:|---:|---:|
| body | 17 | 1,085 | 1,102 |
| head | 125 | 6,695 | 6,820 |
| weapon | 247 | 363 | 610 |
| cloth | 260 | 192 | 452 |
| shoe | 246 | 178 | 424 |
| mask | 174 | 6,117 | 6,291 |
| Lcuff | 237 | 199 | 436 |
| Rcuff | 237 | 187 | 424 |
| horse | 56 | 666 | 722 |
| harmorbody | 46 | 690 | 736 |
| harmorhead | 32 | 655 | 687 |
| pet | 0 | 441 | 441 |
| effect | 0 | 98 | 98 |
| npc | 451 | 836 | 1,287 |
| **合计** | **2,128** | **18,402** | **20,530** |

CSV 的 `names`、`sources` 列用 ` | ` 保留别名和来源；`pending` 不代表资源
不可用，只表示尚无可靠名称映射。

## 场景模式（.map）

打开 `.map` 自动合成可交互场景（`map_scene` 插件 + `scene.py`/`scene_view.py`）：

- 图层（绘制序）：背景 → MAN 动画（**按 perinfo.dat 帧延时播放**，无 perinfo
  时默认 80ms）→ GAL 云雾（默认关）→ MSK/core 遮罩（默认关）→ CEL 碰撞
  （默认开，蓝色=阻挡）→ MLK 跳转框/出生格（默认关）；各层复选框独立开关；
- 动画目录/背景/遮罩等全部**直接从 PAK 解析**（`PakSource.find_path/list_dir`，
  含 `map\`/`map1\` 前缀变体），不依赖 extracted 中间文件；
- 滚轮缩放、「适应」/「1:1」、播放/暂停；「导出当前资源」导出**当前屏幕
  合成**（含图层开关与动画当前帧）的 PNG；批量导出按默认图层合成；
- 合成失败（如背景缺失）自动降级为 INI 文本预览；
- 性能：大 JPEG 背景解码结果写入 `kimi_viewer_data/imgcache/`（键含 PAK
  mtime+size，游戏更新自动失效）；静态层 QPixmap 首次可见时才创建。
  应天府（7808×5760）首次构建约 37s，缓存命中后约 16s；小地图秒级。

## 目录结构

| 文件 | 职责 |
|---|---|
| `source.py` | 索引加载（pickle 缓存）、mmap 随机读、NZ 解码、LRU 缓存、兄弟文件/调色板查找 |
| `app.py` | 主窗口、列表模型、后台解码线程、三种预览页 |
| `qtcompat.py` | Qt 绑定兼容层（PySide6 优先，PyQt5 回退） |
| `plugins/` | **可插拔格式层**（见下） |

## 性能设计

- 索引：默认取**当前工作目录下 `kimi_viewer_data/pak_inventory.json`**
  （`--inventory` 可覆盖；缓存与插件配置同目录），首载编译为 pickle
  缓存，二次启动 ~6s；每个 PAK 抽样校验偏移，游戏更新导致索引过期时自动用
  `dhsh.pak` 实时重建；**索引文件缺失/被删时直接从 PAK 全量扫描重建**
  （一次性 ~95s），并重新生成同构 JSON 供其他工具使用；非 PAKF 包
  （如 perinfo.pak APCI）以整文件单条目形式保留；
- 启动提示分三步打印（Qt 绑定 → 索引进度 → 窗口打开），状态栏显示「就绪」；
- 大量小文件：41 个 PAK 全部只读 mmap，读条目 = 切片；解码结果进 384MB
  字节配额 LRU；解码在后台线程，快速连点自动丢弃过期结果；
- 单个大文件：只切所需区间；hex 只取存储字节前 256KB（不完整解压）；
  APG 预览限 400 帧；大图仅缩放显示，原图只在导出时写出。

## 插件协议

每个插件是一个类，两个成员：

```python
class MyPlugin:
    name = "我的格式"              # 显示在信息面板
    exts = (".xyz",)              # 负责的扩展名（小写、带点）

    def load(self, payload: bytes, raw: bytes, entry, src) -> dict:
        ...
```

`load` 的四个参数：

| 参数 | 内容 |
|---|---|
| `payload` | NZ 解码后的**逻辑字节**（非 NZ 文件即存储字节），插件 normally 只用它 |
| `raw` | **存储字节**（含 12 字节 NZ 头），上限 512KB，hex 类插件用 |
| `entry` | `source.Entry`：`pak / path / name / ext / offset / size / nz` |
| `src` | `PakSource`，可继续取其他资源（见下方 API） |

返回值必须是三种之一：

```python
{"kind": "image", "frames": [PIL.Image, ...], "meta": {...}}   # 单帧/多帧图像
{"kind": "text",  "text": str,                "meta": {...}}   # 文本
{"kind": "hex",   "data": bytes, "truncated": bool, "meta": {...}}
```

- `frames` 多于 1 个时 GUI 自动启用播放条；
- `meta` 的键值原样显示在信息面板（NZ 信息由框架自动补充）；
- 插件抛异常不会崩 GUI：框架自动降级为 hex 预览并把错误写进信息面板。

### 注册（两步）

1. 插件文件末尾实例化并注册：

```python
from . import register
register(MyPlugin())
```

2. 在 `plugins/__init__.py` 的导入列表里加上模块名。

未注册任何 `exts` 的扩展名落到 `hex_fallback`（`.dat` 等目前都在这里）。
运行期可在「插件管理」对话框中禁用某插件（降级为 hex），禁用列表持久化于
`manifests/kimi_viewer_config.json`；代码侧接口为
`plugins.set_enabled(p, on)` / `plugins.is_enabled(p)`。

### PakSource 可用 API

```python
payload, nz_desc = src.decode_entry(entry)   # NZ 解码（带 LRU），nz_desc 为描述串
raw = src.raw_entry(entry, limit=None)       # 存储字节切片，limit 截断保护
sib = src.find_sibling(entry, "clrtb.cti")   # 同 PAK 同目录找文件
cti = src.resolve_clrtb(entry)               # 沿逻辑目录向上 + 跨 PAK 找调色板
```

### 完整示例：给 `.dat` 写一个摘要插件

```python
# tools/kimi_viewer/plugins/dat_summary.py
import struct

class DatSummaryPlugin:
    name = "DAT 摘要"
    exts = (".dat",)

    def load(self, payload: bytes, raw: bytes, entry, src) -> dict:
        n = len(payload) // 4
        head = struct.unpack(f"<{min(n, 8)}I", payload[: min(n, 8) * 4])
        text = f"u32 数量: {n}\n前 {len(head)} 个: {head}"
        return {"kind": "text", "text": text,
                "meta": {"大小": f"{len(payload):,}B", "插件": self.name}}

from . import register
register(DatSummaryPlugin())
```

然后在 `plugins/__init__.py` 的导入元组里加 `dat_summary` —— 完成。

### 注意事项

- `load` 在**后台线程**执行：不要碰 Qt 控件，只返回数据；PIL Image 可以
  直接返回（GUI 线程负责转 QPixmap）；
- 解码可复用 `tools/dhsh/formats/` 现有实现（a16 / apg / pg2 / apci /
  scp / map / pif / special_jpg），优先用 `*_fast` numpy 路径并回退参考实现；- 处理大文件时自行加保护（帧数上限、字节截断），参考 `apg_anim.MAX_FRAMES`
  与 `hex_fallback.HEX_LIMIT`；
- numba 首次 JIT（LZW/LZO 解压）约 5s，发生在后台线程且有磁盘缓存，
  属一次性成本。

## 现有插件

| 插件 | 扩展名 | 输出 |
|---|---|---|
| `std_image` | .jpg .jpeg .bmp .png .tga | PIL 直读；失败转私有 JPEG 解码 |
| `image_a16_pg2` | .a16 .pg2 .gal | numpy 快路径解码，PG2 多帧播放；GAL=纯 alpha 云雾层 |
| `apg_anim` | .apg | 帧组播放，自动解析 clrtb.cti 调色板 |
| `cti_view` | .cti | table1/table2 色卡 |
| `dat_pif` | .dat（PIF） | 逐帧参数表（锚点/偏移/层叠键/帧延时）；非 PIF 自动降级 hex |
| `scp_view` | .scp | 结构化预览：窗口树 + include/脚本/警告 + 原始文本 |
| `text_view` | .lua .csv .ini .txt | GBK/UTF-8 文本 |
| `map_scene` | .map | **场景合成**：背景+MAN 动画（可播放）+碰撞/遮罩/云雾/跳转图层开关，可导出当前合成 PNG |
| `map_side` | .msk .core / .cel / .man / .mlk | 遮罩逐记录帧图（白=覆盖）/ 碰撞层图（白=阻挡）/ 结构化表 |
| `hex_fallback` | （兜底） | 存储字节 hex，前 256KB |
