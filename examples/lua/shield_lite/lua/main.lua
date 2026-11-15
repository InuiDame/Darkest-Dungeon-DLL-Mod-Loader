-- shield_lite：用 Lua 复刻 DDShield 的护盾规则（护盾值、上限、获得、吸收伤害、每回合增减、shielded 规则）。
-- 规则和 DDShield 一致，代码是重新写的；和 DDShield 的差异见 README.md。
--
-- 数据层：effects/shield_lite.effects.darkest 定义 shield_gain_5 这类空的 effect，技能里引用它们（.effect "shield_self_gain_5"），
-- 游戏施加时 effect.pre 按名字进到这里（dd.effect.register("shield_*")），按名字里的数字结算护盾。
-- 吸收：damage.final 是护体、格挡之后、扣血之前的最终伤害，absorb 写多少游戏就少扣多少；全部吸收时游戏按格挡处理（显示 Blocked）。
-- 显示：有护盾的角色身上挂一条载体 Buff（脚本 Buff shield_lite），护盾值存在它的状态里，随存档保存。
local cfg = require("config")
local json = require("jsonline")

local CARRIER = "shield_lite"   -- 载体 Buff 的 id
local EPS = 0.001               -- 全部吸收的判定余量（DDShield 同）

-- ---------------------------------------------------------------- 日志

local function key(a)
  return (a.is_hero and "hero:" or "monster:") .. a.id
end

local function log(ev, t)
  if not cfg.log then
    return
  end
  t.ev = ev
  dd.log.info("SHIELD " .. json.encode(t))
end

-- ---------------------------------------------------------------- 名字解析（DDShield 的 ParseNameSpec）

local function atoi(s)
  local n = s and s:match("^[+-]?%d+")
  return n and tonumber(n) or 0
end

