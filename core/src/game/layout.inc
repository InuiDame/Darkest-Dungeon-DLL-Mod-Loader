// 游戏布局清单：DDCore 和加载器用到的全部游戏布局常量的唯一来源（地址在 signatures.inc）。
// C++ 经 layout.h 取常量（layout::k<id>），tools/dd_update.py verify 逐项求值依据，DDCore 的运行时核对（U1b）用同一份
// 依据和依赖表，修复工具（U1c）按依据重新定位。格式的语义在 U1a 定下，之后只加项，不改含义。
//
// ---- 清单项 ----
// DD_LAYOUT(id, type, value, need, check, "deps")
//   id     C++ 里是 layout::k<id>；不能和 signatures.inc 的特征码同名
//   type   OFFSET 结构内偏移或命中位置上的固定偏移    SIZE 结构、元素或缓冲的大小    SLOT 虚表槽号
//          ENUM 枚举数值    COUNT 元素个数    CONST 代码里的其他常量（浮点数写位模式）
//          TABLE 表，value 是项数，内容在 DD_NAMES（名字表）或 DD_VALUES（整数表）
//          C++ 类型：ENUM 是 int32_t，CONST 是 uint32_t，其余 size_t
//   value  整数字面量
//   need   REQUIRED 失效时 DDCore 整体停用（依赖表里必须有 core）；OPT 只停用依赖它的服务和主题
//   check  RUNTIME 至少有一条 DD_EVIDENCE，运行时可以核对（也可以另有 DD_OFFLINE，verify 一并求值）
//          OFFLINE 只有 DD_OFFLINE（只能离线核对），运行时按不能核对处理
//          NONE    没有静态依据，必须有 DD_UNVERIFIED 写明怎么核实（自检、探针、人工）
//   deps   依赖表：这一项失效时受影响的服务和主题，空格分隔。词汇：
//            core             DDCore 整体（ADR 0006 的必需项）       loader         加载器的早期 hook
//            events.native    原生事件桥（native.* 和映射的主题）    events.query   Query 桥（query.*）
//            pvp              PvP 判定（所有 .pre 主题、Query 写回、combat 写操作的前提）
//            game.actor       actor 句柄（from_pointer/find/resolve，以及一切按句柄解析 actor 的能力）
//            game.actor_info  actor_info 的字段      game.actor_list  party_heroes、battle_monsters
//            game.battle      战斗序号和战斗状态     game.mods        启用 Mod 列表
//            combat.buff_read buff_list/buff_get/buff_def_get    combat.buff_add / buff_remove / buff_define
//            combat.effect_get / effect_apply / effect_define   combat.rule 规则判定   combat.skill 技能句柄
//            combat.enum      枚举表、source_flags 和 combat 的参数校验
//            persist          持久化服务            check            只给校验和修复用（DDCore 不用）
//            topic:<主题>     单个主题，可以用 * 结尾表示前缀（topic:buff.*）
//          signatures.inc 的 DD_DEPS 用同一套词汇。
//
// ---- 依据 ----
// DD_EVIDENCE(id, sig, origin, arg, where, span, "pattern", at, read, mul, rel, add)
//   可在运行时求值的依据。id 是清单项，或者特征码名字（这时是特征码的同一性依据，常量按 0 算）。
//   一项可以有多条，全部通过这一项才算通过。求值（C++ 和 Python 同义，tools/ddupdate/evidence.py 是参考实现）：
//   1. 起点（origin，sig 必须解析成功，否则这条依据不成立）
//        HIT          sig 的命中位置（GLOBAL_ANY 取地址最低的命中）
//        TARGET       sig 的解析结果（HOOK/CODE 是命中位置，GLOBAL 是 rip 相对的目标）；DD_RUNTIME_SELECT 的特征码
//                     只能用 HIT（按内容选的候选只有运行时才选得出来）
//        HIT_VALUE    命中位置 + 这一项的值 + arg（arg 可以为负），用于命中位置上的固定偏移
//        VSLOT        把 TARGET 当虚表，取第 arg 槽的 8 字节指针，减映像基址得到 RVA（必须在 .text）；
//                     加载后的模块减实际基址（指针已重定位），没有做重定位的映射减 OptionalHeader.ImageBase
//        VSLOT_VALUE  同上，槽号是这一项的值（用于虚表槽号本身）
//        NOSIG        没有起点，sig 写 NONE，只能配 where=TEXT
//   2. 范围（where）
//        ENTRY   模式必须恰好在起点处匹配          WINDOW  [起点, 起点 + span)
//        FUNC    起点所在函数的全部分块：只看异常目录（IMAGE_DIRECTORY_ENTRY_EXCEPTION）的 RUNTIME_FUNCTION，
//                按 BeginAddress 二分找包含起点的表项，沿间接表项（UnwindData 低位为 1）和 UNW_FLAG_CHAININFO
//                找到主函数（最多 32 次），主函数相同的表项都算；起点不在任何表项里时这条依据不成立。
//                C++ 自己解析异常目录，不用 RtlLookupFunctionEntry（要能对没加载的映像做对照测试）
//        TEXT    整个 .text（VirtualAddress .. +VirtualSize）
//      模式语法同 signatures.inc（?? 是通配），必须完整落在某一个范围里，按字节逐个位置试（可以重叠），总共恰好命中一次。
//   3. 读取（read）：命中位置 + at 处，小端。NONE 只要求唯一命中；U8/U16/U32/U64 无符号；I8/I32 有符号；
//      DEREF_U32 把 +at 处的 int32 当 rip 相对位移（位移是指令的最后一个字段，目标 = 命中 + at + 4 + 位移），读目标处的 uint32；
//      REL32 读出的是这个目标的 RVA 本身（call/jmp rel32 调用了谁），rel 必须是特征码。
//   4. 比较：期望值 = value(id) * mul + value(rel) + add。value(id)：清单项是它的值，特征码名字是 0；
//      value(rel)：NONE 是 0，清单项是它的值，特征码名字是它的解析结果的 RVA（那条特征码失效时这条依据不成立）。
//      读到的值无符号读取零扩展、有符号读取符号扩展成 64 位整数，和期望值直接比较，不截断（期望值超出读取宽度时
//      清单检查报错）。read=NONE 时 mul/rel/add 写 0/NONE/0。
//   修复工具用 (读到的值 - value(rel) - add) / mul 反推新值。
//
// DD_OFFLINE(id, check)        只能离线求值的依据，check 是 tools/ddupdate/offline.py 里注册的检查名
// DD_UNVERIFIED(id, "说明")    check=NONE 的项写明更新后怎么核实
// DD_NAMES(id, "名字", ...)    TABLE 的名字表，顺序就是游戏里的数值
// DD_VALUES(id, 值, ...)       TABLE 的整数表，值可以用 SRC_S | SRC_T | SRC_Y（来源标志，等于 DD_SOURCE_F_*）
// DD_GROUP(组名, "说明", 成员, ...)（U1 修复 FB，只给 Python 用，C++ 展开成空）
//                              一起变的项编成组（同一张虚表、同一个结构、同一个函数和它的原型），成员是清单项或特征码，
//                              一个成员可以在几个组里。组里运行时能核对的成员（RUNTIME 项、特征码）变了或失效时，组里没有
//                              运行时依据的成员（NONE、OFFLINE 项）在 verify 里列进「同组有变化」、在 repair 里报需要人工、
//                              accept 时要逐项确认（审核 A 的 S4）。组要同时有这两类成员，清单检查会查。组在文件末尾
//
// ---- 约定（用上面的格式登记，不是新的格式）----
// 原型项 Proto<函数>：COUNT，值是 C++ 声明并转发（hook 的 detour）或传入（调用游戏函数）的参数个数，说明里写全原型；
//   代码里用 static_assert(layout::kArgs<函数指针类型> == layout::kProto...) 和函数指针类型绑定（layout.h）。特征码命中、
//   依据成立都不代表参数没变，运行时核对不了，一律 NONE。按 ADR 0036 装 hook、调用游戏函数算「写」，依赖表写用到它的能力
//   和主题，未知版本上它们停用。
// 宽度项 <结构><种类>Size：SIZE，值是字节数，C++ 按这个宽度读写的字段写在说明里（代码里有 static_assert）。
//
// 新增或修改后运行 python -B DD.ModLoader/tools/dd_update.py verify（Steam 版和离线版都要全部通过），
// 再 xmake build dd_core_tests 跑一遍（tests/unit/test_main.cpp 里有旧值的对照断言，改值时同步改那里）。

#ifndef DD_LAYOUT
#define DD_LAYOUT(id, type, value, need, check, deps)
#endif
#ifndef DD_EVIDENCE
#define DD_EVIDENCE(id, sig, origin, arg, where, span, pattern, at, read, mul, rel, add)
#endif
#ifndef DD_OFFLINE
#define DD_OFFLINE(id, check)
#endif
#ifndef DD_UNVERIFIED
#define DD_UNVERIFIED(id, text)
#endif
#ifndef DD_NAMES
#define DD_NAMES(id, ...)
#endif
#ifndef DD_VALUES
#define DD_VALUES(id, ...)
#endif
#ifndef DD_GROUP
#define DD_GROUP(id, ...)
#endif

// ============================================================================
// 特征码的同一性依据（id 是特征码）：虚表确实是 Hero / Monster 的
// ============================================================================
// Hero 槽位 3（AsHero）返回 this，Monster 的返回 0
DD_EVIDENCE(HeroVtable, HeroVtable, VSLOT, 3, ENTRY, 0, "48 8B C1 C3", 0, NONE, 0, NONE, 0)
DD_EVIDENCE(MonsterVtable, MonsterVtable, VSLOT, 3, ENTRY, 0, "33 C0 C3", 0, NONE, 0, NONE, 0)

// ============================================================================
// actor 句柄（game/actors.h、actors.cpp），依据见 knowledge/dd1/combat.md 的「Actor 结构」「Actor 的标识和反查」
// ============================================================================
// Campaign+0x728：备用 Roster::System（App+0x54B 非零时游戏用它）。名册分支：lea rbp,[rax+0x728] 之后读 m_HeroEntries
DD_LAYOUT(RosterAlt, OFFSET, 0x728, OPT, RUNTIME, "game.actor")
DD_EVIDENCE(RosterAlt, NONE, NOSIG, 0, TEXT, 0, "E8 ?? ?? ?? ?? 48 8D A8 ?? ?? ?? ?? EB ?? E8 ?? ?? ?? ?? 48 8B E8 4C 8B 65 ?? 4C 2B 65 ??", 8, U32, 1, NONE, 0)
// Roster::System::m_HeroEntries，vector<Roster::Entry*>（同一处读 +0x28 / +0x20）
DD_LAYOUT(RosterHeroes, OFFSET, 0x20, OPT, RUNTIME, "game.actor")
DD_EVIDENCE(RosterHeroes, NONE, NOSIG, 0, TEXT, 0, "E8 ?? ?? ?? ?? 48 8D A8 ?? ?? ?? ?? EB ?? E8 ?? ?? ?? ?? 48 8B E8 4C 8B 65 ?? 4C 2B 65 ??", 29, U8, 1, NONE, 0)
DD_EVIDENCE(RosterHeroes, NONE, NOSIG, 0, TEXT, 0, "E8 ?? ?? ?? ?? 48 8D A8 ?? ?? ?? ?? EB ?? E8 ?? ?? ?? ?? 48 8B E8 4C 8B 65 ?? 4C 2B 65 ??", 25, U8, 1, NONE, 8)
// Roster::Entry 内嵌的 Hero
DD_LAYOUT(EntryHero, OFFSET, 0x08, OPT, NONE, "game.actor")
DD_UNVERIFIED(EntryHero, "名册遍历的运行核对（P3 探针 probe_p3、tests/plugins/probe_p3c.cpp）；Hero 虚表检查不过时句柄不给出，不会读错位置")
// Raid 的队伍 vector<Hero*>（4C41F0 的 target_hero_random 读 +8/+0x10）
DD_LAYOUT(RaidHeroes, OFFSET, 0x08, OPT, RUNTIME, "game.actor_list")
DD_EVIDENCE(RaidHeroes, NONE, NOSIG, 0, TEXT, 0, "4C 8B 2D ?? ?? ?? ?? 4D 8B 7D ?? 4D 2B 7D ?? 49 C1 FF 03", 14, U8, 1, NONE, 0)
DD_EVIDENCE(RaidHeroes, NONE, NOSIG, 0, TEXT, 0, "4C 8B 2D ?? ?? ?? ?? 4D 8B 7D ?? 4D 2B 7D ?? 49 C1 FF 03", 10, U8, 1, NONE, 8)
DD_OFFLINE(RaidHeroes, party_monster_targets)
// Hero 虚函数槽位 5：hero id；Monster 槽位 5：本场的 guid
DD_LAYOUT(HeroId, OFFSET, 0x137C, OPT, RUNTIME, "game.actor topic:buff.*")
DD_EVIDENCE(HeroId, HeroVtable, VSLOT, 5, ENTRY, 0, "8B 81 ?? ?? ?? ?? C3", 2, U32, 1, NONE, 0)
DD_LAYOUT(MonsterGuid, OFFSET, 0x1320, OPT, RUNTIME, "game.actor topic:buff.*")
DD_EVIDENCE(MonsterGuid, MonsterVtable, VSLOT, 5, ENTRY, 0, "8B 81 ?? ?? ?? ?? C3", 2, U32, 1, NONE, 0)
// Hero 槽位 6：HeroClass*；Monster 槽位 6：MonsterClass*（怪物受伤的免疫判断也读它）
DD_LAYOUT(HeroClass, OFFSET, 0x1320, OPT, RUNTIME, "game.actor_info combat.skill")
DD_EVIDENCE(HeroClass, HeroVtable, VSLOT, 6, ENTRY, 0, "48 8B 81 ?? ?? ?? ?? C3", 3, U32, 1, NONE, 0)
DD_LAYOUT(MonsterClass, OFFSET, 0x1468, OPT, RUNTIME, "game.actor_info combat.skill topic:damage.final")
DD_EVIDENCE(MonsterClass, MonsterVtable, VSLOT, 6, ENTRY, 0, "48 8B 81 ?? ?? ?? ?? C3", 3, U32, 1, NONE, 0)
DD_EVIDENCE(MonsterClass, MonsterBlockReturn, HIT, 0, WINDOW, 64, "48 8B 87 ?? ?? ?? ?? 80 B8 ?? ?? ?? ?? 00 75", 3, U32, 1, NONE, 0)
// Raid 内嵌的 Battle、Battle 的 vector<Monster*>（Raid+0x48D0）
DD_LAYOUT(RaidBattle, OFFSET, 0x4898, OPT, RUNTIME, "game.actor game.actor_list game.battle")
DD_EVIDENCE(RaidBattle, NONE, NOSIG, 0, TEXT, 0, "4C 8B 2D ?? ?? ?? ?? 4D 8B BD ?? ?? ?? ?? 4D 2B BD ?? ?? ?? ?? 49 C1 FF 03", 17, U32, 1, BattleMonsters, 0)
DD_OFFLINE(RaidBattle, raid_battle_lea)
DD_OFFLINE(RaidBattle, party_monster_targets)
DD_LAYOUT(BattleMonsters, OFFSET, 0x38, OPT, RUNTIME, "game.actor game.actor_list game.battle")
DD_EVIDENCE(BattleMonsters, NONE, NOSIG, 0, TEXT, 0, "40 53 55 56 57 48 83 EC 28 48 8B 69 ?? 48 8B F1 48 8B 59 ?? 48 3B DD 74", 19, U8, 1, NONE, 0)
DD_EVIDENCE(BattleMonsters, NONE, NOSIG, 0, TEXT, 0, "40 53 55 56 57 48 83 EC 28 48 8B 69 ?? 48 8B F1 48 8B 59 ?? 48 3B DD 74", 12, U8, 1, NONE, 8)
// Battle 的状态（bs_none = 0），即 Raid+0x4B20；本场的怪物 guid 计数（Raid+0x4B6C），Battle::Create 先清零再置 bs_create
DD_LAYOUT(BattleState, OFFSET, 0x288, OPT, OFFLINE, "game.battle game.actor")
DD_OFFLINE(BattleState, battle_create_order)
DD_LAYOUT(BattleGuidCounter, OFFSET, 0x2D4, OPT, RUNTIME, "game.battle game.actor")
// AddMonsterToBattle：mov rdi,[Raid::Current]; inc dword ptr [rdi+0x4B6C]（Raid+Battle+计数，这一处唯一）
DD_EVIDENCE(BattleGuidCounter, NONE, NOSIG, 0, TEXT, 0, "48 8B 3D ?? ?? ?? ?? FF 87 ?? ?? ?? ?? 80 3D", 9, U32, 1, RaidBattle, 0)
DD_EVIDENCE(BattleGuidCounter, NONE, NOSIG, 0, TEXT, 0, "48 8B 3D ?? ?? ?? ?? FF 87 ?? ?? ?? ?? 80 3D", 3, REL32, 0, RaidCurrent, 0)
DD_OFFLINE(BattleGuidCounter, battle_create_order)
DD_LAYOUT(BattleStateNone, ENUM, 0, OPT, NONE, "game.battle")
DD_UNVERIFIED(BattleStateNone, "Battle::Create 置 bs_create（1）之前、撤退时（5A93B3）状态是 0；knowledge/dd1/combat.md 的「Battle」，战斗序号的单元测试和 P3c 探针")
// 当前生命、压力（float）。Hero 槽位 40（HP == 0，在死门上）、43（HP < 0 或已死）读生命
DD_LAYOUT(ActorHp, OFFSET, 0x10A8, OPT, RUNTIME, "game.actor_info topic:damage.* topic:heal.* topic:effect.*")
DD_EVIDENCE(ActorHp, HeroVtable, VSLOT, 40, ENTRY, 0, "F3 0F 10 89 ?? ?? ?? ?? 0F 57 C0 0F 2E C8", 4, U32, 1, NONE, 0)
DD_EVIDENCE(ActorHp, HeroVtable, VSLOT, 43, ENTRY, 0, "0F 57 C0 0F 2F 81 ?? ?? ?? ?? 77", 6, U32, 1, NONE, 0)
DD_LAYOUT(ActorStress, OFFSET, 0x12FC, OPT, RUNTIME, "game.actor_info topic:stress.* topic:effect.*")
DD_EVIDENCE(ActorStress, HeroVtable, VSLOT, 37, FUNC, 0, "F3 0F 10 8B ?? ?? ?? ?? F3 0F 5C C8", 4, U32, 1, NONE, 0)
DD_OFFLINE(ActorStress, hero_stress_slot)
// 最大生命：治疗和伤害用 (Actor+0x768, [Actor+0x790], true) 调用 ActorMaxStat
DD_LAYOUT(ActorMaxHpStat, OFFSET, 0x768, OPT, RUNTIME, "game.actor_info")
DD_EVIDENCE(ActorMaxHpStat, HeroVtable, VSLOT, 50, FUNC, 0, "48 8D 8B ?? ?? ?? ?? 41 B0 01 8B 93 ?? ?? ?? ?? E8", 3, U32, 1, NONE, 0)
DD_OFFLINE(ActorMaxHpStat, maxstat_calls)
DD_LAYOUT(ActorMaxHpMask, OFFSET, 0x790, OPT, RUNTIME, "game.actor_info")
DD_EVIDENCE(ActorMaxHpMask, HeroVtable, VSLOT, 50, FUNC, 0, "48 8D 8B ?? ?? ?? ?? 41 B0 01 8B 93 ?? ?? ?? ?? E8", 12, U32, 1, NONE, 0)
DD_OFFLINE(ActorMaxHpMask, maxstat_calls)
// ActorClass+8：职业 id，char[0x40]（hero_class_name_%s、str_monstername_%s 的参数）
DD_LAYOUT(ClassId, OFFSET, 0x08, OPT, OFFLINE, "game.actor_info")
DD_OFFLINE(ClassId, class_id_strings)
DD_LAYOUT(ClassIdCap, SIZE, 0x40, OPT, NONE, "game.actor_info")
DD_UNVERIFIED(ClassIdCap, "职业 id 的字段宽度 char[0x40]（knowledge/dd1/combat.md）；读的时候要求在宽度内以 0 结尾，否则不给 class_id")
// actor_info 调用 ActorMaxStat 取最大生命（U1 修复 FA：调用游戏函数，game.actor_info 归为「写」）
DD_LAYOUT(ProtoActorMaxStat, COUNT, 3, OPT, NONE, "game.actor_info")
DD_UNVERIFIED(ProtoActorMaxStat, "float ActorMaxStat(const Stat* rcx, uint32 mask edx, bool clamp r8b)，返回 xmm0（4C3F20，叶函数）；治疗 483DD3、伤害里的调用点按这个形状传参（ActorMaxHpStat 的依据）。更新后在 dd_disasm 里看调用点和函数开头的参数用法")
// 生命、压力是 float，hero id、怪物 guid 是 uint32
DD_LAYOUT(ActorFloatSize, SIZE, 4, OPT, NONE, "game.actor_info topic:damage.* topic:heal.* topic:stress.* topic:effect.*")
DD_UNVERIFIED(ActorFloatSize, "ActorHp、ActorStress 按 float 读写，ActorMaxStat 返回 float，ActorMaxHpMask 是 uint32。这几项的依据是 movss / mov r32，宽度变了时模式通常对不上；单独登记让更新后的人工核实清单列出它")
DD_LAYOUT(ActorIdSize, SIZE, 4, OPT, NONE, "game.actor topic:buff.*")
DD_UNVERIFIED(ActorIdSize, "HeroId、MonsterGuid 按 uint32 读（槽位 5 的 getter 是 mov eax,[rcx+X]，HeroId、MonsterGuid 的依据就是这条指令）")

