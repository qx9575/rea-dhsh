"""PIF 逐帧参数表插件（.dat 中的 PIF 文件，如 perinfo.dat / animation/*.dat）。

非 PIF 的 .dat 抛出异常，由框架自动降级为 hex 预览。
行字段语义：f0/f1 锚点、f2/f3 x/y 偏移、f4 层叠键、f5 帧延时（ms），
详见 docs/formats/pif.md 与 apci.md §3（12B 单元 = PIF 重排）。
"""
from __future__ import annotations

from dhsh.formats.pif import parse_pif

MAX_ROWS = 4000


class PIFPlugin:
    name = "PIF 逐帧参数表"
    exts = (".dat",)

    def load(self, payload: bytes, raw: bytes, entry, src) -> dict:
        pif = parse_pif(payload)
        lines = [
            f"PIF 逐帧参数表：{entry.path}",
            f"count={pif.count}  行数(width)={pif.width}  "
            f"行字节(height)={pif.height}  flags={pif.flags}  "
            f"data_offset={pif.data_offset}",
            "字段：f0/f1 锚点 | f2/f3 x/y 偏移 | f4 层叠键 | f5 帧延时 ms"
            "（24B 行归一化为 28B 补 0；其余 u16 原样列出）",
            "",
            "行    f0   f1   f2   f3   f4   f5   | 其余",
        ]
        for i, row in enumerate(pif.rows):
            if i >= MAX_ROWS:
                lines.append(f"… 仅显示前 {MAX_ROWS} 行")
                break
            f = list(row) + [0] * (6 - len(row))
            rest = " ".join(str(v) for v in row[6:])
            lines.append(f"{i:4d}  " + " ".join(f"{v:4d}" for v in f[:6])
                         + f"  | {rest}")
        return {"kind": "text", "text": "\n".join(lines),
                "meta": {"行数": pif.width, "行字节": pif.height,
                         "count": pif.count, "插件": self.name}}


from . import register  # noqa: E402

register(PIFPlugin())
