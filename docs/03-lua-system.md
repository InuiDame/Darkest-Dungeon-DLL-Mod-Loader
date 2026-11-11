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
- 配置在 `<游戏根目录>\DDModLoader\cfg\DDLua.cfg`（`key = value`），不存在时用默认值：`memory_limit_mb = 128`、`budget_ms = 50`、`load_budget_ms = 500`、`max_failures = 3`、`max_per_mod = 1000`、`frame_budget_ms = 20`（调度器每帧的总时间，见下）、`log_lines_per_sec = 200`（每个 Mod 每秒的日志行数）、`log_max_mb = 32`（一次进存档里和 Mod 有关的日志总量）、`teardown_budget_ms = 200`（一次拆除或重载里关闭协程的总时间）、`dev_mode = 0`。日志写 `DDModLoader\log\DDLua.log`，每行以 `[Mod id]` 或会话名（`[存档#N]`）开头。
- LuaLS 类型声明在 `sdk/lua/`（`dd.lua` 手写，`dd_topics.lua` 由 `tools/gen_lua_types.py` 从 DDCore 的主题表生成，`sandbox.lua` 是沙箱里的 `os`、`debug` 子集）；Mod 模板在 `templates/lua/`，示例在 `examples/lua/hello_lua/`。

## 需求一：事件

Lua 的主题名和 SDK 一致：`native.<类名>`、`query.<类名>` 和语义主题，全部 146 个主题及字段见 `sdk/lua/dd_topics.lua`（运行覆盖见 [07-event-coverage.md](07-event-coverage.md)，派发和取消语义以 `sdk/include/dd/dd_api.h` 为准）。还没有实现的主题：

| 主题 | 阶段 |
| --- | --- |
| `buff.add/remove/tick`、`campaign.save` | P4 |
| `raid.curio/trap/camp`（`InteractWithProp` 等） | P5 |
| `spine.event/start/complete` | P6 |

负载 `ev` 是 userdata，**只在回调执行期间有效**，回调返回后任何访问都报错：

- `ev.<字段>` 读主题的字段，读未知字段报错。指针字段里的 Hero、Monster 转成 actor 句柄，其余指针（skill、effect、raid、inventory……）是 `nil`；延迟事件的指针字段都是 `nil`。
- `.pre` 主题和 `query.*` 的可写字段用赋值写，类型必须一致：整数字段收整数（或值为整数的浮点数），数字字段收有限数，布尔字段收布尔。字符串和指针字段 Lua 不能写（`skill.pre` 的 `skill`、`target` 是指针，所以 Lua 只能读 `skill.pre`）。写只读字段、只读主题、延迟事件都报错。
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
- 回调用保护调用执行，错误带 Mod id、文件、行号和 traceback 写日志；连续出错 `max_failures` 次（默认 3，成功一次清零）或超出执行预算时停用该订阅。每个 Mod 的订阅（含等待者）、定时器、协程各自最多 `max_per_mod` 个。
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

**战斗序号和 `battle.start`**：DDCore 靠每帧和每次 actor 服务调用时的取样（战斗状态、怪物表、guid 计数）发现新战斗，不依赖 `battle.start`；`Battle::Create` 填好怪物表之后、置 `bs_create` 之前的回调里已经是新的战斗序号（flashback 战斗在这段时间里经 `5A45B2` 的循环派发 `heal.pre`/`heal.post`，普通战斗不执行这个循环）。有两条开战路径游戏不派发 `BattleCreatedEvent`，Lua 收不到 `battle.start` [静态]：

- 撤退后重进同一格等情况：探索的已保存战斗列表里有当前格子，`7730C0` 不调用 `Battle::Create`，经 `60E1C0` 从存档结构恢复怪物，直接把状态写成 1（`7734F0`）；
- `Battle::Create` 自己的读档分支（`5A40C6` 调用 `5ACD50` 后返回）。

这两种战斗同样有新的战斗序号，上一场的怪物句柄失效，即使是同一批怪物、同样的 guid。依赖 `battle.start` 建立每场状态的 Mod 会漏掉这些战斗，可以在 `battle.round.start` 里补上（静态上恢复的战斗照常推进回合，游戏里没有确认）。战斗结束都走同一个销毁函数，`battle.end` 照常派发。依据见 `knowledge/dd1/combat.md` 的「当前战斗」。

