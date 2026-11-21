# U1 修复 FA 报告：清单和 C++

阶段工作文件，用户验收后删除。修复简报见 [fix-brief.md](fix-brief.md)「FA」一节，审核报告见 [review-a.md](review-a.md)。完整输出在 guest 的 `D:\workplace\DD.ModLoader\build\u1fa\`（下文的文件名都相对这个目录，`rig_logs` 指 `build/rig_logs/`）。审核的每条发现都先在 guest 上复核再改；没 commit，没碰真实安装。

## 1. 结论

| 审核编号 | 处理 | 证据 |
| --- | --- | --- |
| S1 读写归类 | 按 ADR 0036 的定义修：装内联 hook、改虚表槽、调用游戏函数算写。`game.actor_info`、`persist` 改为写；主题加 `hooked` 标志，「不能核对」时停用可写、可取消或背后装 hook/改虚表槽的主题；原型项让 hook 主题、事件桥、persist 在未知版本上停用。`core`、`loader` 是例外（第 3 节） | `layout_check.cpp` 的 `kDepInfo`、`BuildGate`；`TestPolicy`；`rig_logs/u1fa-unknown` |
| S1 HOOK 入口检查 | 运行时加上，已知、未知版本都做（`check::CheckHookEntry`，和 `sigs.py` 同义）；加载器也做（`RtlLookupFunctionEntry`） | `TestImageAndScan`；`compare/u1d_hook_entry.cpp.json` 里 `ConsumeHealthDamageBlock` 是 `fail`、「命中位置不是函数入口（在 C71EF0 开始的函数里，+0x9）」 |
| S2 `VtSlotStress` | 补运行时依据：新锚点 `ActorStressInternal`（480FE0），Hero、Monster 槽 36 各一条 REL32。NONE → RUNTIME | verify 两个版本 `VtSlotStress = 0x24 运行时 2/2`；`TestPolicy` 里 stress.post 的断言改了 |
| S2 `VtSlotHeal` | 补一条能区分槽 50、51 互换的依据（治疗函数里 `hp += min(治疗量, 最大生命 - hp)` 的指令串，伤害函数里没有） | `u1d/vslot` 样本上 `VtSlotHeal[1]` 两边都是「命中 0 次」，已知版本开关停 `heal.*` |
| S3 覆盖缺口 | 登记 36 个原型项、12 个宽度/类型项、`BuffEntryGuid`、`StdVectorFirst`、伪造 Listener 虚表只用槽 0、RTTI 结构大小和修饰名偏移，全部 `DD_UNVERIFIED` 加依赖表；代码里用 `static_assert` 绑定参数个数和宽度 | 清单 227 → 282 项；第 2 节 |
| B1 的运行时缓解（FA 第 5 项） | 盘点「靠一条指令的字面量区分字段」的依据，收紧 14 条（第 4 节）。B1 的两个样本 `inst44.exe`、`inst_remove44.exe` 上原来判 OK 的 InstSubHash、InstAmount 等现在依据不成立（verify 退出码 1，原来 0）；`evidence_misbind` 样本上 `EffectDotBurn` 原来依据成立、误绑定到邻居（只有离线的依据位置核对能发现，运行时判成立），现在两边都判不成立 | `review_inst44.verify.txt`、`review_inst_remove44.verify.txt`、`compare_run.txt` |
| 备注 1 | `signatures.inc` 末尾的过时注释改成 ADR 0036 的说法 | `signatures.inc` |
| 备注 2 | 复核结论：**两边本来就一致**。`sigs.py` 写的是 `sig.need == "REQUIRED"`，而 `manifest.Sig.need` 把 LOADER 也归为 REQUIRED（guest 上实测 `CreateAppSystems` 的 `need` 是 `REQUIRED`）。C++ 加了注释说明同义，没改行为；Python 侧的建议见第 7 节 | `layout_check.cpp` 的 `ResolveSignatures` |
| 备注 4（C++ 侧） | 加载器 `static_assert(kSteal_CreateAppSystems >= 5)`；DDCore 的 hook 引擎本来就拒绝小于 5 的 | `loader/early_hook.h` |
| ADR 0036 | 重写：新归类、HOOK 入口检查、两个例外、原型和宽度项、局限、未知版本的代价、给用户的选项 | `docs/adr/0036-runtime-layout-check.md` |

收尾证据（第 6 节）：`xmake build -r` 6 个目标没有警告；`dd_core_tests` 1272 项失败 0，`dd_lua_tests` 1291 项失败 0；verify 两个版本退出码 0（依据位置 100 对上、0 对不上、0 没法对照）；基线重新生成，两次字节一致；`cpp_compare` 22 个 exe 全部一致；测试目录已知版本自检全部通过、不停用；未知版本模拟见第 5 节；`rig_textdiff` 没有我们多出的补丁；测试目录已部署回 scenario4。

## 2. 清单的变化（给 FB）

`layout.inc` 227 → 282 项（RUNTIME 88 → 89、NONE 118 → 172、OFFLINE 21），依据 114 → 117 条；`signatures.inc` 61 → 62 条（新锚点）。格式的语义没改，`evidence.py` 的求值语义没改；文件开头加了一段「约定」（原型项、宽度项怎么用现有格式登记），不是新格式。

### 新特征码

| id | 种类 | 说明 |
| --- | --- | --- |
| `ActorStressInternal` | CODE ANCHOR | 480FE0（离线 46F1B0），压力结算，往 +0x12FC 加增量；`VtSlotStress` 的 REL32 目标；deps `check` |

### 改的项和依据

| 项 | 变化 |
| --- | --- |
| `VtSlotStress` | NONE → RUNTIME，删 `DD_UNVERIFIED`，加 2 条依据（Hero 槽 36 `0F 28 D0 48 8B D6 48 8B CB E8 ?? ?? ?? ??…` at 10，Monster 槽 36 `48 8B CB E8 ?? ?? ?? ?? 44 0F 28 C0 41 0F 28 C0` at 4，都是 REL32 rel `ActorStressInternal`，mul 0） |
| `VtSlotHeal` | 加依据 1：Hero 槽 51 `E8 ?? ?? ?? ?? F3 0F 5C C2 41 0F 28 F8 F3 0F 5D F8 F3 0F 58 D7 F3 0F 11 93 ?? ?? ?? ??` at 25，U32，mul 0，rel `ActorHp` |
| 14 条收紧的依据 | 见第 4 节，模式换了、序号不变（都是各项的依据 0） |

### 新项（全部 OPT、NONE，带 `DD_UNVERIFIED`）

- 原型项（COUNT，值是 C++ 声明并转发或传入的参数个数）36 个：
  - hook：`ProtoActorDamage` 8、`ProtoActorHeal` 8、`ProtoActorStress` 5、`ProtoConsumeHealthDamageBlock` 3、`ProtoEffectApplyInternal` 13、`ProtoApplyHit` 3、`ProtoApplyMiss` 3、`ProtoActorDoAttackSkill` 4、`ProtoActorDoSkillFriendly` 5、`ProtoBattleAdvanceRound` 1、`ProtoCombatStartTurn` 3、`ProtoBuffGroupAdd` 3、`ProtoBuffGroupErase` 3、`ProtoBuffDurationUpkeep` 3、`ProtoBuffRuleActive` 5、`ProtoStorageQueueWrite` 9、`ProtoStorageReadFile` 6、`ProtoSaveSlotBackup` 4、`ProtoAppShutdown` 4、`ProtoUpdateStateInternal` 1（deps `core`）、`ProtoCreateAppSystems` 1（deps `loader`）
  - 调用的游戏函数：`ProtoActorMaxStat` 3、`ProtoActorPredicate` 1（槽 40/43）、`ProtoListenerAdd` 2、`ProtoQueryListenerAdd` 2、`ProtoBuffInstanceCopy` 2、`ProtoVectorDtor64` 1、`ProtoEhVectorDtor` 4、`ProtoAddBuff` 4（槽 13）、`ProtoRemoveBuffs` 2（槽 14）、`ProtoBuffLibraryIndex` 2、`ProtoApplyEffect` 12、`ProtoPrngFloat` 1、`ProtoFreeU32Vector` 1、`ProtoParseEffectFile` 2
  - 游戏回调我们：`ProtoStdFuncDoCall` 2（伪造的 `std::function` 实现，deps `events.native events.query combat.buff_remove`）
- 宽度/类型项（SIZE 或 COUNT）：`ActorFloatSize` 4、`ActorIdSize` 4、`InstScalarSize` 4、`InstFlagSize` 1、`BuffGuidSize` 4、`EffectScalarSize` 4、`EffectFlagSize` 1、`SkillScalarSize` 4、`HitDataScalarSize` 4、`BattleIntSize` 4、`NativePayloadFields` 47（Ev* 字段个数）、`QueryPayloadFields` 22（Qy* 字段个数）
- 偏移 0 的字段：`BuffEntryGuid`、`StdVectorFirst`
- 伪造 Listener 虚表和 RTTI：`ForgedListenerVtableSlots` 1、`RttiTypeDescriptorName` 0x10、`RttiBaseClassDescriptorSize` 0x1C、`RttiClassHierarchySize` 0x10、`RttiObjectLocatorSize` 0x18（deps `events.native events.query`）

### 组的线索（S4，FB 加组的宏时参考）

同组有 RUNTIME 项的值变了时，组里的 NONE、OFFLINE 项要升级成「需要人工」：

| 组 | RUNTIME 项 | 组里的 NONE / OFFLINE 项 |
| --- | --- | --- |
| Hero/Monster 虚表槽 | `VtSlotStress`、`VtSlotDamage`、`VtSlotHeal`、`VtSlotDeathsDoor`、`VtSlotDead`、`SlotAddBuff`、`SlotRemoveBuffs` | `ProtoActorDamage`、`ProtoActorHeal`、`ProtoActorStress`、`ProtoActorPredicate`、`ProtoAddBuff`、`ProtoRemoveBuffs` |
| Actor 结构 | `ActorHp`、`ActorStress`、`ActorMaxHpStat`、`ActorMaxHpMask`、`ActorBuffGroup`、`ActorNextGuid`、`HeroId`、`MonsterGuid`、`HeroClass`、`MonsterClass` | `ActorFloatSize`、`ActorIdSize`、`EntryHero`（名册项里的 Hero） |
| Buff::Instance | `InstSubType` … `InstClearOnRule`、`BuffInstanceSize`、`InstAdditionalCount` | `InstStat`、`InstScalarSize`、`InstFlagSize`、`GameTextCap` |
| GroupEntry | `BuffEntryInstance`、`BuffEntrySize` | `BuffEntryGuid`、`BuffGuidSize` |
| Effect | `EffectDotBurn`、`EffectDotStress`、`EffectOnHit` 等、`EffectBuffIds`、`EffectSwap` | `EffectName`、`EffectNameCap`、`EffectReadSize`、`EffectScalarSize`、`EffectFlagSize`；OFFLINE 的 `EffectTarget`、`EffectChance` 等 |
| EffectApplyResult | `EffectApplyResultGuids` | `EffectApplyResultSize` |
| 技能 | — | `HeroClassSkills` … `SkillFriendly`、`SkillScalarSize`；OFFLINE `SkillEffects` |
| Raid/Battle | `RaidBattle`、`BattleMonsters`、`BattleGuidCounter`、`BattleRound`、`RaidHeroes`、`RaidPvpName` | `BattleStateNone`、`BattleIntSize`；OFFLINE `BattleState` |
| 命中结算的参数 | `AttackPerformer`、`AttackSkill`、`TargetActor`、`TargetDamage`、`TargetCrit` | `TargetHit`、`HitDataScalarSize`、`ProtoApplyHit`、`ProtoApplyMiss` |
| listener 和 RTTI | `ListDataSize`、`ListDataDirty`、`ListenerEntry*`、`QueryListenerEntrySize`、`ListenerPriority`、`ListenerFunction`、`EventTime` | `ProtoListenerAdd`、`ProtoQueryListenerAdd`、`ForgedListenerVtableSlots`、`Rtti*` |
| std::function | `StdFunctionSize`、`StdFunctionImpl`、`StdFuncImplVtableSlots` | `ProtoStdFuncDoCall` |
| std::vector | `StdVectorSize` | `StdVectorFirst`、`StdVectorLast` |
| MonsterBlockReturn | `MonsterClassCanBeDamagedDirectly`、`MonsterBlockImmuneLeaDisp`、`MonsterBlockImmuneLeaNext` | OFFLINE `MonsterImmuneTypeCount` |
| 存档 | `StorageProfileSlot`、`StorageDeviceName` | `StorageDeviceNameCap`、`ProtoStorage*` 4 个、`ProtoSaveSlotBackup`、`ProtoAppShutdown` |
| 每个原型项和它的特征码 | 特征码被 repair 重新定位或重新生成（`ProtoApplyHit` ↔ `ApplyHit`，`ProtoActorDamage` ↔ 槽 50 …） | 对应的 `Proto*` 项 |
| 每个原生事件、每个 Query | — | `Ev<事件>_*`、`NativePayloadFields`；`Qy<Query>_*`、`QueryPayloadFields` |

最后两行的「组」不是 RUNTIME 项触发的，是特征码触发的：特征码一被修复工具挪过，它的原型就该人工看一遍。FB 决定要不要做。

## 3. S1 的实现

### 归类

`kDepInfo`（`layout_check.cpp`）：`game.actor_info` → 写（调用 `ActorMaxStat`），`persist` → 写（4 个内联 hook，按固定参数个数转发）。其余词不变；`combat.buff_read` 复核过只读 map 和 buff 表，不调用会插入的 `BuffLibraryIndex`（`buff_define` 才调）。

主题：`events::Topic` 加 `hooked`（`hub.h`），`System::AddSemantic` 建的 17 个语义 hook 主题、4 个 `app.*`、combat 的 `buff.*`、persist 的 `campaign.save` 置位。`ApplyTopicGate` 把 `writable || cancellable || hooked` 传给 `Gate::TopicOff`（参数名从 `writable` 改成 `writes`）。`native.*` 和映射来的语义主题只靠往 listener 列表注册（归 `events.native`），不置位，它们的负载项（`Ev*`，只读）不能核对时照常；`events.native` 本身被原型项停掉时整条停用。

### 两个例外（`BuildGate`）

- `core`：项的状态是「只能离线核对、没有静态依据」时不停 `core`，记一行警告。唯一触发它的是 `ProtoUpdateStateInternal`：DDCore 的每帧 hook 原型不能运行时核对，按「写」停用就等于未知版本 DDCore 整体停用（方案 C），和简报「不是改成 B 或 C」冲突。「没法求值」和「不成立」照旧停 `core`（`ListenerFunction` 等的既有行为不变，`TestPolicy` 有断言）。
- `loader`：DDCore 管不了加载器，依赖 `loader` 的项（`ProtoCreateAppSystems`）只记一行；加载器自己核对唯一命中、函数入口（新）、`DD_ENTRY`、偷取字节数（新，编译期）。

这两点写进了 ADR 0036 的「决策」和「局限」，是给用户的决定点。

### HOOK 入口检查

- `FunctionTable` 加 `Enclosing`、`IsFunctionStart`（和 `image.py` 的 `enclosing_function`、`is_function_start` 逐行对应，同起点只留最后一个表项、主函数按链和间接表项找）；`CheckHookEntry` 的条件和 `sigs.py` 一样：在某个表项里、又不是主函数入口才算问题，不在任何表项里不拦。
- `ResolveSignatures` 对 HOOK 在唯一命中后检查，失败时照样给出 `hit`、`resolved`（和 Python 一样只改状态），`problem` 写「命中位置不是函数入口（在 X 开始的函数里，+0x..）」。
- 函数表只建一次：`game::Resolve`、`dd_layout_compare` 先建表，再传给 `ResolveSignatures` 和 `RunCheck`（`Evaluator` 加了可选的共享表参数）。DDCore 日志的耗时行改成「扫描 N ms（含函数表 N ms）」。
- 加载器 `early_hook.h` 的 `IsFunctionEntry`：`RtlLookupFunctionEntry` 找到表项时要求 `BeginAddress` 就是命中位置、不是间接表项、unwind 信息没有 `UNW_FLAG_CHAININFO`；不成立就不装早期 hook（日志写明）。

### 代码里的绑定

`layout.h` 加 `layout::kArgs<函数指针类型>`。`system.cpp`、`combat.cpp`、`actors.cpp`、`persist.cpp`、`core.cpp` 里每个 detour、游戏函数的函数指针类型都有 `static_assert(kArgs<...> == kProto...)`；宽度项有 `static_assert(kInstScalarSize == sizeof(std::int32_t))` 一类。`ForgeListenerVtable` 的 `RttiAlloc` 改用 `kRtti*Size`，虚表大小按 `kForgedListenerVtableSlots` 算，类型描述符的修饰名偏移改用 `kRttiTypeDescriptorName`；`VecFirst` 用 `kStdVectorFirst`，combat 读组 guid 的 6 处加 `+ kBuffEntryGuid`。值都没变（0、0x10、0x18、0x1C、0x10），行为不变。加载器没有绑定 `ProtoCreateAppSystems`（加载器不包含 `layout.h`，保持精简）。

### 单元测试

- `TestImageAndScan`：`Enclosing`、`IsFunctionStart`（主函数、链式分块、间接表项、函数中间、表外），`CheckHookEntry` 的 5 种情况。
- `TestManifestTables`：新锚点是 ANCHOR，`VtSlotStress` 是 RUNTIME，原型项都是 COUNT/NONE、至少 30 个。
- `TestPolicy`：未知版本停 10 个能力（新增 `game.actor_info`、`persist`、`events.native`、`events.query`），`core`、`loader` 和只读的 9 个不停，停用原因里写着 `ProtoActorMaxStat`、`ProtoStorageQueueWrite`、`ProtoListenerAdd`；审核 S2 指出的断言改成 `stress.post` 停用（`writes` 为真），`hit.post`、`damage.pre`、`buff.tick`、`battle.round.start` 停，`native.HeroDeathEvent`、`actor.death`、`app.tick` 不停；日志里有 `ProtoUpdateStateInternal`、`ProtoCreateAppSystems` 的例外说明。`ActorStress` 没法求值的用例改成 `game.actor_info` 停用（现在是写），只读的换成 `RaidHeroes`（`game.actor_list` 不停）；新加 `VtSlotStress` 不成立时 `stress.*` 整条停用。

## 4. 收紧的依据（FA 第 5 项）

盘点办法：逐条看 117 条依据，找「要读的值和区分字段用的字面量在同一条指令或相邻一条指令上、周围有同形状的邻居」的。

| 项 | 原来 | 现在 |
| --- | --- | --- |
| `InstSubType`、`InstSubHash`、`InstAmount`、`InstSource`、`InstSourceData`、`InstId`、`InstIdHash`、`InstTarget`、`InstRemoveInactive`、`InstRemoveBattle`、`InstRuleString`（复制构造 482480） | 一条复制，靠目的偏移字面量（等于自己的旧值）区分 | 要读的字段（偏移 `??`）+ 同一串同形状复制里它后面的全部复制（字面量）+ 界标（形状不同的下一个字段，字面量）。界标 +0x44、+0x60、+0xA4、+0xAC、+0xB0、+0xF4 |
| `EffectDotBurn`、`EffectDotStress`（4F02D0） | 带后一个字段的字面量 | 前后两个字段都带字面量（它们在结构里排在后面，结构在前面变了就对不上） |
| `EffectApplyResultGuids`（4CB4D0） | 带后一条清零的字面量 0x10 | 整段清零：`+0` dword 到 vector 的 end |

为什么是「后面 + 界标」：同一串复制的偏移是等差的，中间插入或删除一个字段后剩下的偏移还是那串数（B1 的根因），只有串尾之后的下一个字段的位置会变。所以只带前面的上下文挡不住（实测推演：在 0x44 插入 int 后，「0x34 movups + ?? 」照样命中新字段），带上「它后面的复制 + 界标」才挡得住插入和删除。第一版带了整段（stat 到 id、false_rule 到 rule_string），selftest 的 `struct_shift`（0xB0 插入 8 字节）让 0x61–0xAD 那些值没变的字段也不成立；改成现在的写法后只剩紧挨着 0xB0 的 `InstRemoveInactive`、`InstRemoveBattle` 两项（界标是 0xB0 的 dword）。

效果（两边一致）：
- 审核 B1 的 `inst44.exe`、`inst_remove44.exe`：0x44 之后被挪动的 Instance 字段全部「依据不成立」，verify 退出码 1（原来 0，判 OK）；没被挪动的 `InstSubType` 照常成立。
- U1d 的 `evidence_misbind`：`EffectDotBurn`、`EffectDotStress` 命中 0 次（原来唯一命中邻居的解析）。

没收紧、仍有同类风险的（依据是一条指令，靠位移区分，结构插入时位移跟着变通常判不成立，但同函数里恰好有同形状指令访问新字段的旧偏移时会误绑定）：`InstFalseRule`、`InstRuleHash`、`InstRule`（BuffRuleActive 里），`ActorBuffGroup`、`ActorNextGuid`（槽 13），`EffectBuffIds`，`AttackSkill`、`TargetDamage`。这些没有找到更强的语义依据，ADR 0036 的「局限」写了。

## 5. 未知版本上停用的能力（给 FB 写 08 用）

`DD_LAYOUT_ASSUME_UNKNOWN=1`，方案 topics（DDCore + DDLua + topic_dump），`rig_logs/u1fa-unknown`：

- **停用的能力 10 个**：`events.native`（`ProtoListenerAdd`、`ProtoStdFuncDoCall`、`Rtti*`、`ForgedListenerVtableSlots`）、`events.query`（同上加 `ProtoQueryListenerAdd`）、`game.actor_info`（`ClassId`、`ClassIdCap`、`ProtoActorMaxStat`、`ActorFloatSize`）、`combat.buff_add`、`buff_remove`、`buff_define`、`effect_apply`、`effect_define`、`rule`、`persist`（`StorageDeviceNameCap`、4 个 `ProtoStorage*`）。
- **主题**：150 个里 146 个不可用：语义 hook 主题 17 个（原型项）、`buff.*` 3 个、原生事件桥 115 个、Query 桥 11 个；`campaign.save` 不注册（persist 没创建）。
- **照常**：DDCore 本身（每帧 hook、插件加载、生命周期阶段；`ProtoUpdateStateInternal` 只警告）、`app.tick`、`app.state`、`campaign.load`、`campaign.unload`，只读服务 `game.actor`、`game.actor_list`、`game.battle`、`game.mods`、`combat.buff_read`、`effect_get`、`skill`、`enum`（各一行警告）。
- 新出现的插件侧警告：DDLua「DDCore 没有 persist 服务，dd.store 只在内存里」。
- 对照：U1b 的同一模拟只停 6 个写能力和 15 个主题（151 个里 135 个照常）。

## 6. 收尾证据

- 构建（`build.txt`、`build_*.log`）：`xmake build -r` DDCore、DDModLoader、DDLua、dd_core_tests、dd_lua_tests、dd_layout_compare 全部 build ok，日志里没有警告（xmake 默认输出警告，`-w` 只多一行「选项已废弃」）。
- 单元测试：`dd_core_tests` 1272 项失败 0（`core_tests.txt`），`dd_lua_tests` 1291 项失败 0（`lua_tests.txt`）。
- 基线：`python -B DD.ModLoader/tools/dd_update.py baseline`，再 `--out build/u1fa/baseline_run2`，两次 sha256 相同、`cmp` 一致（`steam.json` `410329ae…`，`nosteam.json` `ffe23155…`）。
- verify（`verify.txt`、`verify.json`）：清单 62 条特征码、282 项、117 条依据；Steam 版、离线版退出码 0，依据位置 100 对上、0 对不上、0 没法对照。
- `cpp_compare`（`compare_run.txt`，22 个 exe）：全部一致，退出码 0。HOOK 入口检查带来的变化见第 7 节。
- 已知版本自检（`rig_logs/u1fa-selfcheck`，方案 scenario4，`rig.py run --seconds 150`，默认带 `-skipfeflowintro`）：

  ```
  布局核对：特征码 62 条扫描 62.3 ms（含函数表 2.8 ms）；运行时依据 117 条（成立 117、不成立 0、没法求值 0），求值 10.1 ms；清单项 282 个（运行时核对通过 89、不成立 0、没法求值 0、只能离线核对 21、没有静态依据 172），已知版本
  布局核对：已知版本：只能离线核对的 21 项、没有静态依据的 172 项按 accept 时的离线核对和自检处理，不停用
  布局核对：全部通过，没有停用任何能力
  ```
  DDCore.log、DDLua.log 没有 WARN/ERROR；和 `u1b-selfcheck/DDCore.log` 归一化后比较，只有上面两行的数字和措辞不同（存档哈希、字节数随存档变化，归一化掉了）。
- `rig_textdiff`（`rig_logs/u1fa-selfcheck/textdiff_scenario4.txt`）：DDCore 5 处（`3DCE10` 每帧、`2487E0`、`248D60`、`2923A0`、`3D1CC0` 4 个 persist 观察 hook）、加载器 1 处（`3E9320`），和 U1b 相同；其余 15 处是别的 DLL：`buff_icon.dll` 5、跳到私有页 5、数据补丁 4，和 U1b 相同，另多 1 处 `2585E0 → SpineFix.dll`（见第 8 节）。没有我们多出的补丁。
- 未知版本模拟：第 5 节。
- 测试目录最后部署的方案：scenario4（2026-11-19 19:25:45）；`rig.py status` 报游戏没在运行、真实安装未被改动。

## 7. 留给 FB

### `cpp_compare.py` 要怎么适配

- C++ 现在也做「HOOK 的命中位置是函数入口」。Python 只因这一条判 HOOK 失效时，两边状态都是 `fail`，不再进 `static_only`；C++ 的 `problem` 是「命中位置不是函数入口（在 X 开始的函数里，+0x..）」，`hit`、`resolved` 照样给出。本次 22 个 exe 里受影响的是 `u1d/hook_entry`（`ConsumeHealthDamageBlock`），原来单列，现在两边都失效、逐项一致。
- 文件开头说明里「Python 独有的检查」去掉「命中位置是不是函数入口」。S5 要的「静态检查类别」集合（偷取范围的指令、偷取不在指令边界、分支跳进偷取范围、hooks.md 重叠、`DD_ENTRY` 不符或长度不对）不要包含函数入口；建议反过来加一条：Python 有「不是函数入口」时 C++ 也必须失效，否则算不一致。
- `dd_layout_compare` 的 JSON 里 `timing_ms.function_table` 现在是解析特征码之前单独建表的耗时，`resolve` 包含它。

### 备注 2 的 Python 侧

行为不用改。建议把 `sigs.py` 125 行写明 `sig.required in ("REQUIRED", "LOADER")`，或者在旁边注释「`Sig.need` 把 LOADER 归为 REQUIRED」，免得再被读成只查 REQUIRED；selftest 可以加一个 LOADER GLOBAL 的断言（manifest 里现在没有这种特征码，要构造）。

### 备注 4 的 verify 侧

`check_hook` 加 `if steal < 5: res.fail("偷取 N 字节小于 5，放不下 jmp rel32")`（manifest 的 lint 也可以查）。C++ 这边：加载器是编译期 `static_assert`，DDCore 的 hook 引擎装的时候拒绝。

### 收紧依据对 Python 工具的影响

我跑了 `dd_update.py selftest --only mutants,mcheck,roundtrip --out build/u1fa/selftest`（`selftest.txt`，退出码 1）。往返通过（`struct_shift` repair --apply 后 verify 0、副本编译 DDCore、accept 都通过）；不通过的 4 处都是样本表的预期跟不上这次的改动，没有修错：
- `vslot`：多出 `VtSlotHeal` 失效。这是新依据（S2）该有的结果，样本表补上。
- `evidence_misbind`：`EffectDotBurn` 原来预期「误绑定、repair 修好」，现在是「依据不成立」，repair 对 `EffectDotBurn`、`EffectDotStress` 报 manual（模式里前后字段的字面量也跟着挪了，修复工具只改值不改模式）。verify 退出码因此是 1 不是 2。样本表要改成这个预期；如果还想保留「依据位置核对抓住误绑定」的样本，要换一个没收紧的依据来造。
- `struct_shift`（0xB0 插入 8 字节、新字段没有复制代码）：多出 `InstRemoveInactive`、`InstRemoveBattle` 失效（值没变，界标 0xB0 的 dword 挪走了），repair 给 repaired-中、值不变。这是收紧的代价（ADR 0036「局限」）。按 S7 给新字段加 8 字节的复制代码（`48 8B 82` 形状）后界标照样对不上，预期照这个写；或者插入点避开界标。
- `mcheck`：`EffectDotBurn` 的变异（`40 01 00 00` → `48 01 00 00`）原意是让模式唯一命中别处、靠依据位置核对判误绑定（`MUST_MISBIND`）。收紧后它命中 0 次，判「依据不成立」，repair 报 manual、修错 0。`MUST_MISBIND` 要换一个没收紧的依据，或者放宽成「判误绑定或不成立、且不能修错」。

基线已经按新清单重新生成，`position.py` 的依据位置核对两个版本都是 100 对上；收紧后的依据命中位置是模式开头（要读的那条复制），不是原来的那条指令。盲定位、对照两步单独跑过：`selftest --only blind`（`selftest_blind.txt`）通过，两个方向特征码 62 条正确 62、错误 0，函数级错误 0（需要人工 1、2）；对照见第 6 节。

### 写进 08 的事

第 5 节的未知版本清单、第 3 节的两个例外、第 8 节的 `relocate_sched` 发现。

## 8. 没做到的事、发现和注意事项

- **审核备注 7 的三个样本**现在的结果：`hook_entry` 的 hook 装在函数中间 → C++ 也判 `ConsumeHealthDamageBlock` 失效（入口检查，设计上保证），已知版本开关停 `damage.*` 的写主题（`VtSlotDamage[0]` 没法求值，`damage.*` 都是 hooked）；`evidence_misbind` → `EffectDotBurn`、`EffectDotStress` 依据不成立（收紧），停 `combat.effect_get`、`effect.*`；`vslot` → `VtSlotHeal[1]` 不成立，停 `heal.*`（新依据，设计上保证）。三处保护现在都有对应的依据，不再只靠别的项碰巧失败。08 的描述由 FB 改。
- **新发现（和审核备注 9 同类）**：U1d 的 `relocate_sched` 样本（`QueryListenerAdd` 搬迁）在已知版本开关下就停 `core`：必需项 `ListenerEntryListener`、`ListenerEntryActive` 的第 3 条依据锚在可选特征码 `QueryListenerAdd` 上，它失效时这两项「没法求值」，DDCore 整体停用。U1d 时的对照输出也是这样（`build/u1d/selftest/compare/compare.txt`），不是这次改出来的。写进了 ADR 0036「局限」；要不要把这两条依据拆到 Query 那边由主线定。
- **测试目录里有一个不是我部署的 DLL**：`mods/DD Mod Lib/SpineFix.dll`（修改时间 2026-11-19 18:33），第二次自检（19:12）时加载器「强制加载前置 DLL」加载了它，它 hook 了 `2585E0`（`SpineSprite::Render`），第一次运行时写了默认配置 `DDModLoader\cfg\SpineFix.cfg`。第一次自检（15:48）时还没有它。它不在 rig 方案里，我没动它和它的配置；请主线确认是谁放的、要不要留在 scenario4 里。
- **真实安装的游戏运行过**：19:14:49 起 guest 上有一个 `steamapps\common\DarkestDungeon\_windows\win64\Darkest.exe`（真实安装，不是测试目录）在运行，不是我启动的，我没碰；19:23:50 再看时已经退出。rig.py 在它运行时拒绝部署和启动，未知版本模拟（19:24）等它退出后才做。`rig.py status` 报真实安装未被改动。
- 未知版本上停用的能力明显变多（第 5 节），按简报写进了 ADR 0036 的「考虑过的方案」，A/B/C 由用户定。缩小办法（给原型补「入口序言的参数接收指令」一类的推出依据、给 RTTI 补运行时检查）没做。
- 原型项只记参数个数，参数类型、返回值宽度只写在说明里，`static_assert` 只绑定个数。
- 加载器没有和 `ProtoCreateAppSystems` 绑定（不想让加载器包含 `layout.h`）。
- 收紧的依据之外，第 4 节末尾列的单条指令依据仍可能误绑定；运行时没有基线，依据误绑定和诱饵函数的局限照旧（ADR 0036）。
- 第一版收紧（整段）跑过一次完整的游戏自检和未知版本模拟，日志改名留底为 `rig_logs/u1fa-selfcheck-pre`、`u1fa-unknown-pre`；之后只改了复制构造那 11 条依据的模式和注释，最终的证据是重新编译后重跑的。

## 9. 改动文件

- `core/src/game/layout.inc`：文件开头的「约定」；`VtSlotStress` 改 RUNTIME 加 2 条依据；`VtSlotHeal` 加 1 条；14 条依据收紧；新项 55 个（第 2 节）；注释
- `core/src/game/signatures.inc`：锚点 `ActorStressInternal`；末尾 `DD_KNOWN_BUILD` 的注释（备注 1）
- `core/src/game/layout.h`：`layout::kArgs`
- `core/src/game/layout_check.h`、`layout_check.cpp`：`FunctionTable::Enclosing`、`IsFunctionStart`，`CheckHookEntry`，`ResolveSignatures`、`RunCheck`、`Evaluator` 的共享函数表参数，`kDepInfo` 的归类，`BuildGate` 的两个例外和主题规则说明，备注 2 的注释
- `core/src/game/game.cpp`：函数表建一次、共用
- `core/src/core.cpp`：`ApplyTopicGate` 用 `hooked`，耗时日志，每帧 hook 原型的 `static_assert`
- `core/src/events/hub.h`：`Topic::hooked`
- `core/src/events/system.cpp`：语义主题和 `app.*` 置 `hooked`；原型、宽度的 `static_assert`；`ListenerAddFn`；RTTI 和伪造虚表用清单常量；`VecFirst` 用 `kStdVectorFirst`
- `core/src/combat/combat.cpp`：`buff.*` 置 `hooked`；原型、宽度的 `static_assert`；组 guid 读写加 `kBuffEntryGuid`
- `core/src/persist/persist.cpp`：`campaign.save` 置 `hooked`；原型的 `static_assert`
- `core/src/game/actors.cpp`：`ActorMaxStat` 原型和宽度的 `static_assert`
- `loader/early_hook.h`：偷取字节数的 `static_assert`、函数入口检查、开头说明
- `tests/unit/test_main.cpp`：「布局核对」一节（第 3 节）
- `tests/layout_compare/main.cpp`：函数表建一次、共用，耗时单列
- `tools/baseline/steam.json`、`nosteam.json`：重新生成
- `docs/adr/0036-runtime-layout-check.md`：重写
- `knowledge/dd1/combat.md`：槽 36 调 480FE0、槽 51 的加血指令串和参数位置、复制构造的逐字段复制、ApplyEffect 结果的清零、4F02D0 的解析顺序，都带 [静态]
- `docs/u1/fa-report.md`：本文件

没改 `tools/` 下的 Python（FB 的范围）和 FB 负责的文档。
