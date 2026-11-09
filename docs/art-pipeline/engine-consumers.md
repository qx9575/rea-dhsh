# 引擎消费函数定点分析

- 基线 SHA-256：`7a5e2c085ac79557ca104a2917e06f1a7e5f2827e4b68753bbaf1c754a4e78dc`
- Ghidra 项目：`ghidra-proj/dhsh-7a5e`
- 地址为 VA；只读静态分析，不运行游戏

## 1. 表 → 加载函数

| 表 | 加载函数 | 结构/大小 | 观察 |
|---|---|---|---|
| config/ui/login/viewjob.csv | `0x523790` | 0x3c | career 0..16 检查；每条 CSV 行插入 gender=0/1 两份；读取 advance、mount、7 个部件 ID、英雄分支、星神造型 |
| config/misc/resmirror.csv | `0x8a4f40` | map node | 同时加载 mountmirror / mouthairmirror / mouthairid；part type < 0x15，direction < 9 |
| config/misc/mountmirror.csv | `0x8a4f40` | map node | 与 resmirror 同一加载器；键为 model/action/dir 的打包值 |
| config/misc/mouthairmirror.csv | `0x8a4f40` | map node | 旧头发 ID → 新头发 ID |
| config/misc/mouthairid.csv | `0x8a4f40` | map node | 坐骑/体型/飞行/种族 → 头发服务器 ID / 资源 ID；资源 ID 存入节点 +0x10 |
| config/magic/MirrorEffect.csv | `0x8a6850` | ? | 调用 0x8a6650；按 part type / bodyID / action / face0..7 建镜像开关 |
| config\misc\horseoffset.csv | `0x81d4e0` | 0x2c | 11 字段：人物图 ID、坐骑图 ID、人物/阴影/宠物/翅膀/披风/星将/头部/脚底光环/脚底特效/中心特效偏移 |
| config/misc/colorconvert.csv | `0x81d790` | 8 | part type、part ID、颜色、新 part ID、新颜色、信息 |
| config/misc/cloakoffset.csv | `0x81dae0` | ? | 披风 ID/人物图 ID/坐骑/动作/方向 → 偏移；按身体图 + 动作 + 方向存 |
| config/ui/combat/hpoffset.csv | `0x799010` | u16 array | 目标 ID（u16）→ +0x504 数组的 u16 高度偏移 |
| config/bodyidequal.csv | `0x4408e0` | 0x20 | 16 个 u16：怪物 ID、身体、头、头色、衣、衣色、武器、武器色、武器光效、光效色、鞋、鞋色、面具、面具色、护腕、护腕色；qsort 比较器 0x43fbc0 |
| config/ui/login/partinfo.csv | `0x523a60` | ? | 先调用 viewjob 加载器 0x523790；再读 partinfo 的 6 个颜色列 |
| config/ui/user/shapeclrinfo.csv | `0x44e270` | 0x10 | part type 0..0xc，part ID，颜色 0..0xfb；每项 {type,id,color,index} |
| config/ui/faeriewing/faeriewing.csv | `0x6de530` | ? | 该函数同时绑定 wndFaeriewing/wndLevel/wndAttribute* 等 UI，表读取在函数后段 |
| config/ui/horse/horseinfo.csv | `0x504b60` | 0xec | 坐骑 ID、32 个字段块、怪物/等级/职业、名称/说明、类型/品级/特技等 |
| config/ui/horse/horseskill.csv | `0x5054c0` | 0x1c | 技能行、技能 ID、等级、坐骑等级、名称、说明 |
| config/magic/Skill.csv | `0x763b50` | 0x10 | 仅保留第二字段 == 0x11 的记录，存入 +0x72c 的列表 |
| config/magic/Skill.csv | `0x82c420 -> 0x82ba00` | ? | 主技能管理器；具体结构需继续分析 0x82ba00 |
| ui\item\ItemSpecData.csv | `0x50f0c0` | ? | 物品库；供 FSLuaItemManager.GetItemSpecByID/ByName 使用 |
| config/ui/horse/horseinfo.csv（技能扩展） | `0x5054c0 / 0x505ae0` | 0x1c | 骑术技能与坐骑等级关系 |
| config/ui/property/summon/MonsterEquipSuits.csv | `0x56c890` | ? | 召唤兽装备套装 |
| config\SummonerEquipRecipe.csv | `0x557500` | ? | 召唤兽装备配方 |
| config/ui/pet/skillinfo.csv | `0x550bf0` | ? | 宠物技能 |
| config/ui/career/CareerInfo.csv | `0x62e8b0` | ? | 职业进阶任务条件 |

## 2. `FSBodyResMirror` 消费者

| 地址 | 作用 |
|---|---|
| `0x8a5ca0` | FSBodyResMirror 单例 getter |
| `0x8274e0` | 按 part type=3（宠物）构造 /model/action/dir/ 路径 |
| `0x827be0` | 按 part type=4（魔法）构造 /model/action/front|back/ 路径 |
| `0x828510` | APCI/角色装配主引擎；调用镜像转换 |
| `0x8276c0` | part type=1（身体）镜像与路径 |
| `0x827970` | part type=3 的逐方向镜像 |
| `0x828070` | part type=0x11→8 的特殊映射，构造 /model/action/dir/ 路径 |
| `0x827350` | part type=3 的通用镜像与路径 |
| `0x8a5d50` | FSBodyResMirror 内部辅助 |

## 3. Lua 宿主模块与方法

