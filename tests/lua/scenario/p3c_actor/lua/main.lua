-- P3c 场景测试：actor 句柄。只读，只写日志（DDLua.log 里以 [p3c_actor] 开头），不改任何游戏数据。
-- 需要进存档、在同一次探索里打至少两场战斗。每行以「通过」「失败」或「信息」开头，失败行说明原因。
-- 覆盖：事件里的 Hero/Monster 指针转句柄、字段读取、跨回调和跨协程保存、按句柄过滤、actor 作用域的撤销、
-- wait_event 快照里的句柄、战斗结束后怪物句柄失效、英雄句柄跨战斗有效，以及同一次探索的下一场战斗里
-- 同 guid 的怪物（游戏每场战斗重新编 guid）不会被上一场的旧句柄指到（审核 B-B1）。
-- 出现过 actor.death（阵亡）或 hero.dismiss（解雇）的英雄不计入「英雄句柄仍有效」的检查：阵亡的英雄条目留在名册里，
-- 句柄照常有效；解雇的条目被删除，句柄失效（knowledge/dd1/combat.md），两种都不是这里要检查的东西。

local battle = 0
local seen = {}          -- 句柄 -> true（句柄驻留，可以当键）
local monsters = {}      -- 本场见过的怪物句柄（数组）
local prev_monsters = {} -- 上一场的
local heroes = {}        -- 见过的英雄句柄（数组，跨战斗保留）
local gone = {}          -- hero id -> "阵亡" / "解雇"
local scoped = nil       -- 本场第一个怪物的 actor 作用域订阅
local scoped_hits = 0
local hero_scoped = nil  -- 一个英雄的 actor 作用域订阅（不应随战斗结束撤销）
local hero_scoped_id = nil
local filtered_hits, filtered_wrong = 0, 0
local snapshot_done = false -- wait_event 快照本次进存档已经检查过
local snapshot_hero = nil   -- 正在等 damage.post 快照的英雄句柄

local function result(ok, text)
  dd.log.info((ok and "通过 " or "失败 ") .. text)
end

local function describe(h)
  return string.format("%s %s HP %s/%s 压力 %s", tostring(h), tostring(h.class), tostring(h.hp), tostring(h.max_hp), tostring(h.stress))
end

local function count_valid(list)
  local n = 0
  for _, h in ipairs(list) do
    if h:valid() then n = n + 1 end
  end
  return n
end

-- 英雄句柄的检查：不计阵亡、解雇过的英雄
local function hero_check(prefix)
  local total, valid, skipped = 0, 0, 0
  for _, h in ipairs(heroes) do
    if gone[h.id] then
      skipped = skipped + 1
    else
      total = total + 1
      if h:valid() then valid = valid + 1 end
    end
  end
  result(valid == total, string.format("%s %d 个，仍有效 %d 个（不计阵亡、解雇的 %d 个）", prefix, total, valid, skipped))
end

-- wait_event 的快照：指针字段是句柄，下一帧交给协程。等的是正在 damage.pre 里挨打的那个英雄（任何英雄都行），
-- 同一次结算的 damage.post 马上就到，不依赖某一个英雄以后会不会挨打（第二轮审核 R2B-应修2）。
-- 超时（damage.pre 之后没有 damage.post）就放弃这个英雄，下一次有英雄挨打时换它重新等。
local function arm_snapshot(h)
  if snapshot_done or snapshot_hero then return end
  snapshot_hero = h
  -- 快照之后到协程恢复（下一帧）之前，这个英雄的生命可能又变了（流血、腐蚀连续结算、治疗）：
  -- 记下这段时间里同一个英雄的 damage.post / heal.post，比较时按最后一次的 hp_after（审核 C 备注 4）
  -- 从快照的那一次 damage.post（这个英雄的第一次）开始计
  local armed, changes, last_hp = false, 0, nil
  local function track(ev, is_damage)
    if not armed then
      if not is_damage then return end
      armed = true
    end
    changes = changes + 1
    last_hp = ev.hp_after
  end
  local track_damage = dd.on("damage.post", function(ev) track(ev, true) end, { filter = { target = h } })
  local track_heal = dd.on("heal.post", function(ev) track(ev, false) end, { filter = { target = h } })
  dd.spawn(function()
    local snap = dd.wait_event("damage.post", { target = h }, 60)
    track_damage:off()
    track_heal:off()
    snapshot_hero = nil
    if not snap then
      dd.log.info("信息 " .. tostring(h) .. " 的 damage.pre 之后 60 秒内没有 damage.post，换下一个挨打的英雄重新等 wait_event 快照")
      return
    end
    snapshot_done = true
    result(rawequal(snap.target, h), "wait_event 快照的 target 就是保存的英雄句柄（" .. tostring(snap.target) .. "）")
    local now = h.hp
    if changes <= 1 then
      result(snap.hp_after == now, string.format("快照 hp_after=%s，下一帧句柄读到 hp=%s", tostring(snap.hp_after), tostring(now)))
    elseif now == last_hp then
      result(true, string.format("快照 hp_after=%s；下一帧之前这个英雄又结算了 %d 次伤害或治疗，最后一次 hp_after=%s，句柄读到 hp=%s",
        tostring(snap.hp_after), changes - 1, tostring(last_hp), tostring(now)))
    else
      dd.log.info(string.format("信息 快照 hp_after=%s；下一帧之前又结算了 %d 次伤害或治疗，最后一次 hp_after=%s，句柄读到 hp=%s，判不了",
        tostring(snap.hp_after), changes - 1, tostring(last_hp), tostring(now)))
    end
  end)
