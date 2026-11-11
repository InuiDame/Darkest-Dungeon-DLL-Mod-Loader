-- 夹具：正常加载，print、require（模块和 init.lua）、导出给 dd.import
BASIC_LOADED = true
print("hello from basic", 1, true)
local util = require("lib.util")
assert(util.name == "basic util" and util.owner == "basic", "util")
assert(require("lib.util") == util, "require 缓存")
assert(require("pkg").kind == "init", "init.lua")
return { value = 42, util = util }
