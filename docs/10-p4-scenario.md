# P4 的场景验收（Effect、Buff、持久化）

agent 只能做到主菜单自检，进存档、地牢、战斗的验收由用户按这份流程操作一次，日志作证据（[ADR 0016](adr/0016-test-rig-and-manual-scenarios.md)）。和 [09-lua-scenario.md](09-lua-scenario.md) 的关系：09 是 P3（Lua 运行时）的场景，已经验收；这里是 P4 的场景，部署、启动、保存日志的做法相同（`rig.py`），方案和测试 Mod 不同，检查脚本是 `tools/p4_scenario_check.py`。09 里 `lua` 方案的 hook 报告口径（1 + 4 个目标）在 P4 也成立，这里的检查脚本按同样的口径判 #6。

P4 在 2026-11-15 按这份流程验收通过。第一次运行跑了必做部分和第二段（日志 `build/rig_logs/scenario4`、`rig_logs/combat`）：检查脚本 46 项，通过 36，失败 0，必需项没有触发 2。`shield.natural` 是归因问题，修复 F6 之后用同一份日志重判通过；`persist.battle` 是那次没做战斗中读档。补跑的一段（`rig_logs/scenario4-battle`，构建含 F6、F7）做了战斗中存档读档：`persist.battle`、`shield.natural` 通过，护盾 237 次变化逐行重放都和 DDShield 一致；报的 2 项失败是 `p4c_persist` 对两条旧载体的误报（上一次运行 `tamper-persist` 之后不一致的读档按设计丢弃了状态，空状态的载体留在英雄身上，回城时游戏照样删掉，原因 `town_visit`），之后 `p4c_persist` 对这种载体只记信息（`town_visit.stale`）。用户目测：有护盾时 tray 里多出载体的图标，全部挡下时弹 Blocked；没有护盾条，护盾数值看不出来。

分三部分：

- **必做部分**：一次游戏运行（scenario4 方案，存档 A，中途几次退回主菜单再读档），大约 40–50 分钟。
- **第二段**：另一次游戏运行（combat 方案，重跑 `test_combat`，用存档 B），大约 10 分钟，**也是必做的**。为什么要另跑见那一节。检查脚本要两段的日志一起给才会全部通过：只给必做部分的日志时 `combat.stack` 是「没有触发」，退出码 2。
- **可选部分**：单独列在最后，不做不影响必做部分的判定。

所有命令都在 guest 的 Git Bash 里执行，工作目录 `D:\workplace`。

## P4 的整体验收

检查脚本逐项判定这 6 条，下文和检查脚本输出里的 #1–#6 就是这里的编号：

1. 用 Lua 复刻 DDShield 的 `shield_gain_N`（和护盾上限、护盾吸收伤害、战斗结束清理），逐次命中的结算和 DDShield 的规则一致：吸收量 = min(护盾, 最终伤害)，全部吸收时显示 Blocked，部分吸收时生命损失 = 最终伤害 − 吸收量，护盾不超过上限。
2. 脚本 Buff 的状态经过存档和读档（城镇里、地牢走廊里、战斗中）后保持一致；旁路和存档不一致时被发现并报告。
3. 重复施加、战斗结束清理、回城清理等边界正确：同一个 id 加两次是两条（各自的状态），战斗结束删掉的载体调用 `on_remove`，怪物的状态随战斗丢弃。
4. 事件回调里的写操作在下一帧执行，日志可以对上；整个场景没有崩溃，buff 的 guid 前后一致。
5. 从 Lua 施加 effect 在走廊和战斗里生效，在城镇里被拒绝并给出原因；规则扩展在城镇、走廊、战斗里按 Lua 的结果生效。
6. 没有 Lua Mod 时行为是原版的，hook 报告符合存档旁路的口径（观察 hook 常驻，1 + 4 个目标，[ADR 0031](adr/0031-persist-sidecar-content-hash.md)）。

## 这次要拿到的证据

检查脚本逐项判定上面的 6 条，括号里是编号；另外落实第一轮三份审核报告（A、B、C）要的运行证据，对应关系见「审核要的运行证据在哪里」一节，审核的编号查 [history/p4-review-findings.md](history/p4-review-findings.md)。

| 验收项 | 由谁产生 | 需要的游戏流程 |
| --- | --- | --- |
| 复刻 DDShield：技能获得护盾、全部吸收不扣血、部分吸收的生命损失（英雄和怪物）、不超过上限、每回合增减、战斗结束清理、`shielded` 规则（#1） | `shield_lite`（DDLua Example Shield Lite）的 SHIELD 日志，`p4c_shield`（DDLua Test P4c Shield）驱动其余规则 | 开战后先别操作，等日志窗口里出现「通过 [drive.N]」（约 5 秒）；之后圣骑士用 Smite（数据层给它加了 `shield_self_gain_5`），怪物打有护盾的英雄，英雄打怪物（每个怪物都有 10 点护盾） |
| 脚本 Buff 的状态经过存档、读档保持一致：城镇、走廊、战斗中（#2） | `p4c_persist`（DDLua Test P4c Persist） | 三个场合各让游戏存一次档，退回主菜单再读 |
| 旁路真的写了；和存档不一致时被发现并报告（#2） | DDCore、DDLua、`p4c_persist` | 最后用 `rig.py tamper-persist` 改旁路再读档 |
| 同一个 id 两条、战斗结束和撤退时删除的载体调用 `on_remove`、怪物的状态随战斗丢弃、回城清理（#3） | `p4c_persist` | 城镇里进存档、打完一场战斗、撤退一场、回城 |
| 事件回调里的写操作下一帧执行；城镇、走廊、战斗里各种写操作都走过；没有崩溃，guid 前后一致（#4） | `p4b_combat`（DDLua Test P4b Combat）、`p4c_persist`、DDLua | 城镇、走廊、战斗各停一会儿；最后从菜单正常退出 |
| 从 Lua 施加 effect：城镇里拒绝，走廊、战斗里生效；规则在三个场合按 Lua 的结果生效（#5） | `p4b_combat` | 城镇、走廊（刚进地牢时）、战斗各停 5 秒 |
| 没有 Lua Mod 时是原版行为，hook 报告是 1 + 4 个目标（#6） | DDCore 主菜单的 hook 报告；可选：一个没有 Lua Mod 的存档 | 无（主菜单）；可选项见后 |

