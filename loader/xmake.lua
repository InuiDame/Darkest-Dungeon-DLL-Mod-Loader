-- DDModLoader：dbghelp 代理，导出只有 MiniDumpWriteDump（proxy.def，LIBRARY dbghelp）
-- 输出 DDModLoader.dll，不自动部署；发布时由安装包改名为 dbghelp.dll
target("DDModLoader")
    add_rules("dd.native")
    set_values("dd.dkutil", "off")
    set_values("dd.rc", "off")
    set_version("1.0.0")

    set_pcxxheader("pch.h")
    add_files("dllmain.cpp", "DDModLoader.rc", "proxy.def")
    add_headerfiles("*.h")
    add_includedirs(".", "../common", "../core/src/game")
    add_defines("DDMODLOADER_EXPORTS", "_WINDOWS", "_USRDLL")
target_end()
