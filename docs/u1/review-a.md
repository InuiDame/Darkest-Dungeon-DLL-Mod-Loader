# U1 对抗审核报告 A

审核对象：HEAD `ea788bf` 之上未提交的 U1 改动（清单、运行时核对、`tools/dd_update.py` 及 `tools/ddupdate/`、基线、对照测试、ADR 0035/0036、手册和接手说明）。没读 `docs/u1/` 下四份实现报告，结论都来自自己读代码和在 guest 上跑的命令。没启动游戏，没改源码和文档（本文件除外），没 commit。审核用的脚本、变异 exe 和输出都在 guest 的 `DD.ModLoader/build/u1review/`。

结论：**阻断 2 项、应修 7 项、备注 10 条**。两项阻断都是「验证报通过、版本被记为已知，但清单里的值是错的」，accept 之后运行时不再怀疑这些值，combat 的写操作会按错误偏移写 buff，最后写进存档。

## 阻断

### B1 依据位置「没法对照」按通过处理：结构里插入或删除字段后，verify 和 repair 都报通过，accept 把错误偏移记成已知版本

**位置**
- `tools/ddupdate/verify.py` 的 `_summarize`（264–268 行）：依据位置核对只把 `DISAGREE` 计为失效，`UNKNOWN` 只在报告里打一行 `--`，不影响退出码。
- `tools/ddupdate/position.py` 的 `check`（148–151 行）：指令对齐给不出结果时返回 `UNKNOWN`。两种对齐方法（唯一窗口、全局匹配块）结论不同时也算这种情况。
- `tools/ddupdate/layoutfix.py` 的 `assess_evidence`（207–216 行）：依据成立、位置是 `unknown` 时按「依据成立」处理，可信度照旧是「高」，不重新定位，`report.txt` 里也不出现这一项。

**问题**：ADR 0035 把依据位置核对写成防依据误绑定的手段，但它恰好在最需要它的情况下给出 `UNKNOWN`：一串形状相同的字段复制（复制构造里连续的 `mov eax,[rdx+X]; mov [rcx+X],eax`）中间多了或少了一项。这时第一个受影响字段的依据模式（靠字面量里的目的偏移区分，例如 `8B 42 ?? 89 41 44`）会唯一命中邻居字段的复制，读到的值又恰好等于旧常量。唯一窗口靠左边的上下文定位，全局对齐靠右边更长的匹配块定位，两者差一对指令，于是结果是 `UNKNOWN`，verify 照样给出退出码 0。

**触发条件**：游戏更新给 `Buff::Instance` 加一个 int 字段，或者删掉一个。字段位于 0x44–0x151 这段 int 字段之间时，0x152–0x158 原有的 6 字节尾部填充能吸收掉这 4 字节，Instance（0x410）和 Entry（0x418）的大小都不变，靠大小发现不了。这种改动在真实更新里很常见。

**反例 1：插入一个 int**（`build/u1review/mut_inst44.py`）。在 sub_type（char[64]，到 0x44）后面加一个 int：复制构造原地重排，多一对 `mov eax,[rdx+44h]` / `mov [rcx+44h],eax`。函数后面有 9 字节填充，原地放得下，不需要搬迁。复制构造、构造、BuffRuleActive、BuffGroupErase、BuffDurationUpkeep 里 [0x44,0x152) 的访问后移 4。真值：InstSubHash 0x48、InstAmount 0x4C ……
```
source DD.ModLoader/tools/env.sh
python -B DD.ModLoader/build/u1review/mut_inst44.py DD.ModLoader/build/u1review/inst44.exe
python -B DD.ModLoader/tools/dd_update.py verify DD.ModLoader/build/u1review/inst44.exe --json DD.ModLoader/build/u1review/inst44.verify.json
  依据位置……：对上 87、对不上 4、没法对照 1
  BIND InstAmount[0] / InstSource[0] / InstSourceData[0] / InstTarget[0] ……
  --   InstSubHash[0]  指令对齐不可用：唯一窗口（新下标 16）和全局对齐（新下标 18）不一致
  OK   InstSubHash                  = 0x44  运行时 1/1          ← 真值 0x48
python -B DD.ModLoader/tools/dd_update.py repair --exe DD.ModLoader/build/u1review/inst44.exe --out DD.ModLoader/build/u1review/inst44_repair
  （修了 15 项，都对；report.txt 里没有 InstSubHash）
  预演 verify（补丁应用到临时副本）：OK（全部通过）（退出码 0）
  结论：需要人工 必需 0、可选 0（退出码 0）
# 源码副本 build/u1review/tree（照 selftest 的 copy_tree 复制）
python -B DD.ModLoader/tools/dd_update.py repair --exe …/inst44.exe --apply --tree DD.ModLoader/build/u1review/tree --out …/inst44_apply
  apply_exit=0，apply 后 verify 结论：OK（全部通过）（退出码 0）
python -B DD.ModLoader/tools/dd_update.py accept --exe …/inst44.exe --label review_inst44 --tree DD.ModLoader/build/u1review/tree
  accept_exit=0；+DD_KNOWN_BUILD("63903d0f45f0b91ba431954e819ce70fb0eb7f21", "review_inst44")
  tree/core/src/game/layout.inc:185  DD_LAYOUT(InstSubHash, OFFSET, 0x44, …)
```
C++ 侧（`cpp_compare.py …/inst44.exe`，结论「一致」）：InstSubHash、InstAmount、InstSource、InstSourceData、InstTarget 的依据同样都是 ok，运行时没有位置核对。

