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
| 特征码和布局校验 | `python -B DD.ModLoader/tools/dd_update.py verify [exe...]`，默认 Steam 版和离线版：`signatures.inc` 的全部检查（含 HOOK 命中位置必须是函数入口、偷取不少于 5 字节）、命中的函数和基线（`tools/baseline/`）是不是同一个、依据唯一命中的位置是不是基线里那条指令（依据位置核对：对不上是误绑定，没法对照又没有别的依据佐证是「无法确认」）、`layout.inc` 每条依据求值、同组（`DD_GROUP`）有成员变了时列出组里要人工核实的项，报出受影响的服务和主题；`--json <文件>` 写机器可读的报告，`--only sigs,identity,layout` 只做其中几项，`--tree <源码副本>` 用副本里的清单和基线，`--sigs <文件>` 校验插件自己的特征码文件；退出码 0 通过、1 只有可选项失效、2 必需项失效、有误绑定或无法确认、3 清单或参数有错。`check_sigs.py`、`check_actor_layout.py` 是它的薄封装（`--only sigs,identity`、`--only layout`，两个都含依据位置核对），新文档不再用这两个名字 |
| 修复 | `python -B DD.ModLoader/tools/dd_update.py repair --exe <新 exe> [--apply] [--tree <源码副本>] [--old-exe <旧 exe>] [--out <目录>]`：对 verify 失败的项在新 exe 里定位，重新生成特征码、偏移、依据模式，预演 verify；输出 `report.txt`、`report.json`、`signatures.diff`、`layout.diff`、`rva_map.tsv`（默认 `build/repair/<SHA1 前 12 位>/`）。`--apply` 只改清单（`--tree` 时改副本）并再 verify，不记为已知版本。退出码 0 没有需要人工的项、1 只有可选项需要人工、2 有必需项需要人工 |
| 确认新版本 | `python -B DD.ModLoader/tools/dd_update.py accept --exe <新 exe> --label <版本名> [--replace] [--tree <源码副本>] [--confirm <清单>] [--confirm-template <文件>] [--allow-optional]`：把 SHA1 加进 `DD_KNOWN_BUILD`、生成 `tools/baseline/<版本名>.json`、把确认清单存成 `tools/baseline/confirm/<版本名>.json`。规则见 [ADR 0035](adr/0035-update-pipeline.md)「确认新版本」：必需项失效、误绑定、只有 verify 做的静态检查不过、离线依据不过一律拒绝；`--allow-optional` 只放行运行时自己会发现的失效；全部 NONE 项、同组升级的项、依据位置无法确认的项要在 `--confirm` 的清单里逐项写核实方式和证据路径，`--confirm-template` 生成要填的清单（不接受）；`--replace` 去掉同名的旧版本行 |
| 基线快照 | `python -B DD.ModLoader/tools/dd_update.py baseline`，重新生成 `tools/baseline/steam.json`、`nosteam.json`（format 2，同一个 exe 两次生成字节一致）；改了 `signatures.inc` 的特征码、`layout.inc` 的依据，或者改了 `tools/ddupdate/baseline.py`，都要重新生成 |
| 自测 | `python -B DD.ModLoader/tools/dd_update.py selftest`：跨版本盲定位、变异样本（`tools/ddupdate/mutants.py`，生成到 `build/u1d/mut/`，13 个）、清单变异冒烟、`repair --apply`/编译/`accept`（含确认清单）的往返、规则自检（对照测试的分类、加载器 GLOBAL、偷取字节、accept 的拦截、审核 A 的 B1 原始反例）、C++/Python 对照，退出码 0 才算通过，约 40 分钟；`--only`、`--samples` 只跑一部分。要先 `xmake build dd_layout_compare`。只跑盲定位：`dd_update.py blind` |
| C++/Python 对照 | `xmake build dd_layout_compare`，然后 `python -B DD.ModLoader/tools/ddupdate/cpp_compare.py [exe...]`：DDCore 的运行时核对（按节映射 exe，不执行）和 verify 逐项比较，退出码 0 全部一致；Python 独有的检查（失效原因全是类别 `static` 的 HOOK 静态检查和 `DD_ENTRY`、依据位置核对）单列，原因里有运行时也做的检查而 C++ 判成立的算不一致；`--self-check` 只跑分类规则的自检。U1b 的失败路径样本用 `python -B DD.ModLoader/tests/layout_compare/make_mutants.py` 生成 |
| 写布局依据 | `python -B DD.ModLoader/tools/dd_update.py suggest <清单项> --sig <特征码> [--origin HIT\|TARGET\|VSLOT --slot N] [--rel 项 --mul N]`，在函数里找等于常量的字段，给出两个版本都通过的 `DD_EVIDENCE` 候选；格式见 `core/src/game/layout.inc` 开头 |
| 相似度数据 | `python -B DD.ModLoader/tools/dd_update.py simstats --json <文件>`，同一函数跨版本、最近冒充者、随机函数对的相似度分布（同一性阈值的依据） |
| Lua 类型声明 | `python -B DD.ModLoader/tools/gen_lua_types.py`，从 `sdk/lua/topics.json`（`rig.py deploy topics` 运行一次导出）生成 `sdk/lua/dd_topics.lua`，`--check` 只核对 |
| Lua 离线测试 | `xmake build dd_lua_tests && xmake run dd_lua_tests`（`-v` 打印日志），沙箱、预算、生命周期、事件、调度、句柄 |
| P3 场景检查 | `python -B DD.ModLoader/tools/p3_scenario_check.py`，逐项判断场景日志通过、失败或没有触发；`--selftest DD.ModLoader/build/p3d_rehearsal` 用 `dd_lua_tests` 生成的预演日志自测；用法见 [09-lua-scenario.md](09-lua-scenario.md) |
| P4 场景检查 | `python -B DD.ModLoader/tools/p4_scenario_check.py --log-dir <scenario4 的日志> --combat-log-dir <combat 的日志>`，`--selftest` 用 `dd_lua_tests` 生成的 `build/p4c_rehearsal` 自测；构造旁路不一致用 `rig.py tamper-persist`，重跑前清旁路用 `rig.py reset-persist`；用法见 [10-p4-scenario.md](10-p4-scenario.md) |
| 探针日志分析 | `python -B DD.ModLoader/tools/p4_probe_report.py --log-dir <目录>`，逐项判定 P4 探针（`probe_p4`，方案 `probe4`）的待核实清单 |
| 运行中比对 .text | `python -B DD.ModLoader/tools/rig_textdiff.py`，列出被改动的区间，确认没有多余的 hook |
| 更新事件覆盖表 | `python -B DD.ModLoader/tools/event_coverage.py`，读 `test_events.log`，重新生成 `docs/07-event-coverage.md` |
| 看函数 | `bash knowledge/tools/ghidra/decompile.sh <RVA>...`，或 `python -B knowledge/tools/dd_disasm.py` |
| 游戏更新后 | 见下面「游戏更新后」的操作手册 |

