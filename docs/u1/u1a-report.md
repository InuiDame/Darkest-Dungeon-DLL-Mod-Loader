# U1a 报告：布局清单、基线快照、verify

阶段工作文件，用户验收后删除。简报见 [brief.md](brief.md)，范围是第 1、2、3 节，验收项 A1、A2、A3 和 A7 里与改动相关的构建、单元测试。完整输出在 guest 的 `D:\workplace\DD.ModLoader\build\u1a\`（下文「证据」一栏的文件名都相对这个目录）。

## 1. 结论

| 验收 | 状态 | 证据 |
| --- | --- | --- |
| A1 清单覆盖全部布局常量、C++ 不再有散落定义、每项有依据或写明怎么核实、有依赖表 | 完成 | 第 4 节盘点表；`layout.inc` 227 项（运行时依据 82、只有离线依据 21、没有静态依据 124，每项都有 `DD_UNVERIFIED` 说明）；`tests/unit/layout_golden.cpp` 用 230 条常量断言和 13 张表的内容比较对照改动前的值（编译期）；`dd_update.py verify` 的清单检查（每项都有依赖表、类型和依据一致）通过 |
| A2 verify 在 Steam 版和离线版上全部通过；原两个脚本的检查全部覆盖；JSON 和退出码 | 完成 | `verify.txt`、`verify.json`（退出码 0）；第 5 节对照表；冒烟样本 A 退出码 1、B 退出码 2（`mut/`）；`--sigs` 见 `plugin/` |
| A3 基线可复现，相似度阈值有数据 | 完成 | `baseline_sha256.txt`（两次生成 sha256 相同、`cmp` 一致）；`simstats.txt`、`simstats.json`；第 7 节 |
| A7（与本阶段相关的部分） | 完成 | `build.txt`：DDCore、DDModLoader、DDLua、dd_core_tests、dd_lua_tests 全部 `xmake build -r` 通过，没有警告；dd_core_tests 728 项失败 0，dd_lua_tests 1291 项失败 0。自检到主菜单不在本阶段（没有启动游戏） |

## 2. 构建结果

guest 上 `source DD.ModLoader/tools/env.sh; dd_config`，然后逐个 `xmake build -r`（`build.txt`）：

```
== xmake build -r DDCore          [100%]: build ok
== xmake build -r DDModLoader     [100%]: build ok
== xmake build -r DDLua           [100%]: build ok
== xmake build -r dd_core_tests   [100%]: build ok
== xmake build -r dd_lua_tests    [100%]: build ok
== xmake run dd_core_tests        728 项检查，失败 0
== xmake run dd_lua_tests         Lua 5.4.9，1291 项检查，失败 0
```

常量值没有变的证明是 `tests/unit/layout_golden.cpp`：它由改动前（`git show HEAD:`）的源码生成，逐项写 `static_assert(layout::kX == 旧值)`，四张枚举表、六张 sub_stat 表、内置状态、`.target` 表和来源标志逐项比较内容，`kDamageEpsilon` 比较 `std::bit_cast<float>(kDamageEpsilonBits) == 1e-6f`，规则类型 13/42 和 `ValidateSubStat` 的 13 个 stat 下标比较 `IndexOf(表, "名字")` 和原来的数字。清单的 227 项全部被断言覆盖（生成时核对过没有遗漏），dd_core_tests 能编译就说明值没变。另外在 `build/u1a/head/` 用 `git archive HEAD` 构建了改动前的 DDCore 和加载器做二进制对比：两边不是字节一致（加载器的入口字节检查改成了读 `DD_ENTRY`，DDCore 的 `.text` 差 0x60 字节、`.data` 差 0x40 字节，属于代码组织变化），所以二进制对比不能当作证据，以编译期断言为准。

## 3. 清单格式

格式的权威说明写在 `core/src/game/layout.inc` 和 `signatures.inc` 的开头，这里是给 U1b、U1c 的摘要和实现要点。U1a 之后只加项，不改语义。

### 3.1 文件和宏

- `core/src/game/signatures.inc`：地址（不变的 `DD_SIG`、`DD_ENTRY`、`DD_CHAIN_TARGET`、`DD_RUNTIME_SELECT`、`DD_KNOWN_BUILD`），新增：
  - `required` 多一个取值 `ANCHOR`：只给布局依据当锚点，DDCore 不用它的地址（`game.cpp` 用 `DD_SIG_ROW_##req` 在编译期把它排除出 `kSignatures`，U1a 的 `Resolve` 行为不变）。现在只有一条 `BuffInstanceCtor`（Instance 构造 `431240`）。失效时只影响以它为锚点的依据。
  - `DD_DEPS(id, "词汇 ...")`：特征码的依赖表，每条都要有（插件自己的 `--sigs` 文件可以不写）。
  - `DD_ENTRY(CreateAppSystems, "48 89 4C 24 08")`：加载器原来写死的入口字节改从这里读。
- `core/src/game/layout.inc`（新增）：布局清单。
  - `DD_LAYOUT(id, type, value, need, check, "deps")`：type 是 `OFFSET/SIZE/SLOT/ENUM/COUNT/CONST/TABLE`（C++ 类型：ENUM 是 `int32_t`，CONST 是 `uint32_t`，其余 `size_t`；浮点常量写位模式）；need 是 `REQUIRED`（依赖表里必须有 `core`）或 `OPT`；check 是 `RUNTIME`（至少一条 `DD_EVIDENCE`）、`OFFLINE`（只有 `DD_OFFLINE`）、`NONE`（必须有 `DD_UNVERIFIED`）。check 是显式标记，和依据是否一致由清单检查保证。
  - `DD_EVIDENCE(id, sig, origin, arg, where, span, "pattern", at, read, mul, rel, add)`：可在运行时求值的依据，语义见 3.2。`id` 也可以是特征码名字，这时是那条特征码的同一性依据（现在有 `HeroVtable`、`MonsterVtable` 的槽位 3 检查）。
  - `DD_OFFLINE(id, check)`：只能离线求值的依据，按名字挂到 `tools/ddupdate/offline.py` 的注册函数上，一个检查可以支撑多项。
  - `DD_UNVERIFIED(id, "说明")`：没有静态依据的项写明怎么核实。
  - `DD_NAMES(id, "名字", ...)`、`DD_VALUES(id, 值, ...)`：TABLE 的内容，value 是项数。`DD_VALUES` 里可以写 `SRC_S | SRC_T | SRC_Y`（等于 `DD_SOURCE_F_*`，C++ 和 Python 都定义了这三个符号）。
- `core/src/game/layout.h`（新增）：把 `layout.inc` 展开成 `ddc::game::layout::k<id>`、`k<id>Names[]`、`k<id>Values[]`，外加编译期的 `IndexOf(表, "名字")`（名字不在表里时编译失败，规则类型和 stat 下标都用它从名字表取，不再写数字）。

依赖表的词汇（`tools/ddupdate/manifest.py` 的 `DEPENDENTS`，清单检查只接受这些）：`core`、`loader`、`events.native`、`events.query`、`pvp`、`game.actor`、`game.actor_info`、`game.actor_list`、`game.battle`、`game.mods`、`combat.buff_read`、`combat.buff_add`、`combat.buff_remove`、`combat.buff_define`、`combat.effect_get`、`combat.effect_apply`、`combat.effect_define`、`combat.rule`、`combat.skill`、`combat.enum`、`persist`、`check`（只给校验用），以及任意 `topic:<主题>`（可以用 `*` 结尾当前缀，如 `topic:buff.*`）。U1b 要把这些词映射到具体的停用开关：`game.actor` 停用意味着所有按句柄解析 actor 的能力都停（combat 的读写都经过句柄），`pvp` 停用意味着所有 `.pre` 主题、Query 写回、combat 写操作停（现有代码在 `RaidCurrent` 缺失时已经这么做）。

### 3.2 运行时依据的求值语义（给 U1b 的 C++ 实现）

参考实现是 `tools/ddupdate/evidence.py` 的 `evaluate`（50 行），C++ 照它写，两边对同一个映像必须给出同样的结论（U1b 的对照测试）。

1. **起点**（sig 必须解析成功，否则这条依据 blocked，按不成立处理）
   - `HIT`：命中位置。GLOBAL_ANY 取地址最低的命中（`ScanAll` 的第一个）。
   - `TARGET`：解析结果（HOOK/CODE 是命中位置，GLOBAL 是 rip 相对目标）。`DD_RUNTIME_SELECT` 的特征码只能用 `HIT`（清单检查强制），因为类名表在文件里是空的，离线选不出候选。
   - `HIT_VALUE`：命中位置 + 这一项的值 + arg（arg 有符号）。用于 `MonsterBlockReturn` 的 +52/+56 这类「命中位置上的固定偏移」。
   - `VSLOT`：把 TARGET 当虚表，读第 arg 槽的 8 字节指针减映像基址得到 RVA，必须落在 `.text`。加载后的模块减实际基址（指针已重定位）；对照测试如果是没做重定位的映射，减 `OptionalHeader.ImageBase`。
   - `VSLOT_VALUE`：同上，槽号是这一项的值（用于虚表槽号本身）。
   - `NOSIG`：没有起点，sig 写 `NONE`，只能配 `where=TEXT`。
