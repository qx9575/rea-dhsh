# 表字典与 ID 空间

> 路径为 PAK 内逻辑路径；读取工具：`tools/art_pipeline`。

## 1. 已纳入分析的表

| key | PAK 路径 | 行数 | 用途 |
|---|---|---|---|
| `resmirror` | `config.pak:misc/resmirror.csv` | 6951 | 部件/动作枚举 + 镜像规则 |
| `mirror_effect` | `config.pak:magic/MirrorEffect.csv` | 1 |  |
| `viewjob` | `config.pak:UI/login/viewjob.csv` | 272 | 职业/进阶/坐骑/性别 → 模型预设（13 列） |
| `bodyidequal` | `config.pak:bodyidequal.csv` | 396 | 怪物/身体 ID → 部件 + 颜色 + 武器光效 |
| `partinfo` | `config.pak:UI/login/partinfo.csv` | 46 |  |
| `shapeclrinfo` | `config.pak:UI/user/shapeclrinfo.csv` | 18 |  |
| `mouthairid` | `config.pak:misc/mouthairid.csv` | 161 |  |
| `mouthairmirror` | `config.pak:misc/mouthairmirror.csv` | 488 |  |
| `itemtypename` | `config.pak:item/itemtypename.csv` | 63 |  |
| `petinfo` | `config.pak:UI/pet/petinfo.csv` | 6 |  |
| `petequipsetname` | `config.pak:item/petequipsetname.csv` | 10 |  |
| `horseinfo` | `config.pak:UI/horse/horseinfo.csv` | 280 |  |
| `horseoffset` | `config.pak:misc/horseoffset.csv` | 339 | 挂点/偏移 |
| `cloakoffset` | `config.pak:misc/cloakoffset.csv` | 5957 | 挂点/偏移 |
| `hpoffset` | `config.pak:UI/combat/hpoffset.csv` | 7 | 挂点/偏移 |
| `faeriewing` | `config.pak:UI/faeriewing/faeriewing.csv` | 11 |  |
| `colorconvert` | `config.pak:misc/colorconvert.csv` | 20 |  |
| `special_suit` | `config.pak:specialSuit.csv` | 6 |  |
| `monster_equip_suits` | `config.pak:UI/property/summon/MonsterEquipSuits.csv` | 15 |  |
| `summoner_equip_recipe` | `config.pak:SummonerEquipRecipe.csv` | 250 |  |
| `skill` | `config.pak:magic/Skill.csv` | 973 | 玩家技能树/光效/攻击帧/费用 |
| `pet_skill` | `config.pak:UI/pet/skillinfo.csv` | 9 |  |
| `horse_skill` | `config.pak:UI/horse/horseskill.csv` | 514 |  |
| `ib_skill` | `config.pak:magic/ibskill.csv` | 195 |  |
| `armory_skill` | `config.pak:magic/ArmorySkill.csv` | 42 |  |
| `specialty` | `config.pak:UI/stargod/Specialty.csv` | 58 |  |
| `item_spec` | `ui.pak:item/itemSpecData.csv` | 14191 |  |
| `monsters` | `config.pak:monster/Monsters.csv` | 885 |  |
| `npchead` | `config.pak:monster/npchead.csv` | 8 |  |
| `career_info` | `config.pak:UI/career/CareerInfo.csv` | 37 |  |
| `player` | `config.pak:player.csv` | 0 |  |
| `lua_window_map` | `config.pak:lua/LuaWindowMap.csv` | 22 |  |
| `msg` | `config.pak:msg.csv` | 3 |  |
| `allstruct` | `config.pak:allstruct.csv` | 84 |  |

## 2. ID 空间

