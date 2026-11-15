# P4 审核发现索引

P4（Effect 与 Buff）经过四轮对抗审核：第一轮三份报告（A：DDCore combat；B：持久化；C：DDLua 的 P4 API、`dd.fs` 写入、复刻和场景），第二轮两份（R2A、R2B），第三轮、第四轮各一份（R3、R4）。修复由代理 F1–F5 完成，F6、F7 处理最终场景发现的问题，P4 在 2026-11-15 验收通过。代码注释和测试用例里的编号指的是这里的条目。原始报告、简报和修复记录是 P4 的工作文件，验收后删除，这里只留编号、问题、处理和位置。设计上的结论见 ADR 0025–0034。

编号规则：

- 第一轮写成「报告-条目」，条目号沿用各报告：A-B1（阻断）、A-S1（应修）、A-N1（备注）；C-B1、C-Y1（应修）、C-N1、C-U1（无法确认）。报告 B 的条目在持久化代码里常不带前缀，写作「审核 B1」「审核 S1」「审核 N7」，别处写作 B-S5、B-N6。
- 第二轮写成 R2A-S1、R2A-N1（报告 R2A 的应修、备注），R2B-1（报告 R2B 的阻断和应修按序号）、「R2B 备注 6」；第三轮 R3-B1、R3-S1、「R3 备注 5」；第四轮 R4-S1、「R4 备注 7」。
- **和 P3 重名的**：P3 第一轮也有 A-B1、C-B1、B-N6，第二轮写成 R2A-阻断1、R2B-备注6（带连字符、中文类别），修复代理也叫 F1–F3（加上 G1–G2、H1–H2）。按位置区分：P4 的编号出现在 combat、persist、effect 文本、`dd.fs` 写入、`p4*` 场景和检查脚本的代码里，有的注释直接写「P4 审核」；`lua/src/sandbox.cpp` 和 `tests/unit/lua_tests.cpp` 第 700 行一带的 A-B1、`lua_tests.cpp` 里 P3 场景预演的 C-B1、`lua_tests.cpp` 的「审核修复 F1」一节、`tools/rig.py` 的「R2B-备注6」、`tools/p3_scenario_check.py` 的 F2 是 P3 的，查 [p3-review-findings.md](p3-review-findings.md)。P3 的「B-B1」是怪物 guid 的问题，P4 报告 B 的阻断写作「B1」。

## 第一轮：报告 A（DDCore combat）

| 编号 | 问题 | 处理 | 位置 |
| --- | --- | --- | --- |
| A-B1（阻断） | effect 文本里一个块达到 1024 字节，游戏的 `strncpy_s` 让 CRT 终止进程 | 按游戏解析器的规则分块，块长超过 1023、零长度块、列表项从块尾引号开始、缺少 `.target` 都拒绝（[ADR 0028](../adr/0028-runtime-definitions.md)） | `core/src/combat/logic.cpp`（`ParseEffectText`）、`lua/src/combat.cpp`、`tests/unit/test_main.cpp` |
| A-S1 | DDCore 取 `.name` 的方式和游戏不同，`.name"rakebuff_5"` 能改写游戏自己的 effect | 按 `36F020` 的规则取名字（最后一次出现），归属按哈希 | `logic.cpp`、`lua/src/combat.cpp`、`test_main.cpp` |
| A-S2 | `damage.final` 部分吸收时怪物的生命算错（免疫怪物生命变多，`+0x1581` 时吸收白丢） | 按 `480400` 的返回地址认怪物，照 `5FC3E0` 的规则补血（[ADR 0030](../adr/0030-damage-final-absorb.md)） | `core/src/events/system.cpp`、`core/src/game/signatures.inc`（`MonsterBlockReturn`）、`tools/check_actor_layout.py` |
| A-S3 | 加载线程运行时主线程照样读 buff 表，`PruneTags` 据此删掉还在的条目的标签 | 读接口和 `PruneTags` 在加载中不读 | `core/src/combat/combat.cpp`、`sdk/include/dd/dd_api.h` |
| A-S4 | `test_combat` 的 STACK 没算速度的乘法修正，走廊、战斗里必报 WARN，检查脚本据此的结论超出证据 | 逐条量增量、按倍率核对；场景第二段在三个场合重跑，检查脚本的 `combat.stack` | `tests/plugins/test_combat.cpp`、`tools/p4_scenario_check.py` |
| A-N1 | `buff.tick` 漏掉减到 0 的那一次 | 删除之前补派发 1→0 的 tick | `combat.cpp` |
| A-N2 | `buff_remove` 期间刷新顺带删掉的条目也报 `plugin` | 只有谓词命中的报 `plugin` | `combat.cpp` |
| A-N3 | 规则名可以占用游戏内置的状态名 | 8 个内置名和哈希相同的拒绝注册 | `logic.cpp`、`dd_api.h`、`test_main.cpp` |
| A-N4 | `stat_sub_type` 不校验 | 同 C-Y3 | `logic.cpp` |
| A-N5 | `skill.pre` 字段只读了，事件仍带可写标志 | 第二轮 R2A-N2 改掉 | `core/src/events/system.cpp` |
| A-N6 | `combat.md` 说读档 `5C4D10` 调 `5C4F50`，不对 | 知识库改正（P4 整理） | `knowledge/dd1/combat.md` |
| A-N7 | `save-log` 的目录里有上一次运行留下的日志 | 检查脚本核对运行标识和时间 | `tools/p4_scenario_check.py` |
| A-N8 | effect 临时文件名直接用插件名 | 只留 `[A-Za-z0-9_-]`，最多 32 个字符 | `combat.cpp` |
| A-N9 | 查 effect 不看联机开关 | 不改：只影响联机模式下的 `effect_get`，写操作在 PvP 里本来就拒绝 | — |
| A-N10 | 审核者编译验证程序的副作用 | 不适用 | — |
| A 无法确认 | 饰品类目标的 `effect_apply`、帧边界施加会改战斗结构的 effect、槽位 13 刷新时再加 buff、`Monster+0x1581` | `+0x1581` 已查清（`combat.md`），修法也不再依赖它；其余仍未确认 | — |

