# Lua 运行时的场景验收

agent 只能做到主菜单自检，进存档、地牢、战斗的验收项由用户按这份流程操作一次，日志作证据（[ADR 0016](adr/0016-test-rig-and-manual-scenarios.md)）。P3 在 2026-11-11 按这份流程验收通过：检查脚本 29 项，通过 28，唯一的可选项没有触发。后续阶段扩充 Lua 能力时，在这里加场景步骤，在 `tools/p3_scenario_check.py` 和 `tests/lua/scenario/` 加对应的检查。

场景分两次游戏运行：

1. **scenario 方案**：DDCore、DDLua、只读探针 `probe_p3c` 和 5 个测试 Lua Mod，在存档 A 里走完大部分验收项；
2. **lua 方案**：只有 DDCore 和 DDLua，进一个没有 Lua Mod 的存档 B，对照验收 #7 的口径（1 个目标、语义 hook 0、订阅 0）。

游戏每次启动都会重写日志，所以每次退出游戏后先用 `rig.py save-log` 把日志复制出来，最后用一条命令检查两份副本，把输出交给 agent。

所有命令都在 guest 的 Git Bash 里执行，工作目录 `D:\workplace`。

## 这次要拿到的证据

检查脚本 `tools/p3_scenario_check.py` 逐项判定，括号里是简报验收清单的编号：

| 验收项 | 由谁产生 | 需要的游戏流程 |
| --- | --- | --- |
| 示例 Mod 跑通（#1） | `hello_lua`（标题 DDLua Example Hello） | 进存档 A，打一场战斗 |
| `hit.pre`、`skill.pre`、`heal.pre`、`stress.pre` 的写入，`damage.pre` 的取消 | `p3d_core`（DDLua Test P3d Core） | 战斗里：怪物命中英雄、英雄受伤至少两次、治疗一个没满血的英雄、英雄加一次压力 |
| 一个 Mod 报错不影响其他（#5） | `p3d_faulty`、`p3d_broken`，由 `p3d_core` 核对 | 打完一场有 3 次以上伤害的战斗 |
| 死循环被预算打断、游戏不卡死（#3） | `p3d_faulty` | 第一场战斗开始后再过 2 秒以上 |
| 切换存档时拆除和重建（#6） | DDLua 自己的日志 | 回主菜单，再进一次存档 A |
| 句柄跨回调有效、战斗结束后失效、actor 作用域撤销、`wait_event` 快照（#8） | `p3c_actor`（DDLua Test P3c Actor）和探针 | **同一次探索里打两场战斗**（任何一个英雄挨一次打就有快照）；再进一次存档 |
| 同一次探索的下一场战斗里，上一场的怪物句柄解析不到同 guid 的新怪物（#8） | `p3c_actor` 和探针 | 同上，而且两场要在**加载了测试 Mod 的那次进入**里（第 3 步之后）：游戏每场战斗从同一组 guid 重新编号，第二场的怪物 guid 会和第一场的重复 |
| `scope="raid"` 撤销 | `p3d_core` | 在同一次进入存档里结束这次探索 |
| 延迟事件的指针字段为 nil（#9） | `p3d_core` | 从城镇出发一次探索 |
| 没有 Lua Mod 时 `CampaignLoad` 的 hook 报告（#7） | 第二次运行的 `DDCore.log` | lua 方案下进存档 B |
| 启用 Mod 列表解析 | `DDCore.log`、`DDLua.log` | 进存档 A |

另外还检查：三份日志属于同一次运行、晚于部署、DDLua 是默认配置；订阅核对一致（#2）；`scope="battle"` 撤销；回调返回后负载失效；`campaign.load` 能收到；加载顺序（`p3d_faulty` 依赖 `p3d_core`）；探针收到的战斗、治疗、技能事件 Lua Mod 也收到了；日志里没有意外的错误。

## 测试 Mod 会改什么

**只有 DDLua Test P3d Core 改玩法**，每次进入存档各一次（退回主菜单再进来会再来一遍）：

- 英雄受到的第一次伤害（2 点以上、而且原值打不死他）减半，之后的下一次伤害取消（按 0 结算）。英雄有受到伤害的百分比修正、减半的效果判不了时，下一场再减半一次，每次进入存档最多 3 次；
- 第一次命中英雄的攻击改成未命中；
- 第一次给没满血的英雄的治疗改成 0（这次治疗没有效果）；
- 第一次让英雄加压力的变化改成 0。