## 测试 Mod 会改什么

- **DDLua Example Shield Lite**：护盾。部署时 `rig.py` 从真实安装只读地复制圣骑士的职业文件，给 Smite 的 5 个等级加上 `shield_self_gain_5`，放进这个 Mod 的副本（`mods\DDLuaTest_shield_lite\heroes\crusader\`）：**勾选它的存档里，圣骑士每次用 Smite 都给自己加 5 点护盾**（上限 12），护盾吸收他受到的伤害。有护盾的角色身上有一条 `protection_rating +0` 的载体 Buff。生命降到 0（死门）时护盾清零，和 DDShield 一样。
- **DDLua Test P4c Shield**：每次进存档的前两场战斗（读档恢复的那场也算），进入战斗 2 秒后给队伍里一个英雄、第二个英雄和每个怪物施加各种护盾 effect（上限、加满、破盾、再生、衰减、穿透……），每个怪物有 10 点护盾（之后新出现的怪物也补上）；给两个英雄各挂几条速度 +0 的规则探针 buff（战斗结束游戏删掉）。驱动做到一半战斗就结束了（撤退、打完）时这场不算，下一场再驱动。英雄在死门上挨打时，前两次分别写一半和全部的吸收量（部分吸收在死门上不起作用，全部吸收按格挡处理：这一下不判定死亡）。
- **DDLua Test P4c Persist**：给英雄、怪物加速度 +0 的标记 buff（`p4c_mark`、`p4c_bend`），记录状态；不改玩法。来源是 skill，回城时游戏清掉队伍英雄身上的；城镇里空闲英雄身上的留着。每次进存档在城镇里加两条临时的同 id 标记、核对后马上删掉；名册里没有它的城镇标记时（第一次进存档、回城之后）再加两条要保存的。每场战斗给队伍里一个英雄加两条、给每个怪物各加一条。
- **DDLua Test P4b Combat**：城镇、走廊、战斗各一轮，临时加几条测试 buff（速度 +0、+1、+2），给一个怪物加两条再删一条，约 1.5 秒内删干净。

三个测试 Mod 从 DDCore 读当前的队伍、怪物和是否在战斗中（`dd.actor.party`、`dd.actor.monsters`、`dd.actor.in_battle`），不用等角色出手：真实游戏开战后要过 5～9 秒才有第一次出手，读进战斗中的存档时游戏也不派发 `battle.start`。

## 准备

### 1. 部署

```
source DD.ModLoader/tools/env.sh
python -B DD.ModLoader/tools/rig.py deploy scenario4
python -B DD.ModLoader/tools/rig.py status
```

`deploy` 应输出「生成：mods/DDLuaTest_shield_lite/heroes/crusader/crusader.info.darkest」和 4 行 `Lua Mod：`。`status` 应显示：

```
当前方案：scenario4
Lua Mod：mods/DDLuaTest_p4b_combat（标题 DDLua Test P4b Combat）
Lua Mod：mods/DDLuaTest_shield_lite（标题 DDLua Example Shield Lite）
Lua Mod：mods/DDLuaTest_p4c_shield（标题 DDLua Test P4c Shield）
Lua Mod：mods/DDLuaTest_p4c_persist（标题 DDLua Test P4c Persist）
DDModLoader\cfg：DeathBlowCap.cfg、ProtectionCap.cfg
游戏运行中：否
真实安装未被改动（……）
```

`DDModLoader\cfg` 里不能有 `DDLua.cfg`（DDLua 要用默认配置）。`status` 有别的问题行时不要开始，把输出交给 agent。

这个方案不带 `test_combat`：它的 `damage.final` 订阅也写吸收量，和护盾冲突。它在「第二段」里单独跑。

### 2. 选存档

用 **P3 的存档 A**（09 里新建、勾过 5 个 P3 测试 Mod 的那个）。原因：已经走过教学关，城镇里有英雄，有圣骑士 Reynauld；P3 的 5 个测试 Mod 在 scenario4 方案下不在 `mods` 里，DDLua 会当作「没有找到目录」跳过。**游戏读到启用列表里不存在的 Mod 时怎么处理（提示、忽略、拒绝读档）没有确认**：读档出问题时改用一个空存档位新建战役（走完教学关，回主菜单再进一次，见 09 的第 2、3 步），或者 P4-0C 用过的存档 B（那样第二段和可选部分 A 要换一个别的存档位，下文的「存档 A」都指跑必做部分的那个存档）。存档位以前跑过本场景也没关系：新战役读到的是 `empty`，旧的旁路会被归档。

不要用正在玩的存档：这几个 Mod 会改圣骑士的 Smite，存档里会留下测试 buff。存档会被 Steam 云同步；想更保险，开始前把 `Steam\userdata\<账号>\262060\remote` 整个复制一份。

**存档 A 只在 scenario4 方案下进**。在别的方案（第二段的 combat、`deploy lua`）下进存档 A 并让游戏存了档，游戏存档往前走了，DDLua 的旁路没跟上（那两个方案不写 p4c_persist 的旁路），下次在 scenario4 下读存档 A 就是 `mismatch`，检查脚本判失败（`persist.unexpected`，提示里会写这一条）。第二段用存档 B。万一进过了，或者不确定，重跑之前先在游戏没运行时执行

```
python -B DD.ModLoader/tools/rig.py reset-persist
```

它把测试目录的 `DDModLoader\save`（所有存档位的旁路）改名为 `save.old-<时间>` 留底，之后第一次读档是 `empty`，照常往下走。

存档里要有：一个圣骑士（Crusader），技能里选着 Smite（新生成的圣骑士必带 Smite，在英雄界面的技能栏里确认它是选中的四个之一）；另外至少 3 个英雄组队；城镇里有空位能招募一个英雄（名册满了就先解雇一个不要的）。

### 3. 要勾选的 Mod

在存档 A 的 Mod 列表里勾选这 4 个（标题）：

- `DDLua Test P4b Combat`
- `DDLua Example Shield Lite`
- `DDLua Test P4c Shield`（依赖 Shield Lite，两个都要勾）
- `DDLua Test P4c Persist`

### 4. 开一个看日志的窗口

另开一个 Git Bash 窗口，运行下面这条（游戏启动前后都可以，`Ctrl+C` 结束），之后几步要看到它输出的某一行再往下走：

```
tail -n 0 -F "/d/Program Files (x86)/Steam/steamapps/common/DarkestDungeon_Dev/DDModLoader/log/DDLua.log" | grep --line-buffered -E 'drive\.|drive\] 第|battle\.add|round\] where=|rounddone\.|town\.add|失败 \['
```

会看到的行（`[时间] [INFO] [Mod] …` 开头）：

- `[p4b_combat] 信息 [round] where=town|corridor|battle`：`p4b_combat` 在这个场合开始一轮，约 1.5 秒后是「通过 [rounddone.<场合>]」。
- `[p4c_persist] 信息 [battle.add] 第 N 场战斗的载体加好了`：这场战斗的标记加好了（开战 1～2 秒）。
- `[p4c_shield] 通过 [drive.N] 第 N 场：施加 …步`：这场的护盾驱动做完了（开战约 5 秒）。`drive] 第 N 场：新出现的怪物 … 补上护盾` 是之后才出现的怪物。
- `[p4c_persist] 信息 [town.add] 城镇的载体加好了`：城镇的标记加好了。
- 任何「失败 [标签]」：记下来，照常往下走，最后交给 agent。

## 一次运行（必做）

全程在一次游戏运行里走完；中途退回主菜单再读档是流程的一部分，不要退出游戏。每次「进存档」之后至少等 10 秒再操作（测试 Mod 在这段时间里加载、核对、写日志）。

1. **启动游戏**：确认 `rig.py status` 显示「游戏运行中：否」，再运行测试目录的 `start.bat`：

   ```
   (cd "/d/Program Files (x86)/Steam/steamapps/common/DarkestDungeon_Dev" && cmd //c start.bat)
   ```

   不要从 Steam 启动（那是真实安装），不要用 `rig.py run`（到时会强杀游戏）。

2. **城镇，第一次存档**：在存档 A 上勾选上面 4 个 Mod，进入存档，在城镇里**等 10 秒**（日志窗口里出现「城镇的载体加好了」和 `rounddone.town`：`p4c_persist` 给名册里第一个英雄加两条同 id 的标记，`p4b_combat` 做城镇那一轮）。然后在驿站马车**招募一个英雄，等 5 秒，再把刚招的这个解雇，再等 5 秒**（检查脚本据此看招募、解雇是不是各自让游戏存档）。

3. **读城镇存档**：用游戏菜单退回主菜单，再进存档 A，等 10 秒。

4. **出发**：组队（带上圣骑士，放在第 1 或第 2 位，Smite 只能从这两个位置用），选一个短的探索任务出发。

5. **走廊，第二次存档**：进地牢后**先站着别动，等 5 秒**（日志窗口里出现 `round] where=corridor`、之后 `rounddone.corridor`：`p4b_combat` 做走廊那一轮），再在走廊里**走 4 步以上**（游戏几乎每走一步都存档；`p4c_persist` 第一步时给队伍里的一个英雄加走廊的标记，之后每步改它的状态），停 3 秒，退回主菜单，再进存档 A，等 10 秒（这次读档时 DDLua 订阅着 `buff.*`，加载线程上的增删会作为延迟事件出现；`p4b_combat` 在这次进存档的走廊里再做一轮）。

   **止损**：进地牢后 15 秒内日志窗口里没有出现 `round] where=corridor`，就不要往下走：走廊、战斗里的必需项都靠读队伍（`dd.actor.party()`，队伍表还没在游戏里读过），读不到时测试 Mod 什么都不做，再跑也是白跑。这时用游戏菜单退回主菜单、退出游戏，执行 `python -B DD.ModLoader/tools/rig.py save-log scenario4-party`，把这个目录交给 agent。

6. **第一场战斗，第三次存档**：往前走到第一场战斗。
   - 开战后**先别操作**，等日志窗口里出现 `p4c_shield` 的「通过 [drive.1]」（一般在开战 5 秒以内；看不到日志就等 8 秒）。这段时间里 `p4c_persist` 给队伍里一个英雄和每个怪物加标记（`battle.add`），`p4b_combat` 做战斗那一轮，`p4c_shield` 驱动护盾规则、给每个怪物 10 点护盾。三者都直接读队伍和怪物，不用等谁出手；怪物先出手没关系（怪物的回合游戏自己走），轮到英雄时先别选技能。**止损**：开战 5 秒内没有 `battle.add`，或者 10 秒内没有 `drive.1`，就按第 5 步的止损做法停下（`save-log scenario4-battle`），把目录交给 agent。
   - 之后圣骑士的回合**用 Smite**（打谁都行，命中、未命中都会加护盾），至少用两次；让怪物打到圣骑士（用过 Smite 就有护盾；驱动时另有一个英雄也被加了护盾，打到他也算）。要看到一次护盾**全部挡下**（怪物的伤害小于护盾，生命不变）和一次**部分挡下**（伤害大于剩下的护盾，打不死他）。
   - **目测两件事**，结果记下来，最后写进交给 agent 的说明：
     - 有护盾的圣骑士**头顶的 tray 里有没有多出一个 buff 图标**（护盾的载体，数值 0），护盾打空或战斗结束后它是否消失。看不出来就写「没看到」。
     - 护盾全部挡下的那一下，**界面弹的是不是「Blocked」**（格挡）。
   - 让英雄**连打同一个怪物两次以上**（或者用伤害高的技能），把它的 10 点护盾打穿：检查脚本要一次怪物身上的部分吸收。每个怪物都有护盾、都有标记，打哪个、打死哪个都行。
   - 至少打满两个回合（第 2 回合开始时护盾的再生、衰减才结算），然后在**某个角色行动完之后**（游戏在每个角色的回合结束时存档）用游戏菜单退回主菜单，再进存档 A。战斗从存档恢复，**等 10 秒**（日志窗口里出现 `drive.1` 和 `round] where=battle`：读进来的这场也算这次进存档的第一场，`p4c_shield` 再驱动一次，`p4b_combat` 做战斗那一轮）。
   - **把这场战斗打完**（不要撤退），战斗结束后等 5 秒。护盾的全部、部分吸收在这两段里都没碰上的话，第 8 步之前再打一场完整的战斗补上（开战后照样先等 `drive.2`）。

7. **走廊里 `p4b_combat` 的一轮**：打完战斗后在走廊里停 5 秒（日志窗口里出现 `round] where=corridor`：这次进存档的走廊那一轮；刚才读进来的那场战斗它按战斗做过一轮）。

8. **第二场战斗：撤退**：往前走到下一场战斗。开战后**不攻击**，等日志窗口里出现 `battle.add`（这一场的标记，开战 1～2 秒）和 `drive.2`（这次进存档的第二场驱动，约 5 秒；第 6 步补打过一场的话这场不驱动，等 5 秒就行），然后**撤退**（撤退失败就下回合再撤）。回到走廊后等 5 秒。检查脚本把「没有怪物死亡就结束的战斗」当作撤退。

9. **回城**：在地图界面放弃探索（或者完成任务），走完结算界面回到城镇，等 10 秒（游戏回城时删掉队伍英雄身上的标记，`on_remove` 收到 `town_visit`）。

10. **读回城后的城镇存档**：退回主菜单，再进存档 A，**等 10 秒**。城镇标记在名册里 id 最小的英雄身上：他跟着出征了的话，回城时标记被清掉，`p4c_persist` 这时重新加两条（日志窗口里出现「城镇的载体加好了」）；没跟着出征的话标记还在（DDLua.log 写「城镇的载体还在」），两种都可以。然后**再招募一个英雄、等 5 秒、解雇他、等 5 秒**：让游戏把带标记的名册存下来，下一步改旁路时名册里要有带状态的标记，才能核对「标记的状态不恢复」。

11. **构造「旁路和存档不一致」**：退回主菜单，**游戏停在主菜单，等 5 秒**（让后台写盘写完：杀毒软件占着文件时 DDCore 会重试，晚到的写入会盖掉改过的旁路），在 Git Bash 里：

    ```
    python -B DD.ModLoader/tools/rig.py tamper-persist p4c_persist
    ```

    它把测试目录 `DDModLoader\save\<账号>\steam_profile_N\DDLua\p4c_persist.ddsave` 和上一代 `.prev` 头里记的名册哈希各改一位（原文件另存 `.before-tamper`），并在 `DDModLoader\log\rig_tamper.log` 记一行（带这次运行的运行标识）。游戏正在存档里时它会拒绝；上一次场景改过还没恢复时它也会拒绝，先按提示 `--restore`。然后在游戏里再进存档 A，等 10 秒：DDCore 报 `persist：DDLua/p4c_persist：mismatch`（WARN，两代的原因都写），`dd.store_status()` 是 `mismatch`，`p4c_persist` 的 `dd.store` 是空表，名册里的标记都是空状态。别的 3 个 Mod 的旁路没改，照常 `loaded`。读城镇存档后游戏通常马上自己存一次档（写 `persist.narration.json`，DDCore.log 里紧接着有一行 `campaign.save`），DDCore 这时把改过的两代改名为 `p4c_persist.ddsave.mismatch-<时间>`、写新的旁路，这是正常的；之后这一步**不要再做会让游戏存档的操作**（招募、解雇、升级建筑）。

12. **恢复旁路**：退回主菜单，再运行

    ```
    python -B DD.ModLoader/tools/rig.py tamper-persist p4c_persist --restore
    ```

    通常输出「没有要恢复的文件：改过的旁路已经被游戏归档或轮换掉……」，退出码 0：第 11 步读档后游戏马上存了一次档，改过的两代已经归档，测试目录里现在的旁路是游戏之后写的，不用恢复。万一游戏那次没有存档，它会把改过的两代恢复成原样，`rig_tamper.log` 多一行 `"restore": true`。

    想做可选部分 D（读到上一代）就在这里做，做完再接第 13 步。

13. **退出**：用游戏菜单退出游戏（不要关窗口、不要强杀，检查脚本要看到正常退出的那一行，没有就判失败）。

14. **保存日志并检查**：

    ```
    python -B DD.ModLoader/tools/rig.py save-log scenario4
    python -B DD.ModLoader/tools/p4_scenario_check.py --log-dir DD.ModLoader/build/rig_logs/scenario4
    ```

    `save-log` 的目标已存在时会拒绝，换一个名字（如 `scenario4-2`），检查命令里同步改。这时还没有第二段的日志，检查脚本最后一行是「没有触发：combat.stack」、退出码 2，这是预期的；别的项有「没有触发」或「失败」时先看下面「检查」一节。做完第二段后两个目录一起再检查一次，把那次的完整输出、第 6 步目测的两件事一起交给 agent。

## 第二段：combat 方案重跑 test_combat（另一次运行，必做）

为什么要另跑：`test_combat`（P4a 的 native 验证插件）的 `STACK` 核对在 F1 改成了按速度倍率核对（审核 A-S4），之前只有城镇那一轮在游戏里通过过；它要在城镇、走廊、战斗里各跑一遍。它的 `damage.final` 订阅会把每场头两次伤害改成全部、一半吸收，和必做部分的护盾冲突，`combat` 方案也不带 DDLua，所以不能和必做部分放在同一次运行里。

做完必做部分（游戏已经退出、`save-log scenario4` 做过）之后：

```
python -B DD.ModLoader/tools/rig.py deploy combat
python -B DD.ModLoader/tools/rig.py status
```

`status` 应显示「当前方案：combat」，`DDModLoader\cfg` 里有 `test_combat.cfg`。启动 `start.bat`，主菜单上等到 `DDModLoader\log\test_combat.log` 里出现 `SELFDONE ok=1` 再进**存档 B**（出现的是 `SELFDONE ok=0` 就不要进存档，从菜单退出游戏，`save-log combat-selfcheck` 后把这个目录交给 agent）（不要进存档 A：这个方案不写 DDLua 的旁路，在存档 A 上玩过之后，再在 scenario4 下读存档 A 就是 `mismatch`，见「选存档」）。没有存档 B 时用任何一个不用来跑必做部分的存档位（可以新建战役，走完教学关回到城镇再出征）：

1. 城镇里停 5 秒（`ROUND where=town`）。
2. 出征，进地牢后在走廊停 5 秒（`ROUND where=corridor`）。
3. 打一场战斗，开战后不操作等 5 秒（`ROUND where=battle`），之后让英雄挨两次打（第一次全部吸收、应显示 Blocked，第二次吸收一半）。第二次挨打的英雄血量别太低：这一下把他打进死门的话，生命损失等于原来的生命、不等于伤害减吸收量，`test_combat` 写 WARN，`combat.stack` 失败，要重跑这一段。
4. 打完战斗，退回主菜单，从菜单退出游戏。

```
python -B DD.ModLoader/tools/rig.py save-log combat
python -B DD.ModLoader/tools/p4_scenario_check.py --log-dir DD.ModLoader/build/rig_logs/scenario4 --combat-log-dir DD.ModLoader/build/rig_logs/combat
```

检查脚本的 `combat.stack` 核对 `test_combat.log` 和同目录的 `DDCore.log` 是同一次运行（运行标识、开始时间），三个场合的 `STACK`、`ROUNDDONE` 和两种 `DMGCHECK` 都是 INFO，整个文件没有 WARN。两段都通过时退出码 0。`save-log combat` 的目标已存在时换个名字（如 `combat-2`），检查命令里同步改。之后要再跑必做部分时先 `rig.py deploy scenario4`。

## 可选部分

- **A. 没有 Lua Mod 的存档（#6 的原版行为）**：第 13 步之前，在主菜单进**存档 B**（没有勾选任何 DDLua 开头的 Mod），城镇里等 5 秒，退回主菜单。检查脚本的 `hooks.nolua` 核对这次进存档不建 VM、`CampaignLoad` 的 hook 报告和主菜单相同。没有存档 B 时也可以另跑一次 `rig.py deploy lua`、进一个没有 Lua Mod 的存档（不要进存档 A）、`save-log lua-p4`，检查时加 `--lua-log-dir DD.ModLoader/build/rig_logs/lua-p4`（`build/rig_logs/lua` 是 P3 时的旧日志，不要用）。
- **B. 地牢里的 raid 不一致**：放在最后做（要做 D 的话先做 D：D 要在城镇里，做完 B 存档 A 停在地牢里）。第 12 步（和 D）之后另走一次探索，在走廊里走几步，退回主菜单，运行 `python -B DD.ModLoader/tools/rig.py tamper-persist p4c_persist --what raid`，再进存档：游戏在读档之前就读了探索存档，所以读档时 DDCore 就报 `mismatch`（原因是「当前一代：探索存档（persist.raid.json）的哈希不同（旁路 <改过的哈希>…）」），`p4c_persist` 写「通过 [persist.mismatch]」，怪物的状态丢弃。之后同样 `--restore`，然后在地图界面放弃这次探索回到城镇，再做第 13 步（下次重跑要从城镇开始）。
- **C. 战斗中读档后怪物的护盾**：第 6 步读档之前怪物身上还有护盾时，读档后怪物的护盾从载体恢复（`shield.persist`）。
- **D. 读到上一代（PREV）**：在第 12 步之后、第 13 步之前做（放在第 11 步之前会和它的改动互相影响）。
  1. 游戏停在主菜单，进存档 A，在城镇里等 10 秒，**什么都不做**（读城镇存档后游戏通常马上自己存一次档，旁路因此有了两代，两代记的名册相同），退回主菜单。
  2. `python -B DD.ModLoader/tools/rig.py tamper-persist p4c_persist --current-only`（只改当前一代）。
  3. 再进存档 A，等 10 秒：上一代和存档一致，DDCore 报 `persist：DDLua/p4c_persist：prev（上一代，……）：当前一代不一致（……），用上一代`（INFO），`p4c_persist` 写「信息 [store] status=prev」和「通过 [persist.town]」。检查脚本的 `persist.prev` 记下它，`persist.store` 拿读到的 saves 和这次运行里更早的那次存档核对（不是和最后一次比）。
  4. 退回主菜单，`python -B DD.ModLoader/tools/rig.py tamper-persist p4c_persist --restore`：读档后游戏又存了一次档，改过的当前一代轮换成了 `.prev`，它会跟着找到并恢复。然后做第 13 步。

  第 1 步里做了会写名册的操作（招募、解雇、和英雄有关的建筑操作）、或者那次游戏没有自己存档（只有一代）时，第 3 步读到的是 `mismatch`（原因里有改过的哈希，不算意外），`persist.prev` 没有触发，不影响别的判定；想补就从第 1 步再来一次（第 4 步的 `--restore` 照做）。
- **E. 死门**：战斗里有英雄在死门上又挨打时，`p4c_shield` 第一次写一半的吸收量、第二次写全部（`damage.deaths_door`）；全部的那一下应显示 Blocked、不判定死亡。圣骑士在死门上时没有护盾（`shield.zero`）。

## 检查

脚本先核对日志本身：`DDLua.log`、`DDCore.log` 属于同一次运行（开始时间、运行标识），晚于部署，方案是 scenario4，DDLua 是默认配置，4 个 Mod 都按标题找到并加载成功；`rig_tamper.log` 只用本次运行的记录（运行标识对得上，或者游戏没运行时改的、早于本次启动），`--restore` 之后的改动不再算预期的不一致。之后逐项输出「通过」「失败」「格式不认识」「没有触发」「需要目测」，「没有触发」会写还缺什么流程。退出码 0 表示全部必需项通过，1 表示有失败或格式不认识，2 表示没有失败但有必需项没有触发；「需要目测」不影响退出码。可选项没做是「没有触发」，不影响退出码；做了却判「失败」时退出码同样是 1（这时把输出交给 agent）。

| 检查项 | 判定 |
| --- | --- |
| `selfreport` | 4 个场景 Mod 写的任何一行「失败 [标签]」都判失败（审核 C-B1） |
| `shield.formula` | 按 SHIELD 行逐个会话重放每个角色的护盾：每一行的 `before` 必须等于上一行之后的状态；effect 名字按 DDShield 的 `ParseNameSpec` 独立解析（配置表 `specs` 优先），和 Lua 解析出的 op、数值、回合数对得上；获得、上限、加满、破盾、百分比、设定、再生、衰减、穿透用 DDShield 的规则独立重算；吸收量 = min(护盾, 最终伤害 × (1 − 穿透))，全部吸收的判定、写给 `damage.final` 的值、吸收后的护盾都要对；生命 ≤ 0 时清零、不在死门上吸收；第 1 回合不结算，之后每回合每个有状态的角色按 DDShield 的回合规则变化（没写 round 行的必须不变）；初始值按 `config.lua` 的职业表 |
| `shield.cap` | 每次变化后护盾不超过上限 |
| `shield.natural` | 至少一次 Smite 施加的 `shield_self_gain_5` 给执行者自己加护盾 |
| `shield.full` / `shield.partial` | 全部吸收：生命不变；部分吸收：生命损失 = 最终伤害 − 吸收量（打空生命的单独列出，只核对不为正） |
| `shield.monster` | 怪物目标的部分吸收：生命损失 = 最终伤害 − 吸收量 |
| `shield.ops` / `shield.round` | 上限、加满、破盾、获得、再生、衰减都用到了；至少一次回合结算带再生或衰减 |
| `shield.clear` | 战斗结束清掉护盾，载体 Buff 都删掉 |
| `shield.rule` | `shielded` 的每次结果和重放出来的护盾一致（有护盾时 `true`，没有时交还原来的判定），两种都出现过；`p4c_shield` 的「通过 [rule.live]」：remove_if_not_active 的探针在有护盾的英雄身上留下、在没有的身上被游戏删掉，`is_false_rule` 的反过来 |
| `shield.persist`、`shield.zero`（可选） | 读档后从载体恢复的护盾等于上一次进存档最后写进载体的；生命 ≤ 0 时护盾清零出现过 |
| `persist.town` / `persist.corridor` / `persist.battle` | `p4c_persist` 读档后逐条核对：条目在、id 和 guid 对得上、状态和存档时的快照相同（战斗中的存档还要有怪物的） |
| `persist.tag` | 读档后 DDCore 的标签也恢复了（`mine`、`tag`） |
| `persist.written` | 进存档期间观察到写名册（大小正常）就要派发 `campaign.save`；DDLua 交出数据的那次 DDCore 提交的旁路文件不能是 0；整次运行一次写入都没观察到判失败（审核 B-S9）。列出每次存档从回调到写出的最长耗时 |
| `persist.roster_ops`（可选） | 招募、解雇各自之后 5 秒内有没有存档 |
| `persist.store` | 每次读档时 `dd.store` 的 saves、最后一次存档的序号和场合：`loaded` 等于上一次进存档最后写的，`prev` 等于本次运行里更早写的那一次（不能是最后一次）；DDCore 和 DDLua 报的状态相同 |
| `persist.mismatch` | 本次运行改过的旁路读档时报 `mismatch`（原因里有改过的哈希），`p4c_persist` 确认 `dd.store` 是空表，名册里的标记至少一条、都是空状态（审核 C-Y5） |
| `persist.unexpected` | 除此之外没有不一致、`unavailable`，同一个存档位写过旁路之后没有读到 `empty`（新战役除外）；`prev` 不算。本次运行第一次读某个存档位就不一致时，提示这个存档可能在别的方案下进过、重跑前 `rig.py reset-persist` |
| `persist.prev`（可选） | 可选部分 D 读到 `prev` |
| `boundary.*` | 同 id 两条各自的状态；战斗结束游戏删掉的载体调用 `on_remove`；撤退时也一样（游戏撤退时不删的话记下来）；怪物的载体随战斗 `gone`；回城时 `town_visit` |
| `buff.reasons` | `buff.remove` 主题的原因和 `on_remove` 收到的一致；列出这次出现过哪些原因 |
| `queue` / `combat.writes` | 三个场合的排队写操作都在下一帧执行、`done` 拿到的 guid 就是那一条；立即添加、脚本 Buff、叠加、按标签和 guid 删除、施加 effect、规则结果、怪物上的增删在三个场合都通过 |
| `guid` | 读档后状态没有因对不上而丢弃；读档恢复的条目不派发 `buff.add` |
| `buff.deferred`（可选） | 读档时出现了加载线程上的 `buff.*` 延迟事件（游戏不一定这样做，没出现不算失败） |
| `crash` / `errors` | 没有 VM 损坏、规则回调崩溃，最后从菜单正常退出（没有「退到桌面」判失败）；DDLua、DDCore 没有意外的 WARN、ERROR |
| `effect` / `rule` / `rule.stats` | `p4b_combat` 的施加 effect 在城镇被拒绝、走廊和战斗里生效；`p4b_live` 在三个场合按 Lua 的结果留下或删掉条目，规则回调只读；每次退出存档的规则统计里单帧峰值不超过 `combat_frame_ms`、没有崩溃 |
| `hooks.menu` / `hooks.nolua`（可选） | 主菜单的 hook 报告是 1 + 4 个目标、语义 hook 0、订阅 0，进存档前 DDLua 不建 VM；没有 Lua Mod 的存档同上 |
| `damage.deaths_door`（可选） | 死门上部分吸收和全部吸收后生命都不变 |
| `combat.stack`（第二段，必需） | 见「第二段」；没给 `--combat-log-dir` 时没有触发 |
| `look.tray` / `look.blocked`（需要目测） | 第 6 步目测的两件事，检查脚本只列出载体增删和全部吸收的次数、时刻 |

有「没有触发」：按提示补流程。补流程要在新的一次运行里重走（检查脚本一次只看一次运行），存档 A 已经勾好了 Mod，从第 1 步开始；`save-log` 换一个名字。中间跑过第二段或别的方案、进过存档 A 的话，先 `rig.py deploy scenario4`，再 `rig.py reset-persist`（见「选存档」）。只缺 `combat.stack` 时不用重跑必做部分，做第二段就行。有「失败」：先看是不是部署、方案、旧日志的问题（`logs` 那一项），其余的把检查脚本的输出和日志目录交给 agent，不要改测试 Mod。

自测：`xmake run dd_lua_tests P4c2` 在假世界里按这份流程走一遍 4 个 Mod（含可选部分 D 和回城时的加载线程），时序按真实游戏：走廊里没有带角色的事件，开战后 6 秒才有第一次出手、之后一次一个角色，读地牢存档时先有一段加载，读进来的战斗不派发 `battle.start`，撤退那场一个怪物都没出手；预演还核对 `p4b_combat` 的走廊那一轮不在战斗里做。日志写到 `DD.ModLoader/build/p4c_rehearsal/`；`python -B DD.ModLoader/tools/p4_scenario_check.py --selftest` 用它检查一遍完整样本，再检查几十种篡改过的样本（含第二段、读到上一代、旁路落后的样本和两种退出码）。改了检查脚本或测试 Mod 都要重跑。

## 中途出问题

- **游戏卡死、崩溃**：先 `python -B DD.ModLoader/tools/rig.py save-log scenario4-crash-1` 保存日志，再按 09 的做法结束 `Darkest.exe`。把日志目录交给 agent。强杀、崩溃之后要重跑的话，先在游戏没运行时执行 `python -B DD.ModLoader/tools/rig.py reset-persist`：强杀时游戏排队的存档可能还没落盘，旁路却已经按快照写了（旁路比存档新），不重置的话重跑第一次读档就是 `mismatch`，`persist.unexpected` 失败。
- **读档时游戏提示 Mod 缺失或拒绝读档**（存档 A 里 P3 的测试 Mod 不在了）：改用新存档位或存档 B，见「选存档」。
- **`tamper-persist` 说已经改过**：上一次场景没恢复，先 `tamper-persist p4c_persist --restore`。它说某一代「头里的哈希是记过的改后的值，不再改」也没关系：那一代已经和存档不一致了（再改一位可能正好改回原值）。
- **DDCore.log 里有「旁路写盘失败，200 毫秒后重试」**（WARN）：多半是 360 之类的杀毒软件在扫描时占着文件，DDCore 会按 0.2、0.5、1、2 秒重试；检查脚本照样把它算作意外的 WARN（`errors` 失败）。先在杀毒软件里把测试目录加入信任，再重跑；之后出现「放弃」（ERROR）或者不是杀毒软件的原因，把日志交给 agent。
- **读档时 `p4c_persist` 是 `mismatch`，但这次运行没有 `tamper-persist`**：存档 A 在别的方案下进过（见「选存档」），游戏存档往前走了、旁路落后。退出游戏，`rig.py reset-persist`，从第 1 步重来。
- **不想继续**：`python -B DD.ModLoader/tools/rig.py deploy lua` 换回没有测试 Mod 的方案，测试 Mod 的目录（含生成的圣骑士职业文件）会被删掉；存档里的勾选不会自动取消。

## 日志里的关键行（给 agent 核对）

- `DDLua.log`：`[shield_lite] SHIELD {...}`（`ev` 是 init、apply、absorb、hit、zero、round_begin、round、endhit、clear、carrier、save、restore、rule、battle）；`[p4c_persist] 信息 [store] status=…`（每次进存档）、`信息 [save] #k where=… saves=N expect=M`（每次游戏存档）、`信息 [roster] hero.recruit|dismiss`、`通过 [persist.town|corridor|battle]`、`通过 [stack]`、`通过 [bend.remove]`、`通过 [bend.retreat]`、`通过 [monster.gone]`、`通过 [town_visit]`、`信息 [town_visit.stale]`（之前不一致的读档丢了状态的旧载体，回城时被游戏删掉，不判定）、`通过 [remove.match]`、`通过 [restore.noadd]`、`通过 [persist.mismatch]`；`信息 [battle] 第 N 场战斗是读档恢复的`、`信息 [battle.add] …`、`信息 [bend.gone] …`（带 p4c_bend 的英雄不在名册里了，不判定）；`[p4c_shield] 信息 [drive] …`、`通过 [drive.N]`、`信息 [drive] 第 N 场战斗在驱动中结束了…`（这场不算，下一场再驱动）、`通过 [rule.live]`、`信息 [deathsdoor] …`；`[p4b_combat] 信息 [round] where=…`、`通过 [rounddone.*]`、`[queue.done.*]`、`[effect.*]`、`[rule.live.*]`、`[monster.remove.battle]`（怪物在这一轮里死了时是一行「信息」，换一个怪物再做战斗那一轮）、`信息 [buff.deferred]`、`信息 [summary]`；`[存档#N] 脚本 Buff 状态（英雄|怪物）：从旁路恢复 N 条，丢弃 0 条`；`[存档#N] campaign.save #k（…）：写出 N 个 Mod`；`[<mod>] dd.store：loaded|prev|mismatch（…）`。
- `DDCore.log`：`persist：观察到写存档：设备 steam_profile_N，persist.roster.json，<几万> 字节，…（进存档后第一次写名册）`、`persist：进存档：设备 steam_profile_N，…，存档在城镇|地牢里，raid <哈希>（读档前读到）`、`persist：DDLua/<mod>：loaded|prev|mismatch…`、`persist：DDLua/<mod>：raid 核对一致`、`persist：campaign.save #k（…）：…；提交旁路文件 N 个`、`persist：标签表恢复英雄|怪物的 N 条`、`规则统计（退出存档时）：…崩溃 0 次，…单帧峰值 X us / N 次`、最后一行附近的 `persist：退到桌面：后台写盘已写完（…失败 0 次，重试 0 次）`；主菜单的 `hook 报告（AssetsReady），共 5 个目标` 和 `persist：观察 hook 4 个（已装）`。
- `rig_tamper.log`：每次 `tamper-persist` 一行 JSON（时间、运行标识 `run`、key、改了哪两个文件、旧哈希 → 新哈希、`current_only`；恢复时 `"restore": true`；`--restore` 没有要恢复的时候不记）。