| 空间 | 值 | 标签 | 来源 | 置信度 |
|---|---|---|---|---|
| part_type.resmirror | 0 | 头部 | config.pak:misc/resmirror.csv | Observed |
| part_type.resmirror | 1 | 身体 | config.pak:misc/resmirror.csv | Observed |
| part_type.resmirror | 2 | 武器 | config.pak:misc/resmirror.csv | Observed |
| part_type.resmirror | 3 | 宠物 | config.pak:misc/resmirror.csv | Observed |
| part_type.resmirror | 4 | 魔法 | config.pak:misc/resmirror.csv | Observed |
| part_type.resmirror | 7 | 衣服 | config.pak:misc/resmirror.csv | Observed |
| part_type.resmirror | 8 | 武器光效 | config.pak:misc/resmirror.csv | Observed |
| part_type.resmirror | 9 | 脸部 | config.pak:misc/resmirror.csv | Observed |
| part_type.resmirror | 10 | 手腕 | config.pak:misc/resmirror.csv | Observed |
| part_type.resmirror | 11 | 脚部 | config.pak:misc/resmirror.csv | Observed |
| part_type.resmirror | 12 | 右腕 | config.pak:misc/resmirror.csv | Observed |
| part_type.resmirror | 13 | 坐骑 | config.pak:misc/resmirror.csv | Observed |
| part_type.resmirror | 14 | 骑甲身 | config.pak:misc/resmirror.csv | Observed |
| part_type.resmirror | 15 | 骑甲头 | config.pak:misc/resmirror.csv | Observed |
| part_type.resmirror | 16 | 裤子 | config.pak:misc/resmirror.csv | Observed |
| part_type.partinfo | 0 | 头部 | config.pak:ui/login/partinfo.csv | Observed |
| part_type.partinfo | 1 | 脸 | config.pak:ui/login/partinfo.csv | Observed |
| part_type.partinfo | 7 | 衣服 | config.pak:ui/login/partinfo.csv | Observed |
| part_type.shapeclr | 0 | 头部 | config.pak:ui/user/shapeclrinfo.csv | Observed |
| part_type.shapeclr | 2 | 武器 | config.pak:ui/user/shapeclrinfo.csv | Observed |
| part_type.shapeclr | 7 | 衣服 | config.pak:ui/user/shapeclrinfo.csv | Observed |
| part_type.shapeclr | 8 | 武器光效 | config.pak:ui/user/shapeclrinfo.csv | Observed |
| part_type.shapeclr | 9 | 面罩 | config.pak:ui/user/shapeclrinfo.csv | Observed |
| part_type.shapeclr | 10 | 护手 | config.pak:ui/user/shapeclrinfo.csv | Observed |
| part_type.shapeclr | 11 | 鞋子 | config.pak:ui/user/shapeclrinfo.csv | Observed |
| part_type.shapeclr | 12 | 衣服后面的手 | config.pak:ui/user/shapeclrinfo.csv | Observed |
| action | 10 | 待机 | config.pak:misc/resmirror.csv | Observed |
| action | 20 | 战斗待机 | config.pak:misc/resmirror.csv | Observed |
| action | 30 | 跑 | config.pak:misc/resmirror.csv | Observed |
| action | 40 | 物理攻击 | config.pak:misc/resmirror.csv | Observed |
| action | 50 | 防御 | config.pak:misc/resmirror.csv | Observed |
| action | 60 | 受击 | config.pak:misc/resmirror.csv | Observed |
| action | 70 | 死亡 | config.pak:misc/resmirror.csv | Observed |
| action | 80 | 冲刺 | config.pak:misc/resmirror.csv | Observed |
| action | 90 | 魔法攻击 | config.pak:misc/resmirror.csv | Observed |
| action | 100 | 肢体语言 | config.pak:misc/resmirror.csv | Observed |
| action.pseudo | 10000 | 10 与 30 | config.pak:misc/resmirror.csv | Observed |
| action.pseudo | 10001 | 10–90 | config.pak:misc/resmirror.csv | Observed |
| action.pseudo | 10002 | 10/20/30/50/60/70/80 | config.pak:misc/resmirror.csv | Observed |
| action.pseudo | 10003 | 10002 + 91/92 | config.pak:misc/resmirror.csv | Observed |
| action.pseudo | 10004 | 10–100 + 91/101 | config.pak:misc/resmirror.csv | Observed |
| action.pseudo | -1 | 魔法/宠物（忽略朝向与转换） | config.pak:misc/resmirror.csv | Observed |
| career | 0 | 初心者 | config.pak:ui/login/viewjob.csv | Observed |
| career | 1 | 剑客 | config.pak:ui/login/viewjob.csv | Observed |
| career | 2 | 武师 | config.pak:ui/login/viewjob.csv | Observed |
| career | 3 | 医师 | config.pak:ui/login/viewjob.csv | Observed |
| career | 4 | 术士 | config.pak:ui/login/viewjob.csv | Observed |
| career | 5 | 道士 | config.pak:ui/login/viewjob.csv | Observed |
| career | 6 | 浪子 | config.pak:ui/login/viewjob.csv | Observed |
| career | 7 | 弓手 | config.pak:ui/login/viewjob.csv | Observed |
| career | 8 | 禁卫 | config.pak:ui/login/viewjob.csv | Observed |
| career | 9 | 墨者 | config.pak:ui/login/viewjob.csv | Observed |
| career | 10 | 巫煞 | config.pak:ui/login/viewjob.csv | Observed |
| career | 11 | 魂武 | config.pak:ui/login/viewjob.csv | Observed |
| career | 12 | 寒星 | config.pak:ui/login/viewjob.csv | Observed |
| career | 13 | 花间舞 | config.pak:ui/login/viewjob.csv | Observed |
| career | 14 | 剑九 | config.pak:ui/login/viewjob.csv | Observed |
| career | 15 | 魔刀 | config.pak:ui/login/viewjob.csv | Observed |
| career | 16 | 百变 | config.pak:ui/login/viewjob.csv | Observed |
| advance | 0 | 职业 | config.pak:ui/login/viewjob.csv | Observed |
| advance | 1 | 英雄 | config.pak:ui/login/viewjob.csv | Observed |
| advance | 2 | 星将 | config.pak:ui/login/viewjob.csv | Observed |
| mount_tier | 0 | 徒步 | config.pak:ui/login/viewjob.csv | Observed |
| mount_tier | 1 | 坐骑 | config.pak:ui/login/viewjob.csv | Observed |
| mount_tier | 2 | 神骑 | config.pak:ui/login/viewjob.csv | Observed |
| gender | 0 | 男 | config.pak:ui/login/viewjob.csv | Observed |
| gender | 1 | 女 | config.pak:ui/login/viewjob.csv | Observed |
| item_slot.fallback | 0 | 消耗品 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 1 | 帽子 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 2 | 衣服 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 3 | 项链 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 4 | 武器 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 5 | 首饰 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 6 | 纹章 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 7 | 背囊 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 8 | 腰带 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 9 | 鞋子 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 10 | 面罩 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 11 | 护手 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 12 | 卡片 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 13 | 宝石 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 14 | 任务 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 15 | 特殊 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 16 | 原料 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 17 | 头像 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 18 | 技能 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 19 | 食物 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 20 | 宠物 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 21 | 帮派 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 22 | 宠物 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 23 | 图鉴 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 24 | 家具 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 25 | 配方 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 26 | 饲料 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 27 | 骑甲 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 28 | 贺彩 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 29 | 珍宝 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 30 | 召唤兽项链 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 31 | 召唤兽护甲 | viewer + itemSpecData.csv | Inferred |
| item_slot.fallback | 32 | 召唤兽护腕 | viewer + itemSpecData.csv | Inferred |
| direction | 0 | 方向 0 | resource path <direction>.apg | Observed |
| direction | 1 | 方向 1 | resource path <direction>.apg | Observed |
| direction | 2 | 方向 2 | resource path <direction>.apg | Observed |
| direction | 3 | 方向 3 | resource path <direction>.apg | Observed |
| direction | 4 | 方向 4 | resource path <direction>.apg | Observed |
| direction | 5 | 方向 5 | resource path <direction>.apg | Observed |
| direction | 6 | 方向 6 | resource path <direction>.apg | Observed |
| direction | 7 | 方向 7 | resource path <direction>.apg | Observed |

