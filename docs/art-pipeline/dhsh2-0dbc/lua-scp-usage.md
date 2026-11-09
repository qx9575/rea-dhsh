# Lua / SCP 交互索引

> 只做静态提取，不执行 Lua；来源为 46 个 `.lua` 与 1,940 个 SCP 的 `#script`。

## 1. 规模

| 项 | 数量 |
|---|---|
| SCP 文件 | 1940 |
| SCP 窗口/控件 | 31652 |
| SCP MESSAGE 绑定 | 12423 |
| SCP #script 块 | 26 |
| Lua 单元（.lua + #script） | 72 |
| 其中 .lua | 46 |
| 其中 #script | 26 |

## 2. FSLua API 使用频次

| API | 次数 |
|---|---|
| FSLuaWndScript.RegMsgCall | 106 |
| FSLuaSystem.Log | 104 |
| FSLuaSystem.GetRootWindow | 35 |
| FSLuaWndScript.RegAfterCreate | 22 |
| FSLuaSystem.SearchRootWindow | 10 |
| FSLuaWndScript.RegWinRun | 7 |
| FSLuaSystem.RegFunction | 6 |
| FSLuaWndScript.RegBeforeRelease | 6 |
| FSLuaWndScript.RegWinSysClose | 6 |
| FSLuaSystem.CreateRootModal | 5 |
| FSLuaWndScript.RegWinDrawThis | 3 |
| FSLuaWndScript.RegWinPreDraw | 3 |
| FSLuaSystem.CreateRootWindow | 1 |

## 3. FSLuaInstall 模块

| 模块 | 次数 |
|---|---|
| FSLuaBaseEx | 30 |
| FSGameExchange | 23 |
| FSLuaUIWindow | 19 |
| FSLuaUIWins | 19 |
| FSLuaEvent | 18 |
| FSLuaCanvas | 11 |
| FSLuaSelfPlayer | 11 |
| FSLuaBitmap | 10 |
| FSLuaMemStream | 10 |
| FSLuaStrArray | 9 |
| FSLuaFSImgList | 8 |
| FSLuaFont | 8 |
| FSLuaSocket | 8 |
| FSLuaLine | 7 |
| FSLuaZCsv | 7 |
| config/ui/main/fbcommon | 5 |
| FSLuaIntBits | 4 |
| FSLuaFSListView | 3 |
| FSLuaTImgListItem | 3 |
| config/lua/scdefine | 3 |
| FSLuaEffect | 2 |
| FSLuaFSRootWindow | 2 |
| FSLuaHorseMng | 2 |
| FSLuaItemManager | 2 |
| FSLuaMonsterManager | 2 |
| FSLuaSummonMng | 2 |
| FSLuaTItemSpecific | 2 |
| FSLuaTMonsterClientData | 2 |
| FSLuaTUserHorseInfo | 2 |
| FSLuaTimer | 2 |
| FSLuaUserPic | 2 |
| FSLuaDBCFile | 1 |
| FSLuaFSBmpArray | 1 |
| FSLuaFSButton | 1 |
| FSLuaFSFlashInfo | 1 |
| FSLuaFSRichText | 1 |
| FSLuaFSWindow | 1 |
| FSLuaRollMsg | 1 |
| config/UI/smallgame/common | 1 |
| plugins/simpleproc | 1 |

## 4. 窗口名（GetRootWindow/SearchRootWindow/CreateRoot*）

| 窗口名 | 次数 |
|---|---|
| fsbuyfinger | 5 |
| activity1 | 3 |
| fssignin | 3 |
| goldfinger | 3 |
| CrossSrvFBTeamBtn | 2 |
| bossrankbtn | 2 |
| haiwangbossrankbtn | 2 |
| wndBossPoint | 2 |
| wndBtnSeriesFestival | 2 |
| AskCrossSrvFB | 1 |
| AskCrossSrvFB1 | 1 |
| CrossSrvFB | 1 |
| CrossSrvFBTeambtn | 1 |
| CrossSrvFBbtn | 1 |
| LaboureventQuestion | 1 |
| activity | 1 |
| activitybtn | 1 |
| bossrank | 1 |
| config/UI/main/CrossSrvFB.scp | 1 |
| config/UI/main/CrossSrvFBTeambtn.scp | 1 |
| config/UI/main/activity.scp | 1 |
| config/UI/main/activity1.scp | 1 |
| config/UI/question/labourevent.scp | 1 |
| crystalchoujiang | 1 |
| faction_fight_countdown | 1 |

## 5. CallScProcess

| 进程名 | 次数 |
|---|---|
| OpenMe | 11 |
| CloseMe | 4 |
| OnRecvList | 2 |
| Open | 2 |
| count_down_msg | 2 |
| FlashSereseventBtn | 1 |
| OnDoubleExpTipOpen | 1 |
| OnFBInstance | 1 |
| OnOpen | 1 |
| OnPropsExchangeOpen | 1 |
| RecvSigninData | 1 |
| SetHpRate | 1 |
| SetSigninData | 1 |
| SetSpRate | 1 |
| set_roll_msg | 1 |
| set_team_type | 1 |

## 6. RegMsgCall / RegFunction 名称