测试目录是 `steamapps\common\DarkestDungeon_Dev`，数据目录联接到真实安装，`dbghelp.dll`、配置和日志都是自己的。任何阶段都用它测试，真实安装里的文件不动。新增的产物要加进 `rig.py` 的 `PROFILES`。和框架冲突的 Mod 加进 `rig.py` 的 `UNINSTALLED_MODS`，再运行 `refresh`，它只从测试目录卸载。换测试目录用环境变量 `DD_RIG_NAME`，只接受 `steamapps\common` 下 `DarkestDungeon_` 开头的一层目录名；会写、会删或会启动游戏的命令都先检查测试目录本身（不是联接或符号链接，实际位置不是真实安装、也不和它互相包含），有问题时什么都不动；`refresh`、`deploy`、`run` 还检查要写的子目录不是联接。文件这一层：`deploy` 只删 `.rig.json` 里记录的、某个方案确实会部署的相对路径，记录不对时整个拒绝；覆盖写入的目标是符号链接、联接或硬链接时先移除测试目录里的这个名字再写新文件，`status` 会列出这样的文件；`verify-real` 在真实安装的基线文件缺失、是链接或有多个硬链接时报出来，不抛异常。检查和写入之间仍有很短的窗口，并发的外部进程在这段时间里把目录或文件换成链接时察觉不到：`rig.py` 防误操作和遗留状态，不防并发的恶意进程。

