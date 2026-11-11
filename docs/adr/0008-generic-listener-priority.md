# 通用 listener 在 Event 中排最前，在 Query 中排最后

`DDCore` 给每种原生事件注册一个通用 listener，排序键是 `listener+8` 的 int，游戏用有符号比较升序排列（`3CCA10`）。

- **Event 用 `INT_MIN`。** 游戏有回调会返回 `1` 中止后续派发（如 `HeroStartBarkEvent`、`ResidentOccupiesActivityEvent`），排在它们后面就收不到事件。
- **Query 用 `INT_MAX`。** 订阅者排在最后，能看到游戏累加后的结果，并对结果有最终决定权。

`DDCore` 在注册后和每种事件首次派发时遍历列表核对实际顺序，写进 `DDCore.log` 的 `排序确认`、`排序汇总`。

## 后果

Query 排最后的代价是：游戏的 Query 回调如果返回 `1`，订阅者就收不到。P2 场景验收中，有游戏回调的 3 个 Query 都收到了，其余的还没有逐一核对。