2. **范围**
   - `ENTRY`：模式必须恰好在起点处匹配。
   - `WINDOW`：`[起点, 起点+span)`。
   - `FUNC`：只看异常目录（`DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION]`）里的 `RUNTIME_FUNCTION`，跳过 BeginAddress 为 0 的项，按 BeginAddress 排序，二分找 `Begin <= 起点 < End` 的表项；从它出发找主函数：UnwindData 低位为 1 时是间接表项（指向另一个 RUNTIME_FUNCTION），否则看 `UNWIND_INFO` 的 `Flags & UNW_FLAG_CHAININFO`（Flags 是第一个字节右移 3 位），有时链到 `unwind_info + 4 + ((CountOfCodes + 1) & ~1) * 2` 处的 RUNTIME_FUNCTION；最多跟 32 次，最后到达的表项的 BeginAddress 是主函数。主函数相同的所有表项都是这个函数的分块。起点不在任何表项里时依据不成立。C++ 自己解析异常目录，不用 `RtlLookupFunctionEntry`。
   - `TEXT`：`.text` 的 `VirtualAddress .. VirtualAddress + VirtualSize`（和 `ModuleTextSection` 一致）。
   - 匹配：模式语法同 `ParsePattern`，必须完整落在某一个范围里（不跨分块），每个字节位置都试（可以重叠，和 `ScanPattern` 的计数一致），所有范围加起来恰好命中一次。
3. **读取**：命中位置 + at 处，小端。`NONE` 只要求唯一命中；`U8/U16/U32/U64` 零扩展；`I8/I32` 符号扩展；`DEREF_U32`：把 +at 处的 int32 当位移，目标 = 命中 + at + 4 + 位移（位移必须是指令的最后一个字段），读目标处的 uint32；`REL32`：同样算出的目标 RVA 本身。
4. **比较**：期望值 = `value(id) * mul + value(rel) + add`，64 位有符号整数，不截断。`value(id)`：清单项是它的值，特征码名字是 0。`value(rel)`：`NONE` 是 0，清单项是它的值，特征码名字是它的解析结果的 RVA（该特征码失效时依据 blocked）。读到的值直接和期望值比较；期望值超出读取宽度的写法会被清单检查拒绝。
5. 一项的所有 `DD_EVIDENCE` 都成立这一项才成立；`OFFLINE`、`NONE` 的项运行时一律按「不能核对」处理（策略是 U1b 的 ADR）。

实现时的几个边界：

- `CreateAppSystems` 是 `LOADER`，DDCore 启动时入口已被加载器打了 `E9`，扫描必然失败；如果以后有依据以它为锚点，C++ 要用 `CoreBootInfo.createAppSystems` 当解析结果。现在没有这样的依据。
- HOOK 特征码的偷取字节写成了 `??`，DDCore 自己装了 hook 之后再扫描也能命中，但核对应该在 `Resolve` 之后、装 hook 之前做。
- `ANCHOR` 特征码 U1b 要开始解析（只用于依据），失败只记日志。
- 现有的 106 条依据里起点和范围的组合：`TARGET+FUNC` 60 条、`VSLOT/VSLOT_VALUE`（ENTRY 或 FUNC）21 条、`HIT+WINDOW/ENTRY` 13 条、`NOSIG+TEXT` 10 条、`HIT_VALUE+ENTRY` 2 条；读取 `U32` 48、`U8` 44、`NONE` 11、`REL32` 2、`DEREF_U32` 1。TEXT 范围要扫一遍 12 MB 的 `.text`，10 条在运行时大约是 10 次 `ScanPattern` 的开销。

### 3.3 Python 模块（给 U1c 的接口）

入口 `tools/dd_update.py`，模块在 `tools/ddupdate/`：

| 模块 | 给修复工具用的接口 |
| --- | --- |
| `manifest` | `load(sig_file, layout_file, use_layout)` 返回 `Manifest`（`sigs`、`items`、`evidence`、`known_builds`、`value(name)`）；`Sig`、`Item`、`Evidence` 数据类；`iter_macros`、`literal` 解析 X-macro；`lint(m)`。修复写补丁时要按行号（`Sig.line`、`Item.line`、`Evidence.line`）改原文件，解析器保留了这些行号 |
| `image` | `Image(path)`：`mem`（按 RVA 映射）、`function_ranges(rva)`、`enclosing_function`、`entry_of`、`func_chunks`、`scan(regex, start, end)`、`scan_text`；`compile_pattern(text)` 返回可重叠匹配的正则 |
| `sigs` | `resolve(image, sig)` → `SigResult`（`hits`、`hit`、`resolved`、`candidates`、`status`、`problems`）；`check_hook`（偷取范围的安全检查，重新生成 HOOK 特征码后要再过一遍）；`resolve_all` |
| `evidence` | `evaluate(image, manifest, sig_results, ev)` → `EvidenceResult`（`hit`、`captured`、`expected`、`implied`：按读到的值反推的新常量，「模式还能命中但值变了」时就是修复的新值）；`origin_of`、`ranges_of` 可单独用来定位起点和范围 |
| `fingerprint` | `fingerprint(image, rva, cs, imports)`（规范化 token、callees、imports、strings、globals、imms、thunk）；`similarity`、`fp_similarity`、`shingles`；`CallIndex(image).callers(rva, cs)`；`SIMILARITY_THRESHOLD` |
| `baseline` | `load(path)`（token 已解开）、`generate`、`sig_functions`（特征码对应的函数和目标函数）、`compute_relations`；基线结构见第 7 节 |
| `suggest` | `build_pattern(image, insns, index, field, ranges)`：从一条指令起生成在范围里唯一的模式（rip 位移、分支 rel 和要读的字段写成 `??`，不够唯一时往前多取几条）；`fields_equal(ins, value)` 找等于某值的位移、立即数字段。修复的「重新生成依据」可以直接用；命令行 `dd_update.py suggest` 是写清单用的 |
| `offline` | `CHECKS` 注册表、`Context`（`value`、`sig`、`hits`、`body`、`init_tables` 等）；新检查用 `@check("名字", "说明")` 注册 |
| `verify` | `verify_exe(path, manifest, baselines, sections, foreign)` 返回报告 dict；修复后自动再校验可以直接调它 |

U1c 只改「修复相关的模块」：建议新增 `ddupdate/repair.py`（定位、生成、补丁），命令接到 `dd_update.py`。如果需要在基线里加字段，`baseline.FORMAT` 加一并保持旧字段，verify 只读它认识的字段。

## 4. 盘点表（A1）

范围是 `core/`、`loader/` 的全部源码，逐个文件读过，另外用正则扫了所有十六进制字面量和 `+ 数字` 形式的偏移。下表每行是改动前的一个常量（或写在代码里的字面量）、值、登记到的清单项、依据。表由 `tests/unit/layout_golden.cpp`（改动前的值和出处）和 `layout.inc`（依据）生成。

**game/actors.h**

| 原来的常量 | 值 | 清单项 | 依据 |
| --- | --- | --- | --- |
| `actor_layout::kRosterAlt` | 0x728 | `kRosterAlt` | 运行时：.text TEXT |
| `actor_layout::kRosterHeroes` | 0x20 | `kRosterHeroes` | 运行时：.text TEXT；.text TEXT |
| `actor_layout::kRaidHeroes` | 0x08 | `kRaidHeroes` | 运行时：.text TEXT；.text TEXT；离线：party_monster_targets |
| `actor_layout::kEntryHero` | 0x08 | `kEntryHero` | 没有静态依据：名册遍历的运行核对（P3 探针 probe_p3、tests/plugins/probe_p3c.cpp）；Hero 虚表检查不过时句柄不给出，不会读错位置 |
| `actor_layout::kHeroId` | 0x137C | `kHeroId` | 运行时：HeroVtable 槽5 ENTRY |
| `actor_layout::kHeroClass` | 0x1320 | `kHeroClass` | 运行时：HeroVtable 槽6 ENTRY |
| `actor_layout::kMonsterGuid` | 0x1320 | `kMonsterGuid` | 运行时：MonsterVtable 槽5 ENTRY |
| `actor_layout::kMonsterClass` | 0x1468 | `kMonsterClass` | 运行时：MonsterVtable 槽6 ENTRY；MonsterBlockReturn 命中 WINDOW |
| `actor_layout::kRaidBattle` | 0x4898 | `kRaidBattle` | 运行时：.text TEXT；离线：raid_battle_lea、party_monster_targets |
| `actor_layout::kBattleMonsters` | 0x38 | `kBattleMonsters` | 运行时：.text TEXT；.text TEXT |
| `actor_layout::kBattleState` | 0x288 | `kBattleState` | 离线：battle_create_order |
| `actor_layout::kBattleGuidCounter` | 0x2D4 | `kBattleGuidCounter` | 运行时：.text TEXT；.text TEXT；离线：battle_create_order |
| `actor_layout::kActorHp` | 0x10A8 | `kActorHp` | 运行时：HeroVtable 槽40 ENTRY；HeroVtable 槽43 ENTRY |
| `actor_layout::kActorStress` | 0x12FC | `kActorStress` | 运行时：HeroVtable 槽37 FUNC；离线：hero_stress_slot |
| `actor_layout::kActorMaxHpStat` | 0x768 | `kActorMaxHpStat` | 运行时：HeroVtable 槽50 FUNC；离线：maxstat_calls |
| `actor_layout::kActorMaxHpMask` | 0x790 | `kActorMaxHpMask` | 运行时：HeroVtable 槽50 FUNC；离线：maxstat_calls |
| `actor_layout::kClassId` | 0x08 | `kClassId` | 离线：class_id_strings |
| `actor_layout::kClassIdCapacity` | 0x40 | `kClassIdCap` | 没有静态依据：职业 id 的字段宽度 char[0x40]（knowledge/dd1/combat.md）；读的时候要求在宽度内以 0 结尾，否则不给 class_id |

**combat/combat.cpp**

