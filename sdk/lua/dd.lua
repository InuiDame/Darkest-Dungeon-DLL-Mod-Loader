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

---DDLua 的 API 表。每个 Mod 有自己的一份（函数绑定在调用它的 Mod 上）。
---@class dd
---@field id string 本 Mod 的 id（manifest.json 的 id）
---@field version string 本 Mod 的版本（manifest.json 的 version，原文）
---@field api integer DDLua 的 Lua API 版本，现在是 1
---@field dev boolean 是否开发模式（DDLua.cfg 的 dev_mode = 1）。开发模式下才有 dd.reload 和文件监视
---@field state table 按 Mod id 保存的表：开发模式下重载后保留，退出存档时丢弃
---@field log dd.loglib 日志，写进 DDModLoader\log\DDLua.log，每行以 [Mod id] 开头
---@field fs dd.fslib 只读访问本 Mod 自己的目录
---@field actor dd.actorlib actor 句柄（英雄、怪物）
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
---@field subscriptions integer dd.on / dd.once 的订阅
---@field waiters integer 正在等待的 dd.wait_event
---@field timers integer dd.after / dd.every
---@field tasks integer dd.spawn 的协程
---@field loads integer main.lua 执行过的次数（重载会增加）

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
---Hero、Monster 字段是 actor 句柄，可以跨回调保存。读未知字段报错；只有 .pre 主题和 query.* 的可写字段
---能赋值，类型必须一致（整数字段收整数，数字字段收有限数，布尔字段收布尔），字符串和指针字段 Lua 不能写。
---数字字段在游戏里是 float 或 int32：超出这个宽度的值（绝对值超过约 3.4e38，int32 以外的整数）报「超出范围」，不写入。
---主题的字段优先于这里的元信息（damage.post 自己有 cancelled 字段，含义相同）。
---@class dd.Event
---@field topic string 主题名
---@field deferred boolean 是否延迟事件：加载线程上发生、排队到主线程派发的。延迟事件只读、不能取消、指针字段都是 nil
---@field cancelled boolean 是否已被本次派发里排在前面的回调取消
---@field writable boolean 当前是否可写（.pre 或 query.* 主题，并且不是延迟事件；PvP 对局中这些主题不派发）
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

-- ---------------------------------------------------------------- 文件（只读）

---只读访问本 Mod 的目录。路径用 / 或 \ 分隔、相对 Mod 根目录（project.xml 所在的目录），
---不能含 ..、盘符、冒号、通配符；联接和符号链接逃出 Mod 目录时拒绝。
---@class dd.fslib
local fslib = {}

---读取整个文件（二进制原样）。
---@param path string 例如 "data/config.json"
---@return string? content
---@return string? reason
function fslib.read(path) end

---文件或目录是否存在。
---@param path string
---@return boolean
function fslib.exists(path) end

---列出目录里的文件和子目录名，按名字排序，子目录名后面带 "/"（不含 . 和 ..）。
---@param path? string 默认是 Mod 根目录
---@return string[]? names
---@return string? reason
function fslib.list(path) end

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
