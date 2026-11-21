# 运行时核对布局清单，不能核对的写能力停用

[ADR 0006](0006-signature-addressing.md) 只管地址：特征码命中、SHA1 未知时只警告。结构偏移变了（字段后移、虚表加槽）发现不了，DDCore 会照旧按旧偏移读写游戏内存。U1 把全部布局常量登记进 `core/src/game/layout.inc`（流水线见 ADR 0035），这里定运行时怎么用它：玩家的游戏被 Steam 自动更新、我们还没跑工具时，DDCore 自己发现布局变化，只停用依赖它的能力。

## 决策

- **什么时候核对**：`game::Resolve` 解析完特征码之后、装任何 hook 之前，求值 `layout.inc` 的全部运行时依据（`DD_EVIDENCE`，含特征码的同一性依据）。实现在 `core/src/game/layout_check.cpp`，语义和 `tools/ddupdate` 的 `sigs.resolve`、`evidence.evaluate` 相同：函数范围自己解析异常目录，不用 `RtlLookupFunctionEntry`；锚点特征码（`ANCHOR`）也解析，只给依据用。已知版本也照样核对。函数表在解析特征码之前建一次，HOOK 的入口检查和依据的 FUNC 范围共用。Steam 版自检实测（U1 修复 FA，`build/rig_logs/u1fa-selfcheck`）：62 条特征码扫描 63 ms（含函数表 3 ms），117 条依据求值 10 ms。
- **HOOK 的命中位置必须是函数入口**（U1 修复 FA，审核 A 的 S1）：命中位置落在异常目录记录的某个函数里、却不是那个函数的入口（是链式分块、间接表项或函数中间）时，这条特征码按失效处理，已知版本和未知版本都做（`check::CheckHookEntry`，和 `sigs.py` 的 `check_hook` 同一条）。编译器在原来的序言前面插了指令时，带通配的特征码可能命中挪下去的旧序言，hook 会装在函数中间。加载器对 `CreateAppSystems` 做同样的检查（`RtlLookupFunctionEntry`），偷取字节数不小于 5 在编译期核对。
- **两边一致**：`tests/layout_compare`（`dd_layout_compare`）把 exe 按节映射、不执行，跑同一份 C++；`tools/ddupdate/cpp_compare.py` 和 `dd_update.py verify --json` 逐项比较特征码的命中和解析结果、每条依据的 `status`、`hit`、`captured`。运行时不做、只在 Python 做的检查：HOOK 偷取范围的指令和 `hooks.md` 重叠（要反汇编，hook 引擎装 hook 时自己核对入口，[ADR 0007](0007-hooks-on-demand.md)）、`DD_ENTRY` 和入口字节的比较（入口可能已被旧式 DLL 打成 `E9`，hook 引擎串联）、依据位置核对（依据唯一命中、值也对，命中的却不是基线里那条指令，见 [ADR 0035](0035-update-pipeline.md)）。对照测试把它们单列为「Python 独有的检查」。
- **每一项的结论**：成立；不成立（命中次数不对、读到的值不对、起点不在函数里……）；没法求值（起点或 `rel` 的特征码失效，或者锚点特征码的同一性依据不成立）；只能离线核对（`OFFLINE`）；没有静态依据（`NONE`）。
- **「写」和「读」**：会写游戏内存、调用游戏函数或改变结算的能力是「写」。装内联 hook、改虚表槽、调用游戏函数都算写，包括只读主题背后的 hook（U1 修复 FA，审核 A 的 S1：原实现把 hook 主题、`persist`、`game.actor_info` 归为读，和这条定义冲突）。只读：只读游戏内存，偏移错了只会读出错的值或给不出结果。
- **停用规则**，都按依赖表（`layout.inc` 每项的 deps）：

  | 结论 | 已知版本 | 未知版本 |
  | --- | --- | --- |
  | 成立 | 照常 | 照常 |
  | 不成立 | 依赖表里的能力和主题全部停用（读写都停） | 同左 |
  | 没法求值 | 按「不能核对」 | 按「不能核对」 |
  | 只能离线核对、没有静态依据 | 照常（`accept` 拒绝离线依据不过的版本，NONE 项、同组升级的项要在确认清单里逐项核实，确认记录存在 `tools/baseline/confirm/`；规则见 ADR 0035「确认新版本」） | 按「不能核对」 |

  「不能核对」：写的能力停用，只读能力写一行警告后照常。主题停用「写」的那些：可写或可取消（`.pre`、`damage.final`、`query.*`），或者背后装内联 hook、改虚表槽（`Topic::hooked`：语义 hook 主题、`buff.*`、`app.*`、`campaign.save`）；只读、只靠往游戏的 listener 列表注册的主题（`native.*` 和映射来的语义主题）照常，注册本身归 `events.native` 管。依赖表里有 `core` 的必需项落到停用时，DDCore 整体停用，照 ADR 0006 写明原因、列出不会加载的插件。特征码的同一性依据不成立时，这条特征码按缺失处理（不给地址），它的 `DD_DEPS` 停用。