// ============================================================================
// Buff（combat/combat.cpp），依据见 knowledge/dd1/combat.md 的「Buff」
// ============================================================================
// Actor 的 Buff::Group（vector<GroupEntry> 的 first/last/end）和下一个组 guid（Group+0x18）
DD_LAYOUT(ActorBuffGroup, OFFSET, 0x460, OPT, RUNTIME, "combat.buff_read combat.buff_add combat.buff_remove topic:buff.*")
DD_EVIDENCE(ActorBuffGroup, HeroVtable, VSLOT, 13, FUNC, 0, "48 81 C1 ?? ?? ?? ??", 3, U32, 1, NONE, 0)
DD_LAYOUT(ActorNextGuid, OFFSET, 0x478, OPT, RUNTIME, "combat.buff_add")
DD_EVIDENCE(ActorNextGuid, HeroVtable, VSLOT, 13, FUNC, 0, "44 8B B9 ?? ?? ?? ??", 3, U32, 1, NONE, 0)
// GroupEntry：+0 组 guid，+8 Instance；Instance 0x410 字节
DD_LAYOUT(BuffEntryGuid, OFFSET, 0x00, OPT, NONE, "combat.buff_read combat.buff_remove topic:buff.*")
DD_UNVERIFIED(BuffEntryGuid, "GroupEntry 的 +0 是组 guid（uint32），+8 是 Instance（BuffEntryInstance 有依据）；偏移 0 没有位移可读。buff_list、按 guid 的 buff_remove 谓词、buff.* 的 guid 字段读它，test_combat 运行核对")
DD_LAYOUT(BuffEntrySize, SIZE, 0x418, OPT, RUNTIME, "combat.buff_read combat.buff_add combat.buff_remove topic:buff.*")
DD_EVIDENCE(BuffEntrySize, BuffGroupErase, TARGET, 0, FUNC, 0, "49 8D B0 ?? ?? ?? ??", 3, U32, 1, NONE, 0)
DD_LAYOUT(BuffEntryInstance, OFFSET, 0x08, OPT, RUNTIME, "combat.buff_read combat.buff_remove topic:buff.*")
DD_EVIDENCE(BuffEntryInstance, BuffDurationUpkeep, TARGET, 0, FUNC, 0, "89 43 ?? 48 81 C3 18 04 00 00", 2, U8, 1, InstDuration, 0)
DD_LAYOUT(BuffInstanceSize, SIZE, 0x410, OPT, RUNTIME, "combat.buff_add")
DD_EVIDENCE(BuffInstanceSize, BuffGroupErase, TARGET, 0, FUNC, 0, "49 8D B0 ?? ?? ?? ??", 3, U32, 1, BuffEntryInstance, 0)
DD_EVIDENCE(BuffInstanceSize, BuffLibraryIndex, TARGET, 0, FUNC, 0, "33 D2 41 B8 ?? ?? ?? ?? 48 8D 4F", 4, U32, 1, NONE, 0)
DD_EVIDENCE(BuffInstanceSize, BuffLibraryIndex, TARGET, 0, FUNC, 0, "B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B F8", 1, U32, 1, MapNodeValue, 0)
DD_OFFLINE(BuffInstanceSize, instance_copy_fields)
// Buff::Instance 的字段。复制构造（BuffInstanceCopy，482480）从 +0 起逐字段复制到 +0x151，形状相同的复制连成一串
// （mov eax,[rdx+X]; mov [rcx+X],eax）。原来的依据只带这一条复制、靠目的偏移的字面量区分字段，结构里插入或删除一个
// int 时会唯一命中邻居字段的复制、读到的值恰好等于旧常量（审核 A 的 B1）。同形状的一串复制里偏移是等差的，插入或删除
// 之后剩下的偏移还是那串数，只有这一串结尾之后、形状不同的下一个字段（界标）的位置会变。U1 修复 FA 把这些依据收紧成：
// 要读的字段（偏移写 ??）+ 同一串里它后面的全部复制（偏移写字面量）+ 界标（偏移写字面量），char[64] 字段是 4 次 movups 加界标。
// 这一串里插入、删除字段时依据不成立（停用，不会误绑定）；串外、界标之后的改动不影响。代价：紧挨着这一串插入别的形状的
// 字段时依据也不成立（值其实没变），要人工核实、改模式
//   界标：+0x44（dword）、+0x60（byte）、+0xA4（dword）、+0xAC（byte）、+0xB0（dword）、+0xF4（dword）
DD_LAYOUT(InstStat, OFFSET, 0x00, OPT, NONE, "combat.buff_read combat.buff_define topic:buff.*")
DD_UNVERIFIED(InstStat, "Instance+0 的 stat_type（偏移 0 没有位移可读）；字段布局由复制构造的逐字段复制（InstSubType 起）佐证，test_combat 的 buff_get 运行核对")
DD_LAYOUT(InstSubType, OFFSET, 0x04, OPT, RUNTIME, "combat.buff_read combat.buff_define")
DD_EVIDENCE(InstSubType, BuffInstanceCopy, TARGET, 0, FUNC, 0, "0F 10 42 ?? 0F 11 41 ?? 0F 10 4A 14 0F 11 49 14 0F 10 42 24 0F 11 41 24 0F 10 4A 34 0F 11 49 34 8B 42 44 89 41 44", 3, U8, 1, NONE, 0)
DD_LAYOUT(InstSubHash, OFFSET, 0x44, OPT, RUNTIME, "combat.buff_define")
DD_EVIDENCE(InstSubHash, BuffInstanceCopy, TARGET, 0, FUNC, 0, "8B 42 ?? 89 41 ?? 8B 42 48 89 41 48 8B 42 4C 89 41 4C 8B 42 50 89 41 50 8B 42 54 89 41 54 8B 42 58 89 41 58 8B 42 5C 89 41 5C 0F B6 42 60 88 41 60", 2, U8, 1, NONE, 0)
DD_LAYOUT(InstAmount, OFFSET, 0x48, OPT, RUNTIME, "combat.buff_read combat.buff_define topic:buff.*")
DD_EVIDENCE(InstAmount, BuffInstanceCopy, TARGET, 0, FUNC, 0, "8B 42 ?? 89 41 ?? 8B 42 4C 89 41 4C 8B 42 50 89 41 50 8B 42 54 89 41 54 8B 42 58 89 41 58 8B 42 5C 89 41 5C 0F B6 42 60 88 41 60", 2, U8, 1, NONE, 0)
DD_LAYOUT(InstDuration, OFFSET, 0x4C, OPT, RUNTIME, "combat.buff_read combat.buff_add combat.buff_define topic:buff.*")
DD_EVIDENCE(InstDuration, BuffInstanceCtor, TARGET, 0, FUNC, 0, "C7 41 ?? FF FF FF FF", 2, U8, 1, NONE, 0)
DD_EVIDENCE(InstDuration, BuffDurationUpkeep, TARGET, 0, FUNC, 0, "89 43 ?? 48 81 C3 18 04 00 00", 2, U8, 1, BuffEntryInstance, 0)
DD_OFFLINE(InstDuration, instance_ctor_defaults)
DD_LAYOUT(InstDurationType, OFFSET, 0x50, OPT, RUNTIME, "combat.buff_read combat.buff_add combat.buff_define topic:buff.*")
DD_EVIDENCE(InstDurationType, BuffInstanceCtor, TARGET, 0, FUNC, 0, "48 C7 41 ?? 09 00 00 00", 3, U8, 1, NONE, 0)
DD_EVIDENCE(InstDurationType, BuffDurationUpkeep, TARGET, 0, FUNC, 0, "44 39 73 ??", 3, U8, 1, BuffEntryInstance, 0)
DD_OFFLINE(InstDurationType, instance_ctor_defaults)
DD_LAYOUT(InstSource, OFFSET, 0x54, OPT, RUNTIME, "combat.buff_read combat.buff_add combat.buff_remove combat.buff_define topic:buff.*")
DD_EVIDENCE(InstSource, BuffInstanceCopy, TARGET, 0, FUNC, 0, "8B 42 ?? 89 41 ?? 8B 42 58 89 41 58 8B 42 5C 89 41 5C 0F B6 42 60 88 41 60", 2, U8, 1, NONE, 0)
DD_LAYOUT(InstSourceData, OFFSET, 0x58, OPT, RUNTIME, "combat.buff_read combat.buff_add combat.buff_define topic:buff.*")
DD_EVIDENCE(InstSourceData, BuffInstanceCopy, TARGET, 0, FUNC, 0, "8B 42 ?? 89 41 ?? 8B 42 5C 89 41 5C 0F B6 42 60 88 41 60", 2, U8, 1, NONE, 0)
DD_LAYOUT(InstRule, OFFSET, 0x5C, OPT, RUNTIME, "combat.buff_read combat.buff_define combat.rule topic:buff.*")
DD_EVIDENCE(InstRule, BuffRuleActive, TARGET, 0, FUNC, 0, "41 8B 47 ?? 83 C0 FA", 3, U8, 1, NONE, 0)
DD_LAYOUT(InstFalseRule, OFFSET, 0x60, OPT, RUNTIME, "combat.buff_read combat.buff_define combat.rule")
DD_EVIDENCE(InstFalseRule, BuffRuleActive, TARGET, 0, FUNC, 0, "41 80 7F ?? 00", 3, U8, 1, NONE, 0)
DD_LAYOUT(InstId, OFFSET, 0x61, OPT, RUNTIME, "combat.buff_read combat.buff_define combat.rule topic:buff.*")
DD_EVIDENCE(InstId, BuffInstanceCopy, TARGET, 0, FUNC, 0, "0F 10 42 ?? 0F 11 41 ?? 0F 10 4A 71 0F 11 49 71 0F 10 82 81 00 00 00 0F 11 81 81 00 00 00 0F 10 8A 91 00 00 00 0F 11 89 91 00 00 00 8B 82 A4 00 00 00 89 81 A4 00 00 00", 3, U8, 1, NONE, 0)
DD_LAYOUT(InstIdHash, OFFSET, 0xA4, OPT, RUNTIME, "combat.buff_define combat.buff_remove")
DD_EVIDENCE(InstIdHash, BuffInstanceCopy, TARGET, 0, FUNC, 0, "8B 82 ?? ?? ?? ?? 89 81 ?? ?? ?? ?? 8B 82 A8 00 00 00 89 81 A8 00 00 00 0F B6 82 AC 00 00 00 88 81 AC 00 00 00", 2, U32, 1, NONE, 0)
DD_LAYOUT(InstTarget, OFFSET, 0xA8, OPT, RUNTIME, "combat.buff_add combat.buff_define")
DD_EVIDENCE(InstTarget, BuffInstanceCopy, TARGET, 0, FUNC, 0, "8B 82 ?? ?? ?? ?? 89 81 ?? ?? ?? ?? 0F B6 82 AC 00 00 00 88 81 AC 00 00 00", 2, U32, 1, NONE, 0)
DD_LAYOUT(InstRemoveInactive, OFFSET, 0xAC, OPT, RUNTIME, "combat.buff_read combat.buff_define")
DD_EVIDENCE(InstRemoveInactive, BuffInstanceCopy, TARGET, 0, FUNC, 0, "0F B6 82 ?? ?? ?? ?? 88 81 ?? ?? ?? ?? 0F B6 82 AD 00 00 00 88 81 AD 00 00 00 8B 82 B0 00 00 00 89 81 B0 00 00 00", 3, U32, 1, NONE, 0)
DD_LAYOUT(InstRemoveBattle, OFFSET, 0xAD, OPT, RUNTIME, "combat.buff_read combat.buff_define")
DD_EVIDENCE(InstRemoveBattle, BuffInstanceCopy, TARGET, 0, FUNC, 0, "0F B6 82 ?? ?? ?? ?? 88 81 ?? ?? ?? ?? 8B 82 B0 00 00 00 89 81 B0 00 00 00", 3, U32, 1, NONE, 0)
DD_LAYOUT(InstRuleFloat, OFFSET, 0xB0, OPT, RUNTIME, "combat.buff_read combat.buff_define combat.rule")
DD_EVIDENCE(InstRuleFloat, BuffRuleActive, TARGET, 0, FUNC, 0, "F3 41 0F 10 87 ?? ?? ?? ?? 0F 2F C2", 5, U32, 1, NONE, 0)
DD_LAYOUT(InstRuleString, OFFSET, 0xB4, OPT, RUNTIME, "combat.buff_read combat.buff_define")
DD_EVIDENCE(InstRuleString, BuffInstanceCopy, TARGET, 0, FUNC, 0, "0F 10 82 ?? ?? ?? ?? 0F 11 81 ?? ?? ?? ?? 0F 10 8A C4 00 00 00 0F 11 89 C4 00 00 00 0F 10 82 D4 00 00 00 0F 11 81 D4 00 00 00 0F 10 8A E4 00 00 00 0F 11 89 E4 00 00 00 8B 82 F4 00 00 00 89 81 F4 00 00 00", 3, U32, 1, NONE, 0)
DD_LAYOUT(InstRuleHash, OFFSET, 0xF4, OPT, RUNTIME, "combat.buff_read combat.buff_define combat.rule topic:buff.*")
DD_EVIDENCE(InstRuleHash, BuffRuleActive, TARGET, 0, FUNC, 0, "41 8B 87 ?? ?? ?? ?? 0F 57 F6", 3, U32, 1, NONE, 0)
// +0x158 的 25 个 vector<char[64]>（附加效果），+0x3B0 的 vector<char[64]>（规则清除列表）；析构时
// EhVectorDtor(+0x158, 0x18, 0x19, VectorDtor64b) 和 VectorDtor64(+0x3B0)
DD_LAYOUT(InstAdditional, OFFSET, 0x158, OPT, RUNTIME, "combat.buff_add")
DD_EVIDENCE(InstAdditional, BuffInstanceCopy, TARGET, 0, FUNC, 0, "48 81 C2 ?? ?? ?? ?? 48 81 C1 ?? ?? ?? ??", 3, U32, 1, NONE, 0)
DD_EVIDENCE(InstAdditional, BuffInstanceCopy, TARGET, 0, FUNC, 0, "48 81 C2 ?? ?? ?? ?? 48 81 C1 ?? ?? ?? ??", 10, U32, 1, NONE, 0)
DD_OFFLINE(InstAdditional, instance_copy_fields)
DD_LAYOUT(InstAdditionalCount, COUNT, 25, OPT, RUNTIME, "combat.buff_add")
DD_EVIDENCE(InstAdditionalCount, BuffGroupErase, TARGET, 0, FUNC, 0, "BA ?? ?? ?? ?? 41 B8 ?? ?? ?? ??", 7, U32, 1, NONE, 0)
DD_LAYOUT(InstClearOnRule, OFFSET, 0x3B0, OPT, RUNTIME, "combat.buff_add")
DD_EVIDENCE(InstClearOnRule, BuffInstanceCopy, TARGET, 0, FUNC, 0, "48 8D 93 ?? ?? ?? ?? 48 8D 8F ?? ?? ?? ??", 3, U32, 1, NONE, 0)
DD_EVIDENCE(InstClearOnRule, BuffInstanceCopy, TARGET, 0, FUNC, 0, "48 8D 93 ?? ?? ?? ?? 48 8D 8F ?? ?? ?? ??", 10, U32, 1, NONE, 0)
DD_OFFLINE(InstClearOnRule, instance_copy_fields)
// Buff::Library：+0 普通 map，+0x10 联机（备用名册）的 map
DD_LAYOUT(LibraryAltMap, OFFSET, 0x10, OPT, NONE, "combat.buff_read combat.buff_add combat.buff_define")
DD_UNVERIFIED(LibraryAltMap, "查 Buff 的函数在 App+0x54B（AltRosterFlag）为 1 时用 Buff::Library+0x10 的联机 map（knowledge/dd1/combat.md 的「Buff」，P4-0D 探针）")
// 虚表槽位：13 AddBuff（Instance 复制进组、返回组 guid），14 按谓词删除（std::function<bool(GroupEntry const&)>）
DD_LAYOUT(SlotAddBuff, SLOT, 13, OPT, RUNTIME, "combat.buff_add")
DD_EVIDENCE(SlotAddBuff, HeroVtable, VSLOT_VALUE, 0, FUNC, 0, "44 8B B9 ?? ?? ?? ??", 3, U32, 0, ActorNextGuid, 0)
DD_LAYOUT(SlotRemoveBuffs, SLOT, 14, OPT, RUNTIME, "combat.buff_remove topic:buff.remove")
DD_EVIDENCE(SlotRemoveBuffs, HeroVtable, VSLOT_VALUE, 0, FUNC, 0, "48 81 C1 ?? ?? ?? ??", 3, U32, 0, ActorBuffGroup, 0)
// 规则类型的数值来自 EnumRule 的名字表（actorStatus、is_actor_status），不单独登记

