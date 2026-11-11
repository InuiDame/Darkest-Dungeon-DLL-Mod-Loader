-- DDCore.dll：native 插件运行时，部署到 mods\DD Mod Lib（玩家不可关闭，没有新协议插件时休眠）
target("DDCore")
    add_rules("dd.native")
    set_values("dd.dkutil", "off")
    set_values("dd.mod", "DD Mod Lib")
    set_version("1.0.0")

    add_files("src/main.cpp", "src/core.cpp", "src/guard.cpp", "src/log.cpp")
    add_files("src/hook/*.cpp", "src/logic/*.cpp", "src/runtime/*.cpp", "src/game/*.cpp", "src/plugin/*.cpp", "src/events/*.cpp")
    add_headerfiles("src/**.h", "src/game/signatures.inc")
    add_syslinks("shlwapi")
target_end()
