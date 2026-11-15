# Lua 脚本系统

`DDLua.dll` 是 `DDCore` 之上的一个 native 插件，只通过 SDK 调用下层（[ADR 0003](adr/0003-thin-proxy-core-layering.md)）。选型是 Lua 5.4.9 + sol2 3.5.0（[ADR 0012](adr/0012-lua54-sol2.md)），单 VM、每个 Mod 一张 `_ENV`（[ADR 0013](adr/0013-single-vm-per-mod-env.md)）。所有 Lua 代码都在主线程运行；加载线程上的原生事件由事件中枢复制后排队到主线程，只读、不能取消、指针字段为空（[ADR 0009](adr/0009-off-thread-events-deferred.md)）。

各需求的落地阶段：事件和动态注册 P3，Effect/Buff P4，回合外系统 P5，Spine P6。

## Mod 生命周期（P3）

```
<mod>/project.xml         游戏的 Mod 描述，存档按 <Title> 识别本地 Mod
<mod>/lua/manifest.json   id、版本、依赖、需要的 Lua API 版本
<mod>/lua/main.lua        入口，返回值是给别的 Mod 的导出
```

```json
{ "id": "my_mod", "version": "1.2.0", "api": 1, "dependencies": [ "lib_a", { "id": "lib_b", "version": "2.0" } ] }
```

- `id` 是字母、数字和 `_ . -`，1–63 字节，日志、依赖、`dd.import`、`dd.reload` 都用它；`version` 是 1 到 3 段数字；`api` 必须等于 DDLua 的 Lua API 版本（现在是 1）；`dependencies` 的 `version` 是最低版本。其余字段忽略。
- 启用范围跟随存档的 `applied_ugcs_1_0`（[ADR 0015](adr/0015-scripts-follow-save-mods.md)）。DDCore 在 `CampaignLoad` 读当前存档的启用列表、解析出目录（`game` 服务的 `mod_count` / `mod_at`），DDLua 把其中有 `lua/manifest.json` 的当作 Lua Mod。列表的位置、`{name, source}` 格式和目录解析见 `knowledge/dd1/data-and-mods.md`；本地 Mod 按 `project.xml` 的标题识别，不是目录名。
- 每次进入存档新建一个 Lua VM，没有 Lua Mod 时不建。加载顺序是依赖拓扑排序，同层按启用列表的顺序。id 重复、`api` 不符、缺依赖、依赖版本不够、循环依赖的 Mod 不加载；`main.lua` 出错的 Mod 加载失败，它出错前注册的东西全部撤销，依赖它的 Mod 不加载。其余照常。
- `CampaignUnload` 时按加载的逆序拆除：撤销订阅、定时器、协程，丢弃环境表和 `dd.state`，完整 GC 后关闭 VM，日志写出拆除前后的内存和分配器残留。`campaign.unload` 主题因此收不到；`campaign.load` 在 `main.lua` 里订阅能收到。直接退出游戏时不保证拆除。
- 开发模式（`DDLua.cfg` 的 `dev_mode = 1`，默认关）：`dd.reload(id)` 和文件监视（每 0.5 秒扫描 `<mod>/lua/`，连续两次一致才重载）。重载拆除目标和依赖它的 Mod，下一帧重新执行 `main.lua`，`dd.state` 保留。重载不重新解析 manifest：`lua/manifest.json` 变化也会触发文件监视的重载，但 `id`、`version`、`api`、`dependencies` 的改动要重新进存档才生效。`dev_global_dirs` 可以在 `AssetsReady` 全局加载指定目录（只用于开发自检，不是按存档加载的路径）。
- 配置在 `<游戏根目录>\DDModLoader\cfg\DDLua.cfg`（`key = value`），不存在时用默认值：`memory_limit_mb = 128`、`budget_ms = 50`、`load_budget_ms = 500`、`max_failures = 3`、`max_per_mod = 1000`、`frame_budget_ms = 20`（调度器每帧的总时间，见下）、`log_lines_per_sec = 200`（每个 Mod 每秒的日志行数）、`log_max_mb = 32`（一次进存档里和 Mod 有关的日志总量）、`teardown_budget_ms = 200`（一次拆除或重载里关闭协程的总时间）、`rule_budget_ms = 2`（`dd.buff.rule` 回调每次的预算，见需求三）、`combat_frame_ms = 10`（每个 Mod 每帧的规则和脚本 Buff 回调的总时间，见需求三）、`data_max_mb = 16`、`data_max_files = 256`（每个 Mod 的数据目录的总大小和条目数，文件和目录都算）、`store_max_kb = 256`（每个 Mod 每次随存档写出的 `dd.store` 和脚本 Buff 状态的上限，最大 1024，见需求三的「持久化」）、`dev_mode = 0`。日志写 `DDModLoader\log\DDLua.log`，每行以 `[Mod id]` 或会话名（`[存档#N]`）开头。
- LuaLS 类型声明在 `sdk/lua/`（`dd.lua` 手写，`dd_topics.lua` 由 `tools/gen_lua_types.py` 从 DDCore 的主题表生成，`sandbox.lua` 是沙箱里的 `os`、`debug` 子集）；Mod 模板在 `templates/lua/`，示例在 `examples/lua/`（`hello_lua`，P4 的 `shield_lite`）。

## 需求一：事件

Lua 的主题名和 SDK 一致：`native.<类名>`、`query.<类名>` 和语义主题，全部 151 个主题及字段见 `sdk/lua/dd_topics.lua`（运行覆盖见 [07-event-coverage.md](07-event-coverage.md)，派发和取消语义以 `sdk/include/dd/dd_api.h` 为准）。还没有实现的主题：

| 主题 | 阶段 |
| --- | --- |
| `raid.curio/trap/camp`（`InteractWithProp` 等） | P5 |
| `spine.event/start/complete` | P6 |

负载 `ev` 是 userdata，**只在回调执行期间有效**，回调返回后任何访问都报错：

- `ev.<字段>` 读主题的字段，读未知字段报错。指针字段里的 Hero、Monster 转成 actor 句柄；`effect.pre/post` 的 `effect` 是 effect 句柄（按 `effect_name`），`skill.pre/post` 的 `skill` 是技能句柄（`combat.skill_from_pointer`，不在执行者职业的技能表里时是 `nil`）；其余指针（raid、inventory、item……）是 `nil`。延迟事件的指针字段都是 `nil`，只有 `buff.*` 的 `actor` 按 `actor_kind`、`actor_id` 找回句柄。`buff.*` 里 Lua Mod 加的条目，`plugin` 是 `"DDLua"`，`tag` 是 `"<mod id>:<tag>"`。
- `.pre` 主题和 `query.*` 的可写字段用赋值写，类型必须一致：整数字段收整数（或值为整数的浮点数），数字字段收有限数，布尔字段收布尔。字符串和指针字段 Lua 不能写。`skill.pre` 从 P4 起全部只读（hook 点在游戏分派之后，见 [ADR 0033](adr/0033-skill-pre-read-only.md)；主题不可写，`ev.writable` 为 `false`），`damage.final` 的 `absorb` 可写。写只读字段、只读主题、延迟事件都报错。
- 数字字段在游戏里是 `float` 或 `int32`：超出这个宽度的值（绝对值超过约 3.4e38 的数，比如 `ev.damage = ev.damage * 1e38`；`int32` 以外的整数，比如 `ev.interest = 3e9`）由 DDCore 拒绝（`DD_EV_E_RANGE`），Lua 报「超出范围」，字段保持原值。native 插件经过同一个 `events` 服务，同样受限。
- 元信息 `ev.topic`、`ev.deferred`（加载线程上发生、排队到主线程派发的延迟事件：只读、不能取消）、`ev.cancelled`、`ev.writable`；主题自己的同名字段优先（`damage.post` 有 `cancelled` 字段）。
- `ev:cancel()` 只对 `damage.pre`（伤害按 0 结算）和 `effect.pre`（跳过这个目标）有效，别的主题和延迟事件报错。PvP 对局中 `.pre` 和 `query.*` 的订阅不派发（[ADR 0011](adr/0011-pvp-no-mutating-dispatch.md)）。

## 需求二：动态注册与响应（P3）

```lua
local sub = dd.on("damage.pre", function(ev)
    if ev.target and ev.target.is_hero and ev.target.hp < 5 then
        ev.damage = ev.damage * 0.5
    end
end, { priority = 10, scope = "battle", filter = { is_hero = true } })

dd.once("raid.finish", function(ev) dd.log.info("探索结算") end)
sub:off()
```