**反例 2：删除一个 int**（`build/u1review/mut_inst_remove.py`，去掉 sub_hash，0x48–0x151 前移 4）：
```
verify：对上 87、对不上 3、没法对照 2
  --   InstSubHash[0]  旧指令 16 不在全局对齐的匹配块里
  --   InstAmount[0]   唯一窗口（新下标 18）和全局对齐（新下标 16）不一致
  OK   InstSubHash = 0x44（这个字段已经不存在）   OK   InstAmount = 0x48（真值 0x44）
repair：FIX高 InstDuration 0x4c → 0x48；InstAmount 不动，仍是 0x48；预演 verify 退出码 0；repair 退出码 0
```
修完以后 InstAmount 和 InstDuration 都是 0x48。

**后果**：accept 之后这个 SHA1 是已知版本，运行时核对用的依据语义和 Python 一样，同样判成立。`G_FillDefinition`（`combat.cpp` 229–248 行）在反例 1 里把 sub_type 的哈希写进新字段，在反例 2 里把 amount 写进 duration 的位置，再把哈希写进 amount。插件定义的 buff 经 `buff_add` 复制到英雄身上，进存档。accept 还会把这个错误的命中位置写进新基线，以后的版本做位置核对时，会和这条错误的指令「对上」。

**selftest 为什么没发现**：`mutants.py` 的 `struct_shift` 把插入点选在 0xB0，避开了这段 int 字段（301 行）；新字段也没有加复制指令，`unreal` 里写的理由是「加指令要挪函数，这个函数有 C++ 异常表」（501 行）。这个理由不成立：复制构造后面有填充，原地重排就够，审核脚本就是这么做的。不变式 1（verify 判通过的项也要等于真值）本身没问题，样本里加入这两个例子就能抓住。

**修法**
1. 不是本 SHA1 的基线时，位置 `UNKNOWN` 的依据，至少是成立的那些，在 verify 里计入失效（建议记为「需要人工」，退出码 ≥1，accept 拒绝）。repair 遇到「依据成立 + 位置 unknown」时不当作成立：走重新定位，定位失败就报 manual，并写进 `report.txt`。现在 Steam 版、离线版各自对本 SHA1 基线是 97 对 0 对 0 未知，这样改不影响已知版本通过。
2. 把这两个样本加进 `mutants.py`，truth 写全。
3. 可以顺手加一道清单检查：同一结构的 OFFSET 项不能重叠，宽度另行登记。反例 2 里 InstAmount 和 InstDuration 相同，这道检查能抓住。
4. 手册第 2、3 步写明：「依据位置」一节的 `--`（没法对照）和 `BIND` 一样，必须处理。

### B2 `accept --allow-optional` 能把离线依据失效的版本记为已知，运行时对已知版本不再核对 OFFLINE/NONE 项

**位置**：`tools/ddupdate/repair.py` 的 `cmd_accept`（699–702 行；拒绝信息里直接建议「用 --allow-optional 接受只有可选项失效的版本」），参数定义在 984 行。`core/src/game/layout_check.cpp` 的 `BuildGate`（1133–1140 行）：已知版本上 Offline 和 Unverified 项只计数，不停用任何东西。ADR 0036 的停用规则表把「已知版本 → 照常」的前提写成「accept 时 verify 全部通过」。

