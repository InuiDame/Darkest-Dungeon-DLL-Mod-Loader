# 工作流

## 阶段流程

1. 主线整理阶段简报：范围、验收清单（来自 [04-roadmap.md](04-roadmap.md)）、需要读的知识库条目。
2. 派 Opus 代理实现。结束条件：验收清单逐项有证据，或写明做不到的原因。工程量大时拆成小阶段，每个小阶段派一个新的代理，报告追加到阶段的工作文件（如 `p<N>-progress.md`），下一个代理从那里接手，避免上下文被压缩。
3. 派另一个 Opus 代理做对抗审核，只给简报和产物位置，不给实现者的结论。
4. 处理审核结果。有阻断项时修完回到第 3 步；只有应修项时修完并验证即可。
5. 跑验收清单，证据（日志、测试输出）汇报给用户。
6. 更新文档，新的设计取舍写成 [ADR](adr/)；涉及加载器集成的，同步更新功能清单和 Mod 作者指南。整理知识库和工具。阶段的工作文件（简报、进度、审核报告、修复简报）在用户验收后删除，代码和测试里引用的审核编号整理成 `history/` 下的索引。
7. 停下来等用户手动验收，通过后才启动下一阶段。

## 验收分级

依据见 [ADR 0016](adr/0016-test-rig-and-manual-scenarios.md)。

- **离线**：xmake 单元测试（事件中枢、沙箱、调度不依赖游戏），特征码离线校验。
- **自检**：在 guest 上启动测试目录里的游戏，自检后退出，停留在主菜单，不进存档。
- **场景**：真实走到城镇、地牢、战斗的流程，由用户操作，日志作证据。

## 工具

所有命令都在 guest 上运行，工作目录是 `D:\workplace`。

| 要做的事 | 命令 |
| --- | --- |
| 配置环境 | `source DD.ModLoader/tools/env.sh`，默认不部署进真实安装 |
| 配置并构建 | `dd_config`，然后 `xmake build <目标>` |
| 测试目录 | `python -B DD.ModLoader/tools/rig.py <命令>`，`status`、`deploy`、`run`、`save-log`、`compare-log`、`verify-real` 等，见 `--help` |
| 保存一次运行的日志 | `python -B DD.ModLoader/tools/rig.py save-log <名字>`，复制到 `build/rig_logs/<名字>/`，保留修改时间并记下方案和部署时间，同名拒绝 |
| 特征码离线校验 | `python -B DD.ModLoader/tools/check_sigs.py [exe...]`，默认 Steam 版和离线版 |
| Actor 偏移校验 | `python -B DD.ModLoader/tools/check_actor_layout.py`，核对 DDCore 里 actor 字段的偏移常量和战斗计数清零点，游戏更新后运行 |
| Lua 类型声明 | `python -B DD.ModLoader/tools/gen_lua_types.py`，从 `sdk/lua/topics.json`（`rig.py deploy topics` 运行一次导出）生成 `sdk/lua/dd_topics.lua`，`--check` 只核对 |
| Lua 离线测试 | `xmake build dd_lua_tests && xmake run dd_lua_tests`（`-v` 打印日志），沙箱、预算、生命周期、事件、调度、句柄 |
| P3 场景检查 | `python -B DD.ModLoader/tools/p3_scenario_check.py`，逐项判断场景日志通过、失败或没有触发；`--selftest DD.ModLoader/build/p3d_rehearsal` 用 `dd_lua_tests` 生成的预演日志自测；用法见 [09-lua-scenario.md](09-lua-scenario.md) |
| 运行中比对 .text | `python -B DD.ModLoader/tools/rig_textdiff.py`，列出被改动的区间，确认没有多余的 hook |
| 更新事件覆盖表 | `python -B DD.ModLoader/tools/event_coverage.py`，读 `test_events.log`，重新生成 `docs/07-event-coverage.md` |
| 看函数 | `bash knowledge/tools/ghidra/decompile.sh <RVA>...`，或 `python -B knowledge/tools/dd_disasm.py` |
| 游戏更新后 | `bash knowledge/tools/regen.sh --ghidra` |

测试目录是 `steamapps\common\DarkestDungeon_Dev`，数据目录联接到真实安装，`dbghelp.dll`、配置和日志都是自己的。任何阶段都用它测试，真实安装里的文件不动。新增的产物要加进 `rig.py` 的 `PROFILES`。和框架冲突的 Mod 加进 `rig.py` 的 `UNINSTALLED_MODS`，再运行 `refresh`，它只从测试目录卸载。换测试目录用环境变量 `DD_RIG_NAME`，只接受 `steamapps\common` 下 `DarkestDungeon_` 开头的一层目录名；会写、会删或会启动游戏的命令都先检查测试目录本身（不是联接或符号链接，实际位置不是真实安装、也不和它互相包含），有问题时什么都不动；`refresh`、`deploy`、`run` 还检查要写的子目录不是联接。文件这一层：`deploy` 只删 `.rig.json` 里记录的、某个方案确实会部署的相对路径，记录不对时整个拒绝；覆盖写入的目标是符号链接、联接或硬链接时先移除测试目录里的这个名字再写新文件，`status` 会列出这样的文件；`verify-real` 在真实安装的基线文件缺失、是链接或有多个硬链接时报出来，不抛异常。检查和写入之间仍有很短的窗口，并发的外部进程在这段时间里把目录或文件换成链接时察觉不到：`rig.py` 防误操作和遗留状态，不防并发的恶意进程。

## 实现简报

每次派发都写明：

- 阶段目标和范围，明确列出不做的内容
- 开工前要读的文档和知识库主题
- 验收清单
- 约束：不 commit、不碰真实安装、启动游戏前确认 guest 上没有 `Darkest.exe` 在运行、遇到 360 拦截就停下
- 报告：构建结果、每条验收的证据、改动文件、没做到的事和失败的尝试

## 审核简报

审核者的任务是证伪，只审核不修改。给阶段要求、产物位置和 [04-roadmap.md](04-roadmap.md) 的审核重点，不给实现者的结论。

除阶段自己的重点外，每次都检查：

- 逆向结论要自己读反汇编或反编译结果复核，不采信知识库和实现者的说法
- 每个 [运行] 标记都要有对应的运行证据，覆盖范围不能超过实际测到的
- 自造的测试条件（如手动调用游戏函数）不能当作真实路径的证据
- 发布的产物本身要实际运行过，而不只是共用源码的测试版本
- 给用户的脚本和操作步骤要实际跑过
- 进入用户手动验收之前，任何可能损坏存档、卡死或让验证失效的问题都是阻断项

和现有 DD 插件的共存、新旧版本的搭配都不算发现项（[ADR 0017](adr/0017-no-legacy-plugin-compat.md)、[ADR 0018](adr/0018-no-version-compat-until-p7.md)）。

报告分为阻断、应修、备注，每条写位置、问题、触发条件、证据、修法。无法确认的问题单列并说明缺什么证据，最后写清楚查过且没发现问题的范围。
