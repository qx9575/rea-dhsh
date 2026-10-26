"""场景预览页：图层开关 + MAN 动画播放 + 缩放，导出当前合成图。

非 GUI 的场景构建在 scene.py；本模块只做合成与交互。
"""
from __future__ import annotations

from PIL import Image

from .qtcompat import (FMT_RGBA8888, QCheckBox, QColor, QHBoxLayout, QImage,
                       QLabel, QPainter, QPixmap, QPushButton, QScrollArea,
                       QTimer, QVBoxLayout, QWidget, Qt)
from .scene import blit_clip


def _to_pix(img: Image.Image) -> QPixmap:
    if img.mode != "RGBA":
        img = img.convert("RGBA")
    buf = img.tobytes("raw", "RGBA")
    q = QImage(buf, img.width, img.height, img.width * 4, FMT_RGBA8888)
    return QPixmap.fromImage(q.copy())


class SceneCanvas(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.scene = None
        self._bg = None
        self._layer_pix = []   # 每层静态图 QPixmap 或 None
        self._anim_pix = []    # 每层 [[QPixmap...] per anim]
        self._anim_state = []  # 每层 [[frame_idx, elapsed_ms] per anim]
        self.playing = True
        self.fit = True
        self.zoom = 1.0
        self._scale = 1.0
        tile = QPixmap(16, 16)
        p = QPainter(tile)
        p.fillRect(0, 0, 16, 16, QColor(40, 40, 40))
        p.end()
        self._tile = tile

    def set_scene(self, scene):
        self.scene = scene
        self._bg = _to_pix(scene.background)
        # 静态叠加层保持 PIL，首次可见时才转 QPixmap（大地图每层上百 MB）
        self._layer_pix = [None] * len(scene.layers)
        self._anim_pix = []
        self._anim_state = []
        for layer in scene.layers:
            self._anim_pix.append([[_to_pix(fr) for fr in anim.frames]
                                   for anim in layer.anims])
            self._anim_state.append([[0, 0] for _ in layer.anims])
        self._relayout()

    def tick(self, ms: int):
        if not self.playing or self.scene is None:
            return
        changed = False
        for li, layer in enumerate(self.scene.layers):
            if not layer.visible:
                continue
            for ai, anim in enumerate(layer.anims):
                state = self._anim_state[li][ai]
                state[1] += ms
                n = len(anim.frames)
                guard = 0
                while state[1] >= anim.delays[state[0]] and guard < 100:
                    state[1] -= anim.delays[state[0]]
                    state[0] = (state[0] + 1) % n
                    guard += 1
                    changed = True
        if changed:
            self.update()

    def _relayout(self):
        if self.scene is None:
            self.resize(64, 64)
        else:
            if self.fit and self.parent() is not None:
                vw = max(self.parent().width() - 4, 64)
                vh = max(self.parent().height() - 4, 64)
                s = min(vw / self.scene.width, vh / self.scene.height)
            else:
                s = self.zoom
            self._scale = max(s, 0.01)
            self.resize(max(1, int(self.scene.width * self._scale)),
                        max(1, int(self.scene.height * self._scale)))
        self.update()

    def wheelEvent(self, event):
        if self.scene is None:
            return
        self.fit = False
        self.zoom = self._scale
        self.zoom *= 1.25 if event.angleDelta().y() > 0 else 0.8
        self.zoom = min(max(self.zoom, 0.02), 64.0)
        self._relayout()

    def paintEvent(self, event):
        p = QPainter(self)
        p.drawTiledPixmap(self.rect(), self._tile)
        if self.scene is not None:
            p.setRenderHint(QPainter.SmoothPixmapTransform)
            p.scale(self._scale, self._scale)
            p.drawPixmap(0, 0, self._bg)
            for li, layer in enumerate(self.scene.layers):
                if not layer.visible:
                    continue
                if layer.image is not None and self._layer_pix[li] is None:
                    self._layer_pix[li] = _to_pix(layer.image)
                pix = self._layer_pix[li]
                if pix is not None:
                    p.drawPixmap(0, 0, pix)
                for ai, anim in enumerate(layer.anims):
                    idx = self._anim_state[li][ai][0]
                    ox, oy = anim.offsets[idx]
                    p.drawPixmap(anim.x + ox, anim.y + oy,
                                 self._anim_pix[li][ai][idx])
        p.end()

    def composite_current(self) -> Image.Image:
        """当前屏幕内容（含动画当前帧、图层开关）合成为 RGB。"""
        base = self.scene.background.convert("RGBA")
        for li, layer in enumerate(self.scene.layers):
            if not layer.visible:
                continue
            if layer.image is not None:
                base.alpha_composite(layer.image, (0, 0))
            for ai, anim in enumerate(layer.anims):
                idx = self._anim_state[li][ai][0]
                ox, oy = anim.offsets[idx]
                blit_clip(base, anim.frames[idx].convert("RGBA"),
                          anim.x + ox, anim.y + oy)
        return base.convert("RGB")


class ScenePage(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.scene = None
        self.canvas = SceneCanvas()
        self.scroll = QScrollArea(widgetResizable=False)
        self.scroll.setWidget(self.canvas)
        from .pan import RightDragPan
        self._pan = RightDragPan(self.scroll, self.canvas)
        self.bar = QHBoxLayout()
        self.btn_play = QPushButton("暂停")
        self.btn_fit = QPushButton("适应")
        self.btn_11 = QPushButton("1:1")
        for w in (self.btn_play, self.btn_fit, self.btn_11):
            self.bar.addWidget(w)
        self._checks = []
        self.bar.addStretch(1)
        lay = QVBoxLayout(self)
        lay.addLayout(self.bar)
        lay.addWidget(self.scroll, 1)
        self.timer = QTimer(self)
        self.timer.timeout.connect(lambda: self.canvas.tick(30))
        self.timer.start(30)
        self.btn_play.clicked.connect(self._toggle)
        self.btn_fit.clicked.connect(self._fit)
        self.btn_11.clicked.connect(self._11)

    def set_scene(self, scene):
        self.scene = scene
        for cb in self._checks:
            self.bar.removeWidget(cb)
            cb.deleteLater()
        self._checks = []
        for layer in scene.layers:
            if layer.image is None and not layer.anims:
                continue
            cb = QCheckBox(layer.label)
            cb.setChecked(layer.visible)
            cb.stateChanged.connect(
                lambda _s, l=layer, c=cb: self._toggle_layer(l, c))
            self._checks.append(cb)
            self.bar.addWidget(cb)
        self.canvas.set_scene(scene)

    def _toggle_layer(self, layer, cb):
        layer.visible = cb.isChecked()
        self.canvas.update()

    def _toggle(self):
        self.canvas.playing = not self.canvas.playing
        self.btn_play.setText("暂停" if self.canvas.playing else "播放")

    def _fit(self):
        self.canvas.fit = True
        self.canvas._relayout()

    def _11(self):
        self.canvas.fit = False
        self.canvas.zoom = 1.0
        self.canvas._relayout()

    def composite_current(self):
        return self.canvas.composite_current()