**问题**：所有 OFFLINE 项都是 OPT，失效时 verify 退出码是 1。加上 `--allow-optional`，accept 照样把 SHA1 写进 `DD_KNOWN_BUILD`、生成基线。之后运行时把这个版本当已知版本，失效的离线项被当作 accept 时核对过的。

**证据**（全新的源码副本 `build/u1review/tree2`，样本是 selftest 的 `enum.exe`，stat 枚举第 2、3 项对调）：
```
python -B DD.ModLoader/tools/dd_update.py accept --exe DD.ModLoader/build/u1d/mut/enum.exe --label review_enum --tree DD.ModLoader/build/u1review/tree2
  verify 退出码 1，不接受（先 repair，或用 --allow-optional 接受只有可选项失效的版本）
… 同上再加 --allow-optional
  WARN EnumStat = 0x66  离线 0/1；结论：只有可选项失效（退出码 1）
  +DD_KNOWN_BUILD("0d67aa44b3620e4f53838217e247fc1c86dc0fb2", "review_enum")；退出码 0
DD.ModLoader/build/windows/x64/releasedbg/dd_layout_compare.exe DD.ModLoader/build/u1d/mut/enum.exe --json …/enum.cpp.json
  EnumStat state: offline；known gate off: []
```
**后果**：已知版本上 `combat.buff_define`、`buff_add` 照常可用，按旧表把 `combat_stat_add` 和 `combat_stat_multiply` 互换着写进 buff，进存档。其他 OFFLINE 项失效（来源表、规则表、来源标志、BattleState 等）也是一样。

**修法**：去掉 `--allow-optional`；或者只在失效项全是 RUNTIME 时允许（这些项运行时还会再判一次、会被停用），有 OFFLINE 项失效一律拒绝，拒绝信息里也不再推荐它。另一个办法是在 `DD_KNOWN_BUILD` 旁边记下这个版本 accept 时失效的项，运行时照样停用它们。

## 应修

### S1 未知版本上的「只读」归类和 ADR 0036 自己的定义冲突：hook 主题、`game.actor_info`、`persist` 都会改游戏代码或调用游戏函数，依赖的特征码和函数原型运行时都核对不了

- **位置**：ADR 0036 把「写」定义成「会写游戏内存、调用游戏函数或改变结算」，`layout_check.cpp` 的 `kDepInfo`（876–899 行）按读写归类，`events/system.cpp` 的 `BuildHookDefs`（2030–2097 行）安装 hook。
- **问题**：
  - `game.actor_info` 归为读，但 `ActorReadInfo` 会调用游戏函数 `ActorMaxStat`（`actors.cpp` 42–45、280 行）。这条 CODE 特征码运行时没有同一性核对。
  - `persist` 归为读，但它在存档读写函数上装 4 个内联 hook。`QueueWriteDetour`、`ReadFileDetour` 按固定的 9 个、6 个参数转发给原函数（`persist.cpp` 31、835 行）。游戏更新给这些函数加了参数时特征码照样命中，原函数会收到垃圾参数，直接影响存档写入。原型不在清单里，未知版本上照常安装。
  - 只读主题（`hit.post`、`skill.*`、`effect.post`、`battle.*`、`stress.post`、`heal.post`、`damage.post`、`buff.*`）一有订阅就装内联 hook 或改虚表槽。运行时没有 HOOK 命中位置必须是函数入口的检查，也没有同一性检查（ADR 0036 承认这两点），detour 的原型也不在清单里。`damage.pre`、`heal.pre` 可写、可取消，同样只靠运行时依据保持开启（`build/rig_logs/u1b-selfcheck/topics.json` 是按未知版本导出的主题表，里面这两个主题是 `available: true`），它们的原型（`HeroDamageDetour` 有 8 个参数）也没登记。
- **触发条件**：Steam 自动更新改了这些函数的原型或位置，而特征码仍然唯一命中，或者 hook 落在挪下去的旧序言上。
- **修法**：
  - 照 ADR 自己的定义，把「装 hook、调用游戏函数」算成写：特征码在未知版本上没有运行时同一性依据时，停用依赖它的 hook 主题和服务；至少要把 persist 和 actor_info 改为写。
  - 运行时加一条便宜的检查：HOOK 的命中必须是函数入口。C++ 已经有 `FunctionTable`，`image.is_function_start` 的逻辑可以直接搬过来。
  - 给被 hook 和被调用的函数登记原型项（`DD_UNVERIFIED` 加依赖表），让未知版本的策略能把它们算进去。
  - ADR 0036 的「待用户决定 4（persist 算只读）」请附上这个风险。

