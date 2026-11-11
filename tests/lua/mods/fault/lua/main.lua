-- 夹具（审核 B-Y3）：每帧的定时器里调用 dd.actor.hero(666)。离线测试让假的 game 服务在这个 id 上触发访问违例，
-- 模拟 lifecycle TICK 里执行 Lua 代码时发生原生异常（SEH）
dd.every(0, function()
  print("定时器")
  dd.actor.hero(666)
end)
