-- 示例 Mod：DDLua 的常用写法。只读，只写日志（DDLua.log 里以 [hello_lua] 开头），不改任何游戏数据。
-- 演示：require、dd.fs、dd.on（scope、filter）、actor 句柄、dd.spawn + dd.wait_event、dd.after、导出。

local util = require("util") -- lua/util.lua，每个 Mod 自己的 require 缓存

local greeting = dd.fs.read("data/greeting.txt") -- 只能读本 Mod 目录里的文件
dd.log.info("已加载（版本 " .. dd.version .. "）：" .. util.trim(greeting or "没有 data/greeting.txt"))

local battles = 0 -- 本次进入存档以来的战斗数（退出存档时整个 Mod 被拆除，下次进存档从 0 开始）
local hero_hits = 0 -- 本场英雄受到伤害的次数

dd.on("battle.start", function()
  battles = battles + 1
  hero_hits = 0
  dd.log.info("战斗开始：本次进入存档后的第 " .. battles .. " 场")
  -- 只在本场战斗里有效的订阅：战斗结束时 DDCore 自动撤销
  dd.on("damage.post", function(ev)
    local target = ev.target -- Hero/Monster 指针字段是 actor 句柄，其余指针是 nil
    if target and target.is_hero then
      hero_hits = hero_hits + 1
      if hero_hits <= 5 then
        dd.log.info(string.format("%s 受到 %.1f 点伤害，生命 %s", util.name(target), ev.damage, util.hp(target)))
      end
    end
  end, { scope = "battle" })
end)

-- 协程：等战斗结束写一行小结。dd.wait_event 拿到的是字段快照（普通表），下一帧交给协程
dd.spawn(function()
  while true do
    dd.wait_event("battle.end")
    dd.log.info(string.format("战斗结束：英雄受到伤害 %d 次；本 Mod 现在有订阅 %d 个", hero_hits, dd.subscriptions()))
  end
end)

dd.after(3, function()
  dd.log.info(string.format("进入存档 3 秒了（dd.time() = %.1f）", dd.time()))
end)

-- 导出：别的 Mod 可以 dd.import("hello_lua").battles()
return {
  battles = function() return battles end,
}
