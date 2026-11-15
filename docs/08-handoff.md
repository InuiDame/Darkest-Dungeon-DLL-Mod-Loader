# 接手说明

更新于 2026-11-15。这里只放当前状态和下一步；设计看 02、03，取舍看 [adr/](adr/)，流程看 06。

## 当前状态

- P0–P4 都已通过用户验收。P4（Effect 与 Buff）在 2026-11-15 按 [10-p4-scenario.md](10-p4-scenario.md) 验收，四轮对抗审核，修复 F1–F7：
  - 第一次运行（必做部分和第二段，日志在 guest 的 `DD.ModLoader/build/rig_logs/scenario4`、`rig_logs/combat`）：检查脚本 46 项，通过 36，失败 0，必需项没有触发 2。`shield.natural` 是归因问题：游戏在技能演出时才施加 Smite 的 `shield_self_gain_5`，晚于 `skill.post`，F6 让 shield_lite 按执行者记最近一次技能，同一份日志重判通过。`persist.battle` 是那次没做战斗中读档。
  - 补跑一段（构建含 F6、F7，日志在 `rig_logs/scenario4-battle`）：战斗中存档读档后载体 9 条（英雄 6、怪物 3）的状态、guid 和标签一致，`persist.battle` 通过；`shield.natural` 通过，护盾 237 次变化逐行重放都和 DDShield 一致，读档后英雄和怪物的护盾从载体恢复（`shield.persist`）；退出存档时只丢弃状态、不调用 `on_remove`（F7）。检查脚本报的 2 项失败是 `p4c_persist` 对两条旧载体（guid 221、222）的误报：上一次运行 `tamper-persist` 造成的不一致读档按设计丢弃了状态，空状态的载体留在英雄身上，回城时游戏照样删掉它们，原因 `town_visit`，`remove.match` 通过。主线随后改了 `tests/lua/scenario/p4c_persist/lua/main.lua`，这种载体只记信息（`town_visit.stale`），离线测试 1291 项和检查脚本自测通过，游戏里没有再跑。
  - 用户目测：有护盾时圣骑士头顶 tray 里多出载体 buff 的图标，护盾全部挡下时弹 Blocked；没有护盾条，护盾数值看不出来。
  - 可选部分只做了 C（随补跑那段）；A、B、D、E 和加载线程上的延迟 `buff.*` 见「仍未验证」。
- 下一步是 P5（回合外系统），还没开始。
- P4 的阶段工作文件（简报、进度、审核报告、整理记录）在验收后删除，备份在 guest 的 `DD.ModLoader/build/p4_worklog/`，不被 git 跟踪。代码和测试里的审核编号、简报的设计编号（D1–D12）查 [history/p4-review-findings.md](history/p4-review-findings.md)。
- 测试目录部署的是 `scenario4` 方案（补跑用的），`dbghelp.dll` 是 `3d54bdeb`，真实安装未被改动（`rig.py status`）。里面的 `p4c_persist` 还是改之前的，再跑场景前重新 `deploy scenario4`。
- 改动还没有 commit，用户要求时才提交，commit 信息只写主题和正文，不加 Claude 署名。

## 接手 P5

1. 读 [04-roadmap.md](04-roadmap.md) 的 P5（含 P4 留下的几项）、[03-lua-system.md](03-lua-system.md) 的「需求四：回合外系统」、ADR 0024、0025、0031、0034，以及 [06-workflow.md](06-workflow.md) 的流程和简报要求。
2. 开工前先做逆向准备：各系统的单例和字段（`knowledge/dd1/campaign.md` 的「系统对象」只有类名），名册、庄园、库存的写入函数和它们会触发哪些存档写入，结论写进 `campaign.md`，能用探针核实的先核实。P4 就是这样准备的：静态分析（P4-0A、0B）、探针插件、用户跑一次探针场景、按日志逐项判定再改知识库（P4-0C、0D）。
3. 按 06 写实现简报，工程量大时拆成小阶段；派 Opus 实现，再派另一个 Opus 对抗审核。

可以直接用的东西：

