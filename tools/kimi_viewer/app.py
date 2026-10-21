"""kimi_viewer GUI：PAK 资源查看器。

性能设计：
- 列表：QAbstractTableModel 懒渲染 + 搜索 250ms 防抖 + 结果上限截断；
- 解码：QThreadPool 后台线程（LRU 缓存），选择变化自动丢弃过期结果；
- 图像：>4000px 自动适应缩放显示，原图保留用于导出；APG 帧组播放；
- 大文件：hex 只切前 256KB；解码耗时/缓存命中显示在状态栏。
"""
from __future__ import annotations

import os
import sys
import time

from PIL import Image

from .qtcompat import (FMT_RGBA8888, QAbstractTableModel, QAction, QApplication,
                       QColor, QComboBox, QFileDialog, QHBoxLayout, QHeaderView,
                       QImage, QLabel, QLineEdit, QMainWindow, QMessageBox,
                       QModelIndex, QObject, QPainter, QPixmap, QPlainTextEdit,
                       QPushButton, QRunnable, QScrollArea, QSlider, QSpinBox,
                       QSplitter, QStackedWidget, QStatusBar, QTableView,
                       QThreadPool, QTimer, QVBoxLayout, QWidget, Qt, Signal,
                       app_exec)

from . import plugins
from .source import Entry, PakSource, human, nz_label

MAX_RESULTS = 200_000
COLS = ("PAK", "路径", "扩展", "大小", "NZ")


# ---------------------------------------------------------------- 列表模型

class EntryModel(QAbstractTableModel):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.entries: list[Entry] = []
        self.idx: list[int] = []

    def set_data(self, entries: list[Entry], idx: list[int]):
        self.beginResetModel()
        self.entries = entries
        self.idx = idx
        self.endResetModel()

    def entry_at(self, row: int) -> Entry:
        return self.entries[self.idx[row]]

    def rowCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(self.idx)

    def columnCount(self, parent=QModelIndex()):
        return len(COLS)

    def headerData(self, section, orientation, role=Qt.DisplayRole):
        if role == Qt.DisplayRole and orientation == Qt.Horizontal:
            return COLS[section]

    def data(self, index, role=Qt.DisplayRole):
        if role != Qt.DisplayRole or not index.isValid():
            return None
        e = self.entries[self.idx[index.row()]]
        c = index.column()
        if c == 0:
            return e.pak
        if c == 1:
            return e.path
        if c == 2:
            return e.ext
        if c == 3:
            return human(e.size)
        if c == 4:
            return nz_label(e.nz)
        return None


# ---------------------------------------------------------------- 解码工作线程

class _Signals(QObject):
    done = Signal(int, dict)


class DecodeJob(QRunnable):
    def __init__(self, token: int, src: PakSource, entry: Entry):
        super().__init__()
        self.token, self.src, self.entry = token, src, entry
        self.signals = _Signals()

    def run(self):
        t0 = time.perf_counter()
        out = {"entry": self.entry}
        try:
            raw = self.src.raw_entry(self.entry, limit=512 * 1024)
            payload, nz_desc = self.src.decode_entry(self.entry)
            plugin = plugins.get_plugin(self.entry.ext)
            try:
                res = plugin.load(payload, raw, self.entry, self.src)
            except Exception as ex:  # noqa: BLE001 — 插件失败自动落 hex
                res = {"kind": "hex",
                       "data": raw[:256 * 1024],
                       "truncated": len(raw) > 256 * 1024,
                       "meta": {"插件错误": f"{type(ex).__name__}: {ex}",
                                "说明": "已自动降级为 hex 预览"}}
            res["meta"] = {"NZ": nz_desc, **res.get("meta", {})}
            out.update(ok=True, result=res)
        except Exception as ex:  # noqa: BLE001
            out.update(ok=False, error=f"{type(ex).__name__}: {ex}")
        out["ms"] = (time.perf_counter() - t0) * 1000
        self.signals.done.emit(self.token, out)


# ---------------------------------------------------------------- 图像画布

