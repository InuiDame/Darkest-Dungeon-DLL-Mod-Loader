# U1d 报告：自测、文档、全量回归

阶段工作文件，用户验收后删除。简报见 [brief.md](brief.md) 第 6、7 节，验收项 A6、A8 和最后的 A7 全量回归；接手 [u1a-report.md](u1a-report.md)、[u1b-report.md](u1b-report.md)、[u1c-report.md](u1c-report.md)。完整输出在 guest 的 `D:\workplace\DD.ModLoader\build\u1d\`（下文的文件名都相对这个目录）。

## 1. 结论

| 验收 | 状态 | 证据 |
| --- | --- | --- |
| A6 `selftest` 一条命令全部通过：盲定位错误为 0、变异样本每类都有断言、往返通过 | 完成 | `selftest_final.txt`、`selftest/summary.txt`、`summary.json`（第 4 节）：5 步全部通过，退出码 0 |
| A8 第 7 节的文档；手册里的命令实际跑过 | 完成 | 第 7 节的文档清单；手册走查 `handbook/walkthrough.txt`、`walkthrough_part2.txt`、`9_regen.txt`（第 6 节）；SDK 的例子 `sdk/`（第 7 节） |
| A7 构建、单元测试 | 完成 | `build.txt`：六个目标 `xmake build -r` 全部通过，没有警告；dd_core_tests 1206 项失败 0，dd_lua_tests 1291 项失败 0 |
| A7 verify、selftest | 完成 | `verify_final.txt`：Steam 版、离线版退出码 0；`selftest_final.txt` 退出码 0 |
| A7 测试目录自检、`rig_textdiff.py` | 沿用 U1b | U1c、U1d 都没有改 `core/`、`loader/` 的 C++（第 9 节的改动文件），DDCore、加载器的二进制和 U1b 自检时的源码相同；测试目录仍是 `scenario4`（`rig_status.txt`），真实安装未被改动 |

主线定下的三件事：

- **verify 加依据位置核对**：做了（第 2 节）。规则在新模块 `tools/ddupdate/position.py`，verify 的同一性一层和修复的布局项共用一套；对不上判误绑定、退出码 2；`evidence.py` 没动；`cpp_compare.py` 把它和 HOOK 的静态检查、`DD_ENTRY` 一起列为「Python 独有的检查」。Steam 版、离线版 verify 退出码 0，22 个 exe 的对照全部一致。
- **字符串区分模板孪生**：没改。
- **未知版本的默认策略**：照 ADR 0036 的 A，U1b 列出的「需要用户决定」原样写进 `08-handoff.md`。

另外有三件主线没要求、我做了的事，理由写在对应的节里，请主线确认：verify 加了「HOOK 的命中位置必须是函数入口」（第 3.3 节，`hook_entry` 样本没有它会静默通过）；依据位置核对加了「虚表槽里的函数要和基线里的是同一个」（第 2 节，`vslot` 样本没有它 `VtSlotHeal` 会静默通过，修复会修错）；修了 repair 的一个缺陷（预演不过的布局项在下一轮又被当成修好的写进补丁，第 2.3 节）。

## 2. verify 的依据位置核对

### 2.1 规则

`position.py` 的 `PositionCheck.check`，verify 里由 `_positions` 调用（同一性一层，`--only` 不含 `identity` 时不做）：

1. 只看唯一命中的依据（成立的，和唯一命中但值变了的）。
2. 参照是「更新前」的基线：有本 SHA1 的基线时用它，否则按同一性检查里各基线的指纹相似度总和取最像的（`verify.pick_baseline`，repair 原来就是这样选的，挪到 verify 里共用）。基线里这条依据要当时成立，记了命中所在函数和指令下标，并存了那个函数的 token 序列（format 2）；没有参照时是 unknown，只在报告里列一行。
3. FUNC、WINDOW 范围的新函数是起点所在的函数；TEXT 范围的新函数是命中所在的函数，它要和基线里那个函数指纹相似度 ≥ 0.85、和已确认的新旧对应不冲突，否则 disagree。
4. 用修复工具同一套指令对齐（唯一窗口和全局匹配块一致才算）把基线的指令下标映射到新函数，命中必须落在映射到的那条指令里。对齐用的 token 并上已确认的调用目标和全局：verify 里用同一性检查通过的特征码播种（`seed_from_ok`），再加成立的依据所在的函数（`seed_from_evidence`）。这两个函数从 `repair.py` 挪到了 `locate.py`，repair 照旧调用。
5. 起点是虚表槽（VSLOT、VSLOT_VALUE）的依据，先核对槽里的函数和基线里这条依据所在的函数指纹相似度 ≥ 0.85（第 2.2 节）。槽里是没有异常目录表项的叶函数（getter）时基线没存指纹，ENTRY 范围的照旧不核对。
6. ENTRY 范围、起点依赖这一项自己的值（VSLOT_VALUE、HIT_VALUE）的依据，位置由定义决定，除第 5 条外不核对。

报告里在同一性之后多一节「依据位置」：对上、对不上、没法对照的个数，对不上的逐条 `BIND` 行（写明依据本身成立还是不成立），没法对照的逐条列原因。JSON 多了 `positions`（`{"项[序号]": {item, index, describe, status, detail, hit, evidence_status}}`）和 `position_baseline`；`failures` 里对不上的那一项类别是 `misbind`，影响按这一项的依赖表汇总。

Steam 版、离线版各自的基线：对上 97、对不上 0、没法对照 0（`verify_final.txt`）。只给对方的基线（`verify_pos_n_vs_s.txt`、`verify_pos_s_vs_n.txt`，加第 5 条之前跑的，当时 92 条）同样全部对上，跨版本没有误报。verify 的耗时没有明显变化（两个 exe 约 37 秒）。

### 2.2 虚表槽里的函数（第 5 条）

`vslot` 样本（第 4.1 节）在两张虚表的第 38 槽插入一个函数，后面整体后移。槽 51 换成了原来槽 50 的伤害函数，它和治疗函数一样调用取最大生命的函数，`VtSlotHeal` 的依据（「槽里的函数有 `lea rcx,[rbx+0x768]; mov r8b,1; mov edx,[rbx+0x790]; call`」）照样成立。只做第 1–4 条时 verify 对它没有任何反应（起点依赖值的依据不核对位置）；按代码推断（没有去掉第 5 条单独重跑），repair 按依据搜索槽号时 51、52 都满足依据，会报「多个值满足依据」。加了第 5 条之后（实测）：verify 判 `VtSlotHeal` 误绑定（槽 51 的函数和基线的相似度 0.166）；repair 搜索槽号时也要求槽里的函数对得上，搜到 52（正确）；按固定槽号取函数的依据（`ActorHp`、`ActorMaxHpStat` 等）在换了的函数里不再重新找，报需要人工。最后这一条防的是修错：不加时修复工具可能在换进来的函数里按指令对齐「找到」一条形状相同的指令，读出别的字段当新值（同样是按代码推断的风险，指令对齐一致性检查不一定挡得住）。

### 2.3 repair 跟着改的地方

- `layoutfix.LayoutRepair.position` 改成调用 `PositionCheck.check`，和 verify 同一套（命中「落在映射到的那条指令里」，原来要求等于指令起点；TEXT 依据多了相似度和对应关系的核对；虚表槽多了第 5 条）。`realign` 在槽里的函数对不上时不重新找，`search_value` 搜索槽号时过滤掉槽里函数对不上的值。
- `seed_from_evidence` 只在命中所在函数和基线指纹相似度 ≥ 0.85 时播种（命中落在别的函数里时不再把那个函数当作已确认）。
- `reconcile`：预演 verify 的依据位置核对不过的项改成需要人工（修过的、没修的都算），特征码的同一性依据对不上时拿掉那条特征码的补丁。
- **修了一个缺陷**：预演循环每轮重新跑 `LayoutRepair`，`reconcile` 改成需要人工的布局项下一轮又被当作修好，循环跑满 4 轮后补丁是在最后一次 `reconcile` 之前建的，`--apply` 会把报告里写着「需要人工」的项写进清单。现在把被否决的项带到下一轮，最后一轮还有变化时按最终结论重建补丁、再预演一次。
- `--apply`、`accept` 写回清单时保持原文件的换行（`patch.write_keep_newlines`）：原来用 `write_text`，在 Windows 上会把 LF 的 `signatures.inc` 整个改成 CRLF。

### 2.4 cpp_compare

单列「Python 独有的检查（运行时不做，不算不一致）」，下面分两类：特征码静态检查（原来就有，含新加的入口检查）和依据位置核对；依据求值本身照常逐项比较。新加 `--tree`（verify 用源码副本里的清单，手册在副本里走查时用）；默认样本目录加了 `build/u1d/mut/`。

## 3. 自测：`dd_update.py selftest`

### 3.1 命令和组成

`python -B DD.ModLoader/tools/dd_update.py selftest [--only blind,mutants,mcheck,roundtrip,compare] [--samples 名字,...] [--roundtrip-sample 名字] [--out 目录]`，实现在 `tools/ddupdate/selftest.py`，输出 `build/u1d/selftest/summary.txt`、`summary.json` 和每一步的详细输出。五步：

| 步 | 做什么 | 通过条件 |
| --- | --- | --- |
| blind | `repair.blind_both()`：Steam 基线 → 离线版、离线版基线 → Steam 版，不用现有特征码 | 特征码级和函数级错误都为 0 |
| mutants | `mutants.generate()` 生成 10 个样本到 `build/u1d/mut/`，逐个 verify、repair | 失效项（名字, 类别）集合和退出码等于样本表；repair 逐项状态和退出码等于样本表（没写的项必须是 ok 或 unverified）；不变式 1：修复项的新位置、新值等于真值，verify 判通过、repair 判 ok 的项现有的位置和值也等于真值（漏报也算错）；不变式 2：预演 verify 剩下的失效项都报了需要人工；9 个类别都有样本 |
| mcheck | `mcheck.run()`（U1c 的 `build/u1c/mcheck.py` 挪进来，见 3.4） | 改坏的清单上 verify 把 `EffectDotBurn` 判成误绑定；修错 0；`MapNodeLeft` 报需要人工 |
| roundtrip | 源码复制到 `build/u1d/selftest/roundtrip/tree`，`struct_shift` 样本 `repair --apply --tree`；副本里 `xmake build -P . DDCore`；副本里 `accept --label u1d_struct_shift` | apply 后 verify 退出码 0、补丁非空；DDCore 编出来；`DD_KNOWN_BUILD` 加上、基线生成、再 verify 是已知版本且退出码 0；仓库里的清单和基线的 sha256 不变 |
| compare | `cpp_compare.main()`，Steam、离线、U1a 的 2 个、U1b 的 8 个、U1d 的 10 个，共 22 个 exe | 退出码 0；没有 `dd_layout_compare.exe` 时这一步失败（提示先 `xmake build dd_layout_compare`），不当作通过 |

一步出错（异常）记为不通过、接着跑下一步；全部通过时退出码 0，否则 1。全部跑完约 25 分钟（repair 每个样本约 70 秒，对照每个 exe 约 18 秒）。

### 3.2 变异样本的做法（`tools/ddupdate/pepatch.py`）

U1b 的 `make_mutants.py` 只改几个字节测失败路径。这次的样本要像编译器的输出，写了一个改 exe 副本的小工具：

- **搬迁**（`relocate`）：函数解码后重新排布到新地址，可以在任意指令前插入指令（可以带指向新标签的短跳转）、替换、去掉、在末尾追加指令；函数内的分支、rip 相对位移、call/jmp rel32 都按新地址重算，短跳转放不下时改成近跳转，重复到布局稳定。所有调用方的 call/jmp/jcc rel32（扫描 .text、解码所在函数确认在指令边界上）、rip 相对引用（函数索引的函数指针反查表）、`.rdata`/`.data` 里的绝对指针都改到新地址；异常目录的表项改成新范围并重新按 BeginAddress 排序；序言变了时在 `.rdata` 末尾写一份新的 UNWIND_INFO（序言长度、每个代码槽的 CodeOffset 同步）；旧位置填 `CC`。带 C++ 异常表（`UNW_FLAG_EHANDLER/UHANDLER`）或链式 unwind 的函数不搬（表里的 IP 到状态映射是 RVA），有跳表的函数不搬。
- **复制**（`copy_function`）：同样重新排布到空隙，加一条异常目录表项（和原函数共用 UNWIND_INFO），不改调用方。
- **原地改**：`set_field` 改一条指令的位移或立即数，宽度放不下时报错，不换编码；`shift_fields` 按「基址寄存器 + 位移范围」把结构字段的访问整体后移；`edit_unwind` 改栈分配的 unwind 代码（和别的函数共用时写一份新的）。函数末尾有 switch 跳表时只解码表之前的代码。
- 新位置只能是 `.text` 末尾的空隙：Steam 版 `.text` 的 VirtualSize 到下一节之间只有 0x11F 字节（对齐后 0x110），`.text` 后面紧接 `.rdata`，不能再大。所以搬迁、复制的函数都在 0x110 字节以内。`.rdata` 原始数据末尾有 0x16C 字节放新的 UNWIND_INFO。
- 地址都按清单在 Steam 版上现场解析（特征码、依据的命中），不写死 RVA；改哪几条指令（基址寄存器、位移范围、指令文本）是读了这些函数的反汇编定的，`generate` 先核对 Steam 版的 SHA1。

### 3.3 HOOK 入口检查（主线没要求，我加的）

`hook_entry` 样本模拟「源码加了空指针判断，编译器把它放在序言之前」：入口变成 `test rcx,rcx; je`，原来的序言整体后移。`ConsumeHealthDamageBlock` 的特征码把入口 5 字节写成了通配，于是命中了挪下去的旧序言（函数 +9），偷取范围是 `mov [rsp+0x10],rbx`，指令边界、不安全指令、跳进偷取范围的分支、`DD_ENTRY` 全部通过：verify 原来的检查会判它通过，hook 却装在函数中间（空指针调用绕过 detour）。所以在 `sigs.check_hook` 加了一条：命中位置在异常目录记录的函数里时必须是函数入口（不在任何表项里的叶函数只记一笔）。现有 18 条 HOOK 都在函数入口，Steam 版、离线版不受影响。这是 Python 独有的静态检查，运行时不做：DDCore 在这种更新上照样会把 hook 装在函数中间，见第 8 节。

### 3.4 清单变异冒烟

`tools/ddupdate/mcheck.py`（也可以单独运行），内容和 U1c 的 `build/u1c/mcheck.py` 相同（16 处改动），多了两条断言：改坏的清单上 verify 要把 `EffectDotBurn` 判成误绑定（U1c 发现的那种「依据靠邻居偏移区分、在别处唯一命中」，U1d 之前 verify 发现不了），`MapNodeLeft` 必须报需要人工。结果：verify 退出码 2、误绑定 `['EffectDotBurn']`；repair 9 条特征码、7 项布局修好，`MapNodeLeft` 需要人工，修错 0。

## 4. 变异样本

### 4.1 样本清单

真值：`truth` 里列出的特征码位置和清单项的值，没列出的就是不变。「verify」列是样本表里写的、selftest 断言的失效项和退出码；「repair」列同样。所有样本的不变式（修错 0、漏报 0、预演剩下的失效项都报了需要人工）都成立。

| 样本 | 类别 | 模拟的真实变化 | verify（断言 = 结果） | repair（断言 = 结果） |
| --- | --- | --- | --- | --- |
| `relocate` | 函数搬迁 | `ListenerAdd`（必需）整个挪到新地址（`3619D0` → `C71EF0`），`call 372340` 后面多一条 `nop`（MSVC 在带异常状态的 call 后放 nop）；改写 135 处 rel32 | 退出码 0，没有失效（特征码跟着走，同一性 0.923、调用者 84；依据位置全部对上） | 退出码 0，全部 ok；RVA 对照表里 `3619D0 → C71EF0` |
| `relocate_sched` | 函数搬迁 | `QueryListenerAdd` 挪到新地址，同时编译器换了调度（`mov rbx,rcx` 排到 `mov [rsp+28h],rdx` 后面），原特征码不再命中 | 退出码 2：`QueryListenerAdd`（可选）、`QueryListenerEntrySize`（可选），`ListenerEntryListener`、`ListenerEntryActive`（必需，它们各有一条依据锚在这条可选特征码上） | 退出码 0：`QueryListenerAdd` repaired-中（只靠指纹，相似度 0.857，新地址 `C71EF0` 正确），四项依据随之成立 |
| `frame` | 特征码被破坏 | `CampaignCurrent`（必需）所在函数的栈帧 0x38 → 0x48，`sub/add rsp` 和 `UWOP_ALLOC_SMALL` 同步改 | 退出码 2：`CampaignCurrent`（必需） | 退出码 0：repaired-高（放宽特征码直接命中，调用图约束 5/5，引用旧全局的已确认函数 3/3） |
| `twin` | 多处命中 | `EffectLookup` 多出一份完全相同的副本（模板的另一个实例，或者 ICF 没合并），有异常目录表项、没有调用者 | 退出码 1：`EffectLookup` 2 次命中、`SkillEffects`（离线检查用它） | 退出码 1：`EffectLookup` manual（原函数靠调用者选对了，但 96 字节内生成不了唯一的模式）、`SkillEffects` manual |
| `decoy` | 静默误绑定 | `BuffRemoveByGuid` 栈帧 0x30 → 0x40，原特征码不再命中真函数；改之前的样子留了一份（有异常目录表项、没有调用者），原特征码唯一命中它 | 退出码 2：`BuffRemoveByGuid` 误绑定（相似度 1.000、关系 1.00，靠「基线有 1 个调用者，现在没有」） | 退出码 0：repaired-中，新地址是真函数 `5C4660`（诱饵相似度同为 1.000，因为没有调用者落选） |
| `enum` | 枚举表变化 | stat 枚举第 2、3 项（`combat_stat_add`、`combat_stat_multiply`）在源码里调换：静态初始化里两条 `lea r8,[名字]` 的目标对调 | 退出码 1：`EnumStat`（离线检查 `enum_stat`，不同的下标 [2, 3]） | 退出码 1：`EnumStat` manual（只有离线依据，不给新值） |
| `hook_entry` | hook 入口不安全 | `ConsumeHealthDamageBlock` 加了空指针判断、编译器放在序言之前（`test rcx,rcx; je` 近跳转到末尾的 `xor al,al; ret`），函数变长挪到新地址，UNWIND_INFO 的序言长度和偏移加 9 | 退出码 1：`ConsumeHealthDamageBlock`（命中位置不是函数入口，+0x9）、`VtSlotDamage`（依据的 rel 是这条特征码） | 退出码 1：`ConsumeHealthDamageBlock` manual（定位对了，相似度 0.946，但偷取范围里有 `je`，hook 入口不安全）、`VtSlotDamage` manual |
| `vslot` | 虚表槽位变化 | Actor 基类在第 38 个虚函数处加了一个虚函数：Hero、Monster 两张表第 38 槽插入（空实现，链接器合并成已有的 `return 0` 叶函数 `237070`），后面整体后移 | 退出码 2：`VtSlotHeal` 误绑定（第 2.2 节）；`ActorHp`、`ActorMaxHpStat`、`ActorMaxHpMask`、`DamageEpsilonBits`、`VtSlotDamage`、`VtSlotDeathsDoor`、`VtSlotDead`（可选） | 退出码 1：`VtSlotDamage` 50→51、`VtSlotHeal` 51→52、`VtSlotDeathsDoor` 40→41、`VtSlotDead` 43→44 都是 repaired-中（按依据搜索）；`ActorHp`、`ActorMaxHpStat`、`ActorMaxHpMask`、`DamageEpsilonBits` manual（按固定槽号取函数，槽里换了函数） |
| `evidence_misbind` | 依据误绑定 | Effect 在 dot_poison（0x134）之前插入 8 字节：解析函数 `4F02D0` 里以 rsi 为基址、位移 ≥ 0x134 的 111 条访问后移 8，`EffectApplyInternal` 读 `.swap` 的那条也后移。`EffectDotBurn` 的依据靠「下一个字段是 0x140」区分，后移后唯一命中 dot_poison 的解析代码，读到的值恰好还是 0x13C | 退出码 2：`EffectDotBurn` 误绑定（命中 `4F096C`，基线那条指令对齐到 `4F0A22`）；19 项可选（值变了的 `EffectOnHit` 等、模式不再命中的 `EffectDotStress`、两个离线检查各自整组失效） | 退出码 1：`EffectDotBurn` 0x13C→0x144、`EffectDotStress` 0x1B8→0x1C0（repaired-中），`EffectOnHit/OnMiss/ApplyOnce/Queue/BuffIds`（repaired-高）；13 项只有离线依据或离线检查修不了的 manual（含值没变的 `EffectChance` 等，它们的离线检查和变了的字段绑在一起） |
| `struct_shift` | 结构偏移整体变化 | `Buff::Instance` 在 rule_float（0xB0）之前插入 8 字节：复制构造、构造、`BuffRuleActive` 里以 Instance 为基址、位移 ≥ 0xB0 的访问后移 8；Instance 0x410→0x418、Entry 0x418→0x420，`BuffGroupErase` 逐个前移 Entry 的拷贝和比例寻址、`BuffDurationUpkeep` 的步长、`BuffLibraryIndex` 的节点分配和 memset、`BuffGroupAdd` 的栈帧和栈上副本、8 个按 Entry 做除法的函数的魔数和移位（`0x3E88CB3C9484E2B`/sar 3 → 33 的逆元/sar 5）同步改 | 退出码 1：12 项可选（值变了的 7 项；依据模式里写着 0x418 的 `InstDuration`、`BuffEntryInstance`；`DD_ENTRY` 里有栈帧大小的 `BuffGroupAdd`；特征码里有除法魔数的 `BuffBurnDecay`、`BuffRemoveByGuid`） | 退出码 0：12 项全部修对（7 个值 +8；两条依据模式重新生成、值不变；`BuffGroupAdd` 的 `DD_ENTRY` 换成 `sub rsp,0x448`；两条魔数特征码重新生成） |

### 4.2 做不到真实的地方

- 搬迁和复制只能用 `.text` 末尾 0x110 字节的空隙，所以只挪得动小函数，而且一次只挪一个；真实的重新链接会让所有函数一起挪。
- 结构字段后移只改了清单依据涉及的函数和按 Entry 做除法的函数（`struct_shift`）、解析函数和一条 `.swap` 读取（`evidence_misbind`）。游戏里其他访问这些结构的代码没有类型信息、认不全，没改；它们不在任何依据里，不影响 verify 和 repair 的结论。
- `struct_shift` 的复制构造没有加复制新字段的指令：加指令要挪函数，这个函数有 C++ 异常表。
- `vslot` 去掉了每张表的最后一槽，表的大小不变（否则要挪动虚表）；按槽号调用虚函数的游戏代码（`call [rax+槽*8]`）没有跟着改，清单里没有依据看那些调用点。
- `hook_entry` 的提前返回路径放在同一个异常目录表项里；MSVC 实际可能把它拆成单独的表项。
- `decoy` 的诱饵是真函数改之前的拷贝；现实里更可能是另一个形状相近的函数（例如模板的另一个实例）。真函数和诱饵指纹完全相同，verify 能发现只靠调用者，repair 能选对也只靠调用者。
- `enum` 只调换了一对，没有新增或删除项。
- 重定位表（`.reloc`）没有改：搬迁的函数里没有绝对地址；`vslot` 改的虚表槽位置不变，原有的重定位项照样适用。改出来的 exe 不运行，没有工具读重定位表。

### 4.3 阈值的数据

U1a 说关系一致度阈值（0.5）只有一个跨版本数据点、要用变异样本再验证：这次的样本都没有碰到这条（诱饵是拷贝，调用关系一致），数据仍然只有那一个点。指纹相似度的新数据点：插入一条 `nop` 的 `ListenerAdd`（135 字节）0.923；序言前插了 3 条指令的 `ConsumeHealthDamageBlock` 0.946；两条相邻指令换序的 `QueryListenerAdd`（109 字节、30 条指令）0.857，刚过 0.85。小函数对调度变化很敏感，再多换一两条就会落到阈值以下、报需要人工（安全的一侧）。

## 5. selftest 的完整汇总

最终一次（全量重建之后，`selftest_final.txt`，`selftest/summary.txt`）：

```
dd_update.py selftest：5 步，1472 秒
== 跨版本盲定位：通过（87.5 秒）
   steam 基线 → _windowsnosteam：特征码 正确 61、需要人工 0、错误 0；函数级 正确 60、错误 0、需要人工 1
   nosteam 基线 → _windows：特征码 正确 61、需要人工 0、错误 0；函数级 正确 60、错误 0、需要人工 2