## 第一轮：报告 B（持久化）

| 编号 | 问题 | 处理 | 位置 |
| --- | --- | --- | --- |
| B1（阻断） | 写入 hook 把 32 位的大小当 64 位读，每次存档写入都被当成超过 64 MB 跳过，持久化在游戏里完全不工作 | 参数截成 32 位；进存档后第一次写名册时写一行带大小的 INFO | `core/src/persist/persist.cpp`、`store.h`（`ArgU32`）、`test_main.cpp`（`TestObserveArgs`） |
| B-S1 | 地牢存档的 raid 核对不试上一代，不一致后下一次存档又按新哈希写回 | 读档时按名册和读档前读到的 raid 一起判断两代；二次核对不一致时这次进存档不再写（`DD_PERSIST_E_INCONSISTENT`）；给 Lua 的通知没做（P7） | `store.cpp`、`persist.cpp`、`lua/src/session.cpp`、`tools/rig.py` |
| B-S2 | 写盘失败不重试，仍报「已写完」 | 按退避重试 4 次，如实报告 | `store.cpp`（`Writer`）、`persist.cpp` |
| B-S3 | 读档不一致之后两次存档就把原来的两代挤掉 | 第一次写之前把两代改名留底（`.mismatch-<时间>`） | `store.cpp`、`persist.cpp`、`test_main.cpp` |
| B-S4 | 只写庄园、城镇文件的存档不派发 `campaign.save` | 当前 profile 上任何 `persist.*` 的写入都派发 | `persist.cpp` |
| B-S5 | 只差大小写的 key、插件名、Lua Mod id 落到同一个文件 | 核对文件头的插件名和 key，声明和加载时拒绝只差大小写的 | `store.cpp`、`persist.cpp`、`lua/src/manifest.cpp` |
| B-S6 | 新战役复用旧存档位时报 `mismatch` | 名册和 `scripts\starting_save` 的相同时按新战役给 `empty`，旧旁路归档成 `oldcampaign` | `store.cpp`、`persist.cpp`、`test_main.cpp` |
| B-S7 | 退到桌面时在加载中也派发 `campaign.save` | 加载中不派发 | `persist.cpp` |
| B-S8 | `ReplaceFileW` 真正失败时的退回路径没有测试 | 加持有句柄、只读属性的真实失败用例，写之前清掉只读属性 | `store.cpp`、`test_main.cpp` |
| B-S9 | 检查脚本把「旁路从来没写过」判成没有触发 | 新项 `persist.written`；`persist.unexpected` 把写过之后读到的 `empty` 判失败 | `tools/p4_scenario_check.py` |
| B-N1 | `02-architecture.md` 里有控制字符 | 改掉 | `docs/02-architecture.md` |
| B-N2 | 知识库「探索文件只在继续探索时读」不完整 | 03 和知识库改正 | `docs/03-lua-system.md`、`knowledge/dd1/campaign.md` |
| B-N3 | 插件名当目录名（`.`、`..`、`backup`、`DDCore`） | 声明时检查 | `store.cpp`、`persist.cpp` |
| B-N4 | 取不到 SteamID 时账号永远是空串 | 按 `local` | `persist.cpp` |
| B-N5 | `rig_tamper.log` 跨运行累积，`.before-tamper` 会被覆盖 | 记录带运行标识，加 `--restore`，改过的拒绝再改 | `tools/rig.py` |
| B-N6 | 存档时的编码不受 `store_max_kb` 约束 | 编码到上限就停下 | `lua/src/state_json.*`、`lua/src/combat.cpp`、`session.cpp`、`lua_tests.cpp` |
| B-N7 | 退出时只等 3 秒 | 退到桌面等 10 秒 | `persist.cpp` |
| B-N8 | 没有 Lua Mod 时每次存档也写一行日志 | 每次进存档只写前 3 次 | `persist.cpp` |
| B-N9–N11 | `p4c_persist` 的核对依据、场景只有一种不一致、给 A 的提醒（窄参数） | 不是代码问题；上一代的路径由 `tamper-persist --current-only` 构造 | — |
| B-U1–U4 | 读 backup、退到桌面时游戏的写盘、战斗读档后怪物 id、Steam `FileWrite` 是否原子 | U3 有运行证据（探针场景和补跑那段里读档后怪物的 guid 和存档一致，载体按 guid 恢复）；其余仍未确认（见 `08-handoff.md`） | — |