## 3. 关键关系

- `viewjob.csv`：`(career, advance, mount_tier, gender)` → 衣服/手套/鞋子/武器/坐骑/骑甲/星神造型。
- `bodyidequal.csv`：怪物/对等身体 ID → 身体/头/衣服/武器/鞋/面具/护腕 + 颜色 + 武器光效。
- `itemSpecData.csv`：物品 ID → 槽位 + 17 职业权限 + 图标 + 阶段/等级。
- `mouthairid.csv`：`(mount_id, body_shape, flying, race)` → 头发服务器/资源 ID。
- `horseinfo.csv`：坐骑 ID → 怪物 ID、类型、品级、技能、升级链。
- `Skill.csv`：技能 ID → 职业/系/阶位/图标/目标数/光效/攻击帧/技能树坐标/前置。
- `specialSuit.csv`：套装 → 面具 + 召唤兽 + 坐骑。
- `MonsterEquipSuits.csv` / `SummonerEquipRecipe.csv`：召唤兽装备。

## 4. 使用状态

| 表 | 查看器 | 原生客户端 | 引擎消费点 |
|---|---|---|---|
| resmirror | 未用 | 未用 | 待定位 |
| viewjob | 只用前 11 列 | 未用 | 待定位 |
| bodyidequal | 只用部件 ID，忽略颜色/武器光效 | 未用 | 待定位 |
| mouthairid / faeriewing / cloakoffset | 未用 | 未用 | 待定位 |
| horseoffset / hpoffset | 未用 | 未用 | 待定位 |
| Skill | 只用 7 列 | 未用 | 待定位 |
| itemSpecData | 只用 ID/图标/名称/等级/描述/槽位/职业 | 未用 | 待定位 |

## 5. 未知

- 每张表的具体加载函数、加载顺序、是否热重载。
- `allstruct.csv` 与网络消息的字段对应关系。
- `msg.csv` 的节流语义。
- Lua/SCP 索引已建立（`lua-scp-usage.md`），但未发现纸娃娃表被 Lua 直接读取；
  表的消费点更可能在 C++/SCP 消息层，需 Ghidra 定点分析。