// 宽度（U1 修复 FA）：combat 按固定宽度读写 Instance 和 GroupEntry（G_FillDefinition、ReadEntry、谓词）
DD_LAYOUT(InstScalarSize, SIZE, 4, OPT, NONE, "combat.buff_read combat.buff_add combat.buff_define combat.buff_remove combat.rule topic:buff.*")
DD_UNVERIFIED(InstScalarSize, "Instance 的 stat_type、duration、duration_type、source、sourcedata、rule_type、target 按 int32，amount、rule_float 按 float，sub_type、id、rule_string 的哈希按 uint32 读写。复制构造逐字段 mov r32 复制（InstSubHash 等的依据带着这一串复制），BuffRuleActive 用 movss 读 rule_float；宽度变了时这些依据通常对不上，单独登记让人工核实清单列出它")
DD_LAYOUT(InstFlagSize, SIZE, 1, OPT, NONE, "combat.buff_read combat.buff_define combat.rule")
DD_UNVERIFIED(InstFlagSize, "false_rule、remove_if_not_active、remove_on_battle_complete 按 1 字节读写（复制构造用 movzx / mov r8 复制 +0x60、+0xAC、+0xAD，InstId、InstRemoveInactive 等的依据带着这几条）")
DD_LAYOUT(BuffGuidSize, SIZE, 4, OPT, NONE, "combat.buff_read combat.buff_add combat.buff_remove combat.effect_apply topic:buff.*")
DD_UNVERIFIED(BuffGuidSize, "buff 的 guid 都按 uint32：GroupEntry+0 的组 guid、Actor 的下一个组 guid（槽位 13 用 mov r15d 读，SlotAddBuff 的依据）、槽位 13 的返回值 eax、ApplyEffect 结果里的 vector<uint32>（FreeU32Vector 按 4 字节元素算）")

// combat 调用的游戏函数和装的 hook 的原型（约定见文件开头；类型在 combat/combat.cpp 的 CopyFn 等）
DD_LAYOUT(ProtoBuffInstanceCopy, COUNT, 2, OPT, NONE, "combat.buff_add")
DD_UNVERIFIED(ProtoBuffInstanceCopy, "Instance* Buff::Instance::Instance(Instance* this rcx, const Instance& rdx)（482480），返回 this；5C4F50 的调用点照这个传参")
DD_LAYOUT(ProtoVectorDtor64, COUNT, 1, OPT, NONE, "combat.buff_add")
DD_UNVERIFIED(ProtoVectorDtor64, "void vector<char[64]> 的释放(vector* rcx)（2345D0），Instance 析构对 +0x3B0 调用；EhVectorDtor 的元素析构 VectorDtor64b（2342E0）也是 void (void*)，由游戏的 EhVectorDtor 调用")
DD_LAYOUT(ProtoEhVectorDtor, COUNT, 4, OPT, NONE, "combat.buff_add")
DD_UNVERIFIED(ProtoEhVectorDtor, "MSVC 的 eh vector destructor iterator(void* 数组 rcx, size_t 元素大小 rdx, size_t 个数 r8, void (*元素析构)(void*) r9)（B9FBD0），Instance 析构对 +0x158 的 25 个 vector 调用")
DD_LAYOUT(ProtoAddBuff, COUNT, 4, OPT, NONE, "combat.buff_add")
DD_UNVERIFIED(ProtoAddBuff, "Hero/Monster 虚表槽位 13：uint32 AddBuff(Actor* rcx, const Instance* rdx, void* r8（传 nullptr）, bool refresh r9b)，返回组 guid（Hero 5CE3C0）")
DD_LAYOUT(ProtoRemoveBuffs, COUNT, 2, OPT, NONE, "combat.buff_remove")
DD_UNVERIFIED(ProtoRemoveBuffs, "Hero/Monster 虚表槽位 14：bool RemoveMultipleBuffs(Actor* rcx, std::function<bool(GroupEntry const&)> 按值传、rdx 指向它)，结尾经实现的 _Delete_this 析构（Hero 5CE600，StdFunctionImpl 的依据）")
DD_LAYOUT(ProtoBuffLibraryIndex, COUNT, 2, OPT, NONE, "combat.buff_define")
DD_UNVERIFIED(ProtoBuffLibraryIndex, "Instance& map<uint32, Instance>::operator[](map* rcx, const uint32* key rdx)（4ACFA0），没有就插入默认构造的 Instance")
DD_LAYOUT(ProtoBuffGroupAdd, COUNT, 3, OPT, NONE, "topic:buff.add")
DD_UNVERIFIED(ProtoBuffGroupAdd, "buff.add 的 hook：void* Buff::Group::Add(Group* rcx, uint32 guid edx, const Instance* r8)（5C4F50），detour 按 3 个参数转发")
DD_LAYOUT(ProtoBuffGroupErase, COUNT, 3, OPT, NONE, "topic:buff.remove topic:buff.tick")
DD_UNVERIFIED(ProtoBuffGroupErase, "buff.remove 的 hook：iterator* Buff::Group::erase(Group* rcx, iterator* 返回值 rdx, GroupEntry* r8)（5C4FF0），detour 按 3 个参数转发")
DD_LAYOUT(ProtoBuffDurationUpkeep, COUNT, 3, OPT, NONE, "topic:buff.tick topic:buff.remove")
DD_UNVERIFIED(ProtoBuffDurationUpkeep, "buff.tick 的 hook：Actor::BuffDurationUpkeep(Actor* rcx, void* 过滤 rdx, int 持续时间类型 r8d)（47AAD0），返回 64 位，detour 按 3 个参数转发")
DD_LAYOUT(ProtoBuffRuleActive, COUNT, 5, OPT, NONE, "combat.rule")
DD_UNVERIFIED(ProtoBuffRuleActive, "规则 hook：bool IsRuleActive(Actor* rcx, const Instance* rdx, const void* r8, Actor* 目标 r9, const Skill* [rsp+0x28])（479CD0），detour 按 5 个参数转发")

// ============================================================================
// MSVC 标准库的布局（游戏用的工具链决定，DDCore 直接读写游戏的这些对象）
// ============================================================================
// std::map 的节点：+0 左、+8 父（头节点的 +8 是根）、+0x10 右、+0x19 _Isnil、+0x20 键、+0x28 值
DD_LAYOUT(MapNodeLeft, OFFSET, 0x00, OPT, RUNTIME, "combat.buff_read combat.buff_add combat.buff_define combat.effect_get combat.effect_apply combat.effect_define")
DD_EVIDENCE(MapNodeLeft, BuffLibraryIndex, TARGET, 0, FUNC, 0, "BE 01 00 00 00 48 8B C8 48 8B 00 80 78", 0, NONE, 0, NONE, 0)
DD_LAYOUT(MapNodeParent, OFFSET, 0x08, OPT, RUNTIME, "combat.buff_read combat.buff_add combat.buff_define combat.effect_get combat.effect_apply combat.effect_define")
DD_EVIDENCE(MapNodeParent, BuffLibraryIndex, TARGET, 0, FUNC, 0, "4C 8B 39 49 8B 47 ?? 49 8B CF 80 78 ?? 00", 6, U8, 1, NONE, 0)
DD_LAYOUT(MapNodeRight, OFFSET, 0x10, OPT, RUNTIME, "combat.buff_read combat.buff_add combat.buff_define combat.effect_get combat.effect_apply combat.effect_define")
DD_EVIDENCE(MapNodeRight, BuffLibraryIndex, TARGET, 0, FUNC, 0, "33 F6 48 8B 40 ?? EB", 5, U8, 1, NONE, 0)
DD_LAYOUT(MapNodeIsNil, OFFSET, 0x19, OPT, RUNTIME, "combat.buff_read combat.buff_add combat.buff_define combat.effect_get combat.effect_apply combat.effect_define")
DD_EVIDENCE(MapNodeIsNil, BuffLibraryIndex, TARGET, 0, FUNC, 0, "4C 8B 39 49 8B 47 ?? 49 8B CF 80 78 ?? 00", 12, U8, 1, NONE, 0)
DD_LAYOUT(MapNodeKey, OFFSET, 0x20, OPT, RUNTIME, "combat.buff_read combat.buff_add combat.buff_define combat.effect_get combat.effect_apply combat.effect_define")
DD_EVIDENCE(MapNodeKey, BuffLibraryIndex, TARGET, 0, FUNC, 0, "44 39 40 ?? 73", 3, U8, 1, NONE, 0)
DD_LAYOUT(MapNodeValue, OFFSET, 0x28, OPT, RUNTIME, "combat.buff_read combat.buff_add combat.buff_define combat.effect_get combat.effect_apply combat.effect_define")
DD_EVIDENCE(MapNodeValue, BuffLibraryIndex, TARGET, 0, FUNC, 0, "48 8D 41 ?? 48 83 C4 58", 3, U8, 1, NONE, 0)
// std::vector：first/last/end 三个指针，0x18 字节
DD_LAYOUT(StdVectorSize, SIZE, 0x18, OPT, RUNTIME, "combat.buff_add combat.skill")
DD_EVIDENCE(StdVectorSize, BuffGroupErase, TARGET, 0, FUNC, 0, "BA ?? ?? ?? ?? 41 B8 ?? ?? ?? ??", 1, U32, 1, NONE, 0)
DD_LAYOUT(StdVectorFirst, OFFSET, 0x00, OPT, NONE, "topic:skill.* combat.buff_read combat.buff_add combat.buff_remove combat.effect_apply game.actor game.actor_list game.mods")
DD_UNVERIFIED(StdVectorFirst, "MSVC std::vector 的 first 在 +0（偏移 0 没有位移可读），DDCore 读 vector 的地方（skill.* 的目标、actor 的 buff 表、名册和战斗的 actor 列表、Mod 列表、ApplyEffect 结果）都按 first/last/end 的布局（StdVectorSize 有依据）；工具链大版本变化才会变")
DD_LAYOUT(StdVectorLast, OFFSET, 0x08, OPT, NONE, "topic:skill.*")
DD_UNVERIFIED(StdVectorLast, "MSVC std::vector 的 first/last/end，DDCore 读 vector 的地方都按这个布局（StdVectorSize 有依据）；工具链大版本变化才会变")
// std::function：0x40 字节，+0x38 指向实现；实现的虚表 _Copy、_Move、_Do_call、_Target_type、_Delete_this、_Get 共 6 槽。
// 依据（U1b）是 Hero 槽位 14（Hero::RemoveMultipleBuffs，按值接收 std::function，5CE600）结尾的析构：
//   mov rcx,[rbp+0x38]; test rcx,rcx; je; mov rax,[rcx]; cmp rcx,rbp; setne dl; call [rax+0x20]
// rbp 是 std::function 对象本身：实现指针在 +0x38，和对象起点比较（相等时实现放在对象内部的小对象缓冲里），
// _Delete_this 在实现虚表的 +0x20（槽 4）。大小按 MSVC 的结构（小对象缓冲 0x38 字节，后面紧跟实现指针，是最后一个成员）
// 由实现指针的位置推出：大小 = 实现指针偏移 + 8；槽数按 _Delete_this 是倒数第二个虚函数推出：+0x20 = (槽数 - 2) * 8。
// 这两条推出的依据只核对上面的结构关系，结构本身换了（比如实现指针之后加成员）要人工确认
DD_LAYOUT(StdFunctionSize, SIZE, 0x40, OPT, RUNTIME, "combat.buff_remove events.native events.query")
DD_EVIDENCE(StdFunctionSize, HeroVtable, VSLOT, 14, FUNC, 0, "48 8B ?? ?? 48 85 C9 74 ?? 48 8B 01 48 3B ?? 0F 95 C2 FF 50 ??", 3, U8, 1, NONE, -8)
DD_LAYOUT(StdFunctionImpl, OFFSET, 0x38, OPT, RUNTIME, "combat.buff_remove events.native events.query")
DD_EVIDENCE(StdFunctionImpl, HeroVtable, VSLOT, 14, FUNC, 0, "48 8B ?? ?? 48 85 C9 74 ?? 48 8B 01 48 3B ?? 0F 95 C2 FF 50 ??", 3, U8, 1, NONE, 0)
DD_LAYOUT(StdFuncImplVtableSlots, COUNT, 6, OPT, RUNTIME, "combat.buff_remove events.native events.query")
DD_EVIDENCE(StdFuncImplVtableSlots, HeroVtable, VSLOT, 14, FUNC, 0, "48 8B ?? ?? 48 85 C9 74 ?? 48 8B 01 48 3B ?? 0F 95 C2 FF 50 ??", 20, U8, 8, NONE, -16)
// 原生事件的派发（ListenerListData::Dispatch，内联在调用点）调用 _Do_call：call [rax+0x10]（槽 2 = 槽数 - 4），见下面 ListenerFunction 的依据
DD_EVIDENCE(StdFuncImplVtableSlots, MonsterBlockReturn, TARGET, 0, FUNC, 0, "48 8B ?? ?? 48 85 C9 74 ?? 48 8D 85 ?? ?? ?? ?? 48 89 85 ?? ?? ?? ?? 48 8B 01 48 8D 95 ?? ?? ?? ?? FF 50 ?? 83 F8 01", 35, U8, 8, NONE, -32)