- `dd.on(topic, fn, opts)` / `dd.once(...)` 返回订阅对象（`sub:off()`、`sub:active()`、`sub.topic`）；主题不存在或不可用（离线版缺特征码）、actor 作用域的句柄已失效时返回 `nil, 原因`，参数和选项写错直接报错。`dd.once` 在调用回调之前撤销。`dd.off(sub)` 等同 `sub:off()`，`dd.subscriptions()` 是本 Mod 的订阅数。
- 每个 Lua 订阅对应一个 SDK 订阅，增删、排序、递归深度、PvP 屏蔽都沿用 `events` 服务：遍历快照，新增的下次派发生效、取消的立即停止；`priority` 升序（默认 0），和 native 插件的订阅交错排序。
- **作用域** `scope`：`"mod"`（默认，直到 Mod 被拆除）、`"campaign"`、`"raid"`、`"battle"`（由 DDCore 在退出存档、探索结算、战斗销毁时撤销），或一个 actor 句柄（怪物离开战斗或战斗结束、英雄离开名册或换存档时撤销：派发前检查，另外每秒检查一次）。
- **filter**：`{ 字段 = 值 }` 的等值比较，最多 16 项，在 C++ 里比较，不匹配不进入 Lua。指针字段只能用 actor 句柄比较。字段名、类型不对在订阅时报错。数字字段是精确相等比较：游戏里的 `float` 转成 double 以后，通常和 Lua 写的字面量（`0.1` 之类）不相等，数字字段一般不适合当 filter；整数、布尔、字符串、actor 句柄没有这个问题。
- 回调用保护调用执行，错误带 Mod id、文件、行号和 traceback 写日志；连续出错 `max_failures` 次（默认 3，成功一次清零）或超出执行预算时停用该订阅。每个 Mod 的订阅（含等待者、规则）、定时器、协程、脚本 Buff 的状态各自最多 `max_per_mod` 个。
- 撤销（`sub:off()`、`dd.off(sub)`、定时器和协程的 `:cancel()`）只能由所属 Mod 自己的代码进行。DDLua 从 C++ 进入 Lua 时记下这段代码代表哪个 Mod 执行（`main.lua`、事件回调、定时器、协程按注册它的 Mod）；别的 Mod 拿到句柄（比如通过 `dd.import`）去撤销会报错，`:active()`、`:status()` 这类查询不受限制。Mod 直接调用别的 Mod 导出的函数时不切换，那段代码代表调用者执行，所以提供方不能在导出函数里替依赖方撤销自己的订阅。
- DDLua 在加载完成、重载、拆除后写一行「订阅核对」，并排给出 DDLua 记录的订阅数和 DDCore 统计的数（`events.subscription_count`），不一致时用 ERROR。

协程把多步逻辑写成顺序代码，调度器每帧（lifecycle TICK，只在有 Lua VM 时订阅）恢复：

```lua
dd.spawn(function()
    dd.wait(1.5)
    local snap = dd.wait_event("battle.turn.start", nil, 30)  -- 字段快照，30 秒超时返回 nil
    if snap then dd.log.info(tostring(snap.actor)) end
end)
```

- `dd.spawn(fn, ...)` 立即运行到第一次挂起，返回协程句柄（`:cancel()`、`:status()` 为 `"running"`/`"waiting"`/`"dead"`、`:active()`）；协程里的错误写日志，不传给调用者。
- 不要用 `coroutine.resume`、`coroutine.close` 操作 `dd.spawn` 的协程（`coroutine.running()` 能拿到它）。手动恢复时协程体在调用者的栈上跑到下一次挂起，里面的 `dd.wait`、`dd.wait_event` 报错；被手动跑完、关掉的协程在下一帧清理（连同它的 `dd.wait_event` 等待者）。
- `dd.wait(秒)`、`dd.wait_event(topic[, filter[, 超时]])`、`coroutine.yield()`（等一帧）只能在 `dd.spawn` 的协程里直接调用；事件回调、定时器回调、`main.lua` 都在主线程栈上，调用会报错（协程不跨越 hook 边界）。`dd.wait_event` 等到事件后取一份字段快照（普通表，指针字段同样转句柄，`.pre` 是所有普通订阅之后的最终值），下一帧交给协程；要改 `.pre` 用 `dd.on`。
- `dd.after(秒, fn)`、`dd.every(秒, fn)` 返回定时器句柄（`:cancel()`、`:active()`），回调收到这个句柄；最早下一帧执行，`every` 不补发错过的次数。`every` 的回调连续出错 `max_failures` 次或超出预算时停用，`dd.after` 只执行一次，出错只写日志。`dd.time()` 是调度器的时钟。时间按真实时间（QPC），每帧检查一次。
- **每帧总时间**：调度器每帧的工作（排队的重载、定时器、协程恢复）共用 `frame_budget_ms`（默认 20 毫秒，真实时间）。每项开始前检查，用完以后剩下的顺延到下一帧，它们仍然到点，下一帧排在前面。到点的工作按 Mod 分组，各 Mod 轮流执行一项，每帧换一个 Mod 先开始，一个 Mod 再多的定时器也饿不死别的 Mod；同一个 Mod 里最久没执行的先执行。每帧至少执行一项，单项仍受 `budget_ms` 约束，所以一帧最多超出一项的预算。发生顺延时写 WARN 日志，第一次和之后每 10 秒最多一行（带上中间的累计）。事件回调不能顺延，见「沙箱和稳定性」的「预算没有覆盖的」。
- 拆除时协程不再恢复，挂起中的被关闭，`<close>` 变量照常执行（每个受 `budget_ms` 约束）。一次拆除（退出存档）或重载里关闭协程的总时间不超过 `teardown_budget_ms`（默认 200 毫秒）：用完以后剩下的协程不再关闭，直接丢弃引用，它们的 `<close>` 不运行（Lua 对被回收的挂起协程本来也不运行 `<close>`），写一行 WARN。所以一次拆除最多超出一个 `<close>` 的预算。

**actor 句柄**（`dd.actor`）：`dd.actor.hero(id)` 在当前存档的名册里找英雄，`dd.actor.monster(guid)` 在当前战斗里找怪物，找不到返回 `nil`；`dd.actor.is(v)`。句柄是值（kind、id、存档序号、探索序号、战斗序号、名册），不持有游戏指针，每次读字段重新解析。怪物的 guid 只在一场战斗内唯一（游戏每场战斗重新编号），怪物句柄靠 DDCore 维护的战斗序号区分：同一次探索里两场战斗的同 guid 怪物是两个句柄，`==` 为假、当表的键分开，上一场的句柄解析失败；阵亡的英雄留在名册里，句柄仍然有效，被解雇的英雄句柄失效。句柄可以跨回调、跨协程保存，同一个 actor 是同一个 userdata（可以当表的键），支持 `==` 和 `tostring`。字段只读：`kind`（`"hero"`/`"monster"`）、`id`、`is_hero`、`is_monster`、`hp`、`max_hp`、`stress`（只有英雄）、`class`（职业 id），`h:valid()`；失效后标识字段照常，读数字段为 `nil`。

**队伍、怪物、战斗状态的枚举**（P4 起，[ADR 0034](adr/0034-party-monster-enumeration.md)，P5 的 `dd.raid` 会在这上面扩展）：`dd.actor.party()` 返回当前探索队伍里英雄的句柄数组，顺序同游戏的队伍表（换位后游戏按站位重排，一般是第 1 位到第 4 位），城镇里是空表；`dd.actor.monsters()` 返回当前战斗的怪物，顺序同游戏的怪物表（按站位），不在战斗里是空表，怪物死后变成的尸体（同一个 guid，`class` 换成 corpse 开头的职业）也在表里，要排除时看 `class` 或 `hp`；`dd.actor.in_battle()` 返回是不是在战斗中和游戏的战斗状态（`Battle+0x288`，0 是没有战斗），读进战斗中的存档时游戏不派发 `battle.start`，这里照样是 `true`，撤退时状态先清零，这里马上是 `false`。三个函数都不用先在事件里见过角色；DDCore 读 `Raid::Current` 的队伍表和 `Battle` 的怪物表，每个指针都按 [ADR 0024](adr/0024-actor-handles-are-values.md) 核对成员关系（英雄在名册里、怪物在当前战斗里）才给句柄，不装 hook。加载线程在跑时（读档进地牢、回城的加载画面）三个函数都返回 `nil` 和以「加载中」开头的原因，不在存档里时返回 `nil` 和原因。场景 Mod 选战斗目标、判断「在走廊还是战斗里」都用它们，不再靠事件里见过的角色（第三轮审核 R3-B1、B3）。

