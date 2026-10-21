"""CTI 染色表插件：把 table1（RGB565）/ table2（RGB888+alpha）渲染成色卡。

table2 是 256×{H,S,V} 染色主表的消费端产物（见 docs/formats/a16-cti.md），
这里按消费端同样的解码方式显色。
"""
from __future__ import annotations

from PIL import Image, ImageDraw

from dhsh.formats import a16

CELL = 20
COLS = 16


def _grid(colors, label_height=18):
    rows = (len(colors) + COLS - 1) // COLS
    img = Image.new("RGBA", (COLS * CELL, rows * CELL + label_height), (40, 40, 40, 255))
    dr = ImageDraw.Draw(img)
    dr.text((2, 2), "", fill=(220, 220, 220, 255))
    for i, (r, g, b, a) in enumerate(colors):
        x, y = (i % COLS) * CELL, label_height + (i // COLS) * CELL
        dr.rectangle([x, y, x + CELL - 1, y + CELL - 1], fill=(r, g, b, a),
                     outline=(0, 0, 0, 255))
    return img


def _rgb565(c):
    return ((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63,
            (c & 31) * 255 // 31, 255)


class CTIPlugin:
    name = "CTI 染色表"
    exts = (".cti",)

    def load(self, payload: bytes, raw: bytes, entry, src) -> dict:
        cti = a16.parse_cti(payload)
        t1 = [_rgb565(c) for c in cti.table1]
        t2 = [((e >> 16) & 255, (e >> 8) & 255, e & 255, (e >> 24) & 255 or 255)
              for e in cti.table2]
        g1, g2 = _grid(t1), _grid(t2)
        img = Image.new("RGBA", (max(g1.width, g2.width), g1.height + g2.height + 36),
                        (30, 30, 30, 255))
        dr = ImageDraw.Draw(img)
        dr.text((4, 2), "table1 (RGB565 默认)", fill=(230, 230, 230, 255))
        img.paste(g1, (0, 18), g1)
        y = g1.height + 18
        dr.text((4, y + 2), "table2 (HSV 染色主表)", fill=(230, 230, 230, 255))
        img.paste(g2, (0, y + 20), g2)
        meta = {
            "色数": len(cti.table1),
            "bpp": cti.header.bpp,
            "format": cti.header.format,
            "插件": self.name,
        }
        return {"kind": "image", "frames": [img], "meta": meta}


from . import register  # noqa: E402

register(CTIPlugin())
