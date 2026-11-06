# 外部分析资料记录

记录日期：2026-11-06。以下资料保留在原位置，只读引用，不作为可执行输入。

## 版本与运行基线

- 当前原生客户端复刻、PAK 清单和 Python/C++ 像素对拍绑定 `D:\software\dhsh` 的现有版本。
- 不直接更新该目录。游戏更新可能改变 PAK、Lua/SCP、资源和客户端代码，使既有反汇编地址、资源路径及像素基线失效。
- 如需运行游戏做动态对比，使用单独的更新版安装或完整副本，并记录更新后的客户端与 PAK 版本。新版现象不能未经复核直接覆盖旧版结论。
- 动态对比重点：角色/ MAN 锚点、八方向动画、core 遮挡、地图出生点、SCP 消息与 Lua 回调。运行游戏只用于观察，不作为联网或加载保护组件的复刻方案。
- 用户已启动更新版游戏，目录为 `D:\software\dhsh2`。该目录仅作为新版动态对比来源；当前构建与测试继续使用旧版 `D:\software\dhsh`，两版证据分别记录。

## `D:\tmp` 中与本项目相关的资料

| 路径 | 内容 | 用途与限制 |
| --- | --- | --- |
| `apci_tree.pkl` | APCI 树状分析数据，2026-10-26 | 可用于核对 APCI 查询和纸娃娃目录结构；当前未反序列化，使用前需确认格式与来源。 |
| `cel_A.png`、`cel_B.png` | 应天府 CEL 两层位图，2026-10-23 | 地面层/辅助层视觉对照；运行时碰撞仍使用已确认的第一张地面层。 |
| `celcheck_crop.jpg`、`hz_tingyuan_cel_overlay.png`、`tingyuan_bg_cel.png`、`tingyuan_cel_overlay.png` | CEL 与地图背景叠加诊断 | 用于坐标和碰撞叠加复核，不直接作为游戏画面。 |
| `preview_yingtianfu.png`、`preview_ytf_maskcrop.jpg`、`preview_ytf_maskcrop2.jpg` | 应天府与 core 遮挡预览 | 用于场景和遮罩视觉对照。 |
| `preview_bangpai.png`、`preview_bp_groupcrop.png`、`preview_bp_groupcrop2.jpg` | 帮派地图与 group/frame 遮挡预览 | 后续动态遮挡状态参考；M1 不伪造帮派网络状态。 |
| `ytf_anim_base.png`、`ytf_anim_center.png`、`ytf_anim_topleft.png` | 应天府 MAN 动画不同锚点假设对照，2026-10-23 | 支持 MAN 使用记录坐标加帧偏移、稳定排序的核对；已实现 C++ 后还需以 PAK 对拍为准。 |
| `ytf_wuqi_bg_cel.png`、`ytf_wuqi_cel.png`、`ytf_wuqi_cel_overlay.png` | 武器店内景、CEL 和原客户端界面叠加截图 | 后续室内地图、角色、NPC、UI 和碰撞对照；包含原客户端截图，不是本复刻客户端输出。 |
| `cv_test.jpg`、`pil_test.jpg` | 图像解码/显示对照 | 仅作历史解码诊断，不替代当前逐像素对拍。 |

## 不纳入本项目的文件

`method_audit.txt`、`timeline_clean.txt`、`jelly-check.mjs`、`scenarioA.js`、`seed.js`、`lb_order` 和 `claude/cache-break-*.diff` 属于其他项目或无法建立与水浒客户端的关联，未作为本项目证据使用。
