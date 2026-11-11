# 事件覆盖表

由 `tools/event_coverage.py` 从 `test_events` 的日志生成，字段含义见 `knowledge/dd1/events.md`。
“运行中收到”表示本次日志里该主题的回调至少被调用过一次；没收到不代表主题不可用，只是这次流程没有触发。
日志来源：test_events.log（D:\Program Files (x86)\Steam\steamapps\common\DarkestDungeon_Dev\DDModLoader\log\test_events.log）

## 原生事件（native.*）

| 主题 | 可用 | 字段数 | 可写/可取消 | 运行中收到 | 派发线程 | 次数/延迟 | 备注 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `native.EvaluateAchievementsEvent` | 是 | 2 | 只读 | 是 | 主线程 | 2/1 |  |
| `native.InitedAchievementsEvent` | 是 | 2 | 只读 | 是 | 线程 26956（非主线程） | 1/1 |  |
| `native.SaveAchievementStateEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.AwardAchievementEvent` | 否 | 2 | 只读 | 否 |  |  | 没有 Listener<T const> 类型描述符（游戏里没有派发点），无法用 __RTDynamicCast 伪造，不可用；不可用：没有 Listener<T const> 虚表：游戏没有对应的 Listener 类型描述符（类型没有派发点，无法用 __RTDynamicCast 伪造） |
| `native.MailingListSignUpEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.MailingListReminderEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.MailingListReminderSeenEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.LocalizationAvailableEvent` | 是 | 2 | 只读 | 是 | 主线程 | 1/0 |  |
| `native.AreaTileContentsChangedEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.AreaTileKnowledgeEvent` | 是 | 2 | 只读 | 是 | 主线程 | 7/0 |  |
| `native.AreaKnowledgeEvent` | 是 | 2 | 只读 | 是 | 线程 34712（非主线程） | 6/2 |  |
| `native.ScoutingEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.ScoutingCurioEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.BattleAttackEvent` | 是 | 2 | 只读 | 是 | 主线程 | 9/0 |  |
| `native.TrinketTriggeredEvent` | 是 | 2 | 只读 | 是 | 主线程 | 40/0 |  |
| `native.CaptorBurnRedirectEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.BattleCreatedEvent` | 是 | 2 | 只读 | 是 | 主线程 | 1/0 |  |
| `native.BattleDestroyedEvent` | 是 | 2 | 只读 | 是 | 主线程 | 1/0 |  |
| `native.MonsterDeathEvent` | 是 | 9 | 只读 | 是 | 主线程 | 7/0 |  |
| `native.CaptureEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.ReleaseStartEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.ReleaseFinishEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.TeleportEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.MonsterBattleStageEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.DeathDamageEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.HeartAttackEvent` | 是 | 6 | 只读 | 否 |  |  |  |
| `native.ActorDazedEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.HeroDeathEvent` | 是 | 4 | 只读 | 否 |  |  |  |
| `native.HeroSurvivedDeathsDoorAttackEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.HeroSkillUpgradedEvent` | 是 | 4 | 只读 | 否 |  |  |  |
| `native.HeroWeaponAndArmourUpgradedEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.QuirkAppliedEvent` | 是 | 5 | 只读 | 是 | 主线程 | 4/0 |  |
| `native.HeroActOutStartEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.HeroActOutFinishEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.HeroAfflictionVirtueEvent` | 是 | 3 | 只读 | 否 |  |  |  |
| `native.HeroUsedItemEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.HeroReachedDeathsDoorEvent` | 是 | 6 | 只读 | 否 |  |  |  |
| `native.InventoryAddItemEvent` | 是 | 5 | 只读 | 是 | 线程 26956（非主线程） | 172/10 |  |
| `native.InventoryPreMergeEvent` | 是 | 4 | 只读 | 是 | 主线程 | 3/0 |  |
| `native.InventoryPostMergeEvent` | 是 | 4 | 只读 | 是 | 主线程 | 3/0 |  |
| `native.InventoryModifiedEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.LaunchCreditsEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.OptionsSavedEvent` | 是 | 2 | 只读 | 否 |  |  | 没有游戏监听者，伪造 COL 和虚表注册（见 knowledge/dd1/events.md） |
| `native.PlayFabConnectionInitializedEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.PlayFabReconnectedEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.PlayFabConnectionLostEvent` | 是 | 2 | 只读 | 否 |  |  | 没有游戏监听者，伪造虚表注册 |
| `native.PlayFabLocalizedDataAvailableEvent` | 是 | 2 | 只读 | 否 |  |  | 没有游戏监听者，伪造虚表注册 |
| `native.PlayFabFinishPostMatchDataUpdatesEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.ReceivedExternalTurnMetaDataInSimpleMPEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.ExternalSubsystemMultiplayerMessageReceivedEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.RaidStartEvent` | 是 | 3 | 只读 | 是 | 线程 34712（非主线程） | 1/1 |  |
| `native.PartyStepEvent` | 是 | 2 | 只读 | 是 | 主线程 | 6/0 |  |
| `native.RaidFinishResultsEvent` | 是 | 3 | 只读 | 是 | 主线程 | 1/0 |  |
| `native.RaidGoalsCompleteEvent` | 是 | 2 | 只读 | 是 | 主线程 | 1/0 |  |
| `native.RaidMapGenerated` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.PreRaidFinishResultsEvent` | 是 | 2 | 只读 | 是 | 主线程 | 1/0 |  |
| `native.TorchModifiedEvent` | 是 | 6 | 只读 | 是 | 主线程 | 3/0 |  |
| `native.PrisonerRescuedEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.QueueWaveLogicBarkEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.MaxPrestigeEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.RosterListHeroDDEffectEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.RosterListHeroDDEffectClearedEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.JournalPopupStartEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.HeroStartBarkEvent` | 是 | 2 | 只读 | 是 | 主线程 | 3/0 |  |
| `native.ActivityOpenedEvent` | 是 | 4 | 只读 | 否 |  |  |  |
| `native.ActivityFinishedEvent` | 是 | 3 | 只读 | 否 |  |  |  |
| `native.QuirkLockedEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.PositiveQuirkTreatedEvent` | 是 | 2 | 只读 | 否 |  |  | 没有游戏监听者，伪造虚表注册 |
| `native.NegativeQuirkTreatedEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.BuildingStateChangedEvent` | 是 | 4 | 只读 | 否 |  |  |  |
| `native.EstateCurrencyChangeEvent` | 是 | 4 | 只读 | 是 | 线程 26956（非主线程） | 13/9 |  |
| `native.HeroDismissedEvent` | 是 | 3 | 只读 | 否 |  |  |  |
| `native.BuildingUpgradedEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.RealmInventoryChangedEvent` | 是 | 2 | 只读 | 是 | 主线程 | 2/0 |  |
| `native.HeroRecruitedEvent` | 是 | 3 | 只读 | 否 |  |  |  |
| `native.TownEventOnStartTownVisitEvent` | 是 | 3 | 只读 | 是 | 主线程 | 1/0 |  |
| `native.PreOnStartTownVisitEvent` | 是 | 3 | 只读 | 是 | 主线程 | 1/0 |  |
| `native.PostOnStartTownVisitEvent` | 是 | 2 | 只读 | 是 | 主线程 | 1/0 | 没有游戏监听者，伪造虚表注册 |
| `native.DistrictBuildingBuiltEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.PrestigeUpdatedEvent` | 是 | 2 | 只读 | 否 |  |  | 没有游戏监听者，伪造虚表注册 |
| `native.PlayFabAchievementsUpdatedEvent` | 是 | 2 | 只读 | 否 |  |  | 没有游戏监听者，伪造虚表注册 |
| `native.PrestigeAndPlayFabAchievementsUpdatedEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.ArenaUnlocksUpdatedEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.TutorialPopupStartEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.TutorialPopupFinishEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.ResidentOccupiesActivityEvent` | 是 | 2 | 只读 | 是 | 主线程 | 2/0 |  |
| `native.ActivitySlotTakenEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.ActivityUnconfirmedSlotCountEvent` | 是 | 2 | 只读 | 否 |  |  |  |
| `native.UpdateHeroStressEvent` | 是 | 2 | 只读 | 是 | 主线程 | 104/0 |  |
| `native.RosterListRandomBarkEvent` | 是 | 2 | 只读 | 是 | 主线程 | 4/0 |  |
| `native.StageCoachBarkEvent` | 是 | 2 | 只读 | 否 |  |  |  |

## Query（query.*）

| 主题 | 可用 | 字段数 | 可写/可取消 | 运行中收到 | 派发线程 | 次数/延迟 | 备注 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `query.CampingPointBuffQuery` | 是 | 2 | 可写 | 否 |  |  |  |
| `query.CurioInteractionBuffQuery` | 是 | 5 | 可写 | 否 |  |  |  |
| `query.IsDisabledQuery` | 是 | 3 | 可写 | 否 |  |  |  |
| `query.InventoryExtraStackingLimitQuery` | 是 | 4 | 可写 | 否 |  |  |  |
| `query.InventoryReplacementEffectIdQuery` | 是 | 4 | 可写 | 否 |  |  |  |
| `query.LootDarknessBuffQuery` | 是 | 4 | 可写 | 否 |  |  |  |
| `query.TownGoldInterestBuffQuery` | 否 | 2 | 可写 | 否 |  |  | 游戏没有 Listener 类型描述符（没有派发点），不可用；不可用：没有 Listener<T> 虚表：游戏没有对应的 Listener 类型描述符（类型没有派发点，无法用 __RTDynamicCast 伪造） |
| `query.TownVisitIdleStressBuffQuery` | 是 | 3 | 可写 | 是 | 主线程 | 75/0 |  |
| `query.ProvisionSupplyBuffQuery` | 是 | 2 | 可写 | 是 | 主线程 | 1/0 |  |
| `query.ProvisionIsItemTooltipsEnabledQuery` | 是 | 2 | 可写 | 是 | 主线程 | 135/0 |  |
| `query.FindNonRosterHeroesQuery` | 是 | 2 | 可写 | 是 | 主线程 | 1/0 |  |

## 语义主题

| 主题 | 可用 | 字段数 | 可写/可取消 | 运行中收到 | 派发线程 | 次数/延迟 | 备注 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `damage.pre` | 是 | 9 | 可写、可取消 | 是 | 主线程 | 29/0 |  |
| `damage.post` | 是 | 12 | 只读 | 是 | 主线程 | 29/0 |  |
| `effect.pre` | 是 | 9 | 可写、可取消 | 是 | 主线程 | 393/0 |  |
| `effect.post` | 是 | 12 | 只读 | 是 | 主线程 | 393/0 |  |
| `skill.pre` | 是 | 5 | 可写 | 是 | 主线程 | 12/0 |  |
| `skill.post` | 是 | 5 | 只读 | 是 | 主线程 | 12/0 |  |
| `hit.pre` | 是 | 6 | 可写 | 是 | 主线程 | 14/0 |  |
| `hit.post` | 是 | 6 | 只读 | 是 | 主线程 | 14/0 |  |
| `battle.round.start` | 是 | 2 | 只读 | 是 | 主线程 | 2/0 | hook 在 UpdateRoundBuffs 的调用者 5A64B0，覆盖第一回合 |
| `battle.round.end` | 是 | 2 | 只读 | 是 | 主线程 | 1/0 | hook 在 5A64B0 入口，回合号大于 0 时触发；战斗最后一回合没有 end，用 battle.end 判定 |
| `battle.turn.start` | 是 | 1 | 只读 | 是 | 主线程 | 7/0 | 只在英雄回合（Combat_StartTurn 调用点）触发，只读。跳过回合暂不提供：要走 TurnResult=2 的路径，没有实测 |
| `battle.turn.end` | 是 | 2 | 只读 | 是 | 主线程 | 7/0 | Combat_StartTurn 返回后触发，不是英雄行动结束 |
| `stress.pre` | 是 | 5 | 可写 | 是 | 主线程 | 2/0 |  |
| `stress.post` | 是 | 7 | 只读 | 是 | 主线程 | 2/0 |  |
| `heal.pre` | 是 | 6 | 可写 | 是 | 主线程 | 27/0 |  |
| `heal.post` | 是 | 8 | 只读 | 是 | 主线程 | 27/0 |  |
| `app.tick` | 是 | 1 | 只读 | 是 | 主线程 | 19707/0 |  |
| `app.state` | 是 | 4 | 只读 | 是 | 主线程 | 18/0 |  |
| `campaign.load` | 是 | 2 | 只读 | 是 | 主线程 | 1/0 |  |
| `campaign.unload` | 是 | 1 | 只读 | 是 | 主线程 |  |  |
| `battle.start` | 是 | 0 | 只读 | 是 | 主线程 | 1/0 |  |
| `battle.end` | 是 | 0 | 只读 | 是 | 主线程 | 1/0 |  |
| `actor.deathsdoor` | 是 | 4 | 只读 | 否 |  |  |  |
| `actor.heartattack` | 是 | 4 | 只读 | 否 |  |  |  |
| `actor.death` | 是 | 8 | 只读 | 是 | 主线程 | 7/0 |  |
| `actor.affliction` | 是 | 1 | 只读 | 否 |  |  |  |
| `raid.start` | 是 | 1 | 只读 | 是 | 线程 34712（非主线程） | 1/1 |  |
| `raid.finish` | 是 | 1 | 只读 | 是 | 主线程 | 1/0 |  |
| `raid.step` | 是 | 0 | 只读 | 是 | 主线程 | 6/0 |  |
| `raid.torch` | 是 | 4 | 只读 | 是 | 主线程 | 3/0 |  |
| `town.enter` | 是 | 1 | 只读 | 是 | 主线程 | 1/0 | 开始一次城镇访问时触发（从地牢回城等），读城镇存档不经过这个流程 |
| `town.event` | 是 | 1 | 只读 | 是 | 主线程 | 1/0 |  |
| `town.building` | 是 | 3 | 只读 | 否 |  |  |  |
| `town.activity` | 是 | 2 | 只读 | 否 |  |  |  |
| `town.activity.finish` | 是 | 1 | 只读 | 否 |  |  |  |
| `hero.recruit` | 是 | 1 | 只读 | 否 |  |  |  |
| `hero.dismiss` | 是 | 1 | 只读 | 否 |  |  |  |
| `hero.quirk` | 是 | 3 | 只读 | 是 | 主线程 | 4/0 |  |
| `hero.skill` | 是 | 2 | 只读 | 否 |  |  |  |
| `inventory.add` | 是 | 3 | 只读 | 是 | 线程 26956（非主线程） | 172/10 |  |
| `inventory.modified` | 是 | 0 | 只读 | 否 |  |  | 静态上未找到 InventoryModifiedEvent 的派发点，可能从不派发 |
| `inventory.premerge` | 是 | 2 | 只读 | 是 | 主线程 | 3/0 |  |
| `inventory.postmerge` | 是 | 2 | 只读 | 是 | 主线程 | 3/0 |  |
| `estate.currency` | 是 | 2 | 只读 | 是 | 线程 26956（非主线程） | 13/9 |  |

## 后续阶段的主题（P2 未实现）

| 主题 | 所属阶段 |
| --- | --- |
| `buff.add` | P4 |
| `buff.remove` | P4 |
| `buff.tick` | P4 |
| `campaign.save` | P4 |
| `raid.curio` | P5 |
| `raid.trap` | P5 |
| `raid.camp` | P5 |
| `spine.event` | P6 |
| `spine.start` | P6 |
| `spine.complete` | P6 |