== 变异样本：通过（724.0 秒）
   （每个样本的生成说明见第 4.1 节，这里省略；非 ok 的项合并写）
   relocate           [函数搬迁] verify 退出码 0（失效 0 项）；repair 退出码 0，非 ok 的项 {}；通过
   relocate_sched     [函数搬迁] verify 退出码 2（失效 4 项）；repair 退出码 0，非 ok 的项 {'QueryListenerAdd': 'repaired-中'}；通过
   frame              [特征码被破坏] verify 退出码 2（失效 1 项）；repair 退出码 0，非 ok 的项 {'CampaignCurrent': 'repaired-高'}；通过
   twin               [多处命中] verify 退出码 1（失效 2 项）；repair 退出码 1，非 ok 的项 {'EffectLookup': 'manual', 'SkillEffects': 'manual'}；通过
   decoy              [静默误绑定] verify 退出码 2（失效 1 项）；repair 退出码 0，非 ok 的项 {'BuffRemoveByGuid': 'repaired-中'}；通过
   enum               [枚举表变化] verify 退出码 1（失效 1 项）；repair 退出码 1，非 ok 的项 {'EnumStat': 'manual'}；通过
   hook_entry         [hook 入口不安全] verify 退出码 1（失效 2 项）；repair 退出码 1，非 ok 的项 {'ConsumeHealthDamageBlock': 'manual', 'VtSlotDamage': 'manual'}；通过
   vslot              [虚表槽位变化] verify 退出码 2（失效 8 项）；repair 退出码 1，ActorHp、ActorMaxHpMask、ActorMaxHpStat、DamageEpsilonBits manual，VtSlotDamage、VtSlotDead、VtSlotDeathsDoor、VtSlotHeal repaired-中；通过
   evidence_misbind   [依据误绑定] verify 退出码 2（失效 20 项）；repair 退出码 1，EffectDotBurn、EffectDotStress repaired-中，EffectOnHit、OnMiss、ApplyOnce、Queue、BuffIds repaired-高，其余 13 项 manual；通过
   struct_shift       [结构偏移整体变化] verify 退出码 1（失效 12 项）；repair 退出码 0，12 项 repaired（BuffEntryInstance、InstDuration、InstRuleString 中，其余高）；通过
