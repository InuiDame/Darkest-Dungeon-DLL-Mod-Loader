# buff.* 主题挂在 buff 表的容器操作上，删除原因按调用栈判断

游戏的 91 个原生事件里没有 buff 增删，`buff.*` 是 DDCore 的语义主题，都只读，有订阅时才装 hook：

- `buff.add`：`Buff::Group::Add`（`5C4F50`）返回后派发，组 guid 已知，在槽位 13 随后的刷新之前。
- `buff.remove`：`Buff::Group::erase`（`5C4FF0`）入口派发，先复制字段（这时条目指针有效）。全部删除路径都汇到这里。
- `buff.tick`：`BuffDurationUpkeep`（`47AAD0`）入口记下各条目的 duration，出口对比，每条变了的派发一次；原函数里减到 0 被删的条目在 `buff.remove`（`expired`）之前补派发一次 1→0 的 tick。
- 只在存档里派发。`CampaignLoad` 之前 `load_campaign` 线程上 `Hero::Load` 重新加 quirk、饰品等的增删不派发，插件在 `campaign.load` 里枚举；读档恢复的条目（`5C4D10` 内联了 push_back，不经过 `5C4F50`）天然不派发。进存档之后加载线程上的增删复制字段，按 [ADR 0009](0009-off-thread-events-deferred.md) 延迟到主线程派发，actor 指针为空，带 `actor_kind`、`actor_id`。
- `buff.remove` 的 `reason`：用 `RtlCaptureStackBackTrace` 取调用栈，沿 `.pdata` 找每一帧的函数起始地址，取最内层认得的游戏函数（到期 `47AAD0`、刷新 `480540`、回城 `587F90`、技能即时 buff `47A7B0`、饰品 `481A40`、灼烧衰减 `5C4C10`、按 guid `5C4660`、`ResetStats` `4849F0`、effect `4C5050`、`429750`）；都没有而经过槽位 14 时是 `predicate`，否则是 `other`；插件的 `buff_remove` 里谓词命中的条目是 `plugin`。这些函数只用特征码定位，不 hook。

插件的标签（（actor，guid）→（插件，标签））也不为它常驻 hook：组 guid 在一个 actor 上只增不减，查询、删除之前把表里已经不在的 guid 的记录清掉，效果和删除时清理一样；actor 不再能解析时，在战斗或探索换了之后的帧边界清掉。

## 考虑过的方案

- 用槽位 13、14 的虚表 hook：漏掉直接调 `5C4F50` 的添加（叠灼烧 `47CF00`、怪物同伴 `5FFE4D`），以及直接调 `5C46D0` 的十几条删除路径。
- `buff.tick` 用 `UpdateRoundBuffDurations`（`775460`）加 `UpdateRoundBuffs`：漏掉走廊格子（`774CC0` → `60A350`，尾调用 `47AAD0`）、撤退和战斗结束的递减。
- 加载线程上的增删一律丢弃：进存档之后 `load_raid`、`load_town` 线程上的增删（静态上可能）插件就看不到了。
- 删除原因按「哪个 hook 先看到」区分：要在每条删除路径上都装 hook。栈回溯一次会话约 177 次（探针），开销可以忽略。
- 标签在 `5C4FF0` 里清理：没有 `buff.*` 订阅时也得常驻这个 hook。

## 后果

- `reason` 是推断：认得的函数之外的删除是 `predicate` 或 `other`，`item` 是推测。P4 最终场景里战斗结束、撤退时游戏删掉 `remove_on_battle_complete` 的 buff 都报 `predicate`。
- 同一次 `47AAD0` 里其他条目的 tick 在原函数返回之后，晚于减到 0 的那些 remove。订阅 `buff.tick` 时也装 `5C4FF0` 的 hook。
- 延迟的 `buff.*` 在所有游戏内运行里都没有出现过（P4 探针、`test_combat` 场景、最终场景），这条路径只有离线测试。

2026-11-15 敲定（P4，经四轮审核）。
