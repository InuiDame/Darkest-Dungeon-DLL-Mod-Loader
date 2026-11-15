# 脚本 Buff 用游戏的 buff 条目做载体，状态按（actor，组 guid）

句柄沿用 [ADR 0024](0024-actor-handles-are-values.md) 的做法，不保存游戏指针：buff 是（actor 句柄，组 guid），组 guid 是条目唯一稳定的标识（删除时后面的条目前移，下标和指针都不稳定），随存档保存；effect 是名字（`Effect*` 只在一次进存档内有效，退出存档时游戏释放全部 effect）；技能是（执行者句柄，技能 id，等级），在执行者职业的技能表里解析。读出来的都是快照。

脚本 Buff = 一条运行时注册的 buff 定义（[ADR 0028](0028-runtime-definitions.md)）+ 按 buff id 注册的 Lua 回调（`on_apply`、`on_remove(reason)`、`on_tick(before, after)`、`on_damage`）+ 每条载体一张 Lua 状态表，键是（actor，组 guid）。tray、提示、`has_buff` 这类规则判定、持续时间、战斗结束和回城清理、存档都交给游戏：

- 状态的生命周期就是载体的生命周期：游戏或插件删掉载体时调用 `on_remove`（原因同 `buff.remove`，[ADR 0026](0026-buff-topics-on-container-ops.md)）并丢弃状态；会话进行中 actor 不再能解析时（怪物随战斗销毁、英雄被解雇），每秒的清理以 `gone` 调用。拆除（退出存档、重载拆除 Mod）时不调用 `on_remove`，状态直接丢弃。
- 状态必须能保存成 JSON（字符串键或从 1 开始的连续整数键，值是布尔、有限数、UTF-8 字符串、表）。英雄的经存档旁路随存档保存（[ADR 0031](0031-persist-sidecar-content-hash.md)），读档后按（hero id，组 guid）和身上实际的条目核对才恢复；怪物的在 raid 核对之后恢复。
- 游戏自己加上的同 id 载体（数据层的 effect 引用了这个 id）在 `buff.add` 里绑定空表；读档恢复的载体（不派发 `buff.add`）第一次用到时绑定空表，不调用 `on_apply`。
- 载体的来源决定游戏怎么对待它，要同时看来源表的三个标志：`+0x44` 进存档、`+0x46` 回城时从回来的队伍英雄身上清掉、`+0x48` 进 tray 的 buff/debuff 组。默认来源 skill（1）三个都为真。

## 考虑过的方案

- 纯 Lua 状态加自定义 tray 槽位（BuffIcon 的做法）：要 tray 的 5 个内联 hook，持久化、持续时间、战斗结束清理全靠自己，游戏的规则也看不到它。适合作图标层的补充，不适合作主方案。
- 状态按 buff id 或下标记：同 id 叠加时分不开，下标随删除变化。
- 把标签或状态编码进条目自己被存档的字段（例如某个来源下游戏不用的 `sourcedata`）：要证明游戏确实不用这个字段，而且装不下一张表。

## 后果

- 不在 P4 范围：自定义 tray 图标和提示文字（数据层的 buff 用 `buff_icon` 插件），跨回城保留的「永久」脚本 Buff（回城时游戏清掉来源表 `+0x46` 为真的 buff，要自动重新施加）。
- `before_turn`/`after_turn` 在走廊格子上也会递减，`on_tick` 在走廊里也会触发；回城清理只针对刚回来的队伍英雄，城镇里空闲英雄身上的载体只有 `idle_start_town_visit` 类型会递减。
- 回调次数随载体条数和规则判定次数增长，单次预算管不住总量：每个 Mod 有状态的载体最多 `max_per_mod` 条，每帧的规则和脚本 Buff 回调合计受 `combat_frame_ms` 约束。
- 退出存档时直接丢弃剩下的状态、不调用 `on_remove`（载体还在游戏存档里，状态已经在最后一次 `campaign.save` 写出）。DDCore 在派发 `CampaignUnload` 之前就把存档标成不活跃，这时英雄句柄都解析不了，所以拆除时也不按 `gone` 清扫；P4 最终场景里英雄的载体在退出存档时收到过 `gone`，F7 在 DDLua 一侧已修。

2026-11-15 敲定（P4，经四轮审核）。
