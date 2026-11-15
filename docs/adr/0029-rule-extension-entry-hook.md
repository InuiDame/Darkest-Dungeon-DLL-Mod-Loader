# 规则扩展复用 is_actor_status / actorStatus，在 479CD0 入口接管，可以交还原函数

数据层的 buff 写 `rule_type` 为 `is_actor_status`（42，检查自己）或 `actorStatus`（13，检查 target），`rule_data.string` 填脚本规则名。DDCore 在 `IsRuleActive`（`479CD0`）入口 hook，有规则注册时才装：规则类型是 13/42、`rule_data.string` 的哈希（`h = h*53 + c`，Instance 里已经算好）命中注册表时调用回调，否则直接转原函数。回调返回三种结果（`DDRuleResult`）：成立、不成立（DDCore 再按 `is_false_rule` 取反后返回）、交还原函数（`DD_RULE_ORIGINAL`）。不在主线程、PvP 中、回调崩溃过或已停用、`actorStatus` 没有 target 时也转原函数。原函数对认不出的字符串结果为假，再由游戏按 `is_false_rule` 取反；城镇里规则 42 在前置检查（要 Raid 和区域）就返回假、不取反。所以「交还」和没有 Lua Mod 时完全一样。

游戏在 13/42 分支里自己判断的 8 个状态名（`stunned`、`tagged`、`poisoned`、`bleeding`、`burning`、`virtued`、`afflicted`、`dazed`），以及和它们哈希相同的名字，不能注册，否则原版数据里所有这类判定都改由插件决定。

DDLua：`dd.buff.rule(name, fn)`，`fn` 返回 `true`/`false`，返回 `nil` 表示有意交还；返回别的值、出错、超预算（单独的 `rule_budget_ms`，默认 2 毫秒）、停用中也都交还，连续出错或超预算后停用、下一帧撤销。回调在 buff 表遍历中运行，只读，写操作和各种注册都报错（[ADR 0025](0025-combat-write-timing.md)）。每个 Mod 每帧的规则和脚本 Buff 回调合计受 `combat_frame_ms`（默认 10 毫秒）约束，超出后这一帧余下的判定交还原函数。

## 考虑过的方案

- 像 DDShield 一样在 `479F41` 打 cave：在前置检查之后，城镇里的 42 号规则收不到，还要自己处理分支中途的寄存器。入口 hook 让脚本规则在城镇里也能生效，想和原版一样时返回 `nil`。
- 新的 `rule_type` 名字：游戏存成 55，每次判定写一条日志，恒为假。
- 回调只返回真假（P4b 的第一版）：出错时只能报「不成立」再由 DDCore 取反，带 `is_false_rule` 的规则在城镇里结果和没有 Mod 时不同（审核 C-Y6）。
- 每次判定都进 Lua：判定约每秒近千次，99.9% 来自 `479BA0` 收集生效 buff，另有 1340 次在加载线程上（探针）。所以先用规则类型和哈希筛，命中的才进 Lua。

## 后果

- 规则名在 DDCore 里全局唯一（数据文件里写的就是名字本身），Mod 之间要自己避开。
- 判定在加载线程上一律交还原函数，读档期间脚本规则不生效。
- 刷新时 `remove_if_not_active` 的谓词（`486CC0`）也经过 `479CD0`，脚本规则的结果决定条目留不留。P4 最终场景在城镇、走廊、战斗里都按 Lua 的结果留下或删掉了条目。
- 命中次数、耗时和单帧峰值有统计，退出存档时写进 `DDCore.log`；最终场景的单帧峰值是 1.4 毫秒（14 次判定）。

2026-11-15 敲定（P4，经四轮审核）。