- 写操作的时机和队列（[ADR 0025](adr/0025-combat-write-timing.md)）：P5 的写操作照同样的规则，安全点立即执行、事件回调里排队。
- `game` 服务的只读枚举（`party_heroes`、`battle_monsters`、`battle_state`），`dd.raid` 在它上面扩展（[ADR 0034](adr/0034-party-monster-enumeration.md)）。
- `persist` 服务和 `campaign.save`（[ADR 0031](adr/0031-persist-sidecar-content-hash.md)）：DDCore 按（设备，文件名）记每个存档文件的读写哈希，可以用来确认某个操作写了哪些存档文件；P5 改名册、庄园之后，旁路和存档照样对得上。
- 离线测试的假宿主（`tests/unit/lua_tests.cpp`，有假的 combat、persist、game 服务和场景预演），场景测试 Mod 和检查脚本的写法（`tests/lua/scenario/p4*`、`tools/p4_scenario_check.py`），探针插件 `tests/plugins/probe_p4.cpp`（用法写在它开头）和分析脚本 `tools/p4_probe_report.py`。

## 仍未验证

| 项目 | 现状 |
| --- | --- |
| 加载线程上的延迟 `buff.*` | P4 的游戏内运行（探针、`test_combat` 场景、两次最终场景，含走廊和战斗中读档）里都没有出现，只有离线测试；静态上 `load_raid`、`load_town` 线程可能增删 |
| 战斗结算画面期间的 `dd.actor.in_battle()` | 读进战斗中的存档时为真（探针的 `batt=46`；补跑那段里 `p4c_persist`、`p4b_combat` 据此认出读进来的战斗）；结算画面期间的值没有日志 |
| `damage.final` 的免疫怪物、死门上的吸收 | 只有静态依据（`5FC3E0` 的免疫分支、英雄槽位 40/43），死门（可选部分 E）两次场景都没做；英雄和普通怪物的全部、部分吸收有运行证据 |
| effect 文本的预检 | 块长 1024、列表项的边界、零长度块只在离线测试和审核的独立复刻里验证过；游戏里只定义过短文本 |
| tray 里载体 Buff 的分组 | 用户看到了图标，归 buff 组还是 debuff 组没有分辨（数值 0，按 `4A9C80` 的正负判定） |
| `buff.remove` 的原因 | 两次最终场景出现过 `expired`、`plugin`、`predicate`、`refresh`、`reset`、`skill_instant`、`town_visit`（P4a 场景另有 `other`）；`trinket`、`burn_decay`、`by_guid`、`effect`、`item` 没出现过 |
| 新战役、读 backup、旁路真的落后、刚存档就退到桌面 | 只有静态依据和离线测试：新战役按名册和 `scripts\starting_save` 相同判断；读 backup 按路径前缀判断；`2624022` 谁置位、Steam `FileWrite` 是否原子都没查 |
| 读到上一代（`prev`）、地牢里的 raid 不一致、没有 Lua Mod 的存档、生命 ≤ 0 时护盾清零 | 最终场景的可选部分 D、B、A 和 `shield.zero`，两次都没做，只有离线测试和预演 |
| Lua 施加的 effect：`.on_miss false`、饰品类目标、会改战斗结构的 | `e:apply` 走游戏的 `ApplyEffect`；第一种以游戏为准，后两种没有证据（审核 A 的「无法确认」） |
| 阵亡、解雇英雄的句柄 | 最终场景里招募、解雇各两次（各自立刻写名册），没有专门核对解雇后的句柄；阵亡仍没有发生过 |
| 战斗序号的盲区 | 同 P3：一场战斗销毁和下一场创建落在同一帧、中间没有 actor 调用时会漏判，静态上不会发生（`phase_tracker.h`） |
| 大堆的 GC 停顿补偿 | 只有离线测试；嵌套入口里的紧急 GC 没有专门的用例 |
| 6 个伪造虚表的事件、Query 的游戏回调是否返回 `1`、PvP 判定、`ResourceManager`/`StorageManager` 后台线程是否碰 listener 列表、`3E06C2` 写 next=4、离线版 | 同 P3，没有新证据；离线版只验证了特征码（P4 新增的 32 条都唯一命中） |

