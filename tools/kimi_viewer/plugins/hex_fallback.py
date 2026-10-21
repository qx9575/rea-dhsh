"""Hex 兜底插件：所有未注册扩展名（.dat .core .cel .map .man .mlk .msk 等）。

只切存储字节的前 256KB，不完整解压大文件——这是单个大文件的性能保护。
"""
from __future__ import annotations

HEX_LIMIT = 256 * 1024


def hexdump(data: bytes, base: int = 0) -> str:
    lines = []
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        hexs = " ".join(f"{b:02x}" for b in chunk)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{base + off:08x}  {hexs:<48s}  {asc}")
    return "\n".join(lines)


class HexFallbackPlugin:
    name = "Hex 兜底"
    exts: tuple = ()

    def load(self, payload: bytes, raw: bytes, entry, src) -> dict:
        data = raw[:HEX_LIMIT]
        truncated = len(raw) > HEX_LIMIT
        meta = {
            "存储大小": f"{entry.size:,}B",
            "插件": self.name,
            "说明": "未注册格式，显示存储字节（含 NZ 头）",
        }
        if truncated:
            meta["截断"] = f"仅显示前 {HEX_LIMIT // 1024}KB"
        return {"kind": "hex", "data": data, "truncated": truncated, "meta": meta}


from . import set_fallback  # noqa: E402

set_fallback(HexFallbackPlugin())
