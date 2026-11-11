-- P3b 主菜单自检（rig.py deploy lua-dev）：在真实的 DDCore 服务下验证事件绑定、调度器、协程、预算和重载，
-- 结果写进 DDLua.log。主菜单上能触发的只有 app.tick（每帧）和 app.state，这里只用它们。
--
-- 每次加载都建立同一组「常驻」订阅（两个 dd.on 和一个 dd.every），用来核对重载前后订阅数不变。
-- 第一次加载另外跑一个检查协程（临时订阅：once、死循环、连续出错、wait_event），跑完从 app.tick 回调里请求重载自己；
-- 第二次加载核对 dd.state 保留、订阅数和第一次相同、新的订阅能收到事件。
local S = dd.state
S.loads = (S.loads or 0) + 1
S.counts = S.counts or {}

local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then
        passed = passed + 1
    else
        failed = failed + 1
        dd.log.error("自检失败：" .. name .. (detail ~= nil and ("（" .. tostring(detail) .. "）") or ""))
    end
end
local function report(stage)
    print(("事件自检（第 %d 次加载）%s：通过 %d 项，失败 %d 项"):format(S.loads, stage, passed, failed))
end

-- 常驻订阅
local ticks, frames = 0, {}
dd.on("app.tick", function(ev)
    ticks = ticks + 1
    frames[#frames + 1] = ev.frame
    if #frames > 8 then
        table.remove(frames, 1)
    end
end)
dd.on("app.state", function(ev) print(("app.state：%s -> %s"):format(ev.from_name, ev.to_name)) end, { priority = 5 })
dd.every(1, function() S.seconds = (S.seconds or 0) + 1 end)
S.counts[S.loads] = dd.subscriptions()

if S.loads == 1 then
    check("dd.dev 为真（开发模式）", dd.dev == true)
    local nope, why = dd.on("no.such.topic", print)
    check("不存在的主题返回 nil 和原因", nope == nil and tostring(why):find("主题不存在"), why)
    -- P3c：主菜单上没有存档，真实的 game 服务对任何 id 都解析不出句柄，不出错
    check("dd.actor 存在", type(dd.actor) == "table" and type(dd.actor.hero) == "function")
    check("主菜单上 dd.actor.hero / monster 返回 nil", dd.actor.hero(1) == nil and dd.actor.monster(1) == nil and dd.actor.hero(0) == nil)
    check("dd.actor.is 对非句柄为 false", dd.actor.is(nil) == false and dd.actor.is({}) == false)
    check("dd.actor.hero 拒绝字符串", not pcall(dd.actor.hero, "1"))
    local okscope, escope = pcall(dd.on, "app.tick", print, { scope = {} })
    check("scope 给表报错", not okscope and tostring(escope):find("actor 句柄"), escope)

    -- 第一次 app.tick：写只读字段、回调里 dd.wait 都要报错；保存负载留到之后检查失效
    local firstChecked = false
    dd.once("app.tick", function(ev)
        local ok, e = pcall(function() ev.frame = 1 end)
        check("app.tick 的字段只读", not ok and tostring(e):find("只读"), e)
        local ok2, e2 = pcall(dd.wait, 1)
        check("事件回调里不能 dd.wait", not ok2 and tostring(e2):find("只能在 dd.spawn"), e2)
        check("负载的元信息", ev.topic == "app.tick" and ev.deferred == false and ev.writable == false)
        S.kept = ev
        -- 在游戏 hook 的调用栈里无限递归 dd.spawn：到 Lua 的 C 调用上限报错（日志里一行「C stack overflow」），游戏不崩
        local depth = 0
        local function nest() depth = depth + 1 dd.spawn(nest) end
        dd.spawn(nest)
        print("事件回调里 dd.spawn 递归到深度 " .. depth)
        check("dd.spawn 递归在 C 调用上限停下", depth > 20 and dd.stats().tasks == 1, depth)
        firstChecked = true
    end)
    local onceCount = 0
    dd.once("app.tick", function() onceCount = onceCount + 1 end)
    -- 回调里的死循环：第一次就被预算打断，订阅立即停用（游戏只卡一帧，约 budget_ms）
    local loop = dd.on("app.tick", function() while true do end end)
    -- 连续出错：max_failures 次后停用
    local bad = dd.on("app.tick", function(ev) error("故意出错 frame=" .. ev.frame) end)

    dd.spawn(function()
        local t0 = dd.time()
        dd.wait(0.5)
        check("dd.wait(0.5) 至少过了 0.5 秒", dd.time() - t0 >= 0.5, dd.time() - t0)
        local snap = dd.wait_event("app.tick")
        check("wait_event 的快照", type(snap) == "table" and math.type(snap.frame) == "integer" and snap.topic == "app.tick"
            and snap.deferred == false, snap and snap.frame)
        local none = dd.wait_event("app.state", { to_name = "没有这个状态" }, 0.2)
        check("wait_event 超时返回 nil（filter 永远不匹配）", none == nil and dd.stats().waiters == 0)
        local ok, e = pcall(function() return S.kept.frame end)
        check("回调返回后负载失效", not ok and tostring(e):find("失效"), e)
        check("第一次 app.tick 的检查跑过", firstChecked)
        check("死循环的订阅被预算停用", not loop:active())
        check("连续出错的订阅被停用", not bad:active())
        check("once 只调用一次", onceCount == 1, onceCount)
        -- 启动阶段（as_load_boot_assets）帧率很低，这里只要求收到了多帧、帧号递增
        check("app.tick 每帧都收到", ticks >= 3 and frames[#frames] > frames[1], ticks)
        -- 定时器、协程里的死循环
        local looper = dd.every(0, function() while true do end end)
        dd.wait(0.1)
        check("定时器里的死循环被停用", not looper:active())
        local victim = dd.spawn(function() dd.wait(0) while true do end end)
        dd.wait(0.1)
        check("协程里的死循环被终止", victim:status() == "dead", victim:status())
        local stats = dd.stats()
        check("临时的订阅、等待者、定时器都已撤销", stats.subscriptions == S.counts[1] and stats.waiters == 0 and stats.timers == 1,
            ("subscriptions=%d waiters=%d timers=%d"):format(stats.subscriptions, stats.waiters, stats.timers))
        report("重载前")
        -- 在自己的回调里请求重载：推迟到下一帧
        dd.once("app.tick", function()
            local requested, err = dd.reload()
            check("dd.reload 排到下一帧", requested == true, err)
            S.reloadRequestedAt = dd.time()
        end)
    end)
else
    check("dd.state 跨重载保留", S.loads == 2 and S.reloadRequestedAt ~= nil, S.loads)
    check("重载后常驻订阅数和第一次相同", S.counts[2] == S.counts[1], ("%s / %s"):format(S.counts[1], S.counts[2]))
    dd.spawn(function()
        dd.wait(0.5)
        check("重载后的订阅收到 app.tick", ticks >= 3, ticks)
        check("重载后订阅数仍然不变", dd.subscriptions() == S.counts[1], dd.subscriptions())
        local stats = dd.stats()
        check("重载后只有常驻的定时器和这个协程", stats.timers == 1 and stats.tasks == 1 and stats.loads == 2,
            ("timers=%d tasks=%d loads=%d"):format(stats.timers, stats.tasks, stats.loads))
        report("重载后（自检结束）")
    end)
end

return { loads = S.loads }
