"""Ghidra 定点分析结果（当前 SHA 7a5e…）。

这些是人工复核后的静态证据，地址为 VA；报告由 report.py 生成。
"""

BINARY_SHA256 = "7a5e2c085ac79557ca104a2917e06f1a7e5f2827e4b68753bbaf1c754a4e78dc"
BINARY_SHA256_0DBC = "0dbc189dd4d3b7f615f5fa79798a2695fef64a13a5c58fb6c507c2c0860b519f"
GHIDRA_PROJECT = "ghidra-proj/dhsh-7a5e"
VERSION_0DBC = "1.50.27"

# 表 → 加载函数
TABLE_LOADERS = [    {
        "table": "config/ui/login/viewjob.csv",
        "loader": "0x523790",
        "struct_size": "0x3c",
        "notes": "career 0..16 检查；每条 CSV 行插入 gender=0/1 两份；读取 advance、mount、7 个部件 ID、英雄分支、星神造型",
    },
    {
        "table": "config/misc/resmirror.csv",
        "loader": "0x8a4f40",
        "struct_size": "map node",
        "notes": "同时加载 mountmirror / mouthairmirror / mouthairid；part type < 0x15，direction < 9",
    },
    {
        "table": "config/misc/mountmirror.csv",
        "loader": "0x8a4f40",
        "struct_size": "map node",
        "notes": "与 resmirror 同一加载器；键为 model/action/dir 的打包值",
    },
    {
        "table": "config/misc/mouthairmirror.csv",
        "loader": "0x8a4f40",
        "struct_size": "map node",
        "notes": "旧头发 ID → 新头发 ID",
    },
    {
        "table": "config/misc/mouthairid.csv",
        "loader": "0x8a4f40",
        "struct_size": "map node",
        "notes": "坐骑/体型/飞行/种族 → 头发服务器 ID / 资源 ID；资源 ID 存入节点 +0x10",
    },
    {
        "table": "config/magic/MirrorEffect.csv",
        "loader": "0x8a6850",
        "struct_size": "?",
        "notes": "调用 0x8a6650；按 part type / bodyID / action / face0..7 建镜像开关",
    },
    {
        "table": "config\\misc\\horseoffset.csv",
        "loader": "0x81d4e0",
        "struct_size": "0x2c",
        "notes": "11 字段：人物图 ID、坐骑图 ID、人物/阴影/宠物/翅膀/披风/星将/头部/脚底光环/脚底特效/中心特效偏移",
    },
    {
        "table": "config/misc/colorconvert.csv",
        "loader": "0x81d790",
        "struct_size": "8",
        "notes": "part type、part ID、颜色、新 part ID、新颜色、信息",
    },
    {
        "table": "config/misc/cloakoffset.csv",
        "loader": "0x81dae0",
        "struct_size": "?",
        "notes": "披风 ID/人物图 ID/坐骑/动作/方向 → 偏移；按身体图 + 动作 + 方向存",
    },
    {
        "table": "config/ui/combat/hpoffset.csv",
        "loader": "0x799010",
        "struct_size": "u16 array",
        "notes": "目标 ID（u16）→ +0x504 数组的 u16 高度偏移",
    },
    {
        "table": "config/bodyidequal.csv",
        "loader": "0x4408e0",
        "struct_size": "0x20",
        "notes": "16 个 u16：怪物 ID、身体、头、头色、衣、衣色、武器、武器色、武器光效、光效色、鞋、鞋色、面具、面具色、护腕、护腕色；qsort 比较器 0x43fbc0",
    },
    {
        "table": "config/ui/login/partinfo.csv",
        "loader": "0x523a60",
        "struct_size": "?",
        "notes": "先调用 viewjob 加载器 0x523790；再读 partinfo 的 6 个颜色列",
    },
    {
        "table": "config/ui/user/shapeclrinfo.csv",
        "loader": "0x44e270",
        "struct_size": "0x10",
        "notes": "part type 0..0xc，part ID，颜色 0..0xfb；每项 {type,id,color,index}",
    },
    {
        "table": "config/ui/faeriewing/faeriewing.csv",
        "loader": "0x6de530",
        "struct_size": "?",
        "notes": "该函数同时绑定 wndFaeriewing/wndLevel/wndAttribute* 等 UI，表读取在函数后段",
    },
    {
        "table": "config/ui/horse/horseinfo.csv",
        "loader": "0x504b60",
        "struct_size": "0xec",
        "notes": "坐骑 ID、32 个字段块、怪物/等级/职业、名称/说明、类型/品级/特技等",
    },
    {
        "table": "config/ui/horse/horseskill.csv",
        "loader": "0x5054c0",
        "struct_size": "0x1c",
        "notes": "技能行、技能 ID、等级、坐骑等级、名称、说明",
    },
    {
        "table": "config/magic/Skill.csv",
        "loader": "0x763b50",
        "struct_size": "0x10",
        "notes": "仅保留第二字段 == 0x11 的记录，存入 +0x72c 的列表",
    },
    {
        "table": "config/magic/Skill.csv",
        "loader": "0x82c420 -> 0x82ba00",
        "struct_size": "?",
        "notes": "主技能管理器；具体结构需继续分析 0x82ba00",
    },
    {
        "table": "ui\\item\\ItemSpecData.csv",
        "loader": "0x50f0c0",
        "struct_size": "?",
        "notes": "物品库；供 FSLuaItemManager.GetItemSpecByID/ByName 使用",
    },
    {
        "table": "config/ui/horse/horseinfo.csv（技能扩展）",
        "loader": "0x5054c0 / 0x505ae0",
        "struct_size": "0x1c",
        "notes": "骑术技能与坐骑等级关系",
    },
    {
        "table": "config/ui/property/summon/MonsterEquipSuits.csv",
        "loader": "0x56c890",
        "struct_size": "?",
        "notes": "召唤兽装备套装",
    },
    {
        "table": "config\\SummonerEquipRecipe.csv",
        "loader": "0x557500",
        "struct_size": "?",
        "notes": "召唤兽装备配方",
    },
    {
        "table": "config/ui/pet/skillinfo.csv",
        "loader": "0x550bf0",
        "struct_size": "?",
        "notes": "宠物技能",
    },
    {
        "table": "config/ui/career/CareerInfo.csv",
        "loader": "0x62e8b0",
        "struct_size": "?",
        "notes": "职业进阶任务条件",
    },
]

