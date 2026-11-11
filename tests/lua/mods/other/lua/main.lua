-- 夹具：依赖 basic，通过 dd.import 拿它的导出；全局表互相隔离
local basic = dd.import("basic")
assert(basic and basic.value == 42, "import basic")
assert(BASIC_LOADED == nil, "全局隔离")
OTHER_LOADED = true
