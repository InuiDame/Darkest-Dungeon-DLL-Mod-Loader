# U1 简报：游戏更新的校验与修复

阶段工作文件，用户验收后删除。路线图条目见 [04-roadmap.md](../04-roadmap.md) 的 U1，流程见 [06-workflow.md](../06-workflow.md)。

## 目标

游戏更新会改动 `Darkest.exe` 的二进制布局：函数挪位置、编译器换寄存器和栈帧、结构加字段导致偏移整体后移、虚表加槽位。U1 要把现在「特征码 + 散落的偏移常量 + 两个手写校验脚本」的做法改成一条可验证、可修复的自动化流程：

1. **校验**：拿到任意 exe，一条命令逐项判定加载器、DDCore 依赖的每个地址和每个结构偏移还成不成立，报出受影响的服务和主题。
2. **修复**：失效项在新 exe 里自动定位，重新生成特征码和偏移，输出补丁和逐项可信度；应用后自动再校验；人工确认后记为已知版本、刷新基线。
3. **运行时兜底**：玩家的游戏被 Steam 自动更新、我们还没跑工具时，DDCore 自己能发现布局变化，只停用依赖它的能力，不读写错位置。

最危险的是**误绑定**和**错误修复**：特征码仍然唯一命中，命中的却是别的函数；或者修复工具给出一个看似合理的错地址、错偏移。这两种比「报失效」糟得多，会导致崩溃或存档损坏。整个设计的取舍按这个排序：宁可报「需要人工」，不给错的结论。

## 现状（开工前自己核实）

- 地址：`core/src/game/signatures.inc`（X-macro，约 60 条，`DD_SIG`、`DD_ENTRY`、`DD_CHAIN_TARGET`、`DD_RUNTIME_SELECT`、`DD_KNOWN_BUILD`），加载器（`loader/early_hook.h`）、DDCore（`core/src/game/game.cpp` 的 `Resolve`）、`tools/check_sigs.py` 共用。依据见 [ADR 0006](../adr/0006-signature-addressing.md)。
- 结构偏移：C++ 常量散落在 `core/src/game/actors.h`（`actor_layout`）、`core/src/combat/combat.cpp`（`layout`）、`core/src/combat/logic.cpp`（枚举表、来源标志）、`core/src/events/system.cpp`（事件和 Query 负载的字段偏移、actor 生命和压力、虚表槽位、listener 容器大小）、`core/src/persist/persist.cpp`、`core/src/game/mods.cpp`、`core/src/game/phase_tracker.*`、`core/src/game/game.cpp`（`MonsterBlockReturn` 的 +52/+56 等）。可能还有别处，盘点要自己做全。
- `tools/check_actor_layout.py` 用手写的特征码核对其中一部分偏移；事件负载偏移没有静态核对，只有 P2 的 `test_events` 运行证据。
- 运行时：已知 SHA1 之外的 exe，特征码全部命中就只警告（`DD_KNOWN_BUILD`），偏移变了发现不了。
- SDK：插件经 hook 服务的 `scan(pattern)` 用自己的特征码（`sdk/include/dd/dd_api.h`），没有任何校验工具。
- 可用的 exe：guest 上 Steam 版 `9db2d73b…` 和离线版 `3f133c26…`（路径见 `check_sigs.py` 的 `EXES`），RVA 不同、结构相同。**没有旧版本或新版本的 exe**，下载旧版要 Steam 账号，不在范围内。所以自测靠两件事：两个版本互相当作「更新前后」，以及在 `build/` 下人工构造的变异样本。

## 范围

### 1. 布局清单（单一来源）

