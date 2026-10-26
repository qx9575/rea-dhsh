"""纸娃娃/展示对话框：类别浏览 + 职业预设 + 技能特效 + 染色 + 资源明细。

设计原则（用户反馈）：展示优先——除「人物」外不使用家族/pak 选择器，
统一模型下拉（有名字显示名称(ID)、无名字显示待确认(ID)）；装备独立直查；
任何类别/选项切换都清空旧帧并立即刷新；资源明细可复制。

装配逻辑在 avatar.py（APCI 层叠键 + perinfo 锚点延时），
预览复用 ImagePage（播放、缩放、右键拖动）。
"""
from __future__ import annotations

from .qtcompat import (QComboBox, QDialog, QFileDialog, QGridLayout,
                       QHBoxLayout, QLabel, QPushButton, QPlainTextEdit,
                       QVBoxLayout, QApplication, Qt, QSpinBox, QCheckBox,
                       QLineEdit)
from .avatar import (PARTS, AvatarSource, load_skills, MagicMixer,
                     magic_variants, composite_with_magic, NpcSource,
                     load_viewjob, all_part_models,
                     all_npc_models, CAREER_NAMES, ADVANCE_NAMES, MOUNT_NAMES,
                     SLOT_NAMES, load_model_names, format_model_name)
from .app import ImagePage

DEFAULT_MODELS = {"body": "200", "head": "105"}
EQUIP_PARTS = ("weapon", "cloth", "shoe", "mask", "Lcuff", "Rcuff")
PART_LABEL = dict(PARTS)