### S2 `VtSlotStress` 没有静态依据，未知版本上只读的 `stress.post` 仍然改写虚表槽 36

- **位置**：`layout.inc` 508 行（NONE，依赖表只有 `topic:stress.*`）；`system.cpp` 2075–2078 行（Hero、Monster 虚表槽 `kVtSlotStress` 换成 detour）；`tests/unit/test_main.cpp` 的 `TestPolicy` 明确断言未知版本上 `!gate.TopicOff("stress.post", …)`。
- **问题**：按规则，不能核对的项只停用可写的主题，所以 `stress.pre` 停用，`stress.post` 照常。可是 `stress.post` 本身就要按一个未经核对的槽号改写游戏的虚表，并假设那个函数的原型是 `(self, prng, float, source, int)`。现在之所以没出事，靠的是附带的耦合：槽号整体后移时，ActorStress 在固定槽 37 上的依据通常也会失败，`topic:stress.*` 被整条停掉。只换掉槽 36、37 不变时（虚函数声明换了顺序，或者槽 36 换成了别的函数），就会把 detour 挂到别的虚函数上。
- **修法**：改虚表槽或装 hook 的主题，依赖的槽号或特征码不能核对时，连只读的主题一起停用。或者给 VtSlotStress 补运行时依据：槽 36 的函数会调用 480FE0，加一条 ANCHOR 特征码，再写一条 REL32 依据。

### S3 清单覆盖缺口：函数原型、字段宽度和类型、几个偏移 0 的字段、Listener 虚表的用法

- 函数原型和调用约定都不在清单里：hook 的 detour（ApplyHit/Miss、技能、EffectApplyInternal 有 13 个参数、槽 50/51/36、ConsumeHealthDamageBlock、存档的 4 个、CreateAppSystems、UpdateStateInternal），以及调用的游戏函数（ApplyEffect 有 12 个参数、PrngFloat、FreeU32Vector、Instance 复制构造和析构、BuffLibraryIndex、ParseEffectFile、ListenerAdd、ActorMaxStat、槽 13 AddBuff、槽 14 按值传的 `std::function`）。简报第 1 节说「全部游戏布局」；参数变化是游戏更新里常见的改动，verify 的指纹阈值 0.85 未必能发现。
- 清单只记偏移，不记字段宽度和类型。C++ 一律按 4 字节读写 int 或 float（`combat.cpp` 233–247、436–443 行），例如 guid 计数扩成 64 位时，verify 和运行时都不会察觉。
- 偏移 0 的字段：`GroupEntry` 的组 guid（`combat.cpp` 286、308、436 行，`Rd<uint32_t>(first + i*kBuffEntrySize)`），以及 vector 的 first（`VecFirst` 的 `ReadPtr(vec, 0)`）都没有登记。同样在偏移 0 的 InstStat、MapNodeLeft、EffectName 是登记了的，口径不一致。
- 伪造的 Listener 虚表只填槽 0（`system.cpp` 1507–1517 行），也就是假设游戏只调用这一个槽；RTTI 结构的大小（0x1C、0x10、0x18）是 MSVC ABI。std::map、std::function 的布局进了清单，这两个没进。
- **修法**：给原型和宽度加 `DD_UNVERIFIED` 项和依赖表，至少让未知版本的策略和「更新后要人工核实」的清单能看到它们；补登 EntryGuid、StdVectorFirst。

### S4 repair 和 accept 不会提示「和已经变化的项同组、却没有依据」的项；accept 也不要求 NONE 项核实过

- **位置**：`layoutfix.py` 的 `run`（NONE 项一律直接标 `unverified`）、`selftest.py` 的不变式（跳过 unverified）、`cmd_accept`。
- **问题**：例如 Actor 基类在槽 36 之前加了一个虚函数。repair 会把 VtSlotDamage、Heal、DeathsDoor、Dead 都加一，VtSlotStress（NONE）仍停在 36。报告只在固定的「更新后要人工核实」清单里列出它，和平时完全一样，看不出这次需要特别注意。accept 只看 verify 的退出码，手册第 6 步（核实 NONE 项）有没有做，工具不过问。accept 之后，已知版本上 NONE 项全部被当作核对过。
- **修法**：同一组（同一张虚表、同一个结构）里有 RUNTIME 项的值变了时，把组里的 NONE、OFFLINE 项升级成「需要人工」。accept 要求显式确认 NONE 项，例如传入 `--confirm-unverified <清单>`，或者给出探针和 test_events 的日志路径。