// ============================================================================
// Effect（combat/combat.cpp、events/system.cpp），4F02D0 解析 .effects.darkest 时写的字段，类型按写入函数
// ============================================================================
DD_LAYOUT(EffectName, OFFSET, 0x00, OPT, NONE, "combat.effect_get topic:effect.*")
DD_UNVERIFIED(EffectName, "4F02D0 把 .name 写进 Effect+0 的 char[64]；effect_get 和 effect.* 的 effect_name 运行核对（test_combat、test_events）")
DD_LAYOUT(EffectNameCap, SIZE, 0x40, OPT, NONE, "topic:effect.*")
DD_UNVERIFIED(EffectNameCap, "4F02D0 把 .name 写进 Effect+0 的 char[64]；effect_get 和 effect.* 的 effect_name 运行核对（test_combat、test_events）")
DD_LAYOUT(EffectTarget, OFFSET, 0x44, OPT, OFFLINE, "combat.effect_get")
DD_OFFLINE(EffectTarget, effect_misc_fields)
DD_LAYOUT(EffectChance, OFFSET, 0x48, OPT, OFFLINE, "combat.effect_get")
DD_OFFLINE(EffectChance, effect_field_types)
DD_LAYOUT(EffectStress, OFFSET, 0x4C, OPT, OFFLINE, "combat.effect_get topic:effect.*")
DD_OFFLINE(EffectStress, effect_field_types)
DD_LAYOUT(EffectStun, OFFSET, 0x50, OPT, OFFLINE, "combat.effect_get")
DD_OFFLINE(EffectStun, effect_field_types)
DD_LAYOUT(EffectHeal, OFFSET, 0xC8, OPT, OFFLINE, "combat.effect_get topic:effect.*")
DD_OFFLINE(EffectHeal, effect_field_types)
DD_LAYOUT(EffectHealPercent, OFFSET, 0xCC, OPT, OFFLINE, "combat.effect_get")
DD_OFFLINE(EffectHealPercent, effect_field_types)
DD_LAYOUT(EffectCombatStatBuff, OFFSET, 0xEC, OPT, OFFLINE, "combat.effect_get")
DD_OFFLINE(EffectCombatStatBuff, effect_field_types)
DD_LAYOUT(EffectDotPoison, OFFSET, 0x134, OPT, OFFLINE, "combat.effect_get")
DD_OFFLINE(EffectDotPoison, effect_field_types)
DD_LAYOUT(EffectDotBleed, OFFSET, 0x138, OPT, OFFLINE, "combat.effect_get")
DD_OFFLINE(EffectDotBleed, effect_field_types)
// .dotBurn、.dotStress：4F02D0 按 lea r8,[rsi+字段]; lea rdx,[键名]; lea rcx,[rbp+0x210]; call 读取函数 逐个解析，解析顺序和
// 结构顺序不同（…duration 0x1CC、dotStress 0x1B8、clearDotStress 0x1BC、dotBurn 0x13C、dot_irresistible 0x140…）。原来的依据只带
// 后一个字段的字面量，前面插入字段时会唯一命中邻居的解析（审核 A 的 evidence_misbind 样本）。U1 修复 FA 收紧成前后两个字段
// 都带字面量：前一个、后一个字段在结构里排在它后面，结构在它之前插入或删除字段时字面量对不上，依据不成立
DD_LAYOUT(EffectDotBurn, OFFSET, 0x13C, OPT, RUNTIME, "combat.effect_get")
DD_EVIDENCE(EffectDotBurn, ParseEffectFile, TARGET, 0, FUNC, 0, "4C 8D 86 BC 01 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 8D 10 02 00 00 E8 ?? ?? ?? ?? 4C 8D 86 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 8D 10 02 00 00 E8 ?? ?? ?? ?? 4C 8D 86 40 01 00 00", 29, U32, 1, NONE, 0)
DD_LAYOUT(EffectDotStress, OFFSET, 0x1B8, OPT, RUNTIME, "combat.effect_get")
DD_EVIDENCE(EffectDotStress, ParseEffectFile, TARGET, 0, FUNC, 0, "4C 8D 86 CC 01 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 8D 10 02 00 00 E8 ?? ?? ?? ?? 4C 8D 86 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 8D 10 02 00 00 E8 ?? ?? ?? ?? 4C 8D 86 BC 01 00 00", 29, U32, 1, NONE, 0)
DD_LAYOUT(EffectDuration, OFFSET, 0x1CC, OPT, OFFLINE, "combat.effect_get")
DD_OFFLINE(EffectDuration, effect_field_types)
DD_LAYOUT(EffectOnHit, OFFSET, 0x345, OPT, RUNTIME, "combat.effect_get")
DD_EVIDENCE(EffectOnHit, ParseEffectFile, TARGET, 0, FUNC, 0, "48 8D 9E ?? ?? ?? ?? 48 89 5C 24 40 4C 8D AE ?? ?? ?? ?? 4C 8D BE ?? ?? ?? ?? ?? ?? ?? ?? 4C 8D B6 ?? ?? ?? ??", 33, U32, 1, NONE, 0)
DD_LAYOUT(EffectOnMiss, OFFSET, 0x346, OPT, RUNTIME, "combat.effect_get")
DD_EVIDENCE(EffectOnMiss, ParseEffectFile, TARGET, 0, FUNC, 0, "48 8D 9E ?? ?? ?? ?? 48 89 5C 24 40 4C 8D AE ?? ?? ?? ?? 4C 8D BE ?? ?? ?? ?? ?? ?? ?? ?? 4C 8D B6 ?? ?? ?? ??", 22, U32, 1, NONE, 0)
DD_LAYOUT(EffectApplyOnce, OFFSET, 0x347, OPT, RUNTIME, "combat.effect_get")
DD_EVIDENCE(EffectApplyOnce, ParseEffectFile, TARGET, 0, FUNC, 0, "48 8D 9E ?? ?? ?? ?? 48 89 5C 24 40 4C 8D AE ?? ?? ?? ?? 4C 8D BE ?? ?? ?? ?? ?? ?? ?? ?? 4C 8D B6 ?? ?? ?? ??", 15, U32, 1, NONE, 0)
DD_LAYOUT(EffectQueue, OFFSET, 0x348, OPT, RUNTIME, "combat.effect_get")
DD_EVIDENCE(EffectQueue, ParseEffectFile, TARGET, 0, FUNC, 0, "48 8D 9E ?? ?? ?? ?? 48 89 5C 24 40 4C 8D AE ?? ?? ?? ?? 4C 8D BE ?? ?? ?? ?? ?? ?? ?? ?? 4C 8D B6 ?? ?? ?? ??", 3, U32, 1, NONE, 0)
DD_LAYOUT(EffectBuffAmount, OFFSET, 0x3B8, OPT, OFFLINE, "combat.effect_get")
DD_OFFLINE(EffectBuffAmount, effect_field_types)
DD_LAYOUT(EffectBuffIds, OFFSET, 0x3C8, OPT, RUNTIME, "combat.effect_get")
DD_EVIDENCE(EffectBuffIds, ParseEffectFile, TARGET, 0, FUNC, 0, "48 8D 8E ?? ?? ?? ?? 48 8B F1", 3, U32, 1, NONE, 0)
// .swap_source_and_target：非零时游戏交换执行者和目标（4C50DF）
DD_LAYOUT(EffectSwap, OFFSET, 0x3E0, OPT, RUNTIME, "combat.effect_get topic:effect.*")
DD_EVIDENCE(EffectSwap, EffectApplyInternal, TARGET, 0, FUNC, 0, "41 0F B6 81 ?? ?? ?? ?? 4C 8B FA", 4, U32, 1, NONE, 0)
DD_OFFLINE(EffectSwap, effect_misc_fields)
DD_LAYOUT(EffectBuffDurationType, OFFSET, 0x430, OPT, OFFLINE, "combat.effect_get")
DD_OFFLINE(EffectBuffDurationType, effect_misc_fields)
DD_LAYOUT(EffectReadSize, SIZE, 0x438, OPT, NONE, "combat.effect_get")
DD_UNVERIFIED(EffectReadSize, "effect_get 一次读出的前缀长度（覆盖到 .buff_duration_type 之后），Effect 对象不小于它；P4-0C 探针和 test_combat 的运行核对")
// ApplyEffect 的结果对象（0x20 字节，+8 是本次加上的 buff guid 的 vector<uint32>）
DD_LAYOUT(EffectApplyResultSize, SIZE, 0x20, OPT, NONE, "combat.effect_apply")
DD_UNVERIFIED(EffectApplyResultSize, "ApplyEffect（4CB4D0）的结果对象：+8 起是 vector<uint32>（EffectApplyResultGuids 有依据），整个对象不超过 0x20；P4-0C 探针的运行核对")
DD_LAYOUT(EffectApplyResultGuids, OFFSET, 0x08, OPT, RUNTIME, "combat.effect_apply")
// ApplyEffect 开头把结果对象清零：mov [rcx],edx; mov [rcx+8],rdx; mov [rcx+0x10],rdx; mov [rcx+0x18],rdx。原来的依据只带后一条的
// 字面量 0x10，结果对象在 +8 之前插入一个指针时会命中新字段；U1 修复 FA 收紧成整段清零（+0 的 dword 到 vector 的 end）
DD_EVIDENCE(EffectApplyResultGuids, ApplyEffect, TARGET, 0, FUNC, 0, "89 11 48 89 51 ?? 48 89 51 10 48 89 51 18", 5, U8, 1, NONE, 0)
// 宽度（U1 修复 FA）：effect_get 按固定宽度读 Effect
DD_LAYOUT(EffectScalarSize, SIZE, 4, OPT, NONE, "combat.effect_get topic:effect.*")
DD_UNVERIFIED(EffectScalarSize, "effect_get 按 int32 读 target、combat_stat_buff、dot_poison、dot_bleed、dot_burn、dot_stress、duration、buff_duration_type，按 float 读 chance、stress、stun、heal、heal_percent、buff_amount，buff_ids 是 vector<uint32>（按 4 字节元素数个数）。4F02D0 按字段类型调用不同的读取函数，effect_field_types 只核对偏移归哪个读取函数")
DD_LAYOUT(EffectFlagSize, SIZE, 1, OPT, NONE, "combat.effect_get topic:effect.*")
DD_UNVERIFIED(EffectFlagSize, "on_hit、on_miss、apply_once、queue、swap_source_and_target 按 1 字节读（EffectApplyInternal 用 movzx 读 swap，EffectSwap 的依据）")

// combat 调用的游戏函数的原型（约定见文件开头）
DD_LAYOUT(ProtoApplyEffect, COUNT, 12, OPT, NONE, "combat.effect_apply")
DD_UNVERIFIED(ProtoApplyEffect, "EffectApplyResult* ApplyEffect(out, prng&, performer, target, const Effect*, float roll, bool crit, float scale, int source, uint32 sourcedata, bool, bool)（4CB4D0），后 8 个在栈上；参数顺序见 knowledge/dd1/combat.md 的「Effect」")
DD_LAYOUT(ProtoPrngFloat, COUNT, 1, OPT, NONE, "combat.effect_apply")
DD_UNVERIFIED(ProtoPrngFloat, "float prng 取随机浮点(prng* rcx)（28FCD0），返回 xmm0")
DD_LAYOUT(ProtoFreeU32Vector, COUNT, 1, OPT, NONE, "combat.effect_apply")
DD_UNVERIFIED(ProtoFreeU32Vector, "void vector<uint32> 的释放(vector* rcx)（243C40），用在 ApplyEffect 结果 +8 上")
DD_LAYOUT(ProtoParseEffectFile, COUNT, 2, OPT, NONE, "combat.effect_define")
DD_UNVERIFIED(ProtoParseEffectFile, "void ParseEffectFile(const char* path rcx, bool network dl)（4F02D0），解析一个 .effects.darkest 文件")

// ============================================================================
// 技能（combat/combat.cpp）
// ============================================================================
DD_LAYOUT(HeroClassSkills, OFFSET, 0xBD0, OPT, NONE, "combat.skill")
DD_UNVERIFIED(HeroClassSkills, "只有运行证据（P4-0C 探针 0B-11：55 次执行的技能指针都在这几张表里），更新后跑 probe_p4 或 test_combat 的技能部分")
DD_LAYOUT(HeroClassMoveSkills, OFFSET, 0xBE8, OPT, NONE, "combat.skill")
DD_UNVERIFIED(HeroClassMoveSkills, "只有运行证据（P4-0C 探针 0B-11：55 次执行的技能指针都在这几张表里），更新后跑 probe_p4 或 test_combat 的技能部分")
DD_LAYOUT(HeroSkillSize, SIZE, 0x510, OPT, NONE, "combat.skill")
DD_UNVERIFIED(HeroSkillSize, "只有运行证据（P4-0C 探针 0B-11：55 次执行的技能指针都在这几张表里），更新后跑 probe_p4 或 test_combat 的技能部分")
DD_LAYOUT(MonsterClassSkills, OFFSET, 0xBA8, OPT, NONE, "combat.skill")
DD_UNVERIFIED(MonsterClassSkills, "只有运行证据（P4-0C 探针 0B-11：55 次执行的技能指针都在这几张表里），更新后跑 probe_p4 或 test_combat 的技能部分")
DD_LAYOUT(MonsterSkillSize, SIZE, 0x500, OPT, NONE, "combat.skill")
DD_UNVERIFIED(MonsterSkillSize, "只有运行证据（P4-0C 探针 0B-11：55 次执行的技能指针都在这几张表里），更新后跑 probe_p4 或 test_combat 的技能部分")
DD_LAYOUT(SkillId, OFFSET, 0x08, OPT, NONE, "combat.skill")
DD_UNVERIFIED(SkillId, "技能对象的 id char[64] 在 +8、等级 +0x48、类型 +0x50、friendly +0x90（knowledge/dd1/combat.md 的「技能」）；P4-0C 探针和 test_combat 的技能部分")
DD_LAYOUT(SkillLevel, OFFSET, 0x48, OPT, NONE, "combat.skill")
DD_UNVERIFIED(SkillLevel, "技能对象的 id char[64] 在 +8、等级 +0x48、类型 +0x50、friendly +0x90（knowledge/dd1/combat.md 的「技能」）；P4-0C 探针和 test_combat 的技能部分")
DD_LAYOUT(SkillType, OFFSET, 0x50, OPT, NONE, "combat.skill")
DD_UNVERIFIED(SkillType, "技能对象的 id char[64] 在 +8、等级 +0x48、类型 +0x50、friendly +0x90（knowledge/dd1/combat.md 的「技能」）；P4-0C 探针和 test_combat 的技能部分")
DD_LAYOUT(SkillFriendly, OFFSET, 0x90, OPT, NONE, "combat.skill")
DD_UNVERIFIED(SkillFriendly, "技能对象的 id char[64] 在 +8、等级 +0x48、类型 +0x50、friendly +0x90（knowledge/dd1/combat.md 的「技能」）；P4-0C 探针和 test_combat 的技能部分")
DD_LAYOUT(SkillEffects, OFFSET, 0x3C8, OPT, OFFLINE, "combat.skill")
DD_OFFLINE(SkillEffects, skill_effects_offset)
DD_LAYOUT(SkillScalarSize, SIZE, 4, OPT, NONE, "combat.skill")
DD_UNVERIFIED(SkillScalarSize, "skill_info 按 int32 读等级（SkillLevel），按 1 字节读 friendly（SkillFriendly），类型（SkillType）是 char[64]（GameTextCap），effects 是指针的 vector；P4-0C 探针和 test_combat 的技能部分")

// ============================================================================
// 枚举表和游戏数据表（combat/logic.cpp）
// ============================================================================
// Raid::Current+0x52C0：模式名，PvP 是 arena_mp（ADR 0011）
DD_LAYOUT(RaidPvpName, OFFSET, 0x52C0, OPT, RUNTIME, "pvp")
DD_EVIDENCE(RaidPvpName, RaidCurrent, HIT, 0, ENTRY, 0, "48 8B 05 ?? ?? ?? ?? 48 8B D9 48 8B 88 ?? ?? ?? ??", 13, U32, 1, NONE, 0)

// 四张枚举表：顺序就是游戏里的数值（stat_type、source、duration_type、rule_type），静态初始化里 strcpy_s 填的表。
// 游戏里来源名带前缀 bsrc_，这里不带
DD_LAYOUT(EnumStat, TABLE, 102, OPT, OFFLINE, "combat.enum combat.buff_define combat.buff_read")
DD_NAMES(EnumStat,
    "status", "tag", "combat_stat_add", "combat_stat_multiply", "hp_dot_poison", "hp_dot_bleed",
    "hp_heal_amount", "hp_heal_percent", "hp_heal_received_percent", "stress_dmg_percent", "stress_heal_percent", "resistance",
    "poison_chance", "bleed_chance", "stun_chance", "move_chance", "debuff_chance", "resolve_check_percent",
    "resolve_xp_bonus_percent", "scouting_chance", "remove_negative_quirk_chance", "food_consumption_percent", "starving_damage_percent", "upgrade_discount",
    "party_surprise_chance", "monsters_surprise_chance", "riposte", "guarded", "damage_received_percent", "stress_heal_received_percent",
    "stress_dmg_received_percent", "vampire", "activity_side_effect_chance", "disable_combat_skill_attribute", "stress_dot", "shuffle_dot",
    "hp_dot_heal", "torchlight_burn_percent", "quirk_evolution_death_immune", "vampire_evolution_duration", "ambush_chance", "tag_blocked",
    "ignore_protection", "stealth", "guard_blocked", "crit_received_chance", "damage_reflect_percent", "shard_reward_percent",
    "shard_consume_percent", "cure_poison_chance", "cure_bleed_chance", "cure_poison_received_chance", "cure_bleed_received_chance", "hp_dot_poison_amount_percent",
    "hp_dot_poison_amount_received_percent", "hp_dot_poison_duration_percent", "hp_dot_poison_duration_received_percent", "hp_dot_bleed_amount_percent", "hp_dot_bleed_amount_received_percent", "hp_dot_bleed_duration_percent",
    "hp_dot_bleed_duration_received_percent", "stress_dot_amount_percent", "stress_dot_amount_received_percent", "stress_dot_duration_percent", "stress_dot_duration_received_percent", "shuffle_dot_duration_percent",
    "shuffle_dot_duration_received_percent", "hp_heal_dot_amount_percent", "hp_heal_dot_amount_received_percent", "hp_heal_dot_duration_percent", "hp_heal_dot_duration_received_percent", "torch_decrease_percent",
    "torch_increase_percent", "ignore_stealth", "transfer_buff_from_attacker_chance", "transfer_debuff_from_attacker_chance", "random_target_friendly_chance", "random_target_attack_chance",
    "guard_duration_percent", "guard_duration_received_percent", "stress_on_miss", "stress_from_idle_in_town", "quirk_tag_evolution_duration", "deathblow_chance",
    "heartattack_stress_heal_percent", "riposte_duration_percent", "buff_duration_percent", "ignore_guard", "hp_dot_burn", "hp_dot_burn_amount_percent",
    "hp_dot_burn_amount_received_percent", "wildfire_apply_adjacent_targets", "hp_dot_burn_decay_percent", "cure_burn_chance", "cure_burn_received_chance", "effect_chance",
    "crit_dmg_multiply", "crit_dmg_add", "hp_heal_amount_received", "heal_crit_chance_add", "heal_crit_chance_multiply", "disable_combat_skill")
DD_OFFLINE(EnumStat, enum_stat)
DD_LAYOUT(EnumSource, TABLE, 34, OPT, OFFLINE, "combat.enum combat.buff_define combat.buff_add")
DD_NAMES(EnumSource,
    "notspecified", "skill", "instantSkill", "affliction", "virtue", "quirk",
    "trinket", "trinket_set", "campingskill", "deathsdoor", "town", "curio",
    "stun", "guard", "riposte", "item", "disease", "companion",
    "deathsdoor_recovery", "deathsdoor_recovery_heart_attack", "quest_failure", "completed_darkest_dungeon_quest_party_hero", "town_event", "vampire",
    "never_again", "battle_modifier", "district", "torchsettings", "flashback_start", "flashback_result",
    "crit", "trinket_additional_effect", "quest_modifier", "last_hero")
DD_OFFLINE(EnumSource, enum_source)
DD_LAYOUT(EnumDuration, TABLE, 11, OPT, OFFLINE, "combat.enum combat.buff_define combat.buff_add")
DD_NAMES(EnumDuration,
    "combat_end", "quest_complete", "quest_not_complete", "quest_end", "activity_end", "idle_start_town_visit",
    "till_removed", "none", "before_turn", "after_turn", "after_round")
DD_OFFLINE(EnumDuration, enum_duration)
DD_LAYOUT(EnumRule, TABLE, 55, OPT, OFFLINE, "combat.enum combat.buff_define combat.rule")
DD_NAMES(EnumRule,
    "always", "monsterSize", "lightbelow", "lightabove", "hpbelow", "hpabove",
    "stress_below", "stress_above", "afflicted", "virtued", "meleeonly", "rangedonly",
    "firstroundonly", "actorStatus", "monsterType", "at_deaths_door", "in_rank", "target_in_rank",
    "has_buff", "in_camp", "in_dungeon", "in_room", "in_corridor", "walking_backwards",
    "in_activity", "riposte", "skill", "in_mode", "actor_base_class", "is_vampire",
    "target_is_vampire", "has_quirk", "monster_type_count_min", "is_guarded", "target_is_guarded", "is_stealthed",
    "target_is_stealthed", "has_item_id", "has_item_type", "is_guarding", "target_is_guarding", "torch_setting",
    "is_actor_status", "target_hpbelow", "target_hpabove", "attacking_monster_type", "no_trinkets", "activated_first",
    "activated_last", "ondeathsdoorenter", "negative_quirk_above", "quest_uses_remaining_above", "target_is_burning", "any_is_burning",
    "is_target_corpse")
DD_OFFLINE(EnumRule, enum_rule)
// 来源的标志（下标是来源的数值）：来源表每项 +0x44 进存档（SRC_S）、+0x46 回城清（SRC_T）、+0x48 进 tray（SRC_Y）
DD_LAYOUT(SourceFlags, TABLE, 34, OPT, OFFLINE, "combat.enum combat.buff_read")
DD_VALUES(SourceFlags,
    SRC_T, SRC_S | SRC_T | SRC_Y, SRC_S | SRC_T, 0, SRC_T, 0,
    0, 0, SRC_S | SRC_T | SRC_Y, SRC_T, SRC_S | SRC_T | SRC_Y, SRC_S | SRC_T | SRC_Y,
    SRC_S | SRC_T | SRC_Y, SRC_S | SRC_T, SRC_S | SRC_T, SRC_S | SRC_T | SRC_Y, 0, SRC_Y,
    SRC_T, SRC_T, SRC_S | SRC_T, SRC_T, SRC_S, SRC_S,
    SRC_T, SRC_S | SRC_T, SRC_S, SRC_T, SRC_S | SRC_T, SRC_S | SRC_T | SRC_Y,
    SRC_S | SRC_T | SRC_Y, SRC_S | SRC_T | SRC_Y, SRC_T, SRC_T)
