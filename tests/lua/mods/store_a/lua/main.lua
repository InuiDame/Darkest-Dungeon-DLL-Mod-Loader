-- 夹具（P4c1）：dd.store 和脚本 Buff 状态随存档保存的离线测试。main.lua 运行时 dd.store 已经从旁路读好，
-- 脚本 Buff 的状态在全部 main.lua 之后导入（这里定义脚本 Buff，导入才认得它）
MAIN = { counter = dd.store.counter, store = dd.store }
MAIN.status, MAIN.reason = dd.store_status()
APPLIED, REMOVED = 0, {}
assert(dd.buff.define{ id = "lua_store_buff", stat = "combat_stat_add", sub_stat = "protection_rating", amount = 0, duration = -1,
    duration_type = "till_removed",
    on_apply = function() APPLIED = APPLIED + 1 end,
    on_remove = function(_, _, reason) REMOVED[#REMOVED + 1] = reason end })
return {}
