# shield_lite：用 Lua 复刻 DDShield 的护盾

DDLua 的示例 Mod（标题 `DDLua Example Shield Lite`，id `shield_lite`）。护盾的规则照 DDShield（`DD.Plugins/DDShield`，GPLv3）：护盾值和上限、按 effect 名字获得、吸收最终伤害、每回合再生和衰减、`shielded` 规则。代码是按规则重新写的，没有复制 DDShield 的源码。

DDShield.dll 和 DDCore 不能同时装（[ADR 0017](../../../docs/adr/0017-no-legacy-plugin-compat.md)）。effect 名字和 DDShield 相同，DDShield 的数据 Mod 和本 Mod 一起勾选时，同名 effect 后加载的覆盖先加载的，内容一样，不影响结算。

## 文件

| 文件 | 内容 |
| --- | --- |
| `effects/shield_lite.effects.darkest` | 护盾 effect：本身不改属性，游戏施加时交给 Lua 结算 |
| `lua/main.lua` | 规则、吸收、回合、清理、载体 Buff、日志 |
| `lua/config.lua` | 基础上限和初始值、每回合增减、职业的上限、名字里看不出规则的 effect |
| `lua/jsonline.lua` | 日志用的一行 JSON |

## 用法

在技能里引用 effect（写在你的职业文件 `heroes/<职业>/<职业>.info.darkest` 里）：

```
combat_skill: .id "my_skill" ... .effect "Stun 1" "shield_self_gain_5"
```

名字规则，数字可以换（用新数字时在 `effects/shield_lite.effects.darkest` 里照样加一行，Lua 不用改）：

| 名字 | 作用 |
| --- | --- |
| `shield_gain_N`、`shield_heal_N` | 护盾加 N |
| `shield_gain_pct_N`、`shield_heal_pct_N` | 护盾加上限的 N% |
| `shield_max_N`、`shield_max_pct_N` | 上限加 N、加 N% |
| `shield_damage_N`、`shield_damage_pct_N` | 护盾扣 N、扣上限的 N% |
| `shield_full`、`shield_break` | 加满、清零 |
| `shield_regen_N_T`、`shield_decay_N_T` | 之后 T 个回合，每回合开始时加、扣 N（T 省略时是 1） |
| `shield_pierce_N`、`shield_pierce` | 这个执行者对目标的下一次命中有 N%（不写是 100%）无视护盾，写在攻击技能上、作用于被攻击的目标 |
| `shield_self_…` | 同上，作用于执行者自己 |

名字里看不出规则的（例如 `shield_set_10`）写进 `config.lua` 的 `specs`。`specs` 的 `op` 和 DDShield 的 `DDShield.effects.json` 一样，另外认 `heal`（同 `add`）和 `clear`（同 `break`）。

每回合：第 2 回合起，每回合开始时（游戏结算完回合的 buff 之后）先清掉没用掉的穿透，再加减 `regen_per_round`、`decay_per_round` 和 effect 给的再生、衰减。第 1 回合不结算：游戏只在回合号大于 1 时结算回合的 buff，DDShield 挂在那后面。

生命 ≤ 0 时护盾清零（每帧检查，受到伤害时也先查一次），和 DDShield 每次画血条时的做法相同：在死门上的英雄没有护盾，在死门上获得的护盾下一帧就清掉，护盾挡不住死门上的那一击。

吸收：护盾吸收 `damage.final` 的最终伤害（护体、格挡之后，扣血之前），吸收量 = min(护盾, 最终伤害 × (1 − 穿透%))。全部吸收时 DDCore 按格挡处理，游戏显示 Blocked、不扣血；部分吸收时 DDCore 先把吸收的部分补回生命，生命损失 = 最终伤害 − 吸收量。游戏自己已经格挡时护盾不消耗。

`shielded` 规则：数据层的 buff 写

```json
{ "id": "bonus_vs_shielded", "stat_type": "combat_stat_multiply", "stat_sub_type": "damage_low", "amount": 0.33,
  "rule_type": "actorStatus", "is_false_rule": false, "rule_data": { "float": 0, "string": "shielded" } }
```

