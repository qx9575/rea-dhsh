# 未决问题（按优先级）

## P0：决定能否扩展

1. **已确认**：17 职业由 `viewjob` 加载器 `0x523790` 的 `career < 0x11` 检查硬限制；第 18 个职业必须改 C++/原生客户端。
2. **部分确认**：`resmirror` 加载器接受 part type `< 0x15`（0–20）、direction `< 9`；action 未看到硬上限。
3. `viewjob` 0x3c 结构的运行时查找函数、`+0x34`/`+0x38` 字段的精确消费点。

## P1：类别与资源

4. part type 16 裤子、8 武器光效的资源目录与消费函数。
5. `effect` 与 part type 4 魔法/特效的对应关系。
6. 发型（mouthairid 资源 ID）、翅膀（faeriewing）、披风的实际资源路径。
7. 星神造型 ID 的解析：是 body/head 模型，还是独立资源？
8. weapon 动作 `401`/`601` 的语义与触发条件。
9. perinfo.dat 覆盖率低（head/body/weapon 大量缺失）：默认锚点/延时是否就是引擎实际行为？

## P2：挂点与技能

10. `horseoffset` / `cloakoffset` / APCI 层叠键的优先级。
11. `Skill.csv` 攻击帧如何驱动光效生成与挂点。
12. 坐骑/骑甲/人物底心对齐的引擎公式。

## P3：交互与通信

13. SCP `MESSAGE` → C++ 分发函数；Lua 回调子集已索引，剩余走内置/C++ 回调。
14. `FSLuaSystem` / `FSLuaWndScript` / `FSLuaHorseMng` / `FSLuaSummonMng` / `FSLuaTUserHorseInfo` 的 C++ 方法语义与表读取关系。
15. `allstruct.csv` 与网络消息字段的对应；外观/技能/坐骑谁是服务器权威。
16. `msg.csv` 的节流规则。

## P4：数据质量

17. CSV 中英文逗号、引号、BOM、空列的统一处理。
18. `itemSpecData` 槽位 → 部件 → 模型公式的完整验证。
19. 大小写目录（Lcuff/lcuff、Rcuff/rcuff）是否为新旧资源并存。
