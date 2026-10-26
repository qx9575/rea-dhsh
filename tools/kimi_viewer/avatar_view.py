"""纸娃娃对话框：家族/部位模型/动作/方向选择 + 动画播放。

装配逻辑在 avatar.py（APCI 层叠键 + perinfo 锚点延时），
预览复用 ImagePage（播放、缩放、右键拖动）。
"""
from __future__ import annotations

from .qtcompat import (QComboBox, QDialog, QFileDialog, QGridLayout,
                       QHBoxLayout, QLabel, QMessageBox, QPushButton,
                       QVBoxLayout, QApplication, Qt, QSpinBox, QCheckBox)
from .avatar import (PARTS, AvatarSource, load_skills, MagicMixer,
                     magic_variants, composite_with_magic, NpcSource,
                     load_monster_names, load_viewjob,
                     CAREER_NAMES, ADVANCE_NAMES, MOUNT_NAMES)
from .app import ImagePage

DEFAULT_MODELS = {"body": "200", "head": "105"}

class AvatarDialog(QDialog):
    def __init__(self, parent, src):
        super().__init__(parent)
        self.setWindowTitle("纸娃娃")
        self.resize(900, 700)
        self.av = AvatarSource(src)
        self.src = src
        self._loading = False

        lay = QVBoxLayout(self)

        # 视图类别：人物 / NPC / 坐骑 / 装备 / 宠物 / 召唤兽
        cat_row = QHBoxLayout()
        self.cmb_category = QComboBox()
        self.cmb_category.addItems(["人物", "NPC", "坐骑", "装备", "宠物", "召唤兽"])
        cat_row.addWidget(QLabel("类别"))
        cat_row.addWidget(self.cmb_category)
        cat_row.addStretch(1)
        lay.addLayout(cat_row)

        # 职业预设（仅人物类别显示）
        career_row = QHBoxLayout()
        self.cmb_career = QComboBox()
        self.cmb_career.addItems(CAREER_NAMES)
        self.cmb_advance = QComboBox()
        self.cmb_advance.addItems(ADVANCE_NAMES)
        self.cmb_mount = QComboBox()
        self.cmb_mount.addItems(MOUNT_NAMES)
        self.cmb_gender = QComboBox()
        self.cmb_gender.addItems(["男", "女"])
        self.btn_career = QPushButton("应用职业")
        self.chk_career_skill = QCheckBox("仅本职业技能")
        self._viewjob = load_viewjob(src)
        for label, combo in (("职业", self.cmb_career), ("进阶", self.cmb_advance),
                             ("坐骑档", self.cmb_mount), ("性别", self.cmb_gender)):
            career_row.addWidget(QLabel(label))
            career_row.addWidget(combo)
        career_row.addWidget(self.btn_career)
        career_row.addWidget(self.chk_career_skill)
        career_row.addStretch(1)
        lay.addLayout(career_row)
        self._career_row = career_row

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

        # 技能特效行：技能名（Skill.csv）+ 光效类型 + 变体
        skill_row = QHBoxLayout()
        self.cmb_skill = QComboBox()
        self.cmb_effect = QComboBox()
        self.cmb_effect.addItems(["攻击", "受击", "失败"])
        self.cmb_variant = QComboBox()
        self.magic = MagicMixer(src)
        self._skills = load_skills(src)
        self.cmb_skill.addItem("无技能特效")
        for sk in self._skills:
            self.cmb_skill.addItem(f"{sk.name}({sk.id})", sk.id)
        self.cmb_skill.setMinimumWidth(200)
        for label, combo in (("技能", self.cmb_skill), ("光效", self.cmb_effect),
                             ("变体", self.cmb_variant)):
            skill_row.addWidget(QLabel(label))
            skill_row.addWidget(combo)
        skill_row.addStretch(1)
        lay.addLayout(skill_row)
        self._skill_row = skill_row

        # 染色行：CTI hue/sat/val 变体（AI 索引帧生效）
        dye_row = QHBoxLayout()
        self.sp_hue = QSpinBox(minimum=0, maximum=251, value=0)
        self.sp_sat = QSpinBox(minimum=0, maximum=200, value=100)
        self.sp_val = QSpinBox(minimum=0, maximum=200, value=100)
        for label, sp in (("染色 色相", self.sp_hue), ("饱和%", self.sp_sat),
                          ("明度%", self.sp_val)):
            dye_row.addWidget(QLabel(label))
            dye_row.addWidget(sp)
        self.btn_dye_reset = QPushButton("重置染色")
        dye_row.addWidget(self.btn_dye_reset)
        dye_row.addStretch(1)
        lay.addLayout(dye_row)
        self._dye_row = dye_row

        # NPC 行：单模型展示（Monsters.csv 提供名字）
        npc_row = QHBoxLayout()
        self.npc = NpcSource(src)
        self._monster_names = load_monster_names(src)
        self._npc = None   # (pak, model_id)；None = 纸娃娃模式
        self.cmb_npc_family = QComboBox()
        self.cmb_npc_family.addItems(self.npc.families())
        self.cmb_npc_model = QComboBox()
        self.cmb_npc_model.setMinimumWidth(200)
        self.btn_npc = QPushButton("显示 NPC")
        for label, combo in (("NPC 家族", self.cmb_npc_family),
                             ("模型", self.cmb_npc_model)):
            npc_row.addWidget(QLabel(label))
            npc_row.addWidget(combo)
        npc_row.addWidget(self.btn_npc)
        npc_row.addStretch(1)
        lay.addLayout(npc_row)
        self._npc_row = npc_row

        self._part_combos = {}
        self._part_rows = {}
        self._solo = None   # 单件查看的部位；None = 整机
        self._equip_part = "body"
        grid = QGridLayout()
        for i, (part, label) in enumerate(PARTS):
            lbl = QLabel(label)
            grid.addWidget(lbl, i // 4, (i % 4) * 3)
            combo = QComboBox()
            combo.setMinimumWidth(110)
            grid.addWidget(combo, i // 4, (i % 4) * 3 + 1)
            self._part_combos[part] = combo
            btn = QPushButton("单看")
            btn.setMaximumWidth(44)
            btn.clicked.connect(lambda _=False, p=part: self._solo_view(p))
            grid.addWidget(btn, i // 4, (i % 4) * 3 + 2)
            self._part_rows[part] = (lbl, combo, btn)
        lay.addLayout(grid)
        self._part_grid = grid

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
            combo.currentTextChanged.connect(
                lambda _t, p=part: self._on_part_changed(p))
        self.btn_export.clicked.connect(self._export)
        self.btn_whole.clicked.connect(self._whole_view)
        self.cmb_skill.currentIndexChanged.connect(self._fill_variants)
        self.cmb_effect.currentIndexChanged.connect(self._fill_variants)
        self.cmb_variant.currentTextChanged.connect(self._rebuild)
        self.sp_hue.valueChanged.connect(self._rebuild)
        self.sp_sat.valueChanged.connect(self._rebuild)
        self.sp_val.valueChanged.connect(self._rebuild)
        self.btn_dye_reset.clicked.connect(self._dye_reset)
        self.cmb_npc_family.currentTextChanged.connect(self._fill_npc_models)
        self.btn_npc.clicked.connect(self._show_npc)

        self.cmb_category.currentTextChanged.connect(self._apply_category)
        self.btn_career.clicked.connect(self._apply_career)
        self.chk_career_skill.stateChanged.connect(self._fill_skills)
        self.cmb_career.currentIndexChanged.connect(self._fill_skills)

        self._fill_parts(self.cmb_family.currentText())
        self._fill_npc_models(self.cmb_npc_family.currentText())
        self._apply_category(self.cmb_category.currentText())

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

    def _on_part_changed(self, part: str):
        if self._loading:
            return
        if self.cmb_category.currentText() == "装备":
            self._equip_part = part
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


    # ---- 类别浏览 / 职业预设 ----
    def _set_row_visible(self, layout, visible: bool):
        for i in range(layout.count()):
            item = layout.itemAt(i)
            if item.widget() is not None:
                item.widget().setVisible(visible)

    def _apply_category(self, cat: str):
        is_npc = cat in ("NPC", "召唤兽")
        self._set_row_visible(self._npc_row, is_npc)
        self._set_row_visible(self._career_row, cat == "人物")
        self._set_row_visible(self._skill_row, not is_npc)
        self._set_row_visible(self._dye_row, not is_npc)
        for part, widgets in self._part_rows.items():
            if is_npc:
                visible = False
            elif cat == "人物":
                visible = True
            elif cat == "坐骑":
                visible = part in ("horse", "harmorbody", "harmorhead")
            elif cat == "宠物":
                visible = part == "pet"
            else:  # 装备
                visible = part not in ("horse", "harmorbody", "harmorhead", "pet")
            for w in widgets:
                w.setVisible(visible)
        if is_npc:
            self._fill_npc_models(self.cmb_npc_family.currentText())
            self._show_npc()
        else:
            self._npc = None
            self._rebuild()

    def _apply_career(self):
        key = (self.cmb_career.currentIndex(), self.cmb_advance.currentIndex(),
               self.cmb_mount.currentIndex(), self.cmb_gender.currentIndex())
        preset = self._viewjob.get(key)
        if preset is None:
            self.lbl_status.setText(f"该组合没有职业预设（{key}）")
            return
        for part, combo in self._part_combos.items():
            if part in preset:
                model = preset[part]
                combo.setCurrentText("无" if model < 0 else str(model))
        self._fill_skills()
        self._rebuild()

    def _fill_skills(self):
        career = self.cmb_career.currentIndex()
        only = self.chk_career_skill.isChecked()
        current = self.cmb_skill.currentData()
        self.cmb_skill.blockSignals(True)
        self.cmb_skill.clear()
        self.cmb_skill.addItem("无技能特效", 0)
        for sk in self._skills:
            if only and sk.career not in (0, career):
                continue
            self.cmb_skill.addItem(f"{sk.name}({sk.id})", sk.id)
        if current:
            idx = self.cmb_skill.findData(current)
            if idx >= 0:
                self.cmb_skill.setCurrentIndex(idx)
        self.cmb_skill.blockSignals(False)
        self._rebuild()

    def _whole_view(self):
        self._solo = None
        self._npc = None
        self._rebuild()

    def _fill_npc_models(self, pak: str):
        self.cmb_npc_model.clear()
        summon_only = self.cmb_category.currentText() == "召唤兽"
        for model in self.npc.models(pak):
            mid = int(model)
            name = self._monster_names.get(mid)
            if summon_only and not name:
                continue  # 召唤兽只列 Monsters.csv 里有名字的战斗模型
            label = f"{name}({model})" if name else model
            self.cmb_npc_model.addItem(label, mid)

    def _show_npc(self):
        if self.cmb_npc_model.currentIndex() < 0:
            return
        pak = self.cmb_npc_family.currentText()
        model = self.cmb_npc_model.currentData()
        self._npc = (pak, model)
        # NPC 的动作集与纸娃娃不同，重填动作下拉
        self.cmb_action.blockSignals(True)
        self.cmb_action.clear()
        self.cmb_action.addItems(self.npc.actions(pak, "", str(model)))
        self.cmb_action.blockSignals(False)
        self._rebuild()

    def _skill_model(self) -> int:
        idx = self.cmb_skill.currentIndex()
        if idx <= 0:
            return 0
        sk = self._skills[idx - 1]
        return (sk.attack, sk.hit, sk.fail)[self.cmb_effect.currentIndex()]

    def _fill_variants(self):
        model = self._skill_model()
        self.cmb_variant.blockSignals(True)
        self.cmb_variant.clear()
        if model:
            self.cmb_variant.addItems(magic_variants(self.src, model))
        self.cmb_variant.blockSignals(False)
        self._rebuild()

    def _dye_reset(self):
        self.sp_hue.setValue(0)
        self.sp_sat.setValue(100)
        self.sp_val.setValue(100)

    def _dye(self):
        h, s, v = self.sp_hue.value(), self.sp_sat.value(), self.sp_val.value()
        return None if (h == 0 and s == 100 and v == 100) else (h, s, v)

    # ---- 装配 ----
    def _rebuild(self):
        if self._loading:
            return
        pak = self.cmb_family.currentText()
        action = self.cmb_action.currentText()
        if not pak:
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
        cat = self.cmb_category.currentText()
        if cat == "坐骑":
            parts = {k: v for k, v in parts.items()
                     if k in ("horse", "harmorbody", "harmorhead")}
        elif cat == "宠物":
            parts = {k: v for k, v in parts.items() if k == "pet"}
        elif cat == "装备":
            parts = {self._equip_part: parts[self._equip_part]} \
                if self._equip_part in parts else {}
        model = self._skill_model()
        if not parts and not model and self._npc is None:
            self.lbl_status.setText("请至少选择一个部件")
            self.page.set_frames([])
            return
        if not action and (parts or self._npc is not None):
            return  # 动作列表刷新中，等下一次触发
        QApplication.setOverrideCursor(Qt.WaitCursor)
        try:
            def assemble_dir(d: int):
                if self._npc is not None:
                    npak, nmodel = self._npc
                    return self.npc.assemble_npc(npak, nmodel, int(action), d)
                if parts:
                    return self.av.assemble(pak, parts, int(action), d, self._dye())
                return [], 80

            frames, delay = assemble_dir(int(self.cmb_dir.currentText()))
            if not frames:
                # 部分模型只有 3/7 等少数方向，自动回退到有帧的方向
                for d in (3, 7, 0, 1, 2, 4, 5, 6):
                    if d == int(self.cmb_dir.currentText()):
                        continue
                    frames, delay = assemble_dir(d)
                    if frames:
                        self.cmb_dir.blockSignals(True)
                        self.cmb_dir.setCurrentText(str(d))
                        self.cmb_dir.blockSignals(False)
                        break
            if model:
                variant = self.cmb_variant.currentText() or "0"
                back = self.magic.anim(model, variant, "back")
                front = self.magic.anim(model, variant, "front")
                if back or front:
                    frames = composite_with_magic(frames, back, front)
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
        if self._npc is not None:
            name = self._monster_names.get(self._npc[1], "")
            solo = f"（NPC {name or self._npc[1]}，点「整机」恢复）"
        self.lbl_status.setText(f"{len(frames)} 帧，{delay}ms/帧{solo}")

    def _export(self):
        if not self.page.frames:
            return
        idx = self.page.cur
        path, _ = QFileDialog.getSaveFileName(self, "导出当前帧", f"avatar_f{idx:03d}.png")
        if path:
            self.page.frames[idx].save(path)