| 原来的常量 | 值 | 清单项 | 依据 |
| --- | --- | --- | --- |
| `layout::kActorBuffGroup` | 0x460 | `kActorBuffGroup` | 运行时：HeroVtable 槽13 FUNC |
| `layout::kActorNextGuid` | 0x478 | `kActorNextGuid` | 运行时：HeroVtable 槽13 FUNC |
| `layout::kEntrySize` | 0x418 | `kBuffEntrySize` | 运行时：BuffGroupErase FUNC |
| `layout::kEntryInstance` | 0x08 | `kBuffEntryInstance` | 运行时：BuffDurationUpkeep FUNC |
| `layout::kInstanceSize` | 0x410 | `kBuffInstanceSize` | 运行时：BuffGroupErase FUNC；BuffLibraryIndex FUNC；BuffLibraryIndex FUNC；离线：instance_copy_fields |
| `layout::kInstStat` | 0x00 | `kInstStat` | 没有静态依据：Instance+0 的 stat_type（偏移 0 没有位移可读）；字段布局由复制构造的逐字段复制（InstSubType 起）佐证，test_combat 的 buff_get 运行核对 |
| `layout::kInstSubType` | 0x04 | `kInstSubType` | 运行时：BuffInstanceCopy FUNC |
| `layout::kInstSubHash` | 0x44 | `kInstSubHash` | 运行时：BuffInstanceCopy FUNC |
| `layout::kInstAmount` | 0x48 | `kInstAmount` | 运行时：BuffInstanceCopy FUNC |
| `layout::kInstDuration` | 0x4C | `kInstDuration` | 运行时：BuffInstanceCtor FUNC；BuffDurationUpkeep FUNC；离线：instance_ctor_defaults |
| `layout::kInstDurationType` | 0x50 | `kInstDurationType` | 运行时：BuffInstanceCtor FUNC；BuffDurationUpkeep FUNC；离线：instance_ctor_defaults |
| `layout::kInstSource` | 0x54 | `kInstSource` | 运行时：BuffInstanceCopy FUNC |
| `layout::kInstSourceData` | 0x58 | `kInstSourceData` | 运行时：BuffInstanceCopy FUNC |
| `layout::kInstRule` | 0x5C | `kInstRule` | 运行时：BuffRuleActive FUNC |
| `layout::kInstFalseRule` | 0x60 | `kInstFalseRule` | 运行时：BuffRuleActive FUNC |
| `layout::kInstId` | 0x61 | `kInstId` | 运行时：BuffInstanceCopy FUNC |
| `layout::kInstIdHash` | 0xA4 | `kInstIdHash` | 运行时：BuffInstanceCopy FUNC |
| `layout::kInstTarget` | 0xA8 | `kInstTarget` | 运行时：BuffInstanceCopy FUNC |
| `layout::kInstRemoveInactive` | 0xAC | `kInstRemoveInactive` | 运行时：BuffInstanceCopy FUNC |
| `layout::kInstRemoveBattle` | 0xAD | `kInstRemoveBattle` | 运行时：BuffInstanceCopy FUNC |
| `layout::kInstRuleFloat` | 0xB0 | `kInstRuleFloat` | 运行时：BuffRuleActive FUNC |
| `layout::kInstRuleString` | 0xB4 | `kInstRuleString` | 运行时：BuffInstanceCopy FUNC |
| `layout::kInstRuleHash` | 0xF4 | `kInstRuleHash` | 运行时：BuffRuleActive FUNC |
| `layout::kInstAdditional` | 0x158 | `kInstAdditional` | 运行时：BuffInstanceCopy FUNC；BuffInstanceCopy FUNC；离线：instance_copy_fields |
| `layout::kInstClearOnRule` | 0x3B0 | `kInstClearOnRule` | 运行时：BuffInstanceCopy FUNC；BuffInstanceCopy FUNC；离线：instance_copy_fields |
| `layout::kInstReadSize（改成由 kInstRuleHash 推出）` | 0xF8 | `kInstRuleHash + 4` | 运行时：BuffRuleActive FUNC |
| `layout::kLibraryAltMap` | 0x10 | `kLibraryAltMap` | 没有静态依据：查 Buff 的函数在 App+0x54B（AltRosterFlag）为 1 时用 Buff::Library+0x10 的联机 map（knowledge/dd1/combat.md 的「Buff」，P4-0D 探针） |
| `layout::kNodeLeft` | 0x00 | `kMapNodeLeft` | 运行时：BuffLibraryIndex FUNC |
| `layout::kNodeRight` | 0x10 | `kMapNodeRight` | 运行时：BuffLibraryIndex FUNC |
| `layout::kNodeIsNil` | 0x19 | `kMapNodeIsNil` | 运行时：BuffLibraryIndex FUNC |
| `layout::kNodeKey` | 0x20 | `kMapNodeKey` | 运行时：BuffLibraryIndex FUNC |
| `layout::kNodeValue` | 0x28 | `kMapNodeValue` | 运行时：BuffLibraryIndex FUNC |
| `layout::kEffectTarget` | 0x44 | `kEffectTarget` | 离线：effect_misc_fields |
| `layout::kEffectChance` | 0x48 | `kEffectChance` | 离线：effect_field_types |
| `layout::kEffectStress` | 0x4C | `kEffectStress` | 离线：effect_field_types |
| `layout::kEffectStun` | 0x50 | `kEffectStun` | 离线：effect_field_types |
| `layout::kEffectHeal` | 0xC8 | `kEffectHeal` | 离线：effect_field_types |
| `layout::kEffectHealPercent` | 0xCC | `kEffectHealPercent` | 离线：effect_field_types |
| `layout::kEffectCombatStatBuff` | 0xEC | `kEffectCombatStatBuff` | 离线：effect_field_types |
| `layout::kEffectDotPoison` | 0x134 | `kEffectDotPoison` | 离线：effect_field_types |
| `layout::kEffectDotBleed` | 0x138 | `kEffectDotBleed` | 离线：effect_field_types |
| `layout::kEffectDotBurn` | 0x13C | `kEffectDotBurn` | 运行时：ParseEffectFile FUNC |
| `layout::kEffectDotStress` | 0x1B8 | `kEffectDotStress` | 运行时：ParseEffectFile FUNC |
| `layout::kEffectDuration` | 0x1CC | `kEffectDuration` | 离线：effect_field_types |
| `layout::kEffectOnHit` | 0x345 | `kEffectOnHit` | 运行时：ParseEffectFile FUNC |
| `layout::kEffectOnMiss` | 0x346 | `kEffectOnMiss` | 运行时：ParseEffectFile FUNC |
| `layout::kEffectApplyOnce` | 0x347 | `kEffectApplyOnce` | 运行时：ParseEffectFile FUNC |
| `layout::kEffectQueue` | 0x348 | `kEffectQueue` | 运行时：ParseEffectFile FUNC |
| `layout::kEffectBuffAmount` | 0x3B8 | `kEffectBuffAmount` | 离线：effect_field_types |
| `layout::kEffectBuffIds` | 0x3C8 | `kEffectBuffIds` | 运行时：ParseEffectFile FUNC |
| `layout::kEffectSwap` | 0x3E0 | `kEffectSwap` | 运行时：EffectApplyInternal FUNC；离线：effect_misc_fields |
| `layout::kEffectBuffDurationType` | 0x430 | `kEffectBuffDurationType` | 离线：effect_misc_fields |
| `layout::kEffectReadSize` | 0x438 | `kEffectReadSize` | 没有静态依据：effect_get 一次读出的前缀长度（覆盖到 .buff_duration_type 之后），Effect 对象不小于它；P4-0C 探针和 test_combat 的运行核对 |
| `layout::kHeroClassSkills` | 0xBD0 | `kHeroClassSkills` | 没有静态依据：只有运行证据（P4-0C 探针 0B-11：55 次执行的技能指针都在这几张表里），更新后跑 probe_p4 或 test_combat 的技能部分 |
| `layout::kHeroClassMoveSkills` | 0xBE8 | `kHeroClassMoveSkills` | 没有静态依据：只有运行证据（P4-0C 探针 0B-11：55 次执行的技能指针都在这几张表里），更新后跑 probe_p4 或 test_combat 的技能部分 |
| `layout::kHeroSkillSize` | 0x510 | `kHeroSkillSize` | 没有静态依据：只有运行证据（P4-0C 探针 0B-11：55 次执行的技能指针都在这几张表里），更新后跑 probe_p4 或 test_combat 的技能部分 |
| `layout::kMonsterClassSkills` | 0xBA8 | `kMonsterClassSkills` | 没有静态依据：只有运行证据（P4-0C 探针 0B-11：55 次执行的技能指针都在这几张表里），更新后跑 probe_p4 或 test_combat 的技能部分 |
| `layout::kMonsterSkillSize` | 0x500 | `kMonsterSkillSize` | 没有静态依据：只有运行证据（P4-0C 探针 0B-11：55 次执行的技能指针都在这几张表里），更新后跑 probe_p4 或 test_combat 的技能部分 |
| `layout::kSkillId` | 0x08 | `kSkillId` | 没有静态依据：技能对象的 id char[64] 在 +8、等级 +0x48、类型 +0x50、friendly +0x90（knowledge/dd1/combat.md 的「技能」）；P4-0C 探针和 test_combat 的技能部分 |
| `layout::kSkillLevel` | 0x48 | `kSkillLevel` | 没有静态依据：技能对象的 id char[64] 在 +8、等级 +0x48、类型 +0x50、friendly +0x90（knowledge/dd1/combat.md 的「技能」）；P4-0C 探针和 test_combat 的技能部分 |
| `layout::kSkillType` | 0x50 | `kSkillType` | 没有静态依据：技能对象的 id char[64] 在 +8、等级 +0x48、类型 +0x50、friendly +0x90（knowledge/dd1/combat.md 的「技能」）；P4-0C 探针和 test_combat 的技能部分 |
| `layout::kSkillFriendly` | 0x90 | `kSkillFriendly` | 没有静态依据：技能对象的 id char[64] 在 +8、等级 +0x48、类型 +0x50、friendly +0x90（knowledge/dd1/combat.md 的「技能」）；P4-0C 探针和 test_combat 的技能部分 |
| `layout::kSkillEffects` | 0x3C8 | `kSkillEffects` | 离线：skill_effects_offset |
| `layout::kRaidPvpName` | 0x52C0 | `kRaidPvpName` | 运行时：RaidCurrent 命中 ENTRY |
| `layout::kSlotAddBuff` | 13 | `kSlotAddBuff` | 运行时：HeroVtable 槽=常量 FUNC |
| `layout::kSlotRemoveBuffs` | 14 | `kSlotRemoveBuffs` | 运行时：HeroVtable 槽=常量 FUNC |
| `MapFind：head + 8 是根` | 8 | `kMapNodeParent` | 运行时：BuffLibraryIndex FUNC |
| `EhVectorDtor 的元素大小、技能表外层 vector 的步长` | 0x18 | `kStdVectorSize` | 运行时：BuffGroupErase FUNC |
| `EhVectorDtor 的元素个数` | 0x19 | `kInstAdditionalCount` | 运行时：BuffGroupErase FUNC |
| `G_ApplyEffect 的 res[0x20]` | 0x20 | `kEffectApplyResultSize` | 没有静态依据：ApplyEffect（4CB4D0）的结果对象：+8 起是 vector<uint32>（EffectApplyResultGuids 有依据），整个对象不超过 0x20；P4-0C 探针的运行核对 |
| `G_ApplyEffect 的 res + 8` | 8 | `kEffectApplyResultGuids` | 运行时：ApplyEffect FUNC |
| `DoRemove 的 function[0x40]` | 0x40 | `kStdFunctionSize` | 没有静态依据：MSVC std::function 的大小和实现指针位置，工具链大版本变化才会变；test_events（原生事件桥）和 test_combat（buff_remove）的运行核对 |
| `DoRemove 的 function + 0x38；system.cpp FakeListener 的 funcStorage[0x38]` | 0x38 | `kStdFunctionImpl` | 没有静态依据：MSVC std::function 的大小和实现指针位置，工具链大版本变化才会变；test_events（原生事件桥）和 test_combat（buff_remove）的运行核对 |
| `kPredVtable、system.cpp kImplVtableStorage 的项数` | 6 | `kStdFuncImplVtableSlots` | 没有静态依据：MSVC STL 的 _Func_base 虚函数顺序，工具链大版本变化才会变；靠 test_events（原生事件桥）和 test_combat（buff_remove）的运行核对 |
| `规则类型 13` | 13 | `IndexOf(kEnumRuleNames, 'actorStatus')` | 由 EnumRule 的名字表按名字 actorStatus 取下标（表本身离线核对） |
| `规则类型 42` | 42 | `IndexOf(kEnumRuleNames, 'is_actor_status')` | 由 EnumRule 的名字表按名字 is_actor_status 取下标（表本身离线核对） |

