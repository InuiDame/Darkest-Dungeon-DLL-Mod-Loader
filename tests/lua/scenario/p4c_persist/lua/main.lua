-- P4c2 场景测试：dd.store 和脚本 Buff 的状态随存档保存（城镇、走廊、战斗中各存一次档，退回主菜单再读，核对）。
-- 日志在 DDLua.log，以 [p4c_persist] 开头：「通过 [标签]」「失败 [标签]」「信息 [标签]」。步骤见 docs/10-p4-scenario.md。
--
-- 会改游戏状态（不改玩法：载体 Buff 是速度 +0）：
--   * 城镇：每次进存档在名册里第一个英雄身上加两条临时的 p4c_mark（标签 stack_a、stack_b，同 id 两条，各自的状态），判完马上删掉
--     （「同一个 id 是两条」每次都判，和要跨存档保存的标记分开，审核 R3-S1）；名册里没有本 Mod 的城镇载体时（第一次进存档、
--     回城清理之后）再给这个英雄加两条要保存的 p4c_mark（town_a、town_b）
--   * 走廊：走一步时队伍里没有走廊载体就给队伍里的一个英雄加一条 p4c_mark（标签 corridor）；每走一步，这几条的状态 steps 加一
--   * 战斗：每场战斗给队伍里的一个英雄加 p4c_mark（battle，挨打时状态 hits 加一）和 p4c_bend（remove_on_battle_complete，战斗结束
--     游戏删掉），给每个怪物加 p4c_mark（mon，这场里后来出现的怪物也加，审核 R3-B2）；每回合开始状态 rounds 加一。
--     这场战斗里没有怪物死亡就结束的算撤退
-- 场合、队伍、怪物靠 DDCore 的枚举（dd.actor.in_battle、party、monsters，F5），不靠「在事件里见过」：真实游戏开战后 5～9 秒
-- 才第一次出手，走廊里几乎没有带角色的事件，读进战斗中的存档时也不派发 battle.start（第三轮审核 R3-B1、B3）。
-- 载体都是来源 skill（进存档、回城时游戏清掉队伍英雄身上的）。要不要加按游戏里实际有没有判断，不记在 dd.store 里，
-- 同一个存档重跑场景时照样会加（审核 C-Y5：回城之后读档、改旁路时名册里要有带状态的标记）。
-- 每次游戏存档（campaign.save）把这些载体当时的状态抄进 dd.store.expect，DDLua 在所有回调之后取快照，所以存下去的
-- 状态和 expect 一致；读档后逐条核对：条目还在、id 和 guid 对得上、状态相同、标签认得，读档恢复的条目不派发 buff.add。
-- 每次 on_remove 都和 buff.remove 主题的 reason 对一遍。
-- 加载线程在跑时（读档进地牢、回城的加载画面）读接口返回 nil 和「加载中」开头的原因（R2A-S1）：这时不判断条目在不在、
-- 不加载体，等加载结束；回城途中（结算后的过渡、as_town_visit_start）不算城镇。
local MARK, BEND = "p4c_mark", "p4c_bend"

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

local function copy(v)
  if type(v) ~= "table" then
    return v
  end
  local t = {}
  for k, x in pairs(v) do
    t[k] = copy(x)
  end
  return t
end

local function same(a, b)
  if type(a) ~= "table" or type(b) ~= "table" then
    return a == b
  end
  for k, v in pairs(a) do
    if not same(v, b[k]) then
      return false
    end
  end
  for k in pairs(b) do
    if a[k] == nil then
      return false
    end
  end
  return true
end