**战斗序号和 `battle.start`**：DDCore 靠每帧和每次 actor 服务调用时的取样（战斗状态、怪物表、guid 计数）发现新战斗，不依赖 `battle.start`；`Battle::Create` 填好怪物表之后、置 `bs_create` 之前的回调里已经是新的战斗序号（flashback 战斗在这段时间里经 `5A45B2` 的循环派发 `heal.pre`/`heal.post`，普通战斗不执行这个循环）。有两条开战路径游戏不派发 `BattleCreatedEvent`，Lua 收不到 `battle.start` [静态]：

- 撤退后重进同一格等情况：探索的已保存战斗列表里有当前格子，`7730C0` 不调用 `Battle::Create`，经 `60E1C0` 从存档结构恢复怪物，直接把状态写成 1（`7734F0`）；
- `Battle::Create` 自己的读档分支（`5A40C6` 调用 `5ACD50` 后返回）。

这两种战斗同样有新的战斗序号，上一场的怪物句柄失效，即使是同一批怪物、同样的 guid。依赖 `battle.start` 建立每场状态的 Mod 会漏掉这些战斗，可以在 `battle.round.start` 里补上（静态上恢复的战斗照常推进回合，游戏里没有确认）。战斗结束都走同一个销毁函数，`battle.end` 照常派发。依据见 `knowledge/dd1/combat.md` 的「当前战斗」。

其余：`dd.id`、`dd.version`、`dd.api`、`dd.dev`；`dd.import(id)` 取已加载 Mod 的导出；`dd.state`；`dd.reload([id])`（开发模式）；`dd.stats()`（本 Mod 的订阅、等待者、定时器、协程数、加载次数，P4 起加上排队的写操作、规则、脚本 Buff 状态的条数）；`dd.log.debug/info/warn/error(...)`（`print` 等同 `info`）；`dd.fs`（见下）。完整签名见 `sdk/lua/dd.lua`。

`dd.fs` 有两个根目录：Mod 目录（只读，默认）和本 Mod 的数据目录 `<游戏根目录>\DDModLoader\data\<mod id>\`（读写，P4 起，[ADR 0032](adr/0032-fs-write-data-dir.md)）。`dd.fs.read(path[, opts])`、`dd.fs.exists(path[, opts])`、`dd.fs.list([path][, opts])` 的 `opts = { root = "data" }` 选数据目录；`dd.fs.write(path, text)`、`dd.fs.remove(path)` 只作用于数据目录。数据目录不分存档（分存档的状态用 `dd.store`），不写 Mod 目录（Workshop 的目录归 Steam 管）。

- 路径规则同只读接口：拒绝 `..`、绝对路径、盘符、冒号、通配符、Windows 保留的设备名（`CON`、`NUL.txt`、`COM1` 这类，读写都拒绝），父目录按需逐级创建，每一级取最终路径和根目录比较，联接和符号链接逃不出去。写入的路径最多 8 层（含文件名）：每一级父目录都要打开、取最终路径，层数越深越慢（审核 C-B2：400 层时一次写入 219 毫秒）。
- 原子写入：先写同目录的临时文件、`FlushFileBuffers`，再 `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` 替换；失败时删掉临时文件，原文件不变。
- 上限：每个 Mod 的数据目录总大小 `data_max_mb`（默认 16 MB）、条目数 `data_max_files`（默认 256，文件和目录都算，写入时要新建的父目录也算，不够时什么都不建），超出返回 `nil` 和原因。用量本会话第一次写入前扫描一次（16 层、10 万个条目为止，写入建不出更深的），之后按写入、删除增减。
- `dd.fs.remove` 删文件或空目录（不空时返回 `nil, "目录不是空的"`），数据目录本身不能删。
- 预算：`dd.fs` 的每次调用按打开、建目录的次数和数据量计入执行预算，并在返回前检查截止时间（循环里反复调用时每轮只有几条指令，等不到计数 hook；数据目录建不出来、每次都失败的写入也一样）。写盘本身不能中断，截止时间在写盘中途过去时，写完这一次再报超时。离线测试里循环写小文件、循环写 8 层的路径、循环 `exists`，都在预算（50 毫秒）后几毫秒内被打断。规则回调里调用写、删报错。还没有写过时读返回 `nil`，列根目录返回空表。

- `dd.import(id)`：导出是表时返回一张只读的浅代理，读、`pairs`、`#` 照常，赋值报错。代理按（使用方、提供方）缓存：同一个 Mod 每次拿到同一个代理（`dd.import(x) == dd.import(x)`，可以当表的键），不同的 Mod 各自一份，`rawset` 只改到自己那份；提供方重载后换新代理。不是表的导出（函数、数字等）原样返回。
  - 代理和原表的语义差异：`next(P)` 是 `nil`（遍历用 `pairs`）；`rawget`、`rawlen`、`rawequal` 看到的是空的代理；`getmetatable(P)` 是 `false`，`setmetatable(P, …)` 报错；代理传回提供方以后，和导出表本身也不相等。
  - 代理只保护导出表这一层。单 VM 里 Mod 之间直接共享对象（[ADR 0013](adr/0013-single-vm-per-mod-env.md)）：更深层的表、导出函数返回的对象都和提供方是同一个，依赖方能改。提供方写类时，类的元表要设 `__metatable`（`getmetatable(obj)` 拿不到元表），不要用导出表本身当元表（`M.__index = M` 加 `setmetatable({}, M)` 会让依赖方经 `getmetatable(P.new())` 拿到并改掉导出表），也不要从导出函数里返回导出表。
- 日志：`print`、`warn`、`dd.log.*` 一次最多 4096 字节，参数只追加到这个长度为止，不会先拼出完整的串。错误对象复制之前先截断（消息最多 2048 字节，连同 traceback 最多 8192 字节），失败原因里引用 Mod 给的主题名最多 200 字节。和 Mod 有关的日志（Mod 自己写的，加上 DDLua 替它写的回调错误、traceback、`dd.on`/`dd.wait_event` 的失败原因、协程被手动结束等）走同一个出口：一条最多 16 KB；每个 Mod 每秒最多 `log_lines_per_sec` 行（默认 200，多行文本按行数算），超出的丢弃，之后补一行「日志限速：有 N 行日志没有写入」；一次进存档里这些日志总共最多 `log_max_mb`（默认 32 MB），到了以后写一行 WARN，之后不再写入，拆除时写出丢弃的行数。会话自己的日志（加载、拆除、订阅核对）不限。`DDCore.log` 不在这些限制之内；Mod 隔帧订阅、撤销同一个主题时 DDCore 每帧装卸一次语义 hook 或 listener，这类 INFO 行按名字限速：每个 hook 或 listener 每 60 秒最多逐条写 8 行，其余合并成一行汇总（装上、卸下各几次）。

## 需求三：Effect 和 Buff（P4）

全部经 DDCore 的 `combat` 服务（`dd_api.h` 的 combat 一节，设计取舍见 [ADR 0025](adr/0025-combat-write-timing.md)–[0034](adr/0034-party-monster-enumeration.md)），DDLua 不碰游戏指针。完整签名见 `sdk/lua/dd.lua`。

```lua
local h = dd.actor.hero(12)
for _, b in ipairs(h:buffs{ source = "skill" }) do print(b.guid, b.id, b.stat, b.amount, b.tag, b.mine) end
local ok, guid = h:add_buff("my_buff", { duration = 3, tag = "mine" })
h:remove_buffs{ tag = "mine" }
local e = dd.effect.get("Stun 1")
local ok2, added = e:apply(h, target)   -- added.count、added[i] = { actor, guid, id }

dd.buff.define{ id = "my_shield", stat = "combat_stat_add", sub_stat = "protection_rating", amount = 0,
    on_apply = function(buff, state) state.shield = state.shield or 5 end,
    on_damage = function(buff, state, ev) local a = math.min(state.shield, ev.damage) state.shield = state.shield - a ev.damage = ev.damage - a end }
dd.effect.register("shield_gain_*", function(ev) ... end)
dd.buff.rule("my_rule", function(self, target, buff) return self.hp < 10 end)
```

