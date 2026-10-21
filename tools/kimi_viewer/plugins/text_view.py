"""文本插件：.scp .lua .csv .ini .txt（自动 GBK/UTF-8 探测）。"""
from __future__ import annotations

from dhsh.formats.scp import decode_scp

MAX_CHARS = 2_000_000


class TextPlugin:
    name = "文本/脚本"
    exts = (".scp", ".lua", ".csv", ".ini", ".txt")

    def load(self, payload: bytes, raw: bytes, entry, src) -> dict:
        text, enc = decode_scp(payload)
        truncated = len(text) > MAX_CHARS
        if truncated:
            text = text[:MAX_CHARS]
        meta = {
            "编码": enc,
            "行数": text.count("\n") + 1,
            "字符数": len(text),
            "插件": self.name,
        }
        if truncated:
            meta["截断"] = f"仅显示前 {MAX_CHARS:,} 字符"
        return {"kind": "text", "text": text, "meta": meta}


from . import register  # noqa: E402

register(TextPlugin())