- **两个例外**：
  - `core` 不因「只能离线核对、没有静态依据」停用，只警告。DDCore 自己的每帧 hook（`UpdateStateInternal`）的原型（`ProtoUpdateStateInternal`）不能运行时核对，按「写」停用就等于未知版本上 DDCore 整体停用，也就是下面的方案 C。`core` 仍在必需项不成立、没法求值时停用（例如 `ListenerFunction` 的锚点 `MonsterBlockReturn` 失效）。
  - `loader` 由加载器自己核对（唯一命中、函数入口、`DD_ENTRY`、偷取字节数），加载器在 `DllMain` 里运行、不读布局清单，DDCore 不管依赖 `loader` 的项（只有 `ProtoCreateAppSystems`）。

- **词汇和开关**（写/读的划分写在 `layout_check.cpp` 的 `kDepInfo`）：

  | 词汇 | 写/读 | 停用开关 |
  | --- | --- | --- |
  | `core` | 写（例外见上） | `CoreBoot` 返回停用，插件不加载 |
  | `loader` | 写 | 加载器自己核对，DDCore 不管 |
  | `events.native` | 写：调用 `ListenerAdd` 往游戏的 listener 列表注册，游戏经伪造的 `std::function` 实现回调 | `native.*` 和映射来的语义主题停用，不往游戏的 listener 列表注册 |
  | `events.query` | 写：同上，回调还能写回 Query 的结果 | `query.*` 停用 |
  | `pvp` | 写 | `.pre` 主题和 `damage.final`、`query.*` 停用，combat 写操作和规则注册拒绝（[ADR 0011](0011-pvp-no-mutating-dispatch.md) 的前提） |
  | `game.actor` | 读 | 不给任何 actor 句柄（`ActorEnvNow` 的虚表置空），按句柄解析 actor 的能力随之失败 |
  | `game.actor_info` | 写：调用游戏函数 `ActorMaxStat` 取最大生命 | `actor_info` 返回失败 |
  | `game.actor_list` | 读 | `party_heroes`、`battle_monsters` 返回失败 |
  | `game.battle` | 读 | `battle_state` 返回 -1、`battle_serial` 返回 0、不再取样战斗、不给怪物句柄 |
  | `game.mods` | 读 | `mods_available` 为假，进存档不读启用列表 |
  | `combat.buff_read`、`effect_get`、`skill`、`enum` | 读 | 对应的服务函数返回失败（`enum_count` 0、`enum_name` 空、`enum_value` -1、`source_flags` 0） |
  | `combat.buff_add`、`buff_remove`、`buff_define`、`effect_apply`、`effect_define` | 写 | 服务函数返回 `DD_COMBAT_E_UNAVAILABLE`，说明写明是布局核对 |
  | `combat.rule` | 写：装规则 hook，判定结果交还游戏 | `rule_register` 拒绝，规则 hook 不会装 |
  | `persist` | 写：在存档读写等 4 个函数上装内联 hook，按固定的参数个数转发给原函数 | 不创建 persist 服务（`campaign.save` 不注册，DDLua 的 `dd.store` 只在内存里） |
  | `topic:<主题>` | 主题可写、可取消或背后装 hook、改虚表槽时是写 | 主题在 hub 里标成停用（`Hub::Gate`，之后不能再置为可用），订阅返回失败 |

  `game.actor` 算读：句柄每次解析都核对虚表，偏移错了只会给不出句柄，不会读写错对象；写操作自己的依赖（`combat.buff_add` 等）另外登记在依赖表里。`combat.enum` 算读：枚举表用在写操作参数校验的那些项，依赖表里同时有 `combat.buff_define` 等词。`combat.buff_read` 只读 Buff 库的 map 和 actor 的 buff 表，不调用会插入的 `BuffLibraryIndex`。