**句柄和快照**（[ADR 0027](adr/0027-script-buff-carrier.md)）：buff 是（actor，组 guid），effect 是名字（effect 句柄只记名字，每次读字段重新查找，同名的句柄是同一个 userdata），技能是（执行者，技能 id，等级）。`actor:buffs([filter])`、`actor:buff(guid)`、`dd.buff.def(id)` 返回普通的表（快照），枚举字段是名字（`dd.buff.enum` 互查，source 去掉 `bsrc_` 前缀），名字认不出时是数值。快照里有 `source_flags`（进存档、回城清掉、进 tray）、`plugin`、`mod`、`tag`、`mine`（本 Mod 加的）、`script`（脚本 Buff）。`filter` 的 `id`、`source`、`tag`、`stat`、`mine` 同时满足。

**加载中读不到**（[ADR 0025](adr/0025-combat-write-timing.md)，审核 A-S3、R2A-S1）：加载线程在跑时（读档进地牢的 `as_raid_start`、回城的 `as_town_visit_start`，这段时间里定时器、协程、延迟事件照常运行），游戏可能正在搬动 buff 表、重建 Buff 库和 effect 表，DDCore 的读接口一律返回失败（`combat.read_ready` 为假）。DDLua 这时让 `actor:buffs`、`actor:buff`、`dd.buff.def`、`dd.buff.state`（还没绑定状态时）、`dd.effect.get` 返回 `nil` 和以「加载中」开头的原因，`e:valid()` 返回 `false` 和同样的原因，effect 句柄的字段读出 `nil`。`actor:buff(guid)` 条目不在时只返回 `nil`（没有第二个返回值），所以 `nil` 加上原因就是「暂时读不到」，不能当成条目已经没了；加载结束（`app.state` 离开 `as_raid_start`、`as_town_visit_start`）后再读。写操作在加载中同样被拒绝（原因是「加载线程正在运行」）。

**标签**：DDLua 对 DDCore 是一个插件，所有 Lua Mod 共用它的标签空间，所以交给 DDCore 的标签是 `<mod id>:<tag>`（不给 tag 时是 `<mod id>:`，用来认出是哪个 Mod 加的），两部分合计最多 63 字节。按标签筛选、删除只看本 Mod 的；按 id、来源、guid 删除不管是谁加的（和 SDK 一致）。

**写操作的返回约定**（[ADR 0025](adr/0025-combat-write-timing.md)；`add_buff`、`remove_buffs`、`e:apply`、`dd.buff.define`、`dd.effect.define`）：

- 参数写错（类型、未知选项、认不出的枚举名、超长）直接报错。
- 被拒绝返回 `nil, 原因`：不在存档里、加载中、PvP、id 或 effect 不存在、句柄解析失败、城镇里施加 effect、已经被游戏数据或别的 Mod 定义、本 Mod 排队超过 128 个（DDCore 给 DDLua 的 256 个所有 Mod 共用，一个 Mod 不能占满）。
- 接受返回 `true, 结果`。在安全点（`main.lua`、定时器、协程、`done` 回调）立即执行，结果是 guid（添加）、删掉的条数（删除，0 也算成功）、`{ count = n, [i] = { actor, guid, id } }`（施加：count 是所有目标加上的条数，数组只列执行者和目标身上新增的）、id（定义 buff）、定义的 effect 数（定义 effect）。在事件回调里（包括立即执行的写操作同步派发的 `buff.add` / `buff.remove` 等嵌套回调）排到下一个帧边界，结果是 `nil`，执行后调用 `opts.done(true, 结果)` 或 `opts.done(nil, 原因)`（施加的结果只有 `count`）。只有排队的操作调用 `done`；`done` 按订阅计入 `max_per_mod`，受执行预算约束；Mod 被拆除后不再调用。
- 写操作的 C 函数返回之前检查一次截止时间：嵌套的回调、游戏函数用掉的时间可能已经让调用者超时，循环里反复调用写操作时不用等 1000 条指令后的计数 hook。

**定义**（[ADR 0028](adr/0028-runtime-definitions.md)）：`dd.buff.define{...}` 只开放 POD 字段（`stat` 必填，`duration` 默认 -1，`duration_type` 默认 `till_removed`，`source` 默认 `skill`，`rule` 默认 `always`）；`sub_stat` 按 `stat` 校验，游戏认不出的返回 `nil, 原因`（`combat_stat_add` / `combat_stat_multiply` 要写 `speed_rating`、`protection_rating` 这类战斗属性名，`resistance` 要写 `stun`、`death_blow` 这类，完整的表见 `dd_api.h` 的 `buff_define`；审核 C-Y3：认不出时游戏有的在显示提示时弹 assert，有的越界加到相邻的属性上）；`dd.effect.define(text)` 用游戏自己的解析器，一段文本里可以有多个 `effect:` 块，运行时定义的 effect 不能被技能引用。文本先按游戏解析器的规则检查（细节见 `dd_api.h` 的 `effect_define`）：块从「标识符:」开始，到下一个冒号前面的标识符为止（`#` 注释和空白都算在块里，值里的 `x:y` 会开始一个新块），每块最多 1023 字节、都必须是 `effect:` 块；`.name` 按游戏的取法（块里最后一次出现的，`# .name x` 这样的注释也算）；`.target` 必须有、而且是游戏认的 21 种之一（缺少时游戏会弹 assert）；违反时报参数错误。Buff 库和 effect 表每次读档重建，定义要每次进存档重新注册（`main.lua` 里注册就行）。归属按 Mod 记录：游戏数据和别的 Mod 定义的 id、名字不能覆盖，同一个 Mod 重复定义时更新；游戏按 id、名字的哈希存，所以和别的 Mod 定义的哈希相同（`Ab` 和 `B-`）也算重复。排队的定义先登记，执行失败时撤销；排队的是「更新自己已有的定义」时失败只通知 `done`，原来登记的回调保留（DDCore 里原来的定义还在）；同一个新 id 排队定义了几次时，由最后一次的结果决定撤不撤销（几次都失败时撤销）。重新定义自己的 effect 时游戏复用原来的对象：标量字段被覆盖，列表字段（`.buff_ids`、`.kill_enemy_types`、`.summon_monsters`、`.set_monster_class_id(s)`）在原来的后面追加、不清空（审核 R2A-N4），要改列表就换一个名字。运行时定义没有个数上限，每条都在游戏里常驻到退出存档，不要每帧定义新名字。

**脚本 Effect**：`dd.effect.register(pattern, fn, opts)` 建在 `effect.pre` 上，是一个普通订阅（返回订阅对象，`priority`、`scope`、`filter` 同 `dd.on`），`pattern` 是精确名字或以 `*` 结尾的前缀，`effect_name` 的比较在 C++ 里做，不匹配的不进入 Lua。`ev.effect` 是 effect 句柄，`ev:cancel()` 跳过这个目标，回调里的写操作排队。

**脚本 Buff**（[ADR 0027](adr/0027-script-buff-carrier.md)）：`dd.buff.define` 带 `on_apply(buff, state)`、`on_remove(buff, state, reason)`、`on_tick(buff, state, before, after)`、`on_damage(buff, state, ev)` 时是脚本 Buff。载体是游戏的 buff 条目：tray、持续时间、`has_buff` 之类的判定、战斗结束和回城清理、存档都交给游戏。每条载体（actor，组 guid）一张状态表：