DDLua Test P3d Faulty 不改游戏数据，但第一场战斗开始时会有三次约 50 毫秒的卡顿（故意的死循环被预算打断），`DDLua.log` 里会有大量它故意写的 ERROR。DDLua Test P3d Broken 故意加载失败。DDLua Example Hello、DDLua Test P3c Actor 和探针只写日志。

## 准备

### 1. 部署

```
source DD.ModLoader/tools/env.sh
python -B DD.ModLoader/tools/rig.py deploy scenario
python -B DD.ModLoader/tools/rig.py status
```

`deploy scenario` 部署 DDCore、DDLua、只读探针 `probe_p3c`，并把 5 个 Lua Mod 复制进**测试目录**的 `mods`（真实安装里没有）。部署前它会确认测试目录本身不是联接、实际位置不是真实安装也不和它互相包含（`DD_RIG_NAME` 只接受 `DarkestDungeon_` 开头的一层目录名），测试目录的 `mods`、`DDModLoader` 等目录也不是指向别处的联接；`.rig.json` 里记的上次部署的文件必须是某个方案确实会部署的、测试目录内的相对路径，否则整个拒绝，什么都不动；要覆盖的文件（`dbghelp.dll`、`start.bat`、`.rig.json`、插件 DLL 等）是符号链接或硬链接时，先移除测试目录里的这个名字再写新文件，链接另一端的文件不变。测试目录里不是它生成的 `DDLua.cfg` 会被改名备份成 `DDLua.cfg.<时间>.bak`（DDLua 要用默认配置）。这些检查防的是手工操作和以前留下的状态，部署时不要同时运行别的会改测试目录的程序。`status` 应当显示：

```
当前方案：scenario
部署时间：<部署的时间>（deploy 记录）
Lua Mod：mods/DDLuaTest_hello_lua（标题 DDLua Example Hello）
Lua Mod：mods/DDLuaTest_p3c_actor（标题 DDLua Test P3c Actor）
Lua Mod：mods/DDLuaTest_p3d_core（标题 DDLua Test P3d Core）
Lua Mod：mods/DDLuaTest_p3d_faulty（标题 DDLua Test P3d Faulty）
Lua Mod：mods/DDLuaTest_p3d_broken（标题 DDLua Test P3d Broken）
DDModLoader\cfg：DeathBlowCap.cfg、ProtectionCap.cfg
dbghelp.dll：<SHA1>
游戏运行中：否
真实安装未被改动（基线 2 个文件，另查了 rig 会写的 28 个位置、DDLua.cfg 的备份和 mods 下的 Lua Mod 目录）
```

`DDModLoader\cfg` 那一行里不能有名字正好是 `DDLua.cfg` 的文件（`DDLua.cfg.<时间>.bak` 是 deploy 改名备份的旧配置，DDLua 不读它，可以留着）。换成别的方案（例如 `rig.py deploy lua`）时这 5 个 Lua Mod 目录会被整个删掉。

`status` 多出以「`.rig.json` 的记录有问题」「测试目录里的 … 是硬链接」「… 是联接或符号链接」开头的行，或者最后一行不是「真实安装未被改动」（例如「真实安装的 … 不存在」「… 有 2 个硬链接」）时，不要开始场景，把 `status` 的输出交给 agent。

### 2. 选存档位

测试目录和真实安装共用同一份 Steam 存档（`Steam\userdata\<账号>\262060\remote\profile_N`，Steam 云会同步）。要用两个**空存档位**：

- **存档 A**：第一次运行里新建战役，勾选 5 个测试 Mod；
- **存档 B**：第二次运行（lua 方案）里新建战役，不勾选任何测试 Mod，专门用来对照 #7。

2026-11-11 查看时 guest 上已有存档的是 `profile_0`、`profile_1`、`profile_2`、`profile_9`，`profile_3` 到 `profile_8` 是空的；游戏界面上的第几个存档位对应哪个 `profile_N` 没有确认。

没有第二个空存档位时，也可以在第二次运行里直接进存档 A。这时测试 Mod 的目录已经被 lua 方案删掉，DDLua 会把它们当作「没有找到目录」跳过，检查脚本照样能判 #7；**但游戏读到启用列表里的 Mod 不存在时怎么处理（提示、忽略、还是拒绝读档）没有确认**，所以首选存档 B。

