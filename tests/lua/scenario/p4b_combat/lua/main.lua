-- P4b 场景测试：dd.buff / dd.effect / actor 的 buff 方法、脚本 Effect、脚本 Buff、规则、dd.fs 的写入，在城镇、走廊、战斗里
-- 各用一遍。日志在 DDLua.log，以 [p4b_combat] 开头，每行是「通过 [标签]」「失败 [标签]」或「信息 [标签]」。
-- 流程参考 tests/plugins/test_combat.cpp（P4a 的 native 验证插件），P4c 的场景会一起跑它。
--
-- 会改游戏状态（每次进存档，城镇、走廊、战斗各一轮，每轮约 1.5 秒，结束时复原）：
--   * 给一个英雄临时加几条测试 buff（来源 skill，after_turn 3 次），一轮结束按标签、guid、id 删干净
--   * 走廊、战斗里施加两个运行时定义的 effect（执行者自己 speed +1、加一条脚本 Buff），结束时删掉加上的条目
--   * 战斗里给一个怪物加两条脚本 Buff：一条按 guid 删掉（on_remove 收到 plugin），一条留到战斗结束（怪物随战斗销毁，状态按 gone 丢弃）
--   * 规则 p4b_rule 恒为真：带它的 p4b_rule_buff 给速度 +1
--   * 规则 p4b_live 的结果由这一轮控制（先真后假），带它的 p4b_live_buff / p4b_live_neg（速度 +0，remove_if_not_active，
--     后者 is_false_rule）在游戏刷新属性时按规则的结果留下或被删掉：Lua 的结果是不是真的生效，看条目在不在（审核 C-Y4）
-- 万一中途存了档，测试 buff 是来源 skill，回城时游戏会清掉。on_damage 只记录，不改伤害。
--
-- 找英雄：城镇里在名册里按 hero id 扫描（0..4000，每帧 200 个）；地牢里用 DDCore 枚举的队伍和怪物（dd.actor.party、
-- dd.actor.monsters，F5），不用先在事件里见过：走廊里游戏几乎不派发带角色的事件，开战后 5～9 秒才第一次出手（第三轮审核 R3-B1）。
-- 场合：dd.actor.in_battle() 为真是战斗（读进战斗中的存档时游戏不派发 battle.start，原来按 app.state 把这场战斗当成走廊，
-- 审核 R3-B3），否则队伍不空就是走廊；城镇和回城途中按 app.state（到了 as_town_visit_update 才算城镇，结算后的
-- as_raid_result_to_town_transition、load_town 线程在跑的 as_town_visit_start 是「途中」，不做任何一轮）。
-- 加载线程在跑时读接口、枚举返回「加载中」，这时也不开始（审核 R2A-S1）。
-- 一轮里的条数按这一轮开始时的条数算增量（英雄身上万一留着以前中途存档存下的测试 buff，也不影响判定）。

local ID = {
  spd = "p4b_spd",           -- 普通的运行时定义：speed +2
  script = "p4b_script",     -- 脚本 Buff：带四个回调，状态表记录调用
  rule_buff = "p4b_rule_buff", -- 带规则 is_actor_status + p4b_rule：speed +1
  fx = "p4b_fx",             -- effect：执行者自己 speed +1，1 回合
  fx_script = "p4b_fx_script", -- effect：给执行者加脚本 Buff（游戏自己加的载体，状态是空表）
  rule = "p4b_rule",
  live = "p4b_live",           -- 结果由一轮的步骤控制的规则
  live_buff = "p4b_live_buff", -- is_actor_status + p4b_live，remove_if_not_active：规则不成立时游戏刷新属性时删掉
  live_neg = "p4b_live_neg",   -- 同上，is_false_rule：规则成立时被删掉
}

local function result(tag, ok, text)
  if ok then
    dd.log.info("通过 [" .. tag .. "] " .. text)
  else
    dd.log.error("失败 [" .. tag .. "] " .. text)
  end
  return ok
end

local function info(tag, text)
  dd.log.info("信息 [" .. tag .. "] " .. text)
end

local function try(f, ...)
  local ok, err = pcall(f, ...)
  return ok, type(err) == "string" and err or tostring(type(err))
end

-- ---------------------------------------------------------------- 场合和英雄

local app = (select(2, dd.store_status()) or ""):find("地牢") and "raid" or "start"  -- start：刚进存档，还没有 app.state
local where = "transit"   -- 下面马上 refresh_where 一次
local where_since = dd.time()
local town_hero = nil
local scanning = false

