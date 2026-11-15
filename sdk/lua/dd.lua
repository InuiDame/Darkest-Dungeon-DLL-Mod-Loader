---@meta dd
-- DDLua 的 Lua API 类型声明（LuaLS / sumneko lua-language-server）。API 版本 1。
-- 主题、负载字段和 dd.on / dd.once / dd.wait_event 的按主题重载在 dd_topics.lua（tools/gen_lua_types.py 生成）。
-- 沙箱里 os、debug 的子集在 sandbox.lua。用法：在 Mod 的 .luarc.json 里把本目录加进 workspace.library，
-- 并按 templates/lua/.luarc.json 关掉 io、os、debug、package 的内置声明。
--
-- 运行规则（详见 docs/03-lua-system.md）：
-- * 所有 Lua 代码都在游戏主线程运行。每次从 C++ 进入 Lua（main.lua、事件回调、定时器、协程恢复）都有执行预算
--   （DDLua.cfg 的 budget_ms，默认 50 毫秒；main.lua 用 load_budget_ms，默认 500 毫秒），超出后报错，pcall 吞不掉。
-- * 定时器、协程恢复、重载每帧共用 frame_budget_ms（默认 20 毫秒），超出的顺延到下一帧，各 Mod 轮流先执行。
--   事件回调不顺延。
-- * 订阅、定时器、协程都属于注册它的 Mod，退出存档（或开发模式下重载）时自动撤销，不需要手动清理；
--   撤销（:off()、dd.off、:cancel()）只能由所属 Mod 自己的代码进行，别的 Mod 拿到句柄去撤销会报错。
--   拆除时关闭协程（运行 <close>）的总时间有上限（teardown_budget_ms，默认 200 毫秒），超过后剩下的协程直接丢弃，
--   它们的 <close> 不运行。
-- * 只有弱键（__mode 含 k 不含 v）的表按强引用表处理，键不会被回收；__mode = "v"、"kv" 照常。
-- * 回调可以嵌套：在安全点立即执行的写操作（add_buff 等）会同步派发 buff.add / buff.remove，订阅者的回调在写操作返回之前运行。
--   嵌套的回调有自己的预算，用掉的时间（有上限）补给外层，互不连累；嵌套回调里的写操作排到下一帧。

---DDLua 的 API 表。每个 Mod 有自己的一份（函数绑定在调用它的 Mod 上）。
---@class dd
---@field id string 本 Mod 的 id（manifest.json 的 id）
---@field version string 本 Mod 的版本（manifest.json 的 version，原文）
---@field api integer DDLua 的 Lua API 版本，现在是 1
---@field dev boolean 是否开发模式（DDLua.cfg 的 dev_mode = 1）。开发模式下才有 dd.reload 和文件监视
---@field state table 按 Mod id 保存的表：开发模式下重载后保留，退出存档时丢弃
---@field store table 随存档保存的表（P4c）：main.lua 运行之前从存档旁路读好，每次游戏存档（campaign.save）时写出。
---键全是字符串或从 1 开始的连续整数，值是布尔、有限数、UTF-8 字符串、表（同脚本 Buff 的状态）；写出的数据（含本 Mod 的
---脚本 Buff 状态）超过 DDLua.cfg 的 store_max_kb（默认 256）时这次不写。可以整体赋值（dd.store = {...}）。
---读档结果见 dd.store_status()；开发会话的 dd.store 只在内存里
---@field log dd.loglib 日志，写进 DDModLoader\log\DDLua.log，每行以 [Mod id] 开头
---@field fs dd.fslib 读本 Mod 自己的目录，读写本 Mod 的数据目录
---@field actor dd.actorlib actor 句柄（英雄、怪物）
---@field buff dd.bufflib Buff：定义（含脚本 Buff）、查询定义、枚举表、脚本 Buff 的状态、规则扩展（P4）
---@field effect dd.effectlib Effect：句柄、文本定义、脚本 Effect（P4）
dd = {}

-- ---------------------------------------------------------------- Mod 之间

---取另一个已加载的 Mod 的导出（它的 main.lua 的返回值）。要保证先加载，把它写进 manifest 的 dependencies。
---导出是表时返回一张只读的浅代理：读、pairs、# 照常，赋值报错。同一个 Mod 每次调用拿到同一个代理
---（dd.import(x) == dd.import(x)，可以当表的键），不同的 Mod 各自一份。不是表的导出原样返回。
---代理和原表的差异：next(P) 是 nil（用 pairs 遍历）；rawget、rawlen、rawequal 看到的是空的代理；getmetatable(P) 是 false；
---代理传回提供方以后，和导出表也不相等。
---只保护导出表这一层：更深层的表、导出函数返回的对象都和提供方共享（单 VM），依赖方能改。提供方写类时，类的元表要设
---__metatable（getmetatable 拿不到它），不要把导出表本身当元表或从导出函数里返回它。
---@param id string
---@return any exports 没有加载（或还没加载完）时为 nil
---@return string? reason
function dd.import(id) end