用了之后的后果：

- 存档 A 的启用 Mod 列表会记下这 5 个测试 Mod 的标题。真实安装里没有这些本地 Mod，以后用真实安装打开这个存档时游戏怎么处理没有确认（同上）。
- DDLua Test P3d Core 改过的战斗结果会存进存档 A。
- 存档会被 Steam 云同步。
- 测完不想留，可以在游戏的存档界面删掉这两个存档位。想更保险，开始前把 `remote` 目录整个复制一份备份。
- 不要在测试目录里打开正在玩的存档：测试目录卸载了 DDShield 等和框架冲突的 Mod，这和本次场景无关，但一样会影响那个存档。

### 3. 在哪里勾选测试 Mod

暗黑地牢的 Mod 是按存档启用的，本地 Mod 在列表里按 `project.xml` 的标题显示。存档 A 要勾选的 5 个标题：

- `DDLua Example Hello`
- `DDLua Test P3c Actor`
- `DDLua Test P3d Core`
- `DDLua Test P3d Faulty`
- `DDLua Test P3d Broken`

**以下界面描述没有在游戏里确认，以实际界面为准**：新建战役时游戏会给出 Mod 选择列表；已有的存档在存档选择界面的存档位上有管理 Mod 的按钮，打开后是带勾选框的列表。已安装的 Mod 有两三百个，找不到时按标题里的 `DDLua` 找。勾选顺序随意（DDLua 按依赖排序）。新战役是不是默认勾选全部 Mod 也没有确认；存档 B 新建时看一眼，不要勾任何 `DDLua` 开头的 Mod（lua 方案下它们已经不在列表里了）。

## 第一次运行：scenario 方案（存档 A）

整个第一次运行要在**一次游戏运行**里走完：重建、同一次探索里的两场战斗都要在同一份日志里。中途必须重启时，看下面「中途出问题」。

1. **启动游戏**。先确认没有别的 `Darkest.exe` 在运行（`rig.py status` 显示「游戏运行中：否」），再运行测试目录里的 `start.bat`：

   ```
   (cd "/d/Program Files (x86)/Steam/steamapps/common/DarkestDungeon_Dev" && cmd //c start.bat)
   ```

   也可以在资源管理器里双击这个目录里的 `start.bat`。不要从 Steam 启动，那是真实安装。不要用 `rig.py run --seconds N`：它到时会强杀游戏，可能正好碰上写存档。

2. **新建战役**：在存档 A 的空存档位新建战役，Mod 选择里勾选上面 5 个。新战役开头可能是一段教学关（印象里是强制的、带战斗，没有确认），走完进城。
   新建战役这一次进入能不能加载 Lua Mod 没有验证过（游戏为新战役准备启用列表的路径没有核实），没加载也不要紧，下一步会再进一次。教学关里的战斗不算数：检查脚本只认加载了 `p3c_actor` 的那次进入里的两场战斗。

3. **回主菜单，再进存档 A**（读已有存档是核实过的路径）：用游戏菜单退回主菜单，再从存档界面进入刚才的存档。

4. **出发一次探索，在这一次探索里打两场战斗，然后结束这次探索**。这一步全程不要回主菜单（第 3 步重新进入之后测试 Mod 已经加载，两场战斗要在这一次进入里）：
   - 在城里准备队伍，最好带一个能治疗的英雄（维斯塔修女的治疗技能等），选一个短的探索任务出发。
   - 在**同一次探索**里打完至少两场战斗，两场之间不要回城、不要退回主菜单。战斗里要出现：怪物命中英雄、英雄受伤至少两次、**给一个没满血的英雄治疗一次**、英雄加一次压力（怪物的压力攻击、被暴击都可以）。
   - 每场战斗结束后等 3 秒以上再做下一件事（作用域撤销在战斗结束 2 秒后检查）。
   - 第二场战斗用来验证怪物句柄跨战斗失效：游戏每场战斗都从同一组 guid 给怪物编号，第二场的怪物 guid 会和第一场的重复，`p3c_actor` 和探针在第二场开始时检查上一场的句柄解析不到它们。两场分在两次探索里时这一项是「没有触发」。
   - 有英雄阵亡没有关系：阵亡、解雇过的英雄不计入「英雄句柄仍有效」的检查（`p3c_actor` 和探针自己排除，日志里写「不计阵亡、解雇的 N 个」）。
   - 打完两场后**结束这次探索**：完成任务，或者在地图界面放弃探索，走到结算界面、回到城镇，再等 3 秒。放弃探索会不会派发 `raid.finish` 没有在游戏里确认，检查脚本报 `scope.raid` 没有触发时，下次改成完成一个短任务。
   - 记得 P3d Core 会让一次命中变成未命中、一次伤害减半、一次伤害取消、一次治疗无效、一次加压力无效。