## 审核要的运行证据在哪里

| 证据（审核报告「需要的运行证据」） | 场景的哪一步 | 检查项 |
| --- | --- | --- |
| A1 当前构建的 combat 写路径：城镇、走廊、战斗的立即添加、事件回调里排队、按标签和 guid 删除、怪物上的增删、施加 effect、操作回调的 guid | 2、5、6、7（`p4b_combat` 每次进存档每个场合一轮） | `combat.writes`、`queue`、`effect` |
| A2 加载线程上的 `buff.*`：走廊和战斗中读档，延迟事件、读档恢复的条目不派发 `buff.add`、没有崩溃 | 5、6 的读档 | `buff.deferred`（可选）、`guid`（restore.noadd）、`crash` |
| A3 `damage.final`：全部吸收显示 Blocked；死门上的部分、全部吸收；怪物目标 | 6；可选 E | `look.blocked`、`shield.full`、`shield.monster`、`damage.deaths_door`（可选） |
| A4 `buff.remove` 的各种原因 | 全程 | `buff.reasons`（列出出现过的；trinket、burn_decay 之类要换饰品、灼烧才会有，不强求） |
| A5 规则判定的单帧峰值 | 每次退出存档 | `rule.stats` |
| A6 撤退、战斗结束、回城时载体的删除和 `on_remove` | 6、8、9 | `boundary.battle_end`、`boundary.retreat`、`boundary.town_visit`、`buff.reasons` |
| A-S4 `test_combat` 按倍率核对 | 第二段（必做） | `combat.stack` |
| B1 写存档被观察到（大小正常）、`campaign.save` 派发、退出存档时写入次数大于 0 | 2 起每次存档 | `persist.written` |
| B2 回城那次存档记成城镇，读档 `loaded` | 9、10 | `persist.town`（第二次）、`persist.unexpected` |
| B3 读地牢存档：存档在地牢里、raid 核对一致、怪物状态恢复 | 5、6 的读档 | `persist.corridor`、`persist.battle` |
| B4 退到桌面时后台写盘写完 | 13 | `crash`、`errors` |
| B5 每次 `campaign.save` 的耗时 | 全程 | `persist.written` 的说明行 |
| B6a PREV | 可选 D（第 12 步之后） | `persist.prev`（可选）、`persist.store` |
| B6b 旁路真的落后、B6c 刚存档就退到桌面 | 没有放进场景 | 6b 要手工复制旁路文件、改回去，造出的不一致检查脚本分不清是不是缺陷（会判 `persist.unexpected` 失败）；6c 的结果要在下一次运行读档时才看得到。两项都需要单独的运行和人工判断，留给主线需要时再做 |
| C1 tray 里看到「有护盾」 | 6 | `look.tray`（需要目测） |
| C2 全部吸收显示 Blocked | 6 | `look.blocked`（需要目测） |
| C3 规则真的按 Lua 的结果生效，三个场合 | 2、5、6、7 | `rule`、`shield.rule` |
| C4 圣骑士在死门上时护盾的表现 | 可选 E | `shield.zero`（可选） |
| C5 规则统计的单帧峰值和总耗时 | 每次退出存档 | `rule.stats` |
| C6 战斗中读档后怪物护盾从载体恢复 | 可选 C | `shield.persist`（可选） |
| C7 招募、解雇是否各自写名册 | 2、10 | `persist.roster_ops`（可选） |
| F2 的观察点：改 raid 后读档就是 mismatch、不一致之后第一次存档前归档 | 可选 B；11（第 11 步之后游戏存过档时） | `persist.mismatch`；DDCore.log 的归档行 |