end

local function remember(h)
  if not h or seen[h] then return end
  seen[h] = true
  dd.log.info("信息 第一次见到 " .. describe(h))
  if h.is_monster then
    monsters[#monsters + 1] = h
    if not scoped then
      scoped = dd.on("damage.pre", function() scoped_hits = scoped_hits + 1 end, { scope = h })
      result(scoped ~= nil, "actor 作用域订阅（" .. tostring(h) .. "）建立")
    end
  else
    heroes[#heroes + 1] = h
    if not hero_scoped then
      hero_scoped = dd.on("stress.pre", function() end, { scope = h })
      hero_scoped_id = h.id
      -- 按句柄过滤：只有 target 是这个英雄的 damage.pre 进入 Lua
      dd.on("damage.pre", function(ev)
        filtered_hits = filtered_hits + 1
        if ev.target ~= h then filtered_wrong = filtered_wrong + 1 end
      end, { filter = { target = h } })
    end
  end
end

-- 同一次探索的下一场战斗，游戏从同一组 guid 重新编号：按上一场的 guid 找本场的怪物，找到的是另一个句柄。
-- DDLua 一侧的 B-B1 检查逐项计数（第二轮审核 R2B-应修1，检查脚本要求每项都等于同 guid 的个数）：
-- 旧句柄失效（valid 为假、读数为 nil，新句柄有效）、== 不相等、不是同一个对象（rawequal）、当表键时分开、
-- 新句柄驻留（再按 guid 取一次是同一个对象）。行首带上新句柄的「（探索 R，战斗 B）」，检查脚本按它和探针对上
local function same_guid_check(prev)
  local same, where = 0, ""
  local n = { invalid = 0, ne = 0, raw = 0, key = 0, intern = 0 }
  for _, old in ipairs(prev) do
    local new = dd.actor.monster(old.id)
    if new then
      same = same + 1
      if where == "" then where = tostring(new):match("（探索 %d+，战斗 %d+）") or "" end
      if not old:valid() and old.hp == nil and new:valid() then n.invalid = n.invalid + 1 end
      if new ~= old and not (old == new) then n.ne = n.ne + 1 end
      if not rawequal(new, old) then n.raw = n.raw + 1 end
      local t = {}
      t[old] = "old"
      t[new] = "new"
      if t[old] == "old" and t[new] == "new" then n.key = n.key + 1 end
      if rawequal(dd.actor.monster(old.id), new) then n.intern = n.intern + 1 end
    end
  end
  return same, where, n
end

dd.on("battle.start", function()
  battle = battle + 1
  prev_monsters, monsters = monsters, {}
  scoped, scoped_hits = nil, 0
  local alive = count_valid(prev_monsters)
  result(alive == 0, string.format("第 %d 场战斗开始：上一场的怪物句柄 %d 个，仍有效 %d 个（应为 0）", battle, #prev_monsters, alive))
  local same, where, n = same_guid_check(prev_monsters)
  if same > 0 then
    local all = n.invalid == same and n.ne == same and n.raw == same and n.key == same and n.intern == same
    result(all, string.format("第 %d 场%s：上一场的怪物里和本场怪物 guid 相同的 %d 个；旧句柄失效 %d/%d，== 不相等 %d/%d，" ..
      "不是同一个对象 %d/%d，表键分开 %d/%d，新句柄驻留 %d/%d", battle, where, same, n.invalid, same, n.ne, same, n.raw, same,
      n.key, same, n.intern, same))
  elseif #prev_monsters > 0 then
    dd.log.info(string.format("信息 第 %d 场：上一场的 %d 个怪物 guid 在本场都没有出现（不在同一次探索里，或者本场怪物更少）", battle, #prev_monsters))
  end
  hero_check(string.format("第 %d 场战斗开始：英雄句柄", battle))
end)

dd.on("damage.pre", function(ev)
  local t, a = ev.target, ev.attacker
  remember(t)
  remember(a)
  if t and t.is_hero then
    arm_snapshot(t)
  end
  if t and t.hp and ev.hp_before and math.abs(t.hp - ev.hp_before) > 0.001 then
    result(false, string.format("damage.pre 的 hp_before=%s，句柄读到 %s", ev.hp_before, t.hp))
  end
end)

dd.on("skill.pre", function(ev)
  remember(ev.performer)
  remember(ev.target)
end)

dd.on("heal.post", function(ev)
  local t = ev.target
  if t then
    result(t.max_hp == nil or ev.hp_after <= t.max_hp + 0.01,
      string.format("heal.post %s 治疗后 %s，最大生命 %s", tostring(t), ev.hp_after, tostring(t.max_hp)))
  end
end)

dd.on("actor.death", function(ev)
  if not ev.is_monster then
    gone[ev.hero_id] = "阵亡"
    dd.log.info(string.format("信息 英雄 hero_id=%d 阵亡，之后不计入英雄句柄的检查（句柄 %s）", ev.hero_id,
      tostring(dd.actor.hero(ev.hero_id))))
  end
end)

dd.on("hero.dismiss", function(ev)
  gone[ev.hero_id] = "解雇"
  dd.log.info(string.format("信息 英雄 hero_id=%d 被解雇，之后不计入英雄句柄的检查", ev.hero_id))
end)

dd.on("battle.end", function()
  -- 游戏先清空怪物表再派发 BattleDestroyedEvent，这时怪物句柄都应失效
  local alive = count_valid(monsters)
  result(alive == 0, string.format("第 %d 场战斗结束：本场怪物句柄 %d 个，仍有效 %d 个（应为 0）", battle, #monsters, alive))
  for _, m in ipairs(monsters) do
    if m.hp ~= nil then result(false, tostring(m) .. " 失效后 hp 不是 nil") end
  end
  hero_check(string.format("第 %d 场战斗结束：英雄句柄", battle))
  if scoped then
    -- actor 作用域：SDK 一侧是 battle 作用域（DDCore 在同一个原生事件上撤销，先后不定），另有每秒的检查，
    -- 所以过两秒再看
    local sub, hits, which = scoped, scoped_hits, battle
    dd.after(2, function()
      result(not sub:active(), string.format("第 %d 场：怪物作用域的订阅在战斗结束后已撤销（期间调用 %d 次）", which, hits))
    end)
  end
  if hero_scoped then
    if gone[hero_scoped_id] then
      -- 解雇的英雄句柄失效，订阅会被撤销；阵亡的不一定。换一个还在的英雄重新订阅，下一场战斗结束时检查
      dd.log.info(string.format("信息 英雄作用域订阅的 hero_id=%d %s了，换一个英雄重新订阅", hero_scoped_id, gone[hero_scoped_id]))
      hero_scoped:off()
      hero_scoped, hero_scoped_id = nil, nil
      for _, h in ipairs(heroes) do
        if not gone[h.id] and h:valid() then
          hero_scoped, hero_scoped_id = dd.on("stress.pre", function() end, { scope = h }), h.id
          break
        end
      end
    else
      result(hero_scoped:active(), "英雄作用域的订阅在战斗结束后仍然有效")
    end
  end
  result(filtered_wrong == 0, string.format("按句柄过滤的 damage.pre 进入 Lua %d 次，target 不符 %d 次", filtered_hits, filtered_wrong))
  dd.log.info("信息 订阅数 " .. dd.subscriptions())
end)

dd.log.info("信息 p3c_actor 已加载；dd.actor.hero(0) = " .. tostring(dd.actor.hero(0)) .. "（hero id 0 不一定存在）")
