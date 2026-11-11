# 每个 Lua 订阅对应一个 SDK 订阅，过滤放在 DDLua

`dd.on` 的每个订阅都是 DDLua 在 `events` 服务上的一个订阅。优先级和 native 插件交错、PvP 屏蔽、派发快照、`campaign`/`raid`/`battle` 作用域都直接沿用 DDCore 的语义。为此 SDK 加了两样：

- `DDEventOptions.on_end`：DDCore 因作用域结束或回调崩溃撤销订阅时通知插件，DDLua 据此释放 Lua 函数的引用。插件自己撤销时不通知。
- `subscription_count(plugin)`：诊断用，DDLua 在加载、重载、拆除后把自己的记录数和 DDCore 的统计并排写进日志，证明没有残留。

`filter` 在 DDLua 里做：订阅时按主题的字段表校验，派发时在 C++ 里做等值比较（整数、数字、布尔、字符串、actor 句柄），不匹配的不进入 Lua。actor 作用域由 DDLua 实现：怪物借 SDK 的 battle 作用域，英雄借 campaign，另外派发前和每秒检查句柄是否失效。

`dd.on` 遇到不存在或不可用的主题返回 `nil, 原因`，不报错，Mod 可以据此降级（例如离线版缺特征码）；参数写错直接报错。

## 考虑过的方案

- DDLua 每个主题只订阅一次，再在 Lua 一侧分发：Lua 订阅之间的优先级没法和 native 插件交错，作用域也要重写一遍。
- 过滤下沉到 DDCore：要扩 ABI，只有 DDLua 用得上。
- DDLua 自己监听 `BattleDestroyedEvent` 等实现作用域：重复 DDCore 的语义，还要多注册 listener，没有 Lua Mod 时也得装。

## 后果

Lua Mod 的订阅数直接计入 DDCore 的 hook 报告。语义主题的 hook 随 Lua 订阅按需安装和移除。

2026-11-11 敲定（P3）。