local function show(v)
  if type(v) ~= "table" then
    return tostring(v)
  end
  local keys = {}
  for k in pairs(v) do keys[#keys + 1] = tostring(k) end
  table.sort(keys)
  local out = {}
  for _, k in ipairs(keys) do out[#out + 1] = k .. "=" .. show(v[k]) end
  return "{" .. table.concat(out, ",") .. "}"
end

local function akey(a)
  return (a.is_hero and "hero#" or "monster#") .. a.id
end

-- 读接口的原因以「加载中」开头：加载线程在跑，暂时读不到（不是条目不在）
local function loading(why)
  return type(why) == "string" and why:find("^加载中") ~= nil
end

local function loading_now()
  local d, why = dd.buff.def(MARK)
  return d == nil and loading(why)
end

-- ---------------------------------------------------------------- 读档结果（main.lua 执行时 dd.store 已经从旁路读好）

local status, reason = dd.store_status()
local S = dd.store
local loaded_empty = next(S) == nil
local prev = { loads = S.loads, saves = S.saves, last = copy(S.last), expect = copy(S.expect) }
S.loads = (S.loads or 0) + 1
info("store", ("status=%s loads=%d saves=%s last=%s expect=%d reason=%s"):format(status, S.loads, tostring(prev.saves),
  prev.last and ("#" .. tostring(prev.last.index) .. "/" .. tostring(prev.last.where)) or "nil", prev.expect and #prev.expect or 0,
  tostring(reason)))

-- ---------------------------------------------------------------- 场合
-- 战斗、走廊靠枚举：dd.actor.in_battle() 为真是战斗（读进来的战斗也是），否则队伍不空就是走廊。城镇和回城途中靠 app.state：
-- 到了 as_town_visit_update 才算城镇；结算后的过渡、load_town 线程在跑的 as_town_visit_start 是「途中」，什么都不做

local app = (reason or ""):find("地牢") and "raid" or "start"  -- start：刚进存档，还没有 app.state
dd.on("app.state", function(ev)
  local to = ev.to_name or ""
  if to:find("town") then
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

local where = "transit"  -- 下面马上 refresh_where 一次（这时就在战斗里的话也按「新的一场」处理）
local where_since = dd.time()
local battle_no = 0
local battle_deaths = 0      -- 这场战斗里死掉的怪物（actor.death）；0 个就结束的算撤退
local battle_added = false   -- 这场战斗给英雄加过载体了没有
local battle_loaded = false  -- 这场是读进来的战斗：载体随存档恢复，不再加
local marked = {}            -- 这场战斗加过 mon 的怪物
-- 读进战斗中的存档：之后第一次看到的战斗就是存档里那场（游戏不派发 battle.start）
local loaded_battle_pending = prev.last ~= nil and prev.last.where == "battle"

local function set_where(w)
  if w ~= where then
    where, where_since = w, dd.time()
  end
end

local function refresh_where()
  local w = where_now()
  if w == nil then
    return
  end
  if w == "battle" and where ~= "battle" then
    battle_no = battle_no + 1
    battle_deaths, battle_added, marked = 0, false, {}
    battle_loaded = loaded_battle_pending
    loaded_battle_pending = false
    if battle_loaded then
      battle_added = true
      info("battle", ("第 %d 场战斗是读档恢复的：载体随存档恢复，不再加"):format(battle_no))
    end
  end
  set_where(w)
end

refresh_where()
-- battle.start、battle.end 时马上看一次（战斗状态这时已经变了），其余靠每 0.5 秒的轮询
dd.on("battle.start", function() refresh_where() end)
dd.on("actor.death", function(ev)
  if ev.is_monster then
    battle_deaths = battle_deaths + 1
  end
end)

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

-- ---------------------------------------------------------------- 跟踪的载体

local tracked = {}  -- { actor, guid, id, tag, where }

local function find_tracked(actor, guid)
  for i, t in ipairs(tracked) do
    if t.actor == actor and t.guid == guid then
      return i, t
    end
  end
end

-- true：条目在；false：不在；nil：加载中，读不到
local function present(t)
  if not t.actor:valid() then
    return false
  end
  local b, why = t.actor:buff(t.guid)
  if b then
    return true
  end
  if loading(why) then
    return nil
  end
  return false
end

-- 同上：有一条在就是 true；有读不到的、又没有在的，是 nil
local function has_tracked(w)
  local unknown = false
  for _, t in ipairs(tracked) do
    if t.where == w then
      local p = present(t)
      if p then
        return true
      elseif p == nil then
        unknown = true
      end
    end
  end
  if unknown then
    return nil
  end
  return false
end

-- buff.remove 主题的 reason（本 Mod 的条目），和 on_remove 收到的对一遍
local function bkey(kind_hero, id, guid)
  return (kind_hero and "hero#" or "monster#") .. tostring(id) .. ":" .. tostring(guid)
end
local ev_reason, match_logged = {}, {}
dd.on("buff.remove", function(ev)
  if ev.plugin == "DDLua" and ev.tag:find("^p4c_persist:") then
    ev_reason[bkey(ev.actor_kind == 1, ev.actor_id, ev.guid)] = ev.reason
  end
end)
-- buff.add 主题里本 Mod 的条目（读档恢复的条目不应出现在这里）
local added = {}
dd.on("buff.add", function(ev)
  if ev.plugin == "DDLua" and ev.tag:find("^p4c_persist:") then
    added[bkey(ev.actor_kind == 1, ev.actor_id, ev.guid)] = true
  end
end)

local function on_removed(b, st, why)
  local i, t = find_tracked(b.actor, b.guid)
  if i then
    table.remove(tracked, i)
  end
  info("removed", ("%s guid=%d id=%s tag=%s reason=%s state=%s"):format(akey(b.actor), b.guid, b.id, tostring(t and t.tag or b.tag), why, show(st)))
  if why ~= "gone" then
    -- gone 是 DDLua 自己判断的（actor 不再能解析），没有 buff.remove；其余的应和主题的 reason 相同
    local k = bkey(b.actor.is_hero, b.actor.id, b.guid)
    dd.after(0, function()
      local r = ev_reason[k]
      if r ~= why or not match_logged[why] then
        match_logged[why] = true
        result("remove.match", r == why, ("%s 的 %s（guid %d）：on_remove 收到 %s，buff.remove 主题的 reason 是 %s"):format(akey(b.actor), b.id, b.guid, why,
          tostring(r)))
      end
    end)
  end
  if why == "town_visit" and t == nil and (st == nil or next(st) == nil) then
    -- 没有跟踪、状态是空表：上一次运行在不一致的读档（tamper-persist）时丢弃了状态的旧载体，同一个存档重跑时会留在英雄身上。
    -- 游戏照样在回城时删掉它，原因由 remove.match 核对，这里不判定（2026-11-15 补充场景：guid 221、222）
    info("town_visit.stale", ("回城时游戏删掉 %s 身上的旧 %s（guid %d，状态在之前不一致的读档时丢弃了），on_remove 收到 town_visit"):format(
      akey(b.actor), b.id, b.guid))
  elseif why == "town_visit" then
    result("town_visit", t ~= nil, ("回城时游戏删掉 %s 身上的 %s（guid %d，标签 %s），on_remove 收到 town_visit"):format(akey(b.actor), b.id, b.guid,
      tostring(t and t.tag)))
  elseif b.id == BEND and why == "gone" and b.actor.is_hero then
    -- 英雄已经不在名册里（阵亡后被移出、被解雇）：DDLua 按 gone 丢弃状态，不是游戏删掉的载体，不判定（审核 R3-S3）
    info("bend.gone", ("%s 不在名册里了，p4c_bend（guid %d）的状态按 gone 丢弃，不算战斗结束的删除"):format(akey(b.actor), b.guid))
  elseif b.id == BEND and why ~= "plugin" then
    local ending = battle_deaths > 0 and "victory" or "retreat"
    result("bend.remove", why ~= "gone", ("战斗结束游戏删掉 remove_on_battle_complete 的载体（%s guid %d，%s：这场死了 %d 个怪物），on_remove 收到 %s，状态 %s"):format(
      akey(b.actor), b.guid, ending == "retreat" and "撤退" or "打完", battle_deaths, why, show(st)))
    if ending == "retreat" then
      result("bend.retreat", why ~= "gone", ("撤退时游戏删掉 %s 的 p4c_bend（guid %d），on_remove 收到 %s"):format(akey(b.actor), b.guid, why))
    end
  elseif b.actor.is_monster and why == "gone" then
    local actor, guid = b.actor, b.guid
    dd.after(0, function()
      result("monster.gone", dd.buff.state(actor, guid) == nil, ("怪物 %s 随战斗销毁，载体（guid %d）的 on_remove 收到 gone，之后状态已丢弃"):format(akey(actor), guid))
    end)
  end
end

local function define(id, extra)
  local def = { id = id, stat = "combat_stat_add", sub_stat = "speed_rating", amount = 0, duration = -1, duration_type = "till_removed",
    source = "skill",
    on_apply = function(b, st) st.applied = (st.applied or 0) + 1 end,
    on_remove = on_removed,
    on_damage = function(b, st) if st.hits then st.hits = st.hits + 1 end end }
  for k, v in pairs(extra or {}) do def[k] = v end
  local ok, why = dd.buff.define(def)
  result("define." .. id, ok == true, "定义脚本 Buff " .. id .. "：" .. tostring(why))
end
define(MARK)
define(BEND, { remove_on_battle_complete = true })

-- 战斗结束 2 秒后还在的 p4c_bend：游戏没有按 remove_on_battle_complete 删掉（撤退时是不是这样没有证据，记下来）
dd.on("battle.end", function()
  refresh_where()
  local ending = battle_deaths > 0 and "打完" or "撤退"
  dd.after(2, function()
    for _, t in ipairs(tracked) do
      if t.id == BEND and present(t) == true then
        info("bend.after_end", ("战斗结束（%s）2 秒后 %s 的 p4c_bend（guid %d）还在"):format(ending, akey(t.actor), t.guid))
      end
    end
  end)
end)

-- 在定时器、协程里立即执行（返回 guid）；在事件回调里（raid.step）排到下一帧，done 收到 guid 时再记下
local function add(actor, id, tag, state, w)
  local function track(guid)
    tracked[#tracked + 1] = { actor = actor, guid = guid, id = id, tag = tag, where = w }
  end
  local ok, guid = actor:add_buff(id, { tag = tag, state = state, done = function(okq, r)
    info("add.done", ("%s %s 标签 %s 排队执行 -> %s %s"):format(akey(actor), id, tag, tostring(okq), tostring(r)))
    if okq then
      track(r)
    end
  end })
  if ok and guid then
    track(guid)
  end
  info("add", ("%s %s 标签 %s -> %s %s"):format(akey(actor), id, tag, tostring(ok), guid and tostring(guid) or "（排队）"))
  return ok and guid
end

-- ---------------------------------------------------------------- 存档：把载体当时的状态抄进 dd.store.expect

local function where_of(ev)
  return ev.in_battle and "battle" or (ev.in_raid and "corridor" or "town")
end

-- 招募、解雇是不是各自让游戏存档（写名册）：记下时刻，检查脚本看之后几秒内有没有 [save]
dd.on("hero.recruit", function() info("roster", "hero.recruit") end)
dd.on("hero.dismiss", function() info("roster", "hero.dismiss") end)

dd.on("campaign.save", function(ev)
  S.saves = (S.saves or 0) + 1
  local expect = {}
  for _, t in ipairs(tracked) do
    local st = t.actor:valid() and dd.buff.state(t.actor, t.guid)
    if st then
      expect[#expect + 1] = { kind = t.actor.kind, id = t.actor.id, guid = t.guid, buff = t.id, tag = t.tag, where = t.where, state = copy(st) }
    end
  end
  S.expect = expect
  S.last = { index = ev.index, where = where_of(ev), files = ev.files, loads = S.loads }
  info("save", ("#%d where=%s saves=%d expect=%d files=%s"):format(ev.index, S.last.where, S.saves, #expect, ev.files))
end)

-- ---------------------------------------------------------------- 读档后核对

local restored_keys = {}  -- 核对通过的（读档恢复的）条目

local function check_entries(kind)
  local ok_n, bad, tag_bad = 0, {}, {}
  for _, e in ipairs(prev.expect or {}) do
    if e.kind == kind then
      local a = kind == "hero" and dd.actor.hero(e.id) or dd.actor.monster(e.id)
      local b = a and a:buff(e.guid)
      local st = b and dd.buff.state(a, e.guid)
      if not b then
        bad[#bad + 1] = ("%s#%d guid %d：条目不在"):format(kind, e.id, e.guid)
      elseif b.id ~= e.buff then
        bad[#bad + 1] = ("%s#%d guid %d：id 是 %s，应为 %s"):format(kind, e.id, e.guid, b.id, e.buff)
      elseif not same(st, e.state) then
        bad[#bad + 1] = ("%s#%d guid %d：状态 %s，应为 %s"):format(kind, e.id, e.guid, show(st), show(e.state))
      else
        ok_n = ok_n + 1
        restored_keys[#restored_keys + 1] = bkey(kind == "hero", e.id, e.guid)
        tracked[#tracked + 1] = { actor = a, guid = e.guid, id = e.buff, tag = e.tag, where = e.where }
        if not (b.mine and b.tag == e.tag) then
          tag_bad[#tag_bad + 1] = ("%s#%d guid %d：标签 %s mine=%s，应为 %s"):format(kind, e.id, e.guid, tostring(b.tag), tostring(b.mine), e.tag)
        end
      end
    end
  end
  return ok_n, bad, tag_bad
end

local hero_result = nil
local function finish_check(monster_part)
  local w = prev.last and prev.last.where or "?"
  local h_ok, h_bad, h_tag = table.unpack(hero_result)
  local m_ok, m_bad, m_tag = 0, {}, {}
  if monster_part then
    m_ok, m_bad, m_tag = check_entries("monster")
  end
  local bad, tag_bad = {}, {}
  for _, list in ipairs({ h_bad, m_bad }) do for _, x in ipairs(list) do bad[#bad + 1] = x end end
  for _, list in ipairs({ h_tag, m_tag }) do for _, x in ipairs(list) do tag_bad[#tag_bad + 1] = x end end
  local total = h_ok + m_ok + #bad
  result("persist." .. w, #bad == 0, ("读 %s 的存档（#%s，%s）：dd.store_status=%s，saves=%s，载体 %d 条一致（英雄 %d，怪物 %d），不一致 %d 条%s"):format(
    w, tostring(prev.last and prev.last.index), tostring(prev.last and prev.last.files), status, tostring(prev.saves), h_ok + m_ok, h_ok, m_ok, #bad,
    #bad > 0 and ("：" .. table.concat(bad, "；")) or ""))
  if total > 0 then
    result("persist.tag." .. w, #tag_bad == 0, ("读档后标签和 mine 认得 %d/%d 条%s"):format(h_ok + m_ok - #tag_bad, h_ok + m_ok,
      #tag_bad > 0 and ("：" .. table.concat(tag_bad, "；")) or ""))
  end
  -- 读档恢复的条目不派发 buff.add（D3；加载线程上的延迟事件也算），等一会儿再看，延迟事件要排到主线程
  if #restored_keys > 0 then
    dd.after(5, function()
      local seen = {}
      for _, k in ipairs(restored_keys) do
        if added[k] then seen[#seen + 1] = k end
      end
      result("restore.noadd", #seen == 0, ("读档恢复的 %d 条载体%s"):format(#restored_keys,
        #seen == 0 and "都没有派发 buff.add" or ("里 " .. #seen .. " 条派发了 buff.add：" .. table.concat(seen, "、"))))
    end)
  end
end

-- 地牢里的存档：raid 核对之后（怪物的状态那时才恢复）再判定。等核对看 dd.store_status() 的第三个返回值，
-- 不看原因里有没有「raid」：读到上一代时原因里本来就写着 raid 不一致的那一代（审核 R2B 备注 6）
local function wait_raid_then(fn)
  local started = dd.time()
  local timer
  timer = dd.every(0.5, function()
    local st, why, pending = dd.store_status()
    if st == "mismatch" then
      -- 旁路和探索存档对不上：DDLua 已经丢弃了等着的怪物状态。是不是预期的（场景里改过旁路）由检查脚本判断
      timer:cancel()
      result("persist.mismatch", true, "raid 核对不一致被发现：" .. tostring(why))
    elseif not pending and not loading_now() then
      timer:cancel()
      info("raid", "raid 核对完成：" .. st)
      fn()
    elseif dd.time() - started > 120 then
      timer:cancel()
      result("persist." .. tostring(prev.last and prev.last.where), false, "raid 核对没有在 120 秒内完成：" .. tostring(why))
    end
  end)
end

-- 读档后在名册里找带 p4c_mark 的英雄（不一致时 dd.store 是空的，不知道 guid）。加载线程在跑时读不到 buff 表
-- （地牢里的存档读档后要等 as_raid_start 加载完），等它结束再读这个英雄（审核 R2A-S1：原来读不到就当成没有标记）
local function scan_marks(fn)
  dd.spawn(function()
    local found, empty, waited = 0, 0, 0
    for id = 0, 8000 do
      local h = dd.actor.hero(id)
      if h then
        local list, why = h:buffs({ id = MARK })
        while list == nil and loading(why) and waited < 120 do
          dd.wait(0.5)
          waited = waited + 0.5
          list, why = h:buffs({ id = MARK })
        end
        if list == nil and loading(why) then
          result("persist.mismatch", false, "加载线程 120 秒还没结束，名册里的标记读不到：" .. tostring(why))
          return
        end
        for _, b in ipairs(list or {}) do
          found = found + 1
          local st = dd.buff.state(h, b.guid)
          if st and next(st) == nil then
            empty = empty + 1
          end
        end
      end
      if id % 200 == 199 then
        coroutine.yield()
      end
    end
    fn(found, empty)
  end)
end

if status == "mismatch" then
  info("persist.mismatch", "dd.store_status() = mismatch：" .. tostring(reason))
  -- 名册里要真有带状态的标记（城镇里、回城后都会加，见上），0 条时「状态不恢复」就是空判（审核 C-Y5）
  scan_marks(function(found, empty)
    result("persist.mismatch", loaded_empty and found > 0 and found == empty, ("旁路和存档不一致被发现：%s；dd.store 是空表 %s，名册里的 p4c_mark 载体 %d 条，状态是空表的 %d 条"):format(
      tostring(reason), tostring(loaded_empty), found, empty))
  end)
end

local function verify_after_load()
  if not (status == "loaded" or status == "prev") or not prev.last then
    return
  end
  hero_result = { check_entries("hero") }
  if (reason or ""):find("raid") then
    wait_raid_then(function() finish_check(prev.last.where == "battle") end)
  else
    finish_check(false)
  end
end

-- 英雄的状态在 main.lua 之后、campaign.load 之前恢复
dd.on("campaign.load", function()
  verify_after_load()
end)

-- ---------------------------------------------------------------- 城镇：同 id 两条，各自的状态

local town_hero = S.town_hero and dd.actor.hero(S.town_hero) or nil
local scanning = false
local town_tried = false  -- 本次进存档已经在城镇里做过（或试过）

-- 同一个 id 加两次是两条、各自的状态：每次进存档都判（审核 R3-S1：原来只在加要保存的城镇标记时判，名册里已经有标记时
-- 就判不到）。两条临时的载体在同一帧里加上、核对、删掉（协程里不让出），游戏存档时它们已经不在了
local function stack_test(h)
  local ga = add(h, MARK, "stack_a", { where = "stack", label = "a", n = 1 }, "stack")
  local gb = add(h, MARK, "stack_b", { where = "stack", label = "b", n = 2 }, "stack")
  local sa = ga and dd.buff.state(h, ga)
  local sb = gb and dd.buff.state(h, gb)
  local n = #(h:buffs({ id = MARK, mine = true }) or {})
  result("stack", ga and gb and ga ~= gb and sa and sb and sa ~= sb and sa.label == "a" and sb.label == "b" and sa.applied == 1 and sb.applied == 1 and n >= 2,
    ("同一个 id 加两次是两条：%s 身上 p4c_mark %d 条，guid %s、%s，状态 %s、%s（临时的两条，判完删掉）"):format(akey(h), n, tostring(ga), tostring(gb),
      show(sa), show(sb)))
  local _, na = h:remove_buffs({ tag = "stack_a" })
  local _, nb = h:remove_buffs({ tag = "stack_b" })
  info("stack", ("临时的两条删掉了：%s、%s 条"):format(tostring(na), tostring(nb)))
end

-- 每次进存档在城镇里先做 stack_test；名册里已经有本 Mod 的城镇载体（读档核对过的）时不再加，第一次进存档、回城清理掉之后、
-- 旁路不一致时加两条要保存的。加载中读不到时不判断，下一次再看
local function town_phase()
  if scanning or town_tried or loading_now() then
    return
  end
  local have = has_tracked("town")
  if have == nil then
    return
  end
  town_tried = true
  scanning = true
  dd.spawn(function()
    local holder = nil
    for _, t in ipairs(tracked) do
      if t.where == "town" and present(t) then
        holder = t.actor
        break
      end
    end
    if not holder then
      for id = 0, 8000 do
        local h = dd.actor.hero(id)
        if h and (h.hp or 0) > 0 then
          holder = h
          break
        end
        if id % 200 == 199 then
          coroutine.yield()
        end
      end
    end
    scanning = false
    if not holder then
      info("town", "名册里 0..8000 没有找到英雄")
      return
    end
    town_hero = holder
    local waited = 0
    while loading_now() and waited < 60 do
      dd.wait(0.5)  -- 扫描的几帧里开始了加载（玩家在城镇里等着时不会发生）
      waited = waited + 0.5
    end
    stack_test(town_hero)
    if have then
      info("town", "城镇的载体还在（读档核对过），不再加")
      return
    end
    S.town_hero = town_hero.id
    add(town_hero, MARK, "town_a", { where = "town", label = "a", n = 1, steps = 0 }, "town")
    add(town_hero, MARK, "town_b", { where = "town", label = "b", n = 2, steps = 0 }, "town")
    info("town.add", ("城镇的载体加好了（%s）：现在做一个会改名册的操作（招募或解雇一个英雄），让游戏存一次档"):format(akey(town_hero)))
  end)
end

-- ---------------------------------------------------------------- 走廊：每走一步状态变化

local corridor_tried = false

dd.on("raid.step", function()
  if where == "battle" then
    return
  end
  for _, t in ipairs(tracked) do
    local st = t.id == MARK and t.actor.is_hero and dd.buff.state(t.actor, t.guid)
    if st and st.steps then
      st.steps = st.steps + 1
    end
  end
  -- 队伍里没有走廊载体（第一次出征、读档核对过的不算）时给队伍里的一个英雄加一条（备注 2：原来加在城镇标记的英雄身上，
  -- 他可能没出征）；在事件回调里，排到下一帧，done 收到 guid 时记下
  if not corridor_tried and has_tracked("corridor") == false then
    local h = alive(dd.actor.party())[1]
    if h then
      corridor_tried = true
      add(h, MARK, "corridor", { where = "corridor", steps = 1 }, "corridor")
    end
  end
end)

-- ---------------------------------------------------------------- 战斗：英雄、怪物、战斗结束删除的载体

dd.on("battle.round.start", function()
  for _, t in ipairs(tracked) do
    local st = t.where == "battle" and t.actor:valid() and dd.buff.state(t.actor, t.guid)
    if st then
      st.rounds = (st.rounds or 0) + 1
    end
  end
end)

-- 英雄的两条（battle、bend）在队伍里有活着的英雄时加；每个活着的怪物加一条 mon，这场里后来出现的怪物也加（审核 R3-B2：
-- 原来只给一个怪物加，它在读档前被打死时战斗中存档里就没有怪物的条目）。读进来的战斗不加（载体随存档恢复）
local function battle_phase()
  if battle_loaded then
    return
  end
  if not battle_added then
    local h = alive(dd.actor.party())[1]
    if h then
      battle_added = true
      add(h, MARK, "battle", { where = "battle", hits = 0, rounds = 0 }, "battle")
      add(h, BEND, "bend", { where = "battle", bend = true }, "battle")
    end
  end
  local new = 0
  for _, m in ipairs(alive(dd.actor.monsters())) do
    if not marked[m] then
      marked[m] = true
      new = new + 1
      add(m, MARK, "mon", { where = "battle", monster = true, rounds = 0 }, "battle")
    end
  end
  if new > 0 then
    info("battle.add", ("第 %d 场战斗的载体加好了（英雄 %s，怪物新加 %d 个）：等一个角色行动完（游戏存档），再退回主菜单读档；撤退那一场直接撤退"):format(
      battle_no, battle_added and "有" or "还没有", new))
  end
end

dd.every(0.5, function()
  refresh_where()
  local since = dd.time() - where_since
  if where == "town" and since >= 3 then
    town_phase()
  elseif where == "battle" and since >= 1 then
    battle_phase()
  end
end)

info("load", ("main.lua 执行完毕：场合 %s，上一次存档 %s"):format(where, prev.last and ("#" .. tostring(prev.last.index) .. "/" .. tostring(prev.last.where)) or "nil"))
return {}
