-- P3d 场景测试：出错和超出执行预算的回调。它的问题不能影响别的 Mod（p3d_core 在 battle.end 检查），
-- 游戏也不能卡死：每次死循环在执行预算（默认 50 毫秒）处被打断，订阅、定时器停用，协程终止。
-- 不改任何游戏数据。依赖 p3d_core（manifest 的 dependencies），所以总在它之后加载。

local post_calls = 0
local post_sub = dd.on("damage.post", function()
  post_calls = post_calls + 1
  error("p3d_faulty 故意在 damage.post 回调里出错（第 " .. post_calls .. " 次）")
end)

local pre_calls = 0
local pre_sub = dd.on("damage.pre", function(ev)
  pre_calls = pre_calls + 1
  local missing = nil
  return missing.field -- 运行时错误：attempt to index a nil value
end, { priority = -100 })

-- 第一次 battle.start：回调里死循环，应在预算处被打断并立即停用
local loop_sub
loop_sub = dd.on("battle.start", function()
  dd.log.info("信息 [budget] battle.start 回调进入死循环，应在执行预算处被打断")
  while true do
  end
end)

-- 第一次战斗开始后：定时器和协程里的死循环（协程里的 pcall 吞不掉预算错误）
local armed = false
dd.on("battle.start", function()
  if armed then
    return
  end
  armed = true
  dd.after(1, function()
    dd.log.info("信息 [budget] 定时器回调进入死循环")
    while true do
    end
  end)
  dd.spawn(function()
    dd.wait(2)
    dd.log.info("信息 [budget] 协程进入死循环（包在 pcall 里）")
    pcall(function()
      while true do
      end
    end)
    dd.log.error("失败 [budget] pcall 吞掉了预算错误，协程继续执行了")
  end)
end)

dd.log.info("信息 [load] p3d_faulty 已加载：订阅 " .. dd.subscriptions() .. " 个")

return {
  post_calls = function() return post_calls end,
  post_active = function() return post_sub:active() end,
  pre_calls = function() return pre_calls end,
  pre_active = function() return pre_sub:active() end,
  loop_active = function() return loop_sub:active() end,
}
