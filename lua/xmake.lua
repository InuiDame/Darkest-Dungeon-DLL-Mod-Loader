-- DDLua.dll：Lua 运行时（P3），native 插件，部署到 mods\DD Mod Lib，和 DDCore 一起（玩家不可关闭，
-- 没有启用 Lua Mod 时只有 lifecycle 订阅）。运行时源码（除 plugin.cpp）也编进离线测试 dd_lua_tests。
target("DDLua")
    add_rules("dd.native")
    set_values("dd.dkutil", "off")
    set_values("dd.mod", "DD Mod Lib")
    set_version("1.0.0")

    add_files("src/*.cpp")
    add_headerfiles("src/*.h")
    add_packages("lua", "nlohmann_json")
target_end()