---开发模式下重载一个 Mod（默认重载自己）：拆除它和依赖它的 Mod，下一帧重新执行 main.lua。
---非开发模式返回 false 和原因。manifest.json 不重新读取。
---@param id? string
---@return boolean ok
---@return string? reason
function dd.reload(id) end

---本 Mod 当前持有的订阅、wait_event 等待者、定时器、协程数，以及 main.lua 被执行的次数。
---@return dd.Stats
function dd.stats() end

---@class dd.Stats
---@field subscriptions integer dd.on / dd.once / dd.effect.register 的订阅
---@field waiters integer 正在等待的 dd.wait_event
---@field timers integer dd.after / dd.every
---@field tasks integer dd.spawn 的协程
---@field loads integer main.lua 执行过的次数（重载会增加）
---@field pending integer 排队等结果、带 done 回调的写操作
---@field rules integer dd.buff.rule 注册的规则
---@field states integer 本 Mod 的脚本 Buff 绑定着状态的载体条数

-- ---------------------------------------------------------------- 存档旁路（P4c）

---dd.store 这次从存档旁路读到的结果：
---"loaded"（旁路和存档一致）、"prev"（当前一代不一致，上一代一致）、"empty"（没有旁路，新战役或从没写过）、
---"mismatch"（旁路和存档不一致或文件损坏，dd.store 是空表）、"unavailable"（DDCore 不能判断，dd.store 是空表）。
---第二个返回值是原因（没有说明时为 nil）。新战役复用以前用过的存档位时是 "empty"。地牢里的存档读档时就按名册和 raid 判断，
---探索存档读完后再确认一次 raid：确认前第三个返回值 raid_pending 为 true（怪物的脚本 Buff 状态这时还没恢复），
---不一致时状态改成 "mismatch"（main.lua 已经读到的 dd.store 收不回来，Mod 自己决定怎么办；这次进存档不再写这个 Mod 的旁路）。
---等 raid 核对用 raid_pending 判断，不要在原因里找「raid」：读到上一代时原因里本来就可能写着 raid 不一致的那一代。
---不一致时原来的旁路改名为 <mod id>.ddsave.mismatch-<时间>。
---@return "loaded"|"prev"|"empty"|"mismatch"|"unavailable" status
---@return string? reason
---@return boolean raid_pending
function dd.store_status() end

-- ---------------------------------------------------------------- 事件

---取消一个订阅，等同于 sub:off()。订阅属于别的 Mod 时报错。
---@param sub dd.Subscription
---@return boolean was_active 原来是否有效
function dd.off(sub) end

---本 Mod 当前有效的 dd.on / dd.once 订阅数（不含 dd.wait_event 的等待者）。
---@return integer
function dd.subscriptions() end

---订阅的作用域：到期时由 DDCore 自动撤销。
---* "mod"：默认，直到本 Mod 被拆除（退出存档或重载）
---* "campaign"：退出存档时
---* "raid"：探索结算（raid.finish）或退出存档时
---* "battle"：战斗销毁（battle.end）或退出存档时
---* actor 句柄：这个 actor 失效时（怪物离开战斗、战斗结束；英雄离开名册、换存档）
---@alias dd.Scope "mod"|"campaign"|"raid"|"battle"|dd.Actor

---dd.on / dd.once 的选项。
---@class dd.OnOptions
---@field priority? integer 升序执行，默认 0；同优先级按订阅先后。和 native 插件的订阅交错排序
---@field scope? dd.Scope 默认 "mod"
---@field filter? table<string, integer|number|boolean|string|dd.Actor> 字段等值过滤，最多 16 项，不匹配时不进入 Lua。指针字段只能用 actor 句柄

---订阅对象。
---@class dd.Subscription
---@field topic string 主题名
local Subscription = {}

---取消订阅。只有注册它的 Mod 自己的代码能取消，别的 Mod（比如通过 dd.import 拿到句柄）调用时报错。
---@return boolean was_active 原来是否有效（已取消、被作用域或连续出错停用的返回 false）
function Subscription:off() end

---订阅是否仍然有效。
---@return boolean
function Subscription:active() end