- 绑定：Lua 立即执行的 `add_buff` 在同步派发的 `buff.add` 里绑定 `opts.state`（hook 还没装上时在 `buff_add` 返回之后），`on_apply` 在 `add_buff` 返回之前调用；排队的在操作结果送达时绑定，然后 `on_apply`；游戏或别的插件加的同 id 载体（数据层的 effect 引用了这个 id）在 `buff.add` 里绑定空表；读档恢复的载体（不派发 `buff.add`）在第一次用到时（`dd.buff.state`、回调）绑定空表，不调用 `on_apply`。`opts.state` 只能给脚本 Buff，添加时检查能不能保存成 JSON（键全是字符串或从 1 开始的连续整数，值是布尔、有限数、UTF-8 字符串、表，嵌套最多 32 层），不能时报错并说明位置（`state.items[3]：不能保存 function`）。
- 回调：`on_remove` 在 `buff.remove` 里（删除之前，原因同主题的 `reason`），之后丢弃状态；`on_tick` 在 `buff.tick` 里（走廊格子上也会递减；减到 0 的那一次也有，在 `on_remove(..., "expired")` 之前，所以持续 1 回合的脚本 Buff 也会收到一次 `on_tick(buff, state, 1, 0)`）；`on_damage` 是这个 actor 的 `damage.pre`（载体按 buff 表的顺序各调用一次，`ev` 可写、可取消，PvP 中不派发）。连续出错 `max_failures` 次或超预算时停用这个回调。
- 丢弃：会话进行中 actor 不再能解析时（怪物随战斗销毁、英雄被解雇），每秒的清理调用 `on_remove(..., "gone")` 并丢弃。拆除时不调用任何 `on_remove`，状态直接丢弃：退出存档时载体还在游戏存档里，状态随存档旁路保存（见下面的「持久化」）；Mod 拆除（重载）时它的定义和状态一起丢弃。拆除时也不按 `gone` 清扫，因为 DDCore 在派发 `CampaignUnload` 之前已经把存档标成不活跃、英雄句柄都解析不了（P4 最终场景里英雄的载体因此收到过 `gone`，F7 已修）。
- 上限：每个 Mod 最多 `max_per_mod` 条载体有状态（回调按载体条数调用，随存档编码的也是这些）。到上限后 `add_buff` 返回 `nil, 原因`，游戏自己加上的载体不绑定状态、不调用回调，写 WARN（每个 Mod 最多 10 秒一次）。每个 Mod 每帧的回调总时间还受 `combat_frame_ms` 约束，见规则扩展。
- 内部订阅：有脚本 Buff 时 DDLua 订阅 `buff.add`、`buff.remove`，有 `on_tick` 时加 `buff.tick`，有 `on_damage` 时加 `damage.pre`，都是不进 Lua 的 C++ 回调，先按 id 过滤；没有脚本 Buff 时不订阅（hook 不装）。订阅核对里写作「内部订阅 N 个」。
- 不在 P4 范围：自定义 tray 图标和提示文字（用 `buff_icon` 插件的数据层做法）、跨回城保留的「永久」脚本 Buff（回城时游戏清掉可存档来源的 buff，要自动重新施加）。

**规则扩展**（[ADR 0029](adr/0029-rule-extension-entry-hook.md)）：`dd.buff.rule(name, fn)` 注册到 DDCore，返回规则句柄（`:off()`、`:active()`、`.name`）。数据层的 buff 写 `rule_type` 为 `is_actor_status`（检查自己）或 `actorStatus`（检查 target），`rule_data.string` 填规则名；`fn(self, target, buff)` 返回规则本身是否成立（`true` / `false`，`is_false_rule` 由 DDCore 取反），返回 `nil`（或不返回）表示这一次交还游戏原来的判定（`DD_RULE_ORIGINAL`，不算出错；例如城镇里想和没有 Mod 时完全一样，就在城镇里返回 `nil`）。`buff` 是 `{ id, rule, rule_string, rule_float, is_false_rule, subject }`（`subject` 是要检查的角色）。

- 回调在游戏遍历 buff 表时运行（主要是收集生效 buff），非常频繁：只读，写操作、`dd.on`/`dd.after`/`dd.spawn` 这类注册、`dd.fs.write`、`rule:off()` 都报错。读 actor 字段、buff 快照照常。
- 预算是单独的 `rule_budget_ms`（默认 2 毫秒：回调应该只读几个字段、查一张表，微秒级；死循环最多卡 2 毫秒加检查点的间隔就被打断）。返回 `nil` 是有意交还；返回别的非布尔值、出错、超预算、停用中也都交还游戏原来的判定（`DDRuleFn` 返回 `DD_RULE_ORIGINAL`，DDCore 转原函数，和没有 Lua Mod 时完全一样：认不出的规则字符串原始结果为假，再由游戏按 `is_false_rule` 取反，城镇里规则 42 在前置检查就返回假、不取反）；连续出错 `max_failures` 次或超预算一次后停用，下一帧向 DDCore 撤销（规则回调里 DDCore 不让撤销）。不在主线程、PvP 中时 DDCore 不调用回调。
- 每帧总量：每个 Mod 每帧的规则回调和脚本 Buff 回调（加上写操作的 `done`）合计不超过 `combat_frame_ms`（默认 10 毫秒）。规则按判定次数调用（一次刷新就判定表里每一条），单次 2 毫秒的预算管不住总量；超出后这一帧余下的规则交还游戏原来的判定、脚本 Buff 的回调跳过（`on_remove` 跳过时状态照样丢弃），写 WARN（每个 Mod 最多 10 秒一次，带累计跳过的次数），下一帧重新计。
- 规则名在 DDCore 里全局唯一（数据文件里写的是名字本身，不加 Mod 前缀），已经注册过（别的 Mod、native 插件或哈希相同）时返回 `nil, 原因`。

**示例：复刻 DDShield**（[examples/lua/shield_lite](../examples/lua/shield_lite/)）：一个完整的脚本 Effect + `damage.final` + 脚本 Buff + 规则的例子。数据层的 `effects/shield_lite.effects.darkest` 定义 `shield_gain_5` 这类空 effect，技能引用它们；`dd.effect.register("shield_*")` 按名字结算护盾，`damage.final` 写 `absorb` 吸收最终伤害（全部吸收时游戏显示 Blocked），`battle.round.start` 结算每回合的再生、衰减，`dd.buff.rule("shielded")` 让数据层的 buff 按护盾生效，有护盾的角色挂一条载体脚本 Buff（护盾值存在它的状态里，随存档保存），`battle.end` 清理。和 DDShield 的差异（上限来自配置表、没有护盾条等）写在它的 README 里。

**技能句柄**（[ADR 0033](adr/0033-skill-pre-read-only.md)）：`skill.pre/post` 的 `ev.skill`，字段 `id`、`level`、`type`、`friendly`、`effects`（effect 名字列表）、`performer`，只读；`skill.pre` 不开放改技能。

**持久化**（[ADR 0031](adr/0031-persist-sidecar-content-hash.md)）：`dd.store` 和脚本 Buff 的状态经 DDCore 的 `persist` 服务（[02-architecture.md](02-architecture.md) 的「persist 服务」）写进存档旁路 `<游戏根目录>\DDModLoader\save\<账号>\<设备名>\DDLua\<mod id>.ddsave`，每个 Lua Mod 一个文件，保留上一代 `.prev`。不放进 Steam 的 profile 目录，避免影响云存档。

```lua
local status, why, raid_pending = dd.store_status()   -- "loaded" / "prev" / "empty" / "mismatch" / "unavailable"
dd.store.runs = (dd.store.runs or 0) + 1
dd.on("campaign.save", function(ev) dd.store.last_save = ev.index end)  -- 回调里改的也会写进这次快照
```

