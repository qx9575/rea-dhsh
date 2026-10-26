"""地图侧文件插件：MSK/core 遮罩、CEL 碰撞层、MAN 动画表、MLK 跳转表。

- MSK/core：每条遮罩记录解成一帧 RGBA（覆盖=白色不透明，其余透明），
  多记录即多帧，可直接用播放条逐张翻看；帧数有上限保护；
- CEL：地面碰撞位图（layer 1，黑=可走、白=阻挡）；
- MAN/MLK：结构化文本表（字段语义见 docs/formats/map.md）。
"""
from __future__ import annotations

from dhsh.formats import map as dmap

MAX_MASKS = 64
MAX_ROWS = 4000


class MSKCorePlugin:
    name = "MSK/core 遮罩"
    exts = (".msk", ".core")

    def load(self, payload: bytes, raw: bytes, entry, src) -> dict:
        from PIL import Image
        import numpy as np

        msk = dmap.parse_msk(payload)
        n = min(msk.record_count, MAX_MASKS)
        frames = []
        for i in range(n):
            mask = dmap.decode_mask(msk, i)  # (h, w) 0/1
            rgba = np.zeros((*mask.shape, 4), dtype=np.uint8)
            rgba[mask != 0] = (255, 255, 255, 255)
            frames.append(Image.fromarray(rgba, "RGBA"))
        meta = {
            "版本": msk.version,
            "遮罩记录": msk.record_count,
            "放置记录": msk.placement_count,
            "静态放置": sum(1 for p in msk.placements if p.flag == 1),
            "插件": self.name,
        }
        if n < msk.record_count:
            meta["预览截断"] = f"仅前 {n} 条遮罩记录"
        if msk.record_count == 0:
            return {"kind": "text", "text": "无遮罩记录", "meta": meta}
        return {"kind": "image", "frames": frames, "meta": meta}


class CELPlugin:
    name = "CEL 碰撞层"
    exts = (".cel",)

    def load(self, payload: bytes, raw: bytes, entry, src) -> dict:
        cel = dmap.parse_cel(payload, layer=1)
        img = dmap.cel_to_image(payload, layer=1)
        layers = 2 if cel.flags & 1 else 1
        return {"kind": "image", "frames": [img],
                "meta": {"尺寸": f"{cel.width}×{cel.height} 格",
                         "层数": layers,
                         "说明": "地面层（layer 1）：黑=可走、白=阻挡",
                         "flags": cel.flags, "插件": self.name}}


def _table(title: str, header: str, rows) -> str:
    lines = [title, header]
    for i, row in enumerate(rows):
        if i >= MAX_ROWS:
            lines.append(f"… 仅显示前 {MAX_ROWS} 行")
            break
        lines.append(row)
    return "\n".join(lines)


class MANPlugin:
    name = "MAN 动画表"
    exts = (".man",)

    def load(self, payload: bytes, raw: bytes, entry, src) -> dict:
        entries = dmap.parse_man_entries(payload)
        rows = [
            f"{i:4d} {e.path}  x={e.x} y={e.y} | 排序Y偏移={e.f48} "
            f"启用={e.f4c} 宿主ID={e.f50} f54={e.f54}(遗留) "
            f"斜率Q8.8={e.f58} 定时={e.f5c}/{e.f60}"
            for i, e in enumerate(entries)]
        return {"kind": "text",
                "text": _table(f"MAN 记录 {len(entries)} 条",
                               "序号 路径  位置 | 语义字段（docs/formats/map.md §3）", rows),
                "meta": {"记录数": len(entries), "插件": self.name}}


class MLKPlugin:
    name = "MLK 跳转表"
    exts = (".mlk",)

    def load(self, payload: bytes, raw: bytes, entry, src) -> dict:
        links = dmap.parse_mlk_links(payload)
        rows = [
            f"{l.value:4d} -> {l.target}(反向={l.paired})  "
            f"入口=({l.x},{l.y},{l.w}x{l.h}) 出生格=({l.dest_x},{l.dest_y}) "
            f"朝向={l.extra0} 类型={l.extra1} 建筑={l.extra2}"
            for l in links]
        return {"kind": "text",
                "text": _table(f"MLK 记录 {len(links)} 条",
                               "链路id -> 目标图  入口矩形  出生格  extra0/1/2", rows),
                "meta": {"记录数": len(links), "插件": self.name}}


from . import register  # noqa: E402

register(MSKCorePlugin())
register(CELPlugin())
register(MANPlugin())
register(MLKPlugin())
