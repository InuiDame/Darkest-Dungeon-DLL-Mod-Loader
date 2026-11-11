-- 官方测试集用的独立解释器。用法见同目录的 README.md：复制成 build/luatests/standalone/xmake.lua 再构建。
-- lua_dd：原版 lua.c + 仓库的补丁库（xmake/lua.lua 的本地包，同一个包哈希）+ ddstubs.cpp 的最简单的桩
-- lua_plain：原版 lua.c + 同一个补丁库，回调全部保持库里的默认值（NULL、默认密钥）
-- lua_vanilla：原版 5.4.9 全部源码，同样按 C++ 编译、同样的配置（没有 LUA_COMPAT_5_3），用来区分平台和配置造成的失败
set_project("luatests")
includes("../../../xmake/lua.lua")
add_rules("mode.release")
set_runtimes("MT")

target("lua_dd")
    set_kind("binary")
    add_files("lua.c", "ddstubs.cpp")
    add_cflags("/TP", "/EHsc", { force = true })
    add_cxflags("/EHsc", "/utf-8")
    add_packages("lua")

target("lua_plain")
    set_kind("binary")
    add_files("lua.c")
    add_cflags("/TP", "/EHsc", { force = true })
    add_packages("lua")

target("lua_vanilla")
    set_kind("binary")
    add_files("../vanilla-src/lua-5.4.9/src/*.c|luac.c|onelua.c")
    add_cflags("/TP", "/EHsc", { force = true })
