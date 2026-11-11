-- 夹具（P3b）：被重载的 Mod。dd.state 跨重载保留；订阅、定时器、协程在重载时撤销再重建
dd.state.loads = (dd.state.loads or 0) + 1
dd.state.old_on = dd.state.old_on or dd.on  -- 第一次加载时的 dd.on，重载后调用要报错
dd.on("app.tick", function() BASE_TICKS = (BASE_TICKS or 0) + 1 end)
dd.every(1, function() end)
dd.spawn(function()
    local guard <close> = setmetatable({}, { __close = function() print("rl_base 的协程被关闭（第 " .. dd.state.loads .. " 次加载的）") end })
    dd.wait(1000)
end)
return { value = dd.state.loads }