- **原型和宽度也登记进清单**（U1 修复 FA，审核 A 的 S3）：被 hook 的函数和被调用的游戏函数各有一个原型项 `Proto<函数>`（COUNT，值是 C++ 声明并转发或传入的参数个数，代码里用 `static_assert(layout::kArgs<函数指针类型> == layout::kProto...)` 绑定），按固定宽度读写的字段有宽度项（`InstScalarSize`、`BuffGuidSize`、`ActorFloatSize` 等，SIZE），伪造 Listener 虚表的假设（`ForgedListenerVtableSlots`）、MSVC RTTI 结构的大小（`Rtti*Size`）、偏移 0 的字段（`BuffEntryGuid`、`StdVectorFirst`）也各占一项。它们都没有静态依据（`NONE`）：特征码命中、依据成立都不代表参数个数没变。登记的作用是让未知版本的策略和「更新后要人工核实」的清单看得到它们。
- **特征码缺失**照 ADR 0006（地址为空，用它的功能不可用）。它当锚点时，以它为锚点的依据「没法求值」，走上面的规则。例如 `RaidCurrent` 缺失时 `RaidPvpName` 没法求值，`pvp`（写）停用，于是 `.pre` 主题、Query 写回、combat 写操作都停；原来只停 `.pre` 主题。
- **补的依据**：按上面的规则，`ListenerPriority`、`ListenerFunction`（必需）、`StdFunctionImpl` 不能核对时，DDCore 会在每次游戏更新后整体停用。U1b 给它们和同组的 `StdFunctionSize`、`StdFuncImplVtableSlots`、`EventTime` 补了运行时依据（`layout.inc` 里有指令和推理），新增锚点 `ListenerEntryVtable`。U1 修复 FA 给 `VtSlotStress` 补了依据（槽 36 的函数调用压力结算 `480FE0`，新锚点 `ActorStressInternal`，Hero、Monster 各一条 REL32），给 `VtSlotHeal` 补了一条能区分槽 50、51 互换的依据（治疗函数里 `hp += min(治疗量, 最大生命 - hp)` 的指令串，伤害函数里没有）。

## 局限

运行时核对做不到的，写在这里，不当作已经防住：

- **依据误绑定到邻居字段**：依据唯一命中、读到的值也等于旧常量，命中的却是邻居字段的指令（审核 A 的 B1 样本 `inst44.exe`、U1d 的 `evidence_misbind` 样本里 `EffectDotBurn` 的情形）。离线的依据位置核对靠基线发现它，运行时没有基线。能做的缓解是把依据模式写得更具体：U1 修复 FA 把靠一条指令的字面量区分字段的依据收紧了：Instance 复制构造里的字段带上同一串复制里它后面的全部复制和界标（形状不同的下一个字段），`EffectDotBurn`/`EffectDotStress` 带前后两个字段，`EffectApplyResultGuids` 带整段清零。这一串里插入、删除字段时依据不成立而不是误绑定。代价是紧挨着这一串插入别的形状的字段时，值没变的字段的依据也不成立，要人工核实、改模式（修复工具只改值，不改模式）。没收紧的依据（按位置读一串同形状指令、或者模式本身只有一条指令）仍可能误绑定。
- **诱饵函数**：真函数改得不再匹配，别处恰好有一份能唯一命中的相似代码（U1d 的 `decoy` 样本，`BuffRemoveByGuid`）。离线靠基线的指纹和调用关系发现，运行时只有特征码的同一性依据（目前只有 `HeroVtable`、`MonsterVtable` 有），其余特征码误绑定时运行时发现不了。
- **原型**：参数个数、调用约定变了，特征码照样命中、依据照样成立。原型项让未知版本上依赖它的能力停用，已知版本靠 accept 时人工核实。
- **DDCore 和加载器自己的 hook**：见上面的两个例外。未知版本上 `UpdateStateInternal`、`CreateAppSystems` 的原型变了，DDCore 和加载器照样装 hook。
- **可选特征码当必需项的锚点**：`ListenerFunction` 锚在 `MonsterBlockReturn` 上，`ListenerEntryListener`、`ListenerEntryActive` 的第 3 条依据锚在 `QueryListenerAdd` 上，这两条可选特征码失效时 DDCore 整体停用（U1d 的 `relocate_sched` 样本在已知版本开关下就是这样）。

## 考虑过的方案（由用户决定）