---事件负载（userdata），**只在回调执行期间有效**，回调返回后任何访问都报错。要保存数据请复制字段，
---Hero、Monster 字段是 actor 句柄，可以跨回调保存。effect.* 的 effect 字段是 effect 句柄（dd.Effect），skill.* 的 skill
---字段是技能句柄（dd.Skill），buff.* 延迟事件的 actor 字段按 actor_kind、actor_id 找回句柄；其余指针字段是 nil。
---读未知字段报错；只有 .pre 主题、query.* 和 damage.final（absorb）的可写字段
---能赋值，类型必须一致（整数字段收整数，数字字段收有限数，布尔字段收布尔），字符串和指针字段 Lua 不能写。
---buff.* 里 Lua Mod 加的条目，plugin 字段是 "DDLua"，tag 字段是 "<mod id>:<tag>"（actor:buffs() 的快照里已经拆开）。
---数字字段在游戏里是 float 或 int32：超出这个宽度的值（绝对值超过约 3.4e38，int32 以外的整数）报「超出范围」，不写入。
---主题的字段优先于这里的元信息（damage.post 自己有 cancelled 字段，含义相同）。
---@class dd.Event
---@field topic string 主题名
---@field deferred boolean 是否延迟事件：加载线程上发生、排队到主线程派发的。延迟事件只读、不能取消、指针字段都是 nil
---@field cancelled boolean 是否已被本次派发里排在前面的回调取消
---@field writable boolean 当前是否可写（.pre、query.* 主题和 damage.final，并且不是延迟事件；PvP 对局中 .pre 和 query.* 不派发，damage.final 也不派发）
local Event = {}

---取消这次事件（只有可取消的 .pre 主题：damage.pre、effect.pre）。damage.pre 取消后伤害按 0 结算，
---effect.pre 取消后跳过这一个目标的 Effect。不能取消的主题、延迟事件调用时报错。
function Event:cancel() end

---dd.wait_event 交给协程的字段快照（普通的表，不会失效）。字段和负载相同，指针字段同样转成 actor 句柄，
---.pre 主题的字段是所有普通订阅执行完之后的最终值。
---@class dd.Snapshot
---@field topic string
---@field deferred boolean
---@field cancelled boolean

-- ---------------------------------------------------------------- 调度器和协程

---创建协程并立即运行到第一次挂起。协程里可以调用 dd.wait、dd.wait_event 和 coroutine.yield()（等一帧）。
---协程里的错误写进日志，不传给调用者（超出执行预算除外）。不要用 coroutine.resume / coroutine.close 操作这个协程
---（coroutine.running() 能拿到它）：手动恢复时里面的 dd.wait、dd.wait_event 报错，被手动跑完、关掉的下一帧清理。
---@param fn async fun(...)
---@param ... any 传给 fn 的参数
---@return dd.Task
function dd.spawn(fn, ...) end

---挂起当前协程，至少等待 seconds 秒（真实时间，每帧检查一次；dd.wait(0) 等一帧）。
---只能在 dd.spawn 创建的协程里直接调用：事件回调、定时器回调、main.lua 里调用会报错。
---@async
---@param seconds number 非负
function dd.wait(seconds) end

---seconds 秒后在主线程调用一次 fn（真实时间，每帧检查；最早下一帧）。只调用一次，出错写日志。
---@param seconds number 非负
---@param fn fun(timer: dd.Timer)
---@return dd.Timer
function dd.after(seconds, fn) end

---每隔 seconds 秒调用一次 fn，不补发错过的次数；seconds 为 0 时每帧一次（不在创建的那一帧）。
---回调连续出错 max_failures 次（默认 3）或超出执行预算时停用。一帧的调度时间用完时顺延到下一帧。
---@param seconds number 非负
---@param fn fun(timer: dd.Timer)
---@return dd.Timer
function dd.every(seconds, fn) end

---调度器的时钟（秒，真实时间，单调递增，起点不固定）。
---@return number
function dd.time() end

---定时器句柄。
---@class dd.Timer
local Timer = {}

---取消定时器。只有注册它的 Mod 自己的代码能取消，别的 Mod 调用时报错。
---@return boolean was_active
function Timer:cancel() end

---@return boolean
function Timer:active() end

---协程句柄。
---@class dd.Task
local Task = {}

---取消协程：挂起中的立即关闭（<close> 变量照常执行），正在运行的在下一次挂起时关闭。
---只有创建它的 Mod 自己的代码能取消，别的 Mod 调用时报错。
---@return boolean was_active
function Task:cancel() end

---"running"：正在运行或下一帧继续；"waiting"：在 dd.wait / dd.wait_event 里；"dead"：已结束、出错或取消。
---@return "running"|"waiting"|"dead"
function Task:status() end

---@return boolean
function Task:active() end

-- ---------------------------------------------------------------- 日志

---@class dd.loglib
local loglib = {}

---参数用 tostring 转换后以制表符连接，写一行 DEBUG 日志（一次最多 4096 字节，超出的截断）。
---每个 Mod 每秒最多 log_lines_per_sec 行（默认 200，print、warn、dd.log 和 DDLua 替这个 Mod 写的回调错误、traceback、
---失败原因合计，多行文本按行数算），超出的丢弃，之后补一行汇总。错误信息超过 2048 字节的截断。
---一次进存档里和 Mod 有关的日志总共最多 log_max_mb（默认 32 MB），超出后不再写入。
---@param ... any
function loglib.debug(...) end

---写一行 INFO 日志。print(...) 等同于 dd.log.info(...)。
---@param ... any
function loglib.info(...) end

---写一行 WARN 日志。
---@param ... any
function loglib.warn(...) end

---写一行 ERROR 日志。
---@param ... any
function loglib.error(...) end

