#include "actors.h"

#include <windows.h>

#include <cmath>
#include <cstring>

namespace ddc::game {

namespace {

using namespace actor_layout;

using MaxStatFn = float (*)(const void* stat, std::uint32_t mask, bool clamp);

bool ReadBytes(std::uintptr_t src, void* dst, std::size_t size)
{
    if (src == 0) {
        return false;
    }
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
bool Read(std::uintptr_t address, T& out)
{
    return ReadBytes(address, &out, sizeof(T));
}

std::uintptr_t LoadGlobal(void* const volatile* global)
{
    std::uintptr_t value = 0;
    return global && Read(reinterpret_cast<std::uintptr_t>(global), value) ? value : 0;
}

// 游戏的属性值合计函数：叶函数，只读，崩溃时按失败处理
bool CallMaxStat(const void* fn, std::uintptr_t stat, std::uint32_t mask, float* out)
{
    __try {
        *out = reinterpret_cast<MaxStatFn>(const_cast<void*>(fn))(reinterpret_cast<const void*>(stat), mask, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// vector<T*> 的元素个数，结构不合法时返回 false
bool PointerVector(std::uintptr_t address, std::size_t max, std::uintptr_t* first, std::size_t* count)
{
    std::uintptr_t vec[3] = {};
    if (!ReadBytes(address, vec, sizeof(vec))) {
        return false;
    }
    if (vec[0] == 0 && vec[1] == 0) {
        *first = 0;
        *count = 0;
        return true;
    }
    if (vec[0] == 0 || vec[1] < vec[0] || vec[2] < vec[1] || (vec[1] - vec[0]) % sizeof(void*) != 0) {
        return false;
    }
    *first = vec[0];
    *count = (vec[1] - vec[0]) / sizeof(void*);
    return *count <= max;
}

bool HeroesUsable(const ActorEnv& env)
{
    return env.heroVtable && env.campaignCurrent;
}

bool MonstersUsable(const ActorEnv& env)
{
    return env.monsterVtable && env.raidCurrent;
}

std::uintptr_t VtableOf(std::uintptr_t object)
{
    std::uintptr_t vtable = 0;
    return Read(object, vtable) ? vtable : 0;
}

// 在名册里找英雄：match 为 0 时按 id 找，否则要求指针等于 match。返回 Hero 指针，找不到返回 0
std::uintptr_t ScanRoster(const ActorEnv& env, std::uint32_t roster, std::uint32_t id, std::uintptr_t match)
{
    const std::uintptr_t campaign = LoadGlobal(env.campaignCurrent);
    if (campaign == 0 || roster > 1) {
        return 0;
    }
    const std::uintptr_t system = campaign + (roster ? kRosterAlt : 0);
    std::uintptr_t first = 0;
    std::size_t count = 0;
    if (!PointerVector(system + kRosterHeroes, kMaxRosterEntries, &first, &count)) {
        return 0;
    }
    const auto heroVtable = reinterpret_cast<std::uintptr_t>(env.heroVtable);
    for (std::size_t i = 0; i < count; ++i) {
        std::uintptr_t entry = 0;
        if (!Read(first + i * sizeof(void*), entry) || entry == 0) {
            continue;
        }
        const std::uintptr_t hero = entry + kEntryHero;
        if (VtableOf(hero) != heroVtable) {
            continue;
        }
        if (match != 0) {
            if (hero == match) {
                return hero;
            }
            continue;
        }
        std::uint32_t heroId = 0;
        if (Read(hero + kHeroId, heroId) && heroId == id) {
            return hero;
        }
    }
    return 0;
}

// 在当前战斗的怪物表里找：match 为 0 时按 guid 找，否则要求指针等于 match
std::uintptr_t ScanBattle(const ActorEnv& env, std::uint32_t guid, std::uintptr_t match)
{
    const std::uintptr_t raid = LoadGlobal(env.raidCurrent);
    if (raid == 0) {
        return 0;
    }
    std::uintptr_t first = 0;
    std::size_t count = 0;
    if (!PointerVector(raid + kRaidBattle + kBattleMonsters, kMaxBattleActors, &first, &count)) {
        return 0;
    }
    const auto monsterVtable = reinterpret_cast<std::uintptr_t>(env.monsterVtable);
    for (std::size_t i = 0; i < count; ++i) {
        std::uintptr_t monster = 0;
        if (!Read(first + i * sizeof(void*), monster) || monster == 0) {
            continue;
        }
        if (VtableOf(monster) != monsterVtable) {
            continue;  // 怪物表里也可能有英雄（被俘获之类），不算怪物
        }
        if (match != 0) {
            if (monster == match) {
                return monster;
            }
            continue;
        }
        std::uint32_t value = 0;
        if (Read(monster + kMonsterGuid, value) && value == guid) {
            return monster;
        }
    }
    return 0;
}

void FillHero(DDActorRef* out, std::uint32_t id, const ActorEnv& env, std::uint32_t roster)
{
    out->size = sizeof(DDActorRef);
    out->kind = DD_ACTOR_HERO;
    out->id = id;
    out->campaign_serial = env.campaignSerial;
    out->raid_serial = 0;
    out->battle_serial = 0;
    out->roster = roster;
}

void FillMonster(DDActorRef* out, std::uint32_t guid, const ActorEnv& env)
{
    out->size = sizeof(DDActorRef);
    out->kind = DD_ACTOR_MONSTER;
    out->id = guid;
    out->campaign_serial = env.campaignSerial;
    out->raid_serial = env.raidSerial;
    out->battle_serial = env.battleSerial;
    out->roster = 0;
}

bool ValidRef(const DDActorRef* ref)
{
    return ref && ref->size >= sizeof(DDActorRef);
}

}  // namespace

std::uint32_t ActorKinds(const ActorEnv& env)
{
    return (HeroesUsable(env) ? 1u << DD_ACTOR_HERO : 0u) | (MonstersUsable(env) ? 1u << DD_ACTOR_MONSTER : 0u);
}

bool ActorFromPointer(const ActorEnv& env, const void* actor, DDActorRef* out)
{
    if (!actor || !out || out->size < sizeof(DDActorRef) || !env.campaignActive) {
        return false;
    }
    const auto object = reinterpret_cast<std::uintptr_t>(actor);
    const std::uintptr_t vtable = VtableOf(object);
    if (vtable == 0) {
        return false;
    }
    if (HeroesUsable(env) && vtable == reinterpret_cast<std::uintptr_t>(env.heroVtable)) {
        std::uint32_t id = 0;
        if (!Read(object + kHeroId, id)) {
            return false;
        }
        for (std::uint32_t roster = 0; roster <= 1; ++roster) {
            if (ScanRoster(env, roster, 0, object) == object) {
                FillHero(out, id, env, roster);
                return true;
            }
        }
        return false;
    }
    if (MonstersUsable(env) && vtable == reinterpret_cast<std::uintptr_t>(env.monsterVtable)) {
        std::uint32_t guid = 0;
        if (!Read(object + kMonsterGuid, guid) || ScanBattle(env, 0, object) != object) {
            return false;
        }
        FillMonster(out, guid, env);
        return true;
    }
    return false;
}

bool ActorFind(const ActorEnv& env, std::uint32_t kind, std::uint32_t id, DDActorRef* out)
{
    if (!out || out->size < sizeof(DDActorRef) || !env.campaignActive) {
        return false;
    }
    if (kind == DD_ACTOR_HERO && HeroesUsable(env) && ScanRoster(env, 0, id, 0) != 0) {
        FillHero(out, id, env, 0);
        return true;
    }
    if (kind == DD_ACTOR_MONSTER && MonstersUsable(env) && ScanBattle(env, id, 0) != 0) {
        FillMonster(out, id, env);
        return true;
    }
    return false;
}

void* ActorResolve(const ActorEnv& env, const DDActorRef& ref)
{
    if (!ValidRef(&ref) || !env.campaignActive || ref.campaign_serial != env.campaignSerial) {
        return nullptr;
    }
    std::uintptr_t found = 0;
    if (ref.kind == DD_ACTOR_HERO && HeroesUsable(env) && ref.raid_serial == 0 && ref.battle_serial == 0) {
        found = ScanRoster(env, ref.roster, ref.id, 0);
    } else if (ref.kind == DD_ACTOR_MONSTER && MonstersUsable(env) && ref.roster == 0 && ref.raid_serial == env.raidSerial &&
        ref.battle_serial == env.battleSerial) {
        found = ScanBattle(env, ref.id, 0);
    }
    return reinterpret_cast<void*>(found);
}

bool ActorReadInfo(const ActorEnv& env, const DDActorRef& ref, DDActorInfo* out)
{
    if (!out || out->size < sizeof(DDActorInfo)) {
        return false;
    }
    const auto object = reinterpret_cast<std::uintptr_t>(ActorResolve(env, ref));
    if (object == 0) {
        return false;
    }
    DDActorInfo info{};
    info.size = sizeof(DDActorInfo);
    info.kind = ref.kind;
    info.id = ref.id;
    if (!Read(object + kActorHp, info.hp)) {
        return false;
    }
    if (ref.kind == DD_ACTOR_HERO) {
        info.has_stress = Read(object + kActorStress, info.stress) && std::isfinite(info.stress);
    }
    std::uint32_t mask = 0;
    float maxHp = 0;
    if (env.maxStat && Read(object + kActorMaxHpMask, mask) && CallMaxStat(env.maxStat, object + kActorMaxHpStat, mask, &maxHp) &&
        std::isfinite(maxHp)) {
        info.max_hp = maxHp;
        info.has_max_hp = true;
    }
    std::uintptr_t actorClass = 0;
    char id[kClassIdCapacity] = {};
    if (Read(object + (ref.kind == DD_ACTOR_HERO ? kHeroClass : kMonsterClass), actorClass) &&
        ReadBytes(actorClass + kClassId, id, sizeof(id))) {
        const std::size_t length = strnlen(id, sizeof(id));
        if (length < sizeof(id) && length < sizeof(info.class_id)) {
            std::memcpy(info.class_id, id, length);
            info.class_id[length] = '\0';
        }
    }
    *out = info;
    return true;
}

BattleSample ReadBattleSample(void* const volatile* raidCurrent)
{
    BattleSample sample;
    const std::uintptr_t raid = LoadGlobal(raidCurrent);
    if (raid == 0) {
        return sample;
    }
    const std::uintptr_t battle = raid + kRaidBattle;
    if (!Read(battle + kBattleState, sample.state) || !Read(battle + kBattleGuidCounter, sample.guidCounter)) {
        sample.state = 0;
        return sample;
    }
    sample.raid = true;
    std::uintptr_t first = 0;
    std::size_t count = 0;
    sample.monsters = PointerVector(battle + kBattleMonsters, kMaxBattleActors, &first, &count) ? static_cast<std::uint32_t>(count) : 0;
    return sample;
}

}  // namespace ddc::game