## 游戏更新后

流水线的取舍见 [ADR 0035](adr/0035-update-pipeline.md)，运行时怎么处理未知版本见 [ADR 0036](adr/0036-runtime-layout-check.md)。在玩家那边，DDCore 遇到新版本会自己核对布局、停用不能核对的写能力；这里是我们这边把新版本变成已知版本的步骤。命令都在 guest 的 `D:\workplace` 下运行，先 `source DD.ModLoader/tools/env.sh`。想先在源码副本里试一遍时，把仓库复制一份（`selftest` 的往返就是这样做的），`verify`、`repair`、`accept` 都加 `--tree <副本>`，构建加 `-P <副本>`，`cpp_compare.py` 加 `--tree <副本> --bin <副本>/build/windows/x64/releasedbg/dd_layout_compare.exe`（用仓库里的工具，副本里的工具找不到工作区根目录的 `knowledge/`）。

1. **拿到新 exe**。Steam 更新后真实安装里的 `_windows\win64\Darkest.exe`（离线版是 `_windowsnosteam\win64\Darkest.exe`），工具只读它。想要完整的旧→新 RVA 对照表，更新前把旧 exe 复制一份到 `DD.ModLoader/build/` 下，后面加 `--old-exe`；没有旧 exe 时修复只靠基线，对照表只有清单涉及的函数。
2. **校验**：`python -B DD.ModLoader/tools/dd_update.py verify "<新 exe>" --json DD.ModLoader/build/update/verify.json`。开头打印新 exe 的 SHA1 和「未知版本」。退出码 0：布局没变，跳到第 5 步；1：只有可选项失效；2：必需项失效、有误绑定或无法确认（必需项失效时 DDCore 在玩家那边会整体停用）；3：清单或参数错。「失效影响」一节列出受影响的服务和主题。「依据位置」一节里两种行都必须处理：
   - `BIND`：依据误绑定（依据照样成立，命中的却不是原来那条指令）。
   - `UNKN`：无法确认。依据成立、唯一命中，但命中位置没法和基线对照，同一项又没有别的位置对得上的依据。最常见的原因是一串形状相同的字段复制里多了或少了一个字段，按指令分不出多的是哪一个。它和 `BIND` 一样不算通过：值可能是邻居字段的（审核 A 的 B1）。
   - `--`：没法对照，但不计入（依据本身不成立，已经按失效处理；或者同一项有别的依据佐证，行尾写明）。
   「同组有变化」一节（`GRP`）列出和变了的项同组、自己又没有运行时依据的项，第 7 步要逐项确认。
