---@meta sandbox
-- DDLua 沙箱里和标准 Lua 5.4 不同的全局函数和库。.luarc.json 关掉 io、os、debug、package 的内置声明后，
-- os 和 debug 用这里的子集。base 库仍是 LuaLS 的内置声明，其中 dofile、loadfile、string.dump 在沙箱里不存在，
-- collectgarbage 只支持 "count" 和 "isrunning"，load 只收文本，setmetatable 拒绝带 __gc 的元表。
-- 每个 Mod 的 string、table、math、utf8、coroutine 是自己的副本，改它们不影响别的 Mod。

---写一行 INFO 日志（等同 dd.log.info），参数用 tostring 转换后以制表符连接。
---@param ... any
function print(...) end

---写一行 WARN 日志。参数必须都是字符串；"@on"、"@off" 这类控制消息被忽略。
---@param message string
---@param ... string
function warn(message, ...) end

---按模块名加载本 Mod lua/ 目录下的文件：模块名是用点分隔的字母、数字、_、-，"a.b" 对应 lua/a/b.lua 或
---lua/a/b/init.lua。每个 Mod 自己缓存；不加载 C 模块，不能访问别的 Mod 或游戏目录。
---@param modname string
---@return any
function require(modname) end

---沙箱里的 os：只有 time、clock、date、difftime。
---@class sandbox.oslib
os = {}

---@param t? table 和标准 Lua 相同的 year、month、day、hour、min、sec、isdst 字段
---@return integer
---@nodiscard
function os.time(t) end

---@return number
---@nodiscard
function os.clock() end

---时间 t 只能是 0 到 3000 年之间的整数。
---@param format? string
---@param time? integer
---@return string|table
---@nodiscard
function os.date(format, time) end

---返回 t2 - t1 的秒数，Lua 5.4 里是浮点数。
---@param t2 integer
---@param t1 integer
---@return number
---@nodiscard
function os.difftime(t2, t1) end

---沙箱里的 debug：只有 traceback。
---@class sandbox.debuglib
debug = {}

---@param message? any
---@param level? integer
---@return string
---@nodiscard
function debug.traceback(message, level) end