其余：`dd.id`、`dd.version`、`dd.api`、`dd.dev`；`dd.import(id)` 取已加载 Mod 的导出；`dd.state`；`dd.reload([id])`（开发模式）；`dd.stats()`（本 Mod 的订阅、等待者、定时器、协程数和加载次数）；`dd.log.debug/info/warn/error(...)`（`print` 等同 `info`）；`dd.fs.read(path)`、`dd.fs.exists(path)`、`dd.fs.list([path])`（只读，限制在本 Mod 目录内）。完整签名见 `sdk/lua/dd.lua`。

- `dd.import(id)`：导出是表时返回一张只读的浅代理，读、`pairs`、`#` 照常，赋值报错。代理按（使用方、提供方）缓存：同一个 Mod 每次拿到同一个代理（`dd.import(x) == dd.import(x)`，可以当表的键），不同的 Mod 各自一份，`rawset` 只改到自己那份；提供方重载后换新代理。不是表的导出（函数、数字等）原样返回。
  - 代理和原表的语义差异：`next(P)` 是 `nil`（遍历用 `pairs`）；`rawget`、`rawlen`、`rawequal` 看到的是空的代理；`getmetatable(P)` 是 `false`，`setmetatable(P, …)` 报错；代理传回提供方以后，和导出表本身也不相等。
  - 代理只保护导出表这一层。单 VM 里 Mod 之间直接共享对象（[ADR 0013](adr/0013-single-vm-per-mod-env.md)）：更深层的表、导出函数返回的对象都和提供方是同一个，依赖方能改。提供方写类时，类的元表要设 `__metatable`（`getmetatable(obj)` 拿不到元表），不要用导出表本身当元表（`M.__index = M` 加 `setmetatable({}, M)` 会让依赖方经 `getmetatable(P.new())` 拿到并改掉导出表），也不要从导出函数里返回导出表。
- 日志：`print`、`warn`、`dd.log.*` 一次最多 4096 字节，参数只追加到这个长度为止，不会先拼出完整的串。错误对象复制之前先截断（消息最多 2048 字节，连同 traceback 最多 8192 字节），失败原因里引用 Mod 给的主题名最多 200 字节。和 Mod 有关的日志（Mod 自己写的，加上 DDLua 替它写的回调错误、traceback、`dd.on`/`dd.wait_event` 的失败原因、协程被手动结束等）走同一个出口：一条最多 16 KB；每个 Mod 每秒最多 `log_lines_per_sec` 行（默认 200，多行文本按行数算），超出的丢弃，之后补一行「日志限速：有 N 行日志没有写入」；一次进存档里这些日志总共最多 `log_max_mb`（默认 32 MB），到了以后写一行 WARN，之后不再写入，拆除时写出丢弃的行数。会话自己的日志（加载、拆除、订阅核对）不限。`DDCore.log` 不在这些限制之内；Mod 隔帧订阅、撤销同一个主题时 DDCore 每帧装卸一次语义 hook 或 listener，这类 INFO 行按名字限速：每个 hook 或 listener 每 60 秒最多逐条写 8 行，其余合并成一行汇总（装上、卸下各几次）。

注册表式的扩展点（`dd.effect.register`、`dd.buff.rule`）属于 P4。

## 需求三：Effect 和 Buff（P4）

**查询和施加**直接用游戏的函数，不依赖对结构的完整理解：

```lua
local e = dd.effect.get("Stun 1")
e:apply(performer, target)

for b in actor:buffs() do print(b.id, b.stat, b.amount) end
actor:add_buff("my_buff", { duration = 3, source = "lua" })
actor:remove_buffs{ source = "lua" }
```

**定义新内容**分三层：

1. 文本定义：Lua 给出 `.effects.darkest` 语法的文本，交给游戏自己的解析器（`EonText`）。不依赖结构体布局，游戏更新时最稳。
2. 脚本 Effect：`dd.effect.register` 把 DDShield 的 `shield_gain_37` 手法通用化，`EffectApplyInternal` 的 hook 按名字路由到 Lua，数据层照常用 `.darkest` 引用。
3. 脚本 Buff：带 `on_apply/on_remove/on_turn/on_damage` 回调的持续状态，字段放在按 actor guid 索引的表里，显示复用 `buff_icon` 的 tray 图标手法。

自定义 `stat_type`、`rule_type` 要先搞清游戏遇到未知枚举时的行为。