**sdk/include/dd/dd_api.h**

| 原来的常量 | 值 | 清单项 | 依据 |
| --- | --- | --- | --- |
| `DD_COMBAT_TEXT（游戏里 char[64] 的字段）` | 64 | `kGameTextCap` | 没有静态依据：Instance 的 sub_type（+0x04）到它的哈希（+0x44）、id（+0x61）到哈希（+0xA4）都隔 0x40 字节（knowledge/dd1/combat.md 的「Buff」） |

**events/system.cpp**

| 原来的常量 | 值 | 清单项 | 依据 |
| --- | --- | --- | --- |
| `kBattleRound` | 0xA8 | `kBattleRound` | 运行时：BattleAdvanceRound FUNC |
| `kVtSlotStress` | 36 | `kVtSlotStress` | 没有静态依据：Hero 槽位 36（4814B0）不直接读压力字段，经 480FE0 结算；test_events 的 stress.* 运行核对。槽位 37（压力恢复）读 +0x12FC 有依据 |
| `kVtSlotDamage` | 50 | `kVtSlotDamage` | 运行时：HeroVtable 槽=常量 FUNC；MonsterVtable 槽=常量 FUNC |
| `kVtSlotHeal` | 51 | `kVtSlotHeal` | 运行时：HeroVtable 槽=常量 FUNC |
| `kEntrySize` | 0x28 | `kListenerEntrySize` | 运行时：EventTypeCount 命中 WINDOW；ListenerAdd FUNC |
| `kEntryListener` | 0x08 | `kListenerEntryListener` | 运行时：EventTypeCount 命中 WINDOW；ListenerAdd FUNC；QueryListenerAdd FUNC |
| `kEntryActive` | 0x10 | `kListenerEntryActive` | 运行时：EventTypeCount 命中 WINDOW；ListenerAdd FUNC；QueryListenerAdd FUNC |
| `kQueryEntrySize` | 0x18 | `kQueryListenerEntrySize` | 运行时：QueryListenerAdd FUNC |
| `kVtSlotDeathsDoor` | 40 | `kVtSlotDeathsDoor` | 运行时：HeroVtable 槽=常量 ENTRY |
| `kVtSlotDead` | 43 | `kVtSlotDead` | 运行时：HeroVtable 槽=常量 ENTRY |
| `kAttackPerformer` | 0x20 | `kAttackPerformer` | 运行时：ApplyHit FUNC；ApplyMiss FUNC |
| `kAttackSkill` | 0x28 | `kAttackSkill` | 运行时：ApplyHit FUNC |
| `kTargetActor` | 0x00 | `kTargetActor` | 运行时：ApplyHit FUNC；ApplyMiss FUNC |
| `kTargetDamage` | 0x08 | `kTargetDamage` | 运行时：ApplyHit FUNC |
| `kTargetHit` | 0x10 | `kTargetHit` | 没有静态依据：目标数据 +0x10 是否命中，决定走 ApplyHit 还是 ApplyMiss（knowledge/dd1/events.md），test_events 的 hit.* 运行核对 |
| `kTargetCrit` | 0x11 | `kTargetCrit` | 运行时：ApplyHit FUNC |
| `MonsterRule 的 monsterClass[0xF97]` | 0xF97 | `kMonsterClassCanBeDamagedDirectly` | 运行时：MonsterBlockReturn 命中 WINDOW |
| `MonsterRule 的 type >= 20` | 20 | `kMonsterImmuneTypeCount` | 离线：monster_immune_table |
| `EffectApplyDetour：Effect+0 是名字` | 0 | `kEffectName` | 没有静态依据：4F02D0 把 .name 写进 Effect+0 的 char[64]；effect_get 和 effect.* 的 effect_name 运行核对（test_combat、test_events） |
| `EffectApplyDetour 的 memcpy(effectName, effect, 64)` | 64 | `kEffectNameCap` | 没有静态依据：4F02D0 把 .name 写进 Effect+0 的 char[64]；effect_get 和 effect.* 的 effect_name 运行核对（test_combat、test_events） |
| `VecLast 的 ReadPtr(vec, 8)` | 8 | `kStdVectorLast` | 没有静态依据：MSVC std::vector 的 first/last/end，DDCore 读 vector 的地方都按这个布局（StdVectorSize 有依据）；工具链大版本变化才会变 |
| `类名表的 i * 0x80；game/game.cpp Query 类名表的 i * 0x80` | 0x80 | `kClassNameStride` | 运行时：EventClassNames 命中 WINDOW；QueryClassNames 命中 WINDOW |
| `HandleEventCallback 的 event + 8` | 8 | `kEventTime` | 没有静态依据：test_events |
| `LogListOrder / OrderSummary 的 listener + 8` | 8 | `kListenerPriority` | 没有静态依据：Listener 的 priority 在 +8（排序键）；运行时的「排序确认」日志读它，通用 listener 排在最前（ADR 0008） |
| `FakeListener：vtable、priority、pad 之后的 funcStorage` | 0x10 | `kListenerFunction` | 没有静态依据：Listener 的 std::function 在 +0x10，_Do_call 经实现指针调用（listener 析构 361730 里 lea rdi,[rbx+0x10]、[rdi+0x38]，但这段在离线版里没有唯一的锚点）；test_events 的运行核对 |
| `struct ListData（first、last、end、dirty、pad[7]）` | 0x20 | `kListDataSize` | 运行时：EventTypeCount 命中 WINDOW |
| `struct ListData 的 dirty` | 0x18 | `kListDataDirty` | 运行时：EventTypeCount 命中 WINDOW；ListenerAdd FUNC |
| `kDamageEpsilon` | 1e-6f | `std::bit_cast<float>(kDamageEpsilonBits)` | 运行时：HeroVtable 槽50 FUNC |
| NativeSources 的 48 个字段偏移和容量（`{"hero", Raw::Ptr, 0x10}` 等） | 各自 | `kEv<类>_<字段>`（`_cap` 是容量） | 没有静态依据：test_events 运行自检 |
| QueryDefs 的 22 个字段偏移 | 各自 | `kQy<类>_<字段>` | 没有静态依据：test_events 运行自检 |

**game/game.cpp**