| 模块 | 类初始化 | 方法 | 地址 | 说明 |
|---|---|---|---|---|
| FSLuaHorseMng | `0x7aa8e0` | GetRideHorse | `0x7ab2f0` | 返回 FSLuaTUserHorseInfo；从 FSHorseMng 取当前坐骑 |
| FSLuaHorseMng | `0x7aa8e0` | GetHorseHead | `0x7ab510` | 返回 FSLuaBitmap；路径 ui/horse/head/%d/%d.a16 |
| FSLuaTUserHorseInfo | `0x7aa750` | GetHorseName | `0x7ab330` | 对象 +0x190 关联名称 |
| FSLuaTUserHorseInfo | `0x7aa750` | IsRide | `0x7ab380` | flags & 0xf |
| FSLuaTUserHorseInfo | `0x7aa750` | IsFeed | `0x7ab3d0` | flags & 0xf0 |
| FSLuaTUserHorseInfo | `0x7aa750` | IsBind | `0x7ab420` | flags bit |
| FSLuaTUserHorseInfo | `0x7aa750` | IsLock | `0x7ab470` | flags bit |
| FSLuaTUserHorseInfo | `0x7aa750` | GetDescStr | `0x7ab4c0` | 说明字符串 |
| FSLuaSummonMng | `0x7aa5c0` | GetSummonerHead | `0x7abf60` | 返回 FSLuaBitmap；经 FSSummonMng 0x5be430 -> 0x5be6a0 查头像 |
| FSLuaItemManager | `0x7a9f80` | GetItemSpecByID | `0x7ab9f0` | 按 ID 查 ItemSpecData，返回 FSLuaTItemSpecific |
| FSLuaItemManager | `0x7a9f80` | GetItemSpecByName | `0x7aba40` | 按名称查 ItemSpecData |
| FSLuaItemManager | `0x7a9f80` | GetBitmapByID | `0x7abaa0` | 按 ID 取图标位图 |
| FSLuaItemManager | `0x7a9f80` | BigSize/SmallSize/LittleSize | `0x7ca5b0/0x7ca5c0/0x7ca5d0` | 图标尺寸 |
| FSLuaMonsterManager | `0x7aa430` | GetMonsterByName | `0x7abe10` | 按名称查怪物，返回 FSLuaTMonsterClientData |
| FSLuaSelfPlayer | `0x7ac820` | GetLevel/GetRoleName/GetCharID | `0x7ac820` | 玩家基础属性 |
| FSLuaSelfPlayer | `0x7ac820` | GetHeadID/GetHeadIP | `0x7ac820` | 玩家头像/头像 IP |
| FSLuaSelfPlayer | `0x7ac820` | GetAttrValue/GetAttrStr | `0x7ac820` | 属性数值/文本 |
| FSLuaSelfPlayer | `0x7ac820` | GetSelfOid/GetHaveTeam/FindTeam | `0x7ac820` | 自身/队伍 |

### 3.1 `FSLuaTUserHorseInfo` 字段

| 模块 | 偏移 | 字段 | 说明 |
|---|---|---|---|
| FSLuaTUserHorseInfo | +0x00 | iHorseOid | u32; getter 0x7abd30 |
| FSLuaTUserHorseInfo | +0x04 | iPos | u32; getter 0x7b17f0 |
| FSLuaTUserHorseInfo | +0x23 | flags | IsBind/IsLock 位 |
| FSLuaTUserHorseInfo | +0x24 | flags | IsRide/IsFeed 位 |

## 4. 硬上限与枚举

| 项 | 观察 |
|---|---|
| 职业数量 | viewjob 加载器 0x523790 检查 career < 0x11（17）；第 18 个职业会被拒绝 |
| 部件类型 | resmirror 加载器 0x8a4f40 接受 part type < 0x15（0..20）；表目前只定义 0..16 |
| 方向 | resmirror 加载器要求 direction < 9（0..8） |
| 动作 | 加载器未看到硬编码上限；实际资源动作包括 10..101、401/601、10000..10004 |
| 性别 | viewjob 每行复制 gender=0/1 两份；性别是查找键 |
| 星神造型 | viewjob 第 13 列读取后存为 u16；实际指向 npc*.pak 模型 |

## 5. 对扩展的直接结论

1. **第 18 个职业不能只靠改表**：`viewjob.csv` 加载器 `0x523790` 检查 `career < 0x11`；超出 0–16 的行会被直接丢弃。要新增职业必须改 C++ 或原生客户端。
2. **部件类型有扩展余量**：`resmirror` 加载器接受 part type `< 0x15`（0–20），但当前表只定义 0–16；17–20 的语义和资源目录仍未知，可作为后续探测方向。
3. **方向固定 0–8**：`resmirror` 加载器要求 direction `< 9`。
4. **`GetHorseHead` / `GetSummonerHead` 是 UI 头像**：分别走 `ui/horse/head/%d/%d.a16` 和 `FSSummonMng` 头像查找，不是 3D 坐骑/召唤兽装配。
5. **坐骑/披风/星将偏移是独立表**：`horseoffset` 0x2c 结构包含人物/阴影/宠物/翅膀/披风/星将/头部/脚底光环/脚底特效/中心特效偏移；`cloakoffset` 按身体图 + 动作 + 方向存偏移。
6. **`viewjob` 每行复制两份性别**：加载器把同一行插入 gender=0 和 gender=1；性别是查找键的一部分。

## 6. 未决

- `viewjob` 的运行时查找函数与 0x3c 结构各偏移的精确消费点。
- `FUN_0082ba00`（主 Skill 管理器）的完整技能结构与攻击帧消费。
- part type 17–20 的消费者与资源目录。
- `faeriewing.csv` 的资源加载段（0x6de530 主要是 UI 绑定）。
- `specialSuit.csv` 未在二进制中找到直接字符串引用，可能由 Lua/SCP 或动态路径加载。