DD_OFFLINE(SourceFlags, source_flags)
// stat_sub_type 的合法值（P4 审核 C-Y3、A-N4，[静态]）：游戏在属性累加（47AD50）和 buff 提示（7C6100 等）里按哈希查这些表，
// 认不出时有的弹 assert、有的越界写到相邻的累加值、有的只写日志。注释里是 Steam 版的表地址和查找函数
// 2AAE800，490810 / 47DB60
DD_LAYOUT(SubStatCombat, TABLE, 10, OPT, NONE, "combat.buff_define")
DD_NAMES(SubStatCombat,
    "damage_low", "damage_high", "defense_rating", "protection_rating", "speed_rating", "attack_rating",
    "crit_chance", "max_hp", "riposte_on_hit_chance", "riposte_on_miss_chance")
DD_UNVERIFIED(SubStatCombat, "游戏的名字表（静态初始化里填，注释里是 Steam 版地址），knowledge/dd1/combat.md 的「Buff」；更新后对照 Ghidra 或 dd_disasm 的 s 命令核对")
// 2AE5330，51B480
DD_LAYOUT(SubStatResistance, TABLE, 8, OPT, NONE, "combat.buff_define")
DD_NAMES(SubStatResistance,
    "stun", "move", "poison", "bleed", "disease", "debuff",
    "death_blow", "trap")
DD_UNVERIFIED(SubStatResistance, "游戏的名字表（静态初始化里填，注释里是 Steam 版地址），knowledge/dd1/combat.md 的「Buff」；更新后对照 Ghidra 或 dd_disasm 的 s 命令核对")
// 2AA2940，44F5F0
DD_LAYOUT(SubStatSideEffect, TABLE, 8, OPT, NONE, "combat.buff_define")
DD_NAMES(SubStatSideEffect,
    "go_missing", "add_quirk", "apply_buff", "add_currency", "remove_currency", "activity_lock",
    "add_trinket", "remove_trinket")
DD_UNVERIFIED(SubStatSideEffect, "游戏的名字表（静态初始化里填，注释里是 Steam 版地址），knowledge/dd1/combat.md 的「Buff」；更新后对照 Ghidra 或 dd_disasm 的 s 命令核对")
// 2AAE650，2AAE670 的线性查找
DD_LAYOUT(SubStatSkillAttribute, TABLE, 12, OPT, NONE, "combat.buff_define")
DD_NAMES(SubStatSkillAttribute,
    "stress", "stun", "heal", "bleed", "poison", "tag",
    "guard", "move", "disease", "buff", "debuff", "daze")
DD_UNVERIFIED(SubStatSkillAttribute, "游戏的名字表（静态初始化里填，注释里是 Steam 版地址），knowledge/dd1/combat.md 的「Buff」；更新后对照 Ghidra 或 dd_disasm 的 s 命令核对")
// 2AE58F0，下标 0 原文就是 unkown
DD_LAYOUT(SubStatStressSource, TABLE, 24, OPT, NONE, "combat.buff_define")
DD_NAMES(SubStatStressSource,
    "unkown", "debug", "activity", "town_idle", "quest_fail", "pass",
    "hunger", "effect", "hero_crit", "monster_crit", "hero_killing_blow", "hero_death",
    "mode", "control", "capture", "camping_relieve_stress", "camping_eat", "camping_skill",
    "deaths_door", "tile", "retreat", "curio", "flashback", "town_event")
DD_UNVERIFIED(SubStatStressSource, "游戏的名字表（静态初始化里填，注释里是 Steam 版地址），knowledge/dd1/combat.md 的「Buff」；更新后对照 Ghidra 或 dd_disasm 的 s 命令核对")
// 2AC85D0，4BDF00
DD_LAYOUT(SubStatHealSource, TABLE, 15, OPT, NONE, "combat.buff_define")
DD_NAMES(SubStatHealSource,
    "hero_skill", "hero_skill_multi_target", "monster_skill", "monster_skill_multi_target", "camp_skill", "camp_skill_multi_target",
    "effect", "act_out", "curio", "eat", "companion", "debug",
    "dot", "flashback", "damage_heal")
DD_UNVERIFIED(SubStatHealSource, "游戏的名字表（静态初始化里填，注释里是 Steam 版地址），knowledge/dd1/combat.md 的「Buff」；更新后对照 Ghidra 或 dd_disasm 的 s 命令核对")
// 479CD0 的规则 13/42 在 479F41 起按这些内置状态名的哈希判断（[静态]，A-N3），插件不能注册同名（或同哈希）的规则
DD_LAYOUT(BuiltinStatus, TABLE, 8, OPT, NONE, "combat.rule")
DD_NAMES(BuiltinStatus,
    "stunned", "tagged", "poisoned", "bleeding", "burning", "virtued",
    "afflicted", "dazed")
DD_UNVERIFIED(BuiltinStatus, "479CD0（BuffRuleActive）里按这些名字的哈希比较的立即数，更新后在 dd_disasm 里核对")
// .target 的合法值：4F02D0 在 21 项的表（2AD58F0，静态初始化 9C560）里找，找不到时弹 assert「EffectTarget: Invalid type」
DD_LAYOUT(EffectTargets, TABLE, 21, OPT, NONE, "combat.effect_define")
DD_NAMES(EffectTargets,
    "performer", "target", "performer_group", "performer_group_other", "target_group", "target_group_other",
    "target_enemy_group", "target_enemy_rank", "target_enemy_random", "target_hero_random", "this_trinket", "global",
    "performer_adjacent_random", "performer_adjacent_front", "performer_adjacent_behind", "performer_adjacents", "target_adjacent_random", "target_adjacent_front",
    "target_adjacent_behind", "target_adjacents", "other_trinket")
DD_UNVERIFIED(EffectTargets, "游戏的 EffectTarget 名字表（2AD58F0，静态初始化 9C560），更新后对照核对")
// .effects.darkest 列表字段的项数：4F02D0 读 .kill_enemy_types、.summon_monsters、.summon_ranks（各 4 个）和 .buff_ids、
// .set_monster_class_ids（各 8 个）；某一项以块的最后一个字符（一个引号）开头时游戏算出 0 的缓冲大小交给 strncpy_s，进程被终止
DD_LAYOUT(EffectListKillEnemyTypes, COUNT, 4, OPT, NONE, "combat.effect_define")
DD_UNVERIFIED(EffectListKillEnemyTypes, "4F02D0 的反编译（knowledge/dd1/combat.md 的「Effect」），更新后对照核对")
DD_LAYOUT(EffectListBuffIds, COUNT, 8, OPT, NONE, "combat.effect_define")
DD_UNVERIFIED(EffectListBuffIds, "4F02D0 的反编译（knowledge/dd1/combat.md 的「Effect」），更新后对照核对")
DD_LAYOUT(EffectListSummonMonsters, COUNT, 4, OPT, NONE, "combat.effect_define")
DD_UNVERIFIED(EffectListSummonMonsters, "4F02D0 的反编译（knowledge/dd1/combat.md 的「Effect」），更新后对照核对")
DD_LAYOUT(EffectListSummonRanks, COUNT, 4, OPT, NONE, "combat.effect_define")
DD_UNVERIFIED(EffectListSummonRanks, "4F02D0 的反编译（knowledge/dd1/combat.md 的「Effect」），更新后对照核对")
DD_LAYOUT(EffectListSetMonsterClassIds, COUNT, 8, OPT, NONE, "combat.effect_define")
DD_UNVERIFIED(EffectListSetMonsterClassIds, "4F02D0 的反编译（knowledge/dd1/combat.md 的「Effect」），更新后对照核对")
// 块的最大长度：2904D0 用 strncpy_s(reader, 0x400, 块, 块长) 复制，块长 >= 0x400 时进程被 CRT 终止
DD_LAYOUT(EffectBlockMax, SIZE, 0x3FF, OPT, NONE, "combat.effect_define")
DD_UNVERIFIED(EffectBlockMax, "2904D0 的 strncpy_s 缓冲大小 0x400 减 1（knowledge/dd1/combat.md 的「Effect」），更新后对照核对")
// 游戏的字符串哈希：h = h * 53 + c（Buff 库、effect map、规则名都按它查）
DD_LAYOUT(GameHashMul, CONST, 53, OPT, NONE, "combat.buff_read combat.buff_add combat.buff_remove combat.buff_define combat.effect_get combat.effect_apply combat.effect_define combat.rule")
DD_UNVERIFIED(GameHashMul, "游戏的哈希函数（knowledge/dd1/combat.md）；查 Buff 库和 effect map 的运行核对（test_combat）对不上时全部找不到")
// Buff、Effect、技能的 id 和字符串字段是 char[64]（SDK 的 DD_COMBAT_TEXT 和它相等）
DD_LAYOUT(GameTextCap, SIZE, 0x40, OPT, NONE, "combat.buff_read combat.buff_add combat.buff_define combat.effect_get combat.skill combat.rule")
DD_UNVERIFIED(GameTextCap, "Instance 的 sub_type（+0x04）到它的哈希（+0x44）、id（+0x61）到哈希（+0xA4）都隔 0x40 字节（knowledge/dd1/combat.md 的「Buff」）")

// ============================================================================
// 事件系统（events/system.cpp），依据见 knowledge/dd1/events.md
// ============================================================================
// Hero/Monster 虚表槽位：36 压力、50 伤害（ApplyHealthDamageInternal）、51 治疗、40 在死门上（HP == 0）、43 已死
// 槽位 36 压力（Hero 4814B0、Monster 5FAEA0）算完增量后调用 ActorStressInternal（480FE0，往 +0x12FC 加压力的那个函数）：
//   Hero     movaps xmm2,xmm0; mov rdx,rsi; mov rcx,rbx; call 480FE0; movaps xmm9,xmm0
//   Monster  mov rcx,rbx; call 480FE0; movaps xmm8,xmm0; movaps xmm0,xmm8
// 依据（U1 修复 FA，审核 A 的 S2）读 call 的目标；槽号整体后移、或者槽 36 换成别的虚函数时不成立
DD_LAYOUT(VtSlotStress, SLOT, 36, OPT, RUNTIME, "topic:stress.*")
DD_EVIDENCE(VtSlotStress, HeroVtable, VSLOT_VALUE, 0, FUNC, 0, "0F 28 D0 48 8B D6 48 8B CB E8 ?? ?? ?? ?? 44 0F 28 C8", 10, REL32, 0, ActorStressInternal, 0)
DD_EVIDENCE(VtSlotStress, MonsterVtable, VSLOT_VALUE, 0, FUNC, 0, "48 8B CB E8 ?? ?? ?? ?? 44 0F 28 C0 41 0F 28 C0", 4, REL32, 0, ActorStressInternal, 0)
DD_LAYOUT(VtSlotDamage, SLOT, 50, OPT, RUNTIME, "topic:damage.*")
DD_EVIDENCE(VtSlotDamage, HeroVtable, VSLOT_VALUE, 0, FUNC, 0, "E8 ?? ?? ?? ?? 45 0F 57 C9 84 C0 74", 1, REL32, 0, ConsumeHealthDamageBlock, 0)
DD_EVIDENCE(VtSlotDamage, MonsterVtable, VSLOT_VALUE, 0, FUNC, 0, "84 C0 74 ?? 45 33 C9 41 B8 31 00 00 00", 0, NONE, 0, NONE, 0)
DD_LAYOUT(VtSlotHeal, SLOT, 51, OPT, RUNTIME, "topic:heal.*")
DD_EVIDENCE(VtSlotHeal, HeroVtable, VSLOT_VALUE, 0, FUNC, 0, "48 8D 8B ?? ?? ?? ?? 41 B0 01 8B 93 ?? ?? ?? ?? E8", 3, U32, 0, ActorMaxHpStat, 0)
// 上一条在伤害（槽位 50）里也有，槽位 50、51 互换时照样成立。治疗（483C70）取最大生命之后 hp += min(治疗量, 最大生命 - hp)：
//   call ActorMaxStat; subss xmm0,xmm2; movaps xmm7,xmm8; minss xmm7,xmm0; addss xmm2,xmm7; movss [rbx+0x10A8],xmm2
// 伤害里没有这一串（U1 修复 FA，审核 A 的 S2），读写回生命的位移
DD_EVIDENCE(VtSlotHeal, HeroVtable, VSLOT_VALUE, 0, FUNC, 0, "E8 ?? ?? ?? ?? F3 0F 5C C2 41 0F 28 F8 F3 0F 5D F8 F3 0F 58 D7 F3 0F 11 93 ?? ?? ?? ??", 25, U32, 0, ActorHp, 0)
DD_LAYOUT(VtSlotDeathsDoor, SLOT, 40, OPT, RUNTIME, "topic:damage.final")
DD_EVIDENCE(VtSlotDeathsDoor, HeroVtable, VSLOT_VALUE, 0, ENTRY, 0, "F3 0F 10 89 ?? ?? ?? ?? 0F 57 C0 0F 2E C8 7A ?? 75", 0, NONE, 0, NONE, 0)
DD_LAYOUT(VtSlotDead, SLOT, 43, OPT, RUNTIME, "topic:damage.final")
DD_EVIDENCE(VtSlotDead, HeroVtable, VSLOT_VALUE, 0, ENTRY, 0, "0F 57 C0 0F 2F 81 ?? ?? ?? ?? 77 ?? 80 B9", 0, NONE, 0, NONE, 0)
// 怪物受伤（5FC3E0）在 480400 返回假之后：[[Monster+0x1468]+0xF97]（MonsterClass 的 .can_be_damaged_directly）为 0 且
// 伤害类型在免疫表里时不扣血；表从 MonsterBlockReturn 命中位置 +52 的 lea 位移解出（+56 是下一条指令），只有 20 种伤害类型有意义
DD_LAYOUT(MonsterClassCanBeDamagedDirectly, OFFSET, 0xF97, OPT, RUNTIME, "topic:damage.final")
DD_EVIDENCE(MonsterClassCanBeDamagedDirectly, MonsterBlockReturn, HIT, 0, WINDOW, 64, "48 8B 87 ?? ?? ?? ?? 80 B8 ?? ?? ?? ?? 00 75", 9, U32, 1, NONE, 0)
DD_LAYOUT(MonsterBlockImmuneLeaDisp, OFFSET, 52, OPT, RUNTIME, "topic:damage.final")
DD_EVIDENCE(MonsterBlockImmuneLeaDisp, MonsterBlockReturn, HIT_VALUE, -3, ENTRY, 0, "48 8D 0D ?? ?? ?? ?? 80 3C 08 00", 0, NONE, 0, NONE, 0)
DD_LAYOUT(MonsterBlockImmuneLeaNext, OFFSET, 56, OPT, RUNTIME, "topic:damage.final")
DD_EVIDENCE(MonsterBlockImmuneLeaNext, MonsterBlockReturn, HIT_VALUE, -7, ENTRY, 0, "48 8D 0D ?? ?? ?? ?? 80 3C 08 00", 0, NONE, 0, NONE, 0)
DD_LAYOUT(MonsterImmuneTypeCount, COUNT, 20, OPT, OFFLINE, "topic:damage.final")
DD_OFFLINE(MonsterImmuneTypeCount, monster_immune_table)
// 4836A0 扣血前和最终伤害比较的阈值（0xEE61E0 处的 float 1e-6，这里是位模式）
DD_LAYOUT(DamageEpsilonBits, CONST, 0x358637BD, OPT, RUNTIME, "topic:damage.final")
DD_EVIDENCE(DamageEpsilonBits, HeroVtable, VSLOT, 50, FUNC, 0, "45 32 ED 0F 2F 3D ?? ?? ?? ?? 0F 86", 6, DEREF_U32, 1, NONE, 0)
// Battle 的回合数（int）
DD_LAYOUT(BattleRound, OFFSET, 0xA8, OPT, RUNTIME, "topic:battle.round.*")
DD_EVIDENCE(BattleRound, BattleAdvanceRound, TARGET, 0, FUNC, 0, "FF 83 ?? ?? ?? ??", 2, U32, 1, NONE, 0)
// 命中结算（ApplyHit / ApplyMiss）的参数：攻击数据 +0x20 执行者、+0x28 技能；目标数据 +0 目标、+8 伤害（float）、
// +0x10 是否命中、+0x11 是否暴击
DD_LAYOUT(AttackPerformer, OFFSET, 0x20, OPT, RUNTIME, "topic:hit.*")
DD_EVIDENCE(AttackPerformer, ApplyHit, TARGET, 0, FUNC, 0, "4C 8B 62 ?? 4C 89 64 24", 3, U8, 1, NONE, 0)
DD_EVIDENCE(AttackPerformer, ApplyMiss, TARGET, 0, FUNC, 0, "48 8B 6A ?? 49 8B 18", 3, U8, 1, NONE, 0)
DD_LAYOUT(AttackSkill, OFFSET, 0x28, OPT, RUNTIME, "topic:hit.*")
DD_EVIDENCE(AttackSkill, ApplyHit, TARGET, 0, FUNC, 0, "49 8B 46 ??", 3, U8, 1, NONE, 0)
DD_LAYOUT(TargetActor, OFFSET, 0x00, OPT, RUNTIME, "topic:hit.*")
DD_EVIDENCE(TargetActor, ApplyHit, TARGET, 0, FUNC, 0, "4C 89 64 24 ?? 49 8B 30 41 80 78", 0, NONE, 0, NONE, 0)
DD_EVIDENCE(TargetActor, ApplyMiss, TARGET, 0, FUNC, 0, "48 8B 6A 20 49 8B 18", 0, NONE, 0, NONE, 0)
DD_LAYOUT(TargetDamage, OFFSET, 0x08, OPT, RUNTIME, "topic:hit.*")
DD_EVIDENCE(TargetDamage, ApplyHit, TARGET, 0, FUNC, 0, "F3 0F 10 57 ??", 4, U8, 1, NONE, 0)
DD_LAYOUT(TargetHit, OFFSET, 0x10, OPT, NONE, "topic:hit.*")
DD_UNVERIFIED(TargetHit, "目标数据 +0x10 是否命中，决定走 ApplyHit 还是 ApplyMiss（knowledge/dd1/events.md），test_events 的 hit.* 运行核对")
DD_LAYOUT(TargetCrit, OFFSET, 0x11, OPT, RUNTIME, "topic:hit.*")
DD_EVIDENCE(TargetCrit, ApplyHit, TARGET, 0, FUNC, 0, "49 8B 30 41 80 78 ?? 00", 6, U8, 1, NONE, 0)
// 宽度（U1 修复 FA）
DD_LAYOUT(HitDataScalarSize, SIZE, 4, OPT, NONE, "topic:hit.*")
DD_UNVERIFIED(HitDataScalarSize, "目标数据的伤害（TargetDamage）按 float 读（movss，TargetDamage 的依据就是这条），是否命中、是否暴击（TargetHit、TargetCrit）按 1 字节读（cmp byte）；test_events 的 hit.* 运行核对")
DD_LAYOUT(BattleIntSize, SIZE, 4, OPT, NONE, "game.battle game.actor topic:battle.round.*")
DD_UNVERIFIED(BattleIntSize, "Battle 的状态（BattleState）、回合数（BattleRound）、怪物 guid 计数（BattleGuidCounter）按 int32 读；后两项的依据是 32 位的 inc dword，宽度变了时模式对不上")

