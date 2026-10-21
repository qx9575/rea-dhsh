"""APG 动画帧组插件（全库 87.7% 的文件）。

- 逐帧解码为 RGBA，GUI 负责播放；
- AI 帧（索引色）需要同目录 clrtb.cti 调色板，找不到时给出明确提示；
- 帧数上限可配，超大帧组默认只解前 400 帧保护内存。
"""
from __future__ import annotations

from dhsh.formats import a16, apg

MAX_FRAMES = 400


def _load_palette(entry, src):
    sib = src.resolve_clrtb(entry)
    if sib is None:
        return None, None
    data, _ = src.decode_entry(sib)
    return a16.parse_cti(data), f"{sib.pak}/{sib.path}"


class APGPlugin:
    name = "APG 动画"
    exts = (".apg",)

    def load(self, payload: bytes, raw: bytes, entry, src) -> dict:
        hdr, frames = apg.iter_frames(payload)
        palette = None
        palette_path = None
        need_ai = any(f.header.magic == b"AI" for f in frames)
        if need_ai:
            palette, palette_path = _load_palette(entry, src)
            if palette is None:
                raise ValueError("含 AI 索引帧，但同目录未找到 clrtb.cti 调色板")

        try:
            from dhsh.formats.apg_fast import frame_to_rgba_fast

            def conv(f):
                return frame_to_rgba_fast(f, cti=palette)
        except Exception:
            def conv(f):
                return apg.frame_to_rgba(f, palette)

        n = min(len(frames), MAX_FRAMES)
        imgs = [conv(frames[i]) for i in range(n)]
        meta = {
            "帧数": f"{len(frames)}" + (f"（预览前 {n} 帧）" if n < len(frames) else ""),
            "AI 索引帧": "是" if need_ai else "否",
            "调色板": palette_path or "—",
            "AG": f"f0={hdr.f0} f1={hdr.f1}" if hasattr(hdr, "f0") else "—",
            "插件": self.name,
        }
        return {"kind": "image", "frames": imgs, "meta": meta}


from . import register  # noqa: E402

register(APGPlugin())