### S5 `cpp_compare.py` 把所有「Python 失效、C++ 成立」的特征码都当作 Python 独有的检查，C++ 解析逻辑退化了也查不出

- **位置**：`tools/ddupdate/cpp_compare.py` 111–115 行。只要命中和解析结果一致、状态是 Python fail / C++ ok，就归入 `static_only`，再用 `--sig-fail` 让 C++ 跟着失效，不核对 Python 的失败原因是不是真属于静态检查（偷取范围、函数入口、`DD_ENTRY`、hooks.md 重叠）。
- **证据**（故意改坏一份副本）：在 `build/u1review/tree2` 里把 `layout_check.cpp` 652 行「必需 GLOBAL 必须指向 .data/.rdata」的检查改成 `if (false)`，编出 `dd_layout_compare`。再造一个样本 `statelast_text.exe`（`mut_statelast.py`：StateLast 的 rip 位移指向 .text）：
  ```
  python -B DD.ModLoader/tools/ddupdate/cpp_compare.py DD.ModLoader/build/u1review/statelast_text.exe --bin DD.ModLoader/build/u1review/tree2/build/windows/x64/releasedbg/dd_layout_compare.exe --out …/compare_mut
    Python 独有的检查……：StateLast：解析出的全局 3DCE85 不在 .data/.rdata（.text）
    结论：一致 / 结果：全部一致，退出码 0
  ```
  这项检查 C++ 本来也做，并不是 Python 独有。（`tree2` 的源码已经还原，`build/` 下那个 exe 仍是改坏的版本。）
- **修法**：`static_only` 只收失败原因全部属于静态检查集合的特征码（`SigResult` 给 problems 加上类别），其余一律算不一致。

### S6 GLOBAL_ANY 没有同一性检查，必需的 `EventTypeCount` 解析到别的全局时 verify 照样通过

- **位置**：`verify.py` 的 `_identity`（83–89 行）。GLOBAL_ANY 只比候选数，不同也只记一条 note，状态仍是 ok。同时 `position.py` 对锚在它最低命中上的 WINDOW 依据，函数对不上时多半落到 `UNKNOWN`，又回到 B1。
- **触发条件**：更新后 Event 派发器的模板实例换了形状，而某个别的模板族（例如 Query 的）恰好匹配这条模式，并且全部解析到同一个全局。
- **后果**：`RegisterNativeTopics` 按错误的类型数遍历类名表，按 `type.id` 往 `EventManager` 的列表数组里注册 listener（`ListOf`），可能越界写游戏内存。
- **修法**：GLOBAL_ANY 至少检查候选里引用这个全局的函数和基线里的候选函数指纹相近，或者要求所有命中所在的函数和基线里的那组函数对上。候选数变了时算失效，不只是记一条 note。

### S7 自测样本有迁就实现的地方：新字段对应的代码一律没加

- `struct_shift`：插入点和新字段的复制指令，见 B1。
- `evidence_misbind`：Effect 里插入 8 字节，解析函数里没有加新字段的解析代码（`mutants.py` 480 行的 unreal 也这么写了）。真实情况下，新字段会有自己的 `lea r8,[rsi+X]; call 读取函数`。这时：①离线检查 `effect_field_types` 只看「这个偏移有没有被同一类读取函数解析」，新字段用 float 或 int 读取函数时会误判通过；②ParseEffectFile 的指令对齐会遇到插入，容易出现 B1 那样的 `UNKNOWN`。
- **修法**：样本按「新字段有完整对应代码」来造。函数后面空间不够时，用 B1 的原地重排；还不够，就放宽 relocate，允许带异常处理信息的函数搬迁（工具只读 .pdata，不需要 FuncInfo 正确）。`unreal` 里不要再把能做到的事写成做不到。

## 备注