- 在 `signatures.inc` 之外新增布局清单（建议 `core/src/game/layout.inc`，同样是 X-macro，格式自定），登记 DDCore 和加载器用到的**全部**游戏布局：结构偏移和大小、虚表槽位、枚举表、事件和 Query 负载偏移、特征码命中位置上的固定偏移（如 `MonsterBlockReturn` 的 +52）。C++ 改为从清单取常量，删掉散落的定义；常量值不变，行为不变。
- 每一项至少带一种依据，或者明确标成「没有静态依据」并写明怎么核实（例如事件负载偏移靠 `test_events` 自检、技能表大小靠探针）。依据要能机器求值：
  - 可在运行时求值的依据（C++ 和 Python 都实现，语义一致）：建议几种形状，例如「锚点（某条特征码解析出的函数）的函数范围内，某个带 `??` 的模式唯一命中，命中位置 +k 处宽 w 的值等于常量」、「虚表第 n 槽的函数以某模式开头，捕获值等于常量」、「锚点后固定窗口内」。函数范围按 `.pdata`，C++ 自己解析异常目录，不依赖 `RtlLookupFunctionEntry`（以便对非加载的映像做对照测试）。
  - 只能离线求值的依据（Python）：如 `check_actor_layout.py` 里从静态初始化函数还原枚举表、Effect 解析函数里按读取函数分组字段类型。保留这些逻辑，按名字挂到清单项上。
- 每一项写明依赖它的服务和主题（例如 `kInstDuration` → combat 服务的 buff 读写、`buff.tick`），特征码也一样。校验报告和运行时停用都按这张依赖表。
- 交付盘点表（放进 U1a 的报告）：逐个文件列出原有的布局常量、登记到清单的哪一项、用的哪种依据；不属于游戏布局的常量（缓冲区大小、x86 编码等）单列并说明为什么不算。

### 2. 基线快照

- 对已知版本（至少 Steam 版和离线版）生成基线，存进仓库（建议 `tools/baseline/`）：每条特征码命中的函数（GLOBAL 是引用它的函数）的指纹（规范化后的指令序列、大小、`.pdata` 范围）、调用者和被调用者、引用的字符串和全局、有区分度的立即数，以及每条依据在函数里命中的位置。
- 有生成命令；同一个 exe 两次生成的结果字节一致。指纹的相似度度量要用数据验证：同一函数在 Steam 版和离线版之间的相似度、随机不同函数之间的相似度，分布和选的阈值写进报告。

### 3. 校验

- 一条命令（建议 `python -B DD.ModLoader/tools/dd_update.py verify [exe...]`，模块放 `tools/` 下自定）对 exe 输出逐项结论，人读摘要加机器可读的 JSON：
  - 特征码：`check_sigs.py` 的全部现有检查（唯一命中、GLOBAL_ANY 的规则、偷取范围在指令边界且不含分支和 rip 相对、函数内没有分支跳进偷取范围、`DD_ENTRY` 和实际字节一致、和 `knowledge/dd1/hooks.md` 清单不重叠）。
  - 同一性：命中的函数和基线里的是不是同一个函数（指纹相似度、调用关系），防误绑定。
  - 布局：每条依据求值，和常量比较。
  - 没有静态依据的项单列成「更新后要人工或靠自检核实」。
  - 每个失效项报出受影响的服务和主题。
  - 退出码区分：全部通过 / 只有可选项失效 / 必需项失效或有误绑定。
- `check_sigs.py`、`check_actor_layout.py` 的检查必须全部被覆盖，两个脚本删掉或改成薄封装，文档里的命令同步改。Steam 版和离线版全部通过。
- 支持 `--sigs <文件>`（同格式，布局清单可选），插件作者可以校验自己的特征码文件。

### 4. 修复

- `repair --exe <新 exe>`：对校验失败（没命中、多处命中、同一性不过）的项，在新 exe 里定位对应函数和依据位置。定位手段按可信度排序，建议：放宽特征码（把基线指令里的位移、立即数、rel32 改成通配）、从已确认的函数沿调用图传播、字符串和常量锚点、全函数的指纹搜索。候选要有打分和「第一名明显领先第二名」的要求，达不到就报「需要人工」并列出候选。
- 定位后重新生成特征码：保持原条目的种类和语义（HOOK 从入口开始、偷取字节写成 `??` 并给 `DD_ENTRY`、偷取范围的安全检查照旧；GLOBAL 算出新的 capture；CODE 从入口开始），rel32 和 rip 位移一律通配，扩展到唯一为止，生成后在新 exe 上自检。
- 布局项：在对应函数里重新求值依据；模式还能命中但捕获值变了就给出新值；模式不命中时用基线函数和新函数的指令对齐找到对应指令再读，可信度降一级。
- 输出：逐项报告（状态、定位方法、可信度、候选）、对清单文件的补丁（diff）、旧→新 RVA 对照表（供更新知识库）。`--apply` 只改清单文件，然后自动跑一遍 verify。人工确认后用单独的命令（如 `accept`）把新版本加进 `DD_KNOWN_BUILD`、刷新基线；`--apply` 本身不把版本记为已知。
- 同样支持 `--sigs`。

