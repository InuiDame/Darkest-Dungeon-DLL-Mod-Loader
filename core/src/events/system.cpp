#include "system.h"

#include "../game/layout.h"
#include "../guard.h"
#include "churn_log.h"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <bit>
#include <climits>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ddc::events {

namespace {

// 游戏布局常量的唯一来源是 game/layout.inc（tools/dd_update.py verify 核对，依据见 knowledge/dd1/combat.md、events.md）
using namespace game::layout;

constexpr int kMaxFields = 32;

// ---- 字段映射 ----

enum class Raw { I32, U32, F32, B8, Ptr, Str, ConstTrue, ConstFalse };

struct FieldMap {
    const char* field;
    Raw raw;
    std::uint32_t offset;
    std::uint32_t cap = 0;  // Raw::Str 的容量
};

// 写入时的取值范围（按原始宽度，见 hub.h 的 FieldRange）
FieldRange RangeOf(Raw raw)
{
    switch (raw) {
    case Raw::I32:
        return FieldRange::I32;
    case Raw::U32:
        return FieldRange::U32;
    case Raw::F32:
        return FieldRange::F32;
    default:
        return FieldRange::Full;
    }
}

DDEventFieldType ToType(Raw raw)
{
    switch (raw) {
    case Raw::I32:
    case Raw::U32:
        return DD_FIELD_INT;
    case Raw::F32:
        return DD_FIELD_FLOAT;
    case Raw::B8:
    case Raw::ConstTrue:
    case Raw::ConstFalse:
        return DD_FIELD_BOOL;
    case Raw::Ptr:
        return DD_FIELD_POINTER;
    case Raw::Str:
        return DD_FIELD_STRING;
    }
    return DD_FIELD_INT;
}

void ReadRaw(const void* base, const FieldMap& map, Slot& slot)
{
    const auto* p = static_cast<const std::uint8_t*>(base) + map.offset;
    slot.set = true;
    switch (map.raw) {
    case Raw::I32:
        slot.v.i = *reinterpret_cast<const std::int32_t*>(p);
        break;
    case Raw::U32:
        slot.v.i = *reinterpret_cast<const std::uint32_t*>(p);
        break;
    case Raw::F32:
        slot.v.f = *reinterpret_cast<const float*>(p);
        break;
    case Raw::B8:
        slot.v.b = *p != 0;
        break;
    case Raw::Ptr:
        slot.v.p = *reinterpret_cast<void* const*>(p);
        break;
    case Raw::Str:
        slot.v.s = reinterpret_cast<const char*>(p);
        break;
    case Raw::ConstTrue:
        slot.v.b = true;
        break;
    case Raw::ConstFalse:
        slot.v.b = false;
        break;
    }
}

void WriteRaw(void* base, const FieldMap& map, const Slot& slot)
{
    auto* p = static_cast<std::uint8_t*>(base) + map.offset;
    switch (map.raw) {
    case Raw::I32:
    case Raw::U32:
        *reinterpret_cast<std::int32_t*>(p) = static_cast<std::int32_t>(slot.v.i);
        break;
    case Raw::F32:
        *reinterpret_cast<float*>(p) = static_cast<float>(slot.v.f);
        break;
    case Raw::B8:
        *p = slot.v.b ? 1 : 0;
        break;
    case Raw::Ptr:
        *reinterpret_cast<void**>(p) = slot.v.p;
        break;
    default:
        break;
    }
}

// 原生事件到语义主题的映射。字段偏移相对事件对象起点（layout.inc 的 Ev*），依据见 knowledge/dd1/events.md
struct NativeSource {
    const char* topic;
    const char* event;
    std::vector<FieldMap> fields;
};

const std::vector<NativeSource>& NativeSources()
{
    static const std::vector<NativeSource> kSources = {
        { "battle.start", "BattleCreatedEvent", {} },
        { "battle.end", "BattleDestroyedEvent", {} },
        { "actor.deathsdoor", "HeroReachedDeathsDoorEvent",
            { { "hero", Raw::Ptr, kEvHeroReachedDeathsDoor_hero }, { "damage_type", Raw::I32, kEvHeroReachedDeathsDoor_damage_type }, { "source_id", Raw::U32, kEvHeroReachedDeathsDoor_source_id },
                { "source_type", Raw::I32, kEvHeroReachedDeathsDoor_source_type } } },
        { "actor.heartattack", "HeartAttackEvent",
            { { "heart_attacks", Raw::I32, kEvHeartAttack_heart_attacks }, { "flag0", Raw::B8, kEvHeartAttack_flag0 }, { "flag1", Raw::B8, kEvHeartAttack_flag1 },
                { "flag2", Raw::B8, kEvHeartAttack_flag2 } } },
        { "actor.death", "HeroDeathEvent",
            { { "hero_id", Raw::I32, kEvHeroDeath_hero_id }, { "is_monster", Raw::ConstFalse, 0 } } },
        { "actor.death", "MonsterDeathEvent",
            { { "monster_guid", Raw::I32, kEvMonsterDeath_monster_guid }, { "attacker_id", Raw::I32, kEvMonsterDeath_attacker_id }, { "damage_type", Raw::I32, kEvMonsterDeath_damage_type },
                { "class_field_d0", Raw::I32, kEvMonsterDeath_class_field_d0 }, { "class_field_cc", Raw::I32, kEvMonsterDeath_class_field_cc }, { "flag", Raw::B8, kEvMonsterDeath_flag },
                { "is_monster", Raw::ConstTrue, 0 } } },
        { "actor.affliction", "HeroAfflictionVirtueEvent", { { "hero", Raw::Ptr, kEvHeroAfflictionVirtue_hero } } },
        { "raid.start", "RaidStartEvent", { { "raid", Raw::Ptr, kEvRaidStart_raid } } },
        { "raid.finish", "RaidFinishResultsEvent", { { "results", Raw::Ptr, kEvRaidFinishResults_results } } },
        { "raid.step", "PartyStepEvent", {} },
        { "raid.torch", "TorchModifiedEvent",
            { { "torch_before", Raw::F32, kEvTorchModified_torch_before }, { "torch_after", Raw::F32, kEvTorchModified_torch_after }, { "delta", Raw::F32, kEvTorchModified_delta },
                { "flag", Raw::B8, kEvTorchModified_flag } } },
        { "town.enter", "PreOnStartTownVisitEvent", { { "flag", Raw::B8, kEvPreOnStartTownVisit_flag } } },
        { "town.event", "TownEventOnStartTownVisitEvent", { { "event", Raw::Ptr, kEvTownEventOnStartTownVisit_event } } },
        { "town.building", "BuildingStateChangedEvent",
            { { "name", Raw::Str, kEvBuildingStateChanged_name, kEvBuildingStateChanged_name_cap }, { "value", Raw::I32, kEvBuildingStateChanged_value }, { "flag", Raw::B8, kEvBuildingStateChanged_flag } } },
        { "town.activity", "ActivityOpenedEvent", { { "activity", Raw::Ptr, kEvActivityOpened_activity }, { "flag", Raw::B8, kEvActivityOpened_flag } } },
        { "town.activity.finish", "ActivityFinishedEvent", { { "activity_id", Raw::I32, kEvActivityFinished_activity_id } } },
        { "hero.recruit", "HeroRecruitedEvent", { { "hero_id", Raw::I32, kEvHeroRecruited_hero_id } } },
        { "hero.dismiss", "HeroDismissedEvent", { { "hero_id", Raw::I32, kEvHeroDismissed_hero_id } } },
        { "hero.quirk", "QuirkAppliedEvent",
            { { "hero_id", Raw::I32, kEvQuirkApplied_hero_id }, { "kind", Raw::I32, kEvQuirkApplied_kind }, { "quirk", Raw::Str, kEvQuirkApplied_quirk, kEvQuirkApplied_quirk_cap } } },
        { "hero.skill", "HeroSkillUpgradedEvent", { { "hero_id", Raw::I32, kEvHeroSkillUpgraded_hero_id }, { "skill_index", Raw::I32, kEvHeroSkillUpgraded_skill_index } } },
        { "inventory.add", "InventoryAddItemEvent",
            { { "inventory", Raw::Ptr, kEvInventoryAddItem_inventory }, { "item", Raw::Ptr, kEvInventoryAddItem_item }, { "value", Raw::I32, kEvInventoryAddItem_value } } },
        { "inventory.modified", "InventoryModifiedEvent", {} },
        { "inventory.premerge", "InventoryPreMergeEvent",
            { { "inventory", Raw::Ptr, kEvInventoryPreMerge_inventory }, { "other", Raw::Ptr, kEvInventoryPreMerge_other } } },
        { "inventory.postmerge", "InventoryPostMergeEvent",
            { { "inventory", Raw::Ptr, kEvInventoryPostMerge_inventory }, { "other", Raw::Ptr, kEvInventoryPostMerge_other } } },
        { "estate.currency", "EstateCurrencyChangeEvent",
            { { "currency", Raw::U32, kEvEstateCurrencyChange_currency }, { "delta", Raw::I32, kEvEstateCurrencyChange_delta } } },
    };
    return kSources;
}

struct QueryField {
    FieldMap map;
    bool writable;
};

struct QueryDef {
    const char* name;
    std::vector<QueryField> fields;
};

const std::vector<QueryDef>& QueryDefs()
{
    static const std::vector<QueryDef> kDefs = {
        { "CampingPointBuffQuery", { { { "points", Raw::I32, kQyCampingPointBuff_points }, true } } },
        { "CurioInteractionBuffQuery",
            { { { "hero", Raw::Ptr, kQyCurioInteractionBuff_hero }, false }, { { "curio", Raw::Ptr, kQyCurioInteractionBuff_curio }, false },
                { { "stress", Raw::F32, kQyCurioInteractionBuff_stress }, true }, { { "heal", Raw::F32, kQyCurioInteractionBuff_heal }, true } } },
        { "FindNonRosterHeroesQuery", { { { "container", Raw::Ptr, kQyFindNonRosterHeroes_container }, false } } },
        { "InventoryExtraStackingLimitQuery",
            { { { "item_id", Raw::U32, kQyInventoryExtraStackingLimit_item_id }, false }, { { "arg", Raw::U32, kQyInventoryExtraStackingLimit_arg }, false },
                { { "limit", Raw::I32, kQyInventoryExtraStackingLimit_limit }, true } } },
        { "InventoryReplacementEffectIdQuery",
            { { { "item_id", Raw::U32, kQyInventoryReplacementEffectId_item_id }, false }, { { "arg", Raw::U32, kQyInventoryReplacementEffectId_arg }, false },
                { { "effect_id", Raw::I32, kQyInventoryReplacementEffectId_effect_id }, true } } },
        { "IsDisabledQuery", { { { "hero_id", Raw::U32, kQyIsDisabled_hero_id }, false }, { { "disabled", Raw::B8, kQyIsDisabled_disabled }, true } } },
        { "LootDarknessBuffQuery",
            { { { "light", Raw::F32, kQyLootDarknessBuff_light }, false }, { { "result", Raw::Ptr, kQyLootDarknessBuff_result }, true },
                { { "arg", Raw::I32, kQyLootDarknessBuff_arg }, false } } },
        { "ProvisionIsItemTooltipsEnabledQuery", { { { "enabled", Raw::B8, kQyProvisionIsItemTooltipsEnabled_enabled }, true } } },
        { "ProvisionSupplyBuffQuery", { { { "items_first", Raw::Ptr, kQyProvisionSupplyBuff_items_first }, false } } },
        { "TownGoldInterestBuffQuery", { { { "interest", Raw::I32, kQyTownGoldInterestBuff_interest }, true } } },
        { "TownVisitIdleStressBuffQuery", { { { "stress_relief", Raw::F32, kQyTownVisitIdleStressBuff_stress_relief }, true }, { { "flag", Raw::B8, kQyTownVisitIdleStressBuff_flag }, true } } },
    };
    return kDefs;
}

// EventManager / QueryDispatcher 按类型序号索引的列表（layout.inc 的 ListDataSize、ListDataDirty）
struct ListData {
    std::uint8_t* first;
    std::uint8_t* last;
    std::uint8_t* end;
    std::uint8_t dirty;
    std::uint8_t pad[7];
};
static_assert(sizeof(ListData) == kListDataSize && offsetof(ListData, dirty) == kListDataDirty);

// Listener<T const> / Listener<T> 布局（ListenerPriority、ListenerFunction，std::function 的实现指针在其中 StdFunctionImpl）
struct FakeListener {
    void* vtable;
    std::int32_t priority;
    std::int32_t pad0;
    std::uint8_t funcStorage[kStdFunctionImpl];
    void* funcImpl;
};
static_assert(offsetof(FakeListener, priority) == kListenerPriority && offsetof(FakeListener, funcStorage) == kListenerFunction &&
    offsetof(FakeListener, funcImpl) == kListenerFunction + kStdFunctionImpl &&
    sizeof(FakeListener) == kListenerFunction + kStdFunctionSize);

// 派发策略，见 system.h
enum class ListKind { Event, Query };

struct TypeState;

struct FakeImpl {
    void* const* vtable;
    TypeState* type;
};

struct TypeState {
    ListKind kind = ListKind::Event;
    int id = -1;
    std::string name;
    void* listenerVtable = nullptr;
    bool forgedVtable = false;
    FakeListener listener{};
    FakeImpl impl{};
    bool registered = false;
    bool orderLogged = false;
    Topic* topic = nullptr;
    struct Target {
        Topic* topic;
        std::vector<FieldMap> fields;
        std::vector<std::uint32_t> slotIndices;  // fields[i] 对应 topic->fields 下标
    };
    std::vector<Target> targets;  // 映射到的语义主题（Event）；Query 的字段在 topic 上
    std::vector<QueryField> queryFields;
    std::vector<std::uint32_t> querySlotIndices;
    bool firstLogged = false;
};

// ---- 语义主题字段表 ----
// 语义主题的数字字段在游戏里都是 float 或 int32（DamageImpl 等写回时 static_cast<float>）
constexpr FieldRange SemanticRange(DDEventFieldType type)
{
    return type == DD_FIELD_FLOAT ? FieldRange::F32 : (type == DD_FIELD_INT ? FieldRange::I32 : FieldRange::Full);
}
#define F(n, t, a) FieldDesc{ n, DD_FIELD_##t, a, SemanticRange(DD_FIELD_##t) }

std::vector<FieldDesc> DamageFields(bool post)
{
    std::vector<FieldDesc> fields = {
        F("target", POINTER, kAccessR),
        F("attacker", POINTER, kAccessR),
        F("damage", FLOAT, post ? kAccessR : kAccessRW),
        F("damage_type", INT, kAccessR),
        F("source_id", INT, kAccessR),
        F("source_type", INT, kAccessR),
        F("crit", BOOL, kAccessR),
        F("hp_before", FLOAT, kAccessR),
        F("is_hero", BOOL, kAccessR),
    };
    if (post) {
        fields.push_back(F("hp_after", FLOAT, kAccessR));
        fields.push_back(F("timeline", POINTER, kAccessR));
        fields.push_back(F("cancelled", BOOL, kAccessR));
    }
    return fields;
}
enum { DF_target, DF_attacker, DF_damage, DF_type, DF_srcId, DF_srcType, DF_crit, DF_hp, DF_isHero, DF_hpAfter, DF_timeline, DF_cancelled };

// 最终伤害（P4，D12）：护体和格挡判定之后、扣血之前。absorb 可写
std::vector<FieldDesc> DamageFinalFields()
{
    return {
        F("target", POINTER, kAccessR),
        F("attacker", POINTER, kAccessR),
        F("damage", FLOAT, kAccessR),
        F("damage_type", INT, kAccessR),
        F("blocked", BOOL, kAccessR),
        F("deaths_door", BOOL, kAccessR),
        F("hp_before", FLOAT, kAccessR),
        F("is_hero", BOOL, kAccessR),
        F("absorb", FLOAT, kAccessRW),
    };
}
enum { DFF_target, DFF_attacker, DFF_damage, DFF_type, DFF_blocked, DFF_deathsDoor, DFF_hp, DFF_isHero, DFF_absorb };

std::vector<FieldDesc> EffectFields(bool post)
{
    std::vector<FieldDesc> fields = {
        F("performer", POINTER, kAccessR),
        F("target", POINTER, kAccessR),
        F("effect", POINTER, kAccessR),
        F("item", POINTER, kAccessR),
        F("source", INT, kAccessR),
        F("stress_amount", FLOAT, kAccessR),
        F("heal_amount", FLOAT, kAccessR),
        F("target_stress", FLOAT, kAccessR),
        F("target_hp", FLOAT, kAccessR),
        F("effect_name", STRING, kAccessR),
    };
    if (post) {
        fields.push_back(F("target_stress_after", FLOAT, kAccessR));
        fields.push_back(F("target_hp_after", FLOAT, kAccessR));
        fields.push_back(F("cancelled", BOOL, kAccessR));
    }
    return fields;
}
enum { EF_performer, EF_target, EF_effect, EF_item, EF_source, EF_stressAmt, EF_healAmt, EF_stress, EF_hp, EF_name, EF_stressAfter, EF_hpAfter, EF_cancelled };

// skill.pre 不开放写入（P4，D11）：DDCore 的 hook 点在游戏分派之后，执行者已经记下原技能（performer+0x8E0）、
// 即时 buff 已经施加，这里换技能或目标会和游戏状态对不上。技能字段经 combat.skill_from_pointer 转成技能句柄
std::vector<FieldDesc> SkillFields(bool)
{
    return {
        F("performer", POINTER, kAccessR),
        F("skill", POINTER, kAccessR),
        F("target", POINTER, kAccessR),
        F("target_count", INT, kAccessR),
        F("friendly", BOOL, kAccessR),
    };
}
enum { SF_performer, SF_skill, SF_target, SF_count, SF_friendly };

std::vector<FieldDesc> HitFields(bool post)
{
    return {
        F("attacker", POINTER, kAccessR),
        F("target", POINTER, kAccessR),
        F("skill", POINTER, kAccessR),
        F("hit", BOOL, post ? kAccessR : kAccessRW),
        F("crit", BOOL, post ? kAccessR : kAccessRW),
        F("damage", FLOAT, post ? kAccessR : kAccessRW),
    };
}
enum { HF_attacker, HF_target, HF_skill, HF_hit, HF_crit, HF_damage };

std::vector<FieldDesc> RoundFields()
{
    return { F("battle", POINTER, kAccessR), F("round", INT, kAccessR) };
}
enum { RF_battle, RF_round };

std::vector<FieldDesc> TurnFields(bool post)
{
    std::vector<FieldDesc> fields = { F("actor", POINTER, kAccessR) };
    if (post) {
        fields.push_back(F("result", INT, kAccessR));
    }
    return fields;
}
enum { TF_actor, TF_result };

std::vector<FieldDesc> StressFields(bool post)
{
    std::vector<FieldDesc> fields = {
        F("target", POINTER, kAccessR),
        F("source", POINTER, kAccessR),
        F("amount", FLOAT, post ? kAccessR : kAccessRW),
        F("type", INT, kAccessR),
        F("stress_before", FLOAT, kAccessR),
    };
    if (post) {
        fields.push_back(F("applied", FLOAT, kAccessR));
        fields.push_back(F("stress_after", FLOAT, kAccessR));
    }
    return fields;
}
enum { STF_target, STF_source, STF_amount, STF_type, STF_before, STF_applied, STF_after };

std::vector<FieldDesc> HealFields(bool post)
{
    std::vector<FieldDesc> fields = {
        F("target", POINTER, kAccessR),
        F("healer", POINTER, kAccessR),
        F("amount", FLOAT, post ? kAccessR : kAccessRW),
        F("type", INT, kAccessR),
        F("crit", BOOL, kAccessR),
        F("hp_before", FLOAT, kAccessR),
    };
    if (post) {
        fields.push_back(F("applied", FLOAT, kAccessR));
        fields.push_back(F("hp_after", FLOAT, kAccessR));
    }
    return fields;
}
enum { HLF_target, HLF_healer, HLF_amount, HLF_type, HLF_crit, HLF_before, HLF_applied, HLF_after };

// ---- detour 的全局上下文。detour 是普通函数，只能走全局 ----
struct Context;
Context* g_ctx = nullptr;

using DamageFn = void*(__fastcall*)(void*, void*, float, const int*, unsigned, const int*, bool, void*);
using HealFn = float(__fastcall*)(void*, void*, float, void*, int, bool, void*, bool);
using StressFn = float(__fastcall*)(void*, void*, float, void*, int);
using EffectFn = void(__fastcall*)(void*, void*, void*, const void*, void*, float, bool, float, int, unsigned, void*, bool, bool);
using VoidThis3 = void(__fastcall*)(void*, void*, void*);
using AttackSkillFn = void(__fastcall*)(void*, void*, const void*, void*);
using FriendlySkillFn = void(__fastcall*)(void*, void*, const void*, void*, bool);
using AdvanceRoundFn = void(__fastcall*)(void*);
using StartTurnFn = int(__fastcall*)(void*, void*, void*);
using ConsumeBlockFn = bool(__fastcall*)(void*, int, float);
using ActorPredicateFn = bool(__fastcall*)(void*);
using ListenerAddFn = void*(__fastcall*)(void*, void*);

// 原型和 layout.inc 的 Proto* 项绑定（参数个数，U1 修复 FA）：游戏更新改了原型要同时改这里和清单
static_assert(kArgs<DamageFn> == kProtoActorDamage && kArgs<HealFn> == kProtoActorHeal && kArgs<StressFn> == kProtoActorStress);
static_assert(kArgs<EffectFn> == kProtoEffectApplyInternal && kArgs<VoidThis3> == kProtoApplyHit && kArgs<VoidThis3> == kProtoApplyMiss);
static_assert(kArgs<AttackSkillFn> == kProtoActorDoAttackSkill && kArgs<FriendlySkillFn> == kProtoActorDoSkillFriendly);
static_assert(kArgs<AdvanceRoundFn> == kProtoBattleAdvanceRound && kArgs<StartTurnFn> == kProtoCombatStartTurn);
static_assert(kArgs<ConsumeBlockFn> == kProtoConsumeHealthDamageBlock && kArgs<ActorPredicateFn> == kProtoActorPredicate);
static_assert(kArgs<ListenerAddFn> == kProtoListenerAdd && kArgs<ListenerAddFn> == kProtoQueryListenerAdd);
// 按固定宽度读写的字段（layout.inc 的宽度项）
static_assert(kHitDataScalarSize == sizeof(float) && kBattleIntSize == sizeof(std::int32_t) && kActorFloatSize == sizeof(float));

struct HookOrig {
    DamageFn heroDamage = nullptr;
    DamageFn monsterDamage = nullptr;
    HealFn heroHeal = nullptr;
    HealFn monsterHeal = nullptr;
    StressFn heroStress = nullptr;
    StressFn monsterStress = nullptr;
    EffectFn effect = nullptr;
    VoidThis3 applyHit = nullptr;
    VoidThis3 applyMiss = nullptr;
    AttackSkillFn attackSkill = nullptr;
    FriendlySkillFn friendlySkill = nullptr;
    AdvanceRoundFn advanceRound = nullptr;
    StartTurnFn startTurn = nullptr;
    ConsumeBlockFn consumeBlock = nullptr;
};

// damage.final 要的攻击者：480400 的参数里没有，在外层的槽位 50（ApplyHealthDamageInternal）记下。
// 只有主线程用；嵌套的伤害（反击之类）按栈恢复
struct DamageContext {
    void* target = nullptr;
    void* attacker = nullptr;
    DamageContext* outer = nullptr;
};
DamageContext* g_damageContext = nullptr;

struct HookDef {
    const char* name = nullptr;
    bool isVtable = false;
    void* target = nullptr;        // 内联：函数入口；虚表：槽位地址
    const char* entryId = nullptr; // 内联：特征码 id，用于入口字节
    void* detour = nullptr;
    void** orig = nullptr;
    std::vector<Topic*> topics;  // 任意一个有订阅就安装
    std::function<bool()> extraDesired;  // 外部 hook：订阅之外的安装条件（例如有规则注册）
    std::uint64_t handle = 0;
    bool installed = false;
    bool failed = false;
    bool logged = false;
};

struct Context {
    Hub* hub = nullptr;
    DWORD mainThread = 0;
    game::Addresses* addr = nullptr;
    HookOrig orig;
    std::function<void(int level, const std::string& message)> log;
    Topic* damagePre = nullptr;
    Topic* damagePost = nullptr;
    Topic* effectPre = nullptr;
    Topic* effectPost = nullptr;
    Topic* skillPre = nullptr;
    Topic* skillPost = nullptr;
    Topic* hitPre = nullptr;
    Topic* hitPost = nullptr;
    Topic* roundStart = nullptr;
    Topic* roundEnd = nullptr;
    Topic* turnStart = nullptr;
    Topic* turnEnd = nullptr;
    Topic* stressPre = nullptr;
    Topic* stressPost = nullptr;
    Topic* healPre = nullptr;
    Topic* healPost = nullptr;
    Topic* damageFinal = nullptr;
};

bool IsMain()
{
    return GetCurrentThreadId() == g_ctx->mainThread;
}

void LogNonMainOnce(const char* name)
{
    static std::mutex mutex;
    static std::unordered_map<std::string, bool> logged;
    std::lock_guard lock(mutex);
    auto& flag = logged[name];
    if (!flag) {
        flag = true;
        char text[160];
        std::snprintf(text, sizeof(text), "语义 hook %s 在非主线程被调用（线程 %lu），已直通（只记一次）", name, GetCurrentThreadId());
        g_ctx->log(DD_LOG_WARN, text);
    }
}

int SafeReadInt(const void* p)
{
    __try {
        return *static_cast<const int*>(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

float ReadFloatRaw(const void* p)
{
    __try {
        return *static_cast<const float*>(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0f;
    }
}

float ReadFloat(const void* p, std::size_t offset)
{
    return p ? ReadFloatRaw(static_cast<const std::uint8_t*>(p) + offset) : 0.0f;
}

void* ReadPtr(const void* p, std::size_t offset)
{
    return p ? *reinterpret_cast<void* const*>(static_cast<const std::uint8_t*>(p) + offset) : nullptr;
}

bool ActivePvP()
{
    auto* addr = g_ctx->addr;
    if (!addr->raidCurrent || !*addr->raidCurrent) {
        return false;
    }
    __try {
        const auto* raid = static_cast<const std::uint8_t*>(*addr->raidCurrent);
        const char* name = *reinterpret_cast<const char* const*>(raid + kRaidPvpName);
        return name && std::strcmp(name, "arena_mp") == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Dispatchable(Topic* topic)
{
    return topic && g_ctx->hub->SubscriberCount(topic) > 0;
}

bool PreAllowed(Topic* topic)
{
    return Dispatchable(topic) && !ActivePvP();
}

// ---- 语义 hook 的 detour ----

void* DamageImpl(DamageFn orig, bool isHero, void* self, void* prng, float damage, const int* type, unsigned src,
    const int* st, bool crit, void* attacker)
{
    if (!IsMain() || !orig) {
        if (!IsMain()) {
            LogNonMainOnce("damage");
        }
        return orig ? orig(self, prng, damage, type, src, st, crit, attacker) : nullptr;
    }
    auto& c = *g_ctx;
    float amount = damage;
    const float hpBefore = ReadFloat(self, kActorHp);
    bool cancelled = false;
    Slot slots[kMaxFields]{};
    const auto fill = [&] {
        slots[DF_target].v.p = self;
        slots[DF_target].set = true;
        slots[DF_attacker].v.p = attacker;
        slots[DF_attacker].set = true;
        slots[DF_damage].v.f = amount;
        slots[DF_damage].set = true;
        slots[DF_type].v.i = type ? *type : 0;
        slots[DF_type].set = true;
        slots[DF_srcId].v.i = src;
        slots[DF_srcId].set = true;
        slots[DF_srcType].v.i = st ? *st : 0;
        slots[DF_srcType].set = true;
        slots[DF_crit].v.b = crit;
        slots[DF_crit].set = true;
        slots[DF_hp].v.f = hpBefore;
        slots[DF_hp].set = true;
        slots[DF_isHero].v.b = isHero;
        slots[DF_isHero].set = true;
    };
    if (PreAllowed(c.damagePre)) {
        fill();
        Payload payload;
        payload.slots = slots;
        payload.flags = DD_EVENT_F_PRE;
        payload.originThread = GetCurrentThreadId();
        payload.allowWrite = true;
        c.hub->Dispatch(c.damagePre, payload);
        amount = static_cast<float>(slots[DF_damage].v.f);
        if (payload.cancelled) {
            cancelled = true;
            amount = 0.0f;
        }
    }
    DamageContext context{ self, attacker, g_damageContext };
    g_damageContext = &context;
    void* result = orig(self, prng, amount, type, src, st, crit, attacker);
    g_damageContext = context.outer;
    if (Dispatchable(c.damagePost)) {
        Slot post[kMaxFields]{};
        if (!slots[DF_target].set) {
            fill();
        }
        std::memcpy(post, slots, sizeof(Slot) * (DF_isHero + 1));
        post[DF_damage].v.f = amount;
        post[DF_damage].set = true;
        post[DF_hpAfter].v.f = ReadFloat(self, kActorHp);
        post[DF_hpAfter].set = true;
        post[DF_timeline].v.p = result;
        post[DF_timeline].set = true;
        post[DF_cancelled].v.b = cancelled;
        post[DF_cancelled].set = true;
        Payload payload;
        payload.slots = post;
        payload.flags = DD_EVENT_F_POST;
        payload.originThread = GetCurrentThreadId();
        c.hub->Dispatch(c.damagePost, payload);
    }
    return result;
}

void* __fastcall HeroDamageDetour(void* self, void* prng, float damage, const int* type, unsigned src, const int* st,
    bool crit, void* attacker)
{
    return DamageImpl(g_ctx->orig.heroDamage, true, self, prng, damage, type, src, st, crit, attacker);
}

void* __fastcall MonsterDamageDetour(void* self, void* prng, float damage, const int* type, unsigned src,
    const int* st, bool crit, void* attacker)
{
    return DamageImpl(g_ctx->orig.monsterDamage, false, self, prng, damage, type, src, st, crit, attacker);
}

// Hero/Monster 虚表槽位 40、43（Hero 478190：HP==0，即在死门上；4781B0：HP<0 或 +0x1304，已死）。
// 4836A0 在 480400 返回假之后先问 43、再问 40，两个都为假才扣血（4836A0 的反编译，[静态]）
bool CallActorPredicate(void* actor, std::size_t slot, bool* out)
{
    __try {
        auto** vtable = *static_cast<void***>(actor);
        *out = reinterpret_cast<ActorPredicateFn>(vtable[slot])(actor);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SetActorHp(void* actor, float value)
{
    __try {
        *reinterpret_cast<float*>(static_cast<std::uint8_t*>(actor) + kActorHp) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

constexpr float kDamageEpsilon = std::bit_cast<float>(kDamageEpsilonBits);  // 4836A0 扣血前比较的阈值（0xEE61E0 处的常量）

// 怪物受伤（5FC3E0）在 480400 返回假之后的扣血规则（P4 审核 A-S2，[静态]）：只有免疫一个分支不扣血——
// [[Monster+0x1468]+0xF97]（.can_be_damaged_directly）为 0 且 DEB7D0[伤害类型] 非 0（类型 0–7、9、10、13–19）；
// 其余一律 HP -= 最终伤害（没有阈值，不问槽位 43、40）。+0x1306 的英雄型怪物改走英雄的 4836A0，不经过这个返回点。
enum class MonsterDeduct { NotMonsterPath, Deducts, Immune, Unknown };

MonsterDeduct MonsterRule(void* actor, int type)
{
    const auto* table = g_ctx->addr->monsterImmuneTable;
    if (type < 0 || type >= static_cast<int>(kMonsterImmuneTypeCount) || !table) {
        return MonsterDeduct::Unknown;  // 表只有 20 种类型有意义；超出时游戏读的是表外的字节
    }
    __try {
        const auto* p = static_cast<const std::uint8_t*>(actor);
        const auto* monsterClass = *reinterpret_cast<const std::uint8_t* const*>(p + kMonsterClass);
        if (!monsterClass) {
            return MonsterDeduct::Unknown;
        }
        return monsterClass[kMonsterClassCanBeDamagedDirectly] == 0 && table[type] != 0 ? MonsterDeduct::Immune : MonsterDeduct::Deducts;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return MonsterDeduct::Unknown;
    }
}

// damage.final：Actor::ConsumeHealthDamageBlock（480400，rcx=Actor、edx=伤害类型、xmm2=最终伤害），返回真表示格挡。
// 4836A0 先算 roundf((Actor+0xBE0 + 常量) * 伤害) 作为最终伤害，调用 480400；返回真就不扣血、显示 Blocked（弹字 0x31），
// 否则在 43、40 两个槽位都为假时扣 HP -= 最终伤害，再按扣完的 HP 判定死门。所以部分吸收在这里先把吸收量补回 HP，
// 死门按补回后的生命判定（DDShield 的 ShieldAbsorbFinal 也是这么做的）
bool __fastcall ConsumeBlockDetour(void* self, int type, float damage)
{
    auto& c = *g_ctx;
    if (!c.orig.consumeBlock) {
        return false;
    }
    // 调用者：怪物受伤的返回点是 MonsterBlockReturn；其余（英雄 4836A0，含英雄型怪物）按英雄的规则
    const void* caller = _ReturnAddress();
    const bool blocked = c.orig.consumeBlock(self, type, damage);
    if (!IsMain() || !self || !Dispatchable(c.damageFinal) || ActivePvP()) {
        return blocked;
    }
    const MonsterDeduct monster =
        c.addr->monsterBlockReturn && caller == c.addr->monsterBlockReturn ? MonsterRule(self, type) : MonsterDeduct::NotMonsterPath;
    bool dead = false;
    bool deathsDoor = false;
    const bool predicatesOk = CallActorPredicate(self, kVtSlotDead, &dead) && CallActorPredicate(self, kVtSlotDeathsDoor, &deathsDoor);
    const bool isHero = c.addr->heroVtable && *static_cast<void* const*>(self) == c.addr->heroVtable;
    void* attacker = g_damageContext && g_damageContext->target == self ? g_damageContext->attacker : nullptr;
    Slot slots[kMaxFields]{};
    const auto set = [&](int index) -> Slot& {
        slots[index].set = true;
        return slots[index];
    };
    set(DFF_target).v.p = self;
    set(DFF_attacker).v.p = attacker;
    set(DFF_damage).v.f = monster == MonsterDeduct::Immune ? 0.0f : damage;  // 免疫的怪物：游戏不扣血，最终伤害按 0
    set(DFF_type).v.i = type;
    set(DFF_blocked).v.b = blocked;
    set(DFF_deathsDoor).v.b = deathsDoor;
    set(DFF_hp).v.f = ReadFloat(self, kActorHp);
    set(DFF_isHero).v.b = isHero;
    set(DFF_absorb).v.f = 0.0;
    Payload payload;
    payload.slots = slots;
    payload.originThread = GetCurrentThreadId();
    payload.allowWrite = true;
    c.hub->Dispatch(c.damageFinal, payload);
    if (blocked || damage <= kDamageEpsilon || monster == MonsterDeduct::Immune) {
        return blocked;  // 免疫：游戏只弹 immune、不扣血，absorb 不起作用（也不改成 Blocked）
    }
    const double absorb = (std::min)((std::max)(slots[DFF_absorb].v.f, 0.0), static_cast<double>(damage));
    if (absorb >= damage) {
        return true;  // 全部吸收：按格挡处理
    }
    if (absorb <= 0.0) {
        return false;
    }
    // 部分吸收：先补回生命，游戏随后照常扣。只在确定游戏会扣血时补，否则补回的会变成治疗
    bool refund = false;
    switch (monster) {
    case MonsterDeduct::NotMonsterPath:
        refund = predicatesOk && !dead && !deathsDoor;  // 英雄：4836A0 在槽位 43、40 都为假时才扣
        break;
    case MonsterDeduct::Deducts:
        refund = ReadFloat(self, kActorHp) > 0.0f;  // 怪物一律扣；HP 已经不大于 0（被 .kill 之类杀死）时不补
        break;
    default:
        break;  // 判断不了：不补（吸收无效，宁可少挡）
    }
    if (refund) {
        SetActorHp(self, ReadFloat(self, kActorHp) + static_cast<float>(absorb));
    }
    return false;
}

float HealImpl(HealFn orig, void* self, void* prng, float amount, void* healer, int type, bool crit, void* out,
    bool silent)
{
    if (!IsMain() || !orig) {
        if (!IsMain()) {
            LogNonMainOnce("heal");
        }
        return orig ? orig(self, prng, amount, healer, type, crit, out, silent) : 0.0f;
    }
    auto& c = *g_ctx;
    float value = amount;
    const float hpBefore = ReadFloat(self, kActorHp);
    Slot slots[kMaxFields]{};
    const auto fill = [&] {
        slots[HLF_target].v.p = self;
        slots[HLF_target].set = true;
        slots[HLF_healer].v.p = healer;
        slots[HLF_healer].set = true;
        slots[HLF_amount].v.f = value;
        slots[HLF_amount].set = true;
        slots[HLF_type].v.i = type;
        slots[HLF_type].set = true;
        slots[HLF_crit].v.b = crit;
        slots[HLF_crit].set = true;
        slots[HLF_before].v.f = hpBefore;
        slots[HLF_before].set = true;
    };
    if (PreAllowed(c.healPre)) {
        fill();
        Payload payload;
        payload.slots = slots;
        payload.flags = DD_EVENT_F_PRE;
        payload.originThread = GetCurrentThreadId();
        payload.allowWrite = true;
        c.hub->Dispatch(c.healPre, payload);
        value = static_cast<float>(slots[HLF_amount].v.f);
    }
    const float applied = orig(self, prng, value, healer, type, crit, out, silent);
    if (Dispatchable(c.healPost)) {
        Slot post[kMaxFields]{};
        fill();
        std::memcpy(post, slots, sizeof(Slot) * (HLF_before + 1));
        post[HLF_amount].v.f = value;
        post[HLF_applied].v.f = applied;
        post[HLF_applied].set = true;
        post[HLF_after].v.f = ReadFloat(self, kActorHp);
        post[HLF_after].set = true;
        Payload payload;
        payload.slots = post;
        payload.flags = DD_EVENT_F_POST;
        payload.originThread = GetCurrentThreadId();
        c.hub->Dispatch(c.healPost, payload);
    }
    return applied;
}

float __fastcall HeroHealDetour(void* self, void* prng, float amount, void* healer, int type, bool crit, void* out,
    bool silent)
{
    return HealImpl(g_ctx->orig.heroHeal, self, prng, amount, healer, type, crit, out, silent);
}

float __fastcall MonsterHealDetour(void* self, void* prng, float amount, void* healer, int type, bool crit, void* out,
    bool silent)
{
    return HealImpl(g_ctx->orig.monsterHeal, self, prng, amount, healer, type, crit, out, silent);
}

float StressImpl(StressFn orig, void* self, void* prng, float amount, void* source, int type)
{
    if (!IsMain() || !orig) {
        if (!IsMain()) {
            LogNonMainOnce("stress");
        }
        return orig ? orig(self, prng, amount, source, type) : 0.0f;
    }
    auto& c = *g_ctx;
    float value = amount;
    const float before = ReadFloat(self, kActorStress);
    Slot slots[kMaxFields]{};
    const auto fill = [&] {
        slots[STF_target].v.p = self;
        slots[STF_target].set = true;
        slots[STF_source].v.p = source;
        slots[STF_source].set = true;
        slots[STF_amount].v.f = value;
        slots[STF_amount].set = true;
        slots[STF_type].v.i = type;
        slots[STF_type].set = true;
        slots[STF_before].v.f = before;
        slots[STF_before].set = true;
    };
    if (PreAllowed(c.stressPre)) {
        fill();
        Payload payload;
        payload.slots = slots;
        payload.flags = DD_EVENT_F_PRE;
        payload.originThread = GetCurrentThreadId();
        payload.allowWrite = true;
        c.hub->Dispatch(c.stressPre, payload);
        value = static_cast<float>(slots[STF_amount].v.f);
    }
    const float applied = orig(self, prng, value, source, type);
    if (Dispatchable(c.stressPost)) {
        Slot post[kMaxFields]{};
        fill();
        std::memcpy(post, slots, sizeof(Slot) * (STF_before + 1));
        post[STF_amount].v.f = value;
        post[STF_applied].v.f = applied;
        post[STF_applied].set = true;
        post[STF_after].v.f = ReadFloat(self, kActorStress);
        post[STF_after].set = true;
        Payload payload;
        payload.slots = post;
        payload.flags = DD_EVENT_F_POST;
        payload.originThread = GetCurrentThreadId();
        c.hub->Dispatch(c.stressPost, payload);
    }
    return applied;
}

float __fastcall HeroStressDetour(void* self, void* prng, float amount, void* source, int type)
{
    return StressImpl(g_ctx->orig.heroStress, self, prng, amount, source, type);
}

float __fastcall MonsterStressDetour(void* self, void* prng, float amount, void* source, int type)
{
    return StressImpl(g_ctx->orig.monsterStress, self, prng, amount, source, type);
}

void __fastcall EffectApplyDetour(void* prng, void* performer, void* target, const void* effect, void* item, float f1,
    bool b1, float f2, int source, unsigned u, void* result, bool b2, bool b3)
{
    auto& c = *g_ctx;
    if (!c.orig.effect) {
        return;
    }
    if (!IsMain() || !effect) {
        if (!IsMain()) {
            LogNonMainOnce("effect");
        }
        c.orig.effect(prng, performer, target, effect, item, f1, b1, f2, source, u, result, b2, b3);
        return;
    }
    // effect+0x3E0 非零时游戏交换执行者和目标（4C50DF），负载给出实际生效的双方，压力和 HP 也读实际目标
    const bool swapped = *(static_cast<const std::uint8_t*>(effect) + kEffectSwap) != 0;
    void* const actualPerformer = swapped ? target : performer;
    void* const actualTarget = swapped ? performer : target;
    // Effect+0 是 char[64] 名字（4F02D0），复制一份保证以 0 结尾
    char effectName[kEffectNameCap + 1] = {};
    std::memcpy(effectName, static_cast<const std::uint8_t*>(effect) + kEffectName, kEffectNameCap);
    Slot slots[kMaxFields]{};
    const auto fill = [&] {
        slots[EF_performer].v.p = actualPerformer;
        slots[EF_performer].set = true;
        slots[EF_target].v.p = actualTarget;
        slots[EF_target].set = true;
        slots[EF_effect].v.p = const_cast<void*>(effect);
        slots[EF_effect].set = true;
        slots[EF_item].v.p = item;
        slots[EF_item].set = true;
        slots[EF_source].v.i = source;
        slots[EF_source].set = true;
        // Effect 字段偏移来自 .darkest 解析函数（4F02D0）：.stress +0x4C、.heal +0xC8
        slots[EF_stressAmt].v.f = ReadFloat(effect, kEffectStress);
        slots[EF_stressAmt].set = true;
        slots[EF_healAmt].v.f = ReadFloat(effect, kEffectHeal);
        slots[EF_healAmt].set = true;
        slots[EF_stress].v.f = actualTarget ? ReadFloat(actualTarget, kActorStress) : 0.0f;
        slots[EF_stress].set = true;
        slots[EF_hp].v.f = actualTarget ? ReadFloat(actualTarget, kActorHp) : 0.0f;
        slots[EF_hp].set = true;
        slots[EF_name].v.s = effectName;
        slots[EF_name].set = true;
    };
    bool cancelled = false;
    if (PreAllowed(c.effectPre)) {
        fill();
        Payload payload;
        payload.slots = slots;
        payload.flags = DD_EVENT_F_PRE;
        payload.originThread = GetCurrentThreadId();
        payload.allowWrite = true;
        c.hub->Dispatch(c.effectPre, payload);
        cancelled = payload.cancelled;
    }
    if (!cancelled) {
        c.orig.effect(prng, performer, target, effect, item, f1, b1, f2, source, u, result, b2, b3);
    }
    if (Dispatchable(c.effectPost)) {
        Slot post[kMaxFields]{};
        if (!slots[EF_performer].set) {
            fill();
        }
        std::memcpy(post, slots, sizeof(Slot) * (EF_name + 1));
        post[EF_stressAfter].v.f = actualTarget ? ReadFloat(actualTarget, kActorStress) : 0.0f;
        post[EF_stressAfter].set = true;
        post[EF_hpAfter].v.f = actualTarget ? ReadFloat(actualTarget, kActorHp) : 0.0f;
        post[EF_hpAfter].set = true;
        post[EF_cancelled].v.b = cancelled;
        post[EF_cancelled].set = true;
        Payload payload;
        payload.slots = post;
        payload.flags = DD_EVENT_F_POST;
        payload.originThread = GetCurrentThreadId();
        c.hub->Dispatch(c.effectPost, payload);
    }
}

// 命中结算路由：hit.pre 可以改 hit、crit、damage
void DispatchHit(void* battle, void* attack, void* tgt, bool originalHit)
{
    auto& c = *g_ctx;
    VoidThis3 hitOrig = c.orig.applyHit;
    VoidThis3 missOrig = c.orig.applyMiss;
    if (!hitOrig || !missOrig) {
        (originalHit ? hitOrig : missOrig)(battle, attack, tgt);
        return;
    }
    if (!IsMain()) {
        LogNonMainOnce("hit");
        (originalHit ? hitOrig : missOrig)(battle, attack, tgt);
        return;
    }
    auto* td = static_cast<std::uint8_t*>(tgt);
    void* target = ReadPtr(tgt, kTargetActor);
    void* attacker = ReadPtr(attack, kAttackPerformer);
    void* skill = ReadPtr(attack, kAttackSkill);
    bool hit = originalHit;
    bool crit = td[kTargetCrit] != 0;
    float damage = ReadFloat(tgt, kTargetDamage);
    if (PreAllowed(c.hitPre)) {
        Slot slots[kMaxFields]{};
        slots[HF_attacker].v.p = attacker;
        slots[HF_attacker].set = true;
        slots[HF_target].v.p = target;
        slots[HF_target].set = true;
        slots[HF_skill].v.p = skill;
        slots[HF_skill].set = true;
        slots[HF_hit].v.b = hit;
        slots[HF_hit].set = true;
        slots[HF_crit].v.b = crit;
        slots[HF_crit].set = true;
        slots[HF_damage].v.f = damage;
        slots[HF_damage].set = true;
        Payload payload;
        payload.slots = slots;
        payload.flags = DD_EVENT_F_PRE;
        payload.originThread = GetCurrentThreadId();
        payload.allowWrite = true;
        c.hub->Dispatch(c.hitPre, payload);
        hit = slots[HF_hit].v.b;
        crit = slots[HF_crit].v.b;
        damage = static_cast<float>(slots[HF_damage].v.f);
        // 写回 TargetData：crit 和 damage 被 ApplyHit 读取，hit 决定走哪个函数
        td[kTargetCrit] = crit ? 1 : 0;
        *reinterpret_cast<float*>(td + kTargetDamage) = damage;
        td[kTargetHit] = hit ? 1 : 0;
    }
    (hit ? hitOrig : missOrig)(battle, attack, tgt);
    if (Dispatchable(c.hitPost)) {
        Slot post[kMaxFields]{};
        post[HF_attacker].v.p = attacker;
        post[HF_attacker].set = true;
        post[HF_target].v.p = target;
        post[HF_target].set = true;
        post[HF_skill].v.p = skill;
        post[HF_skill].set = true;
        post[HF_hit].v.b = hit;
        post[HF_hit].set = true;
        post[HF_crit].v.b = crit;
        post[HF_crit].set = true;
        post[HF_damage].v.f = damage;
        post[HF_damage].set = true;
        Payload payload;
        payload.slots = post;
        payload.flags = DD_EVENT_F_POST;
        payload.originThread = GetCurrentThreadId();
        c.hub->Dispatch(c.hitPost, payload);
    }
}

void __fastcall ApplyHitDetour(void* battle, void* attack, void* tgt)
{
    DispatchHit(battle, attack, tgt, true);
}

void __fastcall ApplyMissDetour(void* battle, void* attack, void* tgt)
{
    DispatchHit(battle, attack, tgt, false);
}

// std::vector<Actor*> 的 first（StdVectorFirst）/last（StdVectorLast）
void** VecFirst(void* vec)
{
    return static_cast<void**>(ReadPtr(vec, kStdVectorFirst));
}
void** VecLast(void* vec)
{
    return static_cast<void**>(ReadPtr(vec, kStdVectorLast));
}

void SkillImpl(void* battle, void* performer, const void* skill, void* targets, bool friendly, bool flag)
{
    auto& c = *g_ctx;
    AttackSkillFn attack = c.orig.attackSkill;
    FriendlySkillFn friendlyFn = c.orig.friendlySkill;
    const auto callOrig = [&](const void* useSkill) {
        if (friendly) {
            friendlyFn(battle, performer, useSkill, targets, flag);
        } else {
            attack(battle, performer, useSkill, targets);
        }
    };
    if (!IsMain()) {
        LogNonMainOnce("skill");
        callOrig(skill);
        return;
    }
    void** first = VecFirst(targets);
    void** last = VecLast(targets);
    const std::int64_t count = (first && last && last >= first) ? (last - first) : 0;
    void* primary = count > 0 ? first[0] : nullptr;
    const void* useSkill = skill;
    void* useTarget = primary;
    if (PreAllowed(c.skillPre)) {
        // 只读（P4，D11），字段都不可写，不写回
        Slot slots[kMaxFields]{};
        slots[SF_performer].v.p = performer;
        slots[SF_performer].set = true;
        slots[SF_skill].v.p = const_cast<void*>(skill);
        slots[SF_skill].set = true;
        slots[SF_target].v.p = primary;
        slots[SF_target].set = true;
        slots[SF_count].v.i = count;
        slots[SF_count].set = true;
        slots[SF_friendly].v.b = friendly;
        slots[SF_friendly].set = true;
        Payload payload;
        payload.slots = slots;
        payload.flags = DD_EVENT_F_PRE;
        payload.originThread = GetCurrentThreadId();
        payload.allowWrite = false;  // 主题只读，事件不带 DD_EVENT_F_WRITABLE（审核 A-N5 / R2A-N2）
        c.hub->Dispatch(c.skillPre, payload);
    }
    callOrig(useSkill);
    if (Dispatchable(c.skillPost)) {
        Slot post[kMaxFields]{};
        post[SF_performer].v.p = performer;
        post[SF_performer].set = true;
        post[SF_skill].v.p = const_cast<void*>(useSkill);
        post[SF_skill].set = true;
        post[SF_target].v.p = useTarget;
        post[SF_target].set = true;
        post[SF_count].v.i = count;
        post[SF_count].set = true;
        post[SF_friendly].v.b = friendly;
        post[SF_friendly].set = true;
        Payload payload;
        payload.slots = post;
        payload.flags = DD_EVENT_F_POST;
        payload.originThread = GetCurrentThreadId();
        c.hub->Dispatch(c.skillPost, payload);
    }
}

void __fastcall AttackSkillDetour(void* battle, void* performer, const void* skill, void* targets)
{
    SkillImpl(battle, performer, skill, targets, false, false);
}

void __fastcall FriendlySkillDetour(void* battle, void* performer, const void* skill, void* targets, bool flag)
{
    SkillImpl(battle, performer, skill, targets, true, flag);
}

void __fastcall AdvanceRoundDetour(void* battle)
{
    auto& c = *g_ctx;
    if (!IsMain() || !c.orig.advanceRound) {
        if (!IsMain()) {
            LogNonMainOnce("battle.round");
        }
        if (c.orig.advanceRound) {
            c.orig.advanceRound(battle);
        }
        return;
    }
    const int roundBefore = *reinterpret_cast<const int*>(static_cast<const std::uint8_t*>(battle) + kBattleRound);
    if (roundBefore > 0 && Dispatchable(c.roundEnd)) {
        Slot slots[kMaxFields]{};
        slots[RF_battle].v.p = battle;
        slots[RF_battle].set = true;
        slots[RF_round].v.i = roundBefore;
        slots[RF_round].set = true;
        Payload payload;
        payload.slots = slots;
        payload.originThread = GetCurrentThreadId();
        c.hub->Dispatch(c.roundEnd, payload);
    }
    c.orig.advanceRound(battle);
    if (Dispatchable(c.roundStart)) {
        const int roundAfter = *reinterpret_cast<const int*>(static_cast<const std::uint8_t*>(battle) + kBattleRound);
        Slot slots[kMaxFields]{};
        slots[RF_battle].v.p = battle;
        slots[RF_battle].set = true;
        slots[RF_round].v.i = roundAfter;
        slots[RF_round].set = true;
        Payload payload;
        payload.slots = slots;
        payload.originThread = GetCurrentThreadId();
        c.hub->Dispatch(c.roundStart, payload);
    }
}

int __fastcall StartTurnDetour(void* behaviors, void* prng, void* actor)
{
    auto& c = *g_ctx;
    if (!IsMain() || !c.orig.startTurn) {
        if (!IsMain()) {
            LogNonMainOnce("battle.turn");
        }
        return c.orig.startTurn ? c.orig.startTurn(behaviors, prng, actor) : 0;
    }
    if (Dispatchable(c.turnStart)) {
        Slot slots[kMaxFields]{};
        slots[TF_actor].v.p = actor;
        slots[TF_actor].set = true;
        Payload payload;
        payload.slots = slots;
        payload.originThread = GetCurrentThreadId();
        c.hub->Dispatch(c.turnStart, payload);
    }
    const int result = c.orig.startTurn(behaviors, prng, actor);
    if (Dispatchable(c.turnEnd)) {
        Slot slots[kMaxFields]{};
        slots[TF_actor].v.p = actor;
        slots[TF_actor].set = true;
        slots[TF_result].v.i = result;
        slots[TF_result].set = true;
        Payload payload;
        payload.slots = slots;
        payload.originThread = GetCurrentThreadId();
        c.hub->Dispatch(c.turnEnd, payload);
    }
    return result;
}

// ---- std::function 兼容对象 ----
void* __fastcall ImplCopy(FakeImpl* self, void*)
{
    return self;
}
void* __fastcall ImplMove(FakeImpl* self, void*)
{
    return self;
}
void __fastcall ImplDelete(FakeImpl*, bool) {}
const void* __fastcall ImplTarget(FakeImpl*)
{
    static const int kDummy = 0;
    return &kDummy;
}
const void* __fastcall ImplGet(FakeImpl* self)
{
    return self + 1;
}

}  // namespace

// ---- 系统实现 ----

struct System::Impl {
    SystemDeps deps;
    std::unique_ptr<Hub> hub;
    Context ctx;
    std::vector<std::unique_ptr<TypeState>> eventTypes;
    std::vector<std::unique_ptr<TypeState>> queryTypes;
    std::vector<HookDef> hooks;
    std::unordered_map<std::string, void*> eventVtables;
    std::unordered_map<std::string, void*> queryVtables;
    std::uint8_t* image = nullptr;
    std::size_t imageSize = 0;
    int prevLastState = -1;
    bool haveState = false;
    bool eventManagerReady = false;
    bool loading = false;
    bool deferredOps = false;
    Topic* appTick = nullptr;
    Topic* appState = nullptr;
    Topic* campaignLoad = nullptr;
    Topic* campaignUnload = nullptr;
    Topic* scopeBattleEnd = nullptr;
    Topic* scopeRaidEnd = nullptr;
    std::uint64_t battleTrackerSub = 0;
    std::uint64_t raidTrackerSub = 0;
    bool suppressNextTick = false;
    ChurnLog churn;  // hook、listener 装卸日志的限速（第三轮审核 R3A-备注3）

    void Log(int level, const std::string& message) { deps.log(level, message); }
    // 装卸一次的 INFO 日志：同一个名字每个窗口最多逐条写几行，其余合并成汇总（FrameBoundary 里写出）
    void LogChurn(const std::string& key, bool on, const std::string& message)
    {
        if (churn.Allow(key, on, GetTickCount64())) {
            Log(DD_LOG_INFO, message);
        }
    }

    // ---- RTTI ----
    std::uint8_t* RdataBegin = nullptr;
    std::uint8_t* RdataEnd = nullptr;
    std::uint8_t* DataBegin = nullptr;
    std::uint8_t* DataEnd = nullptr;
    void* baseListenerTd = nullptr;
    std::uint8_t* rttiPage = nullptr;
    std::size_t rttiUsed = 0;
    static constexpr std::size_t kRttiPageSize = 0x20000;

    void LocateSections()
    {
        auto* base = reinterpret_cast<std::uint8_t*>(deps.exe);
        image = base;
        const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        imageSize = nt->OptionalHeader.SizeOfImage;
        auto* section = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            char name[9] = {};
            std::memcpy(name, section->Name, 8);
            if (std::strcmp(name, ".rdata") == 0) {
                RdataBegin = base + section->VirtualAddress;
                RdataEnd = RdataBegin + section->Misc.VirtualSize;
            } else if (std::strcmp(name, ".data") == 0) {
                DataBegin = base + section->VirtualAddress;
                DataEnd = DataBegin + section->Misc.VirtualSize;
            }
        }
    }

    // 运行时按 RTTI 名查 Listener<T const>/Listener<T> 的虚表：在 .rdata 里找 vtable[-1] 指向的 COL
    void FindListenerVtables()
    {
        if (!RdataBegin || !DataBegin) {
            return;
        }
        static constexpr const char kEventPrefix[] = ".?AV?$Listener@$$CBV";
        static constexpr const char kQueryPrefix[] = ".?AV?$Listener@V";
        const auto rdBegin = reinterpret_cast<std::uintptr_t>(RdataBegin);
        const auto rdEnd = reinterpret_cast<std::uintptr_t>(RdataEnd);
        for (auto* p = reinterpret_cast<std::uint64_t*>(RdataBegin);
             reinterpret_cast<std::uint8_t*>(p + 1) <= RdataEnd; ++p) {
            const std::uintptr_t q = *p;
            if (q < rdBegin || q >= rdEnd || rdEnd - q < kRttiObjectLocatorSize) {
                continue;
            }
            const auto* col = reinterpret_cast<const std::uint32_t*>(q);
            if (col[0] != 1 || col[1] != 0) {
                continue;
            }
            if (static_cast<std::uintptr_t>(col[5]) != q - reinterpret_cast<std::uintptr_t>(image)) {
                continue;
            }
            const std::uintptr_t tdRva = col[3];
            if (tdRva < 0x1000 || tdRva + 0x20 >= imageSize) {
                continue;
            }
            const char* name = reinterpret_cast<const char*>(image + tdRva + kRttiTypeDescriptorName);
            const std::size_t prefixLen = (std::strncmp(name, kEventPrefix, sizeof(kEventPrefix) - 1) == 0)
                ? sizeof(kEventPrefix) - 1
                : (std::strncmp(name, kQueryPrefix, sizeof(kQueryPrefix) - 1) == 0 ? sizeof(kQueryPrefix) - 1 : 0);
            if (!prefixLen) {
                continue;
            }
            const char* inner = name + prefixLen;
            const char* at = std::strchr(inner, '@');
            if (!at) {
                continue;
            }
            std::string key(inner, static_cast<std::size_t>(at - inner));
            auto& map = prefixLen == sizeof(kEventPrefix) - 1 ? eventVtables : queryVtables;
            map.emplace(std::move(key), reinterpret_cast<void*>(p + 1));
        }
    }

    // 在 .data 里建类型描述符索引：TD 的 name 在 +0x10，内容是 ".?AV..."，只索引 Listener 和 BaseListener
    std::unordered_map<std::string, const std::uint8_t*> tdIndex;
    bool tdIndexBuilt = false;
    void BuildTdIndex()
    {
        if (tdIndexBuilt || !DataBegin) {
            return;
        }
        tdIndexBuilt = true;
        static constexpr char kPrefix[] = ".?AV";
        // 类型描述符的修饰名在描述符 +kRttiTypeDescriptorName（layout.inc）
        auto* p = DataBegin + kRttiTypeDescriptorName;
        while (p + 4 < DataEnd) {
            auto* hit = static_cast<std::uint8_t*>(std::memchr(p, '.', static_cast<std::size_t>(DataEnd - p - 4)));
            if (!hit) {
                break;
            }
            if (std::memcmp(hit, kPrefix, 4) == 0 && hit - DataBegin >= static_cast<std::ptrdiff_t>(kRttiTypeDescriptorName)) {
                const char* name = reinterpret_cast<const char*>(hit);
                std::size_t length = strnlen(name, static_cast<std::size_t>(DataEnd - hit));
                if (length > 4 && length < 512) {
                    std::string text(name, length);
                    if (text.rfind(".?AV?$Listener@", 0) == 0 || text == ".?AVBaseListener@@") {
                        tdIndex.emplace(std::move(text), hit - kRttiTypeDescriptorName);
                    }
                }
                p = hit + (length ? length : 1);
            } else {
                p = hit + 1;
            }
        }
    }

    const std::uint8_t* FindTypeDescriptor(const std::string& decorated)
    {
        BuildTdIndex();
        const auto it = tdIndex.find(decorated);
        return it == tdIndex.end() ? nullptr : it->second;
    }

    // 伪造 RTTI：所有偏移都是相对映像基址的 RVA，页必须在映像之上 2GB 内，
    // __RTDynamicCast 用 (COL - pSelf) 算映像基址。依据见 knowledge/dd1/events.md。
    std::uint8_t* AllocRttiPage()
    {
        if (rttiPage) {
            return rttiPage;
        }
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const std::uintptr_t granularity = si.dwAllocationGranularity;
        const auto start = reinterpret_cast<std::uintptr_t>(image) + imageSize;
        for (std::uintptr_t p = (start + granularity - 1) & ~(granularity - 1);
             p < reinterpret_cast<std::uintptr_t>(image) + 0x7F000000; p += granularity) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) && mbi.State == MEM_FREE &&
                mbi.RegionSize >= kRttiPageSize) {
                auto* block = static_cast<std::uint8_t*>(
                    VirtualAlloc(reinterpret_cast<void*>(p), kRttiPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (block) {
                    rttiPage = block;
                    return rttiPage;
                }
            }
        }
        return nullptr;
    }

    std::uint8_t* RttiAlloc(std::size_t size)
    {
        size = (size + 7) & ~std::size_t(7);
        if (!AllocRttiPage() || rttiUsed + size > kRttiPageSize) {
            return nullptr;
        }
        auto* p = rttiPage + rttiUsed;
        rttiUsed += size;
        std::memset(p, 0, size);
        return p;
    }

    std::int32_t Rva(const void* p) const
    {
        return static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(p) - reinterpret_cast<std::uintptr_t>(image));
    }

    // 没有虚表但有类型描述符的类型：造 COL、CHD、BCD 和虚表。失败时返回空，说明不可覆盖
    void* ForgeListenerVtable(const std::string& shortName, ListKind kind, std::string* why)
    {
        const std::string decorated = kind == ListKind::Event ? ".?AV?$Listener@$$CBV" + shortName + "@@@@"
                                                              : ".?AV?$Listener@V" + shortName + "@@@@";
        const auto* td = FindTypeDescriptor(decorated);
        if (!td) {
            *why = "游戏没有对应的 Listener 类型描述符（类型没有派发点，无法用 __RTDynamicCast 伪造）";
            return nullptr;
        }
        if (!baseListenerTd) {
            baseListenerTd = const_cast<std::uint8_t*>(FindTypeDescriptor(".?AVBaseListener@@"));
        }
        if (!baseListenerTd) {
            *why = "未找到 BaseListener 的类型描述符";
            return nullptr;
        }
        // 结构大小按 MSVC x64 RTTI（layout.inc 的 Rtti*Size）；下面按 int32 下标填的字段和它们一致
        static_assert(kRttiBaseClassDescriptorSize == 7 * sizeof(std::int32_t) && kRttiClassHierarchySize == 4 * sizeof(std::int32_t) &&
            kRttiObjectLocatorSize == 6 * sizeof(std::int32_t), "RTTI 结构的大小和下面填的字段不一致");
        // 虚表：COL 指针 + 填的槽（ForgedListenerVtableSlots）+ 结尾留一个空位
        static_assert(kForgedListenerVtableSlots == 1, "伪造虚表只填槽 0，改槽数时同步改下面的填写");
        auto* bcd0 = RttiAlloc(kRttiBaseClassDescriptorSize);
        auto* bcd1 = RttiAlloc(kRttiBaseClassDescriptorSize);
        auto* bases = RttiAlloc(2 * sizeof(std::int32_t));
        auto* chd = RttiAlloc(kRttiClassHierarchySize);
        auto* col = RttiAlloc(kRttiObjectLocatorSize);
        auto* vtable = RttiAlloc((1 + kForgedListenerVtableSlots + 1) * sizeof(void*));
        if (!bcd0 || !bcd1 || !bases || !chd || !col || !vtable) {
            *why = "RTTI 伪造页分配失败";
            return nullptr;
        }
        const auto writeBcd = [&](std::uint8_t* bcd, const void* tdPtr, std::uint32_t contained) {
            auto* i32 = reinterpret_cast<std::int32_t*>(bcd);
            i32[0] = Rva(tdPtr);       // pTypeDescriptor
            i32[1] = static_cast<std::int32_t>(contained);
            i32[2] = 0;                // mdisp
            i32[3] = -1;               // pdisp < 0 表示没有虚基类
            i32[4] = 0;                // vdisp
            i32[5] = 0;                // attributes
            i32[6] = Rva(chd);
        };
        writeBcd(bcd0, td, 1);
        writeBcd(bcd1, baseListenerTd, 0);
        auto* baseArr = reinterpret_cast<std::int32_t*>(bases);
        baseArr[0] = Rva(bcd0);
        baseArr[1] = Rva(bcd1);
        auto* chdFields = reinterpret_cast<std::int32_t*>(chd);
        chdFields[0] = 0;       // signature
        chdFields[1] = 0;       // attributes：单继承
        chdFields[2] = 2;       // numBaseClasses
        chdFields[3] = Rva(bases);
        auto* colFields = reinterpret_cast<std::int32_t*>(col);
        colFields[0] = 1;       // signature
        colFields[1] = 0;       // offset
        colFields[2] = 0;       // cdOffset
        colFields[3] = Rva(td);
        colFields[4] = Rva(chd);
        colFields[5] = Rva(col);  // pSelf
        auto** vt = reinterpret_cast<void**>(vtable);
        // 槽位 0 借一个现成 Listener 的析构，游戏不销毁我们的 listener，但要有合法值
        void* donor = nullptr;
        if (!eventVtables.empty()) {
            donor = *reinterpret_cast<void**>(eventVtables.begin()->second);
        } else if (!queryVtables.empty()) {
            donor = *reinterpret_cast<void**>(queryVtables.begin()->second);
        }
        vt[0] = col;
        vt[1] = donor;
        return reinterpret_cast<void*>(vt + 1);
    }

    // ---- 主题注册 ----
    // 语义 hook 主题：背后都是内联 hook 或虚表槽（BuildHookDefs），按 ADR 0036 算「写」
    Topic* AddSemantic(const char* name, std::vector<FieldDesc> fields, bool writable, bool cancellable)
    {
        Topic* topic = hub->AddTopic(name, TopicKind::Semantic, std::move(fields), writable, cancellable);
        topic->hooked = true;
        return topic;
    }

    void RegisterSemanticTopics()
    {
        auto& c = ctx;
        c.damagePre = AddSemantic("damage.pre", DamageFields(false), true, true);
        c.damagePost = AddSemantic("damage.post", DamageFields(true), false, false);
        c.effectPre = AddSemantic("effect.pre", EffectFields(false), true, true);
        c.effectPost = AddSemantic("effect.post", EffectFields(true), false, false);
        c.skillPre = AddSemantic("skill.pre", SkillFields(false), false, false);  // 只读（D11），不带可写标志
        c.skillPost = AddSemantic("skill.post", SkillFields(true), false, false);
        c.hitPre = AddSemantic("hit.pre", HitFields(false), true, false);
        c.hitPost = AddSemantic("hit.post", HitFields(true), false, false);
        c.roundStart = AddSemantic("battle.round.start", RoundFields(), false, false);
        c.roundEnd = AddSemantic("battle.round.end", RoundFields(), false, false);
        c.turnStart = AddSemantic("battle.turn.start", TurnFields(false), false, false);
        c.turnEnd = AddSemantic("battle.turn.end", TurnFields(true), false, false);
        c.stressPre = AddSemantic("stress.pre", StressFields(false), true, false);
        c.stressPost = AddSemantic("stress.post", StressFields(true), false, false);
        c.healPre = AddSemantic("heal.pre", HealFields(false), true, false);
        c.healPost = AddSemantic("heal.post", HealFields(true), false, false);
        c.damageFinal = AddSemantic("damage.final", DamageFinalFields(), true, false);

        // 应用与生命周期
        appTick = hub->AddTopic("app.tick", TopicKind::App, { FieldDesc{ "frame", DD_FIELD_INT, kAccessR } }, false, false);
        appState = hub->AddTopic("app.state", TopicKind::App,
            { FieldDesc{ "from", DD_FIELD_INT, kAccessR }, FieldDesc{ "to", DD_FIELD_INT, kAccessR },
                FieldDesc{ "from_name", DD_FIELD_STRING, kAccessR }, FieldDesc{ "to_name", DD_FIELD_STRING, kAccessR } },
            false, false);
        campaignLoad = hub->AddTopic("campaign.load", TopicKind::App,
            { FieldDesc{ "serial", DD_FIELD_INT, kAccessR }, FieldDesc{ "campaign", DD_FIELD_POINTER, kAccessR } },
            false, false);
        campaignUnload = hub->AddTopic("campaign.unload", TopicKind::App, { FieldDesc{ "serial", DD_FIELD_INT, kAccessR } },
            false, false);
        // 应用主题由 DDCore 的每帧 hook（UpdateStateInternal）派发
        for (Topic* t : { appTick, appState, campaignLoad, campaignUnload }) {
            t->hooked = true;
        }

        // 按需检查所依赖的地址
        const auto setMissing = [&](Topic* topic, const std::string& reason) {
            hub->SetAvailability(topic, false, reason);
        };
        auto* addr = deps.addr;
        const bool vtables = addr->heroVtable && addr->monsterVtable;
        if (!vtables) {
            for (Topic* t : { c.damagePre, c.damagePost, c.stressPre, c.stressPost, c.healPre, c.healPost }) {
                setMissing(t, "特征码 HeroVtable/MonsterVtable 缺失");
            }
        }
        const auto needInline = [&](Topic* a, Topic* b, std::uint8_t* target, const char* sig) {
            if (!target) {
                setMissing(a, std::string("特征码 ") + sig + " 缺失");
                setMissing(b, std::string("特征码 ") + sig + " 缺失");
            }
        };
        if (!vtables || !addr->consumeBlock) {
            setMissing(c.damageFinal, !vtables ? "特征码 HeroVtable/MonsterVtable 缺失" : "特征码 ConsumeHealthDamageBlock 缺失");
        }
        needInline(c.effectPre, c.effectPost, addr->effectApply, "EffectApplyInternal");
        needInline(c.turnStart, c.turnEnd, addr->combatStartTurn, "CombatStartTurn");
        needInline(c.roundStart, c.roundEnd, addr->advanceRound, "BattleAdvanceRound");
        if (!addr->doAttackSkill || !addr->doSkillFriendly) {
            needInline(c.skillPre, c.skillPost, nullptr, "ActorDoAttackSkill/ActorDoSkillFriendly");
        }
        if (!addr->applyHit || !addr->applyMiss) {
            needInline(c.hitPre, c.hitPost, nullptr, "ApplyHit/ApplyMiss");
        }
        if (!addr->raidCurrent) {
            // PvP 判定不可用时，.pre 主题一律不可用，防止不同步
            for (Topic* t : { c.damagePre, c.effectPre, c.skillPre, c.hitPre, c.stressPre, c.healPre, c.damageFinal }) {
                if (t && t->available) {
                    setMissing(t, "特征码 RaidCurrent 缺失，无法判定 PvP");
                }
            }
        }
        // 布局核对停用了 pvp（ADR 0036）：同上，另外 Query 写回（RegisterQueryTopics）和 combat 写操作（core.cpp）也停
        if (addr->gate.Off(game::check::Dep::Pvp)) {
            for (Topic* t : { c.damagePre, c.effectPre, c.skillPre, c.hitPre, c.stressPre, c.healPre, c.damageFinal }) {
                hub->Gate(t, "PvP 判定停用（" + addr->gate.Reason(game::check::Dep::Pvp) + "）");
            }
        }
    }

    TypeState* FindNative(const char* name)
    {
        for (const auto& type : eventTypes) {
            if (type->name == name) {
                return type.get();
            }
        }
        return nullptr;
    }

    void RegisterNativeTopics()
    {
        auto* addr = deps.addr;
        const int count = *addr->eventTypeCount;
        for (int i = 0; i < count; ++i) {
            const char* raw = addr->eventClassNames + static_cast<std::size_t>(i) * kClassNameStride;
            if (!raw[0]) {
                continue;
            }
            auto type = std::make_unique<TypeState>();
            type->kind = ListKind::Event;
            type->id = i;
            type->name = raw;
            eventTypes.push_back(std::move(type));
        }
        // 语义映射：字段并集
        struct MergedTopic {
            std::vector<FieldDesc> fields;
            std::vector<const NativeSource*> sources;
        };
        std::unordered_map<std::string, MergedTopic> merged;
        std::vector<std::string> mergedOrder;
        for (const auto& source : NativeSources()) {
            auto& m = merged[source.topic];
            if (m.sources.empty()) {
                mergedOrder.push_back(source.topic);
            }
            m.sources.push_back(&source);
            for (const auto& map : source.fields) {
                const auto found = std::find_if(m.fields.begin(), m.fields.end(),
                    [&](const FieldDesc& f) { return f.name == map.field; });
                if (found == m.fields.end()) {
                    m.fields.push_back(FieldDesc{ map.field, ToType(map.raw), kAccessR, RangeOf(map.raw) });
                }
            }
        }
        for (const auto& topicName : mergedOrder) {
            auto& m = merged[topicName];
            Topic* topic = hub->AddTopic(topicName, TopicKind::Semantic, m.fields, false, false);
            for (const auto* source : m.sources) {
                TypeState* type = FindNative(source->event);
                if (!type) {
                    continue;
                }
                TypeState::Target target;
                target.topic = topic;
                target.fields = source->fields;
                for (const auto& map : source->fields) {
                    std::uint32_t index = 0;
                    Hub::FindField(topic, map.field, &index);
                    target.slotIndices.push_back(index);
                }
                type->targets.push_back(std::move(target));
            }
        }

        // native.<类名>：公共字段 name/time + 映射字段
        for (auto& type : eventTypes) {
            std::vector<FieldDesc> fields = { FieldDesc{ "name", DD_FIELD_STRING, kAccessR },
                FieldDesc{ "time", DD_FIELD_INT, kAccessR } };
            std::vector<FieldMap> extras;
            for (const auto& target : type->targets) {
                for (const auto& map : target.fields) {
                    if (std::find_if(fields.begin(), fields.end(), [&](const FieldDesc& f) {
                            return f.name == map.field;
                        }) == fields.end()) {
                        fields.push_back(FieldDesc{ map.field, ToType(map.raw), kAccessR, RangeOf(map.raw) });
                        extras.push_back(map);
                    }
                }
            }
            type->topic = hub->AddTopic("native." + type->name, TopicKind::Native, fields, false, false);
            type->topic->bridgeData = type.get();
            type->impl.vtable = reinterpret_cast<void* const*>(kImplVtable());
            type->impl.type = type.get();
            type->listener.priority = INT_MIN;
            type->listener.funcImpl = &type->impl;
            type->queryFields.clear();
            for (const auto& map : extras) {
                type->queryFields.push_back({ map, false });
            }
            for (std::uint32_t i = 2; i < fields.size(); ++i) {
                type->querySlotIndices.push_back(i);
            }
            // 虚表：真实的，或者伪造的
            std::string key = type->name;
            const auto colon = key.rfind("::");
            if (colon != std::string::npos) {
                key = key.substr(colon + 2);
            }
            auto it = eventVtables.find(key);
            std::string why;
            if (it != eventVtables.end()) {
                type->listenerVtable = it->second;
            } else {
                void* forged = ForgeListenerVtable(key, ListKind::Event, &why);
                if (forged) {
                    type->listenerVtable = forged;
                    type->forgedVtable = true;
                } else {
                    hub->SetAvailability(type->topic, false, "没有 Listener<T const> 虚表：" + why);
                    for (const auto& target : type->targets) {
                        hub->SetAvailability(target.topic, false,
                            "依赖的原生事件 " + type->name + " 没有 Listener 虚表");
                    }
                }
            }
        }
        // 语义映射主题中，来源事件全都不可用的主题置为不可用（FindNative 为空的已被跳过）
        for (const auto& topicName : mergedOrder) {
            auto& m = merged[topicName];
            bool anyAvailable = false;
            for (const auto* source : m.sources) {
                TypeState* type = FindNative(source->event);
                if (type && type->listenerVtable) {
                    anyAvailable = true;
                }
            }
            if (!anyAvailable) {
                Topic* topic = hub->Find(topicName);
                hub->SetAvailability(topic, false, "来源原生事件不存在或没有 Listener 虚表");
            }
        }
        // 布局核对停用了原生事件桥（ADR 0036）：native.* 和映射来的语义主题都不可用，不往游戏的 listener 列表里注册
        if (deps.addr->gate.Off(game::check::Dep::EventsNative)) {
            const std::string reason = "原生事件桥停用（" + deps.addr->gate.Reason(game::check::Dep::EventsNative) + "）";
            for (auto& type : eventTypes) {
                hub->Gate(type->topic, reason);
            }
            for (const auto& topicName : mergedOrder) {
                hub->Gate(hub->Find(topicName), reason);
            }
        }
    }

    void RegisterQueryTopics()
    {
        auto* addr = deps.addr;
        const bool haveQuery = addr->queryDispatcher && addr->queryClassNames && addr->queryListenerAdd;
        if (!haveQuery) {
            for (const auto& def : QueryDefs()) {
                Topic* t = hub->AddTopic(std::string("query.") + def.name, TopicKind::Query, {}, true, false);
                hub->SetAvailability(t, false, "特征码 QueryDispatcher/QueryClassNames/QueryListenerAdd 缺失");
            }
            return;
        }
        for (int i = 0; i < addr->queryTypeCount; ++i) {
            const char* raw = addr->queryClassNames + static_cast<std::size_t>(i) * kClassNameStride;
            auto type = std::make_unique<TypeState>();
            type->kind = ListKind::Query;
            type->id = i;
            type->name = raw;
            std::vector<FieldDesc> fields = { FieldDesc{ "name", DD_FIELD_STRING, kAccessR } };
            const auto def = std::find_if(QueryDefs().begin(), QueryDefs().end(),
                [&](const QueryDef& d) { return type->name == d.name; });
            if (def != QueryDefs().end()) {
                for (const auto& field : def->fields) {
                    fields.push_back(FieldDesc{ field.map.field, ToType(field.map.raw), field.writable ? kAccessRW : kAccessR,
                        RangeOf(field.map.raw) });
                    type->queryFields.push_back(field);
                    type->querySlotIndices.push_back(static_cast<std::uint32_t>(fields.size() - 1));
                }
            }
            type->topic = hub->AddTopic("query." + type->name, TopicKind::Query, fields, true, false);
            type->topic->bridgeData = type.get();
            type->impl.vtable = reinterpret_cast<void* const*>(kImplVtable());
            type->impl.type = type.get();
            // Query 让回调看到游戏累加之后的结果，排在最后
            type->listener.priority = INT_MAX;
            type->listener.funcImpl = &type->impl;
            auto it = queryVtables.find(type->name);
            if (it != queryVtables.end()) {
                type->listenerVtable = it->second;
            } else {
                std::string why;
                type->listenerVtable = ForgeListenerVtable(type->name, ListKind::Query, &why);
                if (type->listenerVtable) {
                    type->forgedVtable = true;
                } else {
                    hub->SetAvailability(type->topic, false, "没有 Listener<T> 虚表：" + why);
                }
            }
            queryTypes.push_back(std::move(type));
        }
        // 布局核对停用了 Query 桥或 PvP 判定（ADR 0036）：Query 的回调能写回结果，PvP 判定不可用时也不派发
        const auto& gate = addr->gate;
        const bool queryOff = gate.Off(game::check::Dep::EventsQuery);
        if (queryOff || gate.Off(game::check::Dep::Pvp)) {
            const std::string reason = queryOff ? "Query 桥停用（" + gate.Reason(game::check::Dep::EventsQuery) + "）"
                                                : "PvP 判定停用，Query 写回不可用（" + gate.Reason(game::check::Dep::Pvp) + "）";
            for (auto& type : queryTypes) {
                hub->Gate(type->topic, reason);
            }
        }
    }

    static void* const* kImplVtable();

    // ---- listener 注册 ----
    ListData* ListOf(TypeState& type)
    {
        void** root = type.kind == ListKind::Event ? deps.addr->eventManager : deps.addr->queryDispatcher;
        if (!root || !*root) {
            return nullptr;
        }
        auto** first = static_cast<ListData**>(*root);
        return *first + type.id;
    }

    void AddListener(TypeState& type)
    {
        auto* list = ListOf(type);
        if (!list) {
            return;
        }
        if (type.kind == ListKind::Event) {
            reinterpret_cast<ListenerAddFn>(deps.addr->listenerAdd)(list, &type.listener);
        } else {
            reinterpret_cast<ListenerAddFn>(deps.addr->queryListenerAdd)(list, &type.listener);
        }
    }

    // 注销：active 清零并置 dirty（游戏析构的做法）
    void DeactivateListener(TypeState& type)
    {
        auto* list = ListOf(type);
        if (!list) {
            return;
        }
        const std::size_t stride = type.kind == ListKind::Event ? kListenerEntrySize : kQueryListenerEntrySize;
        for (auto* e = list->first; e != list->last; e += stride) {
            if (*reinterpret_cast<void**>(e + kListenerEntryListener) == &type.listener) {
                e[kListenerEntryActive] = 0;
                list->dirty = 1;
            }
        }
    }

    std::size_t TotalTopicSubs(TypeState& type)
    {
        std::size_t total = hub->SubscriberCount(type.topic);
        for (const auto& target : type.targets) {
            total += hub->SubscriberCount(target.topic);
        }
        return total;
    }

    void ReconcileListeners()
    {
        if (!eventManagerReady) {
            return;
        }
        const auto reconcileSet = [&](std::vector<std::unique_ptr<TypeState>>& types) {
            for (auto& typePtr : types) {
                TypeState& type = *typePtr;
                if (!type.listenerVtable) {
                    continue;
                }
                const bool desired = TotalTopicSubs(type) > 0;
                if (desired && !type.registered) {
                    if (loading) {
                        deferredOps = true;
                        continue;
                    }
                    type.listener.vtable = type.listenerVtable;
                    AddListener(type);
                    type.registered = true;
                    if (!type.orderLogged && !type.forgedVtable) {
                        LogListOrder(type, "注册后");
                    }
                    LogChurn("listener " + type.topic->name, true, "listener 已注册：" + type.topic->name + "（type " +
                        std::to_string(type.id) + (type.forgedVtable ? "，伪造虚表" : "") + "）");
                } else if (!desired && type.registered) {
                    if (loading) {
                        deferredOps = true;
                        continue;
                    }
                    DeactivateListener(type);
                    type.registered = false;
                    LogChurn("listener " + type.topic->name, false, "listener 已注销：" + type.topic->name);
                }
            }
        };
        reconcileSet(eventTypes);
        reconcileSet(queryTypes);
    }

    // 排序确认：遍历列表，看通用 listener 排在第几个活跃表项
    void LogListOrder(TypeState& type, const char* when)
    {
        auto* list = ListOf(type);
        if (!list) {
            return;
        }
        const std::size_t stride = type.kind == ListKind::Event ? kListenerEntrySize : kQueryListenerEntrySize;
        const std::size_t total = static_cast<std::size_t>(list->last - list->first) / stride;
        int active = 0;
        int ourActiveIndex = -1;
        int minPriority = INT_MAX;
        int minCount = 0;
        for (std::size_t i = 0; i < total; ++i) {
            auto* e = list->first + i * stride;
            auto* listener = *reinterpret_cast<std::uint8_t**>(e + kListenerEntryListener);
            if (!e[kListenerEntryActive]) {
                continue;
            }
            const int priority = SafeReadInt(listener + kListenerPriority);
            if (priority < minPriority) {
                minPriority = priority;
                minCount = 1;
            } else if (priority == minPriority) {
                ++minCount;
            }
            if (reinterpret_cast<void*>(listener) == &type.listener) {
                ourActiveIndex = active;
            }
            ++active;
        }
        const bool sorted = !list->dirty;
        const bool first = ourActiveIndex == 0 || (minPriority == type.listener.priority && minCount == 1);
        // 只有列表里确实有别的表项，才能说明通用 listener 排在游戏回调之前
        const bool comparable = active >= 2;
        const bool misplaced = comparable && (sorted ? ourActiveIndex != 0 : !first);
        char text[320];
        if (type.kind == ListKind::Query) {
            std::snprintf(text, sizeof(text),
                "排序确认（%s）：%s 活跃表项 %d 个，通用 listener 排第 %d（priority=%d，dirty=%d）",
                when, type.topic->name.c_str(), active, ourActiveIndex, type.listener.priority, list->dirty ? 1 : 0);
            Log(DD_LOG_INFO, text);
        } else {
            std::snprintf(text, sizeof(text),
                "排序确认（%s）：%s 活跃表项 %d 个，通用 listener 排第 %d（从 0 起，priority=%d），dirty=%d，%s",
                when, type.topic->name.c_str(), active, ourActiveIndex, type.listener.priority, list->dirty ? 1 : 0,
                !comparable ? "列表里没有游戏回调，无法判定先后"
                : sorted    ? (ourActiveIndex == 0 ? "已确认排在最前，排在所有游戏回调之前"
                                                   : "未排在最前！排序键最小但出现了别的排序结果")
                            : (first ? "排序键最小且唯一，等待下次派发排序" : "排序键不是唯一最小"));
            Log(misplaced ? DD_LOG_WARN : DD_LOG_INFO, text);
        }
        if (sorted && ourActiveIndex >= 0) {
            type.orderLogged = true;
        }
    }

    // 汇总：所有已注册的 listener，同列表里有没有游戏回调。已排序的列表看实际位置，
    // 待排序的列表只能看排序键是否严格小于全部游戏回调
    void OrderSummary(const char* when)
    {
        int listenersWithGame = 0;
        int confirmedFirst = 0;
        int pendingSort = 0;
        int violations = 0;
        std::string violationNames;
        for (auto& typePtr : eventTypes) {
            TypeState& type = *typePtr;
            if (!type.registered) {
                continue;
            }
            auto* list = ListOf(type);
            if (!list) {
                continue;
            }
            int others = 0;
            int lowest = INT_MAX;
            int active = 0;
            int ourIndex = -1;
            for (auto* e = list->first; e != list->last; e += kListenerEntrySize) {
                auto* listener = *reinterpret_cast<std::uint8_t**>(e + kListenerEntryListener);
                if (!e[kListenerEntryActive]) {
                    continue;
                }
                if (listener == reinterpret_cast<std::uint8_t*>(&type.listener)) {
                    ourIndex = active;
                } else {
                    ++others;
                    lowest = (std::min)(lowest, SafeReadInt(listener + kListenerPriority));
                }
                ++active;
            }
            if (others == 0) {
                continue;
            }
            ++listenersWithGame;
            if (type.listener.priority >= lowest || (!list->dirty && ourIndex != 0)) {
                ++violations;
                violationNames += " " + type.name + "(min=" + std::to_string(lowest) + ",index=" + std::to_string(ourIndex) + ")";
            } else if (list->dirty) {
                ++pendingSort;
            } else {
                ++confirmedFirst;
            }
        }
        char text[320];
        std::snprintf(text, sizeof(text),
            "排序汇总（%s）：通用 listener %zu 个，其中 %d 个的列表里有游戏回调：已确认排最前 %d 个，等待下次派发排序 %d 个，冲突 %d 个%s",
            when, CountRegistered(), listenersWithGame, confirmedFirst, pendingSort, violations, violations ? "：" : "");
        Log(violations ? DD_LOG_WARN : DD_LOG_INFO, std::string(text) + violationNames);
    }

    std::size_t CountRegistered() const
    {
        std::size_t count = 0;
        for (const auto& type : eventTypes) {
            count += type->registered ? 1 : 0;
        }
        for (const auto& type : queryTypes) {
            count += type->registered ? 1 : 0;
        }
        return count;
    }

    void HandleEventCallback(TypeState& type, const std::uint8_t* event);
    void HandleQueryCallback(TypeState& type, std::uint8_t* query);

    // ---- 语义 hook ----
    void BuildHookDefs()
    {
        auto& c = ctx;
        auto* addr = deps.addr;
        const auto addVtable = [&](const char* name, void* const* vtable, std::size_t slot, void* detour, void** orig,
                                   std::vector<Topic*> topics) {
            if (!vtable) {
                return;
            }
            HookDef def;
            def.name = name;
            def.isVtable = true;
            def.target = const_cast<void**>(vtable + slot);
            def.detour = detour;
            def.orig = orig;
            def.topics = std::move(topics);
            hooks.push_back(std::move(def));
        };
        const auto addInline = [&](const char* name, std::uint8_t* target, const char* entryId, void* detour,
                                   void** orig, std::vector<Topic*> topics) {
            if (!target) {
                return;
            }
            HookDef def;
            def.name = name;
            def.isVtable = false;
            def.target = target;
            def.entryId = entryId;
            def.detour = detour;
            def.orig = orig;
            def.topics = std::move(topics);
            hooks.push_back(std::move(def));
        };
        // damage.final 也要槽位 50：它在那里记下攻击者（480400 的参数里没有）
        std::vector<Topic*> damage = { c.damagePre, c.damagePost, c.damageFinal };
        std::vector<Topic*> heal = { c.healPre, c.healPost };
        std::vector<Topic*> stress = { c.stressPre, c.stressPost };
        addVtable("damage:Hero", addr->heroVtable, kVtSlotDamage, reinterpret_cast<void*>(&HeroDamageDetour),
            reinterpret_cast<void**>(&c.orig.heroDamage), damage);
        addVtable("damage:Monster", addr->monsterVtable, kVtSlotDamage, reinterpret_cast<void*>(&MonsterDamageDetour),
            reinterpret_cast<void**>(&c.orig.monsterDamage), damage);
        addVtable("heal:Hero", addr->heroVtable, kVtSlotHeal, reinterpret_cast<void*>(&HeroHealDetour),
            reinterpret_cast<void**>(&c.orig.heroHeal), heal);
        addVtable("heal:Monster", addr->monsterVtable, kVtSlotHeal, reinterpret_cast<void*>(&MonsterHealDetour),
            reinterpret_cast<void**>(&c.orig.monsterHeal), heal);
        addVtable("stress:Hero", addr->heroVtable, kVtSlotStress, reinterpret_cast<void*>(&HeroStressDetour),
            reinterpret_cast<void**>(&c.orig.heroStress), stress);
        addVtable("stress:Monster", addr->monsterVtable, kVtSlotStress, reinterpret_cast<void*>(&MonsterStressDetour),
            reinterpret_cast<void**>(&c.orig.monsterStress), stress);

        addInline("effect", addr->effectApply, "EffectApplyInternal", reinterpret_cast<void*>(&EffectApplyDetour),
            reinterpret_cast<void**>(&c.orig.effect), { c.effectPre, c.effectPost });
        addInline("skill:attack", addr->doAttackSkill, "ActorDoAttackSkill", reinterpret_cast<void*>(&AttackSkillDetour),
            reinterpret_cast<void**>(&c.orig.attackSkill), { c.skillPre, c.skillPost });
        addInline("skill:friendly", addr->doSkillFriendly, "ActorDoSkillFriendly",
            reinterpret_cast<void*>(&FriendlySkillDetour), reinterpret_cast<void**>(&c.orig.friendlySkill),
            { c.skillPre, c.skillPost });
        addInline("hit", addr->applyHit, "ApplyHit", reinterpret_cast<void*>(&ApplyHitDetour),
            reinterpret_cast<void**>(&c.orig.applyHit), { c.hitPre, c.hitPost });
        addInline("miss", addr->applyMiss, "ApplyMiss", reinterpret_cast<void*>(&ApplyMissDetour),
            reinterpret_cast<void**>(&c.orig.applyMiss), { c.hitPre, c.hitPost });
        addInline("battle.round", addr->advanceRound, "BattleAdvanceRound", reinterpret_cast<void*>(&AdvanceRoundDetour),
            reinterpret_cast<void**>(&c.orig.advanceRound), { c.roundStart, c.roundEnd });
        addInline("battle.turn", addr->combatStartTurn, "CombatStartTurn", reinterpret_cast<void*>(&StartTurnDetour),
            reinterpret_cast<void**>(&c.orig.startTurn), { c.turnStart, c.turnEnd });
        addInline("damage.final", addr->consumeBlock, "ConsumeHealthDamageBlock", reinterpret_cast<void*>(&ConsumeBlockDetour),
            reinterpret_cast<void**>(&c.orig.consumeBlock), { c.damageFinal });
    }

    void ReconcileHooks()
    {
        for (auto& def : hooks) {
            bool desired = def.extraDesired && def.extraDesired();
            for (Topic* topic : def.topics) {
                if (topic && hub->SubscriberCount(topic) > 0) {
                    desired = true;
                }
            }
            // 加载线程可能正在执行这些函数，内联补丁的写入不是原子的
            if (!def.isVtable && loading && desired != def.installed) {
                deferredOps = true;
                continue;
            }
            if (!desired && def.installed) {
                std::string detail;
                deps.hooks->Remove(hook::kCoreOwner, def.handle, &detail);
                def.installed = false;
                def.handle = 0;
                LogChurn(std::string("语义 hook ") + def.name, false,
                    std::string("语义 hook 已移除：") + def.name + (detail.empty() ? "" : "（" + detail + "）"));
            } else if (desired && !def.installed && !def.failed) {
                hook::InstallResult result;
                if (def.isVtable) {
                    hook::VtableRequest request{};
                    request.name = def.name;
                    request.slot = static_cast<void**>(def.target);
                    request.expectedValue = nullptr;
                    request.priority = 0;
                    request.detour = def.detour;
                    request.original = def.orig;
                    result = deps.hooks->InstallVtable(hook::kCoreOwner, request);
                } else {
                    std::vector<std::uint8_t> expected;
                    if (!game::HookEntryBytes(def.entryId, expected)) {
                        def.failed = true;
                        Log(DD_LOG_ERROR, std::string("内联语义 hook ") + def.name + " 的入口字节未知，不安装");
                        for (Topic* topic : def.topics) {
                            hub->SetAvailability(topic, false, "入口字节未知");
                        }
                        continue;
                    }
                    hook::InlineRequest request{};
                    request.name = def.name;
                    request.target = def.target;
                    request.expected = expected.data();
                    request.expectedLength = static_cast<std::uint32_t>(expected.size());
                    request.priority = 0;
                    request.detour = def.detour;
                    request.original = def.orig;
                    request.chainForeignJump = true;
                    result = deps.hooks->InstallInline(hook::kCoreOwner, request);
                }
                if (result.result == hook::Result::Ok) {
                    def.handle = result.handle;
                    def.installed = true;
                    LogChurn(std::string("语义 hook ") + def.name, true,
                        std::string("语义 hook 已安装：") + def.name + (result.chainedForeign ? "（入口已被第三方占用，已串联）" : "") +
                            "，地址 " + DescribeAddress(def.target));
                } else {
                    def.failed = true;
                    Log(DD_LOG_WARN, std::string("语义 hook 安装失败：") + def.name + "：" +
                        hook::ResultText(result.result) + "（" + result.detail + "）");
                    for (Topic* topic : def.topics) {
                        hub->SetAvailability(topic, false, std::string("hook 安装失败：") + result.detail);
                    }
                }
            }
        }
    }

    // ---- 作用域 tracker：有作用域订阅时才订阅战斗/探索结束的原生事件 ----
    static void DD_CALL BattleScopeEnd(DDEvent*, void* user)
    {
        auto* impl = static_cast<Impl*>(user);
        impl->hub->EndScope(DD_EVENT_SCOPE_BATTLE);
    }

    static void DD_CALL RaidScopeEnd(DDEvent*, void* user)
    {
        auto* impl = static_cast<Impl*>(user);
        impl->hub->EndScope(DD_EVENT_SCOPE_RAID);
    }

    void ReconcileScopeTrackers()
    {
        const auto reconcileScope = [&](std::uint32_t scope, std::uint64_t& subId, const char* eventName, DDEventFn fn) {
            const std::size_t scoped = hub->ScopedCount(scope);
            if (scoped > 0 && subId == 0) {
                std::string reason;
                subId = hub->Subscribe(0, eventName, fn, this, 0, DD_EVENT_SCOPE_PLUGIN, &reason, true);
                if (!subId) {
                    Log(DD_LOG_WARN, std::string("作用域 tracker 无法订阅 ") + eventName + "：" + reason +
                        "（作用域订阅不会在该事件时自动撤销）");
                }
            } else if (scoped == 0 && subId != 0) {
                hub->Unsubscribe(0, subId);
                subId = 0;
            }
        };
        reconcileScope(DD_EVENT_SCOPE_BATTLE, battleTrackerSub, "native.BattleDestroyedEvent", &BattleScopeEnd);
        reconcileScope(DD_EVENT_SCOPE_RAID, raidTrackerSub, "native.RaidFinishResultsEvent", &RaidScopeEnd);
    }

    void Reconcile()
    {
        ReconcileScopeTrackers();
        ReconcileListeners();
        ReconcileHooks();
    }
};

namespace {

// 通用 listener 的 _Do_call：rcx 是 FakeImpl，rdx 是 T const*&&
int __fastcall ImplDoCall(FakeImpl* self, const void* const* ppEvent);

void* const kImplVtableStorage[] = {
    reinterpret_cast<void*>(&ImplCopy),
    reinterpret_cast<void*>(&ImplMove),
    reinterpret_cast<void*>(&ImplDoCall),
    reinterpret_cast<void*>(&ImplTarget),
    reinterpret_cast<void*>(&ImplDelete),
    reinterpret_cast<void*>(&ImplGet),
};
static_assert(std::size(kImplVtableStorage) == kStdFuncImplVtableSlots, "std::function 实现的虚表槽数和 layout.inc 不一致");
static_assert(kArgs<decltype(&ImplDoCall)> == kProtoStdFuncDoCall, "_Do_call 的参数个数和 layout.inc 的 ProtoStdFuncDoCall 不一致");

// 回调入口用的全局实例（同时只有一个 System）
System::Impl* g_impl = nullptr;

// SEH 外壳：_Do_call 不能抛异常穿越游戏栈帧
int ImplDoCallSeh(FakeImpl* self, const void* const* ppEvent)
{
    __try {
        auto* type = self->type;
        if (type->kind == ListKind::Event) {
            g_impl->HandleEventCallback(*type, static_cast<const std::uint8_t*>(*ppEvent));
        } else {
            g_impl->HandleQueryCallback(*type, const_cast<std::uint8_t*>(static_cast<const std::uint8_t*>(*ppEvent)));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            OutputDebugStringA("[DDCore] 事件桥接回调发生异常，已吞掉（不影响游戏派发）\n");
        }
    }
    return 0;  // 0 继续派发，1 才中止后续监听者
}

int __fastcall ImplDoCall(FakeImpl* self, const void* const* ppEvent)
{
    return ImplDoCallSeh(self, ppEvent);
}

}  // namespace

void* const* System::Impl::kImplVtable()
{
    return kImplVtableStorage;
}

void System::Impl::HandleEventCallback(TypeState& type, const std::uint8_t* event)
{
    const DWORD thread = GetCurrentThreadId();
    if (!type.firstLogged) {
        type.firstLogged = true;
        if (type.topic->dispatchCount == 0) {
            type.topic->firstThread = thread;
            type.topic->firstSeen = true;
        }
        LogListOrder(type, "首次派发");
    }
    ++type.topic->dispatchCount;

    const auto emit = [&](Topic* topic, Slot* slots) {
        if (thread == deps.mainThread) {
            Payload payload;
            payload.slots = slots;
            payload.originThread = thread;
            payload.allowWrite = false;
            hub->Dispatch(topic, payload);
        } else {
            hub->EnqueueDeferred(topic, slots, thread);
        }
    };

    if (hub->SubscriberCount(type.topic) > 0) {
        Slot slots[kMaxFields]{};
        slots[0].v.s = type.name.c_str();
        slots[0].set = true;
        slots[1].v.i = *reinterpret_cast<const std::int64_t*>(event + kEventTime);
        slots[1].set = true;
        for (std::size_t i = 0; i < type.queryFields.size() && i < type.querySlotIndices.size(); ++i) {
            const std::uint32_t index = type.querySlotIndices[i];
            if (index < kMaxFields) {
                ReadRaw(event, type.queryFields[i].map, slots[index]);
            }
        }
        emit(type.topic, slots);
    }
    for (auto& target : type.targets) {
        if (hub->SubscriberCount(target.topic) == 0) {
            continue;
        }
        Slot slots[kMaxFields]{};
        for (std::size_t i = 0; i < target.fields.size(); ++i) {
            const std::uint32_t index = target.slotIndices[i];
            if (index < kMaxFields) {
                ReadRaw(event, target.fields[i], slots[index]);
            }
        }
        emit(target.topic, slots);
        ++target.topic->dispatchCount;
    }
}

void System::Impl::HandleQueryCallback(TypeState& type, std::uint8_t* query)
{
    const DWORD thread = GetCurrentThreadId();
    if (!type.firstLogged) {
        type.firstLogged = true;
        if (type.topic->dispatchCount == 0) {
            type.topic->firstThread = thread;
            type.topic->firstSeen = true;
        }
        LogListOrder(type, "首次派发");
    }
    ++type.topic->dispatchCount;
    if (hub->SubscriberCount(type.topic) == 0) {
        return;
    }
    // Query 的回调可以写结果，会改变结算，PvP 对局中不派发（ADR 0011）
    if (ActivePvP()) {
        return;
    }
    Slot slots[kMaxFields]{};
    slots[0].v.s = type.name.c_str();
    slots[0].set = true;
    for (std::size_t i = 0; i < type.queryFields.size(); ++i) {
        const std::uint32_t index = type.querySlotIndices[i];
        if (index < kMaxFields) {
            ReadRaw(query, type.queryFields[i].map, slots[index]);
        }
    }
    if (thread != deps.mainThread) {
        hub->EnqueueDeferred(type.topic, slots, thread);
        return;
    }
    Payload payload;
    payload.slots = slots;
    payload.originThread = thread;
    payload.allowWrite = true;
    hub->Dispatch(type.topic, payload);
    // 写回结果字段
    for (std::size_t i = 0; i < type.queryFields.size(); ++i) {
        const auto& field = type.queryFields[i];
        const std::uint32_t index = type.querySlotIndices[i];
        if (field.writable && index < kMaxFields && slots[index].set) {
            WriteRaw(query, field.map, slots[index]);
        }
    }
}

System::System(SystemDeps deps) : impl_(std::make_unique<Impl>())
{
    auto& impl = *impl_;
    impl.deps = std::move(deps);
    g_impl = &impl;
    impl.LocateSections();
    impl.hub = std::make_unique<Hub>(impl.deps.invoke, impl.deps.log);
    impl.ctx.hub = impl.hub.get();
    impl.ctx.mainThread = impl.deps.mainThread;
    impl.ctx.addr = impl.deps.addr;
    impl.ctx.log = impl.deps.log;
    g_ctx = &impl.ctx;
    impl.FindListenerVtables();
    impl.RegisterSemanticTopics();
    impl.RegisterNativeTopics();
    impl.RegisterQueryTopics();
    impl.BuildHookDefs();
    impl.Log(DD_LOG_INFO, "事件系统就绪：原生主题 " + std::to_string(impl.eventTypes.size()) + " 个，Query 主题 " +
        std::to_string(impl.queryTypes.size()) + " 个，语义 hook 定义 " + std::to_string(impl.hooks.size()) + " 个");
}

System::~System() = default;

Hub& System::hub()
{
    return *impl_->hub;
}

void System::OnAppReady()
{
    auto& impl = *impl_;
    impl.eventManagerReady = impl.deps.addr->eventManager && *impl.deps.addr->eventManager;
    if (!impl.eventManagerReady) {
        impl.Log(DD_LOG_WARN, "AppReady 时 EventManager 仍为空，原生事件不可用");
    }
    impl.Reconcile();
}

bool System::Loading() const
{
    return impl_->loading;
}

void System::AddHook(ExternalHook hook)
{
    if (!hook.target || !hook.detour || !hook.orig) {
        return;
    }
    HookDef def;
    def.name = hook.name;
    def.isVtable = hook.isVtable;
    def.target = hook.target;
    def.entryId = hook.entryId;
    def.detour = hook.detour;
    def.orig = hook.orig;
    def.topics = std::move(hook.topics);
    def.extraDesired = std::move(hook.extraDesired);
    impl_->hooks.push_back(std::move(def));
}

void System::RequestReconcile()
{
    impl_->hub->MarkDirty();
}

bool System::HookInstalled(const char* name) const
{
    for (const auto& def : impl_->hooks) {
        if (def.name && name && std::strcmp(def.name, name) == 0) {
            return def.installed;
        }
    }
    return false;
}

void System::FrameBoundary(const GameSample& sample, std::uint64_t frame, std::uint32_t, bool assetsReady)
{
    auto& impl = *impl_;
    // 三条加载线程（load_campaign、load_raid、load_town）运行时状态停留在各自的状态上，
    // 它们会派发事件、注册 listener。这期间不改 listener 列表，也不改内联 hook
    const auto& states = impl.deps.addr->states;
    impl.loading = sample.lastState == sample.nextState &&
        (sample.lastState == states.loadCampaign || sample.lastState == states.raidStart ||
            sample.lastState == states.townVisitStart);
    if (impl.deferredOps && !impl.loading) {
        impl.deferredOps = false;
        impl.hub->MarkDirty();
    }
    if (impl.hub->TestAndClearDirty()) {
        impl.Reconcile();
    }
    if (!impl.churn.Idle()) {
        impl.churn.Flush(GetTickCount64(), [&](const std::string& line) { impl.Log(DD_LOG_INFO, line); });
    }

    // 读档线程派发的事件这时已经排在队列里，主线程照常处理（数据已复制）
    impl.hub->DrainDeferred();

    // app.state
    if (!impl.haveState || sample.lastState != impl.prevLastState) {
        if (impl.haveState && impl.appState && impl.hub->SubscriberCount(impl.appState) > 0) {
            Slot slots[kMaxFields]{};
            slots[0].v.i = impl.prevLastState;
            slots[0].set = true;
            slots[1].v.i = sample.lastState;
            slots[1].set = true;
            const auto& names = impl.deps.addr->stateNames;
            const auto nameOf = [&](int state) {
                return state >= 0 && static_cast<std::size_t>(state) < names.size() ? names[static_cast<std::size_t>(state)] : "";
            };
            slots[2].v.s = nameOf(impl.prevLastState);
            slots[2].set = true;
            slots[3].v.s = nameOf(sample.lastState);
            slots[3].set = true;
            Payload payload;
            payload.slots = slots;
            payload.originThread = GetCurrentThreadId();
            impl.hub->Dispatch(impl.appState, payload);
        }
        impl.haveState = true;
        impl.prevLastState = sample.lastState;
    }

    if (assetsReady && impl.appTick && impl.hub->SubscriberCount(impl.appTick) > 0) {
        Slot slots[kMaxFields]{};
        slots[0].v.i = static_cast<std::int64_t>(frame);
        slots[0].set = true;
        Payload payload;
        payload.slots = slots;
        payload.originThread = GetCurrentThreadId();
        impl.hub->Dispatch(impl.appTick, payload);
    }
}

void System::OnCampaignLoad(std::uint32_t serial)
{
    auto& impl = *impl_;
    impl.OrderSummary("CampaignLoad");
    if (impl.campaignLoad && impl.hub->SubscriberCount(impl.campaignLoad) > 0) {
        Slot slots[kMaxFields]{};
        slots[0].v.i = serial;
        slots[0].set = true;
        slots[1].v.p = impl.deps.addr->campaignCurrent ? *impl.deps.addr->campaignCurrent : nullptr;
        slots[1].set = true;
        Payload payload;
        payload.slots = slots;
        payload.originThread = GetCurrentThreadId();
        impl.hub->Dispatch(impl.campaignLoad, payload);
    }
}

void System::OnCampaignUnload(std::uint32_t serial)
{
    auto& impl = *impl_;
    impl.hub->EndScope(DD_EVENT_SCOPE_BATTLE);
    impl.hub->EndScope(DD_EVENT_SCOPE_RAID);
    impl.hub->EndScope(DD_EVENT_SCOPE_CAMPAIGN);
    if (impl.campaignUnload && impl.hub->SubscriberCount(impl.campaignUnload) > 0) {
        Slot slots[kMaxFields]{};
        slots[0].v.i = serial;
        slots[0].set = true;
        Payload payload;
        payload.slots = slots;
        payload.originThread = GetCurrentThreadId();
        impl.hub->Dispatch(impl.campaignUnload, payload);
    }
}

std::size_t System::ListenerCount() const
{
    std::size_t count = 0;
    for (const auto& type : impl_->eventTypes) {
        count += type->registered ? 1 : 0;
    }
    for (const auto& type : impl_->queryTypes) {
        count += type->registered ? 1 : 0;
    }
    return count;
}

std::size_t System::InstalledHookCount() const
{
    std::size_t count = 0;
    for (const auto& def : impl_->hooks) {
        count += def.installed ? 1 : 0;
    }
    return count;
}

void System::LogOrderSummary(const char* when)
{
    impl_->OrderSummary(when);
}

std::string System::HookReportSummary() const
{
    return "事件系统：已注册 listener " + std::to_string(ListenerCount()) + " 个，语义 hook " +
        std::to_string(InstalledHookCount()) + " 个，订阅 " + std::to_string(impl_->hub->ExternalSubscriberCount()) + " 个";
}

}  // namespace ddc::events
