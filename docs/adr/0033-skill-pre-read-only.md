# skill.pre 不开放写入

`skill.pre`/`skill.post` 的 `skill` 字段经 `combat` 服务转成技能句柄（执行者，技能 id，等级），只读字段有 id、等级、`.type`、友方标志、effect 名字列表。`skill.pre` 整个主题只读：字段只读，事件也不带 `DD_EVENT_F_WRITABLE`。P3 留给 P4 的「`skill.pre` 的写入」不做。

原因：DDCore 的 hook 在 `Actor_DoAttackSkill`（`5BE500`）和 `Actor_DoSkillFriendly`（`5BF440`），位于执行的总入口 `5B9B50` 分派之后。到这里时执行者已经把原技能记进 `+0x8E0/+0x8E8`，原技能的即时 buff 已经施加，敌我和路径也已选定；换成别的技能，即时 buff 和记下的技能对不上，敌方技能换成友方的还会进错函数。改目标时，目标列表是按原技能的站位掩码选出来的，只换第一个会和其余目标不一致。

## 考虑过的方案

- 把可写的 `skill.pre` 挪到 `5B9B50` 入口：游戏还没动手，一个 hook 覆盖攻击、友方、移动、teleport 四条路径。但冷却和 `per_battle_limit` 的计数在哪里记、按指针还是按 id 记还没查，换技能之前要先查清，留到 P5。
- 留在 `5BE500`/`5BF440`，只允许同一职业表里、友方标志相同、都不是 `move`/`teleport` 的技能互换：`+0x8E0` 和即时 buff 仍然对不上。

## 后果

- P3 场景 Mod `p3d_core` 的判定改成 `skill.pre` 不可写。
- 技能自带的 effect 在技能演出时才施加，晚于 `skill.post`（P4 最终场景：Smite 的 `skill.pre` 之后约 1.9 秒，`shield_self_gain_5` 才经 `effect.pre` 到达）。要把 effect 归到技能上的 Mod 不能靠 `skill.pre` 到 `skill.post` 之间的窗口。

2026-11-15 敲定（P4，经四轮审核）。
