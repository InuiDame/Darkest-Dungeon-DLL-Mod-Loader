// 布局清单的旧值对照（U1a）：常量从散落的定义改成从 core/src/game/layout.inc 取之后，逐项和改动前源码里的值比较。
// 本文件由改动前（git HEAD）的源码生成，全部是编译期断言：dd_core_tests 能编译就说明值没有变。以后有意改布局常量时，
// 先改 layout.inc，再改这里对应的一行（注明原因），或者整个文件随下一次基线刷新一起删掉。

#include "../../core/src/game/layout.h"

#include <dd/dd_api.h>

#include <bit>
#include <cstddef>
#include <string_view>

namespace layout_golden {

using namespace ddc::game::layout;

template <std::size_t N, std::size_t M>
consteval bool SameNames(const char* const (&a)[N], const char* const (&b)[M])
{
    if (N != M) {
        return false;
    }
    for (std::size_t i = 0; i < N; ++i) {
        if (std::string_view(a[i]) != std::string_view(b[i])) {
            return false;
        }
    }
    return true;
}

template <std::size_t N, std::size_t M>
consteval bool SameValues(const std::uint32_t (&a)[N], const std::uint32_t (&b)[M])
{
    if (N != M) {
        return false;
    }
    for (std::size_t i = 0; i < N; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

// ---- 常量 ----
static_assert(kRosterAlt == 0x728);  // game/actors.h actor_layout::kRosterAlt
static_assert(kRosterHeroes == 0x20);  // game/actors.h actor_layout::kRosterHeroes
static_assert(kRaidHeroes == 0x08);  // game/actors.h actor_layout::kRaidHeroes
static_assert(kEntryHero == 0x08);  // game/actors.h actor_layout::kEntryHero
static_assert(kHeroId == 0x137C);  // game/actors.h actor_layout::kHeroId
static_assert(kHeroClass == 0x1320);  // game/actors.h actor_layout::kHeroClass
static_assert(kMonsterGuid == 0x1320);  // game/actors.h actor_layout::kMonsterGuid
static_assert(kMonsterClass == 0x1468);  // game/actors.h actor_layout::kMonsterClass
static_assert(kRaidBattle == 0x4898);  // game/actors.h actor_layout::kRaidBattle
static_assert(kBattleMonsters == 0x38);  // game/actors.h actor_layout::kBattleMonsters
static_assert(kBattleState == 0x288);  // game/actors.h actor_layout::kBattleState
static_assert(kBattleGuidCounter == 0x2D4);  // game/actors.h actor_layout::kBattleGuidCounter
static_assert(kActorHp == 0x10A8);  // game/actors.h actor_layout::kActorHp
static_assert(kActorStress == 0x12FC);  // game/actors.h actor_layout::kActorStress
static_assert(kActorMaxHpStat == 0x768);  // game/actors.h actor_layout::kActorMaxHpStat
static_assert(kActorMaxHpMask == 0x790);  // game/actors.h actor_layout::kActorMaxHpMask
static_assert(kClassId == 0x08);  // game/actors.h actor_layout::kClassId
static_assert(kClassIdCap == 0x40);  // game/actors.h actor_layout::kClassIdCapacity
static_assert(kActorBuffGroup == 0x460);  // combat/combat.cpp layout::kActorBuffGroup
static_assert(kActorNextGuid == 0x478);  // combat/combat.cpp layout::kActorNextGuid
static_assert(kBuffEntrySize == 0x418);  // combat/combat.cpp layout::kEntrySize
static_assert(kBuffEntryInstance == 0x08);  // combat/combat.cpp layout::kEntryInstance
static_assert(kBuffInstanceSize == 0x410);  // combat/combat.cpp layout::kInstanceSize
static_assert(kInstStat == 0x00);  // combat/combat.cpp layout::kInstStat
static_assert(kInstSubType == 0x04);  // combat/combat.cpp layout::kInstSubType
static_assert(kInstSubHash == 0x44);  // combat/combat.cpp layout::kInstSubHash
static_assert(kInstAmount == 0x48);  // combat/combat.cpp layout::kInstAmount
static_assert(kInstDuration == 0x4C);  // combat/combat.cpp layout::kInstDuration
static_assert(kInstDurationType == 0x50);  // combat/combat.cpp layout::kInstDurationType
static_assert(kInstSource == 0x54);  // combat/combat.cpp layout::kInstSource
static_assert(kInstSourceData == 0x58);  // combat/combat.cpp layout::kInstSourceData
static_assert(kInstRule == 0x5C);  // combat/combat.cpp layout::kInstRule
static_assert(kInstFalseRule == 0x60);  // combat/combat.cpp layout::kInstFalseRule
static_assert(kInstId == 0x61);  // combat/combat.cpp layout::kInstId
static_assert(kInstIdHash == 0xA4);  // combat/combat.cpp layout::kInstIdHash
static_assert(kInstTarget == 0xA8);  // combat/combat.cpp layout::kInstTarget
static_assert(kInstRemoveInactive == 0xAC);  // combat/combat.cpp layout::kInstRemoveInactive
static_assert(kInstRemoveBattle == 0xAD);  // combat/combat.cpp layout::kInstRemoveBattle
static_assert(kInstRuleFloat == 0xB0);  // combat/combat.cpp layout::kInstRuleFloat
static_assert(kInstRuleString == 0xB4);  // combat/combat.cpp layout::kInstRuleString
static_assert(kInstRuleHash == 0xF4);  // combat/combat.cpp layout::kInstRuleHash
static_assert(kInstAdditional == 0x158);  // combat/combat.cpp layout::kInstAdditional
static_assert(kInstClearOnRule == 0x3B0);  // combat/combat.cpp layout::kInstClearOnRule
static_assert(kInstRuleHash + 4 == 0xF8);  // combat/combat.cpp layout::kInstReadSize（改成由 kInstRuleHash 推出）
static_assert(kLibraryAltMap == 0x10);  // combat/combat.cpp layout::kLibraryAltMap
static_assert(kMapNodeLeft == 0x00);  // combat/combat.cpp layout::kNodeLeft
static_assert(kMapNodeRight == 0x10);  // combat/combat.cpp layout::kNodeRight
static_assert(kMapNodeIsNil == 0x19);  // combat/combat.cpp layout::kNodeIsNil
static_assert(kMapNodeKey == 0x20);  // combat/combat.cpp layout::kNodeKey
static_assert(kMapNodeValue == 0x28);  // combat/combat.cpp layout::kNodeValue
static_assert(kEffectTarget == 0x44);  // combat/combat.cpp layout::kEffectTarget
static_assert(kEffectChance == 0x48);  // combat/combat.cpp layout::kEffectChance
static_assert(kEffectStress == 0x4C);  // combat/combat.cpp layout::kEffectStress
static_assert(kEffectStun == 0x50);  // combat/combat.cpp layout::kEffectStun
static_assert(kEffectHeal == 0xC8);  // combat/combat.cpp layout::kEffectHeal
static_assert(kEffectHealPercent == 0xCC);  // combat/combat.cpp layout::kEffectHealPercent
static_assert(kEffectCombatStatBuff == 0xEC);  // combat/combat.cpp layout::kEffectCombatStatBuff
static_assert(kEffectDotPoison == 0x134);  // combat/combat.cpp layout::kEffectDotPoison
static_assert(kEffectDotBleed == 0x138);  // combat/combat.cpp layout::kEffectDotBleed
static_assert(kEffectDotBurn == 0x13C);  // combat/combat.cpp layout::kEffectDotBurn
static_assert(kEffectDotStress == 0x1B8);  // combat/combat.cpp layout::kEffectDotStress
static_assert(kEffectDuration == 0x1CC);  // combat/combat.cpp layout::kEffectDuration
static_assert(kEffectOnHit == 0x345);  // combat/combat.cpp layout::kEffectOnHit
static_assert(kEffectOnMiss == 0x346);  // combat/combat.cpp layout::kEffectOnMiss
static_assert(kEffectApplyOnce == 0x347);  // combat/combat.cpp layout::kEffectApplyOnce
static_assert(kEffectQueue == 0x348);  // combat/combat.cpp layout::kEffectQueue
static_assert(kEffectBuffAmount == 0x3B8);  // combat/combat.cpp layout::kEffectBuffAmount
static_assert(kEffectBuffIds == 0x3C8);  // combat/combat.cpp layout::kEffectBuffIds
static_assert(kEffectSwap == 0x3E0);  // combat/combat.cpp layout::kEffectSwap
static_assert(kEffectBuffDurationType == 0x430);  // combat/combat.cpp layout::kEffectBuffDurationType
static_assert(kEffectReadSize == 0x438);  // combat/combat.cpp layout::kEffectReadSize
static_assert(kHeroClassSkills == 0xBD0);  // combat/combat.cpp layout::kHeroClassSkills
static_assert(kHeroClassMoveSkills == 0xBE8);  // combat/combat.cpp layout::kHeroClassMoveSkills
static_assert(kHeroSkillSize == 0x510);  // combat/combat.cpp layout::kHeroSkillSize
static_assert(kMonsterClassSkills == 0xBA8);  // combat/combat.cpp layout::kMonsterClassSkills
static_assert(kMonsterSkillSize == 0x500);  // combat/combat.cpp layout::kMonsterSkillSize
static_assert(kSkillId == 0x08);  // combat/combat.cpp layout::kSkillId
static_assert(kSkillLevel == 0x48);  // combat/combat.cpp layout::kSkillLevel
static_assert(kSkillType == 0x50);  // combat/combat.cpp layout::kSkillType
static_assert(kSkillFriendly == 0x90);  // combat/combat.cpp layout::kSkillFriendly
static_assert(kSkillEffects == 0x3C8);  // combat/combat.cpp layout::kSkillEffects
static_assert(kRaidPvpName == 0x52C0);  // combat/combat.cpp layout::kRaidPvpName
static_assert(kSlotAddBuff == 13);  // combat/combat.cpp layout::kSlotAddBuff
static_assert(kSlotRemoveBuffs == 14);  // combat/combat.cpp layout::kSlotRemoveBuffs
static_assert(kMapNodeParent == 8);  // combat/combat.cpp MapFind：head + 8 是根
static_assert(kStdVectorSize == 0x18);  // combat/combat.cpp EhVectorDtor 的元素大小、技能表外层 vector 的步长
static_assert(kInstAdditionalCount == 0x19);  // combat/combat.cpp EhVectorDtor 的元素个数
static_assert(kEffectApplyResultSize == 0x20);  // combat/combat.cpp G_ApplyEffect 的 res[0x20]
static_assert(kEffectApplyResultGuids == 8);  // combat/combat.cpp G_ApplyEffect 的 res + 8
static_assert(kStdFunctionSize == 0x40);  // combat/combat.cpp DoRemove 的 function[0x40]
static_assert(kStdFunctionImpl == 0x38);  // combat/combat.cpp DoRemove 的 function + 0x38；system.cpp FakeListener 的 funcStorage[0x38]
static_assert(kStdFuncImplVtableSlots == 6);  // combat/combat.cpp kPredVtable、system.cpp kImplVtableStorage 的项数
static_assert(kGameTextCap == 64);  // SDK 的 DD_COMBAT_TEXT（游戏里 char[64] 的字段）
static_assert(kBattleRound == 0xA8);  // events/system.cpp kBattleRound
static_assert(kVtSlotStress == 36);  // events/system.cpp kVtSlotStress
static_assert(kVtSlotDamage == 50);  // events/system.cpp kVtSlotDamage
static_assert(kVtSlotHeal == 51);  // events/system.cpp kVtSlotHeal
static_assert(kListenerEntrySize == 0x28);  // events/system.cpp kEntrySize
static_assert(kListenerEntryListener == 0x08);  // events/system.cpp kEntryListener
static_assert(kListenerEntryActive == 0x10);  // events/system.cpp kEntryActive
static_assert(kQueryListenerEntrySize == 0x18);  // events/system.cpp kQueryEntrySize
static_assert(kVtSlotDeathsDoor == 40);  // events/system.cpp kVtSlotDeathsDoor
static_assert(kVtSlotDead == 43);  // events/system.cpp kVtSlotDead
static_assert(kAttackPerformer == 0x20);  // events/system.cpp kAttackPerformer
static_assert(kAttackSkill == 0x28);  // events/system.cpp kAttackSkill
static_assert(kTargetActor == 0x00);  // events/system.cpp kTargetActor
static_assert(kTargetDamage == 0x08);  // events/system.cpp kTargetDamage
static_assert(kTargetHit == 0x10);  // events/system.cpp kTargetHit
static_assert(kTargetCrit == 0x11);  // events/system.cpp kTargetCrit
static_assert(kMonsterClassCanBeDamagedDirectly == 0xF97);  // events/system.cpp MonsterRule 的 monsterClass[0xF97]
static_assert(kMonsterImmuneTypeCount == 20);  // events/system.cpp MonsterRule 的 type >= 20
static_assert(kEffectName == 0);  // events/system.cpp EffectApplyDetour：Effect+0 是名字
static_assert(kEffectNameCap == 64);  // events/system.cpp EffectApplyDetour 的 memcpy(effectName, effect, 64)
static_assert(kStdVectorLast == 8);  // events/system.cpp VecLast 的 ReadPtr(vec, 8)
static_assert(kClassNameStride == 0x80);  // events/system.cpp 类名表的 i * 0x80；game/game.cpp Query 类名表的 i * 0x80
static_assert(kEventTime == 8);  // events/system.cpp HandleEventCallback 的 event + 8
static_assert(kListenerPriority == 8);  // events/system.cpp LogListOrder / OrderSummary 的 listener + 8
static_assert(kListenerFunction == 0x10);  // events/system.cpp FakeListener：vtable、priority、pad 之后的 funcStorage
static_assert(kListDataSize == 0x20);  // events/system.cpp struct ListData（first、last、end、dirty、pad[7]）
static_assert(kListDataDirty == 0x18);  // events/system.cpp struct ListData 的 dirty
static_assert(kQueryClassCapacity == 64);  // game/game.cpp Query 类名表最多读 64 项
static_assert(kMonsterBlockImmuneLeaDisp == 52);  // game/game.cpp MonsterBlockReturn 的 hit + 52
static_assert(kMonsterBlockImmuneLeaNext == 56);  // game/game.cpp MonsterBlockReturn 的 hit + 56
static_assert(kBattleStateNone == 0);  // game/phase_tracker.cpp 的 s.state != 0（bs_none）
static_assert(kEvHeroReachedDeathsDoor_hero == 0x10);  // events/system.cpp NativeSources HeroReachedDeathsDoorEvent.hero
static_assert(kEvHeroReachedDeathsDoor_damage_type == 0x18);  // events/system.cpp NativeSources HeroReachedDeathsDoorEvent.damage_type
static_assert(kEvHeroReachedDeathsDoor_source_id == 0x1C);  // events/system.cpp NativeSources HeroReachedDeathsDoorEvent.source_id
static_assert(kEvHeroReachedDeathsDoor_source_type == 0x20);  // events/system.cpp NativeSources HeroReachedDeathsDoorEvent.source_type
static_assert(kEvHeartAttack_heart_attacks == 0x14);  // events/system.cpp NativeSources HeartAttackEvent.heart_attacks
static_assert(kEvHeartAttack_flag0 == 0x18);  // events/system.cpp NativeSources HeartAttackEvent.flag0
static_assert(kEvHeartAttack_flag1 == 0x19);  // events/system.cpp NativeSources HeartAttackEvent.flag1
static_assert(kEvHeartAttack_flag2 == 0x1A);  // events/system.cpp NativeSources HeartAttackEvent.flag2
static_assert(kEvHeroDeath_hero_id == 0x10);  // events/system.cpp NativeSources HeroDeathEvent.hero_id
static_assert(kEvMonsterDeath_monster_guid == 0x18);  // events/system.cpp NativeSources MonsterDeathEvent.monster_guid
static_assert(kEvMonsterDeath_attacker_id == 0x1C);  // events/system.cpp NativeSources MonsterDeathEvent.attacker_id
static_assert(kEvMonsterDeath_damage_type == 0x20);  // events/system.cpp NativeSources MonsterDeathEvent.damage_type
static_assert(kEvMonsterDeath_class_field_d0 == 0x10);  // events/system.cpp NativeSources MonsterDeathEvent.class_field_d0
static_assert(kEvMonsterDeath_class_field_cc == 0x14);  // events/system.cpp NativeSources MonsterDeathEvent.class_field_cc
static_assert(kEvMonsterDeath_flag == 0x24);  // events/system.cpp NativeSources MonsterDeathEvent.flag
static_assert(kEvHeroAfflictionVirtue_hero == 0x10);  // events/system.cpp NativeSources HeroAfflictionVirtueEvent.hero
static_assert(kEvRaidStart_raid == 0x10);  // events/system.cpp NativeSources RaidStartEvent.raid
static_assert(kEvRaidFinishResults_results == 0x10);  // events/system.cpp NativeSources RaidFinishResultsEvent.results
static_assert(kEvTorchModified_torch_before == 0x10);  // events/system.cpp NativeSources TorchModifiedEvent.torch_before
static_assert(kEvTorchModified_torch_after == 0x14);  // events/system.cpp NativeSources TorchModifiedEvent.torch_after
static_assert(kEvTorchModified_delta == 0x18);  // events/system.cpp NativeSources TorchModifiedEvent.delta
static_assert(kEvTorchModified_flag == 0x1C);  // events/system.cpp NativeSources TorchModifiedEvent.flag
static_assert(kEvPreOnStartTownVisit_flag == 0x10);  // events/system.cpp NativeSources PreOnStartTownVisitEvent.flag
static_assert(kEvTownEventOnStartTownVisit_event == 0x10);  // events/system.cpp NativeSources TownEventOnStartTownVisitEvent.event
static_assert(kEvBuildingStateChanged_name == 0x10);  // events/system.cpp NativeSources BuildingStateChangedEvent.name
static_assert(kEvBuildingStateChanged_name_cap == 0x50);  // events/system.cpp NativeSources BuildingStateChangedEvent.name 的容量
static_assert(kEvBuildingStateChanged_value == 0x60);  // events/system.cpp NativeSources BuildingStateChangedEvent.value
static_assert(kEvBuildingStateChanged_flag == 0x64);  // events/system.cpp NativeSources BuildingStateChangedEvent.flag
static_assert(kEvActivityOpened_activity == 0x10);  // events/system.cpp NativeSources ActivityOpenedEvent.activity
static_assert(kEvActivityOpened_flag == 0x18);  // events/system.cpp NativeSources ActivityOpenedEvent.flag
static_assert(kEvActivityFinished_activity_id == 0x10);  // events/system.cpp NativeSources ActivityFinishedEvent.activity_id
static_assert(kEvHeroRecruited_hero_id == 0x10);  // events/system.cpp NativeSources HeroRecruitedEvent.hero_id
static_assert(kEvHeroDismissed_hero_id == 0x10);  // events/system.cpp NativeSources HeroDismissedEvent.hero_id
static_assert(kEvQuirkApplied_hero_id == 0x10);  // events/system.cpp NativeSources QuirkAppliedEvent.hero_id
static_assert(kEvQuirkApplied_kind == 0x14);  // events/system.cpp NativeSources QuirkAppliedEvent.kind
static_assert(kEvQuirkApplied_quirk == 0x18);  // events/system.cpp NativeSources QuirkAppliedEvent.quirk
static_assert(kEvQuirkApplied_quirk_cap == 0x100);  // events/system.cpp NativeSources QuirkAppliedEvent.quirk 的容量
static_assert(kEvHeroSkillUpgraded_hero_id == 0x10);  // events/system.cpp NativeSources HeroSkillUpgradedEvent.hero_id
static_assert(kEvHeroSkillUpgraded_skill_index == 0x14);  // events/system.cpp NativeSources HeroSkillUpgradedEvent.skill_index
static_assert(kEvInventoryAddItem_inventory == 0x10);  // events/system.cpp NativeSources InventoryAddItemEvent.inventory
static_assert(kEvInventoryAddItem_item == 0x18);  // events/system.cpp NativeSources InventoryAddItemEvent.item
static_assert(kEvInventoryAddItem_value == 0x20);  // events/system.cpp NativeSources InventoryAddItemEvent.value
static_assert(kEvInventoryPreMerge_inventory == 0x10);  // events/system.cpp NativeSources InventoryPreMergeEvent.inventory
static_assert(kEvInventoryPreMerge_other == 0x18);  // events/system.cpp NativeSources InventoryPreMergeEvent.other
static_assert(kEvInventoryPostMerge_inventory == 0x10);  // events/system.cpp NativeSources InventoryPostMergeEvent.inventory
static_assert(kEvInventoryPostMerge_other == 0x18);  // events/system.cpp NativeSources InventoryPostMergeEvent.other
static_assert(kEvEstateCurrencyChange_currency == 0x10);  // events/system.cpp NativeSources EstateCurrencyChangeEvent.currency
static_assert(kEvEstateCurrencyChange_delta == 0x14);  // events/system.cpp NativeSources EstateCurrencyChangeEvent.delta
static_assert(kQyCampingPointBuff_points == 0x08);  // events/system.cpp QueryDefs CampingPointBuffQuery.points
static_assert(kQyCurioInteractionBuff_hero == 0x08);  // events/system.cpp QueryDefs CurioInteractionBuffQuery.hero
static_assert(kQyCurioInteractionBuff_curio == 0x10);  // events/system.cpp QueryDefs CurioInteractionBuffQuery.curio
static_assert(kQyCurioInteractionBuff_stress == 0x18);  // events/system.cpp QueryDefs CurioInteractionBuffQuery.stress
static_assert(kQyCurioInteractionBuff_heal == 0x1C);  // events/system.cpp QueryDefs CurioInteractionBuffQuery.heal
static_assert(kQyFindNonRosterHeroes_container == 0x08);  // events/system.cpp QueryDefs FindNonRosterHeroesQuery.container
static_assert(kQyInventoryExtraStackingLimit_item_id == 0x08);  // events/system.cpp QueryDefs InventoryExtraStackingLimitQuery.item_id
static_assert(kQyInventoryExtraStackingLimit_arg == 0x0C);  // events/system.cpp QueryDefs InventoryExtraStackingLimitQuery.arg
static_assert(kQyInventoryExtraStackingLimit_limit == 0x10);  // events/system.cpp QueryDefs InventoryExtraStackingLimitQuery.limit
static_assert(kQyInventoryReplacementEffectId_item_id == 0x08);  // events/system.cpp QueryDefs InventoryReplacementEffectIdQuery.item_id
static_assert(kQyInventoryReplacementEffectId_arg == 0x0C);  // events/system.cpp QueryDefs InventoryReplacementEffectIdQuery.arg
static_assert(kQyInventoryReplacementEffectId_effect_id == 0x10);  // events/system.cpp QueryDefs InventoryReplacementEffectIdQuery.effect_id
static_assert(kQyIsDisabled_hero_id == 0x08);  // events/system.cpp QueryDefs IsDisabledQuery.hero_id
static_assert(kQyIsDisabled_disabled == 0x0C);  // events/system.cpp QueryDefs IsDisabledQuery.disabled
static_assert(kQyLootDarknessBuff_light == 0x08);  // events/system.cpp QueryDefs LootDarknessBuffQuery.light
static_assert(kQyLootDarknessBuff_result == 0x10);  // events/system.cpp QueryDefs LootDarknessBuffQuery.result
static_assert(kQyLootDarknessBuff_arg == 0x18);  // events/system.cpp QueryDefs LootDarknessBuffQuery.arg
static_assert(kQyProvisionIsItemTooltipsEnabled_enabled == 0x08);  // events/system.cpp QueryDefs ProvisionIsItemTooltipsEnabledQuery.enabled
static_assert(kQyProvisionSupplyBuff_items_first == 0x08);  // events/system.cpp QueryDefs ProvisionSupplyBuffQuery.items_first
static_assert(kQyTownGoldInterestBuff_interest == 0x08);  // events/system.cpp QueryDefs TownGoldInterestBuffQuery.interest
static_assert(kQyTownVisitIdleStressBuff_stress_relief == 0x08);  // events/system.cpp QueryDefs TownVisitIdleStressBuffQuery.stress_relief
static_assert(kQyTownVisitIdleStressBuff_flag == 0x0C);  // events/system.cpp QueryDefs TownVisitIdleStressBuffQuery.flag
static_assert(kStorageProfileSlot == 0x38);  // persist/persist.cpp kSmSlot5
static_assert(kStorageDeviceName == 0x20);  // persist/persist.cpp kDeviceName
static_assert(kStorageDeviceNameCap == 0x80);  // persist/persist.cpp kDeviceNameCap
static_assert(kUgcIdSize == 0xC0);  // game/mods.cpp kUgcIdSize
static_assert(kUgcIdSource == 0x80);  // game/mods.cpp kUgcIdSource
static_assert(kUgcListKeyOffset == 0x200);  // game/mods.cpp kListKeyOffset
static_assert(kUserServicesInstalled == 0x20);  // game/mods.cpp kInstalledVector
static_assert(kUgcPath == 0x20);  // game/mods.cpp kUgcPath
static_assert(kUgcObjectId == 0x620);  // game/mods.cpp kUgcId
static_assert(kUgcListKeyCap == 0x200);  // game/mods.cpp 键名的 ReadFixedString(.., 0x200, ..)
static_assert(kUgcIdNameCap == 0x80);  // game/mods.cpp name 的 ReadFixedString(.., 0x80, ..)
static_assert(kUgcIdSourceCap == 0x40);  // game/mods.cpp source 的 ReadFixedString(.., 0x40, ..)
static_assert(kUgcPathCap == 0x200);  // game/mods.cpp 目录的 ReadFixedString(.., 0x200, ..)
static_assert(kGameTextCap - 1 == 63);  // combat/logic.h kTextMax
static_assert(kEffectBlockMax == 0x3FF);  // combat/logic.h kEffectBlockMax
static_assert(kGameHashMul == 53);  // combat/logic.cpp GameHash 的乘数
static_assert(kEffectListKillEnemyTypes == 4);  // combat/logic.cpp kLists 的 .kill_enemy_types
static_assert(kEffectListBuffIds == 8);  // combat/logic.cpp kLists 的 .buff_ids
static_assert(kEffectListSummonMonsters == 4);  // combat/logic.cpp kLists 的 .summon_monsters
static_assert(kEffectListSummonRanks == 4);  // combat/logic.cpp kLists 的 .summon_ranks
static_assert(kEffectListSetMonsterClassIds == 8);  // combat/logic.cpp kLists 的 .set_monster_class_ids
static_assert(IndexOf(kEnumRuleNames, "actorStatus") == 13);  // combat/combat.cpp 规则类型 13
static_assert(IndexOf(kEnumRuleNames, "is_actor_status") == 42);  // combat/combat.cpp 规则类型 42
static_assert(IndexOf(kEnumStatNames, "combat_stat_add") == 2);  // combat/logic.cpp ValidateSubStat 的 case 2
static_assert(IndexOf(kEnumStatNames, "combat_stat_multiply") == 3);  // combat/logic.cpp ValidateSubStat 的 case 3
static_assert(IndexOf(kEnumStatNames, "resistance") == 11);  // combat/logic.cpp ValidateSubStat 的 case 11
static_assert(IndexOf(kEnumStatNames, "activity_side_effect_chance") == 32);  // combat/logic.cpp ValidateSubStat 的 case 32
static_assert(IndexOf(kEnumStatNames, "disable_combat_skill_attribute") == 33);  // combat/logic.cpp ValidateSubStat 的 case 33
static_assert(IndexOf(kEnumStatNames, "stress_dmg_percent") == 9);  // combat/logic.cpp ValidateSubStat 的 case 9
static_assert(IndexOf(kEnumStatNames, "stress_heal_percent") == 10);  // combat/logic.cpp ValidateSubStat 的 case 10
static_assert(IndexOf(kEnumStatNames, "stress_heal_received_percent") == 29);  // combat/logic.cpp ValidateSubStat 的 case 29
static_assert(IndexOf(kEnumStatNames, "stress_dmg_received_percent") == 30);  // combat/logic.cpp ValidateSubStat 的 case 30
static_assert(IndexOf(kEnumStatNames, "hp_heal_amount") == 6);  // combat/logic.cpp ValidateSubStat 的 case 6
static_assert(IndexOf(kEnumStatNames, "hp_heal_percent") == 7);  // combat/logic.cpp ValidateSubStat 的 case 7
static_assert(IndexOf(kEnumStatNames, "hp_heal_received_percent") == 8);  // combat/logic.cpp ValidateSubStat 的 case 8
static_assert(IndexOf(kEnumStatNames, "hp_heal_amount_received") == 98);  // combat/logic.cpp ValidateSubStat 的 case 98
static_assert(std::bit_cast<float>(kDamageEpsilonBits) == 1e-6f);  // events/system.cpp kDamageEpsilon
static_assert(SRC_S == DD_SOURCE_F_SAVED && SRC_T == DD_SOURCE_F_TOWN_CLEAR && SRC_Y == DD_SOURCE_F_TRAY);
static_assert(DD_COMBAT_TEXT == kGameTextCap);

// ---- 表（combat/logic.cpp）----
constexpr const char* const kOld_kStatNames[] = { "status", "tag", "combat_stat_add", "combat_stat_multiply", "hp_dot_poison", "hp_dot_bleed", "hp_heal_amount", "hp_heal_percent", "hp_heal_received_percent", "stress_dmg_percent", "stress_heal_percent", "resistance", "poison_chance", "bleed_chance", "stun_chance", "move_chance", "debuff_chance", "resolve_check_percent", "resolve_xp_bonus_percent", "scouting_chance", "remove_negative_quirk_chance", "food_consumption_percent", "starving_damage_percent", "upgrade_discount", "party_surprise_chance", "monsters_surprise_chance", "riposte", "guarded", "damage_received_percent", "stress_heal_received_percent", "stress_dmg_received_percent", "vampire", "activity_side_effect_chance", "disable_combat_skill_attribute", "stress_dot", "shuffle_dot", "hp_dot_heal", "torchlight_burn_percent", "quirk_evolution_death_immune", "vampire_evolution_duration", "ambush_chance", "tag_blocked", "ignore_protection", "stealth", "guard_blocked", "crit_received_chance", "damage_reflect_percent", "shard_reward_percent", "shard_consume_percent", "cure_poison_chance", "cure_bleed_chance", "cure_poison_received_chance", "cure_bleed_received_chance", "hp_dot_poison_amount_percent", "hp_dot_poison_amount_received_percent", "hp_dot_poison_duration_percent", "hp_dot_poison_duration_received_percent", "hp_dot_bleed_amount_percent", "hp_dot_bleed_amount_received_percent", "hp_dot_bleed_duration_percent", "hp_dot_bleed_duration_received_percent", "stress_dot_amount_percent", "stress_dot_amount_received_percent", "stress_dot_duration_percent", "stress_dot_duration_received_percent", "shuffle_dot_duration_percent", "shuffle_dot_duration_received_percent", "hp_heal_dot_amount_percent", "hp_heal_dot_amount_received_percent", "hp_heal_dot_duration_percent", "hp_heal_dot_duration_received_percent", "torch_decrease_percent", "torch_increase_percent", "ignore_stealth", "transfer_buff_from_attacker_chance", "transfer_debuff_from_attacker_chance", "random_target_friendly_chance", "random_target_attack_chance", "guard_duration_percent", "guard_duration_received_percent", "stress_on_miss", "stress_from_idle_in_town", "quirk_tag_evolution_duration", "deathblow_chance", "heartattack_stress_heal_percent", "riposte_duration_percent", "buff_duration_percent", "ignore_guard", "hp_dot_burn", "hp_dot_burn_amount_percent", "hp_dot_burn_amount_received_percent", "wildfire_apply_adjacent_targets", "hp_dot_burn_decay_percent", "cure_burn_chance", "cure_burn_received_chance", "effect_chance", "crit_dmg_multiply", "crit_dmg_add", "hp_heal_amount_received", "heal_crit_chance_add", "heal_crit_chance_multiply", "disable_combat_skill" };
static_assert(SameNames(kEnumStatNames, kOld_kStatNames));
constexpr const char* const kOld_kSourceNames[] = { "notspecified", "skill", "instantSkill", "affliction", "virtue", "quirk", "trinket", "trinket_set", "campingskill", "deathsdoor", "town", "curio", "stun", "guard", "riposte", "item", "disease", "companion", "deathsdoor_recovery", "deathsdoor_recovery_heart_attack", "quest_failure", "completed_darkest_dungeon_quest_party_hero", "town_event", "vampire", "never_again", "battle_modifier", "district", "torchsettings", "flashback_start", "flashback_result", "crit", "trinket_additional_effect", "quest_modifier", "last_hero" };
static_assert(SameNames(kEnumSourceNames, kOld_kSourceNames));
constexpr const char* const kOld_kDurationNames[] = { "combat_end", "quest_complete", "quest_not_complete", "quest_end", "activity_end", "idle_start_town_visit", "till_removed", "none", "before_turn", "after_turn", "after_round" };
static_assert(SameNames(kEnumDurationNames, kOld_kDurationNames));
constexpr const char* const kOld_kRuleNames[] = { "always", "monsterSize", "lightbelow", "lightabove", "hpbelow", "hpabove", "stress_below", "stress_above", "afflicted", "virtued", "meleeonly", "rangedonly", "firstroundonly", "actorStatus", "monsterType", "at_deaths_door", "in_rank", "target_in_rank", "has_buff", "in_camp", "in_dungeon", "in_room", "in_corridor", "walking_backwards", "in_activity", "riposte", "skill", "in_mode", "actor_base_class", "is_vampire", "target_is_vampire", "has_quirk", "monster_type_count_min", "is_guarded", "target_is_guarded", "is_stealthed", "target_is_stealthed", "has_item_id", "has_item_type", "is_guarding", "target_is_guarding", "torch_setting", "is_actor_status", "target_hpbelow", "target_hpabove", "attacking_monster_type", "no_trinkets", "activated_first", "activated_last", "ondeathsdoorenter", "negative_quirk_above", "quest_uses_remaining_above", "target_is_burning", "any_is_burning", "is_target_corpse" };
static_assert(SameNames(kEnumRuleNames, kOld_kRuleNames));
constexpr const char* const kOld_kCombatStatNames[] = { "damage_low", "damage_high", "defense_rating", "protection_rating", "speed_rating", "attack_rating", "crit_chance", "max_hp", "riposte_on_hit_chance", "riposte_on_miss_chance" };
static_assert(SameNames(kSubStatCombatNames, kOld_kCombatStatNames));
constexpr const char* const kOld_kResistanceNames[] = { "stun", "move", "poison", "bleed", "disease", "debuff", "death_blow", "trap" };
static_assert(SameNames(kSubStatResistanceNames, kOld_kResistanceNames));
constexpr const char* const kOld_kSideEffectNames[] = { "go_missing", "add_quirk", "apply_buff", "add_currency", "remove_currency", "activity_lock", "add_trinket", "remove_trinket" };
static_assert(SameNames(kSubStatSideEffectNames, kOld_kSideEffectNames));
constexpr const char* const kOld_kSkillAttributeNames[] = { "stress", "stun", "heal", "bleed", "poison", "tag", "guard", "move", "disease", "buff", "debuff", "daze" };
static_assert(SameNames(kSubStatSkillAttributeNames, kOld_kSkillAttributeNames));
constexpr const char* const kOld_kStressSourceNames[] = { "unkown", "debug", "activity", "town_idle", "quest_fail", "pass", "hunger", "effect", "hero_crit", "monster_crit", "hero_killing_blow", "hero_death", "mode", "control", "capture", "camping_relieve_stress", "camping_eat", "camping_skill", "deaths_door", "tile", "retreat", "curio", "flashback", "town_event" };
static_assert(SameNames(kSubStatStressSourceNames, kOld_kStressSourceNames));
constexpr const char* const kOld_kHealSourceNames[] = { "hero_skill", "hero_skill_multi_target", "monster_skill", "monster_skill_multi_target", "camp_skill", "camp_skill_multi_target", "effect", "act_out", "curio", "eat", "companion", "debug", "dot", "flashback", "damage_heal" };
static_assert(SameNames(kSubStatHealSourceNames, kOld_kHealSourceNames));
constexpr const char* const kOld_kBuiltinStatus[] = { "stunned", "tagged", "poisoned", "bleeding", "burning", "virtued", "afflicted", "dazed" };
static_assert(SameNames(kBuiltinStatusNames, kOld_kBuiltinStatus));
constexpr const char* const kOld_kTargets[] = { "performer", "target", "performer_group", "performer_group_other", "target_group", "target_group_other", "target_enemy_group", "target_enemy_rank", "target_enemy_random", "target_hero_random", "this_trinket", "global", "performer_adjacent_random", "performer_adjacent_front", "performer_adjacent_behind", "performer_adjacents", "target_adjacent_random", "target_adjacent_front", "target_adjacent_behind", "target_adjacents", "other_trinket" };
static_assert(SameNames(kEffectTargetsNames, kOld_kTargets));
constexpr std::uint32_t kOld_kFlags[] = { DD_SOURCE_F_TOWN_CLEAR, DD_SOURCE_F_SAVED | DD_SOURCE_F_TOWN_CLEAR | DD_SOURCE_F_TRAY, DD_SOURCE_F_SAVED | DD_SOURCE_F_TOWN_CLEAR, 0, DD_SOURCE_F_TOWN_CLEAR, 0, 0, 0, DD_SOURCE_F_SAVED | DD_SOURCE_F_TOWN_CLEAR | DD_SOURCE_F_TRAY, DD_SOURCE_F_TOWN_CLEAR, DD_SOURCE_F_SAVED | DD_SOURCE_F_TOWN_CLEAR | DD_SOURCE_F_TRAY, DD_SOURCE_F_SAVED | DD_SOURCE_F_TOWN_CLEAR | DD_SOURCE_F_TRAY, DD_SOURCE_F_SAVED | DD_SOURCE_F_TOWN_CLEAR | DD_SOURCE_F_TRAY, DD_SOURCE_F_SAVED | DD_SOURCE_F_TOWN_CLEAR, DD_SOURCE_F_SAVED | DD_SOURCE_F_TOWN_CLEAR, DD_SOURCE_F_SAVED | DD_SOURCE_F_TOWN_CLEAR | DD_SOURCE_F_TRAY, 0, DD_SOURCE_F_TRAY, DD_SOURCE_F_TOWN_CLEAR, DD_SOURCE_F_TOWN_CLEAR, DD_SOURCE_F_SAVED | DD_SOURCE_F_TOWN_CLEAR, DD_SOURCE_F_TOWN_CLEAR, DD_SOURCE_F_SAVED, DD_SOURCE_F_SAVED, DD_SOURCE_F_TOWN_CLEAR, DD_SOURCE_F_SAVED | DD_SOURCE_F_TOWN_CLEAR, DD_SOURCE_F_SAVED, DD_SOURCE_F_TOWN_CLEAR, DD_SOURCE_F_SAVED | DD_SOURCE_F_TOWN_CLEAR, DD_SOURCE_F_SAVED | DD_SOURCE_F_TOWN_CLEAR | DD_SOURCE_F_TRAY, DD_SOURCE_F_SAVED | DD_SOURCE_F_TOWN_CLEAR | DD_SOURCE_F_TRAY, DD_SOURCE_F_SAVED | DD_SOURCE_F_TOWN_CLEAR | DD_SOURCE_F_TRAY, DD_SOURCE_F_TOWN_CLEAR, DD_SOURCE_F_TOWN_CLEAR };
static_assert(SameValues(kSourceFlagsValues, kOld_kFlags));

}  // namespace layout_golden