class AvatarDialog(QDialog):
    def __init__(self, parent, src):
        super().__init__(parent)
        self.setWindowTitle("纸娃娃")
        self.resize(960, 760)
        self.src = src
        self.av = AvatarSource(src)
        self.npc = NpcSource(src)
        self.magic = MagicMixer(src)
        self._skills = load_skills(src)
        self._model_names = load_model_names(src)
        self._viewjob = load_viewjob(src)
        self._items = self._model_names.items
        self._item_by_id = {i.id: i for i in self._items}
        self._loading = False
        self._solo = None
        self._equip_part = "body"
        self._npc = None      # (pak, model_id)

        lay = QVBoxLayout(self)

        # 类别 + 动作 + 方向（全类别共享）
        top = QHBoxLayout()
        self.cmb_category = QComboBox()
        self.cmb_category.addItems(["人物", "NPC", "坐骑", "装备", "宠物", "召唤兽"])
        self.cmb_action = QComboBox()
        self.cmb_dir = QComboBox()
        self.cmb_dir.addItems([str(i) for i in range(8)])
        for label, combo in (("类别", self.cmb_category), ("动作", self.cmb_action),
                             ("方向", self.cmb_dir)):
            top.addWidget(QLabel(label))
            top.addWidget(combo)
        top.addStretch(1)
        lay.addLayout(top)


        self.row_career = QHBoxLayout()
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
        for label, combo in (("职业", self.cmb_career), ("进阶", self.cmb_advance),
                             ("坐骑档", self.cmb_mount), ("性别", self.cmb_gender)):
            self.row_career.addWidget(QLabel(label))
            self.row_career.addWidget(combo)
        self.row_career.addWidget(self.btn_career)
        self.row_career.addWidget(self.chk_career_skill)
        self.row_career.addStretch(1)
        lay.addLayout(self.row_career)

        # 人物：部件网格（单看按钮）
        self._part_combos = {}
        self._part_rows = {}
        grid = QGridLayout()
        for i, (part, label) in enumerate(PARTS):
            lbl = QLabel(label)
            grid.addWidget(lbl, i // 4, (i % 4) * 2)
            combo = QComboBox()
            combo.setMinimumWidth(110)
            grid.addWidget(combo, i // 4, (i % 4) * 2 + 1)
            self._part_combos[part] = combo
            self._part_rows[part] = (lbl, combo)
        lay.addLayout(grid)
        self._part_grid = grid

        # NPC/召唤兽：统一模型下拉（无 pak 选择）
        self.row_npc = QHBoxLayout()
        self.cmb_npc_model = QComboBox()
        self.cmb_npc_model.setMinimumWidth(220)
        self.row_npc.addWidget(QLabel("NPC 模型"))
        self.row_npc.addWidget(self.cmb_npc_model)
        self.row_npc.addStretch(1)
        lay.addLayout(self.row_npc)

        # 坐骑：统一坐骑模型 + 骑甲（无 pak 选择）
        self.row_mount = QHBoxLayout()
        self.cmb_horse = QComboBox()
        self.cmb_horse.setMinimumWidth(140)
        self.cmb_hhead = QComboBox()
        self.cmb_hbody = QComboBox()
        for label, combo in (("坐骑", self.cmb_horse), ("骑甲头", self.cmb_hhead),
                             ("骑甲身", self.cmb_hbody)):
            self.row_mount.addWidget(QLabel(label))
            self.row_mount.addWidget(combo)
        self.row_mount.addStretch(1)
        lay.addLayout(self.row_mount)

        # 装备：物品库浏览（类型/职业/名称搜索，图标+详情）
        self.row_equip = QHBoxLayout()
        self.cmb_equip_type = QComboBox()
        self.cmb_equip_type.addItem("全部类型", 0)
        for slot, name in SLOT_NAMES.items():
            self.cmb_equip_type.addItem(name, slot)
        self.cmb_equip_model = QComboBox()
        self.cmb_equip_model.setMinimumWidth(220)
        self.ed_equip_search = QLineEdit(placeholderText="名称搜索")
        self.cmb_equip_career = QComboBox()
        self.cmb_equip_career.addItems(["全部职业"] + list(CAREER_NAMES))
        for label, combo in (("类型", self.cmb_equip_type), ("物品", self.cmb_equip_model),
                             ("职业过滤", self.cmb_equip_career)):
            self.row_equip.addWidget(QLabel(label))
            self.row_equip.addWidget(combo)
        self.row_equip.addWidget(self.ed_equip_search)
        self.row_equip.addStretch(1)
        lay.addLayout(self.row_equip)

        # 宠物：统一宠物模型
        self.row_pet = QHBoxLayout()
        self.cmb_pet = QComboBox()
        self.cmb_pet.setMinimumWidth(140)
        self.row_pet.addWidget(QLabel("宠物模型"))
        self.row_pet.addWidget(self.cmb_pet)
        self.row_pet.addStretch(1)
        lay.addLayout(self.row_pet)

        # 技能特效 + 染色（人物/坐骑/宠物显示）
        self.row_skill = QHBoxLayout()
        self.cmb_skill = QComboBox()
        self.cmb_effect = QComboBox()
        self.cmb_effect.addItems(["攻击", "受击", "失败"])
        self.cmb_variant = QComboBox()
        self.cmb_skill.setMinimumWidth(180)
        for label, combo in (("技能", self.cmb_skill), ("光效", self.cmb_effect),
                             ("变体", self.cmb_variant)):
            self.row_skill.addWidget(QLabel(label))
            self.row_skill.addWidget(combo)
        self.row_skill.addStretch(1)
        lay.addLayout(self.row_skill)

        self.row_dye = QHBoxLayout()
        self.sp_hue = QSpinBox(minimum=0, maximum=251, value=0)
        self.sp_sat = QSpinBox(minimum=0, maximum=200, value=100)
        self.sp_val = QSpinBox(minimum=0, maximum=200, value=100)
        for label, sp in (("染色 色相", self.sp_hue), ("饱和%", self.sp_sat),
                          ("明度%", self.sp_val)):
            self.row_dye.addWidget(QLabel(label))
            self.row_dye.addWidget(sp)
        self.btn_dye_reset = QPushButton("重置染色")
        self.row_dye.addWidget(self.btn_dye_reset)
        self.row_dye.addStretch(1)
        lay.addLayout(self.row_dye)

        self.page = ImagePage()
        lay.addWidget(self.page, 1)

        # 资源明细（可复制）
        bottom = QHBoxLayout()
        self.btn_export = QPushButton("导出当前帧")
        self.btn_whole = QPushButton("整机")
        self.btn_prov = QPushButton("资源明细")
        self.btn_copy = QPushButton("复制")
        self.lbl_status = QLabel("")
        for w in (self.btn_export, self.btn_whole, self.btn_prov, self.btn_copy):
            bottom.addWidget(w)
        bottom.addWidget(self.lbl_status, 1)
        lay.addLayout(bottom)
        self.prov = QPlainTextEdit()
        self.prov.setReadOnly(True)
        self.prov.setMaximumBlockCount(500)
        self.prov.setVisible(False)
        lay.addWidget(self.prov)

        # 信号
        self.cmb_category.currentTextChanged.connect(self._apply_category)
        self.cmb_action.currentTextChanged.connect(self._rebuild)
        self.cmb_dir.currentTextChanged.connect(self._rebuild)
        self.cmb_career.currentIndexChanged.connect(self._career_linked)
        self.cmb_advance.currentIndexChanged.connect(self._career_linked)
        self.cmb_mount.currentIndexChanged.connect(self._career_linked)
        self.cmb_gender.currentIndexChanged.connect(self._career_linked)
        self.btn_career.clicked.connect(self._apply_career)
        self.chk_career_skill.stateChanged.connect(self._fill_skills)
        for part, combo in self._part_combos.items():
            combo.currentTextChanged.connect(
                lambda _t, p=part: self._on_part_changed(p))
        self.cmb_skill.currentIndexChanged.connect(self._fill_variants)
        self.cmb_effect.currentIndexChanged.connect(self._fill_variants)
        self.cmb_variant.currentTextChanged.connect(self._rebuild)
        self.sp_hue.valueChanged.connect(self._rebuild)
        self.sp_sat.valueChanged.connect(self._rebuild)
        self.sp_val.valueChanged.connect(self._rebuild)
        self.btn_dye_reset.clicked.connect(self._dye_reset)
        self.cmb_npc_model.currentIndexChanged.connect(self._npc_changed)
        self.cmb_horse.currentIndexChanged.connect(self._mount_changed)
        self.cmb_hhead.currentIndexChanged.connect(self._mount_changed)
        self.cmb_hbody.currentIndexChanged.connect(self._mount_changed)
        self.cmb_equip_type.currentIndexChanged.connect(self._fill_equip_models)
        self.cmb_equip_model.currentIndexChanged.connect(self._equip_changed)
        self.cmb_equip_career.currentIndexChanged.connect(self._fill_equip_models)
        self.ed_equip_search.textChanged.connect(self._fill_equip_models)
        self.cmb_pet.currentIndexChanged.connect(self._pet_changed)
        self.btn_export.clicked.connect(self._export)
        self.btn_whole.clicked.connect(self._whole_view)
        self.btn_prov.clicked.connect(self._toggle_prov)
        self.btn_copy.clicked.connect(self._copy_prov)

        self._fill_parts()
        self._apply_category(self.cmb_category.currentText())

    # ---------------------------------------------------------------- 工具
    def _set_row_visible(self, layout, visible: bool):
        for i in range(layout.count()):
            item = layout.itemAt(i)
            if item.widget() is not None:
                item.widget().setVisible(visible)

    def _label_for(self, part: str, model: int) -> str:
        return format_model_name(self._model_names.query(part, model))

    def _actions_for(self, pak: str, part: str, model: int) -> list[str]:
        if part:
            return self.av.actions(pak, part, str(model))
        return self.npc.actions(pak, "", str(model))

    def _set_actions(self, pak: str, part: str, model: int):
        self.cmb_action.blockSignals(True)
        self.cmb_action.clear()
        self.cmb_action.addItems(self._actions_for(pak, part, model))
        self.cmb_action.blockSignals(False)

    # ---------------------------------------------------------------- 类别
    def _apply_category(self, cat: str):
        is_npc = cat in ("NPC", "召唤兽")
        for row, visible in (
                (self.row_career, cat == "人物"),
                (self._part_grid, cat == "人物"),
                (self.row_npc, is_npc),
                (self.row_mount, cat == "坐骑"),
                (self.row_equip, cat == "装备"),
                (self.row_pet, cat == "宠物"),
                (self.row_skill, cat in ("人物", "坐骑", "宠物")),
                (self.row_dye, cat in ("人物", "坐骑", "宠物"))):
            self._set_row_visible(row, visible)
        self._npc = None
        self._solo = None
        self.page.set_frames([])
        if is_npc:
            self._fill_npc_models()
        elif cat == "坐骑":
            self._fill_mount_models()
        elif cat == "装备":
            self._fill_equip_models()
        elif cat == "宠物":
            self._fill_pet_models()
        else:
            self._fill_actions()

    # ---------------------------------------------------------------- 人物
    def _fill_parts(self, _pak: str = ""):
        self._loading = True
        for part, combo in self._part_combos.items():
            combo.blockSignals(True)
            combo.clear()
            combo.addItem("无")
            for pak, mid in self._part_model_items(part):
                combo.addItem(self._model_label(part, mid), (pak, mid))
            want = DEFAULT_MODELS.get(part)
            if want:
                idx = self._model_index(combo, int(want))
                if idx >= 0:
                    combo.setCurrentIndex(idx)
            combo.blockSignals(False)
        self._loading = False
        self._update_mount_state()
        self._fill_actions()

    def _fill_actions(self):
        actions: set[int] = set()
        for part, combo in self._part_combos.items():
            data = combo.currentData()
            if data is not None and part == "body":
                pak, mid = data
                actions.update(int(a) for a in self.av.actions(pak, part, str(mid))
                               if a.isdigit())
        if not actions:
            for part, combo in self._part_combos.items():
                data = combo.currentData()
                if data is not None:
                    pak, mid = data
                    actions.update(int(a) for a in self.av.actions(pak, part, str(mid))
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
        if part == "horse":
            self._match_harmor()
        self._update_mount_state()
        self._fill_actions()

    def _model_label(self, part: str, mid: int) -> str:
        return self._label_for(part, mid)

    def _model_index(self, combo, model: int) -> int:
        fallback = -1
        for index in range(combo.count()):
            data = combo.itemData(index)
            if data is None or data[1] != model:
                continue
            if data[0] == "player.pak":
                return index
            if fallback < 0:
                fallback = index
        return fallback

    def _update_mount_state(self):
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
        self._npc = None
        self._rebuild()

    def _career_linked(self):
        # 职业/进阶/坐骑档/性别变化 → 装备与技能列表、画面全部联动刷新
        if self.cmb_category.currentText() == "坐骑":
            self._fill_mount_models()
        self._apply_career()

    # ---- 职业/进阶/坐骑档 筛选 ----
    def _career_allowed(self) -> dict:
        """当前职业+进阶允许的装备模型集合（按部位，来自 viewjob 行）。"""
        career, advance = self.cmb_career.currentIndex(), self.cmb_advance.currentIndex()
        allowed: dict[str, set] = {}
        for (c, a, _m, _g), preset in self._viewjob.items():
            if c != career or a != advance:
                continue
            for part, model in preset.items():
                if model >= 0:
                    allowed.setdefault(part, set()).add(model)
        return allowed

    def _tier_horses(self) -> set:
        """当前坐骑档（徒步/坐骑/神骑）允许的坐骑模型集合。"""
        tier = self.cmb_mount.currentIndex()
        return {preset["horse"] for (_c, _a, m, _g), preset in self._viewjob.items()
                if m == tier and preset["horse"] >= 0}

    def _part_model_items(self, part: str):
        # 全量模型：viewjob 只是各职业默认套装，不能用来限制可选范围
        for pak, mid in all_part_models(self.src, part):
            yield pak, mid

    def _match_harmor(self):
        """骑甲与坐骑种类匹配：限定为坐骑 ID 及其 +1/+2（viewjob 实测规律）。"""
        data = self._part_combos["horse"].currentData()
        if data is None:
            return
        base = data[1]
        for part in ("harmorhead", "harmorbody"):
            combo = self._part_combos[part]
            current = combo.currentData()
            combo.blockSignals(True)
            combo.clear()
            combo.addItem("无")
            for pak, mid in all_part_models(self.src, part):
                if mid in (base, base + 1, base + 2):
                    combo.addItem(self._model_label(part, mid), (pak, mid))
            if current is not None and current[1] in (base, base + 1, base + 2):
                idx = self._model_index(combo, current[1])
                if idx >= 0:
                    combo.setCurrentIndex(idx)
            combo.blockSignals(False)

    def _apply_career(self):
        key = (self.cmb_career.currentIndex(), self.cmb_advance.currentIndex(),
               self.cmb_mount.currentIndex(), self.cmb_gender.currentIndex())
        preset = self._viewjob.get(key)
        if preset is None:
            self.lbl_status.setText(f"该组合没有职业预设（{key}）")
            return
        self._fill_parts()
        for part, combo in self._part_combos.items():
            if part in preset:
                model = preset[part]
                if model < 0:
                    combo.setCurrentText("无")
                else:
                    idx = self._model_index(combo, model)
                    if idx >= 0:
                        combo.setCurrentIndex(idx)
        self._match_harmor()
        self._update_mount_state()
        self._fill_skills()
        self._fill_actions()

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

    def _skill_model(self) -> int:
        idx = self.cmb_skill.currentIndex()
        if idx <= 0:
            return 0
        # 过滤后下拉索引与全量表不再对应，必须按技能 ID 查找
        sk = self._skill_by_id.get(self.cmb_skill.currentData())
        if sk is None:
            return 0
        model = (sk.attack, sk.hit, sk.fail)[self.cmb_effect.currentIndex()]
        if model == 0:
            # 所选类型无光效时回退到有光效的类型（很多技能只有受击光效）
            model = next((m for m in (sk.attack, sk.hit, sk.fail) if m), 0)
        return model

    @property
    def _skill_by_id(self):
        if not hasattr(self, "_skill_id_map"):
            self._skill_id_map = {s.id: s for s in self._skills}
        return self._skill_id_map

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

    # ---------------------------------------------------------------- NPC/召唤兽
    def _fill_npc_models(self):
        self.cmb_npc_model.blockSignals(True)
        self.cmb_npc_model.clear()
        summon_only = self.cmb_category.currentText() == "召唤兽"
        for pak, mid in all_npc_models(self.src):
            if summon_only and mid not in self._model_names.summon_models:
                continue
            self.cmb_npc_model.addItem(self._label_for("npc", mid), (pak, mid))
        self.cmb_npc_model.blockSignals(False)
        self._npc_changed()

    def _npc_changed(self):
        data = self.cmb_npc_model.currentData()
        if data is None:
            self.page.set_frames([])
            return
        pak, mid = data
        self._npc = (pak, mid)
        self._set_actions(pak, "", mid)
        self._rebuild()

    # ---------------------------------------------------------------- 坐骑
    def _fill_mount_models(self):
        tier_horses = set()  # 不再按档位限制坐骑显示范围
        for combo, part in ((self.cmb_horse, "horse"),
                            (self.cmb_hhead, "harmorhead"),
                            (self.cmb_hbody, "harmorbody")):
            combo.blockSignals(True)
            combo.clear()
            combo.addItem("无")
            for pak, mid in all_part_models(self.src, part):
                if part == "horse" and tier_horses and mid not in tier_horses:
                    continue
                combo.addItem(self._model_label(part, mid), (pak, mid))
            combo.blockSignals(False)
        if self.cmb_horse.count() > 1:
            self.cmb_horse.setCurrentIndex(1)
        self._mount_changed()

    def _mount_changed(self):
        data = self.cmb_horse.currentData()
        if data is not None:
            base = data[1]
            for combo, part in ((self.cmb_hhead, "harmorhead"),
                                (self.cmb_hbody, "harmorbody")):
                current = combo.currentData()
                combo.blockSignals(True)
                combo.clear()
                combo.addItem("无")
                for pak, mid in all_part_models(self.src, part):
                    if mid in (base, base + 1, base + 2):
                        combo.addItem(self._model_label(part, mid), (pak, mid))
                if current is not None and current[1] in (base, base + 1, base + 2):
                    idx = self._model_index(combo, current[1])
                    if idx >= 0:
                        combo.setCurrentIndex(idx)
                combo.blockSignals(False)
            self._set_actions(data[0], "horse", data[1])
        self._rebuild()

    # ---------------------------------------------------------------- 装备
    def _fill_equip_models(self):
        slot = self.cmb_equip_type.currentData()
        career = self.cmb_equip_career.currentIndex()
        needle = self.ed_equip_search.text().strip()
        self.cmb_equip_model.blockSignals(True)
        self.cmb_equip_model.clear()
        for item in self._items:
            if slot and item.slot != slot:
                continue
            if career > 0 and not item.careers[career - 1]:
                continue
            if needle and needle not in item.name:
                continue
            self.cmb_equip_model.addItem(f"{item.name}({item.id})", item.id)
        self.cmb_equip_model.blockSignals(False)
        if self.cmb_equip_model.count():
            self.cmb_equip_model.setCurrentIndex(0)
        self._equip_changed()

    def _equip_changed(self):
        item = self._item_by_id.get(self.cmb_equip_model.currentData())
        if item is None:
            self.page.set_frames([])
            self._update_prov()
            return
        from .scene import _decode_image
        icon_ent = None
        for rel in (f"icon/small/{item.icon}.a16", f"icon/big/{item.icon}.a16"):
            icon_ent = self.src.find_path(rel)
            if icon_ent is not None:
                break
        if icon_ent is not None:
            try:
                self.page.set_frames([_decode_image(self.src, icon_ent)])
            except Exception:  # noqa: BLE001
                self.page.set_frames([])
        else:
            self.page.set_frames([])
        self.lbl_status.setText(f"{item.name}（{item.id}）")
        self._update_prov()

    # ---------------------------------------------------------------- 宠物
    def _fill_pet_models(self):
        self.cmb_pet.blockSignals(True)
        self.cmb_pet.clear()
        for pak, mid in all_part_models(self.src, "pet"):
            self.cmb_pet.addItem(self._model_label("pet", mid), (pak, mid))
        self.cmb_pet.blockSignals(False)
        if self.cmb_pet.count():
            self.cmb_pet.setCurrentIndex(0)
        self._pet_changed()

    def _pet_changed(self):
        data = self.cmb_pet.currentData()
        if data is not None:
            self._set_actions(data[0], "pet", data[1])
        self._rebuild()

    # ---------------------------------------------------------------- 装配
    def _current_parts(self):
        """人物类别的部件选择（含坐骑甲联动）。"""
        parts = {}
        for part, combo in self._part_combos.items():
            data = combo.currentData()
            if data is not None:
                parts[part] = data[1]
        if parts.get("horse") is None:
            parts.pop("harmorbody", None)
            parts.pop("harmorhead", None)
        return parts

    def _current_pak(self) -> str:
        body = self._part_combos["body"].currentData()
        if body is not None:
            return body[0]
        for combo in self._part_combos.values():
            if combo.currentData() is not None:
                return combo.currentData()[0]
        return "player.pak"

    def _rebuild(self):
        if self._loading:
            return
        cat = self.cmb_category.currentText()
        action = self.cmb_action.currentText()
        model = self._skill_model()
        parts = self._current_parts() if cat == "人物" else {}
        if cat == "人物" and not parts and not model:
            self.lbl_status.setText("请至少选择一个部件")
            self.page.set_frames([])
            return
        if not action and (parts or self._npc is not None or cat != "人物"):
            return
        QApplication.setOverrideCursor(Qt.WaitCursor)
        try:
            def assemble_dir(d: int):
                if self._npc is not None:
                    npak, nmodel = self._npc
                    return self.npc.assemble_npc(npak, nmodel, int(action), d)
                if cat == "坐骑":
                    sel = {}
                    horse = self.cmb_horse.currentData()
                    if horse is not None:
                        sel["horse"] = horse[1]
                        for part, combo in (("harmorhead", self.cmb_hhead),
                                            ("harmorbody", self.cmb_hbody)):
                            data = combo.currentData()
                            if data is not None:
                                sel[part] = data[1]
                    if not sel:
                        return [], 80
                    pak = horse[0]
                    return self.av.assemble(pak, sel, int(action), d, self._dye())
                if cat == "装备":
                    return [], 80  # 装备走物品图标+详情（_equip_changed 已处理）
                if cat == "宠物":
                    data = self.cmb_pet.currentData()
                    if data is None:
                        return [], 80
                    pak, mid = data
                    return self.av.assemble(pak, {"pet": mid}, int(action), d,
                                            self._dye())
                if parts:
                    return self.av.assemble(self._current_pak(), parts,
                                            int(action), d, self._dye())
                return [], 80

            frames, delay = assemble_dir(int(self.cmb_dir.currentText()))
            if not frames:
                for d in (3, 7, 0, 1, 2, 4, 5, 6):
                    if d == int(self.cmb_dir.currentText()):
                        continue
                    frames, delay = assemble_dir(d)
                    if frames:
                        self.cmb_dir.blockSignals(True)
                        self.cmb_dir.setCurrentText(str(d))
                        self.cmb_dir.blockSignals(False)
                        break
            if model and cat in ("人物", "坐骑", "宠物"):
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
            self._update_prov()
            return
        self.page.set_frames(frames)
        self.page.fps.setValue(max(1, round(1000 / max(1, delay))))
        solo = f"（单看 {self._solo}，点「整机」恢复）" if self._solo else ""
        if self._npc is not None:
            name = self._model_names.query("npc", self._npc[1]).name
            solo = f"（NPC {name or self._npc[1]}）"
        self.lbl_status.setText(f"{len(frames)} 帧，{delay}ms/帧{solo}")
        self._update_prov()

    # ---------------------------------------------------------------- 资源明细
    def _update_prov(self):
        cat = self.cmb_category.currentText()
        action = self.cmb_action.currentText()
        direction = self.cmb_dir.currentText()
        lines = [f"类别: {cat}    动作: {action or '—'}    方向: {direction}",
                 "层叠键/逐帧参数: perinfo.pak（APCI）+ 各动作目录 perinfo.dat（PIF）", ""]
        def add(pak, rel, note=""):
            lines.append(f"  {pak}:{rel}" + (f"    {note}" if note else ""))
        if cat == "人物":
            pak = self._current_pak()
            lines.append(f"资源 pak: {pak}")
            for part, model in self._current_parts().items():
                rel = f"{part}/{model}/{action}/{direction}.apg"
                add(pak, rel, PART_LABEL.get(part, part))
                add(pak, f"{part}/{model}/{action}/perinfo.dat")
        elif cat == "坐骑":
            for part, combo in (("horse", self.cmb_horse),
                                ("harmorhead", self.cmb_hhead),
                                ("harmorbody", self.cmb_hbody)):
                data = combo.currentData()
                if data is not None:
                    pak, mid = data
                    add(pak, f"{part}/{mid}/{action}/{direction}.apg",
                        PART_LABEL[part])
        elif cat == "装备":
            item = self._item_by_id.get(self.cmb_equip_model.currentData())
            if item is not None:
                careers = [CAREER_NAMES[i] for i, ok in enumerate(item.careers) if ok]
                lines.append(f"物品: {item.name}（ID {item.id}，图标 {item.icon}）")
                lines.append(f"类型: {SLOT_NAMES.get(item.slot, item.slot)}    等级: {item.level}")
                lines.append(f"可用职业: {'、'.join(careers) if careers else '全部'}")
                lines.append(f"说明: {item.desc}")
                add("ui.pak", f"icon/small/{item.icon}.a16", "图标")
        elif cat == "宠物":
            data = self.cmb_pet.currentData()
            if data is not None:
                pak, mid = data
                add(pak, f"pet/{mid}/{action}/{direction}.apg", "宠物")
        elif self._npc is not None:
            pak, mid = self._npc
            name = self._model_names.query("npc", mid).name
            add(pak, f"{mid}/{action}/{direction}.apg",
                f"NPC {name or mid}")
        model = self._skill_model()
        if model and cat in ("人物", "坐骑", "宠物"):
            variant = self.cmb_variant.currentText() or "0"
            sk = self._skill_by_id.get(self.cmb_skill.currentData())
            lines.append("")
            lines.append(f"技能: {sk.name if sk else '?'}({self.cmb_skill.currentData()})  光效模型 {model}  变体 {variant}")
            add("magic*.pak", f"{model}/{variant}/back.apg", "背层")
            add("magic*.pak", f"{model}/{variant}/front.apg", "前层")
        self._prov_text = "\n".join(lines)
        self.prov.setPlainText(self._prov_text)

    def _toggle_prov(self):
        self.prov.setVisible(not self.prov.isVisible())

    def _copy_prov(self):
        QApplication.clipboard().setText(self.prov.toPlainText())
        self.lbl_status.setText("资源明细已复制到剪贴板")

    # ---------------------------------------------------------------- 导出
    def _export(self):
        if not self.page.frames:
            return
        idx = self.page.cur
        path, _ = QFileDialog.getSaveFileName(self, "导出当前帧", f"avatar_f{idx:03d}.png")
        if path:
            self.page.frames[idx].save(path)
