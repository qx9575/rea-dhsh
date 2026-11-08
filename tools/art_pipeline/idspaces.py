"""ID 空间注册表：把三套部件枚举、动作、职业、槽位分开记录。

所有条目都带 source 与 confidence，避免把查看器的 UI 分类当成引擎权威。
"""
from __future__ import annotations

from dataclasses import dataclass

RESMIRROR_PART_TYPES = {
    0: "头部",
    1: "身体",
    2: "武器",
    3: "宠物",
    4: "魔法",
    7: "衣服",
    8: "武器光效",
    9: "脸部",
    10: "手腕",
    11: "脚部",
    12: "右腕",
    13: "坐骑",
    14: "骑甲身",
    15: "骑甲头",
    16: "裤子",
}

PARTINFO_PART_TYPES = {
    0: "头部",
    1: "脸",
    7: "衣服",
}

SHAPECLR_PART_TYPES = {
    0: "头部",
    2: "武器",
    7: "衣服",
    8: "武器光效",
    9: "面罩",
    10: "护手",
    11: "鞋子",
    12: "衣服后面的手",
}

ACTION_GROUPS = {
    10: "待机",
    20: "战斗待机",
    30: "跑",
    40: "物理攻击",
    50: "防御",
    60: "受击",
    70: "死亡",
    80: "冲刺",
    90: "魔法攻击",
    100: "肢体语言",
}

PSEUDO_ACTIONS = {
    10000: "10 与 30",
    10001: "10–90",
    10002: "10/20/30/50/60/70/80",
    10003: "10002 + 91/92",
    10004: "10–100 + 91/101",
    -1: "魔法/宠物（忽略朝向与转换）",
}

CAREERS = {
    0: "初心者",
    1: "剑客",
    2: "武师",
    3: "医师",
    4: "术士",
    5: "道士",
    6: "浪子",
    7: "弓手",
    8: "禁卫",
    9: "墨者",
    10: "巫煞",
    11: "魂武",
    12: "寒星",
    13: "花间舞",
    14: "剑九",
    15: "魔刀",
    16: "百变",
}

ADVANCES = {0: "职业", 1: "英雄", 2: "星将"}
MOUNT_TIERS = {0: "徒步", 1: "坐骑", 2: "神骑"}
GENDERS = {0: "男", 1: "女"}

# 查看器/物品表观察到的槽位；完整标签以 itemtypename.csv 为准。
ITEM_SLOTS_FALLBACK = {
    0: "消耗品", 1: "帽子", 2: "衣服", 3: "项链", 4: "武器",
    5: "首饰", 6: "纹章", 7: "背囊", 8: "腰带", 9: "鞋子",
    10: "面罩", 11: "护手", 12: "卡片", 13: "宝石", 14: "任务",
    15: "特殊", 16: "原料", 17: "头像", 18: "技能", 19: "食物",
    20: "宠物", 21: "帮派", 22: "宠物", 23: "图鉴", 24: "家具",
    25: "配方", 26: "饲料", 27: "骑甲", 28: "贺彩", 29: "珍宝",
    30: "召唤兽项链", 31: "召唤兽护甲", 32: "召唤兽护腕",
}


@dataclass(frozen=True)
class IdEntry:
    space: str
    value: str
    label: str
    source: str
    confidence: str


def id_entries() -> list[IdEntry]:
    out: list[IdEntry] = []

    def add(space: str, table: dict, source: str, confidence: str = "Observed") -> None:
        for value, label in table.items():
            out.append(IdEntry(space, str(value), label, source, confidence))

    add("part_type.resmirror", RESMIRROR_PART_TYPES, "config.pak:misc/resmirror.csv")
    add("part_type.partinfo", PARTINFO_PART_TYPES, "config.pak:ui/login/partinfo.csv")
    add("part_type.shapeclr", SHAPECLR_PART_TYPES, "config.pak:ui/user/shapeclrinfo.csv")
    add("action", ACTION_GROUPS, "config.pak:misc/resmirror.csv")
    add("action.pseudo", PSEUDO_ACTIONS, "config.pak:misc/resmirror.csv")
    add("career", CAREERS, "config.pak:ui/login/viewjob.csv")
    add("advance", ADVANCES, "config.pak:ui/login/viewjob.csv")
    add("mount_tier", MOUNT_TIERS, "config.pak:ui/login/viewjob.csv")
    add("gender", GENDERS, "config.pak:ui/login/viewjob.csv")
    add("item_slot.fallback", ITEM_SLOTS_FALLBACK, "viewer + itemSpecData.csv", "Inferred")
    for direction in range(8):
        out.append(IdEntry("direction", str(direction), f"方向 {direction}",
                           "resource path <direction>.apg", "Observed"))
    return out


def action_group(action: int) -> str:
    """把动作 ID 归入 resmirror 的语义组；未知返回空串。"""
    for base, label in sorted(ACTION_GROUPS.items(), reverse=True):
        if base == 100:
            if 100 <= action <= 199:
                return label
        elif base <= action < base + 10:
            return label
    return ""
