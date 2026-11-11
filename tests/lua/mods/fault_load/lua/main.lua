-- 夹具（审核 B-Y3 的后续，主线的决定）：执行 main.lua 时调用 dd.actor.hero(666)。离线测试让假的 game 服务在这个 id 上
-- 触发访问违例，模拟执行 main.lua 时发生原生异常（SEH）。DDLua 要自己接住，不能让 DDCore 撤销 CampaignLoad 订阅
dd.actor.hero(666)
print("加载正常")