## 第一轮：报告 C（DDLua 的 P4 API、`dd.fs`、复刻、场景）

| 编号 | 问题 | 处理 | 位置 |
| --- | --- | --- | --- |
| C-B1（阻断） | 检查脚本不看场景 Mod 自报的大部分失败 | 新项 `selfreport` | `tools/p4_scenario_check.py` |
| C-B2（阻断） | `dd.fs.write` 的耗时不受预算约束，深路径一次两百多毫秒 | 写入最多 8 层，按文件操作次数报告工作量，返回前检查截止时间（[ADR 0032](../adr/0032-fs-write-data-dir.md)） | `lua/src/modfs.h/.cpp`、`sandbox.cpp`、`lua_tests.cpp` |
| C-Y1 | 数据目录的配额能绕过，目录不计数、也删不掉 | 写入 8 层、扫描 16 层，目录计入条目数，`dd.fs.remove` 能删空目录 | `modfs.*`、`sandbox.cpp`、`config.h` |
| C-Y2 | Mod id 区分大小写，数据目录不区分 | 只差大小写的都不加载（同 B-S5） | `lua/src/manifest.cpp` |
| C-Y3 | `sub_stat` 不校验，显示提示时会弹游戏的 assert | 按 stat 查游戏的名字表校验 | `logic.cpp`、`dd_api.h`、`test_combat.cpp`、`test_main.cpp` |
| C-Y4 | 「规则按 Lua 的结果生效」只数调用次数 | 用 `remove_if_not_active` 的条目在不在作判据，三个场合各一遍 | `tests/lua/scenario/p4b_combat`、`p4c_shield`、检查脚本 |
| C-Y5 | 不一致读档时「标记的状态不恢复」是空判 | 要求名册里有带状态的标记 | `p4c_persist`、检查脚本 |
| C-Y6 | 复刻和 DDShield 的四处差异没写进 README | 照 DDShield 做：生命 ≤ 0 时清零、没有护盾时规则交还原函数、第 1 回合不结算、认 `heal`/`clear` | `examples/lua/shield_lite`、检查脚本 |
| C-Y7 | 规则、脚本 Buff 回调的总量和存档时的编码不受约束 | 每个 Mod 每帧 `combat_frame_ms`，有状态的载体最多 `max_per_mod` 条，编码按 `store_max_kb` 截停 | `lua/src/combat.cpp`、`config.h` |
| C-Y8 | 检查脚本的其他判定偏松 | WARN 也查，按 effect 名字独立解析，自然触发要求 Smite，没有正常退出判失败 | 检查脚本 |
| C-Y9 | 没核对 tray 里能看到护盾 | 写成目测项 `look.tray` | 检查脚本、`docs/10-p4-scenario.md` |
| C-N1 | `tamper-persist` 改完不恢复，记录不按运行过滤 | 同 B-N5；检查脚本只用本次运行的记录 | `rig.py`、检查脚本 |
| C-N2 | `<账号>\<设备>\DDLua` 不查联接 | 三级都查 | `rig.py` |
| C-N3 | 03 说「读属性时游戏判定规则」 | 改文档：读 actor 字段走纯函数 `4C3F20` | `docs/03-lua-system.md` |
| C-N4 | 撤销内部订阅依赖析构顺序 | 看 `HasEvents()` | `lua/src/session.h`、`lua/src/combat.cpp` |
| C-N5 | 排队的重复定义失败时把回调一起删掉 | 只撤销这次新建的定义槽 | `lua/src/combat.cpp`、`lua_tests.cpp` |
| C-N6 | buff 表超过 512 条后读表失败，`PruneTags` 清掉全部标签 | 读表上限 4096，表里已有 1024 条时 `buff_add` 拒绝，读失败不清 | `core/src/combat/combat.cpp` |
| C-N7 | 临时文件名可能超长，保留设备名能建出来 | 临时文件名不带目标名，保留设备名拒绝 | `lua/src/modfs.cpp` |
| C-N8 | `shield_lite` 清理后又从载体把护盾恢复回来 | 清理后到下一场战斗开始不恢复 | `examples/lua/shield_lite/lua/main.lua` |
| C-U1–U4 | effect 名字提取、`7C6100` 的 assert、`battle.end` 和删除载体的先后、规则的峰值 | U1 由 A-B1、A-S1 解决；U2 第二轮核对不成立；U3 由 C-N8 绕开；U4 检查脚本加 `rule.stats` | — |

