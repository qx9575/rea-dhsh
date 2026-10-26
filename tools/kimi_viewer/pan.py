"""右键拖动平移：大资源出现滚动条时，按住右键拖动画布。

挂在 QScrollArea + 内部画布上即可，图像页/场景页共用。
"""
from __future__ import annotations

from .qtcompat import QEvent, QObject, QCursor, Qt


class RightDragPan(QObject):
    def __init__(self, scroll, canvas):
        super().__init__(scroll)
        self.scroll = scroll
        self.canvas = canvas
        self._origin = None   # (x, y, hbar, vbar)
        canvas.installEventFilter(self)

    def eventFilter(self, obj, event):
        et = event.type()
        if et == QEvent.MouseButtonPress and event.button() == Qt.RightButton:
            pos = event.globalPos()
            self._origin = (pos.x(), pos.y(),
                            self.scroll.horizontalScrollBar().value(),
                            self.scroll.verticalScrollBar().value())
            self.canvas.setCursor(QCursor(Qt.ClosedHandCursor))
            event.accept()
            return True
        if et == QEvent.MouseMove and self._origin is not None:
            pos = event.globalPos()
            x0, y0, h0, v0 = self._origin
            self.scroll.horizontalScrollBar().setValue(h0 + x0 - pos.x())
            self.scroll.verticalScrollBar().setValue(v0 + y0 - pos.y())
            event.accept()
            return True
        if et == QEvent.MouseButtonRelease and event.button() == Qt.RightButton:
            if self._origin is not None:
                self._origin = None
                self.canvas.unsetCursor()
                event.accept()
                return True
        return False


def zoom_at(canvas, event):
    """以鼠标位置为中心缩放：光标下的内容点在缩放前后保持不动。

    画布尺寸 = 内容尺寸 × scale，滚动条值即画布左上角的偏移。
    设光标在画布局部坐标 p、缩放比 r，则滚动条需增加 p*(r-1)。
    """
    old = getattr(canvas, "_scale", 1.0)
    factor = 1.25 if event.angleDelta().y() > 0 else 0.8
    new = min(max(old * factor, 0.02), 64.0)
    if new == old:
        return
    canvas.fit = False
    canvas.zoom = new
    pos = event.pos()
    sa = getattr(canvas, "scroll_area", None)
    hbar = sa.horizontalScrollBar() if sa else None
    vbar = sa.verticalScrollBar() if sa else None
    h0, v0 = (hbar.value(), vbar.value()) if hbar else (0, 0)
    canvas._relayout()
    ratio = canvas._scale / old
    if hbar:
        hbar.setValue(round(h0 + pos.x() * (ratio - 1)))
        vbar.setValue(round(v0 + pos.y() * (ratio - 1)))