## 单独跑的方案

P4 另外三个方案，排查和写新探针时用，部署、启动、`save-log` 的做法同上：

- **`combat`**（DDCore + `test_combat`）：就是「第二段」。`DDModLoader\log\test_combat.log` 每行 `[f<帧号>] <标签> …`，不符合预期的都是 WARN：主菜单 `SELFDONE ok=1`（订阅、注册规则后 hook 装上，撤销后拆除）；进存档 `DEFINE`、`RULE`（重新定义游戏数据 `SPD5`、`rakebuff_5` 返回 13）；城镇、走廊、战斗各一轮 `ROUND where=…`、`STACK`、`EFFECT`（城镇返回 12）、`QUEUE` 和下一帧的 `OPRESULT`、`ROUNDDONE`（剩下 0 条）；战斗里 `DMGFINAL`、`DMGCHECK kind=full|partial`；回城、退出存档时 `SUMMARY`。`DDModLoader\cfg\test_combat.cfg` 写 `active = 0` 只做被动记录。
- **`p4b`**（DDCore + DDLua + `p4b_combat`）：scenario4 里的 `p4b_combat` 单独跑。存档里勾选「DDLua Test P4b Combat」，城镇、走廊、战斗（开战后不操作）各停 5 秒，看 `DDLua.log` 里的「通过 [rounddone.<场合>]」和 `信息 [summary]`；`grep -a "p4b_combat\] 失败" DDLua.log` 应为空。数据目录写 `DDModLoader\data\p4b_combat\last_run.json`。
- **`probe4`**（DDCore + `probe_p4`，不带 DDLua）：P4 逆向阶段的探针，留作 P5 探针的参考。用法写在 `tests/plugins/probe_p4.cpp` 开头，日志用 `tools/p4_probe_report.py` 分析。