## 已知限制

- 执行预算防住意外，不承诺挡住刻意构造的卡顿，完整清单在 03 的「沙箱和稳定性」（[ADR 0020](adr/0020-lua-budget-best-effort.md)）。P4 加了嵌套入口的隔离和补时间（[ADR 0025](adr/0025-combat-write-timing.md)）；`dd.fs.write` 每次同步刷盘，单次可能超出预算。
- 只有弱键的 Lua 表按强引用处理；`dd.import` 返回的对象是共享的；硬链接能让 `dd.fs` 读到 Mod 目录外的文件（只能本机创建）。
- 按存档的 Lua Mod 收不到 `campaign.unload`；恢复已保存的战斗、读档恢复的战斗不派发 `battle.start`，用 `dd.actor.in_battle()` 判断（[ADR 0034](adr/0034-party-monster-enumeration.md)）。
- `native.AwardAchievementEvent`、`query.TownGoldInterestBuffQuery` 不可用，游戏里没有对应的类型描述符。
- `battle.turn.*` 只在英雄回合触发，只读；跳过回合要走 `TurnResult=2` 的路径，需要实测。战斗最后一回合没有 `battle.round.end`，用 `battle.end`。`inventory.modified` 静态上没找到派发点。
- 插件的内联 hook 只能在主线程、且不在加载过程中修改，否则返回 `DD_HOOK_E_BAD_TIMING`。
- P4 的：
  - 运行时定义的 effect 不能被技能引用；buff 定义只开放 POD 字段；重新定义 effect 时列表字段追加；运行时定义没有个数上限（[ADR 0028](adr/0028-runtime-definitions.md)）。
  - 规则名全局唯一，游戏内置的 8 个状态名不能注册；加载线程上的判定一律交还原函数（[ADR 0029](adr/0029-rule-extension-entry-hook.md)）。
  - `skill.pre` 只读；技能自带的 effect 在技能演出时、晚于 `skill.post` 才施加，要把 effect 归到技能上得按执行者记最近一次技能（shield_lite 的做法，[ADR 0033](adr/0033-skill-pre-read-only.md)）。
  - `damage.final`：死门上部分吸收无效；别的插件在 `480400` 上排在 DDCore 前面时认不出怪物（[ADR 0030](adr/0030-damage-final-absorb.md)）。
  - 加载画面期间读接口返回「加载中」；`buff.remove` 的原因是按调用栈推断的（[ADR 0025](adr/0025-combat-write-timing.md)、[0026](adr/0026-buff-topics-on-container-ops.md)）。
  - 持久化：不一致时不收回已交出的 `dd.store`，也没有专门的通知；`battle_snapshots` 里没打完的战斗、备用名册上英雄的状态和标签不保存；取不到 SteamID 时整次运行按 `local`（[ADR 0031](adr/0031-persist-sidecar-content-hash.md)）。
  - 不在 P4 范围：自定义 tray 图标和提示文字、跨回城保留的「永久」脚本 Buff（[ADR 0027](adr/0027-script-buff-carrier.md)）。
  - 复刻 `shield_lite` 和 DDShield 的差异（没有护盾条，护盾数值只在日志里；上限来自配置表等）写在它的 README 里。
- 审核备注里没改的，影响都很小：同名 DLL 加载失败时的去重差异、跨线程注册时极窄的重复回调（P3）；DDCore 加载插件的重名检查区分大小写，只在 persist 声明处拦住；联机模式下 `effect_get` 只查普通 effect map（审核 A-N9）；`unavailable` 时旁路不归档（R2B 备注 4）；`campaign.save` 先于同一帧的延迟事件派发（R2B 备注 3）；`DoAdd` 加完无条件记标签，条目在加上时的刷新里就被删掉时留下一条等清理的标签。

## 遗留整理项