-- ---------------------------------------------------------------- 文件

---两个根目录：Mod 目录（只读，project.xml 所在的目录，默认）和本 Mod 的数据目录
---<游戏根目录>\DDModLoader\data\<mod id>\（读写，不分存档；分存档的状态用 P4c 的 dd.store）。
---路径用 / 或 \ 分隔、相对根目录，不能含 ..、盘符、冒号、通配符、Windows 保留的设备名（CON、NUL.txt、COM1……）；
---联接和符号链接逃出根目录时拒绝。
---数据目录的总大小和条目数（文件和目录都算）有上限（DDLua.cfg 的 data_max_mb 默认 16、data_max_files 默认 256）。
---每次调用按文件操作的次数计入执行预算，返回前检查截止时间。
---@class dd.fslib
local fslib = {}

---读写的根目录：`{ root = "data" }` 是数据目录，默认（或 "mod"）是 Mod 目录。
---@class dd.FsOptions
---@field root? "mod"|"data"

---读取整个文件（二进制原样）。
---@param path string 例如 "data/config.json"
---@param opts? dd.FsOptions
---@return string? content
---@return string? reason
function fslib.read(path, opts) end

---文件或目录是否存在。
---@param path string
---@param opts? dd.FsOptions
---@return boolean
function fslib.exists(path, opts) end

---列出目录里的文件和子目录名，按名字排序，子目录名后面带 "/"（不含 . 和 ..）。
---数据目录还没有写过文件时，根目录返回空表。
---@param path? string|dd.FsOptions 默认是根目录；也可以直接给选项：dd.fs.list{ root = "data" }
---@param opts? dd.FsOptions
---@return string[]? names
---@return string? reason
function fslib.list(path, opts) end

---写数据目录里的文件（二进制原样，整个替换）：先写同目录的临时文件再替换，要么是旧内容要么是新内容。
---路径最多 8 层（含文件名），父目录按需创建（新建的目录也计入 data_max_files）。超过 data_max_mb、data_max_files，
---路径不合法、是目录时返回 nil 和原因。规则回调里调用报错。写盘本身不能中断，写完按数据量计入执行预算。
---每次都同步刷盘（FlushFileBuffers、MOVEFILE_WRITE_THROUGH）：机械硬盘上一次小文件可能几十毫秒，大文件更久，
---单次写入的耗时会超出预算（超时在写完后才报），不要每帧写。
---@param path string
---@param content string
---@return true? ok
---@return string? reason
function fslib.write(path, content) end

---删除数据目录里的一个文件或空目录（目录不空时返回 nil 和原因；数据目录本身不能删）。
---@param path string
---@return true? ok
---@return string? reason
function fslib.remove(path) end

-- ---------------------------------------------------------------- actor 句柄

---@class dd.actorlib
local actorlib = {}

---按 hero id 在当前存档的名册里找英雄。不在名册（或不在存档里）时返回 nil。
---@param id integer 非负整数（不接受字符串）
---@return dd.Actor?
function actorlib.hero(id) end

---按 guid 在当前战斗里找怪物。不在战斗里时返回 nil。guid 只在一场战斗内唯一：游戏每场战斗重新编号，
---同一次探索的下一场战斗会出现同样的 guid，这时返回的是另一个句柄（上一场的句柄已失效，两者不相等）。
---@param guid integer 非负整数
---@return dd.Actor?
function actorlib.monster(guid) end

---v 是不是 actor 句柄。
---@param v any
---@return boolean
function actorlib.is(v) end

---当前探索队伍里的英雄（F5 起，P5 的 dd.raid 会在这上面扩展），顺序同游戏的队伍表（换位后游戏按站位重排，
---一般是第 1 位到第 4 位）。每个英雄都在名册里核对过才给句柄。城镇里（没有探索）返回空表。
---不用先在事件里见过：开战前、读进战斗中的存档之后、走廊里都能直接拿到。
---加载线程在跑时（读档进地牢、回城的加载画面）返回 nil 和以「加载中」开头的原因，加载结束后再读；
---不在存档里时返回 nil 和原因。
---@return dd.Actor[]? heroes
---@return string? reason
function actorlib.party() end

---当前战斗的怪物（F5 起），顺序同游戏的怪物表（按站位）。不在战斗里时返回空表。怪物死后变成尸体（同一个 guid，
---class 换成 corpse 开头的职业，身上的 buff 被游戏删掉）仍在表里，要排除时看 class 或 hp。
---加载中、存档外同 dd.actor.party。
---@return dd.Actor[]? monsters
---@return string? reason
function actorlib.monsters() end

---现在是不是在战斗中（F5 起）：看游戏的战斗状态，不靠 battle.start。读进战斗中的存档时游戏不派发 battle.start，
---这里照样是 true；撤退时游戏先把状态清零，这里马上是 false。第二个返回值是游戏的战斗状态（Battle+0x288，0 是没有战斗）。
---加载中、存档外返回 nil 和原因（同 dd.actor.party）。
---@return boolean? in_battle
---@return integer|string? state_or_reason
function actorlib.in_battle() end