1. `signatures.inc` 273 行的注释「未知 SHA1 但特征码全部命中时只警告」已经被 ADR 0036 取代，没跟着改。
2. C++/Python 有一处边界不一致：`ResolveSignatures` 对 `Required || Loader` 的 GLOBAL 检查 .data/.rdata（`layout_check.cpp` 652 行），Python 只检查 REQUIRED（`sigs.py` 125 行）。现在没有 LOADER 的 GLOBAL，走不到。
3. 还有一处不一致：`evidence.read_value` 在映像末尾读不满宽度时不报错，按读到的字节算值；C++ 报「超出映像」。命中都在 .text，实际走不到。
4. verify 不检查 HOOK 的 `steal >= 5`（`check_hook` 只查指令边界）。加载器（`early_hook.h` 136 行）不管 steal 多少都写 5 字节 jmp，也不检查。regen 保证了 ≥5，人工改 `CreateAppSystems` 时可能出错。DDCore 的 hook 引擎会拒绝小于 5 的。
5. 不少离线检查只查「存在」：`raid_battle_lea`（任何 `lea [reg+0x4898]`）、`skill_effects_offset`（调用 EffectLookup 的函数里任何 `0x3c8]`）、`effect_misc_fields`、`instance_copy_fields`、`hero_stress_slot`、`effect_field_types`。只有离线依据的项（ClassId、EffectTarget、SkillEffects 等），在真实插入时可能误判通过。目前受影响的都是只读能力。
6. `check_actor_layout.py` 这个薄封装跑的是 `verify --only layout`，不做依据位置核对，B1 的 4 条 BIND 它都看不到。`knowledge/README.md` 却说旧名字「指的都是」`dd_update.py verify`。
7. `08-handoff.md` 说三类样本在玩家那边会产生的后果（`hook_entry` 的 hook 装在函数中间、`evidence_misbind` 的 effect_get 读错字段、`vslot` 的 heal.* 挂到伤害函数上），和停用开关的实际结果不符。用 `dd_layout_compare` 看这三个样本的 known 和 unknown 开关：分别是 `damage.*` 整条停用、`combat.effect_get` 停用、`heal.*` 整条停用。描述偏严，不危险，但请用户决定 ADR 0036 时知道：这几次保护都来自别的项（ActorHp、ActorStress、EffectDot*）碰巧也失败了，设计上并没有保证。
8. 相似度数据（`build/u1a/simstats.txt`）：最近冒充者有 3 条高于阈值（BuffLibraryIndex 0.941、FreeU32Vector 1.0、VectorDtor64 1.0），ActorDoAttackSkill 的冒充者是 0.804。「同一函数跨版本」只有同源的两个构建（最低 0.929），阈值 0.85 对真实更新的区分力还没有数据。
9. 必需的 `ListenerFunction` 锚在可选的 `MonsterBlockReturn` 上，这条特征码失效时 DDCore 整体停用（ADR 0036 已写明，列为待用户决定）。
10. `simstats`、盲定位都只在 Steam 版和离线版之间做，这两个版本的代码几乎一样。盲定位「错误 0」证明流程走得通，不能证明在真实更新上的区分力（ADR 0035 和接手说明里已写）。

## 无法确认的问题

- **真实更新下的同一性阈值和修复区分力**：没有旧版或新版 exe，只有同源两个构建。缺的证据是一个真实的旧版本，或者重新编译出来的样本。
- **「模板孪生且两份都有调用者」时 repair 的选择**：没有构造。现有 `twin` 样本的复制品没有调用者，靠「基线里有调用者」这条就判掉了。缺一个两份都有调用者、调用图也对称的样本。
- **S1 里原型变化在真实更新中出现的概率**：只能从代码推断后果（固定参数个数转发），没有游戏在原型变化后的运行证据。
- **游戏内表现**：没启动游戏。A7 的测试目录自检只核对了 `build/rig_logs/u1b-selfcheck` 的日志（11-16 11:35，在 `core/` 最后一次修改 11:25 之后，特征码 61、依据 114、清单项 227 和现在的源码一致；「全部通过，没有停用任何能力」）。U1c、U1d 之后没有新的游戏内运行，但这两个阶段没改 `core/`、`loader/` 的源码（文件时间可以佐证）。

## 查过没有发现问题的范围