class ImageCanvas(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._pix: QPixmap | None = None
        self.zoom = 1.0
        self.fit = True
        tile = QPixmap(16, 16)
        p = QPainter(tile)
        p.fillRect(0, 0, 16, 16, QColor(120, 120, 120))
        p.fillRect(0, 0, 8, 8, QColor(160, 160, 160))
        p.fillRect(8, 8, 8, 8, QColor(160, 160, 160))
        p.end()
        self._tile = tile

    def set_pixmap(self, pix: QPixmap | None):
        self._pix = pix
        self._relayout()

    def _relayout(self):
        if self._pix is None:
            self.resize(64, 64)
        else:
            if self.fit and self.parent() is not None:
                vw = max(self.parent().width() - 4, 64)
                vh = max(self.parent().height() - 4, 64)
                s = min(vw / self._pix.width(), vh / self._pix.height())
            else:
                s = self.zoom
            self._scale = max(s, 0.01)
            w = max(1, int(self._pix.width() * self._scale))
            h = max(1, int(self._pix.height() * self._scale))
            self.resize(w, h)
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.drawTiledPixmap(self.rect(), self._tile)
        if self._pix is not None:
            p.setRenderHint(QPainter.SmoothPixmapTransform)
            p.drawPixmap(self.rect(), self._pix)
        p.end()

    def wheelEvent(self, event):
        if self._pix is None:
            return
        self.fit = False
        self.zoom = getattr(self, "_scale", 1.0)
        self.zoom *= 1.25 if event.angleDelta().y() > 0 else 0.8
        self.zoom = min(max(self.zoom, 0.02), 64.0)
        self._relayout()


# ---------------------------------------------------------------- 预览页

class ImagePage(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.frames: list[Image.Image] = []
        self.cur = 0
        bar = QHBoxLayout()
        self.btn_fit = QPushButton("适应")
        self.btn_11 = QPushButton("1:1")
        self.btn_play = QPushButton("播放")
        self.fps = QSpinBox(minimum=1, maximum=60, value=10)
        self.slider = QSlider(Qt.Horizontal)
        self.lbl = QLabel("—")
        for w in (self.btn_fit, self.btn_11, self.btn_play, QLabel("FPS"),
                  self.fps, self.slider, self.lbl):
            bar.addWidget(w)
        bar.setStretchFactor(self.slider, 1)
        self.scroll = QScrollArea(widgetResizable=False)
        self.canvas = ImageCanvas()
        self.scroll.setWidget(self.canvas)
        lay = QVBoxLayout(self)
        lay.addLayout(bar)
        lay.addWidget(self.scroll, 1)
        self.timer = QTimer(self)
        self.timer.timeout.connect(self._next_frame)
        self.btn_fit.clicked.connect(self._fit)
        self.btn_11.clicked.connect(self._11)
        self.btn_play.clicked.connect(self._toggle)
        self.slider.valueChanged.connect(self._seek)

    def set_frames(self, frames: list[Image.Image]):
        self.timer.stop()
        self.btn_play.setText("播放")
        self.frames = frames
        self.cur = 0
        n = len(frames)
        self.slider.setRange(0, max(0, n - 1))
        self.slider.setValue(0)
        multi = n > 1
        for w in (self.btn_play, self.fps, self.slider):
            w.setEnabled(multi)
        self._show(0)

    def _qimg(self, i: int) -> QPixmap:
        img = self.frames[i]
        if img.mode != "RGBA":
            img = img.convert("RGBA")
        buf = img.tobytes("raw", "RGBA")
        q = QImage(buf, img.width, img.height, img.width * 4, FMT_RGBA8888)
        return QPixmap.fromImage(q.copy())

    def _show(self, i: int):
        if not self.frames:
            self.canvas.set_pixmap(None)
            self.lbl.setText("—")
            return
        self.cur = i
        self.canvas.set_pixmap(self._qimg(i))
        img = self.frames[i]
        self.lbl.setText(f"{i + 1}/{len(self.frames)}  {img.width}×{img.height}")
        if i != self.slider.value():
            self.slider.blockSignals(True)
            self.slider.setValue(i)
            self.slider.blockSignals(False)

    def _next_frame(self):
        if self.frames:
            self._show((self.cur + 1) % len(self.frames))

    def _toggle(self):
        if self.timer.isActive():
            self.timer.stop()
            self.btn_play.setText("播放")
        elif len(self.frames) > 1:
            self.timer.start(1000 // self.fps.value())
            self.btn_play.setText("暂停")

    def _seek(self, v):
        if self.frames:
            self._show(v)

    def _fit(self):
        self.canvas.fit = True
        self.canvas._relayout()

    def _11(self):
        self.canvas.fit = False
        self.canvas.zoom = 1.0
        self.canvas._relayout()


class TextPage(QPlainTextEdit):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setReadOnly(True)
        self.setLineWrapMode(QPlainTextEdit.NoWrap)
        f = self.font()
        f.setFamily("Consolas")
        self.setFont(f)


# ---------------------------------------------------------------- 主窗口

class MainWindow(QMainWindow):
    def __init__(self, src: PakSource):
        super().__init__()
        self.src = src
        self.token = 0
        self.pool = QThreadPool(self)
        self.pool.setMaxThreadCount(2)
        self.setWindowTitle("kimi_viewer — 新水浒Q传资源查看器")
        self.resize(1280, 800)

        # 过滤器数据
        self._hay = [f"{e.pak}/{e.path}".lower() for e in src.entries]
        self._pak_counts: dict[str, int] = {}
        self._ext_counts: dict[str, int] = {}
        for e in src.entries:
            self._pak_counts[e.pak] = self._pak_counts.get(e.pak, 0) + 1
            self._ext_counts[e.ext] = self._ext_counts.get(e.ext, 0) + 1

        # 左：过滤 + 列表
        self.cmb_pak = QComboBox()
        self.cmb_pak.addItem(f"全部 PAK（{len(src.entries):,}）")
        for p in sorted(self._pak_counts):
            self.cmb_pak.addItem(f"{p}（{self._pak_counts[p]:,}）", p)
        self.cmb_ext = QComboBox()
        self.cmb_ext.addItem("全部格式")
        for x, n in sorted(self._ext_counts.items(), key=lambda kv: -kv[1]):
            self.cmb_ext.addItem(f"{x}（{n:,}）", x)
        self.ed_search = QLineEdit(placeholderText="路径包含…（250ms 防抖）")
        self.model = EntryModel(self)
        self.table = QTableView()
        self.table.setModel(self.model)
        self.table.setSelectionBehavior(QTableView.SelectRows)
        self.table.setSelectionMode(QTableView.SingleSelection)
        self.table.verticalHeader().setVisible(False)
        self.table.horizontalHeader().setSectionResizeMode(1, QHeaderView.Stretch)
        self.table.setShowGrid(False)
        left = QWidget()
        ll = QVBoxLayout(left)
        fl = QHBoxLayout()
        fl.addWidget(self.cmb_pak, 1)
        fl.addWidget(self.cmb_ext, 1)
        fl.addWidget(self.ed_search, 2)
        ll.addLayout(fl)
        ll.addWidget(self.table, 1)

        # 右：预览页 + 信息面板
        self.page_image = ImagePage()
        self.page_text = TextPage()
        self.page_hex = TextPage()
        self.stack = QStackedWidget()
        for w in (self.page_image, self.page_text, self.page_hex):
            self.stack.addWidget(w)
        self.info = QPlainTextEdit()
        self.info.setReadOnly(True)
        self.info.setMaximumBlockCount(1000)
        right = QSplitter(Qt.Vertical)
        right.addWidget(self.stack)
        right.addWidget(self.info)
        right.setSizes([560, 180])

        split = QSplitter()
        split.addWidget(left)
        split.addWidget(right)
        split.setSizes([620, 660])
        self.setCentralWidget(split)
        self.setStatusBar(QStatusBar())

        act = QAction("导出当前资源…", self)
        act.triggered.connect(self._export)
        self.addAction(act)
        self.toolbar = self.addToolBar("主")
        self.toolbar.addAction(act)

        # 事件
        self._debounce = QTimer(self)
        self._debounce.setSingleShot(True)
        self._debounce.setInterval(250)
        self._debounce.timeout.connect(self._apply_filter)
        self.ed_search.textChanged.connect(self._debounce.start)
        self.cmb_pak.currentIndexChanged.connect(self._apply_filter)
        self.cmb_ext.currentIndexChanged.connect(self._apply_filter)
        self.table.selectionModel().selectionChanged.connect(self._on_select)

        self._apply_filter()

    # ---- 过滤 ----
    def _apply_filter(self):
        pak = self.cmb_pak.currentData()
        ext = self.cmb_ext.currentData()
        sub = self.ed_search.text().strip().lower()
        t0 = time.perf_counter()
        idx = []
        entries = self.src.entries
        hay = self._hay
        for i, e in enumerate(entries):
            if pak and e.pak != pak:
                continue
            if ext and e.ext != ext:
                continue
            if sub and sub not in hay[i]:
                continue
            idx.append(i)
            if len(idx) >= MAX_RESULTS:
                break
        self.model.set_data(entries, idx)
        ms = (time.perf_counter() - t0) * 1000
        note = f"（已达 {MAX_RESULTS:,} 上限）" if len(idx) >= MAX_RESULTS else ""
        self.statusBar().showMessage(
            f"{len(idx):,} / {len(entries):,} 条{note}，过滤 {ms:.0f}ms")

    # ---- 解码 ----
    def _on_select(self):
        rows = self.table.selectionModel().selectedRows()
        if not rows:
            return
        entry = self.model.entry_at(rows[0].row())
        self.token += 1
        self.info.setPlainText(f"解码中… {entry.key}")
        job = DecodeJob(self.token, self.src, entry)
        job.signals.done.connect(self._on_decoded)
        self.pool.start(job)

    def _on_decoded(self, token: int, out: dict):
        if token != self.token:
            return  # 过期结果
        entry: Entry = out["entry"]
        ms = out["ms"]
        if not out["ok"]:
            self.stack.setCurrentWidget(self.page_text)
            self.page_text.setPlainText("")
            self.info.setPlainText(f"✗ {entry.key}\n{out['error']}\n\n"
                                   f"可尝试 hex 预览（未注册格式会自动用 hex）")
            self.statusBar().showMessage(f"解码失败，{ms:.0f}ms")
            return
        res = out["result"]
        kind = res["kind"]
        if kind == "image":
            self.page_image.set_frames(res["frames"])
            self.stack.setCurrentWidget(self.page_image)
        elif kind == "text":
            self.page_text.setPlainText(res["text"])
            self.stack.setCurrentWidget(self.page_text)
        else:
            self.page_hex.setPlainText(plugins.hex_fallback.hexdump(res["data"]))
            self.stack.setCurrentWidget(self.page_hex)
        lines = [f"{k}：{v}" for k, v in res["meta"].items()]
        self.info.setPlainText(f"✓ {entry.key}  （{ms:.0f}ms）\n" + "\n".join(lines))
        self.statusBar().showMessage(
            f"{entry.key} 解码 {ms:.0f}ms　{self.src.cache.stats()}")

    # ---- 导出 ----
    def _export(self):
        rows = self.table.selectionModel().selectedRows()
        if not rows:
            return
        entry = self.model.entry_at(rows[0].row())
        payload, _ = self.src.decode_entry(entry)
        plugin = plugins.get_plugin(entry.ext)
        try:
            res = plugin.load(payload, payload, entry, self.src)
        except Exception as ex:  # noqa: BLE001
            QMessageBox.warning(self, "导出失败", str(ex))
            return
        stem = os.path.splitext(entry.name)[0] or "out"
        if res["kind"] == "image":
            frames = res["frames"]
            if len(frames) == 1:
                path, _ = QFileDialog.getSaveFileName(self, "导出 PNG", stem + ".png")
                if path:
                    frames[0].save(path)
            else:
                d = QFileDialog.getExistingDirectory(self, "选择帧输出目录")
                if d:
                    for i, fr in enumerate(frames):
                        fr.save(os.path.join(d, f"{stem}_f{i:03d}.png"))
        elif res["kind"] == "text":
            path, _ = QFileDialog.getSaveFileName(self, "导出文本", stem + ".txt")
            if path:
                with open(path, "w", encoding="utf-8") as f:
                    f.write(res["text"])
        else:
            path, _ = QFileDialog.getSaveFileName(self, "导出原始字节", entry.name)
            if path:
                with open(path, "wb") as f:
                    f.write(payload)

    def closeEvent(self, event):
        self.src.close()
        super().closeEvent(event)


def run(src: PakSource):
    app = QApplication.instance() or QApplication(sys.argv)
    win = MainWindow(src)
    win.show()
    return app_exec(app)