---actor 句柄（只读的值）：不持有游戏指针，每次读字段都重新查找。可以跨回调、跨协程保存，可以当表的键；
---同一个 actor 总是同一个 userdata（rawequal 成立），也支持 == 和 tostring。
---句柄失效后（怪物离开战斗或战斗结束、下一场战斗、下一次探索，英雄离开名册（被解雇）、换存档）标识字段照常，
---读数字段为 nil。阵亡的英雄留在名册里，句柄仍然有效。怪物句柄带着战斗序号：两场战斗里 guid 相同的怪物是两个
---不同的句柄，== 为假，当表的键也分开。
---只能在主线程使用（DDLua 的代码都在主线程）。
---@class dd.Actor
---@field kind "hero"|"monster"
---@field id integer 英雄是 hero id，怪物是 guid（只在一场战斗内唯一）
---@field is_hero boolean
---@field is_monster boolean
---@field hp number? 当前生命，失效时为 nil
---@field max_hp number? 最大生命（游戏自己的合计函数），失效或不可用时为 nil
---@field stress number? 压力，只有英雄有；怪物和失效时为 nil
---@field class string? 职业 id（如 "crusader"），失效或读不到时为 nil
local Actor = {}

---句柄现在能不能解析到游戏里的对象。
---@return boolean
function Actor:valid() end

---身上的 buff（快照数组，按游戏的表顺序）。filter 的各项同时满足才返回：id、source（名字或数值）、tag（只匹配本 Mod 加的）、
---stat（名字或数值）、mine（是不是本 Mod 加的）。句柄解析失败时返回 nil 和原因。只在存档里有内容。
---加载线程在跑时（读档进地牢的 as_raid_start、回城的 as_town_visit_start 的加载画面，定时器、协程、延迟事件照常运行）
---buff 表、Buff 库、effect 表都读不到：这几个读接口返回 nil 和以「加载中」开头的原因，加载结束后再读。
---@param filter? dd.BuffFilter
---@return dd.Buff[]? buffs
---@return string? reason
function Actor:buffs(filter) end

---按组 guid 取一条 buff 的快照。条目不在（或句柄解析失败）时只返回 nil；加载线程在跑时返回 nil 和以「加载中」开头的原因，
---这时的 nil 不说明条目已经没了。
---@param guid integer
---@return dd.Buff? buff
---@return string? reason
function Actor:buff(guid) end

---按 id 添加 buff（复制 Buff 库里的定义，按 opts 改字段）。同 id 重复添加按游戏的语义叠加（两条），要替换时先删。
---时机：在安全点（main.lua、定时器、协程、done 回调）立即执行，返回 true 和新条目的 guid；在事件回调里（包括立即执行的
---写操作同步派发的 buff.add 等嵌套回调）排到下一帧，返回 true 和 nil，执行后调用 opts.done。
---存档外、加载中、PvP、id 不存在、句柄解析失败、本 Mod 排队超过 128 个时返回 nil 和原因。参数写错、规则回调里调用报错。
---@param id string
---@param opts? dd.AddBuffOptions
---@return true? ok
---@return integer|string|nil result 立即执行时是 guid；排队时是 nil；被拒绝时是原因
function Actor:add_buff(id, opts) end

---删除 buff（游戏刷新属性）。filter 在 guid、tag（只删本 Mod 加的）、id、source 里恰好给一个，可以另带 done。
---返回约定同 add_buff：立即执行时结果是删掉的条数（0 也算成功），排队时是 nil。
---@param filter dd.RemoveBuffFilter
---@return true? ok
---@return integer|string|nil result
function Actor:remove_buffs(filter) end

---@class dd.BuffFilter
---@field id? string
---@field source? string|integer 来源名（去掉 bsrc_ 前缀，带上也认）或数值
---@field tag? string 本 Mod 加的条目的标签
---@field stat? string|integer stat_type 的名字或数值
---@field mine? boolean

---@class dd.AddBuffOptions
---@field duration? integer -1（不计时）或大于 0
---@field duration_type? string|integer
---@field source? string|integer
---@field sourcedata? integer
---@field tag? string 标签（DDCore 记为 "<mod id>:<tag>"，两部分合计最多 63 字节），按标签删除只删本 Mod 的
---@field refresh? boolean 默认 true：添加后游戏重算属性
---@field state? table 脚本 Buff 的初始状态（只能给脚本 Buff；必须能保存成 JSON，见 dd.buff.state）
---@field done? fun(ok: true?, result: integer|string) 排队时执行后调用：(true, guid) 或 (nil, 原因)

---@class dd.RemoveBuffFilter
---@field guid? integer
---@field tag? string
---@field id? string
---@field source? string|integer
---@field done? fun(ok: true?, result: integer|string) 排队时执行后调用：(true, 删掉的条数) 或 (nil, 原因)