| 名称 | 次数 |
|---|---|
| OnClose | 19 |
| OnOpenBuyFinger | 5 |
| OnItemNumChange | 4 |
| OnOpenGoldFinger | 3 |
| ACTION | 2 |
| CACTION | 2 |
| ITEMINFO | 2 |
| OnBuy | 2 |
| OnBuyFinger | 2 |
| OnClick | 2 |
| OnClickCanSelect | 2 |
| OnExchangeSubmit | 2 |
| OnOut | 2 |
| OnReady | 2 |
| OnReg | 2 |
| OnSelectActivity | 2 |
| OnSelectExchange | 2 |
| FitChange | 1 |
| OnAccountFocusChange | 1 |
| OnActivityClick | 1 |
| OnBtn_huoyuezhi | 1 |
| OnBtn_jierihuodong | 1 |
| OnCancel | 1 |
| OnChongXingClick | 1 |
| OnCloseBtnClick | 1 |
| OnCrosssrvfbClick | 1 |
| OnCrytalRewardClick | 1 |
| OnEditChange | 1 |
| OnEditKeyDown | 1 |
| OnEditKeyEnter | 1 |

## 7. SCP MESSAGE 名称（Top 20）

| MESSAGE | 次数 |
|---|---|
| FSM_BTN_MSG | 6140 |
| FSM_MOUSE_UP | 1922 |
| FSM_ITEMDOCK | 806 |
| FSM_ISITEMCANDOCK | 631 |
| FSM_MOUSE_DOWN | 585 |
| FSM_LIST_SELECTED | 473 |
| FSM_ISITEMCANDRAG | 462 |
| FSM_MOUSE_DBCLK | 375 |
| FSM_CHECKBOX | 200 |
| FSM_EDIT_MSG | 181 |
| FSM_KEY_DOWN | 140 |
| FSM_SPIN_CHANGE | 71 |
| FSM_IMGLISTITEMTIP | 60 |
| FSM_RADIOGROUP | 59 |
| FSM_COMBOBOX_SELECTED | 59 |
| FSM_ISITEMCANCHUCK | 44 |
| FSM_MOUSE_LEAVE | 38 |
| FSM_MOUSE_HOVER | 31 |
| FSM_MOUSE_MOVE | 30 |
| FSM_KEY_UP | 29 |

## 8. SCP CODE 与 Lua 回调的交集

| 指标 | 数量 |
|---|---|
| SCP 不同 CODE 名 | 2791 |
| Lua RegMsgCall 回调名 | 69 |
| 两者交集 | 68 |
| 只在 SCP 中（C++/内置回调） | 2723 |

交集示例：FitChange、OnAccountFocusChange、OnActivityClick、OnBtn_huoyuezhi、OnBtn_jierihuodong、OnBuy、OnBuyFinger、OnCancel、OnChongXingClick、OnClick、OnClickCanSelect、OnClose、OnCloseBtnClick、OnCrosssrvfbClick、OnCrytalRewardClick、OnEditChange、OnEditKeyDown、OnEditKeyEnter、OnEditKeyUp、OnEnter

## 9. 纸娃娃相关 SCP 文件

| 指标 | 数量 |
|---|---|
| 相关 SCP 文件（user/property/horse/pet/stargod/equip/login） | 444 |
| 相关 MESSAGE 绑定 | 3553 |

## 10. 观察

- Lua 主要承担 UI 行为：窗口查找、`CallScProcess`、消息/生命周期回调；
  未发现直接读取 `viewjob`/`resmirror`/`mouthairid` 等纸娃娃表的 Lua 代码。
- `FSLuaInstall` 注册了 `FSLuaHorseMng`、`FSLuaSummonMng`、`FSLuaTUserHorseInfo` 等宿主模块，
  坐骑/召唤兽的运行时逻辑很可能在这些 C++ 模块里，而不是 Lua 表读取。
- 纸娃娃的部件/坐骑/技能装配更可能由 C++/SCP 消息层消费表；Lua 只是 UI 触发。
- `RegMsgCall("函数名")` 的参数是**回调函数名**，不是消息名；消息名在 SCP 的 `MESSAGE` 绑定里。
- 大量 SCP `CODE` 不在 Lua 回调集合中，说明它们走 C++/内置消息处理，下一步要用 Ghidra 定位分发函数。

## 11. 示例脚本

```lua

FSLuaInstall( "FSLuaBaseEx" )

MAXSERVERINDEX	= 12

lsAllServer	= {	[0]	= "@changyou.com",
				[1] = "@game.sohu.com",
				[2] = "@sohu.com",
				[3] = "@chinaren.com",
				[4] = "@sogou.com",
				[5] = "@17173.com",
				[6] = "@sina.com",
				[7] = "@163.com",
				[8] = "@qq.com",
				[9] = "@126.com",
				[10] = "@hotmail.com",
				[11] = "@gmail.com",
				[12] = "@yahoo.cn"
			}

function OnEditChange( p1, p2 )
	lsAccount:Open()
	lsAccount:Reset()

	local bInsert = true
	local szAccount = edAccount:GetText()
	local szUserTail = szAccount:match( "(@.*)" )
	if szUserTail then
		szUserTail:lower()
		local iUserTailLen = szUserTail:len()
		for iIdx = 0, MAXSERVERINDEX do
			local szSubHead = lsAllServer[iIdx]:sub( 1, iUserTailLen )
			if szSubHead == szUserTail then
				local szAdd = szAccount:sub( 1, -szUserTail:len() - 1 )
				local szAdr = szAccount:sub( 1, -szUserTail:len() - 1 )
```

## 12. 未知

- `FSLuaSystem` / `FSLuaWndScript` 各方法在 C++ 中的完整语义。
- `FSLuaHorseMng` / `FSLuaSummonMng` 等模块的方法与表读取关系。
- SCP `MESSAGE` 名称 → 消息 ID → C++ 分发函数 → Lua 回调的完整链路。
- 纸娃娃表在 C++ 中的消费函数（下一步用 Ghidra 定位）。
