"""纸娃娃对话框：家族/部位模型/动作/方向选择 + 动画播放。

装配逻辑在 avatar.py（APCI 层叠键 + perinfo 锚点延时），
预览复用 ImagePage（播放、缩放、右键拖动）。
"""
from __future__ import annotations

from .qtcompat import (QComboBox, QDialog, QFileDialog, QGridLayout,
                       QHBoxLayout, QLabel, QMessageBox, QPushButton,
                       QVBoxLayout, QApplication, Qt)
from .avatar import PARTS, AvatarSource
from .app import ImagePage

DEFAULT_MODELS = {"body": "200", "head": "105"}

class AvatarDialog(QDialog):
    def __init__(self, parent, src):
        super().__init__(parent)
        self.setWindowTitle("纸娃娃")
        self.resize(900, 700)
        self.av = AvatarSource(src)
        self._loading = False

        lay = QVBoxLayout(self)
        top = QHBoxLayout()
        self.cmb_family = QComboBox()
        self.cmb_family.addItems(self.av.families())
        self.cmb_action = QComboBox()
        self.cmb_dir = QComboBox()
        self.cmb_dir.addItems([str(i) for i in range(8)])
        for label, combo in (("家族", self.cmb_family), ("动作", self.cmb_action),
                             ("方向", self.cmb_dir)):
            top.addWidget(QLabel(label))
            top.addWidget(combo)
        top.addStretch(1)
        lay.addLayout(top)

        self._part_combos = {}
        self._solo = None   # 单件查看的部位；None = 整机
        grid = QGridLayout()
        for i, (part, label) in enumerate(PARTS):
            grid.addWidget(QLabel(label), i // 4, (i % 4) * 3)
            combo = QComboBox()
            combo.setMinimumWidth(110)
            grid.addWidget(combo, i // 4, (i % 4) * 3 + 1)
            self._part_combos[part] = combo
            btn = QPushButton("单看")
            btn.setMaximumWidth(44)
            btn.clicked.connect(lambda _=False, p=part: self._solo_view(p))
            grid.addWidget(btn, i // 4, (i % 4) * 3 + 2)
        lay.addLayout(grid)

        self.page = ImagePage()
        lay.addWidget(self.page, 1)

        bottom = QHBoxLayout()
        self.btn_export = QPushButton("导出当前帧")
        self.btn_whole = QPushButton("整机")
        self.lbl_status = QLabel("")
        bottom.addWidget(self.btn_export)
        bottom.addWidget(self.btn_whole)
        bottom.addWidget(self.lbl_status, 1)
        lay.addLayout(bottom)

        self.cmb_family.currentTextChanged.connect(self._fill_parts)
        self.cmb_action.currentTextChanged.connect(self._rebuild)
        self.cmb_dir.currentTextChanged.connect(self._rebuild)
        for part, combo in self._part_combos.items():
            combo.currentTextChanged.connect(self._on_part_changed)
        self.btn_export.clicked.connect(self._export)
        self.btn_whole.clicked.connect(self._whole_view)

        self._fill_parts(self.cmb_family.currentText())

    # ---- 选择器填充 ----
    def _fill_parts(self, pak: str):
        self._loading = True
        for part, combo in self._part_combos.items():
            combo.blockSignals(True)
            combo.clear()
            models = self.av.models(pak, part)
            combo.addItem("无")
            combo.addItems(models)
            want = DEFAULT_MODELS.get(part)
            if want and want in models:
                combo.setCurrentText(want)
            combo.blockSignals(False)
        self._loading = False
        self._update_mount_state()
        self._fill_actions()

    def _fill_actions(self):
        pak = self.cmb_family.currentText()
        actions: set[int] = set()
        for part, combo in self._part_combos.items():
            model = combo.currentText()
            if model and model != "无" and part == "body":
                actions.update(int(a) for a in self.av.actions(pak, part, model)
                               if a.isdigit())
        if not actions:
            for part, combo in self._part_combos.items():
                model = combo.currentText()
                if model and model != "无":
                    actions.update(int(a) for a in self.av.actions(pak, part, model)
                                   if a.isdigit())
                    break
        self.cmb_action.blockSignals(True)
        self.cmb_action.clear()
        self.cmb_action.addItems([str(a) for a in sorted(actions)])
        self.cmb_action.blockSignals(False)
        self._rebuild()

    def _on_part_changed(self):
        if self._loading:
            return
        self._update_mount_state()
        # 身体变化会影响可选动作集
        self._fill_actions()

    def _update_mount_state(self):
        """坐骑隐藏时，坐骑甲（身/头）一并禁用且不参与装配。"""
        mounted = self._part_combos["horse"].currentText() != "无"
        for part in ("harmorbody", "harmorhead"):
            self._part_combos[part].setEnabled(mounted)

    def _solo_view(self, part: str):
        if self._part_combos[part].currentText() == "无":
            self.lbl_status.setText("该部位未选模型，先选一个再单看")
            return
        self._solo = part if self._solo != part else None
        self._rebuild()

    def _whole_view(self):
        self._solo = None
        self._rebuild()

    # ---- 装配 ----
    def _rebuild(self):
        if self._loading:
            return
        pak = self.cmb_family.currentText()
        action = self.cmb_action.currentText()
        if not pak or not action:
            return
        parts = {}
        for part, combo in self._part_combos.items():
            text = combo.currentText()
            if text and text != "无":
                parts[part] = int(text)
        if parts.get("horse") is None:
            # 无坐骑时坐骑甲不可见（与游戏一致）
            parts.pop("harmorbody", None)
            parts.pop("harmorhead", None)
        if self._solo:
            parts = {self._solo: parts[self._solo]} if self._solo in parts else {}
        if not parts:
            self.lbl_status.setText("请至少选择一个部件")
            return
        QApplication.setOverrideCursor(Qt.WaitCursor)
        try:
            frames, delay = self.av.assemble(
                pak, parts, int(action), int(self.cmb_dir.currentText()))
        except Exception as ex:  # noqa: BLE001
            self.lbl_status.setText(f"装配失败: {type(ex).__name__}: {ex}")
            return
        finally:
            QApplication.restoreOverrideCursor()
        if not frames:
            self.lbl_status.setText("该组合没有可用帧（换动作/方向试试）")
            self.page.set_frames([])
            return
        self.page.set_frames(frames)
        self.page.fps.setValue(max(1, round(1000 / max(1, delay))))
        solo = f"（单看 {self._solo}，点「整机」恢复）" if self._solo else ""
        self.lbl_status.setText(f"{len(frames)} 帧，{delay}ms/帧{solo}")

    def _export(self):
        if not self.page.frames:
            return
        idx = self.page.cur
        path, _ = QFileDialog.getSaveFileName(self, "导出当前帧", f"avatar_f{idx:03d}.png")
        if path:
            self.page.frames[idx].save(path)