---一条 buff 的快照（普通的表，不会随游戏变化）。枚举字段是名字，名字认不出时是数值。
---Lua Mod 加的条目在 DDCore 里的插件名是 "DDLua"、标签是 "<mod id>:<tag>"，这里拆开成 mod 和 tag。
---@class dd.Buff
---@field guid integer 组 guid：条目唯一稳定的标识（同一个 actor 上只增不减，随存档保存）
---@field id string
---@field stat string|integer stat_type
---@field sub_stat string
---@field amount number
---@field duration integer 剩余次数，-1 不计时
---@field duration_type string|integer
---@field source string|integer
---@field source_flags { saved: boolean, town_clear: boolean, tray: boolean } 来源表的三个标志：进存档、回城清掉、进 tray
---@field sourcedata integer
---@field rule string|integer
---@field rule_float number
---@field rule_string string
---@field is_false_rule boolean
---@field remove_if_not_active boolean
---@field remove_on_battle_complete boolean
---@field plugin? string 由插件经 combat.buff_add 加的（并且 DDCore 还记着，标签不随游戏存档保存）时是插件名
---@field mod? string 由 Lua Mod 加的时是它的 id
---@field tag? string 加的时候给的标签
---@field mine boolean 是不是本 Mod 加的
---@field script boolean id 是不是本会话里的脚本 Buff
---@field actor dd.Actor

-- ---------------------------------------------------------------- Buff 与 Effect（P4）
--
-- 写操作（定义、添加、删除、施加）的返回约定：
-- * 参数写错（类型、未知选项、认不出的枚举名、文本过长）直接报错；规则回调里调用报错（规则回调只读）。
-- * 被拒绝（不在存档里、加载中、PvP、id 或 effect 不存在、句柄解析失败、城镇里施加 effect、已经被游戏数据或别的 Mod 定义、
--   本 Mod 排队超过 128 个）返回 nil 和原因。
-- * 接受时返回 true 和结果。安全点（main.lua、定时器、协程、done 回调）上立即执行，结果见各函数；
--   事件回调里（包括立即执行的写操作同步派发的 buff.add / buff.remove 等嵌套回调）排到下一帧，结果是 nil，
--   执行后调用 opts.done(true, 结果) 或 opts.done(nil, 原因)。只有排队的操作调用 done；done 按订阅计入 max_per_mod，受执行预算约束。
-- * 定义每次进存档都要重新注册（Buff 库、effect 表每次读档重建），main.lua 里注册就行。

---@class dd.bufflib
local bufflib = {}

---在 Buff 库里定义一个 buff（只开放 POD 字段：附加效果、force_positive 之类「只在定义里」的字段要写进数据文件）。
---带 on_apply / on_remove / on_tick / on_damage 时是脚本 Buff：游戏照常负责 tray、持续时间、战斗结束和回城清理、存档，
---每条载体（actor，组 guid）有一张状态表，见 dd.buff.state。
---id 已经是游戏数据或别的 Lua Mod 定义的时返回 nil 和原因；同一个 Mod 重复定义时更新字段和回调。
---立即执行时结果是 id。
---@param def dd.BuffDefinition
---@return true? ok
---@return string? result
function bufflib.define(def) end

---Buff 库里的定义（游戏数据和运行时定义的），没有时返回 nil 和原因（加载中时原因以「加载中」开头）。
---@param id string
---@return dd.BuffDef? def
---@return string? reason
function bufflib.def(id) end

---枚举表的名字和数值互查。table 是 "stat"、"source"、"duration"（也认 "duration_type"）、"rule"。
---只给 table 时返回全部名字的数组（下标 = 数值 + 1）；给名字返回数值，给数值返回名字，认不出时为 nil。
---source 的名字去掉了游戏里的 bsrc_ 前缀，查找时两种写法都认。
---@param table "stat"|"source"|"duration"|"duration_type"|"rule"
---@param value? string|integer
---@return string[]|integer|string|nil
function bufflib.enum(table, value) end

---一条脚本 Buff 载体的状态表（和回调收到的 state 是同一张表）。条目不存在或不是脚本 Buff 时为 nil
---（还没绑定状态、加载线程在跑时返回 nil 和以「加载中」开头的原因）；
---读档恢复的载体第一次用到时绑定一张空表（不调用 on_apply）。状态随载体的生命周期：载体被删掉（到期、战斗结束、回城清理、
---解雇）时丢弃，actor 不再能解析时（怪物随战斗销毁）也丢弃。
---状态要能保存成 JSON（P4c 随存档保存）：键全是字符串，或者是从 1 开始的连续整数；值是布尔、有限数、UTF-8 字符串、表。
---@param actor dd.Actor
---@param guid integer
---@return table? state
---@return string? reason
function bufflib.state(actor, guid) end

