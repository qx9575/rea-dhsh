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
- 透明通道：图像画布背景可在 棋盘格/黑/白 间切换（工具条「背景」按钮），
  便于检查 alpha 边缘。

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
| `text_view` | .lua .csv .ini .txt .map | GBK/UTF-8 文本 |
| `map_side` | .msk .core / .cel / .man / .mlk | 遮罩逐记录帧图（白=覆盖）/ 碰撞层图（白=阻挡）/ 结构化表 |
| `hex_fallback` | （兜底） | 存储字节 hex，前 256KB |