3. **修复（只读）**：`python -B DD.ModLoader/tools/dd_update.py repair --exe "<新 exe>" [--old-exe <旧 exe 副本>] --out DD.ModLoader/build/update/repair`。看 `report.txt`：
   - `FIX高`：结构上有支持（放宽的特征码命中、调用图约束全部满足、唯一的锚点），可以直接接受。
   - `FIX中`：只靠指纹、靠调用者区分了模板孪生、指令对齐或按依据搜索得到的。逐条核对：报告里有新地址和手段，用 `python -B knowledge/tools/dd_disasm.py`（`d <新 RVA>`）看新位置的反汇编，和知识库里这个函数的说明、基线里的旧函数对照。
   - `MANU`：需要人工，按原因处理。生成不了唯一的特征码：手写更长的模式；hook 入口不安全：换偷取字节数或换 hook 点；只有离线依据的项（枚举表、Effect 字段）：按离线检查的输出对照反汇编改值；值写在依据模式字面量里、起点对不上、多条依据不一致：读那条依据的注释和指令，手改 `layout.inc`。人工改的项直接编辑 `signatures.inc`、`layout.inc`。
   - `MANU` 里的「位置无法确认」（B1）：报告写了放宽的模式命中在哪条指令、读到什么值，但这个位置没有指令对齐的确认，可能正好错开一个字段。用 `dd_disasm.py` 的 `d <函数 RVA>` 看整串复制，数清新字段插在哪里（对照构造函数、其他读这个结构的函数），确定每个字段的新偏移，再改值和依据模式里的字面量。改完 verify 多半还是 `UNKN`（按指令本来就对不齐），这是预期的：把核实过程和证据写进第 7 步的确认清单。
   - `MANU` 里的「同组 … 变了」（S4）：组里有成员的值变了或被重新定位，这一项没有运行时依据。按它的 `DD_UNVERIFIED` 写的办法核实（原型项看调用点和入口的参数用法，宽度项看读写它的指令），第 7 步写进确认清单。
   - 「更新后要人工核实」一节是没有静态依据的项（`DD_UNVERIFIED`），按每组写的办法核实，主要是第 6 步的自检和探针。
   - `signatures.diff`、`layout.diff` 是要写进清单的补丁，`rva_map.tsv` 是旧→新地址。退出码 0 没有需要人工的项，1 只有可选项，2 有必需项（必需项不修好 DDCore 会整体停用）。
4. **应用**：`python -B DD.ModLoader/tools/dd_update.py repair --exe "<新 exe>" --apply --out DD.ModLoader/build/update/repair`，写进清单后自动再 verify，要求退出码 0；不是 0 时回第 3 步处理剩下的项，人工改完再跑 `verify`。唯一可以留下的是人工核实过的 `UNKN`（无法确认，退出码 2）：它们靠第 7 步的确认清单放行，别的失效都要修掉。值改了的清单项要同步改 `tests/unit/layout_golden.cpp` 里对应的断言（它是 U1a 时的旧值快照，防止无意中改值；每改一行注明原因）。
5. **构建和单元测试**：`cd DD.ModLoader`（xmake 要在有 `xmake.lua` 的目录里跑），`dd_config`，然后 `xmake build DDCore`、`DDModLoader`、`DDLua`、`dd_core_tests`、`dd_lua_tests`、`dd_layout_compare`，`xmake run dd_core_tests` 和 `xmake run dd_lua_tests` 失败 0；回到 `D:\workplace` 再跑 `python -B DD.ModLoader/tools/ddupdate/cpp_compare.py "<新 exe>"` 退出码 0（C++ 的运行时核对和 verify 结论一致）。
6. **测试目录自检**：真实安装已经是新版本，测试目录的数据联接过去。先 `tasklist` 确认没有 `Darkest.exe` 在运行，再 `python -B DD.ModLoader/tools/rig.py deploy scenario4`、`rig.py run --seconds 150`、`rig.py save-log <名字>`，运行中另开一个命令跑 `python -B DD.ModLoader/tools/rig_textdiff.py`。看 `DDCore.log` 第一屏的「布局核对」：运行时依据全部成立（`不成立 0、没法求值 0`）。还没 accept 时是未知版本，按 ADR 0036 会停用不能核对的写能力，日志列出停用了什么，这是预期的；DDLua.log、DDCore.log 没有新的 WARN/ERROR，`rig_textdiff.py` 没有多余的补丁。没有静态依据的项按 verify 里的分组核实：
   - 事件和 Query 负载、`EventTime`、`TargetHit`、`VtSlotStress`：`rig.py deploy events`，跑 `test_events` 的场景，再 `python -B DD.ModLoader/tools/event_coverage.py` 对照 `docs/07-event-coverage.md`。
   - 名册、Mod 列表（`EntryHero`、`kUgc*`、`UserServicesInstalled`）：`rig.py deploy probe`（`probe_p3`）进存档。
   - 技能表、Effect 前缀、`EffectApplyResultSize`、`kSkill*`：`rig.py deploy probe4`（`probe_p4`）或 `combat`（`test_combat`）进战斗，`python -B DD.ModLoader/tools/p4_probe_report.py --log-dir <日志>` 逐项判定。
   - 游戏名字表（sub_stat、`EffectTargets`、内置状态）、`GameHashMul`、MSVC 标准库布局：按 `DD_UNVERIFIED` 的说明对照 `dd_disasm.py`、`decompile.sh`。
   进战斗的场景要用户操作（[ADR 0016](adr/0016-test-rig-and-manual-scenarios.md)），交给用户前先做一次场景就绪检查。