- **旧值快照**：`layout_golden.cpp` 和 HEAD 对比，脚本 `build/u1review/golden_check.py` 从 `git diff ea788bf -- core loader` 里取出 115 个被删除的 `constexpr` 常量，全部有同值断言（唯一一个没对上名字的 `kEffectSwapRoles` 和 combat 的 `kEffectSwap` 同值，在内）；表、内联字面量（事件和 Query 负载、52/56、0x40 等）都有对应断言。`xmake run dd_core_tests` 1206 项，失败 0；DDCore、DDModLoader、DDLua、dd_layout_compare 都构建通过。
- **基线可复现**：`dd_update.py baseline --out build/u1review/baseline_regen` 重新生成，和仓库里 `steam.json`、`nosteam.json` 的 sha256 完全相同。
- **已知版本校验**：Steam 版、离线版 `verify` 退出码 0，依据位置都是 97 对上、0 对不上、0 没法对照。
- **C++/Python 依据语义**：逐段对照了起点（HIT、TARGET、HIT_VALUE、VSLOT 的基址换算）、范围（ENTRY、WINDOW 越界截断、FUNC 的二分、间接表项、CHAININFO、重复 BeginAddress 保留最后一个、`[begin,end)`）、扫描（可重叠、必须完整落在范围里）、读取宽度和符号扩展、DEREF/REL32 的边界、rel 失效时 blocked、隐含值、GLOBAL_ANY 的最低命中和多候选规则。除备注 2、3 外没有发现不一致。`inst44.exe` 上两边逐项一致。
- **停用开关的接入**：每个依赖表的词都核对了服务入口（`core.cpp` 的 `Gated` 和 `CombatWriteGated`，覆盖 combat 的 6 个写操作、规则注册、枚举、buff 和 effect 的读、技能、actor 系列、battle、mods）、`events/system.cpp`（pvp、EventsNative、EventsQuery 门）、`CreatePersist`、`ReadEnabledModList`、`ApplyTopicGate`。内置主题全部在 `ApplyTopicGate` 之前注册（`System` 初始化和 combat、persist 的 `AddTopic`）。Lua 只能经 SDK 服务到达，开关只在启动时写一次，延迟队列的入口也在开关之后。反过来抽查了 kActorHp、kActorStress、kMonsterClass、kHeroClass、kHeroId/MonsterGuid、kLibraryAltMap、kInstTarget、kInstSubHash、kGameHashMul 的使用点，依赖表都登记了。
- **加载器**：`early_hook.h` 改用 `DD_ENTRY` 比对入口字节，唯一命中的要求不变。
- **盲定位**：`blind_compare` 用 `Matcher(relaxed=False)`，`locate_sig` 只在基线是 format 1 时用模式在旧函数里找锚点下标，不在新 exe 里按现有特征码扫描；现有特征码只用来生成参照和检查重新生成的模式。
- **往返**：selftest 的往返在副本里用修复后的清单编译了 DDCore。C++ 里由清单推出的长度有 `static_assert` 绑定（`FakeListener`、`ListData`、`kPredVtable`、`kImplVtableStorage`、mods 的缓冲），改值时会编译失败、需要人工处理，不会悄悄出错。
- **手册**：在源码副本里实际走了第 2、3、4、5（`cpp_compare.py`）、7 步，命令都能跑，输出和手册描述一致（SHA1、「未知版本」、`FIX高/FIX中/MANU`、`rva_map.tsv` 的列）。第 6 步需要启动游戏，没有做。
- **SDK**：`templates/native/README.md` 的第 2、3、4 步（`verify --sigs`、`baseline --sigs --out`、带 `--baseline` 的 verify）都实际跑过，输出符合描述。
- **ADR 0036 和实现**：停用规则表、词汇和开关表、特征码同一性依据不成立时不给地址、`DD_LAYOUT_ASSUME_UNKNOWN` 都和代码一致。「已知版本 → 照常」的前提被 B2 打破，读写归类见 S1。

## 审核产物（guest `DD.ModLoader/build/u1review/`）

- `mut_inst44.py` → `inst44.exe`，以及 `inst44.verify.*`、`inst44_repair/`、`inst44.apply.txt`、`inst44.accept.txt`、`tree/`（已 accept 的副本）、`compare/`
- `mut_inst_remove.py` → `inst_remove44.exe`，以及 `inst_remove44.verify.txt`、`inst_remove44_repair/`
- `enum.accept*.txt`、`enum.cpp.json`、`tree2/`（B2；其中 `build/` 下的 dd_layout_compare 是 S5 改坏后编出来的）
- `mut_statelast.py` → `statelast_text.exe`，以及 `compare_mut/`、`compare_ok/`（S5）
- `golden_check.py`、`core.diff`、`baseline_regen/`、`base.verify.*`、`sdk/`、`vslot.cpp.json`、`hook_entry.cpp.json`、`evidence_misbind.cpp.json`
