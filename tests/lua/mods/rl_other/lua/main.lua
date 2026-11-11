-- 夹具（P3b）：和重载无关的 Mod，重载别的 Mod 时它不受影响
OTHER_LOADED = (OTHER_LOADED or 0) + 1
dd.on("app.tick", function() end)
return {}