5. **回主菜单，再进一次存档 A**（验证拆除和重建）：在城镇里用游戏菜单退回主菜单，再进这个存档，进去后至少等 5 秒（重建后的 Mod 要写出 `campaign.load` 和 3 秒定时器的日志）。

6. **回到主菜单，用游戏菜单正常退出游戏。**

7. **保存这次的日志**：

   ```
   python -B DD.ModLoader/tools/rig.py save-log scenario
   ```

   它把测试目录的 `DDModLoader\log` 复制到 `DD.ModLoader/build/rig_logs/scenario`，保留文件的修改时间，并记下当时的方案和部署时间（检查脚本用它们判断日志是不是这次的）。目标目录已存在时它会拒绝，换一个名字即可，不会覆盖，也不会往里面再套一层。

## 第二次运行：lua 方案（存档 B，验收 #7）

8. **换成 lua 方案**（测试 Mod 目录和探针会被删掉）：

   ```
   python -B DD.ModLoader/tools/rig.py deploy lua
   python -B DD.ModLoader/tools/rig.py status
   ```

   `status` 应显示「当前方案：lua」，没有 `Lua Mod：` 行，`DDModLoader\cfg` 里没有 `DDLua.cfg`。

9. **启动游戏**（同第 1 步的 `start.bat`），在存档 B 的空存档位**新建战役，不勾选任何 Mod**，走到城镇；回主菜单，**再进一次存档 B**，在城里等几秒；回主菜单，正常退出游戏。

10. **保存这次的日志**：

    ```
    python -B DD.ModLoader/tools/rig.py save-log lua
    ```

## 检查

```
python -B DD.ModLoader/tools/p3_scenario_check.py --log-dir DD.ModLoader/build/rig_logs/scenario --lua-log-dir DD.ModLoader/build/rig_logs/lua
```

脚本先核对日志本身，再逐项判定：

- **日志是不是这次的**：三份日志的开始时间相差不超过 30 秒，运行标识（DDCore 写在 `DDCore.log` 和每个插件日志的第一行，形如 `运行标识 20261111-102632-31344`，本地启动时间加进程 id）必须一致，都晚于最近一次部署；DDCore 加载的插件和方案一致（scenario：DDLua、probe_p3c；lua：只有 DDLua）；DDLua、DDCore、探针记下的每次进存档能一一对上。对不上就是读到了旧日志或混了别的运行，判「失败」。
- **DDLua 是默认配置**：配置行必须是「没有 DDLua.cfg，用默认值」且 `dev_mode=0`。
- 每一项输出「通过」「失败」「格式不认识」「没有触发」或「没有日志」，「没有触发」会写还缺什么流程。探针的日志格式和脚本对不上时判「格式不认识」，不会当作通过。
- **场景 Mod 的日志没有被限速**：DDLua 每个 Mod 每秒最多写 200 行（`log_lines_per_sec`），超出的丢弃并补一行「日志限速：有 N 行日志没有写入」；DDLua 替 Mod 写的回调错误和 traceback 也算在这个 Mod 头上。一次进存档里和 Mod 有关的日志总量到了 `log_max_mb`（默认 32 MB）之后也不再写入。场景 Mod 的判定行都按事件计数，p3d_faulty 故意的错误连 traceback 一共几十行（连续出错 3 次就停用），都远低于这些上限；出现限速行或总量行说明判定行可能丢了，`errors` 判失败。

最后一行是汇总。退出码 0 表示全部必需项通过，1 表示有「失败」或「格式不认识」，2 表示没有失败但有必需项没有触发。

