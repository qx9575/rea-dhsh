"""格式插件框架。

插件协议：每个插件声明 ``exts``（小写扩展名元组）并实现
``load(payload, raw, entry, src) -> dict``：

- ``payload``：NZ 解码后的逻辑字节（非 NZ 文件即存储字节）
- ``raw``：存储字节（含 NZ 头）
- 返回 dict，必须含 ``kind``：
    ``{"kind": "image", "frames": [PIL.Image...], "meta": {...}}``
    ``{"kind": "text",  "text": str, "meta": {...}}``
    ``{"kind": "hex",   "data": bytes, "truncated": bool, "meta": {...}}``
  ``meta`` 键值会显示在信息面板。

注册：在模块 import 时调用 ``register(PluginInstance)``；未注册的扩展名
落到 ``fallback``（hex 预览）。新增格式 = 新建一个插件文件并在
``PLUGINS`` 导入列表里加一行，无需改 GUI。
"""
from __future__ import annotations

_REGISTRY: dict[str, object] = {}
_FALLBACK = None


def register(plugin):
    for ext in plugin.exts:
        _REGISTRY[ext] = plugin


def set_fallback(plugin):
    global _FALLBACK
    _FALLBACK = plugin


def get_plugin(ext: str):
    return _REGISTRY.get(ext, _FALLBACK)


def known_exts() -> list[str]:
    return sorted(_REGISTRY)


# ---- 插件注册表（新增格式在这里加 import）----
from . import (  # noqa: E402,F401
    apg_anim,
    cti_view,
    hex_fallback,
    image_a16_pg2,
    std_image,
    text_view,
)