---注册脚本规则。数据层的 buff 写 rule_type "is_actor_status"（检查自己）或 "actorStatus"（检查 target），
---rule_data.string 填 name；游戏判定这条规则时调用 fn，返回规则本身是否成立（true / false，is_false_rule 由 DDCore 取反）；
---返回 nil（或不返回）表示这一次交还游戏原来的判定，不算出错。
---fn 在游戏遍历 buff 表时运行，非常频繁：只读（写操作、注册订阅、定时器、协程、写文件都报错），预算是 rule_budget_ms（默认 2 毫秒），
---每个 Mod 每帧的规则和脚本 Buff 回调合计不超过 combat_frame_ms（默认 10 毫秒，超出后这一帧余下的交还原来的判定）。
---返回别的非布尔值、出错、超预算、停用中也都交还游戏原来的判定（和没有这个 Mod 时一样：认不出的字符串原始结果为假，再由游戏
---按 is_false_rule 取反）；连续出错 max_failures 次或超预算一次后停用，下一帧向 DDCore 撤销。不在主线程、PvP 中时 DDCore 不调用 fn。
---名字已经注册过（别的 Mod 或 native 插件，或哈希相同）时返回 nil 和原因。
---@param name string 最多 63 字节
---@param fn fun(self: dd.Actor?, target: dd.Actor?, buff: dd.RuleBuff): boolean?
---@return dd.Rule? rule
---@return string? reason
function bufflib.rule(name, fn) end

---@class dd.BuffDefinition
---@field id string 最多 63 字节
---@field stat string|integer stat_type（必填），名字见 dd.buff.enum("stat")
---@field sub_stat? string 例如 "speed_rating"。按 stat 校验：combat_stat_add/multiply、resistance、activity_side_effect_chance、disable_combat_skill_attribute 必须是游戏认的名字（见 dd_api.h 的 buff_define），认不出时返回 nil 和原因
---@field amount? number 默认 0
---@field duration? integer 默认 -1（不计时），否则大于 0
---@field duration_type? string|integer 默认 "till_removed"
---@field source? string|integer 默认 "skill"（进存档、回城清掉、进 tray）
---@field sourcedata? integer
---@field rule? string|integer 默认 "always"
---@field rule_string? string
---@field rule_float? number
---@field is_false_rule? boolean
---@field remove_if_not_active? boolean
---@field remove_on_battle_complete? boolean
---@field on_apply? fun(buff: dd.Buff, state: table) 载体加上之后（Lua 立即执行的 add_buff 在返回之前；排队的在执行后；游戏或别的插件加的在 buff.add 里，状态是空表）
---@field on_remove? fun(buff: dd.Buff, state: table, reason: string) 载体删除之前。reason 同 buff.remove 主题（expired、plugin、town_visit……），actor 不再能解析时是 "gone"
---@field on_tick? fun(buff: dd.Buff, state: table, before: integer, after: integer) 持续时间递减（战斗回合、走廊格子也会递减；减到 0 的那次在 on_remove(..., "expired") 之前）
---@field on_damage? fun(buff: dd.Buff, state: table, ev: dd.ev.damage.pre) 载体所在的 actor 受到伤害（damage.pre，可写、可取消）
---@field done? fun(ok: true?, result: string) 排队时执行后调用

---@class dd.BuffDef
---@field id string
---@field stat string|integer
---@field sub_stat string
---@field amount number
---@field duration integer
---@field duration_type string|integer
---@field source string|integer
---@field sourcedata integer
---@field rule string|integer
---@field rule_string string
---@field rule_float number
---@field is_false_rule boolean
---@field remove_if_not_active boolean
---@field remove_on_battle_complete boolean
---@field mod? string 由本会话的 Lua Mod 定义时是它的 id
---@field script boolean 是不是脚本 Buff

---规则回调的 buff 参数。
---@class dd.RuleBuff
---@field id string 带这条规则的 buff 的 id
---@field rule "actorStatus"|"is_actor_status"
---@field rule_string string 规则名
---@field rule_float number
---@field is_false_rule boolean 只是信息，回调不用自己取反
---@field subject dd.Actor? 要检查的角色：is_actor_status 是 self，actorStatus 是 target

---规则注册句柄。
---@class dd.Rule
---@field name string
local Rule = {}

---撤销规则（如果这是最后一条，下一个帧边界 DDCore 拆掉 hook）。只有注册它的 Mod 自己的代码能撤销，规则回调里调用报错。
---@return boolean was_active
function Rule:off() end

---规则是否有效（停用了的返回 false）。
---@return boolean
function Rule:active() end

---@class dd.effectlib
local effectlib = {}

---按名字取 effect 句柄。不存在（或不在存档里）时返回 nil 和原因（加载中时原因以「加载中」开头）。
---@param name string
---@return dd.Effect? effect
---@return string? reason
function effectlib.get(name) end