`is_actor_status` 看自己有没有护盾，`actorStatus` 看对方；护盾大于 0 算有。规则回调在有护盾时返回 `true`，没有护盾、在城镇里、没有要检查的角色时返回 `nil`（交还游戏原来的判定），这样结果和 DDShield 一样：DDShield 只接管游戏前置检查通过之后「状态是不是 shielded」那一步，前置检查不过（城镇里没有 Raid、`actorStatus` 没有对方）时规则是假、不按 `is_false_rule` 取反，通过了而没有护盾时是假、再取反；游戏对认不出的 `shielded` 原本就是这样判的。

别的 Mod 读护盾值：`dd.import("shield_lite").get(actor)` 返回 `{ cur, max }` 或 `nil`（manifest 里把 `shield_lite` 写进 `dependencies`）。

## 和 DDShield 的差异

- **护盾条不复刻**：DDShield 在血条上方画一条护盾条、悬停有提示。这里改成一条载体 Buff（脚本 Buff `shield_lite`，`protection_rating +0`，来源 skill）：有护盾时角色身上有这一条，数值写在日志里。来源 skill 的「进 tray」标志为真，P4 场景（2026-11-15，`docs/10-p4-scenario.md` 第 6 步）里用户目测到有护盾时头顶 tray 里多出这条载体的图标；数值是 0 的 buff 归 buff 组还是 debuff 组（按 `4A9C80` 的正负判定）没有分辨。护盾的数值界面上看不到，只在日志里。自定义图标和提示文字不在 P4 范围。
- **上限的来源**：DDShield 读英雄 info 文件的 `shield: .max .start` 块，这里改成 `config.lua` 的 `classes` 表（按职业 id）。`DDShield.ini` 的 `defaultMax`、`startFlat`、`startHpRatio`、`maxHpRatio`、`regenPerRound`、`decayPerRound` 对应 `config.lua` 的同名项。
- **buff 带来的护盾不支持**：DDShield 读 `shared/buffs/*.buffs.json` 里 buff 的 `shield_max`、`shield`、`shield_regen` 等自定义字段（buff 在身上时生效）。这里没有做，上限只来自配置和 effect。
- **治疗不同步到护盾**：DDShield 的 `mirrorHeal` 没有做。随附的 `DDShield.ini` 里它是 0（不同步）；代码里的默认值是 1.0（`config.h`），没有 ini 时 DDShield 的治疗会同步加护盾，这时两边不同。
- **初始化的时机**：DDShield 在角色的血条第一次绘制时初始化（职业有初始护盾时这时就有）；这里在角色第一次出现在战斗事件（回合开始、技能、伤害）或 effect 里时初始化。
- **清理**：DDShield 在血条不再绘制 2.5 秒后丢弃（英雄在走廊里血条照画，护盾会留到下一场）；这里在战斗结束（`battle.end`）和回城时清掉全部护盾，载体 Buff 也带 `remove_on_battle_complete`。
- **读档**：DDShield 的护盾只在内存里，读档就没了；这里护盾值写在载体 Buff 的状态里随存档保存（P4c 的旁路），读档后第一次用到这个角色时从载体恢复。穿透不保存（只对一次命中有效）。读档后在 raid 核对之前就碰到的怪物，护盾从零开始（载体的状态那时还没恢复）。战斗结束、回城清掉护盾之后到下一场战斗开始，不从载体恢复（游戏删载体可能晚于 `battle.end`）。

和 DDShield 相同、容易以为不同的几处：死门上没有护盾（见上）；城镇里 `shielded` 不成立（规则交还游戏原来的判定）；第 1 回合不做回合结算；`heal`、`clear` 两个 op。DDCore 的 `damage.final` 在死门上部分吸收时不补生命（游戏这时本来就不扣血），全部吸收按格挡处理；shield_lite 在死门上没有护盾，碰不到这两种情况。

## 日志

每次初始化、获得、吸收、命中结算、回合变化（第 1 回合写一行 `skip`）、穿透清掉、生命 ≤ 0 时清零、清理、载体增删、存档时写进载体、从载体恢复、`shielded` 的结果变化（`result` 是 `true` 或 `"original"`），都写一行 `SHIELD {...}`（JSON，`before`、`after` 是完整的状态）。`tools/p4_scenario_check.py` 按这些行独立重放 DDShield 的规则，逐次核对。`config.lua` 的 `log = false` 关掉。
