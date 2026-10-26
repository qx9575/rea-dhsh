"""场景插件：打开 .map 时合成可交互场景（背景+动画+碰撞+遮罩+云雾+跳转）。

背景缺失等失败场景降级为 INI 文本预览（与原行为一致）。
"""
from __future__ import annotations


class MapScenePlugin:
    name = "地图场景"
    exts = (".map",)

    def load(self, payload: bytes, raw: bytes, entry, src) -> dict:
        from ..scene import build_scene
        try:
            scene = build_scene(src, entry)
        except Exception as ex:  # noqa: BLE001 — 合成失败回退 INI 文本
            from .text_view import TextPlugin
            res = TextPlugin().load(payload, raw, entry, src)
            res["meta"]["场景合成"] = f"不可用（{type(ex).__name__}: {ex}），显示 INI 文本"
            return res
        return {"kind": "scene", "scene": scene, "meta": scene.meta}


from . import register  # noqa: E402

register(MapScenePlugin())
