# U1b 报告：运行时核对和降级

阶段工作文件，用户验收后删除。简报见 [brief.md](brief.md)，范围是第 5 节，验收项 A5 和 A7。完整输出在 guest 的 `D:\workplace\DD.ModLoader\build\u1b\`（下文的文件名都相对这个目录，`rig_logs` 指 `build/rig_logs/`）。

## 1. 结论

| 验收 | 状态 | 证据 |
| --- | --- | --- |
| A5 运行时核对：`Resolve` 之后对全部可在运行时求值的依据求值，语义和 Python 相同 | 完成 | `core/src/game/layout_check.cpp`；自检日志 `rig_logs/u1b-selfcheck/DDCore.log` 第 4 行（114 条依据全部成立） |
| A5 停用：不成立时只停依赖它的服务和主题，日志写明项、期望值、实际值、停用了什么；必需项照 ADR 0006 | 完成 | 第 4 节对照表（每个词汇都有开关）；`dd_core_tests` 的「布局核对」一节；未知版本模拟 `rig_logs/u1b-unknown-topics`；破坏样本的停用结果在 `compare_run.txt` |
| A5 未知版本策略写成 ADR | 完成（草稿） | `docs/adr/0036-runtime-layout-check.md`，`docs/README.md` 的 ADR 表；默认值待用户确认（第 9 节） |
| A5 `ListenerPriority`、`ListenerFunction`、`StdFunctionImpl` 补依据 | 完成 | 补了这 3 项和同组的 `StdFunctionSize`、`StdFuncImplVtableSlots`、`EventTime`（第 3 节）；未知版本上 DDCore 和事件桥不再停用 |
| A5 C++/Python 对照一致 | 完成 | `compare_run.txt`：Steam 版、离线版、U1a 的 2 个破坏样本、U1b 的 8 个破坏样本，12 个 exe 全部一致，退出码 0 |
| A7 构建、单元测试 | 完成 | `build.txt`：DDCore、DDModLoader、DDLua、dd_core_tests、dd_lua_tests、dd_layout_compare 全部 `xmake build -r` 通过，没有警告；dd_core_tests 1206 项失败 0，dd_lua_tests 1291 项失败 0；`layout_golden.cpp` 没改 |
| A7 测试目录自检到主菜单 | 完成 | `rig_logs/u1b-selfcheck`（方案 scenario4）：DDCore 正常启用、DDLua 加载、运行时核对全部通过，DDCore.log 和 DDLua.log 没有 WARN/ERROR；和 P4 的同方案日志相比只多 3 行核对日志 |
| A7 `rig_textdiff.py` 没有多余补丁 | 完成 | `rig_logs/u1b-selfcheck/textdiff_scenario4.txt`：DDCore 的 5 处（每帧 + 4 个 persist 观察 hook）和加载器的 1 处，其余 14 处是测试目录里别的 DLL 的，和 P4a 的记录一致 |

测试目录最后部署的方案：**scenario4**（2026-11-16 11:35:34），和开工前一样。

## 2. 构建结果

guest 上 `source DD.ModLoader/tools/env.sh; dd_config`，逐个 `xmake build -r`（`build.txt`）：

```
== xmake build -r DDCore            [100%]: build ok
== xmake build -r DDModLoader       [100%]: build ok
== xmake build -r DDLua             [100%]: build ok
== xmake build -r dd_core_tests     [100%]: build ok
== xmake build -r dd_lua_tests      [100%]: build ok
== xmake build -r dd_layout_compare [100%]: build ok
== xmake run dd_core_tests          1206 项检查，失败 0
== xmake run dd_lua_tests           Lua 5.4.9，1291 项检查，失败 0
```

dd_core_tests 比 U1a 多的 478 项是新加的「布局核对」一节：最小 PE 上的函数表（间接表项、`UNW_FLAG_CHAININFO`）、扫描（可重叠、完整落在范围里）、一条 VSLOT + FUNC 依据（成立、换基址后不在 `.text`、值变了并反推、起点特征码失效）、清单表（依赖表的词都认识、依据的对象和 rel 都存在）、停用策略（已知版本、未知版本、不成立、必需项不成立和没法求值、只读项没法求值、误绑定、主题规则）、`Hub::Gate` 之后不能再置为可用。

`dd_update.py verify`（`verify.txt`）：Steam 版和离线版都是退出码 0，清单 61 条特征码（ANCHOR 2）、227 项（RUNTIME 88、NONE 118、OFFLINE 21）、依据 114 条。

## 3. 补的依据（清单内容，格式没改）

U1a 第 9 节指出 `ListenerPriority`、`ListenerFunction`（必需）和 `StdFunctionImpl` 不能运行时核对。按 ADR 0036 的原则，事件桥（`events.native`、`events.query` 是写能力）和 DDCore（`core`）在每次游戏更新后都会停用。补的依据（`layout.inc` 里写了指令和推理，`knowledge/dd1/events.md`、`combat.md` 记了 [静态] 结论）：

| 项 | 依据 | 两个版本的命中 |
| --- | --- | --- |
| `ListenerPriority`（必需） | Event 列表的插入排序 `3CCF00`：取表项的 active `+0x10`、listener `+8`、注册时刻 `+0x18`、标志 `+0x20`，再 `mov rax,[rsi+8]; mov ecx,[rax+8]; cmp [r14+8],ecx; jl`。带四个字段的这段在整个 `.text` 唯一（NOSIG TEXT，两条依据分别读两次比较的位移） | Steam `3CCF41`，离线 `3BD0A1` |
| `ListenerFunction`（必需） | 派发：`mov rcx,[rdi+0x48]; test rcx,rcx; je; lea rax,[rbp+..]; mov [rbp+..],rax; mov rax,[rcx]; lea rdx,[rbp+..]; call [rax+0x10]; cmp eax,1`，`0x48 = ListenerFunction + StdFunctionImpl`（`rel StdFunctionImpl`）。在 `MonsterBlockReturn` 所在函数（`5FC3E0`）里唯一 | Steam `5FC754`，离线 `5EA064` |
| `StdFunctionImpl` | Hero 槽位 14（`RemoveMultipleBuffs`，按值接收 `std::function`）结尾的析构：`mov rcx,[rbp+0x38]; test; je; mov rax,[rcx]; cmp rcx,rbp; setne dl; call [rax+0x20]`，读 `+0x38` | Steam `5CE713`，离线 `5BC043` |
| `StdFunctionSize` | 同上，`add -8`：实现指针是最后一个成员（推出的依据，见第 9 节） | 同上 |
| `StdFuncImplVtableSlots` | 同上读 `call [rax+0x20]`（`_Delete_this` 槽 4 = 槽数 - 2，`mul 8 add -16`），派发里读 `call [rax+0x10]`（`_Do_call` 槽 2 = 槽数 - 4，`mul 8 add -32`） | 同上两处 |
| `EventTime` | 新锚点 `ListenerEntryVtable`（GLOBAL ANCHOR，`ListenerAdd` 开头的 `lea rsi,[E64668]`），槽 1 是叶函数 `361610`：`cmp byte [rcx+0x20],0; je; mov rax,[rdx+8]; cmp [rcx+0x18],rax`，读事件的 `+8` | Steam `361610`，离线 `3A0050` |

没走「运行时数据自洽检查」：它只能在 AppReady（EventManager 建好、游戏的 listener 注册完）之后做，那时插件已经加载，必需项失败时没法按 ADR 0006 整体停用；静态依据两个版本都找到了。

加了新特征码，基线用 U1a 的命令重新生成（`python -B DD.ModLoader/tools/dd_update.py baseline`；不重生成时 `ListenerAdd` 的关系一致度变成 0、被判误绑定）。第二次生成到 `baseline_run2/`，两次字节一致：

```
2f1210e2e8d291a4c72610d0d9b11ac787f67922eaecccec911d5ae035bdd5ad  tools/baseline/nosteam.json（371958 字节）
221745f6708b3dad799fe4a7aafa044a2e73e2ded21f3fbb57f4d460008f9584  tools/baseline/steam.json（374688 字节）
（baseline_run2 下 sha256 相同，cmp 一致）
```

基线比 U1a 的大，是因为生成时用的是当前的 `tools/ddupdate/baseline.py`（`FORMAT = 2`，U1c 加了字段），我没改它。

## 4. 依赖词 → 停用开关

写/读的划分在 `layout_check.cpp` 的 `kDepInfo`，策略见 ADR 0036。开关都是 `game::Addresses::gate`（`check::Gate`），`CoreBoot` 填好之后只读；服务入口查 `Gated(Dep::...)`（`core.cpp`）。

| 词汇 | 写/读 | 开关的位置和行为 |
| --- | --- | --- |
| `core` | 写 | `core.cpp` `ApplyLayoutCheck` 返回 false → `CoreBoot` 返回 `kCoreBootDisabled`，日志列出不会加载的插件 |
| `loader` | 写 | 加载器 `early_hook.h` 的 `Install`：唯一命中 + 入口字节和 `DD_ENTRY` 一致才装早期 hook。`layout.inc` 里没有依赖 `loader` 的项（`early_hook.h` 开头补了说明），DDCore 的开关不用它 |
| `events.native` | 写 | `system.cpp` `RegisterNativeTopics`：全部 `native.*` 和映射来的语义主题 `Hub::Gate`，没有订阅就不往游戏的 listener 列表注册 |
| `events.query` | 写 | `system.cpp` `RegisterQueryTopics`：全部 `query.*` `Hub::Gate` |
| `pvp` | 写 | `RegisterSemanticTopics`：`damage/effect/skill/hit/stress/heal.pre` 和 `damage.final` 停用；`RegisterQueryTopics`：`query.*` 停用（Query 写回）；`core.cpp` `CombatWriteGated`：5 个写操作返回 `DD_COMBAT_E_UNAVAILABLE`，`rule_register` 拒绝 |
| `game.actor` | 读 | `core.cpp` `ActorEnvNow`：Hero/Monster 虚表置空，所有句柄 API（含 combat 经句柄解析 actor）失败 |
| `game.actor_info` | 读 | `SvcGameActorInfo` 返回 false |
| `game.actor_list` | 读 | `ListActors`（`party_heroes`、`battle_monsters`）返回 false |
| `game.battle` | 读 | `SvcGameBattleState` 返回 -1、`SvcGameBattleSerial` 返回 0、`ObserveBattle` 不取样、`ActorEnvNow` 不给怪物句柄 |
| `game.mods` | 读 | `SvcGameModsAvailable` 为假，`ReadEnabledModList` 不读 |
| `combat.buff_read` | 读 | `SvcCombatBuffList`、`BuffGet`、`BuffDefGet` 返回 false |
| `combat.buff_add` / `buff_remove` / `buff_define` / `effect_apply` / `effect_define` | 写 | 对应 `SvcCombat*` 返回 `DD_COMBAT_E_UNAVAILABLE`，`message` 写明布局核对（排队执行的操作也只能从这些入口进来） |
| `combat.effect_get` | 读 | `SvcCombatEffectGet` 返回 false |
| `combat.rule` | 写 | `SvcCombatRuleRegister` 返回 0 并写日志；没有规则就不装规则 hook |
| `combat.skill` | 读 | `SvcCombatSkillFromPointer`、`SkillInfo` 返回 false |
| `combat.enum` | 读 | `enum_count` 0、`enum_name` 空、`enum_value` -1、`source_flags` 0 |
| `persist` | 读 | `CreatePersist` 不创建（persist 服务查不到，`campaign.save` 不注册） |
| `check` | — | 只给校验用，DDCore 忽略 |
| `topic:<主题>` | 按主题 | `core.cpp` `ApplyTopicGate`（事件系统、combat、persist 的主题都注册完、插件加载之前）：匹配的主题 `Hub::Gate`（插件自定义的主题不受影响）；不能核对时只停可写或可取消的主题 |

`Hub::Gate`（`hub.h/.cpp`）把主题标成不可用并置 `gated`，之后 `SetAvailability(true)` 不再生效（persist 声明后会把 `campaign.save` 置回可用，这样防住了）。

## 5. 运行时核对的实现

- `core/src/game/layout_check.{h,cpp}`：从 `signatures.inc`、`layout.inc` 生成特征码、清单项、依据三张表（X-macro，`ANCHOR` 和 `LOADER` 都在表里）；`DescribeImage` 解析 PE 头；`FunctionTable` 自己解析异常目录（排序、同起点只留最后一个、间接表项、`UNW_FLAG_CHAININFO` 最多 32 次，和 `image.py` 逐行对应）；`Scanner` 按 `.text` 采样的字节频率挑最少见的固定字节用 `memchr` 找候选（只改搜索顺序，命中集合不变）；`ResolveSignatures` 照 `sigs.resolve`（含必需 GLOBAL 指向 `.data/.rdata`、GLOBAL_ANY 的候选和 `DD_RUNTIME_SELECT`）；`Evaluator` 照 `evidence.evaluate`（五种起点、四种范围、九种读取、比较和反推）；`RunCheck` 汇总每项的结论和误绑定；`BuildGate` 是策略。
- `game::Resolve` 改用共享的解析器：扫描一次，`RunCheck` 在装任何 hook 之前求值全部依据；同一性依据不成立的特征码不给地址（按缺失处理）。锚点特征码现在也解析，失败只记日志。行为变化：必需 GLOBAL 必须指向 `.data/.rdata`、GLOBAL_ANY 有候选超出映像时失效（和 Python 一致，真实 exe 上不触发）。
- `core.cpp` `CoreBoot`：`Resolve` 之后按 `DD_KNOWN_BUILD` 判断版本，`ApplyLayoutCheck` 生成开关并写日志，必需项落到停用时整体停用；`CreatePersist` 之后 `ApplyTopicGate`。
- 测试开关：环境变量 `DD_LAYOUT_ASSUME_UNKNOWN=1` 让 DDCore 在已知版本上按未知版本处理（只会更严，日志第一行写明）。
- `HOOK` 偷取范围的指令检查、`hooks.md` 重叠、`DD_ENTRY` 比较不在运行时做（理由见 ADR 0036）。

## 6. 开销

自检时 DDCore 自己计时（`rig_logs/u1b-selfcheck/DDCore.log` 第 4 行，已知版本也照样核对）：

```
布局核对：特征码 61 条扫描 56.4 ms；运行时依据 114 条（成立 114、不成立 0、没法求值 0），求值 13.2 ms（其中函数表 2.6 ms）；
清单项 227 个（运行时核对通过 88、不成立 0、没法求值 0、只能离线核对 21、没有静态依据 118），已知版本
```

另外几次运行：扫描 54.6–56.4 ms，求值 11.9–13.8 ms，函数表 2.4–4.2 ms。

每条依据的耗时用 `dd_layout_compare --repeat 5` 在映射的 Steam 版上测（先完整求值一遍不计时，再重复 5 次取平均；`compare/steam.cpp.json` 的 `micros`）：

| 范围 | 条数 | 合计 | 平均 | 说明 |
| --- | --- | --- | --- | --- |
| TEXT（整个 `.text`，12.4 MB） | 12 | 10.17 ms | 847 µs | 755–978 µs 一条：`BattleGuidCounter` ×2、`RosterAlt`、`RosterHeroes` ×2、`ListenerPriority` ×2、`RaidHeroes` ×2、`RaidBattle`、`BattleMonsters` ×2 |
| FUNC | 76 | 0.21 ms | 2.7 µs | 最长 10 µs（`ParseEffectFile` 里的两条） |
| ENTRY | 17 | 0.03 ms | 1.8 µs | |
| WINDOW | 9 | 0.02 ms | 1.8 µs | |
| 函数表（一次） | — | 2.4–4.2 ms（游戏内） | | 映射文件上测是 23 ms，多出来的是新分配的 30 MB 缓冲第一次访问的缺页 |

离线版的数字几乎相同（合计 10.3 ms）。同一个 TEXT 模式被两条依据各扫一遍（5 组），按模式缓存命中可以把 TEXT 部分减半，现在没做。

## 7. C++/Python 对照测试

- C++：`tests/layout_compare/main.cpp`（xmake 目标 `dd_layout_compare`），自己按节映射 exe（头部和每个节的原始数据放到 RVA，和 `image.py` 一样，不执行、不重定位，虚表指针减 `OptionalHeader.ImageBase`），跑 `layout_check.cpp`，写 JSON（特征码、每条依据、每项结论、按已知/未知版本的停用开关和日志）。`--sig-fail` 把指定特征码当作失效。
- 比较：`tools/ddupdate/cpp_compare.py`（新文件）。每个 exe 跑 `dd_update.py verify <exe> --json` 和 `dd_layout_compare`，逐项比较特征码的 `hits`、`hit`、`resolved`、`candidates`、`status`，每条依据（含特征码的同一性依据，按「项 + 序号」对齐）的 `status`、`hit`、`captured`、`origin`、`expected`、命中次数。Python 只因静态检查（HOOK 偷取范围、`DD_ENTRY`）判失效、命中和解析结果一致的特征码单列，再用 `--sig-fail` 让两边按同样的输入比较依据。
- 破坏样本：`tests/layout_compare/make_mutants.py` 在 Steam 版上改几个字节（地址按清单现场解析），生成到 `build/u1b/mut/`，只读、不运行。它们只为覆盖依据求值的各条失败路径，不模拟真实的编译器输出（那是 U1d 的变异样本）。

命令：`python -B DD.ModLoader/tests/layout_compare/make_mutants.py`，然后 `python -B DD.ModLoader/tools/ddupdate/cpp_compare.py --repeat 5`（`compare_run.txt`、`compare/`）：

| exe | 模拟的情况 | 依据（两边一致） |
| --- | --- | --- |
| Steam 版 | | ok 114 |
| 离线版 | | ok 114 |
| U1a `Darkest_layout` | Hero 槽 5 的位移 0x137C → 0x1380 | fail 1（`HeroId` 读到 0x1380） |
| U1a `Darkest_layout_sig` | 同上 + `UpdateStateInternal` 的栈帧 | fail 1 |
| `C_vtable_sig` | `HeroVtable` 的特征码被破坏 | blocked 20 |
| `D_epsilon` | `DamageEpsilonBits` 引用的 float 改掉（DEREF_U32） | fail 1 |
| `E_text_dup` | `RosterAlt` 的 TEXT 模式多出一份 | fail 3（命中 2 次） |
| `F_vslot` | Hero 虚表槽 40 指向 `.rdata` | fail 2（起点不在 `.text`） |
| `G_anchor` | 锚点 `BuffInstanceCtor`、`MonsterBlockReturn` 被破坏 | blocked 8、fail 1（`VtSlotDamage[1]` 和 `MonsterBlockReturn` 共用被改的字节，命中 0 次） |
| `H_rel32` | `mov rdi,[Raid::Current]` 的位移加 8（REL32） | fail 1 |
| `I_pdata` | `BuffGroupErase` 的 RUNTIME_FUNCTION 清零 | fail 4（起点不在异常目录） |
| `J_entry_hooked` | `ApplyHit` 入口改成 `jmp` | 静态检查单列 `ApplyHit`，blocked 5 |

```
汇总：steam 一致，nosteam 一致，u1a/Darkest_layout 一致，u1a/Darkest_layout_sig 一致，u1b/C_vtable_sig 一致，u1b/D_epsilon 一致，
u1b/E_text_dup 一致，u1b/F_vslot 一致，u1b/G_anchor 一致，u1b/H_rel32 一致，u1b/I_pdata 一致，u1b/J_entry_hooked 一致
结果：全部一致，退出码 0
```

## 8. 自检

1. `rig.py status`：开工前是 scenario4，游戏没运行。每次启动前 `tasklist` 确认没有 `Darkest.exe`。`rig.py run` 默认带 `-skipfeflowintro`。
2. **已知版本**（最终二进制）：`rig.py deploy scenario4`，`rig.py run --seconds 150`，第 75 秒跑 `rig_textdiff.py`，退出后 `rig.py save-log u1b-selfcheck`。关键行见第 6 节，后面两行：

   ```
   布局核对：已知版本：只能离线核对的 21 项、没有静态依据的 118 项按 accept 时的离线核对和自检处理，不停用
   布局核对：全部通过，没有停用任何能力
   ```

   DDCore.log、DDLua.log 没有 WARN/ERROR，DDModLoader.log「MOD 扫描完成。成功: 7，失败: 0」。和 P4 的 `rig_logs/p4f3_scenario4_menu/DDCore.log` 归一化（去掉时间、地址、线程号、耗时）后比较，只多上面 3 行，少一行「退到桌面：后台写盘已写完」（那次是游戏里正常退出，这次是到时强杀）。
3. **textdiff**：20 处改动 = DDCore 5 处（`3DCE10` 每帧，`2487E0`、`248D60`、`2923A0`、`3D1CC0` 是 DDLua 声明持久化后 AppReady 装的 4 个观察 hook，ADR 0031）+ 加载器 1 处（`3E9320`）+ 14 处别的 DLL（`buff_icon.dll` 5 处、跳到私有页的 5 处、数据补丁 4 处）。除 4 个观察 hook 之外的 16 处，改动位置和原始字节和 P4a 的 `rig_logs/p4a_combat_selfcheck2/textdiff_combat.txt` 逐项相同（那次方案不带 DDLua，没有观察 hook；跳转目标的地址随构建和加载位置不同）。没有多余补丁。
4. **未知版本模拟**：`rig.py deploy topics`（DDCore + DDLua + topic_dump），`DD_LAYOUT_ASSUME_UNKNOWN=1 rig.py run --seconds 75`，`save-log u1b-unknown-topics`。DDCore 正常启动、两个插件加载；停用 combat 的 6 个写能力，按主题停用 15 个（`effect.pre`、`hit.pre`、`stress.pre`、`damage.final`、11 个 `query.*`），`topics.json` 的 151 个主题里这 15 个不可用、原因是「布局核对停用：……」，另有原来就不可用的 `native.AwardAchievementEvent`（没有 Listener 虚表），其余 135 个照常；DDLua.log 没有 WARN/ERROR。
5. `rig.py verify-real`：真实安装未被改动。
6. 中途有过两次用旧二进制跑的同名日志，改名为 `rig_logs/u1b-selfcheck-pre`、`u1b-unknown-topics-pre` 留底（之后只改了日志措辞和测试开关）。

## 9. 需要用户决定

1. **未知版本的默认值**（ADR 0036 现在选 A）。按「Steam 版当作未知版本」实测的影响：combat 的 6 个写能力和 15 个主题停用，直到我们跑 `repair`、`accept` 并发版；DDCore、事件桥、其余语义主题、game 服务、combat 读接口、persist 照常。
   - A（默认）：写能力在依赖的布局项不能运行时核对时停用，只读能力警告。
   - B：未知版本只停「不成立」「没法求值」的，`OFFLINE`/`NONE` 的项只警告。更新后写能力照常，但那 139 项任何一项变了都会按旧偏移写游戏内存。
   - C：未知版本 DDCore 整体停用。
   - 缩小 A 的影响的办法：继续给 `GameHashMul`、四张枚举表、Query 负载、`VtSlotStress`、`TargetHit` 等补运行时依据。
2. **`ListenerFunction`（必需）的依据锚在可选特征码 `MonsterBlockReturn` 上**：按派发的指令形式找，函数里只有一处派发的特征码函数只有 `CombatStartTurn`、`MonsterBlockReturn`、`InventoryBuffRemove`（都是可选），必需特征码的函数里没找到。所以 `MonsterBlockReturn` 失效时 DDCore 整体停用。替代办法是这时在 AppReady 退回运行时数据自洽检查（没做）。
3. **推出的依据**：`StdFunctionSize`（实现指针是最后一个成员）和 `StdFuncImplVtableSlots`（`_Delete_this`、`_Do_call` 离末尾的距离）是按 MSVC `std::function` 的结构推的，结构本身变了要人工确认。是否接受把它们标成 RUNTIME。
4. **`persist` 算只读**：它只观察存档读写、写的是我们自己的旁路文件，所以 `StorageDeviceNameCap`（没有静态依据）不能核对时只警告。
5. **行为变化**：`RaidCurrent` 缺失时，现在除了 `.pre` 主题，Query 写回和 combat 写操作也停（`RaidPvpName` 没法求值 → `pvp` 停用，依赖表对 `pvp` 的定义本来就是这样；原来的代码只停 `.pre` 主题）。
6. 测试开关 `DD_LAYOUT_ASSUME_UNKNOWN` 是否留在发布版里（只会更严）。

## 10. 改动文件

新增：
- `core/src/game/layout_check.h`、`layout_check.cpp`
- `tests/layout_compare/main.cpp`、`make_mutants.py`
- `tools/ddupdate/cpp_compare.py`
- `docs/adr/0036-runtime-layout-check.md`
- `docs/u1/u1b-report.md`

修改：
- `core/src/game/signatures.inc`：新锚点 `ListenerEntryVtable`（GLOBAL ANCHOR）
- `core/src/game/layout.inc`：`ListenerPriority`、`ListenerFunction`、`StdFunctionSize`、`StdFunctionImpl`、`StdFuncImplVtableSlots`、`EventTime` 从 NONE 改成 RUNTIME，加 8 条依据，删掉对应的 `DD_UNVERIFIED`；值和依赖表没改
- `core/src/game/game.h`、`game.cpp`：`Addresses::gate`；`ResolveReport` 加特征码结果、核对结果、扫描耗时；`Resolve` 改用共享解析器并求值全部依据，误绑定的特征码不给地址；删掉旧的特征码表和扫描函数
- `core/src/core.cpp`：`ApplyLayoutCheck`、`ApplyTopicGate`、各服务入口的 `Gated` 检查、`CombatReject` 带说明、测试开关
- `core/src/events/hub.h`、`hub.cpp`：`Topic::gated`、`Hub::Gate`
- `core/src/events/system.cpp`：`events.native`、`events.query`、`pvp` 的开关
- `loader/early_hook.h`：注释（加载器没有布局依赖）
- `tests/xmake.lua`：`dd_core_tests` 加 `layout_check.cpp`，新目标 `dd_layout_compare`
- `tests/unit/test_main.cpp`：「布局核对」一节（`layout_golden.cpp` 没动）
- `tools/baseline/steam.json`、`nosteam.json`：重新生成
- `docs/README.md`：ADR 表加 0036
- `knowledge/dd1/events.md`、`combat.md`：插入排序和派发的指令、`361610` 是叶函数、离线版地址、`RemoveMultipleBuffs` 析构按值传入的 `std::function`，都带 [静态]

没改 `tools/` 下原有的文件和清单格式的语义。

## 11. 没做到的事、失败的尝试、已知局限

- 运行时数据自洽检查没做（第 3 节的理由）。
- 第一次找 `ListenerFunction` 的依据时试了 listener 析构（`EventTypeCount` 最低命中所在的函数，FUNC 范围）：Steam 版里是 `361730`、有 `lea rdi,[rbx+0x10]` / `[rdi+0x38]`，离线版最低的命中落在另一个不含 `std::function` 析构的函数里（`3B71C0`），不能用，和 U1a 的结论一样。
- `make_mutants.py` 的 `E_text_dup` 原想把模式复制进 int3 填充，`.text` 里最长的填充只有 20 字节，改成盖在一个和清单无关的函数中间（脚本里会核对那个函数不含任何特征码和依据的命中）。
- 单元测试第一版把函数 B 的 unwind 信息放在了链式表项的字段上（自己造的 PE 的问题），改了位置后通过。
- 运行时不做 HOOK 的静态检查和 `DD_ENTRY` 比较；对照测试单列这类差异，不算不一致。
- 同一个 TEXT 模式被两条依据各扫一遍，没缓存（约 4 ms 可省）。
- 自检只到主菜单（按 A7）。已知版本上开关全空，城镇、地牢、战斗里的行为和改动前一样；未知版本的停用只在主菜单上看了主题表和日志，combat 写操作被拒绝的路径只有单元测试和代码审阅，没在游戏里触发过。

## 12. 留给后面的事项

U1c：
- 清单多了锚点 `ListenerEntryVtable`（GLOBAL ANCHOR，模式是 `ListenerAdd` 的加 4 个通配）和 8 条依据，其中 `StdFuncImplVtableSlots` 的两条是 `mul 8` 加负的 `add`（反推 `(读到的值 + 16) / 8`），`StdFunctionSize` 是 `add -8`。修复工具重新生成这几条时要保留这种写法。
- 基线已按当前的 `baseline.py`（FORMAT 2）重新生成。

U1d：
- 把对照测试并进 `selftest`：先 `xmake build dd_layout_compare`，再 `python -B DD.ModLoader/tools/ddupdate/cpp_compare.py`（退出码 0 才算通过；默认比较 Steam、离线、`build/u1a/mut`、`build/u1b/mut`，样本用 `tests/layout_compare/make_mutants.py` 生成）。U1d 自己的变异样本也可以直接传给它；「hook 入口变得不安全」那类样本会走「只在离线做的静态检查」那条路径。
- 文档：`06-workflow.md` 的工具表加 `cpp_compare.py`、`dd_layout_compare`、`make_mutants.py`；`02-architecture.md` 讲地址和校验的部分加运行时核对（ADR 0036）；ADR 0035 引用 0036；ADR 0006 的「后果」里旧脚本名。
- ADR 0036 的未知版本默认值等用户决定（第 9 节），定了以后把 README 表里的「草稿」去掉。