## 第二轮

| 编号 | 问题 | 处理 | 位置 |
| --- | --- | --- | --- |
| R2A-S1 | 加载中读接口返回 `nil`，和「真的没有」分不开 | combat 服务加 `read_ready`，DDLua 返回以「加载中」开头的原因，场景 Mod 等加载结束 | `core/src/combat/combat.h/.cpp`、`lua/src/combat.cpp`、`lua/src/actor.cpp`、场景 Mod、`lua_tests.cpp` |
| R2A-N1 | `damage.final` 靠返回地址认怪物，别的插件在 `480400` 上排在前面时失效 | 不改，写进 `dd_api.h` | `dd_api.h` |
| R2A-N2 | A-N5 没修 | `skill.pre` 不再可写，`p3d_core` 的判定跟着改 | `system.cpp`、`sdk/lua/topics.json`、`tests/lua/scenario/p3d_core` |
| R2A-N3 | 数据目录建不出来时 `dd.fs.write` 不查预算 | 走同一个预算检查 | `lua/src/sandbox.cpp`、`lua_tests.cpp` |
| R2A-N4 | 重新定义 effect 时列表字段追加 | 写进文档 | `dd_api.h`、03、`sdk/lua/dd.lua` |
| R2A-N5 | 运行时定义没有个数上限 | 写进文档，按 Mod 的上限留到 P7 | 同上 |
| R2A-N6 | 同一个新 id 排队定义两次都失败时留下空槽 | 把「新建」交给后一次，它也失败时撤销 | `lua/src/combat.cpp`、`lua_tests.cpp` |
| R2A-N7 | `dd.fs.write` 每次同步刷盘 | 写进 03 的「预算不能保证什么」 | 03、`dd.lua` |
| R2B-1（阻断） | 可选部分 D 读到上一代时必需项判失败，`tamper-persist` 还可能把哈希改回原值 | `persist.store` 对上一代在本次运行更早的存档里找；D 挪到第 12 步之后；`tamper-persist` 跟着游戏的轮换改备份名，认记录过的哈希 | 检查脚本、`rig.py`、10、`lua_tests.cpp` |
| R2B-2 | 第二段在文档里是必做的，检查脚本却按可选判 | `combat.stack` 改成必需 | 检查脚本、10 |
| R2B-3 | 第二段用存档 A 之后重跑必做部分，第一次读档就不一致 | 第二段改用存档 B，加 `rig.py reset-persist` | `rig.py`、检查脚本、10 |
| R2B 备注 1 | 城镇读档后游戏马上存档，`--restore` 总是没有可恢复的 | 这种情况退出码 0、说明原因 | `rig.py`、10、`lua_tests.cpp` |
| R2B 备注 2 | `E_INCONSISTENT` 的提示写死成 raid | 改文字 | `lua/src/session.cpp` |
| R2B 备注 3 | `campaign.save` 先于同一帧的延迟事件派发 | 不改，写进 `dd_api.h` | `dd_api.h` |
| R2B 备注 4、5 | `unavailable` 时不归档；取不到 SteamID 时整次运行按 `local` | 不改 | — |
| R2B 备注 6 | 场景 Mod 按原因里有没有「raid」判断核对完没有 | `dd.store_status()` 加第三个返回值 `raid_pending` | `lua/src/sandbox.cpp`、`p4c_persist`、`lua_tests.cpp` |
| R2B 备注 7–10 | README 的 `mirrorHeal`、`save-log lua` 重名、写盘重试的 WARN、自测样本的旧文字 | 改 README、改名 `lua-p4`、10 写明先查杀毒软件、改样本 | `shield_lite/README.md`、10、检查脚本 |

