-- Lua Mod 模板（DDLua，Lua API 1）。
--
-- 用法：
-- 1. 把整个目录复制到 <游戏根目录>\mods\<目录名>。
-- 2. 改 project.xml 的 <Title>：游戏和存档按标题识别本地 Mod，标题不能和别的 Mod 重复。
-- 3. 改 lua/manifest.json 的 id（日志、依赖、dd.import 都用它）和 version；依赖别的 Lua Mod 时写进 dependencies，
--    例如 [ "lib_a", { "id": "lib_b", "version": "1.2" } ]。
-- 4. 在存档的 Mod 列表里勾选这个 Mod。DDLua 在进入存档时执行本文件，退出存档时自动撤销它注册的
--    订阅、定时器和协程。日志在 <游戏根目录>\DDModLoader\log\DDLua.log，每行以 [Mod id] 开头。
--
-- 编辑器：.luarc.json 让 LuaLS 加载 DD.ModLoader/sdk/lua 下的类型声明（dd.lua、dd_topics.lua、sandbox.lua）。
-- 在仓库外使用时，把 workspace.library 改成 sdk/lua 的实际路径。
-- 沙箱：没有 io、package、dofile；os 只有 time/clock/date/difftime；require 只在本 Mod 的 lua/ 下找。

dd.log.info("已加载，版本 " .. dd.version)

-- 例：英雄受到伤害后写一行日志（只读，不改游戏）。负载 ev 只在回调里有效，actor 句柄可以保存
dd.on("damage.post", function(ev)
  local target = ev.target
  if target and target.is_hero then
    dd.log.info(string.format("%s 受到 %.1f 点伤害，剩余生命 %.1f", tostring(target), ev.damage, ev.hp_after))
  end
end)

-- main.lua 的返回值是给别的 Mod 用的导出：dd.import("my_lua_mod")
return {}