U1 修复 FA 之后的 A 在未知版本上停用的能力明显变多（下面的「后果」），A、B、C 仍由用户定：

- **A（现在的实现）**：写的能力（含装 hook、改虚表槽、调用游戏函数）在依赖的布局项不能运行时核对时停用，只读能力警告；`core`、`loader` 是例外。
- **A 的缩小办法**：继续给不能核对的项补运行时依据。原型可以考虑「入口序言里的参数接收指令」这类依据（看得到用了哪些参数寄存器，看不出有没有多的参数，属于推出的依据）；`ListenerAdd`、`QueryListenerAdd`、`ProtoStdFuncDoCall`、RTTI 几项补上之后，未知版本上事件桥可以恢复；`StorageDeviceNameCap` 和 4 个存档 hook 的原型补上之后 persist 可以恢复。
- **A 的旧实现（只读主题背后的 hook 和 persist 算读）**：未知版本上保留只读主题和 persist（U1b 的同一模拟里 151 个主题有 135 个照常），但和「写」的定义冲突：hook 按旧原型转发参数、虚表槽按旧槽号改写，原型或槽号变了时直接影响结算和存档写入（审核 A 的 S1、S2）。
- **B**：未知版本只停「不成立」「没法求值」的，`OFFLINE`/`NONE` 的项只警告。更新后写能力照常，但那 193 项任何一项变了都会按旧偏移写游戏内存或按旧原型调用游戏函数。
- **C**：未知版本 DDCore 整体停用。最安全，但每次游戏更新都要等我们发版，只读的 actor 能力也跟着没了。和 A 的差别已经不大（A 在未知版本上只剩生命周期主题和几项只读服务）。
- 运行时数据自洽检查（读游戏已有的 listener 表项、RTTI，看函数指针、优先级、表项大小是否合理）：只能在 AppReady 之后做，那时插件已经加载，必需项失败时没法按 ADR 0006 整体停用；没做。
- 运行时自动采用读到的新值（`implied`）：不做（简报的「不做」），新值要经过修复工具和人工确认。

## 后果

- 已知版本：Steam 版自检全部成立，没有停用任何能力（`build/rig_logs/u1fa-selfcheck`）。
- 未知版本（在 Steam 版上用测试开关模拟，`build/rig_logs/u1fa-unknown`）：
  - 停用 10 个能力：`events.native`、`events.query`、`game.actor_info`、`combat.buff_add`、`buff_remove`、`buff_define`、`effect_apply`、`effect_define`、`rule`、`persist`。原因主要是原型项（`ProtoListenerAdd`、`ProtoActorMaxStat`、`ProtoStorage*` 等）、RTTI 和伪造虚表的假设、宽度项，以及原来就不能核对的枚举表、`GameHashMul`。
  - 主题表 150 个主题里 146 个不可用：语义 hook 主题 17 个和 `buff.*` 3 个（原型项），原生事件桥 115 个（`native.*` 和映射来的语义主题），Query 桥 11 个；`campaign.save` 不注册。
  - 照常：DDCore 本身（每帧 hook、插件加载、生命周期阶段）、4 个应用主题（`app.tick`、`app.state`、`campaign.load`、`campaign.unload`），只读服务 `game.actor`、`game.actor_list`、`game.battle`、`game.mods`、`combat.buff_read`、`effect_get`、`skill`、`enum`（各写一行警告）。
  - U1b 时的同一模拟只停 6 个写能力和 15 个主题。玩家更新游戏后，这些能力要等我们跑 `dd_update.py repair`、`accept` 并发版才恢复。
- `StdFunctionSize`、`StdFuncImplVtableSlots` 的依据是按 MSVC `std::function` 的结构推出来的（实现指针是最后一个成员、`_Delete_this` 是倒数第二个虚函数）；这个结构本身变了要人工确认。
- 测试开关：环境变量 `DD_LAYOUT_ASSUME_UNKNOWN=1` 让 DDCore 在已知版本上按未知版本处理（只会更严），用来验证停用路径；`rig.py run` 继承环境变量。
- 已知版本上的「没法求值」（例如别的 DLL 改了锚点所在的代码）同样按「不能核对」处理：这时内存里的代码已经不是 accept 时核对过的那份。

2026-11-16 起草（U1b），2026-11-19 按审核 A 修正「写」的实现（U1 修复 FA）。未知版本的默认值待用户确认。