dd.on("app.state", function(ev)
  local to = ev.to_name or ""
  if to:find("town") then
    -- 到了 as_town_visit_update 才算城镇：之前的过渡里 Raid::Current 不一定清掉，load_town 线程在跑时读不到 buff
    app = to:find("town_visit_update") and "town" or "transit"
  elseif to:find("raid") then
    app = "raid"
  end
end)

-- town、corridor、battle、transit；加载中读不到时返回 nil
local function where_now()
  if app == "transit" or app == "town" then
    return app
  end
  local ib = dd.actor.in_battle()
  if ib == nil then
    return nil
  end
  if ib then
    return "battle"
  end
  local party = dd.actor.party()
  if party == nil then
    return nil
  end
  if #party > 0 then
    return "corridor"
  end
  return app == "raid" and "transit" or "town"  -- 地牢里的存档在 load_raid 线程跑完之前还没有队伍
end

local function refresh_where()
  local w = where_now()
  if w and w ~= where then
    where = w
    where_since = dd.time()
  end
end
refresh_where()

-- 加载线程在跑：读接口返回 nil 和「加载中」开头的原因（R2A-S1）
local function loading_now()
  local d, why = dd.buff.def(ID.spd)
  return d == nil and type(why) == "string" and why:find("^加载中") ~= nil
end

