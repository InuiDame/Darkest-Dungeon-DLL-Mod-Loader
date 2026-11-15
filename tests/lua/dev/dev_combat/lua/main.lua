-- P4b 主菜单自检（rig.py deploy lua-p4b）：开发用全局加载，在主菜单上跑 Buff 与 Effect 的只读 API，写操作要被拒绝（不在存档里）。
-- 结果写进 DDLua.log，以 [dev_combat] 开头；预期的拒绝写成 INFO，不符合预期的写成 ERROR。不注册规则（规则会让 DDCore 装 479CD0 的 hook），
-- 不碰 actor（主菜单没有存档，句柄都找不到）。
local passed, failed = 0, 0
local function check(name, fn)
  local ok, err = pcall(fn)
  if ok then
    passed = passed + 1
  else
    failed = failed + 1
    dd.log.error("自检失败：" .. name .. "：" .. tostring(err))
  end
end

check("枚举表", function()
  assert(dd.buff.enum("stat", "combat_stat_add") == 2 and #dd.buff.enum("stat") == 102)
  assert(dd.buff.enum("source", "bsrc_trinket") == 6 and dd.buff.enum("source", "trinket") == 6 and dd.buff.enum("source", 1) == "skill")
  assert(#dd.buff.enum("duration") == 11 and dd.buff.enum("duration_type", "after_turn") == 9)
  assert(#dd.buff.enum("rule") == 55 and dd.buff.enum("rule", 42) == "is_actor_status" and dd.buff.enum("rule", 99) == nil)
end)
check("存档外查定义和 effect（只记录：Buff 库和 effect 表在进存档时建立）", function()
  local def, why = dd.buff.def("SPD5")
  dd.log.info("dd.buff.def(\"SPD5\") -> " .. tostring(def and def.stat) .. "，" .. tostring(why))
  local e, why2 = dd.effect.get("rakebuff_5")
  dd.log.info("dd.effect.get(\"rakebuff_5\") -> " .. tostring(e) .. "，" .. tostring(why2))
  assert(def == nil or type(def.stat) == "string")
end)
check("写操作被拒绝（不在存档里）", function()
  local ok, why = dd.buff.define{ id = "dev_combat_buff", stat = "combat_stat_add", sub_stat = "speed_rating", amount = 1 }
  dd.log.info("预期的拒绝：dd.buff.define -> " .. tostring(ok) .. "，" .. tostring(why))
  assert(ok == nil and why:find("不在存档里"), why)
  local ok2, why2 = dd.effect.define('effect: .name "dev_combat_fx" .target "performer"\r\n')
  dd.log.info("预期的拒绝：dd.effect.define -> " .. tostring(ok2) .. "，" .. tostring(why2))
  assert(ok2 == nil and why2:find("不在存档里"), why2)
end)
check("参数写错直接报错", function()
  assert(not pcall(dd.buff.define, {}) and not pcall(dd.buff.define, { id = "x", stat = "nope" }))
  assert(not pcall(dd.buff.enum, "nope") and not pcall(dd.effect.define, "没有块"))
  assert(not pcall(dd.effect.register, "a*b", function() end))
end)
check("主菜单上没有英雄", function()
  assert(dd.actor.hero(0) == nil and dd.actor.hero(1) == nil)
end)
check("脚本 Effect 的订阅（不派发，只验证能注册和撤销）", function()
  local sub = assert(dd.effect.register("dev_combat_*", function() end))
  assert(sub.topic == "effect.pre" and sub:off() == true)
end)
check("数据目录的写入", function()
  assert(dd.fs.write("selftest.txt", "dev_combat") == true)
  assert(dd.fs.read("selftest.txt", { root = "data" }) == "dev_combat")
  local list = dd.fs.list{ root = "data" }
  assert(list and #list >= 1)
  assert(dd.fs.remove("selftest.txt") == true and not dd.fs.exists("selftest.txt", { root = "data" }))
  assert(dd.fs.write("../escape.txt", "x") == nil)
end)
local stats = dd.stats()
print(("P4b 主菜单自检：通过 %d 项，失败 %d 项；订阅 %d 个、排队 %d 个、规则 %d 条"):format(passed, failed, stats.subscriptions, stats.pending, stats.rules))
return { passed = passed, failed = failed }
