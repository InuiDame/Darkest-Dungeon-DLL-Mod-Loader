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

// 已知的 exe 版本。未知 SHA1 但特征码全部命中时只警告
DD_KNOWN_BUILD("9db2d73b89aa034e2055df65a139403e18df641f", "steam")
DD_KNOWN_BUILD("3f133c26312bb667f11105ddf41f92275398a8d3", "nosteam")
