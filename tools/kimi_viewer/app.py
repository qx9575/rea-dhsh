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
                       QCheckBox, QColor, QComboBox, QDialog, QDialogButtonBox,
                       QFileDialog, QHBoxLayout, QHeaderView,
                       QImage, QLabel, QLineEdit, QMainWindow, QMessageBox,
                       QModelIndex, QObject, QPainter, QPixmap, QPlainTextEdit,
                       QProgressDialog, QPushButton, QRunnable, QScrollArea,
                       QSlider, QSpinBox,
                       QSplitter, QStackedWidget, QStatusBar, QTableView,
                       QThreadPool, QTimer, QVBoxLayout, QWidget, Qt, Signal,
                       app_exec)

from . import plugins
from .scene_view import ScenePage
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
    BG_MODES = ("棋盘格", "黑", "白")

    def __init__(self, parent=None):
        super().__init__(parent)
        self._pix: QPixmap | None = None
        self.zoom = 1.0
        self.fit = True
        self.bg_mode = 0  # 透明通道检查：棋盘格/黑/白
        tile = QPixmap(16, 16)
        p = QPainter(tile)
        p.fillRect(0, 0, 16, 16, QColor(120, 120, 120))
        p.fillRect(0, 0, 8, 8, QColor(160, 160, 160))
        p.fillRect(8, 8, 8, 8, QColor(160, 160, 160))
        p.end()
        self._tile = tile

    def cycle_bg(self) -> str:
        self.bg_mode = (self.bg_mode + 1) % len(self.BG_MODES)
        self.update()
        return self.BG_MODES[self.bg_mode]

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
        if self.bg_mode == 0:
            p.drawTiledPixmap(self.rect(), self._tile)
        else:
            p.fillRect(self.rect(), QColor(0, 0, 0) if self.bg_mode == 1
                       else QColor(255, 255, 255))
        if self._pix is not None:
            p.setRenderHint(QPainter.SmoothPixmapTransform)
            p.drawPixmap(self.rect(), self._pix)
        p.end()

    def wheelEvent(self, event):
        if self._pix is None:
            return
        from .pan import zoom_at
        zoom_at(self, event)


# ---------------------------------------------------------------- 预览页

