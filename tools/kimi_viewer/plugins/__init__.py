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
_DISABLED: set[str] = set()  # 被禁用的插件 name


def register(plugin):
    for ext in plugin.exts:
        _REGISTRY[ext] = plugin


def set_fallback(plugin):
    global _FALLBACK
    _FALLBACK = plugin


def all_plugins() -> list:
    """全部已注册插件（去重，按 name 排序）+ 兜底插件（不可禁用）。"""
    uniq = {id(p): p for p in _REGISTRY.values()}
    out = sorted(uniq.values(), key=lambda p: p.name)
    return out + ([_FALLBACK] if _FALLBACK else [])


def is_enabled(plugin) -> bool:
    return plugin.name not in _DISABLED


def set_enabled(plugin, on: bool):
    if plugin is _FALLBACK:
        return  # 兜底不可禁用
    if on:
        _DISABLED.discard(plugin.name)
    else:
        _DISABLED.add(plugin.name)


def get_plugin(ext: str):
    p = _REGISTRY.get(ext)
    if p is None or not is_enabled(p):
        return _FALLBACK
    return p


def known_exts() -> list[str]:
    return sorted(_REGISTRY)


def load_config(path: str):
    """读取 {"disabled": [插件名...]} 配置。"""
    import json
    try:
        with open(path, encoding="utf-8") as f:
            _DISABLED.update(json.load(f).get("disabled", []))
    except Exception:
        pass


def save_config(path: str):
    import json
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"disabled": sorted(_DISABLED)}, f, ensure_ascii=False,
                      indent=2)
    except Exception:
        pass


# ---- 插件注册表（新增格式在这里加 import）----
from . import (  # noqa: E402,F401
    apg_anim,
    cti_view,
    dat_pif,
    hex_fallback,
    image_a16_pg2,
    map_scene,
    map_side,
    scp_view,
    std_image,
    text_view,
)