7. **确认新版本**：已知版本上 DDCore 不再停用只能离线核对、没有静态依据的项，所以 accept 要求运行时核对不了的项都人工确认过（[ADR 0035](adr/0035-update-pipeline.md)「确认新版本」）。
   1. 生成确认清单：`python -B DD.ModLoader/tools/dd_update.py accept --exe "<新 exe>" --label <版本名> --confirm-template DD.ModLoader/build/update/confirm.json`。清单列出全部 NONE 项、同组升级的项和依据位置无法确认的项，每项的 `why` 写着为什么要确认（NONE 项附 `DD_UNVERIFIED` 的说明）。这一步不接受。
   2. 逐项填 `method`（怎么核实的：第 6 步的哪个自检或探针、对照了哪段反汇编）和 `evidence`（证据路径，分号分隔，相对 `D:\workplace` 或 `DD.ModLoader`，accept 时要存在，例如 `DD.ModLoader/build/rig_logs/<名字>/DDCore.log`）。几项共用同一份证据可以，但每项都要写。
   3. `python -B DD.ModLoader/tools/dd_update.py accept --exe "<新 exe>" --label <版本名> --confirm DD.ModLoader/build/update/confirm.json [--replace]`。拒绝时看原因：必需项失效、误绑定、静态检查或离线依据不过要回第 3 步修，`--allow-optional` 和 `--confirm` 都不管用；只剩运行时自己会发现的失效（可选特征码、可选 RUNTIME 项的运行时依据）时，确实要带着它们发版才加 `--allow-optional`（玩家那边对应的能力停用）。
   4. 接受后提交 `signatures.inc` 的 `DD_KNOWN_BUILD`、`tools/baseline/<版本名>.json` 和 `tools/baseline/confirm/<版本名>.json`。
   新版本取代旧版本时（Steam 自动更新）用原来的标签加 `--replace`（例如 `--label steam --replace`），会去掉旧的 `DD_KNOWN_BUILD` 行、重新生成 `tools/baseline/steam.json`；两个版本都要支持时用新标签。`DD_KNOWN_BUILD` 编进 DDCore，所以 accept 之后重新构建 DDCore，再按第 6 步自检一次：「已知版本」，「全部通过，没有停用任何能力」。
8. **更新知识库的 RVA**：知识库的地址是基准版本（Steam 版）的。用第 3 步的 `rva_map.tsv`（`old`、`new` 两列，`names` 是清单里的名字）逐条替换 `knowledge/dd1/*.md` 里的 RVA，对照表里没有的地址用 `dd_disasm.py` 的 `x`、`s` 命令重新找；`knowledge/dd1/hooks.md` 的地址要先换，`verify` 和别的插件的重叠检查只在标签是 `steam` 的 exe 上按这张表比较。改完把 `knowledge/README.md` 的 SHA1 和文件大小换成新版本。
9. **重建生成表和 Ghidra 工程**：`bash knowledge/tools/regen.sh --ghidra`（约 7 分钟）。Steam 版和离线版都换成新版本、基线都重新生成之后，`dd_update.py blind` 跑一遍盲定位。`selftest` 的变异样本、清单变异冒烟和往返是照 Steam 版 `9db2d73b`、离线版 `3f133c26` 和它们的基线写的，旧版本被替换后要留一份旧 exe 和旧基线才能接着跑（见 `tools/ddupdate/mutants.py` 开头）。

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