**持久化**：脚本状态写入 `<游戏根目录>\DDModLoader\save\<profile>\<mod>.json`，随游戏存档一起写，带版本号。不放进 Steam 的 profile 目录，避免影响云存档。

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
- `os` 只留 `time/clock/date/difftime`（`date` 的时间限制在 0 到 3000 年）；`debug` 只留 `traceback`；不开放 `io`、`package`，由 `dd.fs` 只读访问 Mod 自己的目录（拒绝 `..`、绝对路径、盘符，用最终路径比较前缀，联接逃逸也挡住）。已知限制：硬链接挡不住，`GetFinalPathNameByHandle` 返回打开时用的那条路径，Mod 目录里指向目录外文件的硬链接能被读到。硬链接只能在玩家自己的机器上创建，Workshop 分发不了，所以不拦截。
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
  - GC 的阈值在内存上限以下：下一轮 GC 的阈值（原版是活对象的 2 倍）不超过 max(上限的 7/8, 活对象 + 上限的 1/16)（`lgc.c` 的 `setpause` 补丁，`lua_ddgcthreshold_fn`）。活对象超过上限的一半时，原版的阈值到不了，增量 GC 不再启动，每次都是分配撞上上限、做一次完整的紧急 GC；现在活对象在上限的 15/16 以下时照常走增量 GC，余量（上限的 1/16）够一轮增量 GC 做完，不会变成连续不停的 GC。
- **预算不能保证什么**：
  - 一次不能中途打断的操作：超时只能在它做完以后发现，超出的幅度最多是一次操作。实测（预算 50 毫秒）：
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
  - 跨入口的总量：事件回调不能顺延，一帧里所有事件回调的总时间没有上限，只有单次预算，加上 `max_per_mod` 限制回调的数量；调度器的工作有每帧总时间（`frame_budget_ms`，见「需求二」）；拆除和重载时每个 `<close>` 一份预算，总时间由 `teardown_budget_ms` 截断，最多超出一份预算。Mod 每帧都用满预算时游戏会变慢，但不会卡死。
  - 哈希密钥不是密码学强度：每张表的密钥由进程密钥和表的地址混合，Mod 原则上可以通过遍历顺序、计时花大量计算去推测；字符串哈希的种子是 32 位（Lua 的设计）。没有找到能在几帧之内做到的办法，不在保证之内。
  - DDCore 服务和游戏函数本身的耗时（原生代码）不计入预算。
  - 内存到上限后的那 1 秒里，Mod 放掉引用也要等冷却结束才会被紧急 GC 回收，期间的分配直接报内存错误。
- 内存：整个 VM 共享一个上限（默认 128 MB），超出时分配失败、报内存错误，不会崩溃。到上限时紧急 GC 的规则见上。
- 原生异常：Lua 代码执行期间发生原生异常（SEH，比如服务里的访问违例）时，DDLua 把这个会话的 VM 标记为损坏，写一条 ERROR 日志，之后本会话不再进入 Lua，退出存档时不碰 VM、直接丢弃（Lua 内存泄漏），下次进存档新建 VM。两种入口的处理不同：事件回调里的由 DDCore 的隔离接住，它撤销出事的那个订阅，并通过撤销通知（`DD_EVENT_END_CRASH`）告诉 DDLua；订阅已经不在时（`dd.once` 在调用前撤销，或回调里先 `off()` 了自己）DDCore 不发通知，所以 DDLua 的事件跳板另有一个 SEH 过滤器，在异常的第一遍搜索里先标记损坏，再交给 DDCore 照常处理，同一次派发里后面的订阅就不再进入 Lua；lifecycle 回调（`CampaignLoad` 执行 `main.lua`、`TICK` 里的定时器和协程、拆除时的 `<close>`）里的由 DDLua 自己接住（`plugin.cpp` 的 `RunMarked`），正常返回 DDCore，所以 DDCore 不会撤销 DDLua 的 lifecycle 订阅（它对崩溃的 lifecycle 回调一律撤销且不通知），损坏的会话只撤销 `TICK`，下次进存档照常加载。拆除本身再出原生异常时，会话对象整个泄漏、不再析构。展开时中间栈帧里的 C++ 对象不一定析构（按 `/EHsc` 编译），所以出事的会话不再触碰 VM 和它的状态。
- 错误信息带 Mod id、文件和行号，不调用 Mod 的 `__tostring`。
- PvP 对局中会改结算的订阅不派发（[ADR 0011](adr/0011-pvp-no-mutating-dispatch.md)）。
- LuaLS 的类型声明在 `sdk/lua/`，Mod 的 `.luarc.json` 把它加进 `workspace.library`，并关掉 `io`、`os`、`debug`、`package` 的内置声明（见 `templates/lua/.luarc.json`）。