== 清单变异冒烟：通过（114.1 秒）
     改坏 16 处；改坏的清单上 verify 退出码 2，误绑定 ['EffectDotBurn']
     repair 退出码 1，apply 后 verify 退出码 1；修错 0；需要人工 ['MapNodeLeft']
     （9 条特征码 repaired，在离线版上的解析结果和原清单一致；HeroClass、BattleMonsters、InstDuration、EffectDotBurn、
      VtSlotDamage、MonsterBlockImmuneLeaDisp repaired，值和原清单一致；MapNodeLeft manual）
== 往返（repair --apply、编译、accept）：通过（137.7 秒）
   源码复制到 build/u1d/selftest/roundtrip/tree（common, core, examples, loader, lua, sdk, spikes, templates, tests, tools, xmake, xmake.lua, .gitattributes）
   repair --apply --tree：退出码 0，补丁 signatures.inc 5 行、layout.inc 11 行；apply 后 verify 退出码 0
   副本里 xmake build DDCore：退出码 0，14 秒，DDCore.dll 已生成
   accept --label u1d_struct_shift：退出码 0；DD_KNOWN_BUILD 已加，基线 u1d_struct_shift.json 已生成；再 verify：u1d_struct_shift，退出码 0
   仓库里的清单和基线没有变化
