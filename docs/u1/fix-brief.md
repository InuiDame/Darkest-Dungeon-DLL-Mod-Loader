# U1 修复简报（审核 A 之后）

阶段工作文件，用户验收后删除。审核报告见 [review-a.md](review-a.md)，原简报见 [brief.md](brief.md)。审核有 2 个阻断项，修完要重新审核。

分两个代理按顺序修：**FA**（清单和 C++）先做，**FB**（Python 工具、自测、文档）在 FA 之后接手。报告分别写到 `fa-report.md`、`fb-report.md`。

## 主线的决定

- **S1 按 ADR 0036 自己的定义修**。「写」= 写游戏内存、调用游戏函数或改变结算；装内联 hook、改虚表槽、调用游戏函数都算写，包括只读主题背后的 hook，以及 `persist`、`game.actor_info`。这是修正 A 方案的实现，不是改成 B 或 C。代价（未知版本上停用的能力变多）要写进 ADR 0036 和 08 的「需要用户决定」，方案 A/B/C 仍由用户定。
- **运行时补便宜的检查**：HOOK 的命中必须是函数入口（C++ 已有 `FunctionTable`），已知版本和未知版本都做。能补运行时依据的就补上：`VtSlotStress` 照审核 S2 的建议（槽 36 的函数调用 480FE0，加 ANCHOR 和 REL32 依据）；`VtSlotHeal` 补一条能区分槽 50、51 互换的依据。
- **运行时做不到的写明是局限**：依据误绑定到邻居字段（`EffectDotBurn` 样本）、诱饵函数（`BuffRemoveByGuid` 样本）没有基线就发现不了，写进 ADR 0036。能做的缓解是把靠邻居偏移区分的依据模式写得更具体（多带上下文），FA 盘点这一类并收紧。
- **B1**：依据位置核对给出「没法对照」（UNKNOWN）时一律不算通过。verify 把它归为「无法确认」，退出码 2。repair 对这类项最多给「需要人工」，除非用独立的手段（另一条依据、指令对齐的完整序列比对）得到一致的值。accept 在有这类项时拒绝。同时改进对齐（插入、删除指令时用真正的序列对齐，而不是唯一窗口），减少 UNKNOWN 的数量；但改进对齐不能替代「UNKNOWN 不算通过」。
- **B2**：accept 不能把运行时发现不了的失效记为已知版本（OFFLINE 依据失效、没有依据的项按 S4 被升级、UNKNOWN）。`--allow-optional` 只能用于运行时自己会发现并停用的失效（可选特征码缺失、RUNTIME 依据不成立），拒绝信息不能推荐用它绕过其他失效。
- **S4**：清单项加上「组」（同一张虚表、同一个结构）。可以加一个只给 Python 用的宏，C++ 忽略。同组有 RUNTIME 项的值变了时，组里的 NONE、OFFLINE 项升级成「需要人工」。accept 要求对这些项和全部 NONE 项显式确认（例如 `--confirm <清单文件>`，每项写核实方式和证据路径），确认记录存进仓库，可以追溯。
- 备注 1–7 都修。备注 8、10 是数据不足，在 ADR 0035 和 08 里如实写。

## 分工

### FA：清单和 C++（`core/`、`loader/`、`tests/`、`layout.inc`、`signatures.inc`、ADR 0036）