| 原来的常量 | 值 | 清单项 | 依据 |
| --- | --- | --- | --- |
| `Query 类名表最多读 64 项` | 64 | `kQueryClassCapacity` | 运行时：QueryClassNames 命中 ENTRY |
| `MonsterBlockReturn 的 hit + 52` | 52 | `kMonsterBlockImmuneLeaDisp` | 运行时：MonsterBlockReturn 命中+常量 ENTRY |
| `MonsterBlockReturn 的 hit + 56` | 56 | `kMonsterBlockImmuneLeaNext` | 运行时：MonsterBlockReturn 命中+常量 ENTRY |

**game/phase_tracker.cpp**

| 原来的常量 | 值 | 清单项 | 依据 |
| --- | --- | --- | --- |
| `s.state != 0（bs_none）` | 0 | `kBattleStateNone` | 没有静态依据：Battle::Create 置 bs_create（1）之前、撤退时（5A93B3）状态是 0；knowledge/dd1/combat.md 的「Battle」，战斗序号的单元测试和 P3c 探针 |

**persist/persist.cpp**

| 原来的常量 | 值 | 清单项 | 依据 |
| --- | --- | --- | --- |
| `kSmSlot5` | 0x38 | `kStorageProfileSlot` | 运行时：StorageManager 命中 ENTRY |
| `kDeviceName` | 0x20 | `kStorageDeviceName` | 运行时：StorageManager 命中 ENTRY |
| `kDeviceNameCap` | 0x80 | `kStorageDeviceNameCap` | 没有静态依据：设备名的字段宽度 char[0x80]（knowledge/dd1/campaign.md 的「存档」）；读的时候在宽度内截断，P4 场景的运行证据 |

**game/mods.cpp**

| 原来的常量 | 值 | 清单项 | 依据 |
| --- | --- | --- | --- |
| `kUgcIdSize` | 0xC0 | `kUgcIdSize` | 没有静态依据：启用列表的键名自检（applied_ugcs_1_0）和 P3 探针 probe_p3 的运行核对 |
| `kUgcIdSource` | 0x80 | `kUgcIdSource` | 没有静态依据：启用列表的键名自检（applied_ugcs_1_0）和 P3 探针 probe_p3 的运行核对 |
| `kListKeyOffset` | 0x200 | `kUgcListKeyOffset` | 没有静态依据：读的时候核对键名是 applied_ugcs_1_0，不对就报结构变化、不给列表 |
| `kInstalledVector` | 0x20 | `kUserServicesInstalled` | 没有静态依据：P3 探针 probe_p3 的运行核对；vector 不合法或某项读不出时报错、不给列表 |
| `kUgcPath` | 0x20 | `kUgcPath` | 没有静态依据：P3 探针 probe_p3 的运行核对；目录读不出时按没找到处理 |
| `kUgcId` | 0x620 | `kUgcObjectId` | 没有静态依据：P3 探针 probe_p3 的运行核对；读出的 name/source 要和启用列表的项相等才算找到 |
| `键名的 ReadFixedString(.., 0x200, ..)` | 0x200 | `kUgcListKeyCap` | 没有静态依据：读的时候核对键名是 applied_ugcs_1_0，不对就报结构变化、不给列表 |
| `name 的 ReadFixedString(.., 0x80, ..)` | 0x80 | `kUgcIdNameCap` | 没有静态依据：启用列表的键名自检（applied_ugcs_1_0）和 P3 探针 probe_p3 的运行核对 |
| `source 的 ReadFixedString(.., 0x40, ..)` | 0x40 | `kUgcIdSourceCap` | 没有静态依据：启用列表的键名自检（applied_ugcs_1_0）和 P3 探针 probe_p3 的运行核对 |
| `目录的 ReadFixedString(.., 0x200, ..)` | 0x200 | `kUgcPathCap` | 没有静态依据：P3 探针 probe_p3 的运行核对；目录读不出时按没找到处理 |

**combat/logic.h**

| 原来的常量 | 值 | 清单项 | 依据 |
| --- | --- | --- | --- |
| `kTextMax` | 63 | `kGameTextCap - 1` | 没有静态依据：Instance 的 sub_type（+0x04）到它的哈希（+0x44）、id（+0x61）到哈希（+0xA4）都隔 0x40 字节（knowledge/dd1/combat.md 的「Buff」） |
| `kEffectBlockMax` | 0x3FF | `kEffectBlockMax` | 没有静态依据：2904D0 的 strncpy_s 缓冲大小 0x400 减 1（knowledge/dd1/combat.md 的「Effect」），更新后对照核对 |

**combat/logic.cpp**

| 原来的常量 | 值 | 清单项 | 依据 |
| --- | --- | --- | --- |
| `GameHash 的乘数` | 53 | `kGameHashMul` | 没有静态依据：游戏的哈希函数（knowledge/dd1/combat.md）；查 Buff 库和 effect map 的运行核对（test_combat）对不上时全部找不到 |
| `kLists 的 .kill_enemy_types` | 4 | `kEffectListKillEnemyTypes` | 没有静态依据：4F02D0 的反编译（knowledge/dd1/combat.md 的「Effect」），更新后对照核对 |
| `kLists 的 .buff_ids` | 8 | `kEffectListBuffIds` | 没有静态依据：4F02D0 的反编译（knowledge/dd1/combat.md 的「Effect」），更新后对照核对 |
| `kLists 的 .summon_monsters` | 4 | `kEffectListSummonMonsters` | 没有静态依据：4F02D0 的反编译（knowledge/dd1/combat.md 的「Effect」），更新后对照核对 |
| `kLists 的 .summon_ranks` | 4 | `kEffectListSummonRanks` | 没有静态依据：4F02D0 的反编译（knowledge/dd1/combat.md 的「Effect」），更新后对照核对 |
| `kLists 的 .set_monster_class_ids` | 8 | `kEffectListSetMonsterClassIds` | 没有静态依据：4F02D0 的反编译（knowledge/dd1/combat.md 的「Effect」），更新后对照核对 |
| `ValidateSubStat 的 case 2` | 2 | `IndexOf(kEnumStatNames, 'combat_stat_add')` | 由 EnumStat 的名字表按名字 combat_stat_add 取下标（表本身离线核对） |
| `ValidateSubStat 的 case 3` | 3 | `IndexOf(kEnumStatNames, 'combat_stat_multiply')` | 由 EnumStat 的名字表按名字 combat_stat_multiply 取下标（表本身离线核对） |
| `ValidateSubStat 的 case 11` | 11 | `IndexOf(kEnumStatNames, 'resistance')` | 由 EnumStat 的名字表按名字 resistance 取下标（表本身离线核对） |
| `ValidateSubStat 的 case 32` | 32 | `IndexOf(kEnumStatNames, 'activity_side_effect_chance')` | 由 EnumStat 的名字表按名字 activity_side_effect_chance 取下标（表本身离线核对） |
| `ValidateSubStat 的 case 33` | 33 | `IndexOf(kEnumStatNames, 'disable_combat_skill_attribute')` | 由 EnumStat 的名字表按名字 disable_combat_skill_attribute 取下标（表本身离线核对） |
| `ValidateSubStat 的 case 9` | 9 | `IndexOf(kEnumStatNames, 'stress_dmg_percent')` | 由 EnumStat 的名字表按名字 stress_dmg_percent 取下标（表本身离线核对） |
| `ValidateSubStat 的 case 10` | 10 | `IndexOf(kEnumStatNames, 'stress_heal_percent')` | 由 EnumStat 的名字表按名字 stress_heal_percent 取下标（表本身离线核对） |
| `ValidateSubStat 的 case 29` | 29 | `IndexOf(kEnumStatNames, 'stress_heal_received_percent')` | 由 EnumStat 的名字表按名字 stress_heal_received_percent 取下标（表本身离线核对） |
| `ValidateSubStat 的 case 30` | 30 | `IndexOf(kEnumStatNames, 'stress_dmg_received_percent')` | 由 EnumStat 的名字表按名字 stress_dmg_received_percent 取下标（表本身离线核对） |
| `ValidateSubStat 的 case 6` | 6 | `IndexOf(kEnumStatNames, 'hp_heal_amount')` | 由 EnumStat 的名字表按名字 hp_heal_amount 取下标（表本身离线核对） |
| `ValidateSubStat 的 case 7` | 7 | `IndexOf(kEnumStatNames, 'hp_heal_percent')` | 由 EnumStat 的名字表按名字 hp_heal_percent 取下标（表本身离线核对） |
| `ValidateSubStat 的 case 8` | 8 | `IndexOf(kEnumStatNames, 'hp_heal_received_percent')` | 由 EnumStat 的名字表按名字 hp_heal_received_percent 取下标（表本身离线核对） |
| `ValidateSubStat 的 case 98` | 98 | `IndexOf(kEnumStatNames, 'hp_heal_amount_received')` | 由 EnumStat 的名字表按名字 hp_heal_amount_received 取下标（表本身离线核对） |
| `kStatNames[]` | 表 | `kEnumStatNames` | 离线：enum_stat |
| `kSourceNames[]` | 表 | `kEnumSourceNames` | 离线：enum_source |
| `kDurationNames[]` | 表 | `kEnumDurationNames` | 离线：enum_duration |
| `kRuleNames[]` | 表 | `kEnumRuleNames` | 离线：enum_rule |
| `kCombatStatNames[]` | 表 | `kSubStatCombatNames` | 没有静态依据：游戏的名字表（静态初始化里填，注释里是 Steam 版地址），knowledge/dd1/combat.md 的「Buff」；更新后对照 Ghidra 或 dd_disasm 的 s 命令核对 |
| `kResistanceNames[]` | 表 | `kSubStatResistanceNames` | 没有静态依据：游戏的名字表（静态初始化里填，注释里是 Steam 版地址），knowledge/dd1/combat.md 的「Buff」；更新后对照 Ghidra 或 dd_disasm 的 s 命令核对 |
| `kSideEffectNames[]` | 表 | `kSubStatSideEffectNames` | 没有静态依据：游戏的名字表（静态初始化里填，注释里是 Steam 版地址），knowledge/dd1/combat.md 的「Buff」；更新后对照 Ghidra 或 dd_disasm 的 s 命令核对 |
| `kSkillAttributeNames[]` | 表 | `kSubStatSkillAttributeNames` | 没有静态依据：游戏的名字表（静态初始化里填，注释里是 Steam 版地址），knowledge/dd1/combat.md 的「Buff」；更新后对照 Ghidra 或 dd_disasm 的 s 命令核对 |
| `kStressSourceNames[]` | 表 | `kSubStatStressSourceNames` | 没有静态依据：游戏的名字表（静态初始化里填，注释里是 Steam 版地址），knowledge/dd1/combat.md 的「Buff」；更新后对照 Ghidra 或 dd_disasm 的 s 命令核对 |
| `kHealSourceNames[]` | 表 | `kSubStatHealSourceNames` | 没有静态依据：游戏的名字表（静态初始化里填，注释里是 Steam 版地址），knowledge/dd1/combat.md 的「Buff」；更新后对照 Ghidra 或 dd_disasm 的 s 命令核对 |
| `kBuiltinStatus[]` | 表 | `kBuiltinStatusNames` | 没有静态依据：479CD0（BuffRuleActive）里按这些名字的哈希比较的立即数，更新后在 dd_disasm 里核对 |
| `kTargets[]` | 表 | `kEffectTargetsNames` | 没有静态依据：游戏的 EffectTarget 名字表（2AD58F0，静态初始化 9C560），更新后对照核对 |
| `kFlags[]（SourceFlags 函数里）` | 表 | `kSourceFlagsValues` | 离线：source_flags |