### 5. 运行时核对和降级

- DDCore 在 `Resolve` 之后对所有可在运行时求值的依据求值（和 Python 用同一份清单、同样的语义）。不一致时只停用依赖这一项的服务或主题，日志写明哪一项、期望值、实际值、停用了什么；必需项（DDCore 整体停用的那些）照 ADR 0006。
- 未知版本的策略写成 ADR。推荐：写游戏内存或调用游戏函数的能力（combat 的写操作、effect 施加、persist 等），在依赖的布局项不能在运行时核对时停用；只读能力警告后照常。和 ADR 0006 冲突的地方在新 ADR 里说清楚取代关系。
- 对照测试：一个测试程序把 exe 按映像映射（不执行），跑 C++ 的特征码解析和依据求值，和 Python 的 verify 对同一批 exe（Steam、离线、变异样本）逐项比较结论，必须一致。
- 加载器侧（`early_hook.h` 的 `CreateAppSystems`）同样纳入清单和校验。

### 6. 自测

一条命令（如 `dd_update.py selftest`）跑完下面全部，退出码表示通过与否：

- **跨版本盲定位**：以 Steam 版基线为准，**不用现有特征码**，只靠修复的定位手段在离线版里找全部项，和现有特征码在离线版上的解析结果逐项比较；反过来以离线版基线找 Steam 版。错误定位必须为 0；「需要人工」的数量和原因写进报告。
- **变异样本**：生成到 `build/` 下（不碰真实安装和测试目录里的 exe，不运行变异的 exe），每类都断言 verify 的结论和 repair 的结果，至少覆盖：
  - 结构偏移整体变化（例如把某字段所有访问指令的位移一起改掉）
  - 特征码被破坏（函数体里栈帧大小、寄存器编码等改动）
  - 多处命中（把函数复制一份到别处）
  - 静默误绑定（真函数改得不再匹配，同时在别处放一份能唯一命中的诱饵）
  - 函数搬迁（整个函数挪到新地址，修好内部和调用方的 rel32，旧位置填 `CC`）
  - 枚举表变化（静态初始化里的名字或顺序变了）
  - hook 入口变得不安全（偷取范围里出现分支或 rip 相对指令）
  - 虚表槽位变化
  变异要像真实的编译器输出，不能只构造实现恰好能处理的形状；写明每个样本模拟的是什么真实变化。
- **往返**：在源码的临时副本里 `repair --apply`，再 verify 通过；用修复后的清单能编译 DDCore（可以只编译、不运行）。
- **对照**：第 5 节的 C++/Python 对照测试并进来。

### 7. 文档

- 新 ADR：流水线（清单、依据、基线、修复的可信度规则）和运行时未知版本策略；更新 `docs/README.md` 的 ADR 表。
- `06-workflow.md`：工具表改成新命令；「游戏更新后」改成操作手册，从拿到新 exe 到 accept、更新知识库 RVA、跑自检的完整步骤，每一步给命令。
- `02-architecture.md` 里讲地址和校验的部分、`knowledge/README.md` 的「维护」、`04-roadmap.md` 的 U1、`08-handoff.md`。
- SDK 侧：插件作者如何用同一工具校验和修复自己的特征码文件，写进 `templates/native/` 的说明或 SDK 文档。

## 不做

- win32 版。
- 运行时自动采用推测出来的新值（运行时只核对和停用）。
- 改写知识库里的 RVA（只输出对照表，手册里写怎么用）。
- Lua API 的变化；SDK 的 ABI 只在需要时改（ADR 0018 允许直接改）。
- 下载旧版 exe。
- 部署进真实安装、更新 Inno Setup 目录里的功能清单和 Mod 作者指南（照 P4 推迟到 P7，在 08 的遗留整理项里记一笔）。
- 和现有 DD 插件的共存、新旧版本搭配（ADR 0017、0018）。

## 小阶段

每个小阶段派一个新的 Opus 代理，报告写到 `docs/u1/<小阶段>-report.md`，下一个代理从前面的报告接手。