- `dd.store`：每个 Lua Mod 一张表，`main.lua` 运行之前就从旁路读好（跨重载保留）；规则同脚本 Buff 的状态（键全是字符串或从 1 开始的连续整数，值是布尔、有限数、UTF-8 字符串、表）。可以整体赋值（`dd.store = {...}`），DDLua 写出时读 Mod 环境里的 `dd.store`。开发会话（`dev_global_dirs`）的 `dd.store` 只在内存里，状态是 `empty`、原因写明。Mod id 只差大小写的两个 Mod 都不加载（旁路文件、数据目录在 NTFS 上是同一个）。
- 写：游戏在当前存档位上每写完一组存档文件（任何 `persist.*.json`：城镇里写名册的一组、只写庄园或城镇文件的升级建筑和城镇事件，地牢里一次探索存档），DDCore 在下一个帧边界派发 `campaign.save`（Lua 照常 `dd.on`），所有回调都返回之后调用 DDLua 的保存回调，DDLua 这时才给每个加载成功的 Mod 取快照：`dd.store` 和这个 Mod 的脚本 Buff 状态（只要现在还能解析的 actor：名册里的英雄、当前战斗里的怪物），分段写成一个字节串（`lua/src/store.h`），交给 `persist.save`。编码时就按 `store_max_kb`（默认 256 KB）限制，`dd.store` 和状态合起来到上限就停下；超过上限或 `dd.store` 里有不能保存的值时，这次不写、写 ERROR 日志，旁路保留上一次写的（下次读档多半不一致）；某一条状态不能保存时只跳过这一条。走廊几乎每步、战斗每回合都存档，DDCore 的后台线程合并同一个文件的连续写入，写失败（别的程序占着文件）按退避重试几次。
- 读：`CampaignLoad` 时（游戏已经读完名册）DDLua 对每个 Mod 调用 `persist.load`，DDCore 比较这次读到的名册哈希和旁路记录的：`loaded`（当前一代一致）、`prev`（上一代一致）、`empty`（没有旁路；新战役复用以前用过的存档位时也是 `empty`）、`mismatch`（都不一致或文件损坏，原因写清楚，`dd.store` 是空表）、`unavailable`（观察 hook 没装上、没观察到名册读取等，DDCore 判断不了）。`mismatch` 不自动修复：`dd.store` 从空表开始，下一次存档照常写出新的；写之前 DDCore 把原来的两代改名为 `<mod id>.ddsave.mismatch-<时间>`、`<mod id>.ddsave.prev.mismatch-<时间>`（每个 Mod 留最近 2 份），要找回时手动改回名字。新战役时旧战役的旁路同样移开，改名为 `….oldcampaign-<时间>`。
- 脚本 Buff 的状态：英雄的按（hero id，组 guid）在全部 `main.lua` 执行完之后、`CampaignLoad` 回调返回之前导入（脚本 Buff 要先在 `main.lua` 里定义），和英雄身上实际的条目核对，id 和 guid 都对上才恢复，对不上的丢弃并写日志；导入不调用 `on_apply`。`main.lua` 里对读档恢复的载体调用 `dd.buff.state` 会先拿到一张空表，导入后换成旁路里的状态，所以要用状态的代码放在 `main.lua` 之后（回调、定时器里）。
- 地牢里的存档：游戏在 `CampaignLoad` 之前（状态 1）已经读过一次探索存档，DDCore 读档时就按名册和 raid 一起判断两代（raid 不一致的一代不用，可能因此是 `prev`）。探索存档里的当前战斗要到 `as_raid_start` 加载结束才恢复，所以 `dd.store_status()` 的原因里写着待核对、第三个返回值 `raid_pending` 为 `true`（等核对用它判断，不要在原因里找「raid」：读到上一代时原因里本来就可能写着 raid 不一致的那一代），加载结束后的帧边界拿 `load_raid` 读到的 raid 再确认一次：一致时恢复怪物的脚本 Buff 状态（按战斗里的 guid，战斗从存档恢复后 guid 不变），原因清掉；不一致时状态改成 `mismatch`、写 WARN，怪物的状态丢弃，`main.lua` 已经读到的 `dd.store` 收不回来，这次进存档里 DDCore 不再写这个 Mod 的旁路（免得下一次存档把来自不一致那一代的数据当成一致写回去），原来的两代保留。读档前没读到 raid 时只按名册判断，raid 只在这一步核对。退出存档前一直没核对的怪物状态丢弃。

## 需求四：回合外系统（P5）

| 模块 | 范围 |
| --- | --- |
| `dd.campaign` | 周数、难度、当前状态 |
| `dd.estate` | 货币、传家宝、建筑、升级 |
| `dd.roster` | 英雄列表、招募、解雇，`Hero` 的怪癖、疾病、压力、饰品、技能 |
| `dd.town` | 建筑活动、驿站、城镇事件 |
| `dd.raid` | 队伍、地图、房间、奇物、火把、扎营 |
| `dd.inventory` | 物品增删、堆叠 |
| `dd.quest` | 任务生成和目标 |
| `dd.screen` | 当前屏幕和状态机 |

先只读，再逐个开放写操作。写操作在 `app.tick` 这样的安全帧执行，事件回调里调用时自动排队。系统单例的线索在 `knowledge/dd1/campaign.md`。

## 需求五：Spine 动画（P6）

1. 控制现有角色：`actor.sprite:play("attack_sword", { loop = false, queue = true, mix = 0.1 })`，速度、暂停、查询当前动画。入口是 `SpineSprite` 的成员函数，`TargetFx::SetAnimation` 和 `Actor::PlayActionEffect` 是线索。
2. 骨骼级操作：换附件和皮肤，读骨骼世界坐标，接收动画事件和开始、结束回调（`spAnimationState` 的 listener）。结构体用 spine-c 2.1.x 的公开定义，并用游戏二进制校验。
3. 新建实例：加载 Mod 自带的 `.skel/.atlas/.png`，挂在 actor 或屏幕上。资源在专用线程加载，接口是异步的：`dd.spine.load(path, callback)`。

骨骼必须是 2.1.27 导出的二进制，这是游戏决定的。

## 沙箱和稳定性（P3）

沙箱的原则见 [ADR 0014](adr/0014-sandbox-no-raw-memory.md)，清单：

- 单个 VM，每个 Mod 一张 `_ENV`。开放 `base`（不含 `dofile`、`loadfile`；`load` 只收文本，默认环境是调用者的 `_ENV`；`collectgarbage` 只有 `"count"`、`"isrunning"`；`setmetatable` 拒绝带 `__gc` 的元表）、`coroutine`、`table`、`string`（没有 `dump`）、`math`、`utf8`，库表是每个 Mod 自己的副本，`math.random` 的随机数状态也是每个 Mod 一份（一个 Mod 调用 `math.randomseed` 不影响别的 Mod）；`getmetatable("")` 拿不到真实的 `string` 库。只有弱键的表（`__mode` 含 `k` 不含 `v`，ephemeron）按强引用表处理，键不会被回收（`lgc.c` 的补丁）：ephemeron 链会让 GC 原子阶段变成平方级（审核者的 4 万项链每轮 GC 卡 2.7 秒，不受预算约束）。`__mode = "v"`、`"kv"` 照常；`__mode` 每轮 GC 时从元表现读，所以在 GC 里改，不在 `setmetatable` 里拦。
- `os` 只留 `time/clock/date/difftime`（`date` 的时间限制在 0 到 3000 年）；`debug` 只留 `traceback`；不开放 `io`、`package`，由 `dd.fs` 只读访问 Mod 自己的目录、读写本 Mod 的数据目录（拒绝 `..`、绝对路径、盘符，用最终路径比较前缀，联接逃逸也挡住）。已知限制：硬链接挡不住，`GetFinalPathNameByHandle` 返回打开时用的那条路径，Mod 目录里指向目录外文件的硬链接能被读到。硬链接只能在玩家自己的机器上创建，Workshop 分发不了，所以不拦截。
- `require` 只在 Mod 的 `lua/` 下按模块名找 `.lua`（`a.b` 对应 `lua/a/b.lua` 或 `lua/a/b/init.lua`），每个 Mod 自己缓存；不加载 C 模块，没有 FFI 或裸内存接口。`print`、`warn` 写日志。
- 执行预算：每次从 C++ 进入 Lua（`main.lua`、回调、定时器、协程恢复、拆除和重载时的 `<close>`）是一个入口，按真实时间计（默认 50 毫秒，`main.lua` 500 毫秒），超出后报错，而且粘滞到回到 DDLua 为止，Mod 用 `pcall` 吞不掉；协程继承计数 hook。`main.lua` 的读取、语法分析和执行在同一个预算入口里，`require` 的模块同样（读文件每 1 MB、语法分析每 4 KB 检查一次）。超时由这几处发现：
  - 计数 hook（每 1000 条指令）。
  - 库函数长循环里的进度检查（`xmake/lua.lua` 的补丁）。原则是计数 hook 两次检查之间可以有几百次 C 调用，所以单次调用的工作量和数据量有关的循环都计步，只有固定上限的不计（捕获数 32、对齐 16）：模式匹配 `match()` 的每一步、`[...]` 字符集、`%b`、`%f`、回引用、`gsub` 的替换串、plain find 的扫描、`string.find` 对普通模式的检查（含 `\0` 分开的每一段）、`string.rep/byte/char/upper/lower/reverse/format`（含 `%q`）、`string.pack/unpack/packsize` 的格式串、`table.insert/remove/move/concat/sort/pack/unpack`、`utf8.len/codepoint/char/offset` 和 `utf8.codes` 的迭代函数、`next` 跳过空槽、`tonumber(s, base)`（含前后的空白）、`math.min/max` 的参数、`warn` 的参数。每个补丁在 `xmake/lua.lua` 里按锚点列出。
  - Lua 内核里没有 `lua_State` 的长操作在**做完之后**报告工作量：长串的比较（`==`、`<`、`<=`、表的键）、长串第一次当表键时的哈希、字符串转数字（`tonumber`、算术和库函数参数的自动转换，含任意长的空白、十六进制数字、前导零）。它们不能当场报错，超时后让**正在运行的线程**在下一条指令进入计数 hook 报错，C 里的下一次进度检查（比如 `table.sort` 的下一次比较）也立即报错。正在运行的线程由 Lua 补丁报告：`lua_resume`、`lua_closethread` 进出时通知 DDLua，协程开始运行、让出或结束后回到恢复方、嵌套的协程、`coroutine.close` 运行的 `<close>` 都对得上。
  - `load` 每交给语法分析 4 KB 源码检查一次（长注释、空白不分配内存、不经过 hook）。
  - 64 KB 以上的分配。
  - 入口结束时再查一次：入口在下一个检查点之前就结束、但已经过了截止时间的，同样算超预算（回调停用、`main.lua` 加载失败、协程终止），不会当作成功。