class ImagePage(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.frames: list[Image.Image] = []
        self.cur = 0
        bar = QHBoxLayout()
        self.btn_fit = QPushButton("适应")
        self.btn_11 = QPushButton("1:1")
        self.btn_bg = QPushButton("背景:棋盘格")
        self.btn_play = QPushButton("播放")
        self.fps = QSpinBox(minimum=1, maximum=60, value=10)
        self.slider = QSlider(Qt.Horizontal)
        self.lbl = QLabel("—")
        for w in (self.btn_fit, self.btn_11, self.btn_bg, self.btn_play,
                  QLabel("FPS"), self.fps, self.slider, self.lbl):
            bar.addWidget(w)
        bar.setStretchFactor(self.slider, 1)
        self.scroll = QScrollArea(widgetResizable=False)
        self.canvas = ImageCanvas()
        self.scroll.setWidget(self.canvas)
        self.canvas.scroll_area = self.scroll
        from .pan import RightDragPan
        self._pan = RightDragPan(self.scroll, self.canvas)
        lay = QVBoxLayout(self)
        lay.addLayout(bar)
        lay.addWidget(self.scroll, 1)
        self.timer = QTimer(self)
        self.timer.timeout.connect(self._next_frame)
        self.btn_fit.clicked.connect(self._fit)
        self.btn_11.clicked.connect(self._11)
        self.btn_bg.clicked.connect(self._bg)
        self.btn_play.clicked.connect(self._toggle)
        self.slider.valueChanged.connect(self._seek)

    def _bg(self):
        self.btn_bg.setText(f"背景:{self.canvas.cycle_bg()}")

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


# ---------------------------------------------------------------- 批量导出

_INVALID = '<>:"|?*'


def _safe_rel(path: str) -> str:
    return "".join("_" if c in _INVALID else c for c in path)


class _BatchSignals(QObject):
    progress = Signal(int, int, str)
    finished = Signal(dict)


class BatchExportJob(QRunnable):
    """批量导出：解码 → 按类型落盘。在后台线程运行，可取消。"""

    def __init__(self, src: PakSource, entries: list[Entry], out_dir: str):
        super().__init__()
        self.src, self.entries, self.out_dir = src, entries, out_dir
        self.signals = _BatchSignals()
        self.cancelled = False

    def cancel(self):
        self.cancelled = True

    def _dest(self, e: Entry, suffix: str) -> str:
        rel = _safe_rel(e.path)
        base, _ = os.path.splitext(rel)
        pak = _safe_rel(e.pak)
        path = os.path.join(self.out_dir, pak, base + suffix)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        return path

    def run(self):
        ok = fail = 0
        errors = []
        total = len(self.entries)
        t0 = time.perf_counter()
        for i, e in enumerate(self.entries):
            if self.cancelled:
                break
            try:
                payload, _ = self.src.decode_entry(e)
                res = plugins.get_plugin(e.ext).load(payload, payload, e,
                                                     self.src)
                kind = res["kind"]
                if kind == "scene":
                    from .scene import composite
                    composite(res["scene"]).save(self._dest(e, ".png"))
                elif kind == "image":
                    frames = res["frames"]
                    if len(frames) == 1:
                        frames[0].save(self._dest(e, ".png"))
                    else:
                        stem, _ = os.path.splitext(_safe_rel(e.path))
                        for j, fr in enumerate(frames):
                            d = os.path.join(self.out_dir,
                                             _safe_rel(e.pak), stem)
                            os.makedirs(d, exist_ok=True)
                            fr.save(os.path.join(d, f"f{j:03d}.png"))
                elif kind == "text":
                    with open(self._dest(e, ".txt"), "w",
                              encoding="utf-8") as f:
                        f.write(res["text"])
                else:
                    with open(self._dest(e, ".bin"), "wb") as f:
                        f.write(payload)
                ok += 1
            except Exception as ex:  # noqa: BLE001
                fail += 1
                if len(errors) < 20:
                    errors.append(f"{e.key}: {type(ex).__name__}: {ex}")
            if i % 10 == 0 or i == total - 1:
                self.signals.progress.emit(i + 1, total, e.key)
        self.signals.finished.emit({
            "ok": ok, "fail": fail, "total": total,
            "cancelled": self.cancelled, "errors": errors,
            "elapsed": time.perf_counter() - t0,
        })


class BatchExportDialog(QDialog):
    """范围：当前过滤结果 / 指定 PAK / 指定格式 / PAK+格式。"""

    SCOPES = ("当前过滤结果", "指定 PAK", "指定格式", "指定 PAK + 格式")

    def __init__(self, parent, src: PakSource, filtered: list[Entry],
                 pak_counts: dict, ext_counts: dict):
        super().__init__(parent)
        self.setWindowTitle("批量导出")
        self.src = src
        self.filtered = filtered
        self.selected: list[Entry] = []
        self.out_dir = ""

        lay = QVBoxLayout(self)
        row = QHBoxLayout()
        self.cmb_scope = QComboBox()
        self.cmb_scope.addItems(self.SCOPES)
        self.cmb_pak = QComboBox()
        self.cmb_pak.addItems(sorted(pak_counts))
        self.cmb_ext = QComboBox()
        self.cmb_ext.addItems(sorted(ext_counts))
        row.addWidget(self.cmb_scope)
        row.addWidget(self.cmb_pak)
        row.addWidget(self.cmb_ext)
        lay.addLayout(row)

        row2 = QHBoxLayout()
        self.ed_dir = QLineEdit(placeholderText="输出目录")
        btn = QPushButton("选择")
        btn.clicked.connect(self._pick_dir)
        row2.addWidget(self.ed_dir, 1)
        row2.addWidget(btn)
        lay.addLayout(row2)

        self.lbl_count = QLabel()
        lay.addWidget(self.lbl_count)
        bb = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        bb.accepted.connect(self.accept)
        bb.rejected.connect(self.reject)
        lay.addWidget(bb)

        self.cmb_scope.currentIndexChanged.connect(self._refresh)
        self.cmb_pak.currentIndexChanged.connect(self._refresh)
        self.cmb_ext.currentIndexChanged.connect(self._refresh)
        self._refresh()

    def _pick_dir(self):
        d = QFileDialog.getExistingDirectory(self, "选择输出目录")
        if d:
            self.ed_dir.setText(d)

    def _refresh(self):
        scope = self.cmb_scope.currentIndex()
        self.cmb_pak.setEnabled(scope in (1, 3))
        self.cmb_ext.setEnabled(scope in (2, 3))
        if scope == 0:
            sel = self.filtered
        else:
            pak = self.cmb_pak.currentText() if scope in (1, 3) else None
            ext = self.cmb_ext.currentText() if scope in (2, 3) else None
            sel = [e for e in self.src.entries
                   if (pak is None or e.pak == pak)
                   and (ext is None or e.ext == ext)]
        self.selected = sel
        warn = "　⚠ 数量巨大，建议先缩小范围" if len(sel) > 20000 else ""
        self.lbl_count.setText(f"将导出 {len(sel):,} 个文件{warn}")


class PluginManagerDialog(QDialog):
    def __init__(self, parent, config_path: str | None):
        super().__init__(parent)
        self.setWindowTitle("插件管理")
        self.config_path = config_path
        lay = QVBoxLayout(self)
        lay.addWidget(QLabel("取消勾选 = 禁用（该格式降级为 hex 预览）；"
                             "兜底插件不可禁用。"))
        self.boxes = []
        for p in plugins.all_plugins():
            exts = ", ".join(p.exts) if p.exts else "（未注册格式的兜底）"
            cb = QCheckBox(f"{p.name}　[{exts}]")
            cb.setChecked(plugins.is_enabled(p))
            if not p.exts:
                cb.setEnabled(False)
            cb.stateChanged.connect(
                lambda _s, pl=p, c=cb: self._toggle(pl, c))
            lay.addWidget(cb)
            self.boxes.append(cb)
        bb = QDialogButtonBox(QDialogButtonBox.Close)
        bb.rejected.connect(self.reject)
        bb.clicked.connect(lambda *_: self.accept())
        lay.addWidget(bb)

    def _toggle(self, plugin, cb: QCheckBox):
        plugins.set_enabled(plugin, cb.isChecked())
        if self.config_path:
            plugins.save_config(self.config_path)


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
        self.ed_search = QLineEdit(placeholderText="路径包含（250ms 防抖）")
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
        self.page_scene = ScenePage()
        self.stack = QStackedWidget()
        for w in (self.page_image, self.page_text, self.page_hex,
                  self.page_scene):
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

        act = QAction("导出当前资源", self)
        act.triggered.connect(self._export)
        self.addAction(act)
        act_batch = QAction("批量导出", self)
        act_batch.triggered.connect(self._batch_export)
        act_plug = QAction("插件管理", self)
        act_plug.triggered.connect(self._plugin_manager)
        self.toolbar = self.addToolBar("主")
        self.toolbar.addAction(act)
        self.toolbar.addAction(act_batch)
        self.toolbar.addAction(act_plug)
        self._batch_job = None

        # 事件
        self._debounce = QTimer(self)
        self._debounce.setSingleShot(True)
        self._debounce.setInterval(250)
        self._debounce.timeout.connect(self._apply_filter)
        self.ed_search.textChanged.connect(self._debounce.start)
        self.cmb_pak.currentIndexChanged.connect(self._apply_filter)
        self.cmb_ext.currentIndexChanged.connect(self._apply_filter)
        self.table.selectionModel().selectionChanged.connect(self._on_select)

        # 插件启用/禁用配置
        cfg = getattr(self.src, "config_path", None)
        if cfg:
            plugins.load_config(cfg)

        self._apply_filter()
        self.statusBar().showMessage(
            f"就绪：{len(self.src.entries):,} 个条目，点击左侧任意条目预览")

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
        elif kind == "scene":
            self.page_scene.set_scene(res["scene"])
            self.stack.setCurrentWidget(self.page_scene)
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
        stem = os.path.splitext(entry.name)[0] or "out"
        if entry.ext == ".map" and self.page_scene.scene is not None:
            # 直接用当前显示的场景（含图层开关/动画当前帧），不重复构建
            path, _ = QFileDialog.getSaveFileName(self, "导出场景 PNG", stem + ".png")
            if path:
                self.page_scene.composite_current().save(path)
            return
        payload, _ = self.src.decode_entry(entry)
        plugin = plugins.get_plugin(entry.ext)
        try:
            res = plugin.load(payload, payload, entry, self.src)
        except Exception as ex:  # noqa: BLE001
            QMessageBox.warning(self, "导出失败", str(ex))
            return
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

    # ---- 插件管理 / 批量导出 ----
    def _plugin_manager(self):
        dlg = PluginManagerDialog(self, getattr(self.src, "config_path", None))
        dlg.exec() if hasattr(dlg, "exec") else dlg.exec_()
        enabled = sum(1 for p in plugins.all_plugins() if plugins.is_enabled(p))
        self.statusBar().showMessage(
            f"插件：{enabled}/{len(plugins.all_plugins())} 启用")

    def _filtered_entries(self) -> list[Entry]:
        return [self.model.entries[i] for i in self.model.idx]

    def _batch_export(self):
        dlg = BatchExportDialog(self, self.src, self._filtered_entries(),
                                self._pak_counts, self._ext_counts)
        rc = dlg.exec() if hasattr(dlg, "exec") else dlg.exec_()
        if not rc or not dlg.selected:
            return
        out_dir = dlg.ed_dir.text().strip()
        if not out_dir:
            QMessageBox.warning(self, "批量导出", "请选择输出目录")
            return
        n = len(dlg.selected)
        if n > 20000:
            rc = QMessageBox.question(
                self, "批量导出",
                f"即将导出 {n:,} 个文件，可能耗时较长。继续？")
            if rc != QMessageBox.Yes:
                return
        prog = QProgressDialog("批量导出中…", "取消", 0, n, self)
        prog.setWindowModality(Qt.WindowModal)
        prog.setMinimumDuration(0)
        job = BatchExportJob(self.src, dlg.selected, out_dir)
        self._batch_job = job
        job.signals.progress.connect(
            lambda i, total, key: (prog.setValue(i),
                                   prog.setLabelText(f"{i:,}/{total:,}  {key}")))
        prog.canceled.connect(job.cancel)
        job.signals.finished.connect(lambda r: self._batch_done(prog, r))
        self.pool.start(job)

    def _batch_done(self, prog: QProgressDialog, r: dict):
        prog.close()
        self._batch_job = None
        msg = (f"完成 {r['ok']:,} / {r['total']:,}，失败 {r['fail']:,}，"
               f"耗时 {r['elapsed']:.1f}s")
        if r["cancelled"]:
            msg = "（已取消）" + msg
        if r["errors"]:
            msg += "\n\n前若干错误：\n" + "\n".join(r["errors"][:10])
        QMessageBox.information(self, "批量导出", msg)
        self.statusBar().showMessage(msg.splitlines()[0])

    def closeEvent(self, event):
        if self._batch_job is not None:
            self._batch_job.cancel()
        self.src.close()
        super().closeEvent(event)


def run(src: PakSource):
    app = QApplication.instance() or QApplication(sys.argv)
    win = MainWindow(src)
    win.show()
    print("启动完成：主窗口已打开，可搜索/点击预览。", flush=True)
    return app_exec(app)
