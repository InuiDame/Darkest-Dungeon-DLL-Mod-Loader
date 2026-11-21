#pragma once
// actor 句柄的解析（game 服务的 actor 部分）。句柄是值（DDActorRef），每次使用都重新解析：
//   英雄  Campaign::Current（Roster::System）+ 0（主名册）或 +0x728（备用名册）的 m_HeroEntries（+0x20），
//         按 Entry+8 处 Hero 的 +0x137C（hero id）找；
//   怪物  Raid::Current+0x4898（Battle）的 +0x38（vector<Monster*>），按 Monster+0x1320（guid）找；guid 只在一场战斗内
//         唯一，句柄还要求战斗序号相同（PhaseTracker::ObserveBattle，取样用 ReadBattleSample）。
// 读游戏内存全部在 SEH 保护下进行，虚表必须精确等于 Hero / Monster 的虚表（两者都没有子类）。
// 依据：knowledge/dd1/combat.md 的「Actor 结构」「Actor 的标识和反查」；偏移在 layout.inc，由 tools/dd_update.py verify 核对。
// 只在主线程调用（游戏对象只在主线程上修改）。离线测试用假的内存布局覆盖。
//
// 枚举（F5，P5 的 dd.raid 的只读部分）：
//   队伍  Raid::Current+8 的 vector<Hero*>（4C41F0 对组类目标、槽位 13 的 target==1 分支都读它；Battle::HeroesRerank 5B8F80
//         换位后按站位写回），每个指针都要虚表是 Hero、并且在名册里（ADR 0024 的成员关系）才给句柄；
//   怪物  Battle+0x38（Raid+0x48D0），只算虚表是 Monster 的，表里的英雄跳过。

#include "layout.h"
#include "phase_tracker.h"

#include <dd/dd_api.h>

#include <cstddef>
#include <cstdint>

namespace ddc::game {

struct ActorEnv {
    const void* heroVtable = nullptr;
    const void* monsterVtable = nullptr;
    void* const volatile* campaignCurrent = nullptr;  // Campaign::Current 的全局指针
    void* const volatile* raidCurrent = nullptr;      // Raid::Current 的全局指针
    const void* maxStat = nullptr;                     // float (const void* stat, uint32_t mask, bool clamp)，可为空
    bool campaignActive = false;
    std::uint32_t campaignSerial = 0;
    std::uint32_t raidSerial = 0;
    std::uint32_t battleSerial = 0;
};

// 结构偏移的唯一来源是 layout.inc（tools/dd_update.py verify 核对），这里是 actors.cpp 和单元测试用的名字，
// 另外是读表时的合理性上限（不是游戏布局）
namespace actor_layout {
using layout::kActorHp;
using layout::kActorMaxHpMask;
using layout::kActorMaxHpStat;
using layout::kActorStress;
using layout::kBattleGuidCounter;
using layout::kBattleMonsters;
using layout::kBattleState;
using layout::kClassId;
using layout::kEntryHero;
using layout::kHeroClass;
using layout::kHeroId;
using layout::kMonsterClass;
using layout::kMonsterGuid;
using layout::kRaidBattle;
using layout::kRaidHeroes;
using layout::kRosterAlt;
using layout::kRosterHeroes;
constexpr std::size_t kClassIdCapacity = layout::kClassIdCap;
constexpr std::size_t kMaxRosterEntries = 4096;
constexpr std::size_t kMaxBattleActors = 64;
constexpr std::size_t kMaxPartyHeroes = 16;        // 游戏的队伍最多 4 人，超过 16 当作结构读错
}  // namespace actor_layout

// 可用的种类位（1u << DD_ACTOR_HERO 等）
std::uint32_t ActorKinds(const ActorEnv& env);
bool ActorFromPointer(const ActorEnv& env, const void* actor, DDActorRef* out);
bool ActorFind(const ActorEnv& env, std::uint32_t kind, std::uint32_t id, DDActorRef* out);
void* ActorResolve(const ActorEnv& env, const DDActorRef& ref);
bool ActorReadInfo(const ActorEnv& env, const DDActorRef& ref, DDActorInfo* out);
// 当前战斗的取样（SEH 保护），给 PhaseTracker::ObserveBattle
BattleSample ReadBattleSample(void* const volatile* raidCurrent);
// 枚举：写入至多 capacity 个句柄（out 可为空），返回能给出句柄的总个数。没有 Raid、结构读不出来时是 0。
// 队伍按 Raid+8 的顺序，怪物按 Battle+0x38 的顺序（站位）
std::uint32_t ActorListParty(const ActorEnv& env, DDActorRef* out, std::uint32_t capacity);
std::uint32_t ActorListMonsters(const ActorEnv& env, DDActorRef* out, std::uint32_t capacity);

}  // namespace ddc::game
