-- P3d 场景测试：main.lua 在注册了订阅、定时器、协程之后故意出错。这个 Mod 应当加载失败，它注册的东西全部撤销
-- （DDLua.log 有「加载失败」「已拆除」，之后再也不出现 [p3d_broken] 的「失败」行），别的 Mod 照常加载。

dd.on("damage.pre", function()
  dd.log.error("失败 [broken] 加载失败的 Mod 的订阅还在被调用")
end)

dd.after(1, function()
  dd.log.error("失败 [broken] 加载失败的 Mod 的定时器还在运行")
end)

dd.spawn(function()
  dd.wait(1)
  dd.log.error("失败 [broken] 加载失败的 Mod 的协程还在运行")
end)

error("p3d_broken 故意在 main.lua 里出错（场景测试，应当加载失败）")