// 语义 hook 的原型（约定见文件开头；类型在 events/system.cpp 的 DamageFn 等）。装 hook 和改虚表槽按 ADR 0036 算「写」，
// 主题本身只读也一样（U1 修复 FA，审核 A 的 S1）
DD_LAYOUT(ProtoActorDamage, COUNT, 8, OPT, NONE, "topic:damage.*")
DD_UNVERIFIED(ProtoActorDamage, "Hero/Monster 槽位 50 ApplyHealthDamageInternal：void* (Actor*, prng*, float 伤害 xmm2, const int* 伤害类型, uint32 来源, const int* 来源类型, bool 暴击, void* 时间线)（Hero 4836A0，knowledge/dd1/events.md），detour 按 8 个参数转发")
DD_LAYOUT(ProtoActorHeal, COUNT, 8, OPT, NONE, "topic:heal.*")
DD_UNVERIFIED(ProtoActorHeal, "Hero/Monster 槽位 51 治疗：float (Actor*, prng*, float 治疗量 xmm2, Actor* 治疗者 r9, int 来源, bool 可暴击, void* 输出, bool)，Hero 483C70 读第 5–8 个参数（[rsp+0x140]、+0x148、+0x150、+0x158），detour 按 8 个参数转发")
DD_LAYOUT(ProtoActorStress, COUNT, 5, OPT, NONE, "topic:stress.*")
DD_UNVERIFIED(ProtoActorStress, "Hero/Monster 槽位 36 压力：float (Actor*, prng*, float 压力 xmm2, void* 来源 r9, int 来源类型)，Hero 4814B0 读第 5 个参数（[rsp+0xB0]），detour 按 5 个参数转发")
DD_LAYOUT(ProtoActorPredicate, COUNT, 1, OPT, NONE, "topic:damage.final")
DD_UNVERIFIED(ProtoActorPredicate, "damage.final 调用 Hero/Monster 槽位 40（在死门上）、43（已死）：bool (Actor* rcx)，叶函数（VtSlotDeathsDoor、VtSlotDead 的依据核对的是函数开头）")
DD_LAYOUT(ProtoConsumeHealthDamageBlock, COUNT, 3, OPT, NONE, "topic:damage.final")
DD_UNVERIFIED(ProtoConsumeHealthDamageBlock, "damage.final 的 hook：bool Actor::ConsumeHealthDamageBlock(Actor* rcx, int 伤害类型 edx, float 伤害 xmm2)（480400），detour 按 3 个参数转发")
DD_LAYOUT(ProtoEffectApplyInternal, COUNT, 13, OPT, NONE, "topic:effect.*")
DD_UNVERIFIED(ProtoEffectApplyInternal, "effect.* 的 hook：void EffectApplyInternal(prng*, performer, target, const Effect*, item, float, bool, float, int, uint32, void*, bool, bool)（4C5050），detour 按 13 个参数转发")
DD_LAYOUT(ProtoApplyHit, COUNT, 3, OPT, NONE, "topic:hit.*")
DD_UNVERIFIED(ProtoApplyHit, "hit.* 的 hook：void ApplyHit(Battle* rcx, AttackData* rdx, TargetData* r8)（5BA3F0；AttackPerformer、TargetActor 的依据读的就是 rdx、r8 指向的结构），detour 按 3 个参数转发")
DD_LAYOUT(ProtoApplyMiss, COUNT, 3, OPT, NONE, "topic:hit.*")
DD_UNVERIFIED(ProtoApplyMiss, "hit.* 的 hook：void ApplyMiss(Battle* rcx, AttackData* rdx, TargetData* r8)（5BAE30），和 ApplyHit 同形，detour 按 3 个参数转发")
DD_LAYOUT(ProtoActorDoAttackSkill, COUNT, 4, OPT, NONE, "topic:skill.*")
DD_UNVERIFIED(ProtoActorDoAttackSkill, "skill.* 的 hook：void ActorDoAttackSkill(Battle*, Actor* 执行者, const Skill*, vector* 目标)（5BE500），detour 按 4 个参数转发")
DD_LAYOUT(ProtoActorDoSkillFriendly, COUNT, 5, OPT, NONE, "topic:skill.*")
DD_UNVERIFIED(ProtoActorDoSkillFriendly, "skill.* 的 hook：void ActorDoSkillFriendly(Battle*, Actor* 执行者, const Skill*, vector* 目标, bool)（5BF440），detour 按 5 个参数转发")
DD_LAYOUT(ProtoBattleAdvanceRound, COUNT, 1, OPT, NONE, "topic:battle.round.*")
DD_UNVERIFIED(ProtoBattleAdvanceRound, "battle.round.* 的 hook：void Battle::AdvanceRound(Battle* rcx)（5A64B0），detour 按 1 个参数转发")
DD_LAYOUT(ProtoCombatStartTurn, COUNT, 3, OPT, NONE, "topic:battle.turn.*")
DD_UNVERIFIED(ProtoCombatStartTurn, "battle.turn.* 的 hook：int CombatStartTurn(behaviors*, prng*, Actor*)（4A5470），detour 按 3 个参数转发")
// 原生事件的类名表：0x80 字节一项（Factory<X,128> 和 Factory<X,64> 都是，strcpy_s 的缓冲大小）；Query 最多 64 种（Factory<Query,64>）
DD_LAYOUT(ClassNameStride, SIZE, 0x80, REQUIRED, RUNTIME, "core events.native events.query")
DD_EVIDENCE(ClassNameStride, EventClassNames, HIT, 0, WINDOW, 128, "4C 8B C3 BA ?? ?? ?? ?? 48 89 84 FE", 4, U32, 1, NONE, 0)
DD_EVIDENCE(ClassNameStride, QueryClassNames, HIT, 0, WINDOW, 128, "4C 8B C3 BA ?? ?? ?? ?? 48 89 84 FE", 4, U32, 1, NONE, 0)
DD_LAYOUT(QueryClassCapacity, COUNT, 64, OPT, RUNTIME, "events.query")
DD_EVIDENCE(QueryClassCapacity, QueryClassNames, HIT, 0, ENTRY, 0, "83 38 ?? 7C", 2, U8, 1, NONE, 0)
// listener 列表：EventManager / QueryDispatcher 的 +0 是 ListData 数组（按类型序号，0x20 字节一项：first、last、end、dirty）；
// 列表项 EventListenerDataContainer 0x28 字节（+8 listener，+0x10 active），Query 的 BaseListenerDataContainer 0x18 字节。
// EventTypeCount 最低的命中在 listener 的析构（361730）里，后面紧跟着按序号取 ListData（shl 5）、把匹配的表项 active 清零、
// 置 dirty 的循环（DDCore 注销 listener 的做法照抄它）。离线版最低的命中是同一模板的另一个实例，循环相同
DD_LAYOUT(ListDataSize, SIZE, 0x20, REQUIRED, RUNTIME, "core events.native events.query")
DD_EVIDENCE(ListDataSize, EventTypeCount, HIT, 0, WINDOW, 128, "48 8B D7 48 C1 E2 05 48 03 16", 0, NONE, 0, NONE, 0)
DD_LAYOUT(ListDataDirty, OFFSET, 0x18, REQUIRED, RUNTIME, "core events.native events.query")
DD_EVIDENCE(ListDataDirty, EventTypeCount, HIT, 0, WINDOW, 128, "48 39 58 ?? 75 ?? C6 40 ?? 00 C6 42 ?? 01 48 83 C0 ??", 12, U8, 1, NONE, 0)
DD_EVIDENCE(ListDataDirty, ListenerAdd, TARGET, 0, FUNC, 0, "C6 43 ?? 01 48 8B 43 08", 2, U8, 1, NONE, 0)
DD_LAYOUT(ListenerEntrySize, SIZE, 0x28, REQUIRED, RUNTIME, "core events.native")
DD_EVIDENCE(ListenerEntrySize, EventTypeCount, HIT, 0, WINDOW, 128, "48 39 58 ?? 75 ?? C6 40 ?? 00 C6 42 ?? 01 48 83 C0 ??", 17, U8, 1, NONE, 0)
DD_EVIDENCE(ListenerEntrySize, ListenerAdd, TARGET, 0, FUNC, 0, "48 83 43 08 ?? EB", 4, U8, 1, NONE, 0)
DD_LAYOUT(ListenerEntryListener, OFFSET, 0x08, REQUIRED, RUNTIME, "core events.native events.query")
DD_EVIDENCE(ListenerEntryListener, EventTypeCount, HIT, 0, WINDOW, 128, "48 39 58 ?? 75 ?? C6 40 ?? 00 C6 42 ?? 01 48 83 C0 ??", 3, U8, 1, NONE, 0)
DD_EVIDENCE(ListenerEntryListener, ListenerAdd, TARGET, 0, FUNC, 0, "48 89 7A ?? C6 42 ?? 01 48 89 32", 3, U8, 1, NONE, 0)
DD_EVIDENCE(ListenerEntryListener, QueryListenerAdd, TARGET, 0, FUNC, 0, "48 89 42 ?? C6 42 ?? 01 48 83 43 08", 3, U8, 1, NONE, 0)
DD_LAYOUT(ListenerEntryActive, OFFSET, 0x10, REQUIRED, RUNTIME, "core events.native events.query")
DD_EVIDENCE(ListenerEntryActive, EventTypeCount, HIT, 0, WINDOW, 128, "48 39 58 ?? 75 ?? C6 40 ?? 00 C6 42 ?? 01 48 83 C0 ??", 8, U8, 1, NONE, 0)
DD_EVIDENCE(ListenerEntryActive, ListenerAdd, TARGET, 0, FUNC, 0, "48 89 7A ?? C6 42 ?? 01 48 89 32", 6, U8, 1, NONE, 0)
DD_EVIDENCE(ListenerEntryActive, QueryListenerAdd, TARGET, 0, FUNC, 0, "48 89 42 ?? C6 42 ?? 01 48 83 43 08", 6, U8, 1, NONE, 0)
DD_LAYOUT(QueryListenerEntrySize, SIZE, 0x18, OPT, RUNTIME, "events.query")
DD_EVIDENCE(QueryListenerEntrySize, QueryListenerAdd, TARGET, 0, FUNC, 0, "48 83 43 08 ?? C6 43", 4, U8, 1, NONE, 0)
// Listener<T const> / Listener<T>：+0 虚表、+8 priority（int）、+0x10 std::function（0x40 字节，实现指针在其中 +0x38）
// priority 的依据（U1b）：Event 列表排序的插入排序（3CCF00，std::sort 展开）按 *(int*)(listener+8) 比较：
//   movzx ebp,[rdi+0x10]; mov r14,[rdi+8]; mov r12,[rdi+0x18]; movzx r13d,[rdi+0x20]（取表项的 active、listener、注册时刻、标志）……
//   mov rax,[rsi+8]; mov ecx,[rax+8]; cmp [r14+8],ecx; jl
// 带 EventListenerDataContainer 的四个字段，整个 .text 只有这一处（两个版本都是）
DD_LAYOUT(ListenerPriority, OFFSET, 0x08, REQUIRED, RUNTIME, "core events.native events.query")
DD_EVIDENCE(ListenerPriority, NONE, NOSIG, 0, TEXT, 0, "0F B6 6F 10 4C 8B 77 08 4C 8B 67 18 44 0F B6 6F 20 40 84 ED 0F 84 ?? ?? ?? ?? 80 7E 10 00 74 ?? 48 8B 46 08 8B 48 ?? 41 39 4E ?? 0F 8C", 38, U8, 1, NONE, 0)
DD_EVIDENCE(ListenerPriority, NONE, NOSIG, 0, TEXT, 0, "0F B6 6F 10 4C 8B 77 08 4C 8B 67 18 44 0F B6 6F 20 40 84 ED 0F 84 ?? ?? ?? ?? 80 7E 10 00 74 ?? 48 8B 46 08 8B 48 ?? 41 39 4E ?? 0F 8C", 42, U8, 1, NONE, 0)
// std::function 的位置（U1b）：派发经 __RTDynamicCast 转成 Listener<T const> 之后调用其中的 std::function：
//   mov rcx,[rdi+0x48]; test rcx,rcx; je; lea rax,[rbp+..]; mov [rbp+..],rax; mov rax,[rcx]; lea rdx,[rbp+..]; call [rax+0x10]; cmp eax,1
// 0x48 = ListenerFunction + StdFunctionImpl（StdFunctionImpl 另有依据，所以这里核对的是 +0x10）。怪物受伤（5FC3E0，MonsterBlockReturn
// 所在的函数）里只有一处这样的派发（两个版本都是）；listener 析构 361730 里的 lea rdi,[rbx+0x10]、[rdi+0x38] 在离线版里没有唯一的锚点
DD_LAYOUT(ListenerFunction, OFFSET, 0x10, REQUIRED, RUNTIME, "core events.native events.query")
DD_EVIDENCE(ListenerFunction, MonsterBlockReturn, TARGET, 0, FUNC, 0, "48 8B ?? ?? 48 85 C9 74 ?? 48 8D 85 ?? ?? ?? ?? 48 89 85 ?? ?? ?? ?? 48 8B 01 48 8D 95 ?? ?? ?? ?? FF 50 ?? 83 F8 01", 3, U8, 1, StdFunctionImpl, 0)
// Event 基类的时间戳（int64，+8），native.* 的 time 字段。依据（U1b）是 EventListenerDataContainer 虚表槽 1 的过滤函数（361610）：
//   cmp byte [rcx+0x20],0; je; mov rax,[rdx+8]; cmp [rcx+0x18],rax（rdx 是事件）
DD_LAYOUT(EventTime, OFFSET, 0x08, OPT, RUNTIME, "events.native")
DD_EVIDENCE(EventTime, ListenerEntryVtable, VSLOT, 1, ENTRY, 0, "80 79 ?? 00 74 ?? 48 8B 42 ?? 48 39 41 ??", 9, U8, 1, NONE, 0)

// 注册 listener 调用的游戏函数、游戏回调我们的函数（U1 修复 FA，审核 A 的 S1、S3；约定见文件开头）
DD_LAYOUT(ProtoListenerAdd, COUNT, 2, OPT, NONE, "events.native")
DD_UNVERIFIED(ProtoListenerAdd, "void* ListenerListData::Add(ListData* rcx, BaseListener* rdx)（3619D0）；ListenerEntry*、ListDataDirty 的依据读的是它的函数体，看得到 rcx、rdx 的用法，看不出有没有多的参数")
DD_LAYOUT(ProtoQueryListenerAdd, COUNT, 2, OPT, NONE, "events.query")
DD_UNVERIFIED(ProtoQueryListenerAdd, "void* ListenerListData<Query>::Add(ListData* rcx, BaseListener* rdx)（43EE90），和 ListenerAdd 同形")
DD_LAYOUT(ProtoStdFuncDoCall, COUNT, 2, OPT, NONE, "events.native events.query combat.buff_remove")
DD_UNVERIFIED(ProtoStdFuncDoCall, "游戏经 std::function 实现的虚表调用我们伪造的实现（system.cpp 的 kImplVtableStorage、combat.cpp 的 kPredVtable）：_Copy(this, void*)、_Move(this, void*)、_Do_call(this, 参数)、_Target_type(this)、_Delete_this(this, bool)、_Get(this)，MSVC 的声明；值是 _Do_call 的参数个数。Event/Query 的派发调用 _Do_call 后 cmp eax,1（StdFuncImplVtableSlots 的依据），谓词的 _Do_call 返回 bool")
// 伪造 Listener 虚表（system.cpp 的 ForgeListenerVtable，游戏没有现成 Listener<T> 虚表的类型用）：按 MSVC x64 RTTI 的结构写
// COL、CHD、两个 BCD（字段都是相对映像基址的 int32），虚表只填槽 0
DD_LAYOUT(RttiTypeDescriptorName, OFFSET, 0x10, OPT, NONE, "events.native events.query")
DD_UNVERIFIED(RttiTypeDescriptorName, "TypeDescriptor 的修饰名（.?AV…）在 +0x10（虚表指针、spare 之后）；按名字找 Listener 的类型描述符时用。MSVC x64 ABI，工具链换了才会变；test_events 的伪造虚表主题运行核对")
DD_LAYOUT(RttiBaseClassDescriptorSize, SIZE, 0x1C, OPT, NONE, "events.native events.query")
DD_UNVERIFIED(RttiBaseClassDescriptorSize, "_RTTIBaseClassDescriptor：pTypeDescriptor、numContainedBases、PMD（mdisp、pdisp、vdisp）、attributes、pClassDescriptor，7 个 int32；MSVC x64 ABI")
DD_LAYOUT(RttiClassHierarchySize, SIZE, 0x10, OPT, NONE, "events.native events.query")
DD_UNVERIFIED(RttiClassHierarchySize, "_RTTIClassHierarchyDescriptor：signature、attributes、numBaseClasses、pBaseClassArray，4 个 int32；MSVC x64 ABI")
DD_LAYOUT(RttiObjectLocatorSize, SIZE, 0x18, OPT, NONE, "events.native events.query")
DD_UNVERIFIED(RttiObjectLocatorSize, "_RTTICompleteObjectLocator（x64，signature 1）：signature、offset、cdOffset、pTypeDescriptor、pClassDescriptor、pSelf，6 个 int32；__RTDynamicCast 用 COL - pSelf 算映像基址")
DD_LAYOUT(ForgedListenerVtableSlots, COUNT, 1, OPT, NONE, "events.native events.query")
DD_UNVERIFIED(ForgedListenerVtableSlots, "伪造的虚表只填槽 0（借一个现成 Listener 的析构）：假设游戏对我们的 listener 只经 vtable[-1] 的 COL 做 __RTDynamicCast、不调用别的虚函数（游戏不销毁我们的 listener，knowledge/dd1/events.md）；test_events 的伪造虚表主题运行核对")