- P7：DDLua 的配置项（`DDLua.cfg`：预算、内存、`frame_budget_ms`、`teardown_budget_ms`、`log_max_mb`、`log_lines_per_sec`、`max_per_mod`、开发模式，P4 加的 `rule_budget_ms`、`combat_frame_ms`、`data_max_mb`、`data_max_files`、`store_max_kb`）写进 Mod 作者文档；双语功能清单和 Mod 作者指南（对 06 第 6 步的有意推迟，P4 同样没改 Inno Setup 目录里的文件）。
  - 功能清单要补的 P4 内容：`combat` 服务（buff 的读写和标签、运行时 buff 和 effect 定义、施加 effect、规则扩展、技能句柄）、`buff.add/remove/tick` 和 `damage.final` 主题、`persist` 服务和 `campaign.save`（存档旁路在 `DDModLoader\save\`）、`game` 服务的队伍和怪物枚举；Lua 的 `dd.buff`、`dd.effect`、脚本 Effect、脚本 Buff、`dd.buff.rule`、`dd.store`/`dd.store_status`、`dd.fs` 的写入（`DDModLoader\data\`）、`dd.actor.party/monsters/in_battle`；示例 `examples/lua/shield_lite`。
  - Mod 作者指南要写的：写操作的返回约定和 `done`、加载中读不到、运行时定义的限制、规则的数据层写法（`is_actor_status`/`actorStatus` + `rule_data.string`）、载体来源的三个标志、`dd.store` 的几种状态和不一致时的处理。
- P7 的 API 整理：按 Mod 的运行时定义个数上限（R2A-N5）、`damage.final` 改用槽位 50 的上下文认怪物（R2A-N1）、raid 二次核对不一致时给 Lua 的通知（审核 B 的 S1）、`dd.actor` 对旧 DDCore 的处理（R4 备注 7）、检查脚本用到的 DDCore 日志格式集中并加对照测试（R3 备注 5）。
- `docs/07-event-coverage.md` 是 P2 的 `test_events` 日志生成的，P4 的 `buff.*`、`damage.final`、`campaign.save` 还列在「后续阶段的主题」里，要用 `test_events` 重跑一次场景再生成。
- `core/src/hook/engine.cpp` 里串联第三方跳转的代码保留，有单元测试；`FF 25` 跳转槽超出 ±2GB 的回退分支没有用例。按 [ADR 0017](adr/0017-no-legacy-plugin-compat.md) 不再投入。
- P4 的探针：`tests/plugins/probe_p4.cpp`（rig 方案 `probe4`）和 `tools/p4_probe_report.py` 留作 P5 探针的参考，是否删掉由用户决定。`test_combat`（方案 `combat`）是场景第二段要用的，保留。
- guest 的 `DD.ModLoader/build/` 里留着 P1–P4 的验证产物，不被 git 跟踪，可以删。P4 的有 `p4_probe_fake*`、`p4a*`、`p4b_offline`、`p4c_rehearsal`（`dd_lua_tests P4c2` 会重新生成，检查脚本的 `--selftest` 要用）、`p4f3_*`、`f1*`、`f2*`、`f3`、`f4*`、`f5`、`f7_selftest.txt`、`final`、`main_*`、`review_a/b/c`、`review2_*`、`review3*`、`review4*`，以及 `rig_logs` 下 P4 的自检目录。P4 场景的日志在 `rig_logs/p4_scenario`（探针）、`p4a_combat`、`scenario4`、`combat`、`scenario4-battle`，是验收的证据，删之前确认不再需要；P3 场景的在 `rig_logs/scenario`、`rig_logs/lua`。阶段工作文件的备份在 `build/p3_worklog/`、`build/p4_worklog/`。
- 旧目录 `G:\Doc\GitHub\Darkest Dungeon DLL Mod Loader`、`G:\Doc\repo` 已退役，是否删除由用户决定。
- 环境：guest 访问 `github.com`、lua.org 经常失败，下载依赖时在 host 下载、同步到工作区并核对 sha256。在没有 `xmake.lua` 的子目录里运行 xmake 要带 `-P .`，否则会按默认配置重编主工程。