F4 另外修了一处没有编号的：检查脚本按序号把不同次进存档的 `campaign.save` 串在一起，改成按（存档序号，k）配对。

## 第三轮

| 编号 | 问题 | 处理 | 位置 |
| --- | --- | --- | --- |
| R3-B1（阻断） | 开战后 5～9 秒才有带角色的事件，场景 Mod 靠「事件里见过」选目标，多半什么都做不了 | `game` 服务加队伍、怪物、战斗状态的只读枚举（[ADR 0034](../adr/0034-party-monster-enumeration.md)），场景 Mod 改成主动枚举，离线预演改成真实时序 | `core/src/game/actors.*`、`core/src/core.cpp`、`lua/src/actor.cpp`、三个场景 Mod、`lua_tests.cpp` |
| R3-B2（阻断） | 被标记的怪物在读档前被打死，`persist.battle` 没有触发 | 每个怪物都加标记 | `p4c_persist` |
| R3-B3（阻断） | 读进战斗中的存档后把这场战斗当成走廊 | 场合按 `dd.actor.in_battle()` 判断 | 三个场景 Mod、`lua_tests.cpp` |
| R3-S1 | 同一个存档重跑时 `boundary.stack` 可能一直没有触发 | 每次进存档加两条临时的同 id 标记，判完删掉 | `p4c_persist` |
| R3-S2 | 崩溃、强杀后重跑没让先 `reset-persist` | 文档 | 10 |
| R3-S3 | 带 `p4c_bend` 的英雄阵亡时自报失败 | 英雄的 `gone` 只记信息 | `p4c_persist` |
| R3 备注 1–8 | 文档和 Mod 不符、走廊标记加在没出征的英雄身上、可选 B 和 D 的先后、可选项失败的退出码、自测的 `DDCore.log` 是手写的、第二段的自检、第二次挨打别进死门、改旁路前等几秒 | 备注 5 留到 P7（检查脚本用到的日志格式集中、加对照测试），其余改了文档或 Mod | 10、`p4c_persist` |

## 第四轮

| 编号 | 问题 | 处理 | 位置 |
| --- | --- | --- | --- |
| R4-S1 | 队伍表在游戏里没读过，文档没有「看不到就停」的规则 | 主线补进 10 的第 5、6 步（止损） | 10 |
| R4 备注 1–7 | 预演没有加载画面、`live_check` 的 0.2 秒窗口、新分支只靠读代码、`shield_pierce_50` 也是 `.on_miss false`、战斗销毁中途的枚举、第 2 步的过滤命令、`dd.actor` 对 `game` 服务大小的要求 | 没有改代码；备注 5、7 写进 ADR 0034 和 `08-handoff.md`，第 4、6 条可以顺手补进 10 | — |

## 修复代理

