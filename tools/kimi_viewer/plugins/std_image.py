"""标准图像插件：.jpg .jpeg .bmp .png .tga（PIL 直读）。

注意：游戏内部分 .jpg 是私有变体（见 docs/formats/special-jpg.md），
PIL 打不开时给出明确错误并提示用 hex 预览。
"""
from __future__ import annotations

import io

from PIL import Image


class StdImagePlugin:
    name = "标准图像"
    exts = (".jpg", ".jpeg", ".bmp", ".png", ".tga")

    def load(self, payload: bytes, raw: bytes, entry, src) -> dict:
        try:
            img = Image.open(io.BytesIO(payload))
            img.load()
            fmt = f"{img.format} {img.mode}"
        except Exception:
            # 私有 JPEG 变体（docs/formats/special-jpg.md）
            from dhsh.formats import special_jpg

            arr = special_jpg.decode(special_jpg.parse(payload))
            img = Image.fromarray(arr, "RGB")
            fmt = f"私有 JPEG v{special_jpg.parse(payload).version}"
        meta = {
            "格式": fmt,
            "尺寸": f"{img.width}×{img.height}",
            "插件": self.name,
        }
        frames = [img.convert("RGBA")]
        return {"kind": "image", "frames": frames, "meta": meta}


from . import register  # noqa: E402

register(StdImagePlugin())