1. S1：重新归类依赖表里的词（hook 主题、`persist`、`game.actor_info` 等），C++ 的停用开关照新归类工作；单元测试里的 `TestPolicy` 跟着改，比如审核 S2 指出的 `stress.post` 断言。
2. 运行时的 HOOK 入口检查。`cpp_compare` 会因此少一项「Python 独有的检查」，FB 会改 `cpp_compare.py`，FA 只需在报告里写明。
3. S2：`VtSlotStress` 补运行时依据；`VtSlotHeal` 补依据。
4. S3：给被 hook 和被调用的函数登记原型项，给按固定宽度读写的字段登记宽度和类型项（用 `DD_UNVERIFIED` 加依赖表，格式尽量不变）；补登 `EntryGuid`、`StdVectorFirst`、伪造 Listener 虚表只用槽 0 的假设、RTTI 结构大小。
5. 收紧靠邻居偏移区分的依据模式（见上）。
6. 备注 1（`signatures.inc` 的过时注释）、备注 2（C++ 和 Python 对 LOADER GLOBAL 的检查统一，Python 那一侧由 FB 改，FA 在报告里写明要怎么改）、备注 4（加载器的 `steal >= 5` 检查放 C++ 一侧，verify 一侧由 FB 加）。
7. ADR 0036 更新：新归类、局限、代价、给用户的选项。
8. 收尾：
   - 重新生成基线；
   - `xmake build -r` 构建全部目标，`dd_core_tests`、`dd_lua_tests` 通过；
   - verify（Steam 版、离线版）退出码 0；
   - `cpp_compare` 一致，这时可能会有 HOOK 入口检查带来的预期差异，写明即可，由 FB 修对照脚本；
   - 测试目录自检到主菜单（已知版本：全部通过、不停用），日志存为 `u1fa-selfcheck`；
   - 用 `DD_LAYOUT_ASSUME_UNKNOWN=1` 再跑一次，日志存为 `u1fa-unknown`，报告里列出未知版本上停用了什么、还剩什么；
   - `rig_textdiff.py` 没有多余补丁；
   - 测试目录部署回 `scenario4`。

### FB：Python 工具、自测、文档（`tools/`、`docs/`、`knowledge/README.md`、`templates/`）

1. B1、B2、S4、S5、S6 照上面的决定修；备注 2（Python 侧）、3、4（verify 侧）、5、6 也修。备注 5 的离线检查只查「存在」的，能改成有位置约束就改，改不了的在清单里把对应项降为 NONE，或者写明弱点。
2. S7：变异样本按「新字段有完整对应代码」重做。至少包括审核 B1 的两个样本：`Buff::Instance` 在 0x44 插入、删除一个 int，带复制指令，原地重排；以及 `evidence_misbind` 补上新字段的解析代码。再加一个「模板孪生且两份都有调用者」的样本（审核「无法确认」第 2 条）。审核的脚本在 guest 的 `build/u1review/`（`mut_inst44.py`、`mut_inst_remove.py`、`mut_statelast.py`），可以直接吸收。每个样本断言 verify 和 repair 的结果，不能修错。
3. `cpp_compare.py`：`static_only` 只收失败原因全部属于静态检查集合的特征码；同时适配 FA 的 HOOK 入口检查。审核 S5 的故意改坏实验（去掉 C++ 的 .data/.rdata 检查）要能被对照测试抓住，写成自测的一部分或报告里的复现。
4. 文档：ADR 0035（B1、B2、S4 的新规则）、`06-workflow.md` 的手册（accept 的确认步骤、UNKNOWN 的处理）、`08-handoff.md`（备注 7 的描述改成和停用开关的实际结果一致，加上 S1 新归类的代价、用户待定事项的更新）、`knowledge/README.md`（备注 6）、`04-roadmap.md`。
5. 收尾：
   - `selftest` 全部通过，盲定位和全部样本错误为 0；
   - verify（Steam 版、离线版）退出码 0；
   - 手册里改动过的步骤实际跑过；
   - 审核 B1、B2 的复现步骤重放，确认已经挡住：`inst44.exe`、`inst_remove44.exe` 要报无法确认或修对，不能被 accept；`enum.exe` 不能被 accept。

## 共同约束

照原简报「约束」一节：
- 在 guest 上构建和跑工具，用 ToolSearch 加载 `mcp__duet__worker_*` 工具；
- 不 commit，不碰真实安装，遇到 360 拦截就停下；
- 游戏只用 `rig.py` 在测试目录跑，启动前确认没有 `Darkest.exe` 在运行；
- 中间产物放 guest 的 `build/u1fa/`、`build/u1fb/`。

不要读其他代理的结论来代替自己的验证；审核报告是要处理的问题清单，里面的每条发现都要自己复核后再修。

## 报告

写到 `fa-report.md` 或 `fb-report.md`：逐条写审核编号、处理结果和证据，改动文件，没做到的事，留给下一个代理的事项。