// 原生事件的负载（字段偏移相对事件对象起点，依据见 knowledge/dd1/events.md；只有 P2 的 test_events 运行证据）。
// id 是 Ev<类名去掉 Event>_<字段>，_cap 是字符串字段的容量
DD_LAYOUT(EvHeroReachedDeathsDoor_hero, OFFSET, 0x10, OPT, NONE, "topic:actor.deathsdoor topic:native.HeroReachedDeathsDoorEvent")
DD_UNVERIFIED(EvHeroReachedDeathsDoor_hero, "test_events")
DD_LAYOUT(EvHeroReachedDeathsDoor_damage_type, OFFSET, 0x18, OPT, NONE, "topic:actor.deathsdoor topic:native.HeroReachedDeathsDoorEvent")
DD_UNVERIFIED(EvHeroReachedDeathsDoor_damage_type, "test_events")
DD_LAYOUT(EvHeroReachedDeathsDoor_source_id, OFFSET, 0x1C, OPT, NONE, "topic:actor.deathsdoor topic:native.HeroReachedDeathsDoorEvent")
DD_UNVERIFIED(EvHeroReachedDeathsDoor_source_id, "test_events")
DD_LAYOUT(EvHeroReachedDeathsDoor_source_type, OFFSET, 0x20, OPT, NONE, "topic:actor.deathsdoor topic:native.HeroReachedDeathsDoorEvent")
DD_UNVERIFIED(EvHeroReachedDeathsDoor_source_type, "test_events")
DD_LAYOUT(EvHeartAttack_heart_attacks, OFFSET, 0x14, OPT, NONE, "topic:actor.heartattack topic:native.HeartAttackEvent")
DD_UNVERIFIED(EvHeartAttack_heart_attacks, "test_events")
DD_LAYOUT(EvHeartAttack_flag0, OFFSET, 0x18, OPT, NONE, "topic:actor.heartattack topic:native.HeartAttackEvent")
DD_UNVERIFIED(EvHeartAttack_flag0, "test_events")
DD_LAYOUT(EvHeartAttack_flag1, OFFSET, 0x19, OPT, NONE, "topic:actor.heartattack topic:native.HeartAttackEvent")
DD_UNVERIFIED(EvHeartAttack_flag1, "test_events")
DD_LAYOUT(EvHeartAttack_flag2, OFFSET, 0x1A, OPT, NONE, "topic:actor.heartattack topic:native.HeartAttackEvent")
DD_UNVERIFIED(EvHeartAttack_flag2, "test_events")
DD_LAYOUT(EvHeroDeath_hero_id, OFFSET, 0x10, OPT, NONE, "topic:actor.death topic:native.HeroDeathEvent")
DD_UNVERIFIED(EvHeroDeath_hero_id, "test_events")
DD_LAYOUT(EvMonsterDeath_monster_guid, OFFSET, 0x18, OPT, NONE, "topic:actor.death topic:native.MonsterDeathEvent")
DD_UNVERIFIED(EvMonsterDeath_monster_guid, "test_events")
DD_LAYOUT(EvMonsterDeath_attacker_id, OFFSET, 0x1C, OPT, NONE, "topic:actor.death topic:native.MonsterDeathEvent")
DD_UNVERIFIED(EvMonsterDeath_attacker_id, "test_events")
DD_LAYOUT(EvMonsterDeath_damage_type, OFFSET, 0x20, OPT, NONE, "topic:actor.death topic:native.MonsterDeathEvent")
DD_UNVERIFIED(EvMonsterDeath_damage_type, "test_events")
DD_LAYOUT(EvMonsterDeath_class_field_d0, OFFSET, 0x10, OPT, NONE, "topic:actor.death topic:native.MonsterDeathEvent")
DD_UNVERIFIED(EvMonsterDeath_class_field_d0, "test_events")
DD_LAYOUT(EvMonsterDeath_class_field_cc, OFFSET, 0x14, OPT, NONE, "topic:actor.death topic:native.MonsterDeathEvent")
DD_UNVERIFIED(EvMonsterDeath_class_field_cc, "test_events")
DD_LAYOUT(EvMonsterDeath_flag, OFFSET, 0x24, OPT, NONE, "topic:actor.death topic:native.MonsterDeathEvent")
DD_UNVERIFIED(EvMonsterDeath_flag, "test_events")
DD_LAYOUT(EvHeroAfflictionVirtue_hero, OFFSET, 0x10, OPT, NONE, "topic:actor.affliction topic:native.HeroAfflictionVirtueEvent")
DD_UNVERIFIED(EvHeroAfflictionVirtue_hero, "test_events")
DD_LAYOUT(EvRaidStart_raid, OFFSET, 0x10, OPT, NONE, "topic:raid.start topic:native.RaidStartEvent")
DD_UNVERIFIED(EvRaidStart_raid, "test_events")
DD_LAYOUT(EvRaidFinishResults_results, OFFSET, 0x10, OPT, NONE, "topic:raid.finish topic:native.RaidFinishResultsEvent")
DD_UNVERIFIED(EvRaidFinishResults_results, "test_events")
DD_LAYOUT(EvTorchModified_torch_before, OFFSET, 0x10, OPT, NONE, "topic:raid.torch topic:native.TorchModifiedEvent")
DD_UNVERIFIED(EvTorchModified_torch_before, "test_events")
DD_LAYOUT(EvTorchModified_torch_after, OFFSET, 0x14, OPT, NONE, "topic:raid.torch topic:native.TorchModifiedEvent")
DD_UNVERIFIED(EvTorchModified_torch_after, "test_events")
DD_LAYOUT(EvTorchModified_delta, OFFSET, 0x18, OPT, NONE, "topic:raid.torch topic:native.TorchModifiedEvent")
DD_UNVERIFIED(EvTorchModified_delta, "test_events")
DD_LAYOUT(EvTorchModified_flag, OFFSET, 0x1C, OPT, NONE, "topic:raid.torch topic:native.TorchModifiedEvent")
DD_UNVERIFIED(EvTorchModified_flag, "test_events")
DD_LAYOUT(EvPreOnStartTownVisit_flag, OFFSET, 0x10, OPT, NONE, "topic:town.enter topic:native.PreOnStartTownVisitEvent")
DD_UNVERIFIED(EvPreOnStartTownVisit_flag, "test_events")
DD_LAYOUT(EvTownEventOnStartTownVisit_event, OFFSET, 0x10, OPT, NONE, "topic:town.event topic:native.TownEventOnStartTownVisitEvent")
DD_UNVERIFIED(EvTownEventOnStartTownVisit_event, "test_events")
DD_LAYOUT(EvBuildingStateChanged_name, OFFSET, 0x10, OPT, NONE, "topic:town.building topic:native.BuildingStateChangedEvent")
DD_UNVERIFIED(EvBuildingStateChanged_name, "test_events")
DD_LAYOUT(EvBuildingStateChanged_name_cap, SIZE, 0x50, OPT, NONE, "topic:town.building topic:native.BuildingStateChangedEvent")
DD_UNVERIFIED(EvBuildingStateChanged_name_cap, "test_events")
DD_LAYOUT(EvBuildingStateChanged_value, OFFSET, 0x60, OPT, NONE, "topic:town.building topic:native.BuildingStateChangedEvent")
DD_UNVERIFIED(EvBuildingStateChanged_value, "test_events")
DD_LAYOUT(EvBuildingStateChanged_flag, OFFSET, 0x64, OPT, NONE, "topic:town.building topic:native.BuildingStateChangedEvent")
DD_UNVERIFIED(EvBuildingStateChanged_flag, "test_events")
DD_LAYOUT(EvActivityOpened_activity, OFFSET, 0x10, OPT, NONE, "topic:town.activity topic:native.ActivityOpenedEvent")
DD_UNVERIFIED(EvActivityOpened_activity, "test_events")
DD_LAYOUT(EvActivityOpened_flag, OFFSET, 0x18, OPT, NONE, "topic:town.activity topic:native.ActivityOpenedEvent")
DD_UNVERIFIED(EvActivityOpened_flag, "test_events")
DD_LAYOUT(EvActivityFinished_activity_id, OFFSET, 0x10, OPT, NONE, "topic:town.activity.finish topic:native.ActivityFinishedEvent")
DD_UNVERIFIED(EvActivityFinished_activity_id, "test_events")
DD_LAYOUT(EvHeroRecruited_hero_id, OFFSET, 0x10, OPT, NONE, "topic:hero.recruit topic:native.HeroRecruitedEvent")
DD_UNVERIFIED(EvHeroRecruited_hero_id, "test_events")
DD_LAYOUT(EvHeroDismissed_hero_id, OFFSET, 0x10, OPT, NONE, "topic:hero.dismiss topic:native.HeroDismissedEvent")
DD_UNVERIFIED(EvHeroDismissed_hero_id, "test_events")
DD_LAYOUT(EvQuirkApplied_hero_id, OFFSET, 0x10, OPT, NONE, "topic:hero.quirk topic:native.QuirkAppliedEvent")
DD_UNVERIFIED(EvQuirkApplied_hero_id, "test_events")
DD_LAYOUT(EvQuirkApplied_kind, OFFSET, 0x14, OPT, NONE, "topic:hero.quirk topic:native.QuirkAppliedEvent")
DD_UNVERIFIED(EvQuirkApplied_kind, "test_events")
DD_LAYOUT(EvQuirkApplied_quirk, OFFSET, 0x18, OPT, NONE, "topic:hero.quirk topic:native.QuirkAppliedEvent")
DD_UNVERIFIED(EvQuirkApplied_quirk, "test_events")
DD_LAYOUT(EvQuirkApplied_quirk_cap, SIZE, 0x100, OPT, NONE, "topic:hero.quirk topic:native.QuirkAppliedEvent")
DD_UNVERIFIED(EvQuirkApplied_quirk_cap, "test_events")
DD_LAYOUT(EvHeroSkillUpgraded_hero_id, OFFSET, 0x10, OPT, NONE, "topic:hero.skill topic:native.HeroSkillUpgradedEvent")
DD_UNVERIFIED(EvHeroSkillUpgraded_hero_id, "test_events")
DD_LAYOUT(EvHeroSkillUpgraded_skill_index, OFFSET, 0x14, OPT, NONE, "topic:hero.skill topic:native.HeroSkillUpgradedEvent")
DD_UNVERIFIED(EvHeroSkillUpgraded_skill_index, "test_events")
DD_LAYOUT(EvInventoryAddItem_inventory, OFFSET, 0x10, OPT, NONE, "topic:inventory.add topic:native.InventoryAddItemEvent")
DD_UNVERIFIED(EvInventoryAddItem_inventory, "test_events")
DD_LAYOUT(EvInventoryAddItem_item, OFFSET, 0x18, OPT, NONE, "topic:inventory.add topic:native.InventoryAddItemEvent")
DD_UNVERIFIED(EvInventoryAddItem_item, "test_events")
DD_LAYOUT(EvInventoryAddItem_value, OFFSET, 0x20, OPT, NONE, "topic:inventory.add topic:native.InventoryAddItemEvent")
DD_UNVERIFIED(EvInventoryAddItem_value, "test_events")
DD_LAYOUT(EvInventoryPreMerge_inventory, OFFSET, 0x10, OPT, NONE, "topic:inventory.premerge topic:native.InventoryPreMergeEvent")
DD_UNVERIFIED(EvInventoryPreMerge_inventory, "test_events")
DD_LAYOUT(EvInventoryPreMerge_other, OFFSET, 0x18, OPT, NONE, "topic:inventory.premerge topic:native.InventoryPreMergeEvent")
DD_UNVERIFIED(EvInventoryPreMerge_other, "test_events")
DD_LAYOUT(EvInventoryPostMerge_inventory, OFFSET, 0x10, OPT, NONE, "topic:inventory.postmerge topic:native.InventoryPostMergeEvent")
DD_UNVERIFIED(EvInventoryPostMerge_inventory, "test_events")
DD_LAYOUT(EvInventoryPostMerge_other, OFFSET, 0x18, OPT, NONE, "topic:inventory.postmerge topic:native.InventoryPostMergeEvent")
DD_UNVERIFIED(EvInventoryPostMerge_other, "test_events")
DD_LAYOUT(EvEstateCurrencyChange_currency, OFFSET, 0x10, OPT, NONE, "topic:estate.currency topic:native.EstateCurrencyChangeEvent")
DD_UNVERIFIED(EvEstateCurrencyChange_currency, "test_events")
DD_LAYOUT(EvEstateCurrencyChange_delta, OFFSET, 0x14, OPT, NONE, "topic:estate.currency topic:native.EstateCurrencyChangeEvent")
DD_UNVERIFIED(EvEstateCurrencyChange_delta, "test_events")

// 原生事件负载的读取类型（U1 修复 FA，审核 A 的 S3）
DD_LAYOUT(NativePayloadFields, COUNT, 47, OPT, NONE, "topic:native.* topic:actor.* topic:raid.* topic:town.* topic:hero.* topic:inventory.* topic:estate.*")
DD_UNVERIFIED(NativePayloadFields, "上面 Ev* 的 47 个字段偏移（另有 2 个 _cap）按 events/system.cpp 的 NativeSources 里写的原始类型读（I32、U32、F32、B8、Ptr、Str 带容量），类型和偏移一样只有 test_events 的运行证据；值是字段个数，增删字段时同步改")

// Query 的负载（相对 Query 对象起点；只有 test_events 运行证据）。id 是 Qy<类名去掉 Query>_<字段>
DD_LAYOUT(QyCampingPointBuff_points, OFFSET, 0x08, OPT, NONE, "topic:query.CampingPointBuffQuery")
DD_UNVERIFIED(QyCampingPointBuff_points, "test_events")
DD_LAYOUT(QyCurioInteractionBuff_hero, OFFSET, 0x08, OPT, NONE, "topic:query.CurioInteractionBuffQuery")
DD_UNVERIFIED(QyCurioInteractionBuff_hero, "test_events")
DD_LAYOUT(QyCurioInteractionBuff_curio, OFFSET, 0x10, OPT, NONE, "topic:query.CurioInteractionBuffQuery")
DD_UNVERIFIED(QyCurioInteractionBuff_curio, "test_events")
DD_LAYOUT(QyCurioInteractionBuff_stress, OFFSET, 0x18, OPT, NONE, "topic:query.CurioInteractionBuffQuery")
DD_UNVERIFIED(QyCurioInteractionBuff_stress, "test_events")
DD_LAYOUT(QyCurioInteractionBuff_heal, OFFSET, 0x1C, OPT, NONE, "topic:query.CurioInteractionBuffQuery")
DD_UNVERIFIED(QyCurioInteractionBuff_heal, "test_events")
DD_LAYOUT(QyFindNonRosterHeroes_container, OFFSET, 0x08, OPT, NONE, "topic:query.FindNonRosterHeroesQuery")
DD_UNVERIFIED(QyFindNonRosterHeroes_container, "test_events")
DD_LAYOUT(QyInventoryExtraStackingLimit_item_id, OFFSET, 0x08, OPT, NONE, "topic:query.InventoryExtraStackingLimitQuery")
DD_UNVERIFIED(QyInventoryExtraStackingLimit_item_id, "test_events")
DD_LAYOUT(QyInventoryExtraStackingLimit_arg, OFFSET, 0x0C, OPT, NONE, "topic:query.InventoryExtraStackingLimitQuery")
DD_UNVERIFIED(QyInventoryExtraStackingLimit_arg, "test_events")
DD_LAYOUT(QyInventoryExtraStackingLimit_limit, OFFSET, 0x10, OPT, NONE, "topic:query.InventoryExtraStackingLimitQuery")
DD_UNVERIFIED(QyInventoryExtraStackingLimit_limit, "test_events")
DD_LAYOUT(QyInventoryReplacementEffectId_item_id, OFFSET, 0x08, OPT, NONE, "topic:query.InventoryReplacementEffectIdQuery")
DD_UNVERIFIED(QyInventoryReplacementEffectId_item_id, "test_events")
DD_LAYOUT(QyInventoryReplacementEffectId_arg, OFFSET, 0x0C, OPT, NONE, "topic:query.InventoryReplacementEffectIdQuery")
DD_UNVERIFIED(QyInventoryReplacementEffectId_arg, "test_events")
DD_LAYOUT(QyInventoryReplacementEffectId_effect_id, OFFSET, 0x10, OPT, NONE, "topic:query.InventoryReplacementEffectIdQuery")
DD_UNVERIFIED(QyInventoryReplacementEffectId_effect_id, "test_events")
DD_LAYOUT(QyIsDisabled_hero_id, OFFSET, 0x08, OPT, NONE, "topic:query.IsDisabledQuery")
DD_UNVERIFIED(QyIsDisabled_hero_id, "test_events")
DD_LAYOUT(QyIsDisabled_disabled, OFFSET, 0x0C, OPT, NONE, "topic:query.IsDisabledQuery")
DD_UNVERIFIED(QyIsDisabled_disabled, "test_events")
DD_LAYOUT(QyLootDarknessBuff_light, OFFSET, 0x08, OPT, NONE, "topic:query.LootDarknessBuffQuery")
DD_UNVERIFIED(QyLootDarknessBuff_light, "test_events")
DD_LAYOUT(QyLootDarknessBuff_result, OFFSET, 0x10, OPT, NONE, "topic:query.LootDarknessBuffQuery")
DD_UNVERIFIED(QyLootDarknessBuff_result, "test_events")
DD_LAYOUT(QyLootDarknessBuff_arg, OFFSET, 0x18, OPT, NONE, "topic:query.LootDarknessBuffQuery")
DD_UNVERIFIED(QyLootDarknessBuff_arg, "test_events")
DD_LAYOUT(QyProvisionIsItemTooltipsEnabled_enabled, OFFSET, 0x08, OPT, NONE, "topic:query.ProvisionIsItemTooltipsEnabledQuery")
DD_UNVERIFIED(QyProvisionIsItemTooltipsEnabled_enabled, "test_events")
DD_LAYOUT(QyProvisionSupplyBuff_items_first, OFFSET, 0x08, OPT, NONE, "topic:query.ProvisionSupplyBuffQuery")
DD_UNVERIFIED(QyProvisionSupplyBuff_items_first, "test_events")
DD_LAYOUT(QyTownGoldInterestBuff_interest, OFFSET, 0x08, OPT, NONE, "topic:query.TownGoldInterestBuffQuery")
DD_UNVERIFIED(QyTownGoldInterestBuff_interest, "test_events")
DD_LAYOUT(QyTownVisitIdleStressBuff_stress_relief, OFFSET, 0x08, OPT, NONE, "topic:query.TownVisitIdleStressBuffQuery")
DD_UNVERIFIED(QyTownVisitIdleStressBuff_stress_relief, "test_events")
DD_LAYOUT(QyTownVisitIdleStressBuff_flag, OFFSET, 0x0C, OPT, NONE, "topic:query.TownVisitIdleStressBuffQuery")
DD_UNVERIFIED(QyTownVisitIdleStressBuff_flag, "test_events")
// Query 负载的读写类型（U1 修复 FA，审核 A 的 S3）
DD_LAYOUT(QueryPayloadFields, COUNT, 22, OPT, NONE, "topic:query.*")
DD_UNVERIFIED(QueryPayloadFields, "上面 Qy* 的 22 个字段按 system.cpp 的 QueryDefs 里写的原始类型读写，可写的字段按这个宽度写回游戏；只有 test_events 的运行证据。值是字段个数，增删字段时同步改")

