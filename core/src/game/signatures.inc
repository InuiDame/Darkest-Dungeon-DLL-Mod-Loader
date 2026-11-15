// DD_SIG(id, kind, required, steal, capture, "pattern")
//
// 全部特征码的唯一来源，加载器、DDCore 和 tools/check_sigs.py 都从这里读取。
//   kind      HOOK        capture 无意义，steal 是函数入口的偷取字节数，由 check_sigs.py 校验指令边界
//             GLOBAL      capture 是 pattern 中 rip 相对位移（disp32）的偏移，解析结果指向全局变量
//             GLOBAL_ANY  和 GLOBAL 一样，但 pattern 允许多处命中，所有命中必须解析到同一地址
//                         （模板实例化产生的多处相同代码，不存在单独唯一的锚点）。
//                         解析出多个候选时，只有 DD_RUNTIME_SELECT 标记的项才允许，运行时按内容选唯一一个
//             CODE        解析结果是 pattern 命中位置本身（函数入口）
//   required  REQUIRED 缺失时 DDCore 整体停用；LOADER 由加载器在 DllMain 里解析，DDCore 不重复扫描
//             （此时入口已被加载器打补丁，特征码匹配不到）；OPT 缺失时只写日志，只影响依赖它的主题
//   steal     HOOK 的偷取字节数，GLOBAL/CODE 填 0
//   capture   GLOBAL/GLOBAL_ANY 用，HOOK/CODE 填 0
//
// HOOK 的 pattern 可以把入口的 steal 字节写成 ??（入口可能已被旧式 DLL 打成 E9/FF25），
// 这时用 DD_ENTRY(id, "...") 给出真实的入口字节，用来比对和判断是否被 hook。
// DD_CHAIN_TARGET(id) 表示这个目标可能被其他插件占用，DDCore 内部串联，check_sigs.py 不算重叠错误。
// DD_RUNTIME_SELECT(id) 标记解析出多个候选、运行时按内容选唯一一个的 GLOBAL_ANY。
//
// 新增或修改后运行 tools/check_sigs.py：要求在 Steam 版和离线版上都唯一命中（GLOBAL_ANY 见上），
// 内联 hook 的偷取范围不能和 knowledge/dd1/hooks.md 的清单重叠（DD_CHAIN_TARGET 除外）。

#ifndef DD_ENTRY
#define DD_ENTRY(id, bytes)
#endif
#ifndef DD_CHAIN_TARGET
#define DD_CHAIN_TARGET(id)
#endif
#ifndef DD_RUNTIME_SELECT
#define DD_RUNTIME_SELECT(id)
#endif

// Darkest::CreateAppSystems，早期 hook，由加载器安装。入口 mov [rsp+8],rcx 共 5 字节
DD_SIG(CreateAppSystems, HOOK, LOADER, 5, 0, "48 89 4C 24 08 53 55 56 57 41 56 48 83 EC 60 4C 8D 0D")

// Darkest::UpdateStateInternal，DDCore 的每帧 hook（有新协议插件时才安装）。入口 5 字节：push rbp/rbx/rsi/rdi
DD_SIG(UpdateStateInternal, HOOK, REQUIRED, 5, 0, "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 18 FD FF FF 48 81 EC E8 03 00 00")

// 状态机变量：last 是最后进入过的状态，next 是待进入的状态
DD_SIG(StateLast, GLOBAL, REQUIRED, 0, 2, "8B 0D ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 3B C8 0F 84 ?? ?? ?? ?? 8B F8 BE 01 00 00 00 83 E9 0D")
DD_SIG(StateNext, GLOBAL, REQUIRED, 0, 8, "8B 0D ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 3B C8 0F 84 ?? ?? ?? ?? 8B F8 BE 01 00 00 00 83 E9 0D")

// 状态名表，as_load_campaign 等字符串指针。布局校验用它确认状态枚举未变，
// 并在运行时按名字找状态序号（不硬编码）
DD_SIG(StateNames, GLOBAL, REQUIRED, 0, 10, "48 63 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 8B 04 C1 48 89 44 24 20")

// Campaign::Current 读取的全局指针 g_pCurrent
DD_SIG(CampaignCurrent, GLOBAL, REQUIRED, 0, 14, "48 83 EC 38 80 3D ?? ?? ?? ?? 00 48 8B 05 ?? ?? ?? ?? 74 ?? 48 85 C0 75")