# 旧基线 loader → 新基线 loader（0dbc，1.50.27）
LOADER_0DBC = {
    "0x523790": "0x5238b0",
    "0x8a4f40": "0x8a7180",
    "0x8a6850": "0x8a8a70",
    "0x81d4e0": "0x81f7a0",
    "0x81d790": "0x81fa50",
    "0x81dae0": "0x81fda0",
    "0x799010": "0x79aa40",
    "0x4408e0": "0x440500",
    "0x523a60": "0x523b80",
    "0x44e270": "0x44e220",
    "0x6de530": "0x6de670",
    "0x504b60": "0x504b90",
    "0x5054c0": "0x5054f0",
    "0x763b50": "0x763bd0",
    "0x82c420 -> 0x82ba00": "0x82e720 -> ?",
    "0x50f0c0": "0x50f130",
    "0x5054c0 / 0x505ae0": "0x5054f0 / ?",
    "0x56c890": "0x56c990",
    "0x557500": "0x5575b0",
    "0x550bf0": "0x550ca0",
    "0x62e8b0": "unknown",
}
for _entry in TABLE_LOADERS:
    _entry["loader_0dbc"] = LOADER_0DBC.get(_entry["loader"], "")

# resmirror 运行时消费者
MIRROR_CONSUMERS = [
    ("0x8a5ca0", "FSBodyResMirror 单例 getter"),
    ("0x8274e0", "按 part type=3（宠物）构造 /model/action/dir/ 路径"),
    ("0x827be0", "按 part type=4（魔法）构造 /model/action/front|back/ 路径"),
    ("0x828510", "APCI/角色装配主引擎；调用镜像转换"),
    ("0x8276c0", "part type=1（身体）镜像与路径"),
    ("0x827970", "part type=3 的逐方向镜像"),
    ("0x828070", "part type=0x11→8 的特殊映射，构造 /model/action/dir/ 路径"),
    ("0x827350", "part type=3 的通用镜像与路径"),
    ("0x8a5d50", "FSBodyResMirror 内部辅助"),
]

# 新基线 FSBodyResMirror 消费者（0dbc）
MIRROR_CONSUMERS_0DBC = [
    ("0x8a7ee0", "FSBodyResMirror 单例 getter（0dbc）"),
    ("0x829720", "按地址顺序推断：part type 3 路径"),
    ("0x8298b0", "按地址顺序推断：part type 3 路径"),
    ("0x829a90", "按地址顺序推断：part type 1/3"),
    ("0x829d40", "按地址顺序推断：part type 3 逐方向"),
    ("0x829fb0", "按地址顺序推断：part type 4 front/back"),
    ("0x82a440", "按地址顺序推断：part type 0x11→8"),
    ("0x82a5b0", "按地址顺序推断：APCI/角色装配主引擎"),
    ("0x8a7f90", "FSBodyResMirror 内部辅助（0dbc）"),
]

