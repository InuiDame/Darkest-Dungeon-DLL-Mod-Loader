# 队伍、怪物、战斗状态的只读枚举先于 P5 的 dd.raid 提供

Mod 要在「没有事件」的时候知道队伍、怪物和场合：开战后 5～9 秒才有第一次出手，走廊里几乎没有带角色的事件，读进战斗中的存档时游戏不派发 `battle.start`，游戏也没有战斗的 app 状态。靠事件里见过的角色去猜会漏，而且漏得没有痕迹（第三轮审核的三条阻断都出在这里）。所以 `game` 服务先给三个只读函数：

- `party_heroes`：当前队伍，`Raid::Current+8` 的 `vector<Hero*>`，按表的顺序；城镇里成功、0 个。
- `battle_monsters`：当前战斗的怪物，`Battle+0x38`，只算虚表是 Monster 的；尸体（同 guid、职业换成 `corpse_*`）也在表里，调用方自己排除。
- `battle_state`：战斗状态 `Battle+0x288`，0 是没有战斗；读进战斗中的存档时读档线程已经把它写成非零。

每个指针都按 [ADR 0024](0024-actor-handles-are-values.md) 核对成员关系（英雄在主名册或备用名册里、怪物在当前战斗里）才给句柄。只在主线程、存档里、没有加载线程在跑时可用，不装 hook、不订阅事件，读内存在 SEH 下进行。DDLua 对应 `dd.actor.party()`、`dd.actor.monsters()`、`dd.actor.in_battle()`，加载中返回 `nil` 和以「加载中」开头的原因。

范围刻意小：不给站位、不给 Raid 的其他字段（火把、地图、任务），不给写操作，也不派发新事件（「战斗开始」由 Mod 轮询 `in_battle` 得到）。P5 的 `dd.raid` 在这上面扩展，这三个函数的语义保持不变，或在 P5 的 API 整理里一起改。

## 考虑过的方案

- DDCore 在读档恢复的战斗里补派发 `battle.start`：要在 `7730C0` 的恢复路径上装 hook，还改变已有主题的语义，留给 P5 决定。
- 继续靠事件猜，在场景文档里要求用户「等每个怪物都出过手」：流程变长，用户操作稍有偏差就要重跑。

## 后果

- `Raid+8` 的读法有静态依据（`4C41F0` 的 `target_hero_random` 分支、`Battle::HeroesRerank` 的写回）和离线核对（`tools/check_actor_layout.py`），P4 最终场景里三个场景 Mod 靠它在走廊和战斗里认出了队伍 [运行]。队伍按站位排序只有静态依据。
- 战斗销毁中途（`5A5BE0` 逐个删除怪物、最后才清空表）调用 `battle_monsters` 时表里可能有已释放的指针，靠虚表核对和 SEH 挡住，没有在游戏里验证。
- 旧的 DDCore 会让整个 `dd.actor` 不可用（DDLua 要求 `game` 服务的大小覆盖新函数），按 [ADR 0018](0018-no-version-compat-until-p7.md) 开发期不处理。

2026-11-15 敲定（P4，经第四轮审核）。
