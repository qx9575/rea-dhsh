"""A16 / PG2 专有图像插件。

- .a16：RGB565+RLE（或内嵌 JPEG），走 a16_fast numpy 快路径，失败回退参考实现；
- .pg2：多帧容器，每帧是一个完整 AP 块，逐帧解码后支持播放。
"""
from __future__ import annotations

from dhsh.formats import a16, pg2


def _decode_ap_block(block: bytes):
    try:
        from dhsh.formats.a16_fast import a16_to_rgba_fast
        return a16_to_rgba_fast(block)
    except Exception:
        return a16.decode_a16_rgba(block)


class A16Plugin:
    name = "A16 专有图像"
    exts = (".a16",)

    def load(self, payload: bytes, raw: bytes, entry, src) -> dict:
        hdr = a16.parse_header(payload)
        img = _decode_ap_block(payload)
        meta = {
            "尺寸": f"{hdr.width}×{hdr.height}",
            "压缩": hdr.compression,
            "alpha 模式": hdr.alpha_mode,
            "流": f"s1={hdr.stream1_size}B s2={hdr.stream2_size}B",
            "插件": self.name,
        }
        return {"kind": "image", "frames": [img], "meta": meta}


class PG2Plugin:
    name = "PG2 多帧图像"
    exts = (".pg2",)

    def load(self, payload: bytes, raw: bytes, entry, src) -> dict:
        hdr, frames = pg2.iter_frames(payload)
        imgs = []
        for fr in frames:
            imgs.append(_decode_ap_block(pg2.frame_block(payload, fr)))
        meta = {
            "帧数": hdr.frame_count,
            "版本": hdr.version,
            "资源 ID": hdr.resource_id,
            "帧尺寸": ", ".join(f"{f.width}×{f.height}" for f in frames[:8])
                      + (" …" if len(frames) > 8 else ""),
            "插件": self.name,
        }
        return {"kind": "image", "frames": imgs, "meta": meta}


class GALPlugin:
    name = "GAL 云雾层"
    exts = (".gal",)

    def load(self, payload: bytes, raw: bytes, entry, src) -> dict:
        hdr = a16.parse_header(payload)
        img = _decode_ap_block(payload)
        alpha = img.getchannel("A")
        hist = alpha.histogram()
        total = hdr.width * hdr.height
        return {"kind": "image", "frames": [img],
                "meta": {"尺寸": f"{hdr.width}×{hdr.height}",
                         "流": f"s1={hdr.stream1_size}B s2={hdr.stream2_size}B",
                         "不透明占比": f"{sum(hist[255:]) / total:.1%}" if total else "—",
                         "说明": "AL 纯 alpha 层（白色 RGB + RLE8 透明度，云雾遮罩）",
                         "插件": self.name}}


from . import register  # noqa: E402

register(A16Plugin())
register(PG2Plugin())
register(GALPlugin())