- 表的哈希：整数、浮点数、指针键的位置用带密钥的哈希算（`ltable.c` 的补丁，wyhash 的乘法折叠）：密钥每个进程随机生成一次，再和表自己的地址混合，每张表一个哈希函数，Mod 算不出落在同一个主位置的键（原来整数键直接对表大小取模，65537 个 `i*(2^17-1)` 的键一次扩容 3.8 秒）。扩容时直接插入、不先查找，即使有冲突也是线性的。字符串哈希的种子改用系统随机数（原来由时间和几个 Mod 能看到的地址算出）。语法分析的常量表也是 Lua 表，一并受益。语义差异：`pairs`/`next` 的顺序每次运行、每张表都不同（Lua 本来就不保证顺序）。
- 字符串比较（`<`、`<=`、`table.sort`）按字节比较，等于 C locale 下的 `strcoll`（DDLua 的 CRT 一直是 C locale，沙箱没有 `os.setlocale`），结果和原版相同，只是不再逐段调用 `strcoll`。
- 补丁对语义的影响用 Lua 官方测试集（`lua-5.4.9-tests`，非内部模式 `_U=true`，另跑了 `_soft` 为假的加长模式）核对：解释器是原版 `lua.c` 链接补丁后的库，DD 的回调用最简单的桩或保持 NULL，GC 阈值的回调另用很小的上限跑了一遍。28 个文件里只有两类按设计的差异：`gc.lua` 里弱键表的 3 处断言（只有弱键的表按强引用处理），`errors.lua` 的源名长度（`LUA_IDSIZE` 是 160）。没有补丁引入的回归。运行方式和预期结果见 `tests/lua_official/README.md`，改补丁或升级 Lua 后重跑。
- **预算能保证什么**（每个入口；预算 50 毫秒时，审核者三轮的全部用例都在预算加 100 毫秒以内结束，绝大多数在 50–56 毫秒）：
  - 纯 Lua 代码的任何循环、递归、元方法、`pcall` 吞错、协程（`coroutine.create/wrap`、`dd.spawn`、嵌套、来回让出）、`<close>`、`xpcall` 的消息处理函数，超时后最多再执行约 1000 条指令就报错，报错粘滞到入口结束。
  - 一条指令里调用的库函数：和数据量成正比的循环都在循环中检查（上面的清单），超时后在下一个检查点报错，检查点之间的工作量约 6 万个单位（一个单位大约一个字节或一次比较）。
  - 没有 `lua_State` 的内核操作：每次操作之后检查，跨过截止时间的那一次当场发现，下一条指令报错，不管它在主线程还是在哪一层协程里。
  - 表操作：Mod 构造不出哈希冲突，插入、查找接近常数时间，扩容和键的个数成正比；`__index` 链最多 2000 跳，每跳接近常数。
  - 入口用时超过预算就一定按超预算处理：要么当场报错，要么在入口结束时判定。
  - 内存到上限：一个入口里最多做一次没起作用的紧急 GC（做完仍然失败，或者回收后离上限不到 1/16），没起作用之后 1 秒内不论哪个入口都不再做，分配直接失败；那个入口自己分配了上限的 1/16 以上时例外（它结束后局部变量成了垃圾）。审核者的用例（128 MB 的活对象，回调里反复分配）三个回调一帧 186–242 毫秒，修复前 7150 毫秒。
  - 整段的 GC 停顿不算到无辜的回调头上：紧急 GC 和原子阶段和当前入口自己的分配量无关，DDLua 给它们计时（`lua_ddemergency_fn` 的前后、`lua_ddatomic_fn` 的前后），补到当前入口的截止时间上。每个入口最多补 max(一份预算, 这个入口里最长的一次停顿)：碰上一次 GC 的回调总能补全；在一个入口里反复触发 GC 的补不了更多，照样超时。所以入口最长是「预算 + 补的时间 + 截止前开始的最后一次停顿」，GC 本身受内存上限约束，不会无限。审核者的复现（别的 Mod 占着 8 万个挂起协程或 90 万个小表，小回调每次分配 200 个小表）：修复前 22 次紧急 GC 全部把小回调判超预算，`dd.every(0)` 第 978 帧停用、`dd.spawn` 的协程第 2392 帧终止；修复后 0 次判超预算，跑满 6000–30000 帧不停用。活对象约 98 MB 时在一个入口里反复分配 20 MB 的串（每次都撞上上限、紧急 GC 回收上一个）仍在 62–139 毫秒被打断。
  - 嵌套入口（P4b）：立即执行的写操作会同步派发 `buff.add` / `buff.remove`，订阅者的回调在写操作的 C 函数里再次进入 Lua；规则回调会嵌在立即执行的写操作里（添加、删除之后游戏刷新属性时判定规则；读 actor 字段走 `4C3F20`，是按掩码累加的纯函数，不判定规则）；开发会话的代码也可能同步派发到存档会话。`Vm::Call` 在已有入口里调用时是隔离的嵌套入口：保存外层的截止时间、粘滞标记、预算和 GC 补偿计数，按自己的预算（回调 `budget_ms`，规则 `rule_budget_ms`）重新计时，结束后恢复。外层超时（含截止时间在游戏函数里已经过去、还没有检查点发现的情况）不连累嵌套的回调；嵌套的回调超时只停用它自己。嵌套入口用掉的时间补到外层的截止时间上，上限同 GC 停顿：每个外层入口最多补 max(一份预算, 最长的一次嵌套)，在循环里反复触发嵌套回调的外层补不了更多，照样超时（写操作返回前的检查点会及时发现），外层入口最长是「预算 + 补的时间 + 截止前开始的最后一个写操作」。跨 VM 的嵌套补到入口栈栈顶的 VM 上。嵌套入口里的 GC 停顿补给嵌套入口自己（随嵌套的时间一起补给外层）。`dd.spawn` 的立即运行、`task:cancel()` 的 `<close>` 仍然沿用调用者的截止时间（是调用者自己的工作）。离线测试（`TestNestedEntries`）实测：外层自己 35 毫秒加嵌套 40 毫秒，两者都不超预算；嵌套回调死循环时只停用它；外层循环 100 次 `add_buff`、每次嵌套 25 毫秒，101–106 毫秒外层超时（去掉写操作返回前的检查点时 1356 毫秒）。
  - GC 的阈值在内存上限以下：下一轮 GC 的阈值（原版是活对象的 2 倍）不超过 max(上限的 7/8, 活对象 + 上限的 1/16)（`lgc.c` 的 `setpause` 补丁，`lua_ddgcthreshold_fn`）。活对象超过上限的一半时，原版的阈值到不了，增量 GC 不再启动，每次都是分配撞上上限、做一次完整的紧急 GC；现在活对象在上限的 15/16 以下时照常走增量 GC，余量（上限的 1/16）够一轮增量 GC 做完，不会变成连续不停的 GC。
