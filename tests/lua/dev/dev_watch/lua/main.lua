-- P3b 主菜单自检：文件监视（dev_mode = 1）。自检脚本在游戏运行时往 lua/ 下临时加一个 touched.lua、过一会儿删掉，
-- 每次变化触发一次重载；dd.state 记加载次数，日志里的订阅核对证明重载没有留下订阅
dd.state.loads = (dd.state.loads or 0) + 1
dd.on("app.tick", function() end)
print(("dev_watch 第 %d 次加载：订阅 %d 个，lua/touched.lua %s"):format(dd.state.loads, dd.subscriptions(),
    dd.fs.exists("lua/touched.lua") and "存在" or "不存在"))