# Lua 宿主方法注册函数（0dbc；方法实现地址待逐个重定位）
LUA_HOST_REG_0DBC = {
    "FSLuaHorseMng": "0x7ad880",
    "FSLuaSummonMng": "0x7ae180",
    "FSLuaTUserHorseInfo": "0x7ad500",
    "FSLuaItemManager": "0x7adaa0",
    "FSLuaMonsterManager": "0x7ae0f0",
    "FSLuaSelfPlayer": "0x7abf40 / 0x7ac020（待确认）",
}

# Lua 宿主模块
LUA_HOSTS = [
    {
        "module": "FSLuaHorseMng",
        "class_init": "0x7aa8e0",
        "methods": [
            ("GetRideHorse", "0x7ab2f0", "返回 FSLuaTUserHorseInfo；从 FSHorseMng 取当前坐骑"),
            ("GetHorseHead", "0x7ab510", "返回 FSLuaBitmap；路径 ui/horse/head/%d/%d.a16"),
        ],
    },
    {
        "module": "FSLuaTUserHorseInfo",
        "class_init": "0x7aa750",
        "fields": [
            ("+0x00", "iHorseOid", "u32; getter 0x7abd30"),
            ("+0x04", "iPos", "u32; getter 0x7b17f0"),
            ("+0x23", "flags", "IsBind/IsLock 位"),
            ("+0x24", "flags", "IsRide/IsFeed 位"),
        ],
        "methods": [
            ("GetHorseName", "0x7ab330", "对象 +0x190 关联名称"),
            ("IsRide", "0x7ab380", "flags & 0xf"),
            ("IsFeed", "0x7ab3d0", "flags & 0xf0"),
            ("IsBind", "0x7ab420", "flags bit"),
            ("IsLock", "0x7ab470", "flags bit"),
            ("GetDescStr", "0x7ab4c0", "说明字符串"),
        ],
    },
    {
        "module": "FSLuaSummonMng",
        "class_init": "0x7aa5c0",
        "methods": [
            ("GetSummonerHead", "0x7abf60", "返回 FSLuaBitmap；经 FSSummonMng 0x5be430 -> 0x5be6a0 查头像"),
        ],
    },
    {
        "module": "FSLuaItemManager",
        "class_init": "0x7a9f80",
        "methods": [
            ("GetItemSpecByID", "0x7ab9f0", "按 ID 查 ItemSpecData，返回 FSLuaTItemSpecific"),
            ("GetItemSpecByName", "0x7aba40", "按名称查 ItemSpecData"),
            ("GetBitmapByID", "0x7abaa0", "按 ID 取图标位图"),
            ("BigSize/SmallSize/LittleSize", "0x7ca5b0/0x7ca5c0/0x7ca5d0", "图标尺寸"),
        ],
    },
    {
        "module": "FSLuaMonsterManager",
        "class_init": "0x7aa430",
        "methods": [
            ("GetMonsterByName", "0x7abe10", "按名称查怪物，返回 FSLuaTMonsterClientData"),
        ],
    },
    {
        "module": "FSLuaSelfPlayer",
        "class_init": "0x7ac820",
        "methods": [
            ("GetLevel/GetRoleName/GetCharID", "0x7ac820", "玩家基础属性"),
            ("GetHeadID/GetHeadIP", "0x7ac820", "玩家头像/头像 IP"),
            ("GetAttrValue/GetAttrStr", "0x7ac820", "属性数值/文本"),
            ("GetSelfOid/GetHaveTeam/FindTeam", "0x7ac820", "自身/队伍"),
        ],
    },
]

HARD_LIMITS = [
    ("职业数量", "viewjob 加载器 0x523790 检查 career < 0x11（17）；第 18 个职业会被拒绝"),
    ("部件类型", "resmirror 加载器 0x8a4f40 接受 part type < 0x15（0..20）；表目前只定义 0..16"),
    ("方向", "resmirror 加载器要求 direction < 9（0..8）"),
    ("动作", "加载器未看到硬编码上限；实际资源动作包括 10..101、401/601、10000..10004"),
    ("性别", "viewjob 每行复制 gender=0/1 两份；性别是查找键"),
    ("星神造型", "viewjob 第 13 列读取后存为 u16；实际指向 npc*.pak 模型"),
]


def summary() -> dict:
    return {
        "binary_sha256": BINARY_SHA256,
        "binary_sha256_0dbc": BINARY_SHA256_0DBC,
        "version_0dbc": VERSION_0DBC,
        "ghidra_project": GHIDRA_PROJECT,
        "table_loaders": len(TABLE_LOADERS),
        "mirror_consumers": len(MIRROR_CONSUMERS),
        "mirror_consumers_0dbc": len(MIRROR_CONSUMERS_0DBC),
        "lua_hosts": len(LUA_HOSTS),
        "hard_limits": len(HARD_LIMITS),
    }
