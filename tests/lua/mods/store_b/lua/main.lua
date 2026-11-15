-- 夹具（P4c1）：campaign.save 的订阅。DDLua 在所有 campaign.save 回调之后才取 dd.store 的快照，回调里改的会写进去
MAIN = { counter = dd.store.counter }
MAIN.status, MAIN.reason = dd.store_status()
SAVES = {}
dd.on("campaign.save", function(ev)
    SAVES[#SAVES + 1] = { files = ev.files, in_raid = ev.in_raid, in_battle = ev.in_battle, index = ev.index }
    dd.store.saved_in_callback = ev.index
end)
return {}
