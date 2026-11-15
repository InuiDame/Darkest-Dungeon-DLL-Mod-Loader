-- P4c2 场景测试：驱动 shield_lite（DDShield 复刻）的各种护盾规则，让检查脚本能逐条核对公式。
-- 自然的触发（圣骑士的 Smite 给自己加护盾、怪物打有护盾的英雄）不在这里，这里只补自然流程里碰不到的：
-- 每次进存档的前两场战斗，进入战斗 2 秒后用 dd.effect.get(name):apply 依次施加上限、加满、破盾、获得、百分比、
-- 设定、每回合再生和衰减、穿透，给每个怪物加护盾（英雄打怪物时看怪物一侧的吸收），再给英雄挂一条带 shielded 规则的
-- 探针 buff（速度 +0，战斗结束时游戏删掉）。结算和日志都在 shield_lite 里（SHIELD 开头的行），这里只记施加的结果。
-- 规则的结果是不是真的生效（审核 C-Y4）：给有护盾和没有护盾的英雄各挂一条 remove_if_not_active 的 shielded buff
-- （另一种是 is_false_rule），游戏刷新属性时规则不成立的被删掉，看条目在不在。
-- 死门（可选的运行证据，审核 A）：英雄在死门上挨打、身上没有护盾时（shield_lite 在生命 ≤ 0 时清零护盾），第一次写一半的
-- 吸收量、第二次写全部，记下 damage.post 的结果：DDCore 在死门上部分吸收不补生命，全部吸收按格挡处理（不判定死亡）。
-- 战斗、队伍、怪物都靠 DDCore 的枚举（dd.actor.in_battle、party、monsters，F5），不靠「在事件里见过」：真实游戏开战后
-- 5～9 秒才第一次出手，读进战斗中的存档时也不派发 battle.start（第三轮审核 R3-B1、B3）。读进来的战斗也算一场。
-- 驱动做到一半战斗就结束了（撤退、打完）时这场不算，下一场再驱动；驱动之后新出现的怪物（召唤、增援）也补上 10 点护盾。
-- 依赖 shield_lite（manifest 的 dependencies），没有它时什么都不做。
local shield = dd.import("shield_lite")

local function result(tag, ok, text)
  if ok then
    dd.log.info("通过 [" .. tag .. "] " .. text)
  else
    dd.log.error("失败 [" .. tag .. "] " .. text)
  end
end

local function info(tag, text)
  dd.log.info("信息 [" .. tag .. "] " .. text)
end

local PROBE = "p4c_shield_probe"
local LIVE, LIVE_NEG = "p4c_shield_live", "p4c_shield_live_neg"
local function define(id, extra)
  local def = { id = id, stat = "combat_stat_add", sub_stat = "speed_rating", amount = 0, duration = -1, duration_type = "till_removed",
    source = "skill", rule = "is_actor_status", rule_string = "shielded", remove_on_battle_complete = true }
  for k, v in pairs(extra or {}) do def[k] = v end
  local okd, whyd = dd.buff.define(def)
  return okd == true, whyd
end
local ok1, why1 = define(PROBE)
local ok2, why2 = define(LIVE, { remove_if_not_active = true })
local ok3, why3 = define(LIVE_NEG, { remove_if_not_active = true, is_false_rule = true })
result("define", ok1 and ok2 and ok3, ("规则探针 buff %s、%s（remove_if_not_active）、%s（另加 is_false_rule），都是 is_actor_status shielded、战斗结束删掉：%s %s %s"):format(
  PROBE, LIVE, LIVE_NEG, tostring(why1), tostring(why2), tostring(why3)))

local battles, driving = 0, false  -- battles：本次进存档做完驱动的场数（最多 2）
local cur = nil                    -- 当前这场战斗：{ n, since, driven, shielded = { [怪物] = true } }

