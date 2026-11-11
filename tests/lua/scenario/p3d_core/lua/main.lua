-- P3d 场景测试：.pre 写入和取消、延迟事件、作用域、负载失效、campaign.load，以及检查 p3d_faulty、p3d_broken
-- 有没有影响到自己。日志在 DDLua.log，以 [p3d_core] 开头，每行是「通过 [标签]」「失败 [标签]」或「信息 [标签]」，
-- tools/p3_scenario_check.py 按标签判定。
--
-- 会改战斗结果（每次进入存档各一次，只对英雄有利或影响很小）：
--   * 英雄受到的第一次伤害（≥2 点、原值打不死）减半，之后的下一次伤害取消（按 0 结算）。英雄有受到伤害的百分比修正、
--     减半的效果判不了时，下一场再减半一次，每次进入存档最多 3 次
--   * 第一次命中英雄的攻击改成未命中（hit.pre 的 hit = false）
--   * 第一次给没满血的英雄的治疗改成 0（heal.pre 的 amount = 0）
--   * 第一次让英雄加压力的变化改成 0（stress.pre 的 amount = 0）
-- 其余全部只读。

local MOD_PRIORITY = 1000 -- 改写放在最后，前面的订阅（探针、别的 Mod）看到的是原值

local done = {} -- 标签 -> true：每个改写每次进入存档只做一次
local pending = {} -- 等对应 .post 核对的改写
local battles = 0
local counts = { damage_pre = 0, damage_post = 0 } -- 本场
local faulty_base = nil -- 本场开始时 p3d_faulty 的 damage.post 累计出错次数（isolation 比较本场的增量）
local battle_sub = nil -- 本场 scope = "battle" 的订阅
local raid_sub = nil -- 本次探索 scope = "raid" 的订阅
local saved_payload = nil
-- damage.halve 和受到伤害的百分比修正（第二轮审核 R2B-备注4）：游戏按 roundf(伤害 × (1 + [Actor+0xBE0])) 扣血
-- （483733-483747），这个倍率 Lua 读不到。本场每个英雄其余伤害的 (伤害, 掉血) 各给出一个倍率区间，用来判断减半那次的掉血
-- 符合「写入值 × 倍率」还是「原值 × 倍率」
-- 英雄句柄 -> { {lo, hi}, ... }。只放同一段的样本：battle.start、battle.end 判完减半之后都清空，所以战斗里的减半只用
-- 本场的样本，走廊里的减半只用上一场结束之后的走廊样本（第三轮审核 R3B-备注2：受伤倍率的增减益常在战斗结束时到期）
local ksamples = {}
local halve_check = nil -- 等到战斗结束再判的减半
local halve_tries = 0 -- 本次进存档减半过几次（判不了时下一场再试，最多 3 次）

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

local function near(a, b)
  return a ~= nil and b ~= nil and math.abs(a - b) < 0.01
end

local function fmt(v)
  if type(v) == "number" then
    return string.format("%.2f", v)
  end
  return tostring(v)
end

-- 出错时的文字（不调用 __tostring）
local function try(f)
  local ok, err = pcall(f)
  return ok, type(err) == "string" and err or type(err)
end

-- ---------------------------------------------------------------- 减半的判定（受到伤害的百分比修正）

-- roundf(damage × k) == loss 的 k 区间
local function k_range(damage, loss)
  return { lo = (loss - 0.5) / damage, hi = (loss + 0.5) / damage }
end

local function overlaps(a, b)
  return a.lo <= b.hi + 1e-6 and b.lo <= a.hi + 1e-6
end