-- 活着的角色（尸体的职业是 corpse 开头的）
local function alive(list)
  local out = {}
  for _, a in ipairs(list or {}) do
    if a:valid() and (a.hp or 0) > 0 and not tostring(a.class or ""):find("^corpse") then
      out[#out + 1] = a
    end
  end
  return out
end

dd.on("battle.start", function() refresh_where() end)
dd.on("battle.end", function() refresh_where() end)

local function pick_party()
  return alive(dd.actor.party())[1]
end

-- 生命最多的怪物：这一轮要在它身上加两条、1.3 秒后按 guid 删一条，选最不容易被打死的
local function pick_monster()
  local best = nil
  for _, m in ipairs(alive(dd.actor.monsters())) do
    if not best or (m.hp or 0) > (best.hp or 0) then
      best = m
    end
  end
  return best
end

-- 名册扫描（协程，每帧 200 个 id）
local function scan_town()
  if scanning then
    return
  end
  scanning = true
  dd.spawn(function()
    for id = 0, 4000 do
      local h = dd.actor.hero(id)
      if h and (h.hp or 0) > 0 then
        town_hero = h
        info("scan", "城镇英雄 " .. tostring(h) .. "（职业 " .. tostring(h.class) .. "，生命 " .. tostring(h.hp) .. "）")
        break
      end
      if id % 200 == 199 then
        coroutine.yield()
      end
    end
    if not town_hero then
      info("scan", "名册里 0..4000 没有找到可用的英雄")
    end
    scanning = false
  end)
end

-- ---------------------------------------------------------------- 定义（每次进存档）

local calls = { apply = 0, remove = 0, tick = 0, damage = 0 }
local removed = {}        -- guid -> reason（on_remove 记的）
local logged = { apply = 0, remove = 0, tick = 0, damage = 0 }
local function cblog(kind, text)
  calls[kind] = calls[kind] + 1
  if logged[kind] < 20 then
    logged[kind] = logged[kind] + 1
    info("cb." .. kind, text)
  end
end

local function define_all()
  -- 枚举表
  result("enum", dd.buff.enum("stat", "combat_stat_add") == 2 and dd.buff.enum("rule", "is_actor_status") == 42 and
    dd.buff.enum("source", "bsrc_trinket") == 6 and #dd.buff.enum("stat") == 102 and dd.buff.enum("duration", 9) == "after_turn",
    "stat 102 项，is_actor_status = 42，bsrc_trinket = 6，after_turn = 9")
  local base = { stat = "combat_stat_add", sub_stat = "speed_rating", duration = 3, duration_type = "after_turn", source = "skill" }
  local function def(t)
    local d = {}
    for k, v in pairs(base) do d[k] = v end
    for k, v in pairs(t) do d[k] = v end
    return dd.buff.define(d)
  end
  local ok1, r1 = def({ id = ID.spd, amount = 2 })
  local ok2, r2 = def({ id = ID.rule_buff, amount = 1, rule = "is_actor_status", rule_string = ID.rule })
  local ok3, r3 = def({ id = ID.script, amount = 0,
    on_apply = function(b, st)
      st.applied = (st.applied or 0) + 1
      cblog("apply", ("%s guid=%d id=%s mine=%s state.n=%s state.where=%s applied=%d"):format(tostring(b.actor), b.guid, b.id, tostring(b.mine),
        tostring(st.n), tostring(st.where), st.applied))
    end,
    on_remove = function(b, st, reason)
      removed[b.guid] = reason
      cblog("remove", ("%s guid=%d reason=%s state.n=%s applied=%s ticks=%s"):format(tostring(b.actor), b.guid, reason, tostring(st.n),
        tostring(st.applied), tostring(st.ticks)))
    end,
    on_tick = function(b, st, before, after)
      st.ticks = (st.ticks or 0) + 1
      cblog("tick", ("%s guid=%d %d->%d ticks=%d"):format(tostring(b.actor), b.guid, before, after, st.ticks))
    end,
    on_damage = function(b, st, ev)
      cblog("damage", ("%s guid=%d damage=%.1f type=%d（只记录，不改）"):format(tostring(b.actor), b.guid, ev.damage, ev.damage_type))
    end })
  local ok4, r4 = def({ id = ID.live_buff, amount = 0, duration = -1, duration_type = "till_removed", rule = "is_actor_status",
    rule_string = ID.live, remove_if_not_active = true })
  local ok5, r5 = def({ id = ID.live_neg, amount = 0, duration = -1, duration_type = "till_removed", rule = "is_actor_status",
    rule_string = ID.live, remove_if_not_active = true, is_false_rule = true })
  result("define", ok1 and r1 == ID.spd and ok2 and ok3 and ok4 and ok5, ("p4b_spd=%s p4b_rule_buff=%s p4b_script=%s p4b_live_buff=%s p4b_live_neg=%s"):format(
    tostring(r1), tostring(r2), tostring(r3), tostring(r4), tostring(r5)))
  local again, ra = def({ id = ID.spd, amount = 2 })
  result("define.again", again == true and dd.buff.def(ID.spd).mod == dd.id and dd.buff.def(ID.script).script == true,
    "同一个 Mod 重复定义更新字段：" .. tostring(ra))
  -- 字段本身合法（带 sub_stat），被拒绝只能是因为 SPD5 是游戏数据（F1 起不带 sub_stat 会先因参数无效被拒）
  local game, why = dd.buff.define({ id = "SPD5", stat = "combat_stat_add", sub_stat = "speed_rating" })
  result("define.game", game == nil and type(why) == "string" and why:find("游戏数据") ~= nil, "游戏数据 SPD5 不能覆盖：" .. tostring(why))
  local d = dd.buff.def(ID.spd)
  result("def", d and d.stat == "combat_stat_add" and d.sub_stat == "speed_rating" and d.amount == 2 and d.duration == 3 and d.source == "skill",
    ("def(p4b_spd)：%s %s %s %s"):format(tostring(d and d.stat), tostring(d and d.amount), tostring(d and d.duration_type), tostring(d and d.source)))
  -- effect
  local okfx, n = dd.effect.define(('effect: .name "%s" .target "performer" .chance 100%% .combat_stat_buff 1 .speed_rating_add 1 ' ..
    '.on_hit true .on_miss true .duration 1\r\neffect: .name "%s" .target "performer" .chance 100%% .buff_ids %s .on_hit true ' ..
    '.on_miss true .duration 1\r\n'):format(ID.fx, ID.fx_script, ID.script))
  result("effect.define", okfx == true and n == 2, "两个 effect：" .. tostring(n))
  local clash, why2 = dd.effect.define('effect: .name "rakebuff_5" .target "performer"\r\n')
  result("effect.define.game", clash == nil, "游戏数据 rakebuff_5 不能重新定义：" .. tostring(why2))
  local e = dd.effect.get(ID.fx)
  result("effect.get", e and e.runtime == true and e.name == ID.fx, ("p4b_fx：runtime=%s target=%s duration=%s"):format(tostring(e and e.runtime),
    tostring(e and e.target), tostring(e and e.duration)))
  local rake = dd.effect.get("rakebuff_5")
  info("effect.rake", rake and ("rakebuff_5：target=%s chance=%s buff_ids=%s on_hit=%s"):format(tostring(rake.target), tostring(rake.chance),
    tostring(rake.buff_id_count), tostring(rake.on_hit)) or "没有 rakebuff_5")
end

-- 规则：恒为真；第一次调用时试一下写操作（要报错）
local rule_hits, rule_write_tried = 0, false
local rule = dd.buff.rule(ID.rule, function(self, target, buff)
  rule_hits = rule_hits + 1
  if not rule_write_tried then
    rule_write_tried = true
    local ok, err = try(function() return self:add_buff(ID.spd) end)
    result("rule.readonly", not ok and err:find("规则回调"), "规则回调里 add_buff 报错：" .. err)
    info("rule.args", ("self=%s target=%s id=%s rule=%s subject=%s"):format(tostring(self), tostring(target), buff.id, buff.rule, tostring(buff.subject)))
  end
  return true
end)
result("rule.register", rule ~= nil and rule:active(), "注册规则 " .. ID.rule)

-- 结果由一轮的步骤控制的规则：live_value 为 nil 时交还原函数（不在测试中）
local live_value, live_hits = nil, 0
local live = dd.buff.rule(ID.live, function(self, target, buff)
  live_hits = live_hits + 1
  return live_value
end)
result("rule.register.live", live ~= nil and live:active(), "注册规则 " .. ID.live)

-- 脚本 Effect：只在名字以 p4b_ 开头的 effect 上进 Lua
local fx_seen = 0
dd.effect.register("p4b_*", function(ev)
  fx_seen = fx_seen + 1
  if fx_seen <= 10 then
    info("effect.register", ("%s 施加到 %s（执行者 %s，effect 句柄 %s）"):format(ev.effect_name, tostring(ev.target), tostring(ev.performer),
      tostring(ev.effect)))
  end
end)

-- 技能句柄
local skills = 0
dd.on("skill.pre", function(ev)
  skills = skills + 1
  if skills <= 8 then
    local sk = ev.skill
    if sk then
      info("skill", ("%s：%s 等级 %d type=%s friendly=%s effects=[%s]"):format(tostring(sk.performer), sk.id, sk.level, tostring(sk.type),
        tostring(sk.friendly), table.concat(sk.effects or {}, ",")))
    else
      info("skill", "技能句柄为 nil（技能不在执行者职业的表里）")
    end
  end
end)

-- buff.* 主题（只计数，前几条写日志）。deferred 是加载线程上发生、排到主线程派发的（审核 A 的运行证据第 2 条）
local buff_events = { add = 0, remove = 0, tick = 0 }
local deferred = { add = 0, remove = 0, tick = 0 }
local remove_reason = {}  -- 本 Mod 加的条目：guid -> buff.remove 的 reason
local reasons = {}        -- 全部 buff.remove 的 reason -> 次数（汇总行里列出）
for _, kind in ipairs({ "add", "remove", "tick" }) do
  dd.on("buff." .. kind, function(ev)
    buff_events[kind] = buff_events[kind] + 1
    if kind == "remove" then
      reasons[ev.reason] = (reasons[ev.reason] or 0) + 1
    end
    if ev.deferred then
      deferred[kind] = deferred[kind] + 1
      if deferred[kind] <= 5 then
        info("buff.deferred", ("buff.%s 延迟事件：actor_kind=%s actor_id=%s guid=%d id=%s tag=%s%s"):format(kind, tostring(ev.actor_kind),
          tostring(ev.actor_id), ev.guid, ev.id, ev.tag, kind == "remove" and (" reason=" .. ev.reason) or ""))
      end
    end
    if ev.plugin == "DDLua" and ev.tag:find("^p4b_combat:") then
      if kind == "remove" then
        remove_reason[ev.guid] = ev.reason
      end
      if buff_events[kind] <= 200 then
        info("buff." .. kind, ("%s guid=%d id=%s tag=%s%s"):format(tostring(ev.actor), ev.guid, ev.id, ev.tag,
          kind == "remove" and (" reason=" .. ev.reason) or ""))
      end
    end
  end)
end

-- ---------------------------------------------------------------- 一轮

local done = {}
local running = false

local function count(h, filter)
  local list = h:buffs(filter)
  return list and #list or -1
end

local function round(w, h, monster)
  running = true
  info("round", ("where=%s hero=%s buffs=%d"):format(w, tostring(h), count(h)))
  -- 这一轮开始时的条数：之后都按增量判定
  local base = { mine = count(h, { mine = true }), spd = count(h, { id = ID.spd }), script = count(h, { id = ID.script }) }
  local guids = {}
  -- 添加：带标签、脚本 Buff（初始状态）、规则 buff、同 id 叠加
  local ok, g = h:add_buff(ID.spd, { tag = "round" })
  local b = ok and h:buff(g)
  result("add." .. w, ok and math.type(g) == "integer" and b and b.tag == "round" and b.mine and b.mod == dd.id,
    ("add_buff(p4b_spd, tag=round) -> %s %s，快照 tag=%s mine=%s"):format(tostring(ok), tostring(g), tostring(b and b.tag), tostring(b and b.mine)))
  guids.tag = ok and g
  local applied = calls.apply
  local oks, gs = h:add_buff(ID.script, { tag = "script", state = { n = 1, where = w } })
  local st = oks and dd.buff.state(h, gs)
  result("script.apply." .. w, oks and calls.apply == applied + 1 and st and st.n == 1 and st.applied == 1,
    ("脚本 Buff guid=%s：on_apply 在 add_buff 返回之前调用，初始状态 n=%s applied=%s"):format(tostring(gs), tostring(st and st.n), tostring(st and st.applied)))
  guids.script = oks and gs
  local okr, gr = h:add_buff(ID.rule_buff, { tag = "rule" })
  guids.rule = okr and gr
  local okk, gk = h:add_buff(ID.spd)
  guids.stack = okk and gk
  result("stack." .. w, okr and okk and count(h, { id = ID.spd }) == base.spd + 2 and count(h, { mine = true }) == base.mine + 4,
    ("同 id 叠加：p4b_spd %d 条，本 Mod 的 %d 条（这一轮之前 %d、%d 条）"):format(count(h, { id = ID.spd }), count(h, { mine = true }), base.spd, base.mine))
  -- effect：城镇里被拒绝
  local before = {}
  for _, x in ipairs(h:buffs() or {}) do before[x.guid] = true end
  local e = dd.effect.get(ID.fx)
  local okf, rf = e:apply(h)
  if w == "town" then
    result("effect.town", okf == nil and type(rf) == "string", "城镇里施加 effect 被拒绝：" .. tostring(rf))
  else
    result("effect." .. w, okf == true and rf.count >= 1 and #rf >= 1,
      ("施加 p4b_fx：count=%s，执行者身上新增 %s 条"):format(tostring(okf and rf.count), tostring(okf and #rf)))
    local before_apply = calls.apply
    local okg, rg = dd.effect.get(ID.fx_script):apply(h)
    info("effect.script." .. w, ("施加 p4b_fx_script：%s count=%s，on_apply 调用 %d 次（游戏自己加的载体，状态是空表）"):format(tostring(okg),
      tostring(okg and rg.count or rg), calls.apply - before_apply))
  end
  guids.fx = {}
  for _, x in ipairs(h:buffs() or {}) do
    if not before[x.guid] and not x.mine then
      guids.fx[#guids.fx + 1] = x.guid
    end
  end
  -- 事件回调里的写操作排队：buff.add 回调看到 tag=queue 的这条时，在回调里删掉它、再加一条
  local queued = {}
  dd.once("buff.add", function(ev)
    local okq, rq = h:remove_buffs{ guid = ev.guid, done = function(o, n) queued.remove = { o, n } end }
    local oka, ra = h:add_buff(ID.spd, { tag = "queued_add", done = function(o, gg) queued.add = { o, gg } end })
    result("queue." .. w, okq == true and rq == nil and oka == true and ra == nil,
      ("buff.add 回调里：remove_buffs -> %s %s，add_buff -> %s %s（都排队，下一帧 done）"):format(tostring(okq), tostring(rq), tostring(oka), tostring(ra)))
  end, { filter = { id = ID.spd, tag = dd.id .. ":queue" } })
  h:add_buff(ID.spd, { tag = "queue" })
  -- 战斗里：给一个怪物加两条脚本 Buff，一条这一轮里按 guid 删掉，一条留到战斗结束
  local mon_rm = nil
  if monster then
    local okm, gm = monster:add_buff(ID.script, { tag = "mon", state = { n = 2, monster = true } })
    local stm = okm and dd.buff.state(monster, gm)
    local okm2, gm2 = monster:add_buff(ID.script, { tag = "mon_rm", state = { n = 3, monster = true } })
    mon_rm = okm2 and gm2 or nil
    result("monster." .. w, okm == true and okm2 == true and stm and stm.n == 2 and monster:buff(gm) ~= nil and monster:buff(gm2) ~= nil,
      ("怪物 %s 加两条脚本 Buff -> %s %s、%s %s，状态 n=%s（第一条留到战斗结束，之后按 gone 丢弃）"):format(tostring(monster), tostring(okm), tostring(gm),
        tostring(okm2), tostring(gm2), tostring(stm and stm.n)))
  end
  dd.wait(0.2)
  -- 排队添加的 done 收到的 guid 就是游戏里那一条（标签对得上）
  local qb = queued.add and math.type(queued.add[2]) == "integer" and h:buff(queued.add[2]) or nil
  result("queue.done." .. w, queued.remove and queued.remove[1] == true and queued.remove[2] == 1 and queued.add and queued.add[1] == true and
    qb ~= nil and qb.tag == "queued_add" and qb.mine == true,
    ("done：remove %s，add %s（这个 guid 的条目 tag=%s mine=%s）"):format(queued.remove and tostring(queued.remove[2]) or "没有回调",
      queued.add and tostring(queued.add[2]) or "没有回调", tostring(qb and qb.tag), tostring(qb and qb.mine)))
  -- 规则的结果真的生效（审核 C-Y4）：remove_if_not_active 的条目在游戏刷新属性时，规则不成立就被删掉。
  -- add_buff、remove_buffs 都让游戏刷新一次属性（DDCore 默认 refresh）
  do
    local hits0 = live_hits
    live_value = true
    local oka, ga = h:add_buff(ID.live_buff, { tag = "live" })     -- 成立：留下
    local okb, gb = h:add_buff(ID.live_neg, { tag = "live_neg" })  -- is_false_rule：成立取反成不成立，加完的刷新里就删掉
    local a_kept = oka == true and h:buff(ga) ~= nil
    local b_gone = okb == true and h:buff(gb) == nil
    live_value = false
    h:add_buff(ID.spd, { tag = "kick" })                           -- 刷新两次：加一条、删一条
    h:remove_buffs({ tag = "kick" })
    local a_gone = oka == true and h:buff(ga) == nil
    local okc, gc = h:add_buff(ID.live_neg, { tag = "live_neg" })  -- 不成立取反成成立：留下
    local c_kept = okc == true and h:buff(gc) ~= nil
    live_value = nil
    local judged = live_hits - hits0
    h:remove_buffs({ tag = "live" })
    h:remove_buffs({ tag = "live_neg" })
    result("rule.live." .. w, a_kept and b_gone and a_gone and c_kept and judged > 0,
      ("Lua 返回真：p4b_live_buff 留下 %s、p4b_live_neg 被删 %s；返回假并刷新后：p4b_live_buff 被删 %s（reason %s）、p4b_live_neg 留下 %s；" ..
        "规则回调 %d 次"):format(tostring(a_kept), tostring(b_gone), tostring(a_gone), tostring(ga and remove_reason[ga]), tostring(c_kept), judged))
  end
  dd.wait(1.1)
  -- 删干净
  local function rm(label, filter, expect)
    local okd, n = h:remove_buffs(filter)
    result("remove." .. label .. "." .. w, okd == true and n == expect, ("remove_buffs %s -> %s（期望 %d）"):format(label, tostring(n), expect))
  end
  rm("tag", { tag = "round" }, guids.tag and 1 or 0)
  rm("guid", { guid = guids.stack or 0 }, guids.stack and 1 or 0)
  rm("rule", { tag = "rule" }, guids.rule and 1 or 0)
  local reasons_before = removed[guids.script or -1]
  rm("script", { tag = "script" }, guids.script and 1 or 0)
  result("script.remove." .. w, guids.script and removed[guids.script] == "plugin" and dd.buff.state(h, guids.script) == nil and reasons_before == nil,
    ("on_remove reason=%s，状态已丢弃"):format(tostring(guids.script and removed[guids.script])))
  rm("queued_add", { tag = "queued_add" }, queued.add and queued.add[1] and 1 or 0)
  for _, guid in ipairs(guids.fx) do
    local still = h:buff(guid) ~= nil
    rm("effect_buff", { guid = guid }, still and 1 or 0)
  end
  local monster_skipped = false
  if monster and mon_rm then
    if not (monster:valid() and (monster.hp or 0) > 0 and not tostring(monster.class or ""):find("^corpse")) then
      -- 这一轮里怪物被打死了（游戏删掉它身上的 buff）或战斗结束了：不判定，换一个活着的怪物再做战斗那一轮
      -- （这场还有怪物就在这场，否则下一场；审核 R3-B1 的第 4 点）
      monster_skipped = true
      info("monster.remove." .. w, ("怪物 %s 在这一轮里死了或战斗结束了（guid %d 的条目 reason=%s），不判定，换一个怪物再做战斗那一轮"):format(tostring(monster),
        mon_rm, tostring(removed[mon_rm])))
    else
      local okd, n = monster:remove_buffs({ guid = mon_rm })
      result("monster.remove." .. w, okd == true and n == 1 and removed[mon_rm] == "plugin" and dd.buff.state(monster, mon_rm) == nil,
        ("怪物 %s 按 guid 删脚本 Buff（guid %d）-> %s %s，on_remove reason=%s，状态已丢弃"):format(tostring(monster), mon_rm, tostring(okd), tostring(n),
          tostring(removed[mon_rm])))
    end
  end
  local left = (count(h, { mine = true }) - base.mine) + (count(h, { id = ID.script }) - base.script) + (count(h, { id = ID.spd }) - base.spd)
  result("rounddone." .. w, left == 0, ("剩下的测试 buff %d 条，规则判定 %d 次，回调 apply=%d remove=%d tick=%d damage=%d"):format(left, rule_hits,
    calls.apply, calls.remove, calls.tick, calls.damage))
  done[w] = not monster_skipped
  running = false
end

dd.every(0.5, function()
  refresh_where()
  if running or where == "transit" or done[where] or dd.time() - where_since < 2 or loading_now() then
    return
  end
  local h, m
  if where == "town" then
    h = town_hero
    if not h or not h:valid() then
      town_hero = nil
      scan_town()
      return
    end
  else
    h = pick_party()
    if not h then
      return
    end
    if where == "battle" then
      m = pick_monster()
      if not m then
        return
      end
    end
  end
  running = true
  dd.spawn(function()
    local ok, err = pcall(round, where, h, m)
    if not ok then
      result("round." .. where, false, "这一轮出错：" .. tostring(err))
      done[where] = true
      running = false
    end
  end)
end)

-- 每 30 秒一行汇总
dd.every(30, function()
  local gone = 0
  for _, r in pairs(removed) do gone = gone + (r == "gone" and 1 or 0) end
  local rs = {}
  for r, n in pairs(reasons) do rs[#rs + 1] = r .. "=" .. n end
  table.sort(rs)
  info("summary", ("buff.add %d、buff.remove %d、buff.tick %d（延迟事件 add %d、remove %d、tick %d）；buff.remove 的原因 [%s]；脚本 Buff 回调 apply=%d remove=%d tick=%d damage=%d（gone %d）；规则 %d 次（p4b_live %d 次）；p4b_* effect %d 次；轮次 town=%s corridor=%s battle=%s"):format(
    buff_events.add, buff_events.remove, buff_events.tick, deferred.add, deferred.remove, deferred.tick, table.concat(rs, " "), calls.apply, calls.remove,
    calls.tick, calls.damage, gone, rule_hits, live_hits, fx_seen, tostring(done.town), tostring(done.corridor), tostring(done.battle)))
end)

-- ---------------------------------------------------------------- dd.fs 的写入（数据目录）

local function fs_check()
  local text = ('{"loads":%d,"time":%.1f}'):format((dd.state.loads or 0) + 1, dd.time())
  dd.state.loads = (dd.state.loads or 0) + 1
  local okw, why = dd.fs.write("last_run.json", text)
  local back = dd.fs.read("last_run.json", { root = "data" })
  local list = dd.fs.list{ root = "data" } or {}
  local tmp = dd.fs.write("tmp/scratch.txt", "x") and dd.fs.remove("tmp/scratch.txt")
  local bad = dd.fs.write("../escape.txt", "x")
  result("fs", okw == true and back == text and tmp == true and bad == nil and dd.fs.read("last_run.json") == nil,
    ("写入 last_run.json %s，读回 %s，列表 [%s]，删除 %s，../ 被拒绝 %s"):format(tostring(okw or why), tostring(back == text), table.concat(list, ","),
      tostring(tmp), tostring(bad == nil)))
end

-- ---------------------------------------------------------------- 进存档

define_all()
fs_check()
scan_town()
info("load", "main.lua 执行完毕：城镇、走廊、战斗各一轮，日志见 [p4b_combat] 开头的行")
return {}
