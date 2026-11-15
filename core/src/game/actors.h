#pragma once
// actor 句柄的解析（game 服务的 actor 部分）。句柄是值（DDActorRef），每次使用都重新解析：
//   英雄  Campaign::Current（Roster::System）+ 0（主名册）或 +0x728（备用名册）的 m_HeroEntries（+0x20），
//         按 Entry+8 处 Hero 的 +0x137C（hero id）找；
//   怪物  Raid::Current+0x4898（Battle）的 +0x38（vector<Monster*>），按 Monster+0x1320（guid）找；guid 只在一场战斗内
//         唯一，句柄还要求战斗序号相同（PhaseTracker::ObserveBattle，取样用 ReadBattleSample）。
// 读游戏内存全部在 SEH 保护下进行，虚表必须精确等于 Hero / Monster 的虚表（两者都没有子类）。
// 依据：knowledge/dd1/combat.md 的「Actor 结构」「Actor 的标识和反查」；两个版本的偏移由 tools/check_actor_layout.py 核对。
// 只在主线程调用（游戏对象只在主线程上修改）。离线测试用假的内存布局覆盖。
//
// 枚举（F5，P5 的 dd.raid 的只读部分）：
//   队伍  Raid::Current+8 的 vector<Hero*>（4C41F0 对组类目标、槽位 13 的 target==1 分支都读它；Battle::HeroesRerank 5B8F80
//         换位后按站位写回），每个指针都要虚表是 Hero、并且在名册里（ADR 0024 的成员关系）才给句柄；
//   怪物  Battle+0x38（Raid+0x48D0），只算虚表是 Monster 的，表里的英雄跳过。

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

// 结构偏移（Steam 版和离线版相同，见 check_actor_layout.py）
namespace actor_layout {
constexpr std::uintptr_t kRosterAlt = 0x728;       // Campaign+0x728：备用 Roster::System（App+0x54B 非零时游戏用它）
constexpr std::uintptr_t kRosterHeroes = 0x20;     // Roster::System::m_HeroEntries，vector<Roster::Entry*>
constexpr std::uintptr_t kRaidHeroes = 0x08;       // Raid 的队伍，vector<Hero*>（4C41F0 的 target_hero_random 读 +8/+0x10）
constexpr std::uintptr_t kEntryHero = 0x08;        // Entry 内嵌的 Hero
constexpr std::uintptr_t kHeroId = 0x137C;         // Hero 虚函数槽位 5
constexpr std::uintptr_t kHeroClass = 0x1320;      // Hero 虚函数槽位 6：HeroClass*
constexpr std::uintptr_t kMonsterGuid = 0x1320;    // Monster 虚函数槽位 5
constexpr std::uintptr_t kMonsterClass = 0x1468;   // Monster 虚函数槽位 6：MonsterClass*
constexpr std::uintptr_t kRaidBattle = 0x4898;     // Raid 内嵌的 Battle
constexpr std::uintptr_t kBattleMonsters = 0x38;   // Battle 的 vector<Monster*>
constexpr std::uintptr_t kBattleState = 0x288;     // Battle 的状态（bs_none = 0），即 Raid+0x4B20
constexpr std::uintptr_t kBattleGuidCounter = 0x2D4;  // 本场的怪物 guid 计数（Raid+0x4B6C），Battle::Create 清零
constexpr std::uintptr_t kActorHp = 0x10A8;        // float 当前生命
constexpr std::uintptr_t kActorStress = 0x12FC;    // float 压力
constexpr std::uintptr_t kActorMaxHpStat = 0x768;  // 最大生命的属性值结构（传给 ActorMaxStat）
constexpr std::uintptr_t kActorMaxHpMask = 0x790;  // 同上的掩码参数
constexpr std::uintptr_t kClassId = 0x08;          // ActorClass+8：职业 id，char[0x40]
constexpr std::size_t kClassIdCapacity = 0x40;
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