-- 活着的角色（尸体的职业是 corpse 开头的，生命也可能大于 0）
local function alive(list, skip)
  local out = {}
  for _, a in ipairs(list or {}) do
    if a ~= skip and a:valid() and (a.hp or 0) > 0 and not tostring(a.class or ""):find("^corpse") then
      out[#out + 1] = a
    end
  end
  return out
end

local function party()
  return alive(dd.actor.party())
end

local function monsters()
  return alive(dd.actor.monsters())
end

-- 队伍里除 h 以外活着的英雄：优先不是圣骑士的（圣骑士留给 Smite 的自然流程）、还没有护盾的
local function party_other(h)
  local best = nil
  for _, x in ipairs(party()) do
    if x ~= h then
      local s = shield and shield.get(x)
      local score = (x.class ~= "crusader" and 2 or 0) + ((not s or s.cur <= 0) and 1 or 0)
      if not best or score > best.score then
        best = { a = x, score = score }
      end
    end
  end
  return best and best.a
end

-- 这场战斗还在（驱动的每一步之前看一次）
local function still(b)
  return cur == b and dd.actor.in_battle() == true
end

local function apply(name, performer, target)
  local e = dd.effect.get(name)
  if not e then
    info("drive", name .. "：没有这个 effect（shield_lite 的 effects 文件没有加载？）")
    return false
  end
  local okk, r = e:apply(performer, target)
  local s = shield and shield.get(target or performer)
  info("drive", ("%s：%s -> %s（count %s），护盾 %s"):format(name, tostring(target or performer), tostring(okk), tostring(okk and r.count or r),
    s and ("%.2f/%.2f"):format(s.cur, s.max) or "nil"))
  return okk == true
end

local function cur_of(a)
  local s = shield and shield.get(a)
  return s and s.cur or 0
end

-- shielded 的结果真的生效：remove_if_not_active 的条目加上时游戏刷新一次属性，规则不成立的当场删掉。
-- on 有护盾、off 没有：LIVE 应留在 on、在 off 上被删；LIVE_NEG（is_false_rule）反过来
local function live_check(on, off)
  local c_on, c_off = cur_of(on), cur_of(off)
  if c_on <= 0 or c_off > 0 then
    info("rule.live", ("跳过：%s 护盾 %g，%s 护盾 %g（要一个有、一个没有）"):format(tostring(on), c_on, tostring(off), c_off))
    return
  end
  local _, g1 = on:add_buff(LIVE, { tag = "live" })
  local _, g2 = off:add_buff(LIVE, { tag = "live" })
  local _, g3 = on:add_buff(LIVE_NEG, { tag = "live" })
  local _, g4 = off:add_buff(LIVE_NEG, { tag = "live" })
  dd.wait(0.2)
  local k1, k2 = g1 and on:buff(g1) ~= nil, g2 and off:buff(g2) ~= nil
  local k3, k4 = g3 and on:buff(g3) ~= nil, g4 and off:buff(g4) ~= nil
  result("rule.live", k1 == true and k2 == false and k3 == false and k4 == true,
    ("有护盾的 %s（%g）：%s 留下 %s、%s 留下 %s；没有护盾的 %s：%s 留下 %s、%s 留下 %s（应为 true false false true）"):format(tostring(on), c_on, LIVE,
      tostring(k1), LIVE_NEG, tostring(k3), tostring(off), LIVE, tostring(k2), LIVE_NEG, tostring(k4)))
  on:remove_buffs({ tag = "live" })
  off:remove_buffs({ tag = "live" })
end

-- 战斗在驱动中结束时抛出 "ended"（调用方不判定这一场）
local function drive(b, h, h2, ms)
  local done, total = 0, 0
  local function step(name, p, t)
    if not still(b) then
      error("ended", 0)
    end
    total = total + 1
    if apply(name, p, t) then done = done + 1 end
    dd.wait(0.1)
  end
  -- 规则探针挂在 h 和 h2 上：h 的护盾在下面的步骤里变化（规则结果随之变化），h2 先没有护盾、后有
  if h:add_buff(PROBE, { tag = "probe" }) then total = total + 1 done = done + 1 end
  if h2 and h2:add_buff(PROBE, { tag = "probe" }) then total = total + 1 done = done + 1 end
  dd.wait(0.2)
  step("shield_self_max_10", h)          -- 上限 +10（没有职业配置时从 0 到 10）
  step("shield_self_full", h)            -- 加满
  step("shield_self_break", h)           -- 破盾
  step("shield_self_gain_5", h)          -- 获得 5
  step("shield_max_pct_50", h, h)        -- 上限 +50%
  step("shield_set_10", h, h)            -- 设成 10（配置表 specs 里的规则）
  -- h2 在这一步再取（队伍里活着的、还没有护盾的英雄），没有时跳过 rule.live
  local h2now = h2
  if not (h2now and h2now:valid() and (h2now.hp or 0) > 0) then
    h2now = party_other(h)
  end
  if h2now then
    live_check(h, h2now)                 -- 这时 h 有 10 点护盾，h2 还没有
  else
    info("rule.live", "跳过：队伍里只有一个活着的英雄")
  end
  step("shield_self_regen_2_3", h)       -- 之后 3 个回合每回合 +2
  local dtarget = h2now or h             -- 只有一个英雄时衰减也加在 h 上，规则照样用到
  if h2now then
    step("shield_set_10", h, h2now)
  end
  step("shield_decay_2_3", h, dtarget)   -- 之后 3 个回合每回合 -2
  for _, m in ipairs(ms) do
    step("shield_set_10", h, m)          -- 每个怪物都有护盾：英雄打哪个都是怪物一侧的吸收
    b.shielded[m] = true
  end
  step("shield_pierce_50", ms[1], h)     -- 这个怪物对 h 的下一次命中 50% 无视护盾（本回合内）
  step("shield_damage_5", h, h)          -- on_miss false 的 effect（e:apply 时是否生效以游戏为准）
  step("shield_heal_pct_25", h, h)
  result("drive." .. b.n, done >= total - 2, ("第 %d 场：施加 %d/%d 步（h=%s h2=%s，怪物 %d 个）"):format(b.n, done, total, tostring(h),
    tostring(h2now), #ms))
end

-- ---------------------------------------------------------------- 死门上的吸收（可选的运行证据）

local dd_probe = { n = 0, pending = {} }
dd.on("damage.final", function(ev)
  local t = ev.target
  if not t or not t.is_hero or not ev.deaths_door or ev.blocked or ev.damage <= 0 or dd_probe.n >= 2 or cur_of(t) > 0 then
    return
  end
  dd_probe.n = dd_probe.n + 1
  local kind = dd_probe.n == 1 and "partial" or "full"
  ev.absorb = kind == "full" and ev.damage or ev.damage / 2
  dd_probe.pending[t] = { kind = kind, damage = ev.damage, absorb = ev.absorb, hp_before = ev.hp_before }
end)
dd.on("damage.post", function(ev)
  local p = ev.target and dd_probe.pending[ev.target]
  if not p then
    return
  end
  dd_probe.pending[ev.target] = nil
  local t = ev.target
  local hp_after = ev.hp_after
  dd.after(1, function()
    info("deathsdoor", ("kind=%s target=%s damage=%g absorb=%g hp_before=%g hp_after=%g 一秒后 valid=%s hp=%s"):format(p.kind, tostring(t), p.damage,
      p.absorb, p.hp_before, hp_after, tostring(t:valid()), tostring(t:valid() and t.hp)))
  end)
end)

-- 驱动之后新出现的怪物（召唤、增援）补上 10 点护盾（审核 R3-B1）
local function top_up(b)
  local p = party()[1]
  if not p then
    return
  end
  for _, m in ipairs(monsters()) do
    if not b.shielded[m] then
      b.shielded[m] = true
      info("drive", ("第 %d 场：新出现的怪物 %s 补上护盾"):format(b.n, tostring(m)))
      apply("shield_set_10", p, m)
    end
  end
end

dd.every(0.5, function()
  local ib = dd.actor.in_battle()
  if ib == nil then
    return  -- 加载中
  end
  if not ib then
    cur = nil  -- 战斗结束（打完、撤退）：驱动的下一步看到 cur 变了就停下
    return
  end
  if not cur then
    cur = { n = battles + 1, since = dd.time(), driven = false, shielded = {} }
  end
  if driving or not shield then
    return
  end
  if cur.driven then
    top_up(cur)
    return
  end
  if battles >= 2 or dd.time() - cur.since < 2 then
    return
  end
  local hs, ms = party(), monsters()
  if #hs == 0 or #ms == 0 then
    return
  end
  -- 优先选不是圣骑士的英雄（圣骑士留给 Smite 的自然流程）
  local h = hs[1]
  for _, x in ipairs(hs) do
    if x.class ~= "crusader" then h = x break end
  end
  local h2 = party_other(h)
  while #ms > 4 do
    table.remove(ms)
  end
  local b = cur
  b.driven = true
  driving = true
  dd.spawn(function()
    local okk, err = pcall(drive, b, h, h2, ms)
    if okk then
      battles = battles + 1
    elseif err == "ended" then
      -- 撤退、打完：这场不算，下一场再驱动（审核 R3-B1 的第 4 点）
      info("drive", ("第 %d 场战斗在驱动中结束了（撤退或打完），这场不算，下一场再驱动"):format(b.n))
    else
      battles = battles + 1
      result("drive." .. b.n, false, "驱动出错：" .. tostring(err))
    end
    driving = false
  end)
end)

if not shield then
  info("drive", "shield_lite 没有加载，不驱动")
end

info("load", "main.lua 执行完毕：每次进存档的前两场战斗，进入战斗 2 秒后驱动一次护盾规则（战斗、队伍、怪物靠 dd.actor 的枚举）" .. (shield and "" or "（shield_lite 没有加载！）"))
return {}