| 小阶段 | 内容 | 依赖 |
| --- | --- | --- |
| U1a | 第 1 节（清单格式、盘点、C++ 改为从清单取常量）、第 2 节、第 3 节；清单格式包括依赖表和「可运行时求值」的标记，后面的小阶段不再改格式的语义 | — |
| U1b | 第 5 节（运行时核对、降级、ADR 草稿、C++/Python 对照测试），自检 | U1a |
| U1c | 第 4 节（修复、apply、accept、RVA 对照表） | U1a，可和 U1b 并行 |
| U1d | 第 6 节、第 7 节，最后一次全量回归 | U1b、U1c |

U1b 和 U1c 并行时：U1b 只改 `core/`、`loader/`、`tests/` 和 xmake；U1c 只改 `tools/` 下修复相关的模块。两边都需要改清单格式时停下来写进报告，不要各自改。

## 验收清单

| 编号 | 内容 |
| --- | --- |
| A1 | 清单覆盖全部游戏布局常量（盘点表为证），C++ 不再有散落的定义；每项有依据或写明怎么核实；有依赖表 |
| A2 | `verify` 在 Steam 版和离线版上全部通过；原两个脚本的检查全部被覆盖；JSON 报告和退出码符合第 3 节 |
| A3 | 基线可复现（两次生成字节一致），相似度阈值有数据支撑 |
| A4 | `repair`、`--apply`、`accept`、RVA 对照表按第 4 节工作 |
| A5 | 运行时核对和停用按第 5 节工作；C++/Python 对照一致；ADR 写好 |
| A6 | `selftest` 一条命令全部通过：盲定位错误为 0、变异样本每类都有断言、往返通过 |
| A7 | DDCore、DDLua、加载器构建通过；`dd_core_tests`、`dd_lua_tests` 通过；测试目录自检到主菜单，DDCore 正常启用、运行时核对全部通过、日志没有新的错误；`rig_textdiff.py` 没有多余补丁 |
| A8 | 第 7 节的文档；手册里的命令实际跑过 |

## 开工前要读

- [06-workflow.md](../06-workflow.md)（工具、约束）、[02-architecture.md](../02-architecture.md) 讲地址和 hook 的部分、ADR 0006、0007、0017、0018
- `core/src/game/signatures.inc`、`core/src/game/game.{h,cpp}`、`common/pattern.h`、`loader/early_hook.h`
- `tools/check_sigs.py`、`tools/check_actor_layout.py`、`knowledge/tools/dd_pe.py`、`knowledge/tools/dd_disasm.py`
- 第 1 节列出的含偏移常量的文件
- `knowledge/README.md`，以及 `knowledge/dd1/combat.md`、`events.md`、`hooks.md` 里和清单项相关的条目

## 约束

- 所有构建、测试、Python 工具都在 guest 上跑：先用 ToolSearch 加载 `mcp__duet__worker_run`，命令在 guest 的 `D:\workplace` 下执行，开头 `source DD.ModLoader/tools/env.sh`，构建前 `dd_config`。guest 的 Python 是 3.15，有 `pefile`、`capstone`，跑脚本用 `python -B`。在没有 `xmake.lua` 的子目录里运行 xmake 要带 `-P .`。
- 文件在 host 的工作区里改（`D:\Relocation\duet-workspaces\w5e51d41445a4\workplace`，和 guest 的 `D:\workplace` 双向同步）；`build/` 不同步，看 guest 上的产物用 `mcp__duet__worker_read_file`、`mcp__duet__worker_glob`。
- 不 commit。不碰真实安装（`steamapps\common\DarkestDungeon` 下的任何文件只读）。游戏只用 `tools/rig.py` 在测试目录里跑；启动前确认 guest 上没有 `Darkest.exe` 在运行；遇到 360 拦截就停下写进报告。
- 变异样本和中间产物放 guest 的 `DD.ModLoader/build/u1*/`。
- 新的逆向事实写进 `knowledge/` 并带可信度标记。
- 代码注释和文档用中文，风格照现有文件。

## 报告

写到 `docs/u1/<小阶段>-report.md`：构建结果、每条相关验收的证据（命令和输出摘录，完整输出放 `build/u1*/`）、改动文件、没做到的事和失败的尝试、留给下一个小阶段的事项。