- **通过**：汇总里失败 0、格式不认识 0、必需项没有触发 0（退出码 0）。唯一的可选项 `hooks.nolua`（scenario 方案里有没有进过不带 Lua Mod 的存档）不影响通过。
- 有「没有触发」：按提示补流程。漏勾了某个测试 Mod 也算这一类：在存档 A 的 Mod 列表里补勾，回主菜单再进一次。检查脚本一次只看一次游戏运行，所以补流程等于把第一次运行从第 1 步再走一遍（存档 A 已经有了，第 2 步跳过）；已经换成 lua 方案的先 `rig.py deploy scenario`。保存时换一个名字，例如 `save-log scenario-2`。
- 有「失败」或「格式不认识」：先看失败的是不是 `logs` 或 `hooks.lua`、说明里有没有提到「部署」「方案」「旧日志」：这多半是操作顺序的问题，例如先 `deploy lua` 再 `save-log scenario`（存下的是 lua 方案的日志），或者从 Steam 启动了真实安装（测试目录的日志没有更新）。这种情况按说明换一个名字重走那一次运行。其余的失败，把检查脚本的完整输出和两个日志副本目录交给 agent，不要再改测试 Mod。

只走完第一次运行时也可以先检查（不带 `--lua-log-dir`），这时 `hooks.lua` 是「没有触发」。

把检查脚本的输出交给 agent。agent 还可以核对 `probe_p3c.log` 里的最大生命和游戏界面是否一致：战斗里把鼠标放在英雄血条上，记下一两个英雄界面显示的「当前/最大生命」一起交给 agent（可选：知识库里最大生命的读法 `4C3F20(Actor+0x768, [Actor+0x790])` 已经和独立调用对照过，还没有和界面对照）。

没进过存档时（例如只到主菜单），除了「日志文件」和「没有意外的错误」，必需项都是「没有触发」。日志目录里没有 `DDLua.log` 时输出「[没有日志] 日志文件」并以退出码 2 结束。部署之后还没运行过游戏就检查测试目录（不带 `--log-dir`）时，「日志文件」会判失败并说明是旧日志。

## 中途出问题

- **游戏卡死**（超过几秒没有反应，不是 50 毫秒级别的卡顿）：卡死本身就是失败。用任务管理器结束 `Darkest.exe`，或者在 Git Bash 里：

  ```
  taskkill //IM Darkest.exe //F
  ```

- **崩溃、卡死或必须重启游戏时**，重启之前先保存日志，每次用一个新名字：

  ```
  python -B DD.ModLoader/tools/rig.py save-log scenario-crash-1
  python -B DD.ModLoader/tools/p3_scenario_check.py --log-dir DD.ModLoader/build/rig_logs/scenario-crash-1
  ```

  把这份输出和副本目录一起交给 agent。重启后接着走的那部分是另一份日志，检查脚本一次只看一次运行，要重建、同一次探索里两场战斗这类项目在新的一次运行里重新走。
- 不想继续时，`python -B DD.ModLoader/tools/rig.py deploy lua` 换回没有测试 Mod 的方案，测试 Mod 目录会被删掉；存档里的勾选不会自动取消。

## 日志里的关键行（给 agent 核对）