| 代号 | 范围 |
| --- | --- |
| F1 | 报告 A 全部，报告 C 的 B2、Y1–Y3、Y6 第 2 条、Y7、N3–N8、U1、U2（DDCore combat、DDLua 的 P4 API、`dd.fs`） |
| F2 | 报告 B 全部，A、C 里属于持久化代码的备注 |
| F3 | 报告 C 的 B1、Y4–Y6、Y8、Y9、N1、N2、N8，B-S9，A-S4 的场景一侧，A-N7（检查脚本、复刻、场景 Mod、离线预演、10） |
| F4 | 第二轮 R2A、R2B |
| F5 | 第三轮 R3，新增只读枚举 |
| F6 | 最终场景之后：`shield.natural`（技能的 effect 晚于 `skill.post` 施加）。已修：shield_lite 按执行者记最近一次技能和它的 effect 名字，`skill.post` 时不清；检查脚本另认 `p4b_combat` 的 `[skill]` 行，排除 `p4c_shield` 驱动的施加（`examples/lua/shield_lite/lua/main.lua`、`tools/p4_scenario_check.py`、`lua_tests.cpp` 的 `P4cSkillLate`）。第一次运行的日志重判、补跑那段都通过 |
| F7 | 最终场景之后：退出存档时英雄身上载体的 `on_remove` 收到 `gone`（DDCore 在派发 `CampaignUnload` 之前就把存档标成不活跃，英雄句柄解析不了）。已修：DDLua 拆除时不调用任何 `on_remove`，状态直接丢弃，`gone` 只在会话中每秒的清理里调用（`lua/src/combat.cpp`、[ADR 0027](../adr/0027-script-buff-carrier.md)）；补跑那段里两次退出存档都只丢弃状态 |
| 验收后（主线） | 补跑那段里 `p4c_persist` 把两条旧载体（guid 221、222，上一次运行不一致的读档时丢了状态）回城时被删报成失败。改成没有跟踪、状态为空的载体只记信息 `town_visit.stale`（`tests/lua/scenario/p4c_persist/lua/main.lua`），离线测试和检查脚本自测通过 |

## 简报的设计编号和阶段名

代码注释里的 D1–D12 是 P4 实现简报的设计取舍，验收后整理成 ADR：

| 编号 | 内容 | 现在看 |
| --- | --- | --- |
| D1 | 分层：游戏相关的都在 DDCore，经 SDK 的 `combat`、`persist` 服务开放 | [ADR 0003](../adr/0003-thin-proxy-core-layering.md)、`02-architecture.md` |
| D2 | 句柄是值：buff 是（actor，组 guid），effect 是名字，技能是（执行者，id，等级） | [ADR 0024](../adr/0024-actor-handles-are-values.md)、[0027](../adr/0027-script-buff-carrier.md) |
| D3 | 写操作的时机和队列；`buff.*` 在加载期间和读档时不派发 | [ADR 0025](../adr/0025-combat-write-timing.md)、[0026](../adr/0026-buff-topics-on-container-ops.md) |
| D4、D5 | 运行时的 buff 定义（POD 字段）、effect 定义（游戏的解析器） | [ADR 0028](../adr/0028-runtime-definitions.md) |
| D6 | 脚本 Effect：`effect.pre` 按名字路由，名字在 C++ 里匹配 | `03-lua-system.md` 的需求三 |
| D7 | 脚本 Buff 用载体 buff 承载 | [ADR 0027](../adr/0027-script-buff-carrier.md) |
| D8 | 规则扩展 | [ADR 0029](../adr/0029-rule-extension-entry-hook.md) |
| D9 | 存档旁路 | [ADR 0031](../adr/0031-persist-sidecar-content-hash.md) |
| D10 | `dd.fs` 的写入 | [ADR 0032](../adr/0032-fs-write-data-dir.md) |
| D11 | `skill.pre` 不开放写入 | [ADR 0033](../adr/0033-skill-pre-read-only.md) |
| D12 | `damage.final` | [ADR 0030](../adr/0030-damage-final-absorb.md) |

阶段名：P4-0A（逆向 Buff、Effect、规则、tray）、P4-0B（逆向存档、profile、持久化时机、技能）、P4-0C（探针 `probe_p4` 和 2026-11-13 的探针场景）、P4-0D（按探针日志判定、改知识库）；P4a（DDCore 和 SDK）、P4b（DDLua）、P4c1（持久化）、P4c2（复刻和场景）。`0A-n`、`0A#n`、`0B-13` 这类编号是 0A、0B 待探针核实清单的条目，各条的结论在 `knowledge/dd1/combat.md`、`campaign.md` 写着「P4 探针场景」的地方。