## 没有在游戏里确认的事

两次场景之后还剩：

- Lua 施加的 effect 和数据层的 `.on_miss false`：`e:apply` 走游戏的 `ApplyEffect`，`p4c_shield` 用到的 `shield_damage_5`、`shield_heal_pct_25` 是 `.on_miss false`，是否生效以游戏为准（检查脚本不要求它们）。
- 游戏对启用列表里不存在的 Mod 的处理：两次场景的存档启用列表都是齐的（「启用 5 个 Mod，0 个没有找到目录」），没有碰到。
- 队伍的顺序（换位后按站位）只有静态依据，测试 Mod 和检查脚本都不依赖顺序。
- 可选部分 A、B、D、E 没做（没有 Lua Mod 的存档、raid 不一致、读到上一代、死门，即 `hooks.nolua`、`persist.prev`、`shield.zero`、`damage.deaths_door`）；加载线程上的延迟 `buff.*`（`buff.deferred`）两次都没有出现。

原来列在这里、已经有运行证据的：招募、解雇各自在同一帧写名册（`persist.roster_ops`）；战斗结束和撤退时游戏都删掉 `remove_on_battle_complete` 的 buff，`buff.remove` 和 `on_remove` 的原因都是 `predicate`；载体 Buff（数值 0）在 tray 里有图标（用户目测）；队伍表在走廊、战斗里读得到，读进战斗中的存档时 `dd.actor.in_battle()` 为真（补跑那段）；战斗中读档后英雄和怪物的护盾从载体恢复（可选 C，补跑那段的 `shield.persist`）。