### 4.1 不算游戏布局的常量

| 文件 | 常量 | 为什么不算 |
| --- | --- | --- |
| game/actors.h | `kMaxRosterEntries` 4096、`kMaxBattleActors` 64、`kMaxPartyHeroes` 16 | 读 vector 时的合理性上限，防读到垃圾；不对应游戏结构 |
| combat/combat.cpp | `kMaxEntries` 4096、`kAddMaxEntries` 1024、`kQueueLimit` 256、`kEffectTextMax` 64 KB、技能表 `< 256`、`> 64` | 合理性上限和我们自己的 API 限制 |
| combat/combat.cpp | `src < 0x10000`、`char text[16]`、`char text[256]`、`RtlCaptureStackBackTrace(1, 24, ..)` | 空指针页保护、本地缓冲、调用栈深度 |
| combat/combat.cpp | `/ 8`、`i * 8`（`vector<Effect*>`）、`/ 4`（`vector<uint32>`） | 指针和 uint32 的宽度（x64） |
| combat/combat.cpp | `RemoveReason` 里的 `info[0] >> 3`、`info[2]`、`UNW_FLAG_CHAININFO`、`info + 4 + codes * 2` | Windows x64 的 unwind 格式（操作系统 ABI） |
| combat/combat.cpp、events/system.cpp | `"arena_mp"`、`ReadBytes(name, text, 9)` | 游戏数据（模式名）的字符串，不是二进制布局；`RaidPvpName` 偏移本身登记了 |
| combat/logic.cpp | `0x20`（控制字符）、`0x80`（非 ASCII）、`kKnown`（SDK 的 flags）、`"bsrc_"`、`".name"`、`".target"`、`"effect:"` | 文本校验、SDK ABI、游戏数据文件的格式字符串 |
| events/system.cpp | `kMaxFields` 32、`INT_MIN`/`INT_MAX` 优先级、日志缓冲 | 我们自己的限制和取值 |
| events/system.cpp | RTTI：COL 签名 1、`col[3]`/`col[5]`、TD 名字 +0x10、BCD 0x1C、CHD 0x10、COL 0x18、`tdRva < 0x1000`、`kRttiPageSize` 0x20000、`0x7F000000` | MSVC x64 的 RTTI ABI 和我们的分配策略；按 RTTI 名查不到时主题不可用，自带校验 |
| events/system.cpp | `".?AV?$Listener@$$CBV"` 等 RTTI 名前缀、`"native.BattleDestroyedEvent"` | 游戏类名和我们的主题名，查不到时自动不可用 |
| events/system.cpp | `VecFirst` 的 `ReadPtr(vec, 0)` | vector 的 first 就是对象起点，和 `StdVectorLast`、`StdVectorSize` 同一组，没有单独登记 |
| persist/persist.cpp | `kFlushMs`、`kExitFlushMs`、`kVerifyWaitFrames`、`kArchiveKeep`、`kSaveLogFirst`、`kLogFirst`、`path[0x110]`、`id & 0xFFFFFFFF`、`SteamAPI_SteamUser_v%03d` 40..15 | 我们的策略、本地缓冲、Steam SDK 的导出名 |
| persist/store.h | `ArgU32`（2487E0 第 6 个参数、248D60 第 5 个参数只有低 32 位有效） | 被 hook 函数的参数约定。函数原型没有登记成清单项，靠特征码的同一性检查（指纹）间接覆盖，见第 9 节 |
| persist/store.* | `kRosterFile`、`kExtension`、`kFormat`、`kMaxObserved`、FNV 常量 | 我们的旁路文件格式 |
| game/mods.cpp | `kMaxEntries` 8192、`buffer[0x200]`（加了 `static_assert` 不小于登记的容量）、`"applied_ugcs_1_0"` | 合理性上限、本地缓冲、运行时自检用的键名 |
| game/game.cpp | 状态名表最多读 64 项、`"as_"` 前缀、`as_load_campaign` 等状态名、`"Query"` 后缀 | 运行时按内容自检、按名字查，不是写死的布局 |
| hook/engine.cpp、hook/prologue.cpp | `E9`、`FF 25`、`kPoolSize`、`kReach`、操作码表 | x86 编码 |
| loader/early_hook.h | `AllocNear` 的 `0x7FFF0000`、跳板的 `FF 25`/`E9`、页内偏移 0x20/0x40 | x86 编码和跳板布局；`CreateAppSystems` 的入口字节改成读 `DD_ENTRY` |
| common/*.h | PE 解析、`length < 0x40000000` | PE 格式和文件大小上限 |

### 4.2 统计

- 清单 227 项：OFFSET 171、SIZE 24、TABLE 13、COUNT 9、SLOT 7、CONST 2、ENUM 1；REQUIRED 8（类名表步长和 listener 列表结构，事件桥是 DDCore 的必需部分）、OPT 219。
- 依据：运行时依据 106 条（其中 2 条是特征码的同一性依据），82 项有运行时依据；离线检查 17 个（支撑 34 项次）；没有静态依据 124 项：70 个事件和 Query 负载字段加 `EventTime`（简报指定靠 test_events 自检），其余 53 项写了核实办法（技能表 9 项只有 P4-0C 探针的运行证据，MSVC 标准库布局 4 项，Mod 列表 10 项靠键名自检和探针，sub_stat 等游戏名字表 8 张等）。
- 特征码 60 条：REQUIRED 9、LOADER 1、OPT 49、ANCHOR 1，每条都有 `DD_DEPS`。

## 5. 原两个脚本的检查覆盖（A2）

`check_sigs.py` 的全部检查在 `tools/ddupdate/sigs.py`，逻辑逐条照搬：唯一命中、GLOBAL_ANY 多处命中解析到同一地址、`DD_RUNTIME_SELECT` 允许多个候选、地址在映像内、必需的 GLOBAL 指向 `.data/.rdata`、HOOK 偷取范围在指令边界且不含 rip 相对和分支、函数内没有分支跳进偷取范围、`DD_ENTRY` 和实际字节一致且长度等于 steal、Steam 版上和 `hooks.md` 清单不重叠（`DD_CHAIN_TARGET` 只记一笔）。两处有意的差别：命中计数改成可重叠（和 C++ 的 `ScanPattern` 一致，原脚本用不重叠的 `finditer`）；可选特征码缺失由「WARN、退出码 0」改成退出码 1（简报要求区分「只有可选项失效」）。

`check_actor_layout.py` 的 33 项检查：

| 原检查 | 现在 |
| --- | --- |
| Hero / Monster 虚表特征码唯一 | 特征码检查 |
| Hero 槽 3 返回 this、Monster 槽 3 返回 0 | `HeroVtable`、`MonsterVtable` 的同一性依据（VSLOT 3 ENTRY） |
| 槽 5、6 的 id、guid、职业指针 | `HeroId`、`MonsterGuid`、`HeroClass`、`MonsterClass` 的运行时依据（VSLOT ENTRY，读位移） |
| Hero 槽 37 读写 +0x12FC | `ActorStress`：运行时依据（VSLOT 37 FUNC）+ 离线 `hero_stress_slot` |
| ActorMaxStat 唯一 | 特征码检查 |
| (Actor+0x768, [Actor+0x790], true) 的调用都指向 ActorMaxStat | 离线 `maxstat_calls` + 运行时依据（槽 50 里的这次调用） |
| 名册分支 +0x728、+0x20/+0x28 | `RosterAlt`、`RosterHeroes` 的运行时依据（TEXT，常量改成通配后读值） |
| 战斗销毁清空 Battle+0x38/+0x40 | `BattleMonsters` 的运行时依据（TEXT） |
| guid 计数 inc [Raid+0x4B6C] 唯一 | `BattleGuidCounter` 的运行时依据（TEXT，读位移，另一条用 REL32 核对基址来自 `Raid::Current`） |
| Battle::Create 先清零再置 bs_create | 离线 `battle_create_order` |
| 代码里有 lea [reg+0x4898] | 离线 `raid_battle_lea` |
| 队伍 +8/+0x10、怪物表 +0x48D0/+0x48D8 在同一个函数、基址是 Raid::Current | `RaidHeroes`、`RaidBattle` 的运行时依据 + 离线 `party_monster_targets` |
| HeroClass+8、MonsterClass+8 是职业 id | 离线 `class_id_strings` |
| 四张枚举表 | 离线 `enum_stat`、`enum_source`、`enum_duration`、`enum_rule`（名字改从 `DD_NAMES` 取，不再解析 logic.cpp） |
| 怪物免疫表 | 离线 `monster_immune_table` |
| 来源表 +0x44/+0x46/+0x48 | 离线 `source_flags`（期望值从 `DD_VALUES(SourceFlags)` 取） |
| 槽 13 读写 +0x478、传 +0x460 | `ActorNextGuid`、`ActorBuffGroup`、`SlotAddBuff` 的运行时依据 |
| erase 按 0x418 前移 | `BuffEntrySize` 的运行时依据 |
| 复制构造复制 +0x158、+0x3B0、+0x408 | `InstAdditional`、`InstClearOnRule` 的运行时依据 + 离线 `instance_copy_fields` |
| 构造：duration = -1、duration_type = 9 | 新的锚点 `BuffInstanceCtor` 上的运行时依据 + 离线 `instance_ctor_defaults` |
| IsRuleActive 读 +0x5C、+0x60 | `InstRule`、`InstFalseRule` 的运行时依据 |
| BuffDurationUpkeep 读写 +0x54、+0x58 | `InstDuration`、`InstDurationType`、`BuffEntryInstance` 的运行时依据（rel 关联） |
| Effect 字段按读取函数分组 | 离线 `effect_field_types` |
| Effect .target、.swap、.buff_duration_type | 离线 `effect_misc_fields`（`EffectSwap` 另有运行时依据） |
| 技能 effect 列表 +0x3C8 | 离线 `skill_effects_offset` |

两个旧脚本改成了薄封装：`check_sigs.py` = `verify --only sigs,identity`，`check_actor_layout.py` = `verify --only layout`，参数原样转发（`check_sigs_wrapper.txt`、`check_actor_layout_wrapper.txt`，退出码 0）。`docs/06-workflow.md` 的工具表换成了 `dd_update.py` 的 verify、baseline、suggest、simstats 四行。

## 6. verify（A2）

命令：`python -B DD.ModLoader/tools/dd_update.py verify [exe...] [--json 文件] [--only sigs,identity,layout] [--sigs 文件 [--layout 文件]] [--baseline 文件...]`。不带 exe 时检查 Steam 版和离线版。

- 先解析清单并做清单检查（宏参数、依赖表词汇、check 和依据一致、TABLE 项数、读取宽度和期望值范围、rel 和锚点存在、`DD_RUNTIME_SELECT` 只能当 HIT 起点等），有问题直接退出码 3（`broken_layout.inc` 试过：`HeroId 的依据 0：rel NOSUCH 不存在`，退出码 3）。
- 特征码：第 5 节的全部检查。
- 同一性：和基线比较，判为误绑定的条件任意一条成立（`verify.py` 开头有说明）：指纹相似度低于 0.85；基线里有直接调用者而现在没有（复制品、诱饵没人调用）；和其他特征码的关系（调用了哪些特征码的函数、引用了哪些特征码的全局，只算两边都解析成功的）Jaccard 低于 0.5；特征码的同一性依据不成立。高于 0.5 但不完全一致只记一笔。有本 SHA1 的基线时只和它比，否则和所有基线比、取最像的。GLOBAL_ANY 只比较候选数。
- 布局：每项的全部运行时依据和离线检查都求值；没有静态依据的项按说明分组单列。
- 失效影响：每个失效的特征码、误绑定、失效的清单项按依赖表汇总到服务和主题。
- 退出码：0 全部通过；1 只有可选项失效；2 必需项失效或有误绑定；3 清单或参数有错、exe 不存在。多个 exe 取最大值。
- JSON（`--json`）：`{tool, manifest{signatures, layout, sigs, items, evidence}, similarity_threshold, relation_threshold, exes: [{path, sha1, known, baselines, sigs{id: {kind, need, status, hits, hit, resolved, section, candidates, problems, notes, deps}}, identity{id: {status, baseline, score, target_score, relation_score, callers, base_callers, strings, relations, evidence, problems, notes, scores}}, layout{id: {type, value, need, check, deps, status, evidence[{describe, status, origin, hit, hits, captured, expected, implied, detail}], offline[{name, ok, detail}], unverified, problems}}, failures[{name, severity, why}], affected{词汇: [项]}, unverified[], exit_code, result}], exit_code}`。

### 6.1 Steam 版和离线版

`verify.txt`（自身基线）、`verify_nosteam_vs_steam.txt` 和 `verify_steam_vs_nosteam.txt`（只给另一个版本的基线，验证跨版本的同一性检查不误报），三次都是退出码 0：

```
清单：signatures.inc 60 条特征码（LOADER 1、REQUIRED 9、OPT 49、ANCHOR 1）；layout.inc 227 项（RUNTIME 82、NONE 124、OFFLINE 21），依据 106 条
== steam ...\_windows\win64\Darkest.exe
   sha1=9db2d73b89aa034e2055df65a139403e18df641f 已知版本 steam；基线 nosteam.json, steam.json
 特征码
  OK   CreateAppSystems         rva=3E9320 steal=5 chunks=1
  ...（60 条全部 OK）
 同一性（指纹相似度阈值 0.85，关系一致度阈值 0.5）
  OK   UpdateStateInternal      1.000（基线 steam，关系 1.00，字符串 1.00，调用者 1（基线 1））
  OK   HeroVtable               1.000（基线 steam，关系 1.00，字符串 1.00）；同一性依据 1/1
  ...
 布局
  OK   RosterAlt                    = 0x728  运行时 1/1
  OK   BattleGuidCounter            = 0x2d4  运行时 2/2，离线 1/1
  OK   ActorStress                  = 0x12fc  运行时 1/1，离线 1/1
  ...（103 项全部 OK）
 没有静态依据（更新后要人工或靠自检核实）：124 项（按说明分组列出）
 结论：OK（全部通过）（退出码 0）
== nosteam ...（同上，退出码 0）
```

离线版对 Steam 版基线时不完全一致、只记一笔的项（`verify_nosteam_vs_steam.txt`）：`UpdateStateInternal`、`StateLast/Next` 0.979，`StateNames` 0.929 且关系一致度 0.62（Steam 版这个函数里内联了一段 listener 注册，离线版没有，已写进 `knowledge/dd1/events.md`），`CombatStartTurn` 0.991，`EffectApplyInternal` 0.993，`BuffRuleActive` 0.991，`AppShutdown` 0.931，其余 1.000。

### 6.2 手工破坏的冒烟（`mut/`）

样本放在 guest 的 `build/u1a/mut/`，没有运行：

- 样本 A `Darkest_layout.exe`：Steam 版的 Hero 虚表槽 5（`4A1200`，`mov eax,[rcx+0x137C]`）位移改成 0x1380，模拟 Hero 加字段、id 后移。
- 样本 B `Darkest_layout_sig.exe`：在 A 的基础上把 `UpdateStateInternal`（必需）的 `sub rsp,0x3E8` 改成 0x3F0（`3DCE25`），模拟编译器换了栈帧大小。

```
样本 A：
  WARN HeroId                       = 0x137c  运行时 0/1
         依据 0（HeroVtable 虚表槽 5，起点处匹配 8B 81 ?? ?? ?? ?? C3）：读到 0x1380，期望 0x137c（按读到的值，常量应为 0x1380）
 失效影响
  game.actor             ← HeroId
  topic:buff.*           ← HeroId
 结论：只有可选项失效（退出码 1）
样本 B：
  FAIL UpdateStateInternal      0 次命中 []，要求唯一
  WARN HeroId ...（同上）
 失效影响
  core                   ← UpdateStateInternal
  game.actor             ← HeroId
  topic:buff.*           ← HeroId
 结论：必需项失效或有误绑定（退出码 2）
```

### 6.3 `--sigs`

`build/u1a/plugin/my_sigs.inc` 是一个插件作者式的文件（三条：一个 HOOK 带 `DD_ENTRY`、一个必需的 GLOBAL、一个故意失效的可选 CODE，不写 `DD_DEPS`）：`verify --sigs` 报出 `MyStale 0 次命中`，退出码 1；`baseline --sigs ... --out plugin/baseline` 生成了插件自己的基线（版本名沿用主清单的 `DD_KNOWN_BUILD`）；`verify --sigs ... --baseline plugin/baseline/steam.json <离线版>` 的同一性两条都是 1.000（`plugin/verify_identity.txt`）。

## 7. 基线和相似度（A3）

命令 `python -B DD.ModLoader/tools/dd_update.py baseline`，写 `tools/baseline/steam.json`（292144 字节）、`nosteam.json`（290311 字节）。内容（`baseline.py` 开头有完整说明）：每条特征码的 kind、pattern、hit、resolved、candidates，命中的函数（GLOBAL 是命中位置所在的函数，指向 `.text` 时另存目标函数）的指纹：分块（异常目录）、大小、规范化指令序列（存成词表下标）、callees、imports、strings、globals、有区分度的立即数和位移、thunk 目标；HOOK/CODE 的直接调用者；和其他特征码的关系；每条运行时依据的命中位置（起点、命中 RVA、所在函数、函数内偏移、指令下标、读到的值）。游戏更新后旧 exe 通常被覆盖，所以基线存完整指纹而不是摘要，修复只靠基线文件也能做指令对齐。

可复现：同一个 exe 两次生成（第二次写到 `build/u1a/baseline_run2/`）sha256 相同、`cmp` 一致（`baseline_sha256.txt`）：

```
5830e69419b8d8a375cfe874f52d0225dfd5e43d0301c65ef9f668a183df5ab2  tools/baseline/nosteam.json
657295479ab985df0b0ef3672c05f55bc18586d1d09823446f58ef0b273734c7  tools/baseline/steam.json
（baseline_run2 下两个文件的 sha256 相同）
两次生成字节一致
```

为了在不同机器检出后仍然字节一致，`.gitattributes` 加了 `tools/baseline/*.json text eol=lf`。

**相似度度量**：规范化 token 是「助记符 + 操作数的种类和宽度」（寄存器 `r<位宽>`、内存 `m<位宽>`、rip 相对 `mr<位宽>`、立即数 `i`、分支目标 `L`），不含寄存器编号、位移、立即数的值；相似度是 token 3-gram 多重集的 Dice 系数。`simstats` 的数据（`simstats.txt`、`simstats.json`，A 是 Steam 版、B 是离线版）：

| 分布 | n | 最小 | p05 | 中位 | p95 | 最大 |
| --- | --- | --- | --- | --- | --- | --- |
| 同一函数跨版本 | 57 | 0.929 | 0.979 | 1.000 | 1.000 | 1.000 |
| 最近冒充者（B 的函数对 A 里所有其他大小相近的函数取最像的） | 56 | 0.158 | 0.211 | 0.362 | 0.804 | 1.000 |
| 随机不同函数对（大小相近） | 5000 | 0 | 0 | 0.059 | 1.000 | 1.000 |
| 随机不同函数对（两边都不少于 30 个 3-gram） | 534 | 0 | 0 | 0.049 | 0.190 | 0.361 |

阈值取 0.85：同一函数全部不低于 0.929；大函数的随机对最高 0.361；最近冒充者 53/56 低于 0.81。高于阈值的三个冒充者是模板孪生：`VectorDtor64`（`2345D0` 和 `235770` 规范化后完全相同）、`FreeU32Vector`（`243C40` 和 `233DE0`）、`BuffLibraryIndex`（`4ACFA0` 和 `748190`，0.94），随机对里高于阈值的 799 对全是小函数（模板实例、thunk）。所以指纹只能排除「绑到了形状不同的函数」，对完全相同的模板实例无能为力，这类误绑定靠特征码本身的位移和立即数、调用者检查和依据去区分（已写进 `knowledge/dd1/combat.md`）。关系一致度的阈值 0.5 只有一个跨版本数据点（`StateNames` 0.62），其余全是 1.00，数据很薄，U1d 的变异样本要再验证。

## 8. 改动文件

新增：
- `core/src/game/layout.inc`、`core/src/game/layout.h`
- `tests/unit/layout_golden.cpp`
- `tools/dd_update.py`、`tools/ddupdate/`（`__init__`、`manifest`、`image`、`sigs`、`evidence`、`offline`、`fingerprint`、`baseline`、`verify`、`simstats`、`suggest`）
- `tools/baseline/steam.json`、`tools/baseline/nosteam.json`
- `docs/u1/u1a-report.md`

修改：
- `core/src/game/signatures.inc`：头部说明、`ANCHOR`、`DD_DEPS`（60 条）、`DD_ENTRY(CreateAppSystems)`、锚点 `BuffInstanceCtor`，特征码本身没改
- `core/src/game/game.cpp`：编译期排除 ANCHOR；`+52/+56`、Query 类名表的 64 和 0x80 改从清单取
- `core/src/game/actors.h`：`actor_layout` 改成对清单的 using 声明，只留合理性上限
- `core/src/game/mods.cpp`、`core/src/game/phase_tracker.cpp`、`core/src/persist/persist.cpp`
- `core/src/combat/combat.cpp`：`layout` 命名空间删掉，`kEntrySize` 等改名为清单名，字面量（map 根、EhVectorDtor 参数、std::function、ApplyEffect 结果、规则类型 13/42、vector 步长）改从清单取，加 `static_assert`
- `core/src/combat/logic.cpp`、`logic.h`：枚举表、来源标志、sub_stat 表、内置状态、`.target` 表、列表项数、哈希乘数、`kTextMax`、`kEffectBlockMax` 改从清单取；`ValidateSubStat` 的 case 改成按名字取下标
- `core/src/events/system.cpp`：负载偏移、actor 字段、虚表槽位、listener 结构（`ListData`、`FakeListener` 加 `static_assert`）、类名表步长、免疫判断、epsilon、Effect 字段、命中结算参数改从清单取
- `loader/early_hook.h`：入口字节改读 `DD_ENTRY`
- `core/xmake.lua`（headerfiles 加 layout.inc）、`tests/xmake.lua`（dd_core_tests 加 layout_golden.cpp）
- `tools/check_sigs.py`、`tools/check_actor_layout.py`：薄封装
- `docs/06-workflow.md`：工具表
- `.gitattributes`：基线 JSON 固定 LF
- `knowledge/dd1/combat.md`（模板孪生、`4ACFA0` 的节点大小）、`knowledge/dd1/events.md`（`StateNames` 所在函数的版本差异），都带 [静态]

## 9. 没做到的事、失败的尝试、已知局限

- **124 项没有静态依据**。事件和 Query 负载 70 项、`EventTime` 按简报靠 test_events；其余 53 项写了核实办法。其中 2 项是 REQUIRED（`ListenerPriority`、`ListenerFunction`），`ListenerFunction` 还连带 OPT 的 `StdFunctionImpl`：想用 listener 析构（`361730`）里的 `lea rdi,[rbx+0x10]` / `[rdi+0x38]` 做依据，但离线版里 `EventTypeCount` 最低的命中落在同一模板的另一个实例上，析构的尾部不在窗口里，换成 TEXT 范围在 Steam 版有两处命中，没找到两个版本都唯一的锚点，撤回了。U1b 定运行时策略时要注意：事件桥是必需部分，这几项在未知版本上不能核对。
- **`VtSlotStress`（槽 36）**没有依据：`4814B0` 不直接读压力字段。`VtSlotHeal`（槽 51）的依据是「函数里有取最大生命的调用」，槽 50 也有同样的调用；槽 50 有「调用 ConsumeHealthDamageBlock」的依据，两者合起来能发现槽位整体平移，但发现不了 50、51 互换。
- **同一性检查对模板孪生无效**（第 7 节）。修复工具的指纹搜索也会在这里遇到多个同分候选，要按「需要人工」处理。
- **函数原型（参数个数和宽度）没有登记成清单项**，例如 `store.h` 的 `ArgU32`。函数签名一旦变化，现在只能靠指纹相似度间接发现。
- `--only identity` 时 GLOBAL_ANY 只比较候选数；事件类名表、Query 类名表在文件里是空的，`QueryClassNames` 的按内容选择离线做不了（和原 `check_sigs.py` 一样接受多个候选）。
- 运行时依据里有 10 条 TEXT 范围、几条较短的 FUNC 模式（例如 `48 81 C1 ?? ?? ?? ??`、`49 8B 46 ??`），在当前两个版本上唯一，但对新版本的鲁棒性一般；`suggest` 生成的模式偏短，以后可以加「至少几条指令」的选项。
- `02-architecture.md`、ADR 0006、`knowledge/README.md` 和若干知识库条目里还写着 `check_sigs.py` / `check_actor_layout.py`（封装仍可用），按简报留给 U1d 的文档整理。
- 二进制对比不是字节一致（第 2 节），以编译期断言为证据。
- 用 Bash 工具的 heredoc 跑带反斜杠的 Python 片段时多次被截断，改成先写脚本文件再运行；对产物没有影响。

## 10. 留给后面的事项

U1b：
- 按 3.2 实现 C++ 求值器（异常目录解析、五种起点、四种范围、读取、比较），`ANCHOR` 特征码开始解析；把依赖表词汇映射到停用开关；对照测试用 `verify.json` 里每条依据的 `status`、`hit`、`captured` 逐项比较。
- 未知版本策略里处理「REQUIRED 但不能运行时核对」的 2 项和 `StdFunctionImpl`（第 9 节）。
- `layout.h` 里 `DD_EVIDENCE` 等宏现在展开成空，U1b 自己包含 `layout.inc` 生成依据表（`layout.h` 结尾已经 `#undef` 了全部宏，可以重新定义）。

U1c：
- 接口见 3.3；修复后的常量值用 `EvidenceResult.implied`；基线里每条依据的 `insn_index`、函数的 token 序列可以直接做指令对齐。
- 基线里有 `callers`（HOOK/CODE）和 `relations`，调用图传播可以从这里起步；`CallIndex` 能在新 exe 上重建调用者。

U1d：
- 关系一致度阈值、指纹阈值要用变异样本再验证；模板孪生的误绑定样本预期报「需要人工」或靠调用者检查，不应该被当作通过。
- 文档里旧脚本名的替换、`06-workflow.md` 的「游戏更新后」手册。