== C++/Python 对照：通过（408.3 秒）
   比较 22 个 exe：steam, nosteam, u1a/Darkest_layout, u1a/Darkest_layout_sig, u1b/C_vtable_sig, u1b/D_epsilon, u1b/E_text_dup, u1b/F_vslot, u1b/G_anchor, u1b/H_rel32, u1b/I_pdata, u1b/J_entry_hooked, u1d/decoy, u1d/enum, u1d/evidence_misbind, u1d/frame, u1d/hook_entry, u1d/relocate, u1d/relocate_sched, u1d/struct_shift, u1d/twin, u1d/vslot
   汇总：22 个全部一致
   结果：全部一致，退出码 0
结果：全部通过，退出码 0
```

原样的全文在 `selftest/summary.txt`（每个样本的生成说明、清单变异冒烟的逐项结果都在里面），逐项数据在 `summary.json`，每个样本的 repair 报告在 `selftest/repair/<样本>/`，对照测试逐个 exe 的输出在 `selftest/compare/compare.txt`。对照测试里「Python 独有的检查」出现在 4 个 U1d 样本上：`hook_entry`（命中位置不是函数入口）、`struct_shift`（`BuffGroupAdd` 的 `DD_ENTRY`）、`evidence_misbind`（`EffectDotBurn[0]` 依据位置）、`vslot`（`VtSlotHeal[0]` 槽里的函数），以及 U1b 的 `J_entry_hooked`（原来就有）。这次最终运行在第 8 节写的 `hook_entry` UNWIND_INFO 修正之后、全量重建之后；之前另有两次完整运行（`selftest_run1.txt` 和修正前的那次），结论相同。

## 6. 手册走查

用 `struct_shift.exe` 当「新 exe」，在源码副本 `handbook/tree` 里照 `06-workflow.md` 的「游戏更新后」从头走一遍（脚本 `handbook.sh`，输出 `handbook/walkthrough.txt`；第 5 步的对照和第 8 步补跑在 `walkthrough_part2.txt`）：

| 步 | 命令（都加了 `--tree handbook/tree`） | 结果 |
| --- | --- | --- |
| 1 | 新 exe `struct_shift.exe`（SHA1 `8ef49092…`），旧 exe 用 Steam 版 | |
| 2 | `verify <新 exe> --json handbook/verify.json` | 退出码 1，12 项可选失效（同第 4.1 节） |
| 3 | `repair --exe <新 exe> --old-exe <Steam 版> --out handbook/repair` | 退出码 0；`FIX高` 9、`FIX中` 3（`BuffEntryInstance`、`InstDuration` 重新生成依据模式，`InstRuleString` 0xb4→0xbc）；RVA 对照表 4638 行（带旧 exe，调用图扩展 4546 个函数）；`signatures.diff`、`layout.diff` 见 `walkthrough.txt` |
| 4 | `repair --exe <新 exe> --apply`，再同步 `tests/unit/layout_golden.cpp` 里 7 个值改了的断言（8 处，含 `kInstRuleHash + 4`） | apply 后 verify 退出码 0 |
| 5 | 副本里 `xmake build -P .` 六个目标；`xmake run -P . dd_core_tests`、`dd_lua_tests`；`cpp_compare.py <新 exe> --tree <副本> --bin <副本的 dd_layout_compare>` | 六个目标退出码 0；1206 项失败 0、1291 项失败 0；对照一致，退出码 0 |
| 6 | 测试目录自检 | 没做：变异样本不运行。这一步的命令（`rig.py deploy/run/save-log`、`rig_textdiff.py`）U1b 用真实的 exe 跑过，U1d 没改 C++，沿用（第 1 节） |
| 7 | `accept --exe <新 exe> --label steam --replace`，副本里重新构建 DDCore，再 `verify` | 去掉旧的 `DD_KNOWN_BUILD(9db2d73b…, steam)`，加上 `8ef49092…`；基线 `steam.json` 重新生成（374737 字节）；DDCore 构建退出码 0；再 verify「已知版本 steam」、退出码 0 |
| 8 | 用 `rva_map.tsv` 更新知识库（在 `knowledge/dd1` 的副本上） | `struct_shift` 没挪函数，对照表里旧≠新的行是 0；另用 `relocate.exe` 跑 `repair --old-exe`，对照表里 `3619D0 → C71EF0`，按表把副本 `events.md` 里的 3 处 `3619D0` 换成 `C71EF0`（`walkthrough_part2.txt`）；真实的知识库没动 |
| 9 | `bash knowledge/tools/regen.sh --ghidra` | 退出码 0，6 分钟，Ghidra 工程重建成功（`analysis.done`）。生成表只有 `data_vocabulary.md` 的「块类型」多了一个 `immune`：同一个 Steam 版 exe，变的是脚本读的数据目录（和 U1 无关，原因没查），我把这个文件还原成了原样，见第 8 节 |

准备和走查中发现并修掉的问题：`verify` 没有 `--tree`（`repair`、`accept` 原来就有），准备走查时加了；`cpp_compare.py` 用副本里的工具时找不到工作区根目录的 `knowledge/`（`hooks.md`），退出码 3，加了 `--tree`、手册里写成用仓库的工具；`layout_golden.cpp` 里有 `kInstRuleHash + 4 == 0xF8` 这种推出来的断言，同步时要一起改（手册第 4 步说的是「对应的断言」，走查脚本按这一项的名字找全了）；xmake 要在 `DD.ModLoader` 下跑（A7 时踩到，第 8 节），手册第 5 步写明了。

## 7. 文档

| 文件 | 改了什么 |
| --- | --- |
| `docs/adr/0035-update-pipeline.md`（新） | 清单、依据、基线、verify（特征码、同一性、依据位置、布局）、修复的定位手段和门槛、可信度、一定报需要人工的情况、自测；和 0006（补充）、0036（运行时部分）的关系；考虑过的方案和已知局限 |
| `docs/README.md` | ADR 表加 0035（和 0036 的措辞核对过，标题一致）；「当前状态」写 U1 做完、等待审核和验收 |
| `docs/adr/0006-signature-addressing.md` | 「后果」改成新命令和手册入口，注明被 0035、0036 补充（0036 取代第三条）；开头的 `check_sigs.py` 改成 `dd_update.py` |
| `docs/adr/0036-runtime-layout-check.md` | 「两边一致」一条补上 U1d 的两条 Python 独有的检查 |
| `docs/06-workflow.md` | 工具表：verify（新检查、`--tree`、旧脚本标成封装）、repair、accept、baseline、selftest、C++/Python 对照；新的「游戏更新后」一节，9 步，每步有命令和判断标准，写了没有静态依据的项各自怎么核实（`test_events`、`probe_p3`、`probe_p4`/`test_combat`、对照反汇编）、在副本里试的办法、selftest 和旧版本的关系 |
| `docs/02-architecture.md` | 「特征码」一节改成「地址、布局清单和校验」：清单、写法约定、运行时核对、停用、离线校验和修复；工程结构、combat 一节和风险表里的旧脚本名 |
| `docs/04-roadmap.md` | 当前进度和 U1 的状态（做完、等审核和验收）、ADR 链接、校验和自测两条补上新内容；P1 行的 `check_sigs.py` 注明是封装 |
| `docs/08-handoff.md` | 当前状态写 U1（交付、证据、测试目录）；U1b 的 6 条「需要用户决定」原样列出，另加 U1d 的运行时残留风险；仍未验证加 3 行；遗留整理项加 4 条（含「功能清单和 Mod 作者指南的 U1 内容推迟到 P7」） |
| `docs/adr/0034-party-monster-enumeration.md` | 旧脚本名 |
| `templates/native/README.md`（新）、`sigs.example.inc`（新） | 插件作者用 `--sigs` 校验、生成基线、更新后校验和修复的步骤和例子；README 末尾记了一次实际运行 |
| `knowledge/README.md` | 「维护」一节加游戏更新的流程入口（手册、`rva_map.tsv` 的用法、`regen.sh --ghidra`）和旧脚本名的替换说明 |
| `knowledge/dd1/combat.md`、`hooks.md`、`campaign.md` | 旧脚本名改成 `dd_update.py verify`（`hooks.md` 里 SpineCompat 自己的 `check_sigs.py` 写明是它自己的） |

SDK 的例子实际跑过（`sdk/`）：`verify --sigs templates/native/sigs.example.inc` 两个版本退出码 0；`baseline --sigs … --out sdk/baseline` 生成 `steam.json`、`nosteam.json`；拿 `frame.exe` 当新 exe，`verify --sigs … --baseline sdk/baseline/steam.json` 报 `MyCampaign` 0 次命中、退出码 1；`repair --sigs … --apply` 把 `MyCampaign` 修成 `48 83 EC 48 …`（FIX高），apply 后再 verify 退出码 0。

## 8. 没做到的事、失败的尝试、需要主线决定的事

- **运行时的残留风险**（需要主线决定，已写进 08）：运行时只做依据求值，不做 HOOK 的静态检查和依据位置核对。对照测试（`selftest/compare/compare.txt`）显示 C++ 在三个样本上照样认为成立：`hook_entry` 的特征码命中函数 +9，DDCore 会把 hook 装在函数中间；`evidence_misbind` 的 `EffectDotBurn` 依据成立，`effect_get` 会读错字段（只读）；`vslot` 的 `VtSlotHeal` 成立，`heal.*` 的 hook 会挂到伤害函数上。另外 `decoy` 这类静默误绑定运行时本来就发现不了（运行时不做指纹的同一性检查，U1b 的设计），DDCore 会把 `BuffRemoveByGuid` 解析到诱饵上（只影响 `buff.remove` 的原因判断）。也就是说，玩家的游戏先于我们的工具更新、而更新恰好是这几种形状时，运行时兜底兜不住。可以做的：DDCore 运行时检查 HOOK 的命中位置是异常目录里的函数起点（C++ 已经自己解析异常目录，代价很小；要改 `layout_check.cpp`、对照测试，Python 的这条检查要从「静态检查」挪到和 C++ 同义的解析里）；给 `VtSlotHeal` 补一条只有治疗函数才满足的依据（要重新生成基线）。依据位置核对放进运行时代价太大（要带基线的指令序列），不建议。我都没做：前一条改 C++，后一条改清单内容，都超出了「只改 Python 侧」。
- 未知版本的默认值、`ListenerFunction` 的锚点等 6 条照 U1b 原样列在 08，没有动。
- 变异样本是照 Steam 版 `9db2d73b` 写的，游戏更新、`steam.json` 被 `accept --replace` 换掉以后，selftest 的变异样本、清单变异冒烟、往返要留旧 exe 和旧基线才能接着跑（`DD_SELFTEST_STEAM` 指向旧 exe；基线要手工指过去），手册第 9 步和 `mutants.py` 开头写了。
- `regen.sh --ghidra` 走查时 `data_vocabulary.md` 多了一个块类型 `immune`，exe 没变，变的是 `dd_data_survey.py` 读的数据；和 U1 无关，我还原了这个文件，原因没查。Ghidra 工程是用同一个 exe 重建的，留着新的那份。
- 关系一致度阈值没有拿到新数据（第 4.3 节）。
- `DD.Plugins` 的文档里还写着 `DD.ModLoader/tools/check_sigs.py`（薄封装仍可用），不在本项目范围，没改，记在 08。
- 失败的尝试：
  - 第一版依据位置核对对 `VSLOT` 的 ENTRY 依据也要求基线有槽函数的指纹，取字段的叶函数没有异常目录表项、基线没存，每个 exe 多出 11 条「没法对照」；改成这种情况照旧不核对。
  - 第一版 `hook_entry` 用 `xor eax,eax` 当提前返回值，实际函数返回 `bool`（结尾 `movzx eax, sil`），改成 `xor al,al`。`je` 到末尾超出短跳转范围，被自动改成近跳转（入口 9 字节），所以命中位置是 +9 不是 +5。
  - `BuffRuleActive` 末尾有 switch 跳表，第一版的函数解码要求整段解码完、直接报错；改成按 `[基址+变址*4+表]` 找到表的起点、只解码表之前的代码。
  - 驱动脚本没有 `if __name__ == "__main__"` 时，函数索引的多进程会在子进程里重跑整个脚本（重新生成样本、读到半截的 exe）；selftest 从 `dd_update.py` 进入，不受影响，调试用的脚本加了保护。
  - 走查第 5 步第一次用副本里的 `cpp_compare.py`，找不到 `knowledge/dd1/hooks.md`，退出码 3；改成仓库的工具加 `--tree` 后通过。
  - 自查时发现 `hook_entry` 的 UNWIND_INFO 是按写死的 5 字节平移的，实际入口是 9 字节（`je` 被排成近跳转）；改成按排布结果算（`relocate` 把平移量传给 `unwind_edit`），单独重跑这个样本和它的对照（`selftest_hook_entry.txt`、`compare_hook_entry.txt`），结论不变（verify、repair 不读 unwind 代码），然后重跑了整个 selftest。
  - 在 `D:\workplace` 而不是 `DD.ModLoader` 下跑了一次 `dd_config` 和 `xmake build -r`，xmake 找不到 `xmake.lua`、在工作区根目录建了一个空的 `.xmake/` 并停在确认提示上；杀掉进程、删掉那个目录，在 `DD.ModLoader` 下重跑。手册第 5 步写明了要先 `cd DD.ModLoader`。
  - 在 host 上用 Python 的 `write_text` 改文档和模块时，Windows 的换行转换把几个 LF 文件改成了 CRLF；改完后按字节换回 LF（git 的 `text=auto` 本来也会归一，`git diff --stat` 只有实际的改动）。

## 9. 改动文件

新增：
- `tools/ddupdate/position.py`、`pepatch.py`、`mutants.py`、`mcheck.py`、`selftest.py`
- `docs/adr/0035-update-pipeline.md`、`templates/native/README.md`、`templates/native/sigs.example.inc`、`docs/u1/u1d-report.md`

修改（Python）：
- `tools/ddupdate/verify.py`：依据位置核对（`_positions`、`pick_baseline`、`positions` 报告和 JSON、误绑定计入退出码）、`--tree`
- `tools/ddupdate/sigs.py`：HOOK 命中位置必须是函数入口
- `tools/ddupdate/layoutfix.py`：位置核对改用 `PositionCheck`，槽函数核对（`realign`、`search_value`）
- `tools/ddupdate/locate.py`：`new_function_of`、`seed_from_ok`、`seed_from_evidence` 从 `repair.py` 挪过来，后者加了相似度门槛和 `results` 参数
- `tools/ddupdate/repair.py`：`pick_baseline` 用 verify 的；`reconcile` 处理依据位置核对；预演循环带上被否决的布局项、最后一轮重建补丁；写回清单保持换行
- `tools/ddupdate/patch.py`：`write_keep_newlines`
- `tools/ddupdate/cpp_compare.py`：「Python 独有的检查」、依据位置核对单列、`--tree`、默认样本加 `build/u1d/mut/`
- `tools/ddupdate/__init__.py`：模块表；`tools/dd_update.py`：`selftest` 子命令、verify 的 `--tree`

修改（文档）：第 7 节的表。

没动：`evidence.py`（求值语义）、`manifest.py`、清单格式和内容（`signatures.inc`、`layout.inc`）、`tools/baseline/*.json`、`core/`、`loader/`、`tests/`、各 `xmake.lua`。

## 10. 验收清单总表（A1–A8）

| 编号 | 内容 | 状态 | 证据 |
| --- | --- | --- | --- |
| A1 | 清单覆盖全部游戏布局常量、C++ 不再有散落的定义、每项有依据或写明怎么核实、有依赖表 | 完成 | [u1a-report.md](u1a-report.md) 第 1、4 节（盘点表、227 项、编译期旧值断言）；U1b 补了 6 项运行时依据（[u1b-report.md](u1b-report.md) 第 3 节） |
| A2 | verify 在 Steam 版和离线版上全部通过；原两个脚本的检查全部覆盖；JSON 和退出码 | 完成 | [u1a-report.md](u1a-report.md) 第 5、6 节；U1d 加了依据位置核对和 HOOK 入口检查之后两个版本仍是退出码 0（本报告第 2.1 节、`verify_final.txt`） |
| A3 | 基线可复现，相似度阈值有数据支撑 | 完成 | [u1a-report.md](u1a-report.md) 第 7 节（两次生成字节一致、simstats）；[u1b-report.md](u1b-report.md) 第 3 节、[u1c-report.md](u1c-report.md) 第 7 节（format 2 重新生成、仍字节一致）；U1d 没有改基线，新的相似度数据点在第 4.3 节 |
| A4 | repair、`--apply`、accept、RVA 对照表 | 完成 | [u1c-report.md](u1c-report.md)；U1d 的样本和往返（第 4、5 节）、手册走查（第 6 节）又跑了一遍；修了预演循环的缺陷（第 2.3 节） |
| A5 | 运行时核对和停用、C++/Python 对照一致、ADR | 完成（ADR 0036 的默认值待用户决定） | [u1b-report.md](u1b-report.md)；对照测试扩到 22 个 exe 全部一致（第 5 节） |
| A6 | selftest 一条命令全部通过：盲定位错误为 0、变异样本每类都有断言、往返通过 | 完成 | 第 3–5 节 |
| A7 | 构建、单元测试；测试目录自检到主菜单、运行时核对全部通过、没有新错误；`rig_textdiff.py` 没有多余补丁 | 完成 | 第 1 节：六个目标全量重建、两个单元测试、verify、selftest 在本阶段重跑；自检和 textdiff 沿用 [u1b-report.md](u1b-report.md) 第 8 节（U1c、U1d 没改 C++） |
| A8 | 第 7 节的文档；手册里的命令实际跑过 | 完成 | 第 6、7 节 |

## 11. 留给后面的事项

- 对抗审核：审核重点见 `04-roadmap.md` 的 U1。建议特别看变异样本是不是迁就实现（第 4.2 节写了做不到真实的地方），以及第 8 节的运行时残留风险。
- 主线：第 1 节的三件「主线没要求、我做了的事」是否保留；第 8 节的运行时残留风险怎么处理；08 里 U1b 的 6 条需要用户决定。
- 用户验收后：删除 `docs/u1/`（备份到 `build/u1_worklog/`），ADR 0036 的默认值定了之后去掉 README 里的「草稿」。