---用游戏自己的解析器定义 effect：text 是 .effects.darkest 的内容，一个或多个 effect: 块，每块要有 .name。
---运行时定义的 effect 能被 buff 的附加效果、effect 的 buff_ids 和 e:apply 使用，不能被技能引用（技能在职业加载时就解析好了，
---要给技能用的写进 Mod 的 effects/*.effects.darkest）。名字已经是游戏数据或别的 Lua Mod 定义的时返回 nil 和原因；
---同一个 Mod 可以重新定义自己的：标量字段被覆盖，列表字段（.buff_ids、.kill_enemy_types、.summon_monsters、
---.set_monster_class_id(s)）在原来的后面追加（游戏复用原来的对象），要改列表就换一个名字。立即执行时结果是定义的 effect 数。
---定义没有个数上限，每条都在游戏里常驻到退出存档，不要每帧定义新名字。
---文本按游戏解析器的规则检查，违反时报参数错误：块从「标识符:」开始，到下一个冒号前面的标识符为止（# 注释、空白都算在块里，
---值里的 x:y 会开始一个新块），每块最多 1023 字节、都必须是 effect: 块；.name 取块里最后一次出现的（注释里的也算），
---最多 63 字节；.target 必须有且是游戏认的（performer、target、global……）；标识符和 .name 的值周围不能有非 ASCII 字符
---（中文可以写在注释里，.name 的值要中文时加引号）。
---@param text string 最多 64 KB
---@param opts? { done?: fun(ok: true?, result: integer|string) }
---@return true? ok
---@return integer|string|nil result
function effectlib.define(text, opts) end

---脚本 Effect：施加名字匹配的 effect 时（effect.pre，每个目标一次）调用 fn。pattern 是精确的名字，或以 * 结尾的前缀
---（"shield_gain_*"），匹配在 C++ 里做，不匹配不进入 Lua。数据层照常用 .effects.darkest 定义并引用这个名字。
---ev 是 effect.pre 的负载：ev.effect 是 effect 句柄，ev.performer、ev.target 是 actor 句柄，ev:cancel() 跳过这个目标。
---回调里的写操作排到下一帧。opts 同 dd.on（priority、scope、filter），返回订阅对象。
---@param pattern string
---@param fn fun(ev: dd.ev.effect.pre)
---@param opts? dd.OnOptions
---@return dd.Subscription? sub
---@return string? reason
function effectlib.register(pattern, fn, opts) end

---effect 句柄（值，只记名字）：每次读字段都重新查找，effect 表每次进存档重建，跨存档的句柄读出 nil。
---同名的句柄是同一个 userdata，支持 == 和 tostring。字段只读，effect 不存在时为 nil（name 除外），
---加载线程在跑时也读出 nil（这时 e:valid() 返回 false 和以「加载中」开头的原因）。
---@class dd.Effect
---@field name string
---@field target integer? .target 枚举：0 performer、1 target、2 performer_group ……（knowledge/dd1/combat.md）
---@field chance number?
---@field stress number?
---@field stun number?
---@field heal number?
---@field heal_percent number?
---@field combat_stat_buff integer?
---@field dot_poison integer?
---@field dot_bleed integer?
---@field dot_burn integer?
---@field dot_stress integer?
---@field duration integer?
---@field buff_amount number?
---@field buff_duration_type string|integer|nil
---@field buff_id_count integer? .buff_ids 的个数（游戏只存哈希）
---@field on_hit boolean?
---@field on_miss boolean?
---@field apply_once boolean?
---@field queue boolean?
---@field swap_source_and_target boolean?
---@field runtime boolean? 由 Mod 运行时定义
local Effect = {}

---施加 effect（游戏的 ApplyEffect）：按 effect 的 .target 从 performer、target 出发选目标，逐个结算（会触发 effect.pre/post、
---buff.add 等）。target 省略时等于 performer。需要 Raid::Current（走廊、战斗中），城镇里返回 nil 和原因。
---立即执行时结果是表：count 是加上的 buff 条数（所有目标），数组部分是 performer 和 target 身上新增的条目 { actor, guid, id }
---（effect 打到别的角色时，那些条目只计入 count）；排队时 done 收到 { count = n }。
---@param performer dd.Actor
---@param target? dd.Actor
---@param opts? { done?: fun(ok: true?, result: { count: integer }|string) }
---@return true? ok
---@return { count: integer, [integer]: { actor: dd.Actor, guid: integer, id: string? } }|string|nil result
function Effect:apply(performer, target, opts) end

---effect 现在是否存在。加载线程在跑时返回 false 和以「加载中」开头的原因。
---@return boolean
---@return string? reason
function Effect:valid() end

---技能句柄（skill.pre / skill.post 的 ev.skill）：执行者 + 技能 id + 等级，每次在执行者职业的技能表里重新解析。只读。
---@class dd.Skill
---@field id string
---@field level integer 怪物技能为 0
---@field performer dd.Actor
---@field type string? .type，如 melee、ranged、move；解析失败时为 nil
---@field friendly boolean?
---@field effects string[]? effect 名字（最多 16 个）
local Skill = {}

---@return boolean
function Skill:valid() end
