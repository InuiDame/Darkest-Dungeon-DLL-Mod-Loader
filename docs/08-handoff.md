# 接手说明

更新于 2026-11-11。这里只放当前状态和下一步；设计看 02、03，取舍看 [adr/](adr/)，流程看 06。

## 当前状态

- P0–P3 都已通过用户验收。P3 的场景验收按 [09-lua-scenario.md](09-lua-scenario.md) 跑完，检查脚本 29 项：通过 28，唯一的可选项没有触发；没有 Lua Mod 时 `CampaignLoad` 的 hook 报告 1 个目标、语义 hook 0、订阅 0。
- 下一步是 P4（Effect 与 Buff），还没开始。
- 测试目录部署的是 `lua` 方案（DDCore + DDLua），`dbghelp.dll` 是 `530234a4`，真实安装未被改动（`rig.py status`、`verify-real`）。场景的 5 个测试 Lua Mod 只在 `scenario` 方案里复制进测试目录。
- 改动还没有 commit，用户要求时才提交，commit 信息只写主题和正文，不加 Claude 署名。

## 接手 P4

1. 读 [04-roadmap.md](04-roadmap.md) 的 P4、[03-lua-system.md](03-lua-system.md) 的「需求三：Effect 和 Buff」、ADR 0019–0024，以及 [06-workflow.md](06-workflow.md) 的流程和简报要求。
2. 开工前先做逆向准备（buff 增删函数、Effect 的施加路径、存档旁路的写入时机），结论写进 `knowledge/dd1/combat.md`，能用探针核实的先核实，P3 就是这样准备的。
3. 按 06 写实现简报，工程量大时拆成小阶段；派 Opus 实现，再派另一个 Opus 对抗审核。

可以直接用的东西：

- `game` 服务的 actor 句柄、`events` 服务的 `on_end` 和 `subscription_count`，DDLua 的会话、事件绑定、调度器（`lua/src/`）。
- 离线测试的假宿主（`tests/unit/lua_tests.cpp`），场景测试 Mod 和检查脚本（`tests/lua/scenario/`、`tools/p3_scenario_check.py`），P4 的场景可以在 09 和这里扩充。
- Lua 官方测试集的测试环境（`tests/lua_official/`），改 Lua 补丁后重跑。

## 仍未验证

| 项目 | 现状 |
| --- | --- |
| `skill.pre` 的写入 | 可写字段都是指针，Lua 写不了，推迟到 P4 |
| 阵亡、解雇英雄的句柄 | 反汇编结论是阵亡留在名册、解雇立即删除（`combat.md`），P3 场景里没有人阵亡或被解雇 |
| 战斗序号的盲区 | 一场战斗销毁和下一场创建落在同一帧、中间没有 actor 调用时会漏判，静态上不会发生（`phase_tracker.h`） |
| 大堆的 GC 停顿补偿 | 只有离线测试，游戏里没有构造过超过内存上限一半的堆 |
| 6 个伪造虚表的事件 | 已注册，真实派发还没发生过（第 7 个 `PostOnStartTownVisitEvent` 已收到） |
| Query 的游戏回调是否返回 `1` | 只有 `ProvisionSupplyBuffQuery`、`FindNonRosterHeroesQuery`、`TownVisitIdleStressBuffQuery` 确认没被挡住 |
| PvP 判定 | 只有静态依据，`query.*` 和 `.pre` 在 PvP 中不派发 |
| `ResourceManager`、`StorageManager` 后台线程是否碰 listener 列表 | 直接调用里没有，虚调用没跟踪，要在 `3619D0` 上记录线程才能确认 |
| `3E06C2` 写 next=4 | 需要 `[2A6E6FC]` 非零加按键，疑似调试入口；如果能在加载期间触发，加载保护会提前解除 |
| 离线版 | 只验证了特征码和启动到主菜单；`DefaultUserServices` 的 Mod 列表只有静态结构 |

## 已知限制

- 执行预算防住意外，不承诺挡住刻意构造的卡顿，完整清单在 03 的「沙箱和稳定性」（[ADR 0020](adr/0020-lua-budget-best-effort.md)）。
- 只有弱键的 Lua 表按强引用处理；`dd.import` 返回的对象是共享的；硬链接能让 `dd.fs` 读到 Mod 目录外的文件（只能本机创建）。
- 按存档的 Lua Mod 收不到 `campaign.unload`；恢复已保存的战斗、读档恢复的战斗不派发 `battle.start`（`combat.md`）。
- `native.AwardAchievementEvent`、`query.TownGoldInterestBuffQuery` 不可用，游戏里没有对应的类型描述符。
- `battle.turn.*` 只在英雄回合触发，只读；跳过回合要走 `TurnResult=2` 的路径，需要实测。
- 战斗最后一回合没有 `battle.round.end`，用 `battle.end`。`inventory.modified` 静态上没找到派发点。
- 插件的内联 hook 只能在主线程、且不在加载过程中修改，否则返回 `DD_HOOK_E_BAD_TIMING`。
- 审核备注里没改的两处，影响都很小：同名 DLL 加载失败时的去重差异（`core/src/plugin/discovery.cpp`），跨线程注册时极窄的重复回调。

## 遗留整理项

- P7：DDLua 的配置项（`DDLua.cfg`：预算、内存、`frame_budget_ms`、`teardown_budget_ms`、`log_max_mb`、`log_lines_per_sec`、`max_per_mod`、开发模式）写进 Mod 作者文档；双语功能清单和 Mod 作者指南（对 06 第 6 步的有意推迟）。
- `core/src/hook/engine.cpp` 里串联第三方跳转的代码保留，有单元测试；`FF 25` 跳转槽超出 ±2GB 的回退分支没有用例。按 [ADR 0017](adr/0017-no-legacy-plugin-compat.md) 不再投入。
- guest 的 `DD.ModLoader/build/` 里留着 P1–P3 的验证产物（`p1_*`、`p3a_selfcheck`、`p3d_*`、`f1_*`、`f2`、`f3`、`g1*`、`g2`、`h1`、`h2`、`review*`、`luatests`、`rig_logs`、`main_*`），不被 git 跟踪，可以删；P3 场景的日志副本在 `rig_logs/scenario`、`rig_logs/lua`。
- 旧目录 `G:\Doc\GitHub\Darkest Dungeon DLL Mod Loader`、`G:\Doc\repo` 已退役，是否删除由用户决定。
- 环境：guest 访问 `github.com`、lua.org 经常失败，下载依赖时在 host 下载、同步到工作区并核对 sha256。在没有 `xmake.lua` 的子目录里运行 xmake 要带 `-P .`，否则会按默认配置重编主工程。
