-- 夹具（P3b）：依赖 rl_base，rl_base 重载时它也重载，拿到新的导出
BASE_VALUE = dd.import("rl_base").value
dd.on("app.tick", function() end)
return {}
