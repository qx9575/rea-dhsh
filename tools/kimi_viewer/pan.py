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