-- 战斗结束时判减半（下一场战斗开始时也判一次，免得在走廊里减半的那次没人判）。返回 true 表示判出了通过或失败。
--   写入值 × 倍率 符合掉血、原值 × 倍率 不符合：通过；反过来：失败（写入没有生效）；
--   本场这个英雄没有别的可以估倍率的伤害时，掉血正好是写入值就按没有修正（倍率 1）判通过，否则判不了；
--   两种都符合、都不符合（倍率中途变了）、生命降到 0（掉血被截断）：判不了，只写「信息」，下一场再减半一次
local function judge_halve(c)
  local samples = ksamples[c.target] or {}
  local k1, k0 = k_range(c.wrote, c.loss), k_range(c.orig, c.loss)
  local fit1, fit0 = false, false
  local lo, hi = math.huge, -math.huge
  for _, s in ipairs(samples) do
    fit1 = fit1 or overlaps(s, k1)
    fit0 = fit0 or overlaps(s, k0)
    lo, hi = math.min(lo, s.lo), math.max(hi, s.hi)
  end
  local base = string.format("%s：damage.pre 原值 %s 改成 %s；damage.post damage=%s，生命 %s -> %s，掉血 %s", c.who, fmt(c.orig), fmt(c.wrote),
    fmt(c.post_damage), fmt(c.hp_before), fmt(c.hp_after), fmt(c.loss))
  local again = halve_tries < 3 and "，下一场再试" or ""
  local est = string.format("按本场这个英雄另外 %d 次伤害估出受到伤害的倍率 %.2f～%.2f", #samples, lo, hi)
  if c.hp_after <= 0.01 then
    info("damage.halve", base .. "；生命降到 0，掉血被截断，判不了" .. again)
    return false
  elseif #samples == 0 then
    if near(c.loss, c.wrote) then
      result("damage.halve", true, base .. "，正好是写入值（本场没有别的伤害可以估倍率，按没有受到伤害的百分比修正判断）")
      return true
    end
    info("damage.halve", base .. "，不等于写入值（有受到伤害的百分比修正），这个英雄本场没有别的伤害可以估倍率，判不了" .. again)
    return false
  elseif fit1 and not fit0 then
    result("damage.halve", true, base .. "；" .. est .. "，掉血符合写入值 × 倍率，不符合原值 × 倍率")
    return true
  elseif fit0 and not fit1 then
    result("damage.halve", false, base .. "；" .. est .. "，掉血符合原值 × 倍率，不符合写入值 × 倍率：写入没有生效")
    return true
  end
  info("damage.halve", base .. "；" .. est .. "，" .. (fit1 and "写入值和原值都符合" or "写入值和原值都不符合（倍率中途变了？）") .. "，判不了" .. again)
  return false
end

local function settle_halve()
  if halve_check then
    local c = halve_check
    halve_check = nil
    if not judge_halve(c) and halve_tries < 3 then
      done.halve = false -- 下一次合适的伤害再减半一次
    end
  end
end

-- ---------------------------------------------------------------- 存档和状态

dd.on("campaign.load", function(ev)
  result("campaign.load", true, "main.lua 里订阅的 campaign.load 收到了：serial=" .. ev.serial .. "，campaign=" .. tostring(ev.campaign) ..
    "（指针字段不是 actor，应为 nil）")
end)

dd.on("campaign.unload", function()
  -- DDLua 在 CampaignUnload 时先拆除 Lua Mod，DDCore 之后才派发这个主题，所以按存档加载的 Mod 收不到
  info("campaign.unload", "意外收到了 campaign.unload（按设计收不到）")
end)

dd.on("app.state", function(ev)
  info("state", ev.from_name .. " -> " .. ev.to_name)
end)

-- ---------------------------------------------------------------- 延迟事件

dd.on("raid.start", function(ev)
  local raid = ev.raid
  local write_ok, write_err = try(function() ev.raid = nil end)
  local cancel_ok, cancel_err = try(function() ev:cancel() end)
  if ev.deferred then
    result("deferred.raid", raid == nil and not write_ok and write_err:find("延迟事件", 1, true) ~= nil and not cancel_ok,
      "raid.start 是延迟事件：raid=" .. tostring(raid) .. "（应为 nil），写字段报错「" .. write_err .. "」，取消报错「" .. cancel_err .. "」")
  else
    info("deferred.raid", "raid.start 这次不是延迟事件（在主线程派发）：raid=" .. tostring(raid))
  end
  -- 本次探索有效的订阅：探索结算时撤销
  if raid_sub then
    info("scope.raid", "上一次探索的 scope=raid 订阅仍" .. (raid_sub:active() and "有效（探索没有结算就开始了新探索？）" or "已撤销"))
  end
  raid_sub = dd.on("raid.torch", function() end, { scope = "raid" })
end)

dd.on("raid.finish", function()
  local sub = raid_sub
  if sub then
    dd.after(2, function()
      result("scope.raid", not sub:active(), "探索结算后 scope=raid 的订阅" .. (sub:active() and "仍然有效" or "已撤销"))
    end)
  end
end)

dd.spawn(function()
  local snap = dd.wait_event("raid.start")
  if not snap then
    return
  end
  if snap.deferred then
    result("deferred.snapshot", snap.raid == nil, "wait_event(raid.start) 的快照：deferred=true，raid=" .. tostring(snap.raid) .. "（应为 nil）")
  else
    info("deferred.snapshot", "wait_event(raid.start) 的快照不是延迟事件，raid=" .. tostring(snap.raid))
  end
end)

local inventory_logged = false
dd.on("inventory.add", function(ev)
  if ev.deferred and not inventory_logged then
    inventory_logged = true
    result("deferred.inventory", ev.inventory == nil and ev.item == nil,
      "inventory.add 延迟事件：inventory=" .. tostring(ev.inventory) .. "，item=" .. tostring(ev.item) .. "（应为 nil），value=" .. ev.value)
  end
end)

-- ---------------------------------------------------------------- 战斗

dd.on("battle.start", function()
  battles = battles + 1
  counts = { damage_pre = 0, damage_post = 0 }
  settle_halve() -- 在走廊里（战斗之外）减半的那次，用走廊里的样本判
  ksamples = {}
  -- p3d_faulty 的计数从加载起累计（走廊里的陷阱伤害也算），出错 3 次后停用、不再增加，所以记下本场的基线
  local faulty = dd.import("p3d_faulty")
  faulty_base = faulty and faulty.post_calls() or nil
  local before = dd.subscriptions()
  battle_sub = dd.on("damage.post", function() end, { scope = "battle" })
  info("battle", string.format("第 %d 场战斗开始；建立 scope=battle 的订阅，订阅数 %d -> %d", battles, before, dd.subscriptions()))
end)

dd.on("battle.end", function()
  local which = battles
  info("battle", string.format("第 %d 场战斗结束：本场 damage.pre %d 次，damage.post %d 次", which, counts.damage_pre, counts.damage_post))
  settle_halve()
  ksamples = {} -- 本场的样本不带进走廊（R3B-备注2）
  local sub = battle_sub
  battle_sub = nil
  if sub then
    -- DDCore 在同一个原生事件（BattleDestroyedEvent）上撤销 battle 作用域，先后不定，过 2 秒再看
    dd.after(2, function()
      result("scope.battle", not sub:active(), string.format("第 %d 场：scope=battle 的订阅在战斗结束后%s", which,
        sub:active() and "仍然有效" or "已撤销"))
    end)
  end
  -- p3d_faulty 的错误和死循环不能影响本 Mod：本 Mod 的回调照常执行。两边订阅的是同一批 damage.post，
  -- p3d_faulty 的订阅有效时两边次数相同，停用后它不再增加，所以本场的增量不能多于本 Mod 本场收到的次数
  local faulty = dd.import("p3d_faulty")
  if faulty then
    local calls = faulty.post_calls()
    local delta = calls - (faulty_base or calls)
    if calls > 0 then
      local expect_off = calls >= 3 -- 默认配置 max_failures = 3
      result("isolation", counts.damage_post >= delta and (faulty.post_active() ~= expect_off),
        string.format("p3d_faulty 的 damage.post 回调累计出错 %d 次，本场 %d 次（%s），本 Mod 本场的 damage.post 照常收到 %d 次", calls,
          delta, faulty.post_active() and "订阅仍有效" or "已被停用", counts.damage_post))
    end
    result("budget", not faulty.loop_active(), "p3d_faulty 在 battle.start 里死循环的订阅" ..
      (faulty.loop_active() and "仍然有效（没有被预算停用）" or "已被预算停用") .. "，本 Mod 的 battle.end 照常执行")
  else
    info("isolation", "p3d_faulty 没有加载（没勾选？）")
  end
  result("broken.import", dd.import("p3d_broken") == nil, "加载失败的 p3d_broken 不能被 dd.import")
  -- 改写之后应当紧接着收到同一个目标的 .post（DDCore 在同一次调用里派发）
  for key, tag in pairs({ damage = "damage", hit = "hit.miss", heal = "heal.zero", stress = "stress.zero" }) do
    if pending[key] then
      if key == "damage" then
        tag = "damage." .. pending.damage.kind
      end
      result(tag, false, "改写了 " .. key .. " 的 .pre，但到战斗结束都没收到同一个目标的 .post")
      pending[key] = nil
    end
  end
end)

-- damage.pre：先计数，最后（MOD_PRIORITY）改写
dd.on("damage.pre", function(ev)
  counts.damage_pre = counts.damage_pre + 1
  local hit = pending.hit
  if hit and ev.target == hit.target then
    hit.damage_events = hit.damage_events + 1
  end
end, { priority = -MOD_PRIORITY })

dd.on("damage.pre", function(ev)
  local target = ev.target
  -- 有改写还在等 .post 时（嵌套的结算）不再改写；没等到的在战斗结束时报失败
  if pending.damage or not (ev.is_hero and target and not ev.cancelled) then
    return
  end
  -- 减半只挑原值打不死（不会被生命下限截断）的伤害，否则改不改掉血都一样，判不出来。上一次减半还在等判定时不再减半
  if not done.halve and not halve_check and ev.damage >= 2 and ev.hp_before > ev.damage then
    done.halve = true
    halve_tries = halve_tries + 1
    local wrote = math.floor(ev.damage / 2)
    pending.damage = { kind = "halve", target = target, orig = ev.damage, wrote = wrote, hp_before = ev.hp_before }
    ev.damage = wrote
    local read_back = ev.damage
    if not near(read_back, wrote) then
      result("damage.halve", false, "写入后读回 " .. fmt(read_back) .. "，应为 " .. wrote)
    end
  elseif done.halve and not done.cancel and ev.damage > 0 then
    done.cancel = true
    pending.damage = { kind = "cancel", target = target, orig = ev.damage, hp_before = ev.hp_before, seen = false }
    ev:cancel()
  end
end, { priority = MOD_PRIORITY })

-- 排在改写之后的订阅能看到取消
dd.on("damage.pre", function(ev)
  local p = pending.damage
  if p and p.kind == "cancel" and ev.target == p.target then
    p.seen = ev.cancelled
  end
end, { priority = MOD_PRIORITY + 1 })

dd.on("damage.post", function(ev)
  counts.damage_post = counts.damage_post + 1
  if not saved_payload then
    saved_payload = ev
    dd.after(0, function()
      local ok, err = try(function() return saved_payload.damage end)
      result("payload.expired", not ok and err:find("失效", 1, true) ~= nil, "回调返回后访问保存的负载：" .. (ok and "没有报错" or "报错「" .. err .. "」"))
    end)
  end
  local p = pending.damage
  if not p or ev.target ~= p.target then
    -- 英雄受到的其余伤害：(伤害, 掉血) 给出受到伤害的倍率区间，判减半时用（生命降到 0、掉血不到 0.5 的不算：
    -- 截断、格挡（480400）都会让掉血和倍率无关）
    local loss = ev.hp_before - ev.hp_after
    if ev.is_hero and ev.target and not ev.cancelled and ev.damage >= 1 and ev.hp_after > 0.01 and loss >= 0.5 then
      local list = ksamples[ev.target] or {}
      ksamples[ev.target] = list
      list[#list + 1] = k_range(ev.damage, loss)
    end
    return
  end
  pending.damage = nil
  local who = tostring(ev.target)
  if p.kind == "halve" then
    -- 游戏按 roundf(伤害 × (1 + 受到伤害的百分比修正 Actor+0xBE0)) 扣血（4836A0），修正不一定是 0：
    -- 写入值有没有传到游戏在这里判，掉血符合写入值还是原值到战斗结束时按本场别的伤害估出的倍率判（judge_halve）
    if not near(ev.damage, p.wrote) or ev.cancelled then
      result("damage.halve", false, string.format("%s：damage.pre 原值 %s 改成 %s；damage.post damage=%s cancelled=%s（应为写入值、没有取消）", who,
        fmt(p.orig), fmt(p.wrote), fmt(ev.damage), tostring(ev.cancelled)))
    else
      halve_check = { who = who, target = p.target, orig = p.orig, wrote = p.wrote, post_damage = ev.damage, hp_before = p.hp_before,
        hp_after = ev.hp_after, loss = p.hp_before - ev.hp_after }
    end
  else
    result("damage.cancel", ev.cancelled and near(ev.damage, 0) and near(ev.hp_after, p.hp_before),
      string.format("%s：damage.pre 原值 %s 被取消；damage.post cancelled=%s damage=%s，生命 %s -> %s（应不变）", who, fmt(p.orig),
        tostring(ev.cancelled), fmt(ev.damage), fmt(p.hp_before), fmt(ev.hp_after)))
    result("damage.cancel.seen", p.seen == true, "优先级排在后面的订阅看到 ev.cancelled=" .. tostring(p.seen))
  end
end, { priority = MOD_PRIORITY })

-- hit.pre：第一次命中英雄的攻击改成未命中；hit.post 应为 hit=false，期间这个英雄没有 damage.pre
dd.on("hit.pre", function(ev)
  local target = ev.target
  if pending.hit or done.hit or not ev.hit or not (target and target.is_hero) then
    return
  end
  done.hit = true
  pending.hit = { target = target, damage = ev.damage, damage_events = 0 }
  ev.hit = false
end, { priority = MOD_PRIORITY })

dd.on("hit.post", function(ev)
  local p = pending.hit
  if not p or ev.target ~= p.target then
    return
  end
  pending.hit = nil
  result("hit.miss", ev.hit == false and p.damage_events == 0,
    string.format("%s：hit.pre 把 hit 改成 false（原伤害 %s）；hit.post hit=%s，期间这个英雄的 damage.pre %d 次（应为 0，走了未命中路径）",
      tostring(ev.target), fmt(p.damage), tostring(ev.hit), p.damage_events))
end, { priority = MOD_PRIORITY })

-- heal.pre：第一次给没满血的英雄的治疗改成 0；heal.post 应为 amount=0、生命不变
dd.on("heal.pre", function(ev)
  if not done.heal_cancel_error then
    done.heal_cancel_error = true
    local ok, err = try(function() ev:cancel() end)
    result("cancel.error", not ok and err:find("不能取消", 1, true) ~= nil, "heal.pre 不能取消：" .. (ok and "没有报错" or "报错「" .. err .. "」"))
  end
  local target = ev.target
  if pending.heal or done.heal or ev.amount <= 0 or not (target and target.is_hero) then
    return
  end
  local max_hp = target.max_hp
  if max_hp and ev.hp_before >= max_hp - 0.01 then
    info("heal.zero", tostring(target) .. " 满血（" .. fmt(ev.hp_before) .. "/" .. fmt(max_hp) .. "），这次治疗不改")
    return
  end
  done.heal = true
  pending.heal = { target = target, orig = ev.amount, hp_before = ev.hp_before, max_hp = max_hp }
  ev.amount = 0
end, { priority = MOD_PRIORITY })

dd.on("heal.post", function(ev)
  local p = pending.heal
  if not p or ev.target ~= p.target then
    return
  end
  pending.heal = nil
  result("heal.zero", near(ev.amount, 0) and near(ev.hp_after, p.hp_before) and near(ev.applied, 0),
    string.format("%s：heal.pre 原值 %s 改成 0；heal.post amount=%s applied=%s，生命 %s -> %s（应不变；最大生命 %s）", tostring(ev.target),
      fmt(p.orig), fmt(ev.amount), fmt(ev.applied), fmt(p.hp_before), fmt(ev.hp_after), fmt(p.max_hp)))
end, { priority = MOD_PRIORITY })

-- stress.pre：第一次让英雄加压力的变化改成 0；stress.post 应为 amount=0、压力不变
dd.on("stress.pre", function(ev)
  local target = ev.target
  if pending.stress or done.stress or ev.amount <= 0 or not (target and target.is_hero) then
    return
  end
  done.stress = true
  pending.stress = { target = target, orig = ev.amount, before = ev.stress_before }
  ev.amount = 0
end, { priority = MOD_PRIORITY })

dd.on("stress.post", function(ev)
  local p = pending.stress
  if not p or ev.target ~= p.target then
    return
  end
  pending.stress = nil
  result("stress.zero", near(ev.amount, 0) and near(ev.stress_after, p.before),
    string.format("%s：stress.pre 原值 +%s 改成 0；stress.post amount=%s applied=%s，压力 %s -> %s（应不变）", tostring(ev.target),
      fmt(p.orig), fmt(ev.amount), fmt(ev.applied), fmt(p.before), fmt(ev.stress_after)))
end, { priority = MOD_PRIORITY })

-- skill.pre：可写字段都是指针，Lua 只能读；写指针、写只读字段、取消都要报错
dd.on("skill.pre", function(ev)
  if done.skill then
    return
  end
  done.skill = true
  local performer, target = ev.performer, ev.target
  local ptr_ok, ptr_err = try(function() ev.target = target end)
  local ro_ok, ro_err = try(function() ev.target_count = 1 end)
  local cancel_ok, cancel_err = try(function() ev:cancel() end)
  result("skill.pre", not ptr_ok and ptr_err:find("指针", 1, true) ~= nil and not ro_ok and ro_err:find("只读", 1, true) ~= nil and
    not cancel_ok and ev.writable,
    string.format("performer=%s target=%s target_count=%d friendly=%s writable=%s；写指针字段报错「%s」；写只读字段报错「%s」；取消报错「%s」",
      tostring(performer), tostring(target), ev.target_count, tostring(ev.friendly), tostring(ev.writable), ptr_err, ro_err, cancel_err))
end)

info("load", string.format("p3d_core 已加载：订阅 %d 个（dd.stats: 等待者 %d、定时器 %d、协程 %d），dd.dev=%s",
  dd.subscriptions(), dd.stats().waiters, dd.stats().timers, dd.stats().tasks, tostring(dd.dev)))

return {}