-- "shield_[self_]<规则>"：gain_N、heal_N、gain_pct_N、heal_pct_N、max_N、max_pct_N、damage_N、damage_pct_N、
-- regen_N[_回合]、decay_N[_回合]、pierce、pierce_N、full、break。认不出时返回 nil
local function parse_name(name)
  local p = name:match("^shield_(.*)$")
  if not p then
    return nil
  end
  if p:sub(1, 5) == "self_" then
    p = p:sub(6)
  end
  local prefixed = {
    { "gain_pct_", "addpct" }, { "heal_pct_", "addpct" }, { "max_pct_", "maxpct" }, { "damage_pct_", "damagepct" },
    { "gain_", "add" }, { "heal_", "add" }, { "max_", "max" }, { "damage_", "damage" },
  }
  for _, item in ipairs(prefixed) do
    local pre, op = item[1], item[2]
    if p:sub(1, #pre) == pre then
      return { op = op, value = atoi(p:sub(#pre + 1)) }
    end
  end
  for _, op in ipairs({ "regen", "decay" }) do
    if p:sub(1, #op + 1) == op .. "_" then
      local rest = p:sub(#op + 2)
      local turns = rest:match("^[^_]*_(.*)$")
      return { op = op, value = atoi(rest), turns = turns and atoi(turns) or 1 }
    end
  end
  if p == "pierce" then
    return { op = "pierce", value = 100 }
  end
  if p:sub(1, 7) == "pierce_" then
    return { op = "pierce", value = atoi(p:sub(8)) }
  end
  if p == "full" or p == "break" then
    return { op = p, value = 0 }
  end
  return nil
end

-- 配置表优先（DDShield 先查 DDShield.effects.json，再按名字解析）。配置表里的 op 另外认 heal（等于 add）和 clear（等于 break），同 DDShield
local function spec_of(name)
  local s = cfg.specs and cfg.specs[name]
  if s then
    return { op = s.op, value = s.value or 0, turns = s.turns or s.duration or 1 }
  end
  return parse_name(name)
end

-- ---------------------------------------------------------------- 护盾状态

local shields = {}    -- actor 句柄 -> 状态
local probed = {}     -- 本次进存档查过载体的角色（读档后从载体恢复，只查一次）
local carriers = {}   -- actor 句柄 -> 载体的组 guid
local pending = {}    -- actor 句柄 -> 这次伤害的吸收记录，damage.post 时核对生命
local dirty = false   -- 有护盾变化，下一帧对齐载体
local battle_no = 0   -- 本次进存档的第几场战斗（日志里区分怪物）
local hit_no = 0
local restore_blocked = false  -- 战斗结束、回城清理之后到下一场战斗开始之前不从载体恢复（游戏这时还没删掉载体，见 restore）
-- 有没有 Raid（城镇里没有）。DDShield 的规则判定挂在游戏的前置检查之后，城镇里前置检查直接返回假，shielded 不起作用；
-- 这里在城镇里让规则回调返回 nil，交还游戏原来的判定，结果相同。读进存档时按 dd.store_status() 的原因判断（地牢里的存档写着「地牢」）
local in_raid = (select(2, dd.store_status()) or ""):find("地牢") ~= nil

local function clamp(x, lo, hi)
  if x < lo then return lo end
  if x > hi then return hi end
  return x
end

local function snap(s)
  return { cur = s.cur, max = s.max, base = s.base, emax = s.emax, regen = s.regen, regen_turns = s.regen_turns,
    decay = s.decay, decay_turns = s.decay_turns, pierce = s.pierce, pierce_by = s.pierce_by and key(s.pierce_by) or "" }
end

-- 载体状态里保存的字段（穿透只对这一次命中有效，不保存）
local SAVED = { "cur", "max", "base", "emax", "regen", "regen_turns", "decay", "decay_turns" }

-- 读档后：角色身上已经有载体（游戏随存档恢复，状态由 DDLua 从旁路恢复）时从载体的状态接着用。
-- battle.end 时游戏可能还没删掉 remove_on_battle_complete 的载体（先后没有证据，审核 C-N8），清理之后到下一场开始都不恢复，
-- 否则这段时间里的规则判定会把刚清掉的护盾从载体恢复回来
local function restore(a)
  if probed[a] or restore_blocked then
    return nil
  end
  if not cfg.carrier then
    probed[a] = true
    return nil
  end
  local list, why = a:buffs({ id = CARRIER })
  if list == nil and type(why) == "string" and why:find("^加载中") then
    return nil  -- 加载线程在跑，读不到载体（不是没有）：下次用到时再查
  end
  probed[a] = true
  for _, b in ipairs(list or {}) do
    local st = dd.buff.state(a, b.guid)
    carriers[a] = carriers[a] or b.guid
    if st and type(st.cur) == "number" then
      local s = { pierce = 0 }
      for _, k in ipairs(SAVED) do
        s[k] = st[k] or 0
      end
      shields[a] = s
      log("restore", { actor = key(a), battle = battle_no, guid = b.guid, after = snap(s) })
      return s
    end
  end
  return nil
end

-- 已有的状态（必要时从载体恢复），没有时返回 nil，不新建
local function peek(a)
  return shields[a] or restore(a)
end

-- DDShield 的 EnsureInit：第一次用到时按配置和职业定上限、初始值
local function ensure(a)
  local s = peek(a)
  if s then
    return s
  end
  local need_hp = (cfg.max_hp_ratio or 0) > 0 or (cfg.start_hp_ratio or 0) > 0
  local max_hp = need_hp and (a.max_hp or 0) or 0
  local cap = cfg.default_max or 0
  if (cfg.max_hp_ratio or 0) > 0 and max_hp > 0 then
    cap = cfg.max_hp_ratio * max_hp
  end
  local start = cfg.start_flat or 0
  if (cfg.start_hp_ratio or 0) > 0 and max_hp > 0 then
    start = start + cfg.start_hp_ratio * max_hp
  end
  local class = a.is_hero and a.class or nil
  local def = class and cfg.classes and cfg.classes[class:lower()]
  if def then
    if def.max then cap = def.max end
    if def.start then start = def.start end
  end
  s = { cur = 0, max = cap, base = cap, emax = 0, regen = 0, regen_turns = 0, decay = 0, decay_turns = 0, pierce = 0 }
  s.cur = clamp(start, 0, s.max > 0 and s.max or start)
  if s.max <= 0 then
    s.max = s.cur
  end
  shields[a] = s
  log("init", { actor = key(a), battle = battle_no, class = class or "", max_hp = max_hp, class_def = def ~= nil, after = snap(s) })
  if s.cur > 0 then
    dirty = true
  end
  return s
end

-- DDShield 的 RecalcMax（没有 buff 带来的上限，见 README）
local function recalc_max(s)
  local m = s.base + s.emax
  if m < 0 then m = 0 end
  s.max = m
  if s.cur > s.max then s.cur = s.max end
  if s.cur < 0 then s.cur = 0 end
end

-- DDShield 的 ApplySpec
local function apply_spec(s, spec, performer)
  local op, v = spec.op, spec.value
  if op == "add" or op == "heal" then
    s.cur = s.cur + v
  elseif op == "addpct" then
    s.cur = s.cur + s.max * v / 100
  elseif op == "max" then
    s.emax = math.max(s.emax + v, 0)
    recalc_max(s)
  elseif op == "maxpct" then
    s.emax = math.max(s.emax + (s.base + s.emax) * v / 100, 0)
    recalc_max(s)
  elseif op == "set" then
    s.cur = v
  elseif op == "full" then
    s.cur = s.max
  elseif op == "break" or op == "clear" then
    s.cur = 0
  elseif op == "damage" then
    s.cur = s.cur - v
  elseif op == "damagepct" then
    s.cur = s.cur - s.max * v / 100
  elseif op == "pierce" then
    -- effect 先于伤害结算：记下来，紧接着这个执行者对它的这次命中用掉
    s.pierce = clamp(v, 0, 100)
    s.pierce_by = performer
  elseif op == "regen" then
    s.regen = s.regen + v
    s.regen_turns = spec.turns > 0 and spec.turns or 1
  elseif op == "decay" then
    s.decay = s.decay + v
    s.decay_turns = spec.turns > 0 and spec.turns or 1
  end
  if s.cur > s.max and s.max > 0 then s.cur = s.max end
  if s.cur < 0 then s.cur = 0 end
  if s.max <= 0 and s.cur > 0 then s.max = s.cur end  -- 没有上限时第一次获得的量就是上限
end

-- 生命 ≤ 0（英雄在死门上、怪物死了）时护盾清零：DDShield 每次画血条时这样做（ShieldOnActorBar 里 hp <= 0 时 cur = 0），
-- 所以死门上的英雄没有护盾，在死门上获得的护盾下一帧也清掉
local function zero_if_dead(a, s, hp, why)
  if not s or s.cur <= 0 or not hp or hp > 0 then
    return false
  end
  local before = snap(s)
  s.cur = 0
  dirty = true
  log("zero", { actor = key(a), battle = battle_no, hp = hp, why = why, before = before, after = snap(s) })
  return true
end

-- ---------------------------------------------------------------- 获得：effect.pre 按名字路由

-- 每个执行者最近一次的技能：actor 句柄 -> { id, effects = { [effect 名字] = true } }，日志里标明 effect 来自哪个技能。
-- 游戏在技能演出时才施加技能自带的 effect，比 skill.post 晚（P4 场景日志里约 2 秒），所以 skill.post 时不清；
-- 这个执行者开始下一个技能、战斗结束、退出存档（VM 随之销毁）时清掉
local last_skill = {}

dd.on("skill.pre", function(ev)
  local p = ev.performer
  if not p then
    return
  end
  local sk = ev.skill
  local names = {}
  for _, n in ipairs(sk and sk.effects or {}) do
    names[n] = true
  end
  last_skill[p] = { id = sk and sk.id or "?", effects = names }
  ensure(p)
end)

-- 执行者相同、effect 名字在它最近一次技能的 .effect 里时归给那个技能，否则是空串。脚本在这之后给同一个执行者施加同名 effect
-- 也会归给这个技能，这里分不出来（检查脚本按 p4c_shield 的 [drive] 行排除驱动的）
local function skill_of(performer, name)
  local k = performer and last_skill[performer]
  return k and k.effects[name] and k.id or ""
end

dd.effect.register("shield_*", function(ev)
  local name = ev.effect_name
  local spec = spec_of(name)
  local target = ev.target
  if not spec or not target then
    return
  end
  local s = ensure(target)
  local before = snap(s)
  apply_spec(s, spec, ev.performer)
  dirty = true
  log("apply", { actor = key(target), battle = battle_no, effect = name, op = spec.op, value = spec.value, turns = spec.turns,
    performer = ev.performer and key(ev.performer) or "", skill = skill_of(ev.performer, name), before = before, after = snap(s) })
end)

-- ---------------------------------------------------------------- 吸收：damage.final（DDShield 的 ShieldAbsorbFinal）

dd.on("damage.final", function(ev)
  local t = ev.target
  if not t then
    return
  end
  local s = ensure(t)
  if ev.attacker then
    ensure(ev.attacker)
  end
  in_raid = true
  zero_if_dead(t, s, ev.hp_before, "damage.final")  -- 死门上：DDShield 在这之前画血条时已经清零
  local damage = ev.damage
  -- 游戏自己已经格挡（护盾不消耗）、没有伤害、没有护盾时不吸收
  if ev.blocked or damage <= 0 or s.cur <= 0 then
    return
  end
  local pierce = 0
  if s.pierce_by and ev.attacker and s.pierce_by == ev.attacker then
    pierce = s.pierce
  end
  local before = snap(s)
  local absorb = math.min(s.cur, damage * (1 - pierce / 100))
  s.cur = s.cur - absorb
  local full = absorb >= damage - EPS
  ev.absorb = full and damage or absorb  -- 全部吸收：DDCore 按格挡处理；部分吸收：DDCore 把吸收的部分补回生命
  dirty = true
  hit_no = hit_no + 1
  pending[t] = { n = hit_no, damage = damage, absorb = absorb, full = full, hp_before = ev.hp_before, deaths_door = ev.deaths_door }
  log("absorb", { actor = key(t), battle = battle_no, n = hit_no, damage = damage, damage_type = ev.damage_type, pierce = pierce,
    attacker = ev.attacker and key(ev.attacker) or "", absorb = absorb, written = ev.absorb, kind = full and "full" or "partial",
    hp_before = ev.hp_before, deaths_door = ev.deaths_door, before = before, after = snap(s) })
end)

-- 这次伤害结算完：核对生命损失，清掉这个攻击者留下的穿透（DDShield 的 ShieldEndHit）
dd.on("damage.post", function(ev)
  local t = ev.target
  if not t then
    return
  end
  local p = pending[t]
  if p then
    pending[t] = nil
    local net = p.damage - p.absorb
    log("hit", { actor = key(t), battle = battle_no, n = p.n, damage = p.damage, absorb = p.absorb, kind = p.full and "full" or "partial",
      deaths_door = p.deaths_door, hp_before = p.hp_before, hp_after = ev.hp_after, loss = p.hp_before - ev.hp_after,
      expect = p.full and 0 or net })
  end
  local s = shields[t]
  if s and s.pierce_by and ev.attacker and s.pierce_by == ev.attacker then
    local before = snap(s)
    s.pierce, s.pierce_by = 0, nil
    log("endhit", { actor = key(t), battle = battle_no, attacker = key(ev.attacker), before = before, after = snap(s) })
  end
end)

-- ---------------------------------------------------------------- 每回合（DDShield 的 ShieldOnRound，挂在回合的 buff 结算之后）

-- 游戏只在回合号大于 1 时做回合的 buff 结算（5A64B0 调 UpdateRoundBuffs），DDShield 挂在它后面，第 1 回合不结算；
-- battle.round.start 第 1 回合也派发，这里跳过，和 DDShield 一致
dd.on("battle.round.start", function(ev)
  local n = 0
  for _ in pairs(shields) do n = n + 1 end
  in_raid = true
  if (ev.round or 0) <= 1 then
    log("round_begin", { battle = battle_no, round = ev.round, actors = n, skip = true })
    return
  end
  log("round_begin", { battle = battle_no, round = ev.round, actors = n })
  for a, s in pairs(shields) do
    local before = snap(s)
    s.pierce, s.pierce_by = 0, nil  -- 没被命中用掉的穿透不留到下一回合
    s.cur = s.cur + (cfg.regen_per_round or 0) - (cfg.decay_per_round or 0)
    if s.regen_turns > 0 then
      s.cur = s.cur + s.regen
      s.regen_turns = s.regen_turns - 1
      if s.regen_turns == 0 then s.regen = 0 end
    end
    if s.decay_turns > 0 then
      s.cur = s.cur - s.decay
      s.decay_turns = s.decay_turns - 1
      if s.decay_turns == 0 then s.decay = 0 end
    end
    s.cur = clamp(s.cur, 0, s.max > 0 and s.max or s.cur)
    local after = snap(s)
    if json.encode(after) ~= json.encode(before) then
      dirty = true
      log("round", { actor = key(a), battle = battle_no, round = ev.round, before = before, after = after })
    end
  end
end)

-- 战斗里见到的角色先初始化（DDShield 在血条第一次绘制时初始化，职业有初始护盾时这时就有）
dd.on("battle.turn.start", function(ev)
  if ev.actor then
    ensure(ev.actor)
  end
end)

-- ---------------------------------------------------------------- 清理：战斗结束、回城

local function clear_all(reason)
  for a, s in pairs(shields) do
    log("clear", { actor = key(a), battle = battle_no, reason = reason, before = snap(s) })
  end
  shields, pending, probed = {}, {}, {}
  restore_blocked = true  -- 游戏随后删掉载体；在那之前不能从载体恢复（C-N8）
  dirty = true
end

dd.on("battle.start", function()
  battle_no = battle_no + 1
  restore_blocked = false
  in_raid = true
  log("battle", { battle = battle_no })
end)
dd.on("battle.end", function()
  clear_all("battle.end")
  last_skill = {}
end)
dd.on("app.state", function(ev)
  local to = ev.to_name or ""
  if to:find("town") then
    in_raid = false
    if next(shields) then
      clear_all("town")
    end
  elseif to:find("raid") or to:find("combat") then
    in_raid = true
  end
end)

-- ---------------------------------------------------------------- shielded 规则（DDShield 的 ShieldStatus）

local rule_logged, rule_last = 0, {}
dd.on("battle.start", function() rule_logged, rule_last = 0, {} end)

-- 有护盾时返回 true；没有护盾、城镇里、没有要检查的角色时返回 nil，交还游戏原来的判定。DDShield 只接管游戏前置检查通过之后的
-- 「状态是不是 shielded」那一步：前置检查不过（城镇里、actorStatus 没有 target）时结果是假、不取反；通过了而没有护盾时是假、
-- 再按 is_false_rule 取反。游戏原来的判定对认不出的 shielded 正好也是这样，所以没有护盾时交还它，结果和 DDShield 相同
local rule, why = dd.buff.rule("shielded", function(self, target, buff)
  local subject = buff.subject
  local s = in_raid and subject and peek(subject) or nil
  local result = s ~= nil and s.cur > 0
  if subject and rule_last[subject] ~= result and rule_logged < (cfg.rule_log_max or 0) then
    rule_last[subject] = result
    rule_logged = rule_logged + 1
    log("rule", { actor = key(subject), battle = battle_no, buff = buff.id, rule = buff.rule, is_false_rule = buff.is_false_rule,
      cur = s and s.cur or 0, in_raid = in_raid, result = result and true or "original" })
  end
  if result then
    return true
  end
  return nil
end)
if not rule then
  dd.log.warn("shielded 规则注册失败：" .. tostring(why))
end

-- ---------------------------------------------------------------- 载体 Buff

local carrier_ok = false
if cfg.carrier then
  local ok, err = dd.buff.define({
    id = CARRIER, stat = "combat_stat_add", sub_stat = "protection_rating", amount = 0,
    duration = -1, duration_type = "till_removed", source = "skill", remove_on_battle_complete = true,
    on_remove = function(b, state, reason)
      if carriers[b.actor] == b.guid then
        carriers[b.actor] = nil
      end
      log("carrier", { actor = key(b.actor), battle = battle_no, op = "removed", guid = b.guid, reason = reason, cur = state.cur or 0 })
    end,
  })
  carrier_ok = ok == true
  if not ok then
    dd.log.warn("载体 Buff 定义失败，不显示护盾：" .. tostring(err))
  end
end

local function mirror(s, st)
  st = st or {}
  for _, k in ipairs(SAVED) do
    st[k] = s[k]
  end
  return st
end

-- 每帧：生命 ≤ 0 的角色护盾清零（DDShield 每帧画血条时做），再（有变化时）对齐载体：有护盾的角色有一条，没有的删掉。
-- 定时器是安全点，增删立即执行
dd.every(0, function()
  for a, s in pairs(shields) do
    if s.cur > 0 and a:valid() then
      zero_if_dead(a, s, a.hp, "frame")
    end
  end
  if not dirty or not carrier_ok then
    return
  end
  dirty = false
  for a, s in pairs(shields) do
    local guid = carriers[a]
    if s.cur > 0 and not guid then
      local ok, r = a:add_buff(CARRIER, { tag = "shield", state = mirror(s) })
      if ok and r then
        carriers[a] = r
        log("carrier", { actor = key(a), battle = battle_no, op = "add", guid = r, cur = s.cur })
      elseif not ok then
        log("carrier", { actor = key(a), battle = battle_no, op = "add_failed", reason = tostring(r) })
      end
    elseif s.cur <= 0 and guid then
      carriers[a] = nil
      a:remove_buffs({ guid = guid })
    elseif guid then
      local st = dd.buff.state(a, guid)
      if st then mirror(s, st) end
    end
  end
  for a, guid in pairs(carriers) do
    if not shields[a] then
      carriers[a] = nil
      if a:valid() then
        a:remove_buffs({ guid = guid })
      end
    end
  end
end)

-- 存档时把护盾值写进载体的状态（DDLua 在所有 campaign.save 回调之后取快照），变化了的写一行日志
local saved_text = {}
dd.on("campaign.save", function(ev)
  for a, guid in pairs(carriers) do
    local s = shields[a]
    local st = s and dd.buff.state(a, guid)
    if st then
      mirror(s, st)
      local text = json.encode(snap(s))
      if saved_text[a] ~= text then
        saved_text[a] = text
        log("save", { actor = key(a), battle = battle_no, index = ev.index, guid = guid, in_battle = ev.in_battle, state = snap(s) })
      end
    end
  end
end)

dd.log.info(("已加载（版本 %s）：护盾 effect 前缀 shield_，规则 shielded%s，载体 Buff %s"):format(dd.version, rule and "" or "（注册失败）",
  carrier_ok and CARRIER or "（关闭）"))

-- 给别的 Mod 读护盾值（只读的副本）
return {
  get = function(a)
    local s = a and peek(a)
    return s and { cur = s.cur, max = s.max } or nil
  end,
}