- **预算不能保证什么**：
  - 一次不能中途打断的操作：超时只能在它做完以后发现，超出的幅度最多是一次操作。实测（预算 50 毫秒）：
    - `dd.fs.write` 每次同步刷盘（`FlushFileBuffers`、`MOVEFILE_WRITE_THROUGH`）：guest 的固态硬盘上一次小文件约 1.6 毫秒，机械硬盘上可能几十毫秒，单次写 `data_max_mb` 那么大的文件更久，上面「预算加 100 毫秒以内」对这类单次调用不成立（审核 R2A-N7）；
    - 字符串转数字一次扫完整个串（`strtod` 不能中途打断）：32 MB 的十六进制串 71 毫秒，前导零、`tonumber(s, 10)` 的空白 64–66 毫秒；
    - 长串比较是一次 `memcmp`，长串第一次当表键时的哈希一次算完（之后缓存），都和串长成正比；
    - 内存复制（`..`、`string.sub`、`table.concat` 的结果、产生等量输出的 `string.pack` 的 `c` 填充）由 64 KB 以上分配前的检查兜住，复制本身不能中断，单次最多复制内存上限那么多；
    - 一条指令搬动的栈值（可变参数、多返回值、`pcall` 移动参数）最多是栈上限（约 100 万个），40 万个参数反复 `pcall` 93 毫秒；
    - `load` 的语法分析不是线性的（深层嵌套加大量局部变量时，每个全局名要穿过所有层级查找），4 KB 一块时最多超出约 10 毫秒（审核者的 1 MB 深层嵌套源码 53–59 毫秒被打断）；`main.lua` 预算 200 毫秒时 1.1 MB 的同类文件 208 毫秒加载失败，游戏里 500 毫秒的预算 515 毫秒加载失败。
  - GC 停顿：GC 在任意一次分配时推进，不执行 Lua 指令，预算管不到，停顿发生在当时正在运行的入口里。整段的停顿（紧急 GC、原子阶段）补到那个入口的截止时间上（见「能保证」），游戏这一帧照样卡这么久；增量 GC 的每一小步按入口自己的分配量推进，不补。原子阶段要重新遍历这一轮里被改过的表（写屏障退回灰色的）和所有线程的栈，停顿和这些表的大小、线程的个数成正比。实测（内存上限 128 MB，预算 50 毫秒）：
    - 活对象不到上限的一半时（原版的阈值也在上限以下），停顿都在预算以内：约 68 MB 的堆（60 万个小表）一边分配一边改大表，每 256 次分配之间最长 10–11 毫秒；4 万个挂起的协程（43 MB）小回调最长 12 毫秒；
    - 活对象过半时（触发条件是整个 VM 的活对象，多个 Mod 合计，一个缓存大量数据或泄漏的 Mod 就够了）：修复前每次堆满就是一次完整的紧急 GC，8 万个挂起协程（86 MB）56–62 毫秒、90 万个小表（78 MB）96–99 毫秒，3 万次小回调里各 12 次、10 次，全部把当时的小回调判超预算、停用（停用的是当时恰好在分配的 Mod，通常不是占内存的那个）。现在这两种堆都走增量 GC，3 万次小回调里没有紧急 GC，最长的入口 19–22 毫秒、12–15 毫秒（其中原子阶段约 17 毫秒，补上），没有判超预算的。代价是 GC 更频繁：同样的 3 万次小回调总时间多 6%–32%；
    - 活对象超过上限的 15/16 时阈值又回到上限以上，仍是每次堆满做一次紧急 GC（128 MB 的活对象约 85–170 毫秒），按上面的规则限次、补到截止时间上：回调不会因此停用，但游戏卡这一下；
    - 分配很多的入口按自己的分配量分摊增量 GC 的工作，堆越大每个字节分摊得越多，这部分不补：8 万个挂起的协程时，一个入口分配 20 万个临时表最长 65–68 毫秒，两次运行里 6 次中分别有 0 次、1 次超预算（2 万个时最长 33–36 毫秒、完整 GC 9–12 毫秒；8 万个时完整 GC 58 毫秒）；
    - 弱键表按强引用处理以后，审核者的 4 万项 ephemeron 链只让别的 Mod 分配 20 万个临时表最长 17.6 毫秒、完整 GC 2.4 毫秒（修复前 2714 毫秒和 5424 毫秒）。
  - 预算按真实时间判定：主线程被抢占、同步写日志被杀毒软件卡住这类和 Mod 无关的停顿同样计入当时的入口，入口末尾的检查会把它判成超预算（没有实测；要防可以在入口末尾要求线程 CPU 时间也超过预算，没有做）。
  - 跨入口的总量：事件回调不能顺延，一帧里所有事件回调的总时间没有上限，只有单次预算，加上 `max_per_mod` 限制回调的数量；规则和脚本 Buff 的回调按判定次数、载体条数调用，`max_per_mod` 限制不了次数，由每个 Mod 每帧的 `combat_frame_ms` 截断（P4 审核 C-Y7）；调度器的工作有每帧总时间（`frame_budget_ms`，见「需求二」）；拆除和重载时每个 `<close>` 一份预算，总时间由 `teardown_budget_ms` 截断，最多超出一份预算。Mod 每帧都用满预算时游戏会变慢，但不会卡死。
  - 哈希密钥不是密码学强度：每张表的密钥由进程密钥和表的地址混合，Mod 原则上可以通过遍历顺序、计时花大量计算去推测；字符串哈希的种子是 32 位（Lua 的设计）。没有找到能在几帧之内做到的办法，不在保证之内。
  - DDCore 服务和游戏函数本身的耗时（原生代码）不计入预算。
  - 内存到上限后的那 1 秒里，Mod 放掉引用也要等冷却结束才会被紧急 GC 回收，期间的分配直接报内存错误。
- 内存：整个 VM 共享一个上限（默认 128 MB），超出时分配失败、报内存错误，不会崩溃。到上限时紧急 GC 的规则见上。
- 原生异常：Lua 代码执行期间发生原生异常（SEH，比如服务里的访问违例）时，DDLua 把这个会话的 VM 标记为损坏，写一条 ERROR 日志，之后本会话不再进入 Lua，退出存档时不碰 VM、直接丢弃（Lua 内存泄漏），下次进存档新建 VM。两种入口的处理不同：事件回调里的由 DDCore 的隔离接住，它撤销出事的那个订阅，并通过撤销通知（`DD_EVENT_END_CRASH`）告诉 DDLua；订阅已经不在时（`dd.once` 在调用前撤销，或回调里先 `off()` 了自己）DDCore 不发通知，所以 DDLua 的事件跳板另有一个 SEH 过滤器，在异常的第一遍搜索里先标记损坏，再交给 DDCore 照常处理，同一次派发里后面的订阅就不再进入 Lua；lifecycle 回调（`CampaignLoad` 执行 `main.lua`、`TICK` 里的定时器和协程、拆除时的 `<close>`）里的由 DDLua 自己接住（`plugin.cpp` 的 `RunMarked`），正常返回 DDCore，所以 DDCore 不会撤销 DDLua 的 lifecycle 订阅（它对崩溃的 lifecycle 回调一律撤销且不通知），损坏的会话只撤销 `TICK`，下次进存档照常加载。拆除本身再出原生异常时，会话对象整个泄漏、不再析构。展开时中间栈帧里的 C++ 对象不一定析构（按 `/EHsc` 编译），所以出事的会话不再触碰 VM 和它的状态。
  - 嵌套入口里的原生异常（P4b）：DDCore 的隔离只把栈展开到派发处，外层入口的 C 函数（比如 `add_buff`）还在 Lua 的栈帧里，而 `lua_State` 的调用信息、`errorJmp` 已经停在异常发生时。所以调用可能同步回到 Lua 的 SDK 函数（写操作、读 actor 的最大生命）之后，DDLua 检查 VM 是否已损坏，是的话抛一个 SEH 异常（`0xE0444C52`，`AfterReentry`），把外层入口也交给它自己的隔离（DDCore 的事件隔离、DDLua 的 `RunMarked`、操作回调和规则回调外面 DDLua 自己的 SEH 过滤器）。损坏的 VM 从入口栈里移除（没出栈的入口不能再占着栈顶，否则另一个 VM 的长串比较要等 1000 条指令后的计数 hook 才检查预算，离线测试里是 781 毫秒对 51 毫秒）。跨 VM 嵌套时只有出事的那个 VM 损坏，外层 VM 照常。
- 错误信息带 Mod id、文件和行号，不调用 Mod 的 `__tostring`。
- PvP 对局中会改结算的订阅不派发（[ADR 0011](adr/0011-pvp-no-mutating-dispatch.md)）。
- LuaLS 的类型声明在 `sdk/lua/`，Mod 的 `.luarc.json` 把它加进 `workspace.library`，并关掉 `io`、`os`、`debug`、`package` 的内置声明（见 `templates/lua/.luarc.json`）。