// ============================================================================
// 持久化（persist/persist.cpp）：当前 profile 的设备名 [[StorageManager]+0x38]+0x20（char[0x80]），取自 2923A0 开头
// ============================================================================
DD_LAYOUT(StorageProfileSlot, OFFSET, 0x38, OPT, RUNTIME, "persist")
DD_EVIDENCE(StorageProfileSlot, StorageManager, HIT, 0, ENTRY, 0, "45 33 ED 4C 89 6C 24 58 48 8B 0D ?? ?? ?? ?? 4C 8B 41 ?? 49 83 C0 ??", 18, U8, 1, NONE, 0)
DD_LAYOUT(StorageDeviceName, OFFSET, 0x20, OPT, RUNTIME, "persist")
DD_EVIDENCE(StorageDeviceName, StorageManager, HIT, 0, ENTRY, 0, "45 33 ED 4C 89 6C 24 58 48 8B 0D ?? ?? ?? ?? 4C 8B 41 ?? 49 83 C0 ??", 22, U8, 1, NONE, 0)
DD_LAYOUT(StorageDeviceNameCap, SIZE, 0x80, OPT, NONE, "persist")
DD_UNVERIFIED(StorageDeviceNameCap, "设备名的字段宽度 char[0x80]（knowledge/dd1/campaign.md 的「存档」）；读的时候在宽度内截断，P4 场景的运行证据")
// persist 的 4 个观察 hook 的原型（U1 修复 FA，审核 A 的 S1：hook 按固定的参数个数转发，persist 归为「写」；约定见文件开头，
// 类型在 persist/persist.cpp 的 QueueWriteFn 等）
DD_LAYOUT(ProtoStorageQueueWrite, COUNT, 9, OPT, NONE, "persist")
DD_UNVERIFIED(ProtoStorageQueueWrite, "StorageManager 排队写文件（2487E0）：(SM*, 句柄*, 设备名, 文件名, 缓冲 qword, 大小 dword, 不用, byte, qword)，参数宽度反汇编核对过（persist.cpp 的说明）；detour 按 9 个转发，多了参数时第 10 个起不转发，直接影响存档写入")
DD_LAYOUT(ProtoStorageReadFile, COUNT, 6, OPT, NONE, "persist")
DD_UNVERIFIED(ProtoStorageReadFile, "StorageManager::ReadFile（248D60）：(SM*, 设备名, 路径, 缓冲, 容量 dword, 解密 byte)，返回 eax（实读字节数）；detour 按 6 个转发")
DD_LAYOUT(ProtoSaveSlotBackup, COUNT, 4, OPT, NONE, "persist")
DD_UNVERIFIED(ProtoSaveSlotBackup, "选存档槽复制 backup/（2923A0）没有参数、没有返回值；detour 原样转发 rcx、rdx、r8、r9 四个整数寄存器（值是这个 4），有栈上参数或浮点参数时不转发")
DD_LAYOUT(ProtoAppShutdown, COUNT, 4, OPT, NONE, "persist")
DD_UNVERIFIED(ProtoAppShutdown, "退到桌面（3D1CC0）入口不读参数（knowledge/dd1/campaign.md 的「存档」）；detour 原样转发四个整数寄存器")

// ============================================================================
// 启用 Mod 列表（game/mods.cpp）：Steam 版和离线版的 UGC 查找函数 38F0B0 / 383F90 一致，依据见 knowledge/dd1/data-and-mods.md
// ============================================================================
// UGCId：name char[0x80] + source char[0x40]，0xC0 字节
DD_LAYOUT(UgcIdSize, SIZE, 0xC0, OPT, NONE, "game.mods")
DD_UNVERIFIED(UgcIdSize, "启用列表的键名自检（applied_ugcs_1_0）和 P3 探针 probe_p3 的运行核对")
DD_LAYOUT(UgcIdNameCap, SIZE, 0x80, OPT, NONE, "game.mods")
DD_UNVERIFIED(UgcIdNameCap, "启用列表的键名自检（applied_ugcs_1_0）和 P3 探针 probe_p3 的运行核对")
DD_LAYOUT(UgcIdSource, OFFSET, 0x80, OPT, NONE, "game.mods")
DD_UNVERIFIED(UgcIdSource, "启用列表的键名自检（applied_ugcs_1_0）和 P3 探针 probe_p3 的运行核对")
DD_LAYOUT(UgcIdSourceCap, SIZE, 0x40, OPT, NONE, "game.mods")
DD_UNVERIFIED(UgcIdSourceCap, "启用列表的键名自检（applied_ugcs_1_0）和 P3 探针 probe_p3 的运行核对")
// UGCIdList 的键名（+0x08）在 vector（+0x208）之前 0x200 字节，键名 char[0x200]
DD_LAYOUT(UgcListKeyOffset, OFFSET, 0x200, OPT, NONE, "game.mods")
DD_UNVERIFIED(UgcListKeyOffset, "读的时候核对键名是 applied_ugcs_1_0，不对就报结构变化、不给列表")
DD_LAYOUT(UgcListKeyCap, SIZE, 0x200, OPT, NONE, "game.mods")
DD_UNVERIFIED(UgcListKeyCap, "读的时候核对键名是 applied_ugcs_1_0，不对就报结构变化、不给列表")
// UserServicesInterface+0x20：vector<UserGeneratedContent*>；UGC 对象 +0x20 是目录 char[0x200]，+0x620 是 UGCId
DD_LAYOUT(UserServicesInstalled, OFFSET, 0x20, OPT, NONE, "game.mods")
DD_UNVERIFIED(UserServicesInstalled, "P3 探针 probe_p3 的运行核对；vector 不合法或某项读不出时报错、不给列表")
DD_LAYOUT(UgcPath, OFFSET, 0x20, OPT, NONE, "game.mods")
DD_UNVERIFIED(UgcPath, "P3 探针 probe_p3 的运行核对；目录读不出时按没找到处理")
DD_LAYOUT(UgcPathCap, SIZE, 0x200, OPT, NONE, "game.mods")
DD_UNVERIFIED(UgcPathCap, "P3 探针 probe_p3 的运行核对；目录读不出时按没找到处理")
DD_LAYOUT(UgcObjectId, OFFSET, 0x620, OPT, NONE, "game.mods")
DD_UNVERIFIED(UgcObjectId, "P3 探针 probe_p3 的运行核对；读出的 name/source 要和启用列表的项相等才算找到")

// ============================================================================
// DDCore 和加载器自己的 hook（U1 修复 FA，审核 A 的 S3）。原型同样不能运行时核对；按 ADR 0036 的「写」停用它们，就等于未知版本
// 上 DDCore 整体停用（方案 C）。所以这两项是例外：core 不因它们停用、只警告（layout_check.cpp 的 BuildGate），加载器不读布局清单。
// ADR 0036 列为局限，由用户决定
// ============================================================================
DD_LAYOUT(ProtoUpdateStateInternal, COUNT, 1, OPT, NONE, "core")
DD_UNVERIFIED(ProtoUpdateStateInternal, "每帧 hook：void Darkest::UpdateStateInternal(App* rcx)（3DCE10），detour 按 1 个参数转发；更新后在 dd_disasm 里看入口和调用点的参数用法")
DD_LAYOUT(ProtoCreateAppSystems, COUNT, 1, OPT, NONE, "loader")
DD_UNVERIFIED(ProtoCreateAppSystems, "早期 hook：void* Darkest::CreateAppSystems(App* rcx)（3E9320），加载器的 detour（loader/early_hook.h）按 1 个参数转发；加载器不读布局清单，更新后人工核实")

// ============================================================================
// 组（DD_GROUP，U1 修复 FB，审核 A 的 S4；格式见文件开头）。C++ 展开成空，tools/ddupdate 的 verify、repair、accept 用。
// 组的划分来自 U1 修复 FA 的盘点（docs/u1/fa-report.md 第 2 节）。没有运行时能核对的成员的结构（技能表、原生事件和 Query 的
// 负载、游戏名字表）不编组：它们本来就全部是 NONE，accept 时和其他 NONE 项一样逐项确认
// ============================================================================
DD_GROUP(ActorVtableSlots, "Hero/Monster 虚表槽位和槽里函数的原型",
         VtSlotStress, VtSlotDamage, VtSlotHeal, VtSlotDeathsDoor, VtSlotDead, SlotAddBuff, SlotRemoveBuffs,
         ProtoActorDamage, ProtoActorHeal, ProtoActorStress, ProtoActorPredicate, ProtoAddBuff, ProtoRemoveBuffs)
DD_GROUP(ActorStruct, "Actor 结构（生命、压力、最大生命、buff 组、标识、职业）和它的字段宽度",
         ActorHp, ActorStress, ActorMaxHpStat, ActorMaxHpMask, ActorBuffGroup, ActorNextGuid, HeroId, MonsterGuid,
         HeroClass, MonsterClass, ActorFloatSize, ActorIdSize)
DD_GROUP(Roster, "Roster::System 和名册项（名册项里内嵌的 Hero）", RosterAlt, RosterHeroes, EntryHero)
DD_GROUP(BuffInstance, "Buff::Instance 的字段、大小和宽度",
         InstSubType, InstSubHash, InstAmount, InstDuration, InstDurationType, InstSource, InstSourceData, InstRule,
         InstFalseRule, InstId, InstIdHash, InstTarget, InstRemoveInactive, InstRemoveBattle, InstRuleFloat, InstRuleString,
         InstRuleHash, InstAdditional, InstAdditionalCount, InstClearOnRule, BuffInstanceSize,
         InstStat, InstScalarSize, InstFlagSize, GameTextCap)
DD_GROUP(BuffGroupEntry, "Buff::Group 的元素（GroupEntry：组 guid + Instance）", BuffEntryInstance, BuffEntrySize, BuffEntryGuid, BuffGuidSize)
DD_GROUP(Effect, "Effect 的字段（4F02D0 解析时写的）、读取前缀和宽度",
         EffectDotBurn, EffectDotStress, EffectOnHit, EffectOnMiss, EffectApplyOnce, EffectQueue, EffectBuffIds, EffectSwap,
         EffectName, EffectNameCap, EffectReadSize, EffectScalarSize, EffectFlagSize,
         EffectTarget, EffectChance, EffectStress, EffectStun, EffectHeal, EffectHealPercent, EffectCombatStatBuff,
         EffectDotPoison, EffectDotBleed, EffectDuration, EffectBuffAmount, EffectBuffDurationType)
DD_GROUP(EffectApplyResult, "ApplyEffect 的结果对象", EffectApplyResultGuids, EffectApplyResultSize)
DD_GROUP(RaidBattle, "Raid 和内嵌的 Battle", RaidBattle, BattleMonsters, BattleGuidCounter, BattleRound, RaidHeroes, RaidPvpName,
         BattleState, BattleStateNone, BattleIntSize)
DD_GROUP(HitData, "命中结算的参数（执行者、技能、目标、伤害、暴击）和两个 hook 的原型",
         AttackPerformer, AttackSkill, TargetActor, TargetDamage, TargetCrit,
         TargetHit, HitDataScalarSize, ProtoApplyHit, ProtoApplyMiss)
DD_GROUP(Listener, "原生事件和 Query 的 listener 列表、伪造的 listener 虚表和 RTTI",
         ListDataSize, ListDataDirty, ListenerEntrySize, ListenerEntryListener, ListenerEntryActive, QueryListenerEntrySize,
         ListenerPriority, ListenerFunction, EventTime,
         ProtoListenerAdd, ProtoQueryListenerAdd, ForgedListenerVtableSlots, RttiTypeDescriptorName, RttiBaseClassDescriptorSize,
         RttiClassHierarchySize, RttiObjectLocatorSize)
DD_GROUP(StdFunction, "MSVC std::function 和实现的虚表", StdFunctionSize, StdFunctionImpl, StdFuncImplVtableSlots, ProtoStdFuncDoCall)
DD_GROUP(StdVector, "MSVC std::vector", StdVectorSize, StdVectorFirst, StdVectorLast)
DD_GROUP(MonsterBlock, "怪物受伤的免疫判断（MonsterBlockReturn 附近）",
         MonsterClassCanBeDamagedDirectly, MonsterBlockImmuneLeaDisp, MonsterBlockImmuneLeaNext, MonsterImmuneTypeCount)
DD_GROUP(Storage, "存档读写（StorageManager 的结构、4 个 hook 的原型）",
         StorageProfileSlot, StorageDeviceName,
         StorageDeviceNameCap, ProtoStorageQueueWrite, ProtoStorageReadFile, ProtoSaveSlotBackup, ProtoAppShutdown)
DD_GROUP(Ugc, "启用 Mod 列表（UserServices、UGC 对象）",
         UserServices, AppliedUgcVector, UgcIdSize, UgcIdNameCap, UgcIdSource, UgcIdSourceCap, UgcListKeyOffset, UgcListKeyCap,
         UserServicesInstalled, UgcPath, UgcPathCap, UgcObjectId)
// 函数和它的原型：特征码被修复工具重新定位、重新生成（或失效）时，原型要人工看一遍。结构的组里只放 RUNTIME 项当触发条件，
// 不放特征码：一个函数挪了，它的原型要看，同一个结构里别的项不受影响（U1 修复 FB 第一轮把特征码放进结构的组，QueryListenerAdd
// 搬迁时整个 listener 组的原型、RTTI 都升级成需要人工，范围太宽）
DD_GROUP(FnActorMaxStat, "ActorMaxStat 的原型", ActorMaxStat, ProtoActorMaxStat)
DD_GROUP(FnApplyHit, "命中结算 hook 的原型", ApplyHit, ProtoApplyHit)
DD_GROUP(FnApplyMiss, "未命中结算 hook 的原型", ApplyMiss, ProtoApplyMiss)
DD_GROUP(FnListenerAdd, "ListenerAdd 的原型", ListenerAdd, ProtoListenerAdd)
DD_GROUP(FnQueryListenerAdd, "QueryListenerAdd 的原型", QueryListenerAdd, ProtoQueryListenerAdd)
DD_GROUP(FnStorageQueueWrite, "存档写入 hook 的原型", StorageQueueWrite, ProtoStorageQueueWrite)
DD_GROUP(FnStorageReadFile, "存档读取 hook 的原型", StorageReadFile, ProtoStorageReadFile)
DD_GROUP(FnSaveSlotBackup, "存档备份 hook 的原型", SaveSlotBackup, ProtoSaveSlotBackup)
DD_GROUP(FnAppShutdown, "退出时 hook 的原型", AppShutdown, ProtoAppShutdown)
DD_GROUP(FnBuffInstanceCopy, "Buff::Instance 复制构造的原型", BuffInstanceCopy, ProtoBuffInstanceCopy)
DD_GROUP(FnVectorDtor64, "vector<char[64]> 释放函数的原型", VectorDtor64, VectorDtor64b, ProtoVectorDtor64)
DD_GROUP(FnEhVectorDtor, "eh vector destructor iterator 的原型", EhVectorDtor, ProtoEhVectorDtor)
DD_GROUP(FnBuffLibraryIndex, "Buff::Library 的 map operator[] 的原型", BuffLibraryIndex, ProtoBuffLibraryIndex)
DD_GROUP(FnBuffGroupAdd, "buff.add 的 hook 原型", BuffGroupAdd, ProtoBuffGroupAdd)
DD_GROUP(FnBuffGroupErase, "buff.remove 的 hook 原型", BuffGroupErase, ProtoBuffGroupErase)
DD_GROUP(FnBuffDurationUpkeep, "buff.tick 的 hook 原型", BuffDurationUpkeep, ProtoBuffDurationUpkeep)
DD_GROUP(FnBuffRuleActive, "规则 hook 的原型", BuffRuleActive, ProtoBuffRuleActive)
DD_GROUP(FnApplyEffect, "ApplyEffect 的原型", ApplyEffect, ProtoApplyEffect)
DD_GROUP(FnPrngFloat, "PrngFloat 的原型", PrngFloat, ProtoPrngFloat)
DD_GROUP(FnFreeU32Vector, "FreeU32Vector 的原型", FreeU32Vector, ProtoFreeU32Vector)
DD_GROUP(FnParseEffectFile, "ParseEffectFile 的原型", ParseEffectFile, ProtoParseEffectFile)
DD_GROUP(FnConsumeHealthDamageBlock, "damage.final 的 hook 原型", ConsumeHealthDamageBlock, ProtoConsumeHealthDamageBlock)
DD_GROUP(FnEffectApplyInternal, "effect.* 的 hook 原型", EffectApplyInternal, ProtoEffectApplyInternal)
DD_GROUP(FnActorDoAttackSkill, "skill.* 的 hook 原型（攻击技能）", ActorDoAttackSkill, ProtoActorDoAttackSkill)
DD_GROUP(FnActorDoSkillFriendly, "skill.* 的 hook 原型（友方技能）", ActorDoSkillFriendly, ProtoActorDoSkillFriendly)
DD_GROUP(FnBattleAdvanceRound, "battle.round.* 的 hook 原型", BattleAdvanceRound, ProtoBattleAdvanceRound)
DD_GROUP(FnCombatStartTurn, "battle.turn.* 的 hook 原型", CombatStartTurn, ProtoCombatStartTurn)
DD_GROUP(FnUpdateStateInternal, "DDCore 每帧 hook 的原型", UpdateStateInternal, ProtoUpdateStateInternal)
DD_GROUP(FnCreateAppSystems, "加载器早期 hook 的原型", CreateAppSystems, ProtoCreateAppSystems)