- `DDLua.log`：`[存档#N] 进入存档：启用 X 个 Mod`、`[<id>] 发现 Lua Mod … 来源 <标题>（mod_local_source）`、`加载顺序：`、`加载完成：成功 4 个，共 5 个 Lua Mod`、`[p3d_core] 通过 [标签] …`、`[p3d_faulty] … 超出执行预算 …`、`订阅核对（拆除后）：… 0 个`、`分配器残留 0 字节`、`[DDLua] 没有 Lua VM 了，撤销 lifecycle TICK`。lua 方案那次：`[存档#N] 没有可加载的 Lua Mod，不创建 VM`。
- `DDCore.log`：第一行 `DDCore 1.0.0 启动，运行标识 …`、`插件 DDLua 加载成功`（scenario 方案还有 `插件 probe_p3c 加载成功`）、`新战斗：战斗序号 N（探索序号 M，…，actor 服务调用时发现）`（每场战斗一行，同一次探索里递增）、`启用 Mod 列表 N 项（已安装 M 个），找到目录 K 项`、`hook 报告（CampaignLoad）` 后面的 `事件系统：已注册 listener … 订阅 N 个`、`进入 as_raid_start（新建 Raid），探索序号 N`。lua 方案那次 `CampaignLoad` 的报告应为「共 1 个目标」「已注册 listener 0 个，语义 hook 0 个，订阅 0 个」。
- `probe_p3c.log`：`battle.start 第 k 场：探索序号 N，战斗序号 B`（同一个 N 下至少两场，B 递增），第二场起 `上一场战斗的怪物句柄 m 个，仍能解析 0 个（应为 0），其中 guid 和本场怪物相同 j 个`（j 应大于 0）；`battle.end 第 k 场：… 本存档英雄句柄 h 个，仍能解析 h 个（不计阵亡、解雇的 …）`；`==== 汇总（退出存档）====` 后三行；第二次进存档的 `上一个存档的英雄句柄 N 个，仍能解析 0 个`。指针字段的逐条日志每次进存档最多 60 行，每场战斗开始时至少再给 20 行，所以每次进存档都有样本。
- `DDLua.log` 的 `[p3c_actor]`：第二场起 `通过 第 k 场（探索 N，战斗 B）：上一场的怪物里和本场怪物 guid 相同的 j 个；旧句柄失效 j/j，== 不相等 j/j，不是同一个对象 j/j，表键分开 j/j，新句柄驻留 j/j`（N、B 和探针那一行对上）；`通过 wait_event 快照的 target 就是保存的英雄句柄`（等的是第一个在 `damage.pre` 里挨打的英雄）。`[p3d_core] 信息 [damage.halve] … 判不了，下一场再试` 表示减半的那个英雄有受到伤害的百分比修正，下一场会再减半一次。减半的效果用这个英雄同一段里其余伤害估出的倍率判断：战斗里的减半只用本场的伤害，走廊里的减半只用上一场战斗结束之后的走廊伤害（倍率的增减益常在战斗结束时到期）。没有可用的伤害时，掉血正好是写入值就判通过，否则判不了。

检查脚本用离线预演的日志自测：`python -B DD.ModLoader/tools/p3_scenario_check.py --selftest DD.ModLoader/build/p3d_rehearsal`（预演由 `dd_lua_tests` 生成，lua 方案的运行用脚本内置的样本）。改了检查脚本或场景 Mod 都要重跑。

## 没有在游戏里确认的事

- 新战役是不是强制教学关；新建战役第一次进入时启用列表怎么准备（所以第 3 步要求回主菜单再进一次）。
- Mod 选择界面的位置、新战役默认勾不勾选全部 Mod；`<PreviewIconFile/>` 为空的本地 Mod 会不会出现在列表里。
- 存档位和 `profile_N` 的对应。
- 启用列表里的 Mod 不存在时游戏怎么处理（影响「用存档 A 做第二次运行」和以后用真实安装打开存档 A）。
- 放弃探索会不会派发 `raid.finish`。
- 测试 Mod 改结算会不会卡住教学关里脚本化的步骤。
- 撤退后重进同一格、读档恢复的战斗：静态上游戏不派发 `battle.start`（`knowledge/dd1/combat.md` 的「当前战斗」），这一场里 `p3c_actor`、`p3d_core`、`hello_lua` 的「战斗开始」行不会出现，探针的 `battle.start 第 k 场` 也没有；DDCore 的「新战斗」行照样有。流程里不要撤退，碰上了再多打一场正常的战斗。
- 解雇英雄的事件和删除是不是总在一起（没有查清）：`HeroDismissedEvent` 在 `6B46B0` 构造，唯一调用者是 `7F8000` 里的 `7F8213`；名册条目在 `6B4E30` 的 `6B50B6` 调用 `589CC0` 删除，删除后紧接着保存（`6B5178` 调用 `Estate::System::Save`）。`7F8000`、`6B4E30` 都没有直接调用者（虚函数或回调），两者的先后和是否一定成对没有追。流程里没有解雇，不影响这次验收；如果有不派发事件的删除路径，`p3c_actor` 和探针会把被删的英雄算成「仍应有效」，表现为英雄句柄的检查失败。
- 名册条目的其他删除路径：名册代码（`583000-58D000`）里对 `+0x28` 的直接写入只有 `589E1E`，`589CC0` 的调用者只有 `6B50B6`；通过通用 vector erase 删除的路径没有覆盖。