// Buff::Library 单例指针（CreateCampaignSystems），只用于 CampaignLoad 时打印诊断
DD_SIG(BuffLibrary, GLOBAL, OPT, 0, 6, "48 89 03 48 89 3D ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 4F 1E 00 00")

// ---- 事件总线本体（P2）。缺失时 DDCore 整体停用（和 P1 相同）----

// EventManager 全局指针，CreateAppSystems 中 new 后写入
DD_SIG(EventManager, GLOBAL, REQUIRED, 0, 3, "48 89 05 ?? ?? ?? ?? B9 18 00 00 00 E8 ?? ?? ?? ?? 48 8B F8")
// 已注册的 Event 类型数 118E020。Dispatcher<Event>::GetListenerListData 的模板实例遍布各处
DD_SIG(EventTypeCount, GLOBAL_ANY, REQUIRED, 0, 2, "3B 3D ?? ?? ?? ?? 72 ?? 48 8D 05")
// Event 类名表 2C44360，每项 0x80 字节，Factory<Event,128>::Register 的实例遍布各处
DD_SIG(EventClassNames, GLOBAL_ANY, REQUIRED, 0, 120, "81 38 80 00 00 00 7C ?? 48 8D 05 ?? ?? ?? ?? 41 B9 92 00 00 00 4C 8D 05 ?? ?? ?? ?? 48 89 44 24 20 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 E8 01 74 ?? 83 F8 01 75 ?? CC EB ?? C6 05 ?? ?? ?? ?? 00 8B 3B 48 8D 05 ?? ?? ?? ?? 48 8D 35 ?? ?? ?? ?? 8B CF 48 C1 E1 07 48 8D 1D ?? ?? ?? ?? 4C 8B C3 BA 80 00 00 00 48 89 84 FE ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ??")
// ListenerListData::Add，(ListenerListData*, BaseListener*)，注册通用 listener
DD_SIG(ListenerAdd, CODE, REQUIRED, 0, 0, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 48 8D 35")

// ---- Query 派发器（可选，缺失时 query.* 主题不可用）----
DD_SIG(QueryDispatcher, GLOBAL, OPT, 0, 3, "48 89 3D ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 27 1E 00 00")
// Query 类名表 2C48F10。Factory<X,64> 有 6 个模板族（Achievement、CampaignLogItem、DistrictBuff、MapContent、Query、TileContent），
// 代码完全相同，靠运行时内容选（表项全部以 Query 结尾）
DD_SIG(QueryClassNames, GLOBAL_ANY, OPT, 0, 117, "83 38 40 7C ?? 48 8D 05 ?? ?? ?? ?? 41 B9 92 00 00 00 4C 8D 05 ?? ?? ?? ?? 48 89 44 24 20 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 E8 01 74 ?? 83 F8 01 75 ?? CC EB ?? C6 05 ?? ?? ?? ?? 00 8B 3B 48 8D 05 ?? ?? ?? ?? 48 8D 35 ?? ?? ?? ?? 8B CF 48 C1 E1 07 48 8D 1D ?? ?? ?? ?? 4C 8B C3 BA 80 00 00 00 48 89 84 FE ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ??")
DD_RUNTIME_SELECT(QueryClassNames)
// ListenerListData<Query>::Add，(ListenerListData*, BaseListener*)，注册 Query listener
DD_SIG(QueryListenerAdd, CODE, OPT, 0, 0, "40 53 48 83 EC 40 48 8B D9 48 89 54 24 28")

// ---- 语义 hook（可选，单个缺失时只影响对应主题）----
// Hero / Monster 虚表，ApplyHealthDamageInternal（槽位 50）和治疗、压力的虚函数都在这里
DD_SIG(HeroVtable, GLOBAL, OPT, 0, 3, "48 8D 05 ?? ?? ?? ?? 49 89 06 48 8B 86 20 13 00 00 49 89 86")
DD_SIG(MonsterVtable, GLOBAL, OPT, 0, 3, "48 8D 05 ?? ?? ?? ?? 48 89 06 89 9E 20 13 00 00 48 8D BE 28")
// Raid::Current，PvP 判定（arena_mp）用它
DD_SIG(RaidCurrent, GLOBAL, OPT, 0, 3, "48 8B 05 ?? ?? ?? ?? 48 8B D9 48 8B 88 C0 52 00 00 48 85 C9 74 ?? 48 8D 15 ?? ?? ?? ?? E8")

// 内联语义 hook。入口可能已被 DDShield 等打成 E9/FF25，所以入口字节写成通配
DD_SIG(CombatStartTurn, HOOK, OPT, 5, 0, "?? ?? ?? ?? ?? 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 40 FD FF FF 48 81 EC C0 03 00 00 0F 29")
DD_ENTRY(CombatStartTurn, "48 89 5C 24 08")
DD_SIG(ApplyHit, HOOK, OPT, 7, 0, "?? ?? ?? ?? ?? ?? ?? 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 A0 48 81 EC 60 01 00 00 0F 29 70 B8 0F 29 78 A8 44 0F 29 40 98 44 0F 29 48 88 48")
DD_ENTRY(ApplyHit, "48 8B C4 48 89 58 20")
DD_SIG(ApplyMiss, HOOK, OPT, 5, 0, "?? ?? ?? ?? ?? 55 56 57 41 54 41 56 48 81 EC E0 00")
DD_ENTRY(ApplyMiss, "48 89 5C 24 08")
DD_SIG(ActorDoAttackSkill, HOOK, OPT, 5, 0, "?? ?? ?? ?? ?? 56 57 41 54 41 55 41 56 41 57 48 8D A8 D8 FD FF")
DD_ENTRY(ActorDoAttackSkill, "48 8B C4 55 53")
DD_SIG(ActorDoSkillFriendly, HOOK, OPT, 5, 0, "?? ?? ?? ?? ?? 56 57 41 54 41 55 41 56 41 57 48 8D A8 E8 FE FF FF 48 81 EC D8 01 00 00 0F 29 70 A8 0F 29 78 98 48 8B")
DD_ENTRY(ActorDoSkillFriendly, "48 8B C4 55 53")
DD_SIG(BattleAdvanceRound, HOOK, OPT, 5, 0, "?? ?? ?? ?? ?? 48 89 6C 24 18 56 57 41 56 48 83 EC 40 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 38 80 B9")
DD_ENTRY(BattleAdvanceRound, "48 89 5C 24 10")
DD_SIG(EffectApplyInternal, HOOK, OPT, 5, 0, "?? ?? ?? ?? ?? 41 54 41 55 41 56 41 57 48 8D AC 24 C8 DC")
DD_ENTRY(EffectApplyInternal, "40 55 53 56 57")
DD_CHAIN_TARGET(EffectApplyInternal)

// ---- 启用 Mod 列表（P3，可选，缺失时 game 服务的 Mod 列表不可用）----
// 同一段代码（3A1648，遍历当前存档启用列表上报分析数据）里的两个全局：
//   UserServices      UserServicesInterface*（118DFE8），+0x20 是已安装的 vector<UGC*>
//   AppliedUgcVector  当前存档 UGCIdList（App+0x748）的 vector<UGCId>（2A6D8C0），元素 0xC0 字节
DD_SIG(UserServices, GLOBAL, OPT, 0, 3, "4C 8B 35 ?? ?? ?? ?? 33 F6 48 8B 05 ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? 48 2B C2 48 C1 F8 06 49 0F AF C4 85 C0 0F 84")
DD_SIG(AppliedUgcVector, GLOBAL, OPT, 0, 19, "4C 8B 35 ?? ?? ?? ?? 33 F6 48 8B 05 ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? 48 2B C2 48 C1 F8 06 49 0F AF C4 85 C0 0F 84")

// ---- actor 句柄（P3c，可选，缺失时 DDActorInfo.has_max_hp 为 false）----
// 属性值的合计函数（4C3F20，Steam 版；离线版 4B2090）：float (const Stat*, uint32_t mask, bool clamp)。
// 治疗（483C70）和伤害（4836A0）用 (Actor+0x768, [Actor+0x790], true) 调用它取最大生命。只读，叶函数
DD_SIG(ActorMaxStat, CODE, OPT, 0, 0, "48 89 5C 24 08 48 89 7C 24 10 4C 8B 59 30 44 8B D2 48 8B 51 38 45 33 C9")

// ---- combat 服务和 buff.*、damage.final 主题（P4a，可选，缺失时只有对应能力不可用）----
// 依据见 knowledge/dd1/combat.md 的「Buff」「Effect」「技能」各节和函数表。RVA 是 Steam 版的。
// 内联 hook 的目标（按需安装）：
// Buff::Group::Add（5C4F50）：buff.add。入口 push rbx; sub rsp,0x440 共 9 字节
DD_SIG(BuffGroupAdd, HOOK, OPT, 9, 0, "?? ?? ?? ?? ?? ?? ?? ?? ?? 48 8B D9 89 54 24 20 49 8B D0 48 8D 4C 24 28 E8 ?? ?? ?? ??")
DD_ENTRY(BuffGroupAdd, "40 53 48 81 EC 40 04 00 00")
// Buff::Group::erase（5C4FF0）：buff.remove，全部删除路径的汇合点
DD_SIG(BuffGroupErase, HOOK, OPT, 5, 0, "?? ?? ?? ?? ?? 48 89 6C 24 20 56 57 41 54 41 56 41 57 48 83 EC 20 49 8B D8")
DD_ENTRY(BuffGroupErase, "48 89 5C 24 18")
// Actor::BuffDurationUpkeep（47AAD0）：buff.tick，也给 buff.remove 区分「到期」
DD_SIG(BuffDurationUpkeep, HOOK, OPT, 5, 0, "?? ?? ?? ?? ?? 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D0 48 81 EC 30 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 20 45 8B F0")
DD_ENTRY(BuffDurationUpkeep, "48 89 5C 24 18")
// IsRuleActive（479CD0）：脚本规则，有规则注册时才装。入口 push rbp/rbx/rsi/rdi 共 5 字节；DDShield 的 cave 在函数中间的 479F41，不重叠
DD_SIG(BuffRuleActive, HOOK, OPT, 5, 0, "?? ?? ?? ?? ?? 41 54 41 55 41 56 41 57 48 8D 6C 24 E9 48 81 EC E8 00 00 00 0F 29 B4 24 D0 00 00 00")
DD_ENTRY(BuffRuleActive, "40 55 53 56 57")
// Actor::ConsumeHealthDamageBlock（480400）：damage.final。DDShield 也 hook 这里，串联
DD_SIG(ConsumeHealthDamageBlock, HOOK, OPT, 5, 0, "?? ?? ?? ?? ?? 48 89 74 24 18 57 48 83 EC 20 40 32 F6")
DD_ENTRY(ConsumeHealthDamageBlock, "48 89 5C 24 10")
DD_CHAIN_TARGET(ConsumeHealthDamageBlock)
// 怪物受伤（5FC3E0）里调用 480400 之后的返回点（5FC926，test al,al），后面紧跟免疫判断：
// [[Monster+0x1468]+0xF97]（MonsterClass 的 .can_be_damaged_directly）为 0 且伤害类型在表 DEB7D0 里时不扣血。
// damage.final 的部分吸收按这里判断怪物会不会被扣血（P4 审核 A-S2）；表的地址从命中位置 +52 的 lea 位移解出（+56 是下一条指令）
DD_SIG(MonsterBlockReturn, CODE, OPT, 0, 0, "84 C0 74 ?? 45 33 C9 41 B8 31 00 00 00 48 8B D7 49 8B CD E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? 48 8B 87 68 14 00 00 80 B8 97 0F 00 00 00 75 ?? 49 63 04 24 48 8D 0D ?? ?? ?? ?? 80 3C 08 00")

// combat 服务调用的游戏函数（CODE：命中位置就是函数入口）：
// Buff::Instance 复制构造（482480）：函数体和别的复制构造相同，没有唯一的入口特征码，借 5C4F50 里调用它的 call
// （GLOBAL 的解析方式对 call rel32 一样成立：capture 指向 rel32，结果是调用目标）
DD_SIG(BuffInstanceCopy, GLOBAL, OPT, 0, 1, "E8 ?? ?? ?? ?? 90 48 8B 53 08 48 3B 53 10")
// Instance 的析构（5C4FF0 尾部、482360 尾部的写法）：+0x3B0 调 VectorDtor64，+0x158 的 25 个 vector 经
// EhVectorDtor(.., 0x18, 0x19, VectorDtor64b) 析构（P4-0C 发现：元素析构是 2342E0 而不是 2345D0）
DD_SIG(VectorDtor64, CODE, OPT, 0, 0, "40 53 48 83 EC 20 48 8B 01 48 8B D9 48 85 C0 74 47 48 8B 51 10 48 2B D0 48 83 E2 C0")
DD_SIG(VectorDtor64b, CODE, OPT, 0, 0, "E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC CC CC CC 33 C0 48 89 01")
DD_SIG(EhVectorDtor, CODE, OPT, 0, 0, "4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 53 56 57 41 56")
// Buff 库 map 的 operator[]（4ACFA0）：没有就插入默认构造的 Instance，返回引用
DD_SIG(BuffLibraryIndex, CODE, OPT, 0, 0, "40 53 56 57 41 54 41 56 41 57 48 83 EC 58 48 8B DA")
// EffectLookup（4F3E50）：Effect* (void* unused, const char* name)，名字在 rdx；找不到时写游戏日志。
// combat 查 effect 直接读 EffectMap，不调用它；留作备用，tools/check_actor_layout.py 用它找技能的加载函数
DD_SIG(EffectLookup, CODE, OPT, 0, 0, "48 83 EC 28 33 C9 4C 8B C2 38 0A 74 16")
// ApplyEffect（4CB4D0）：EffectApplyResult* (out, prng&, performer, target, effect, roll, crit, scale, source, sourcedata, bool, bool)
DD_SIG(ApplyEffect, CODE, OPT, 0, 0, "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 48 F9 FF FF")
// prng 取随机浮点（28FCD0），ApplyEffect 的 roll 用它取
DD_SIG(PrngFloat, CODE, OPT, 0, 0, "48 8B C4 48 89 58 10 48 89 70 18 57 48 81 EC A0 00 00 00")
// vector<uint32> 的释放（243C40），EffectApplyResult+8 用完调它
DD_SIG(FreeU32Vector, CODE, OPT, 0, 0, "40 53 48 83 EC 20 48 8B 11 48 8B D9 48 85 D2 74 51 48 8B 41 10 48 2B C2 48 C1 F8 02")
// 解析一个 .effects.darkest 文件（4F02D0）：(const char* path, bool network)
DD_SIG(ParseEffectFile, CODE, OPT, 0, 0, "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 48 EB FF FF")
// 全局：游戏的 prng_minstd（2ADD250）、普通 effect map（2ADD290，std::map 的 head 指针和 size），都在静态初始化 4EDE40 里；
// 备用名册 / 联机库的开关 App+0x54B（2A6D4BB），为 1 时游戏查 Buff 库的 +0x10（联机 map）
DD_SIG(GamePrng, GLOBAL, OPT, 0, 3, "48 8D 3D ?? ?? ?? ?? 48 89 BC 24 88 00 00 00")
DD_SIG(EffectMap, GLOBAL, OPT, 0, 3, "48 8D 05 ?? ?? ?? ?? 48 89 84 24 90 00 00 00 33 DB")
DD_SIG(AltRosterFlag, GLOBAL, OPT, 0, 3, "0F B6 05 ?? ?? ?? ?? 84 C0 48 0F 45 0D ?? ?? ?? ?? 4C 8B 05 ?? ?? ?? ??")

// buff.remove 的原因按调用栈判断，下面这些只用来比对函数起始地址，不 hook、不调用：
// UpdateBuffsInternal（480540）、Roster::System::OnStartTownVisit（587F90）、灼烧衰减（5C4C10）、按 guid 删除（5C4660）、
// ResetStats（4849F0，DDShield hook 它的入口，所以入口写成通配）、inventory.cpp 里删除 buff 的 429750（用途没查，推测和物品有关）
DD_SIG(UpdateBuffsInternal, CODE, OPT, 0, 0, "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC D0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 17 4D 8B E9")
DD_SIG(OnStartTownVisit, CODE, OPT, 0, 0, "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 C8 F5 FF FF")
DD_SIG(BuffBurnDecay, CODE, OPT, 0, 0, "48 83 EC 38 4C 8B 09 48 B8 2B 4E 48 C9 B3 8C E8 03")
DD_SIG(BuffRemoveByGuid, CODE, OPT, 0, 0, "40 53 48 83 EC 30 4C 8B 11 48 B8 2B 4E 48 C9 B3 8C E8 03")
DD_SIG(ActorResetStats, CODE, OPT, 0, 0, "?? ?? ?? ?? ?? 48 8D 6C 24 F8 48 81 EC 08 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 F0 8B FA")
DD_SIG(InventoryBuffRemove, CODE, OPT, 0, 0, "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 48 F1 FF FF")
// 技能的即时 buff（47A7B0，5B9B50 调用；生成表把它误标成 RemoveBuffsOfSource）、Actor::UpdateTrinketBuffs（481A40），P4-0D 建议
DD_SIG(SkillInstantBuffs, CODE, OPT, 0, 0, "4C 8B DC 53 55 56 57 41 54 41 56 41 57")
DD_SIG(UpdateTrinketBuffs, CODE, OPT, 0, 0, "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 68 FF FF FF")

// ---- persist 服务的观察 hook（P4c1，可选，缺失时持久化不可用）----
// 有插件声明使用持久化时在 AppReady 安装，之后不拆。只读内存、算哈希、记录，不改游戏行为。依据见 knowledge/dd1/campaign.md 的「存档」，
// 探针 probe_p4 在用户场景里跑通过的做法（P4-0C、P4-0D）。
// StorageManager 排队写文件（2487E0）：(SM, &句柄, 设备名, 文件名, 缓冲, 大小, …)，所有二进制存档写入都经过这里（238290 调用），
// 主线程和加载线程上都会调用。入口 push rbx/rbp/rsi/rdi 共 5 字节
DD_SIG(StorageQueueWrite, HOOK, OPT, 5, 0, "?? ?? ?? ?? ?? 41 56 41 57 48 83 EC 28 49 8B F1 4D 8B F8 48 8B EA 48 8B D9 E8 ?? ?? ?? ?? 48 8B 8B 98 11 00 00 48 8B 01 FF 50 08 8B 83 A8 11 00 00")
DD_ENTRY(StorageQueueWrite, "40 53 55 56 57")
// StorageManager::ReadFile（248D60）：(SM, 设备名, 路径, 缓冲, 容量, 解密)，返回实读字节，对调用者同步。通用读文件入口
DD_SIG(StorageReadFile, HOOK, OPT, 5, 0, "?? ?? ?? ?? ?? 41 54 41 55 41 56 41 57 48 81 EC 08 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 F0 00 00 00 44 8B AC 24 70 01 00 00 48 8B EA 48 8D 54 24 20 4D 8B F9 4D 8B F0 48 8B F9 E8")
DD_ENTRY(StorageReadFile, "40 53 55 56 57")
// 前端选存档槽时把根目录的存档整体复制到 backup/（2923A0，41C390 唯一调用，没有参数、没有返回值），主线程同步完成
DD_SIG(SaveSlotBackup, HOOK, OPT, 5, 0, "?? ?? ?? ?? ?? 41 54 41 55 41 56 41 57 48 8D AC 24 F8 FD FF FF 48 81 EC 08 03 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 F0 01 00 00 41 B4 01 0F 57 C0 F3 0F 7F 44 24 48 45 33 ED")
DD_ENTRY(SaveSlotBackup, "40 55 53 56 57")
// 退到桌面（3D1CC0）：从菜单退出和关窗口都经过这里，在主线程
DD_SIG(AppShutdown, HOOK, OPT, 5, 0, "?? ?? ?? ?? ?? 41 54 41 56 41 57 48 8D 6C 24 D9 48 81 EC E0 00 00 00 0F B6 05 ?? ?? ?? ?? 84 C0 75 ?? FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 0F B6 05 ?? ?? ?? ?? 84 C0 74")
DD_ENTRY(AppShutdown, "40 55 53 56 57")
// StorageManager 单例指针（1185248），取自 2923A0 开头：mov rcx,[SM]; mov r8,[rcx+0x38]; add r8,0x20。
// 同一段代码也确认了槽 5（当前 profile）在 SM+0x38、设备名在设备对象 +0x20
DD_SIG(StorageManager, GLOBAL, OPT, 0, 11, "45 33 ED 4C 89 6C 24 58 48 8B 0D ?? ?? ?? ?? 4C 8B 41 38 49 83 C0 20")

// 已知的 exe 版本。未知 SHA1 但特征码全部命中时只警告
DD_KNOWN_BUILD("9db2d73b89aa034e2055df65a139403e18df641f", "steam")
DD_KNOWN_BUILD("3f133c26312bb667f11105ddf41f92275398a8d3", "nosteam")
