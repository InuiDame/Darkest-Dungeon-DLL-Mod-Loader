-- 不依赖游戏的单元测试，核心逻辑和 DDCore 同一份源码
target("dd_core_tests")
    set_kind("binary")
    set_default(false)
    set_group("tests")
    set_languages("c++23")
    set_runtimes(is_mode("debug") and "MTd" or "MT")
    set_exceptions("cxx")
    set_warnings("all")
    add_cxflags("/utf-8", "/permissive-", "/Zc:__cplusplus", "/Zc:preprocessor", { tools = "cl" })
    add_defines("UNICODE", "_UNICODE")
    add_includedirs("../sdk/include")
    add_files("unit/test_main.cpp", "unit/c_compat.c")
    add_files("../core/src/logic/*.cpp", "../core/src/hook/*.cpp", "../core/src/runtime/*.cpp", "../core/src/events/hub.cpp",
        "../core/src/game/phase_tracker.cpp", "../core/src/game/actors.cpp", "../core/src/guard.cpp")
    add_syslinks("user32", "shlwapi")
target_end()

-- Lua 本地包和 sol2 的冒烟测试，编译选项和 DDLua 相同（dd.native）
target("lua_smoke")
    add_rules("dd.native")
    set_values("dd.kind", "binary")
    set_values("dd.dkutil", "off")
    set_values("dd.rc", "off")
    set_default(false)
    set_group("tests")
    add_files("unit/lua_smoke.cpp")
    add_packages("lua", "sol2")
target_end()

-- DDLua 运行时的离线测试（P3a 起）：编译 lua/src 下的源码（P3d 起含 plugin.cpp，场景预演经过它），选项同 DDLua（dd.native），
-- Mod 夹具在 tests/lua/mods。假的 events 服务直接包装 DDCore 的事件中枢（hub.cpp、access.h），语义和真实服务一致；
-- P3d 的测试还读 sdk/lua（主题表、LuaLS 声明）、templates/lua、examples/lua，场景预演的日志写到 build/p3d_rehearsal
target("dd_lua_tests")
    add_rules("dd.native")
    set_values("dd.kind", "binary")
    set_values("dd.dkutil", "off")
    set_values("dd.rc", "off")
    set_default(false)
    set_group("tests")
    add_files("unit/lua_tests.cpp", "../lua/src/*.cpp", "../core/src/events/hub.cpp")
    add_includedirs("../lua/src", "../core/src")
    add_packages("lua", "nlohmann_json")
    local fixtures = (path.join(os.scriptdir(), "lua"):gsub("\\", "/"))
    add_defines('DD_LUA_FIXTURES="' .. fixtures .. '"')
    local project_root = (path.directory(os.scriptdir()):gsub("\\", "/"))
    add_defines('DD_PROJECT_ROOT="' .. project_root .. '"')
target_end()

-- 负面测试插件：一个源文件，按 NEG_KIND 编译成不同行为的 DLL
local negatives = {
    "neg_abi_mismatch",
    "neg_missing_dep",
    "neg_load_false",
    "neg_load_crash",
    "neg_query_crash",
    "neg_dep_on_failed",
    "neg_cycle_a",
    "neg_cycle_b",
    "neg_hook_conflict",
    "neg_abi_unset",
    "neg_hook_bad_args",
}
for _, name in ipairs(negatives) do
    target(name)
        add_rules("dd.native")
        set_values("dd.dkutil", "off")
        set_values("dd.rc", "off")
        set_default(false)
        set_group("tests")
        add_files("plugins/negative.cpp")
        add_defines("NEG_KIND_" .. name:upper())
    target_end()
end

-- 事件系统验证插件：订阅全部主题，战斗中验证修改和取消
target("test_events")
    add_rules("dd.native")
    set_values("dd.dkutil", "off")
    set_values("dd.rc", "off")
    set_default(false)
    set_group("tests")
    add_files("plugins/test_events.cpp")
target_end()

-- P3 开工前逆向结论的运行探针：只读，核对启用 Mod 列表、名册、Actor 反查和修正后的字段
target("probe_p3")
    add_rules("dd.native")
    set_values("dd.dkutil", "off")
    set_values("dd.rc", "off")
    set_default(false)
    set_group("tests")
    add_files("plugins/probe_p3.cpp")
target_end()

-- P3c 的运行探针：只读，核对 actor 句柄（game 服务）的转换、解析、字段读取和失效
target("probe_p3c")
    add_rules("dd.native")
    set_values("dd.dkutil", "off")
    set_values("dd.rc", "off")
    set_default(false)
    set_group("tests")
    add_files("plugins/probe_p3c.cpp")
target_end()

-- P3d：把 DDCore 的主题表写成 JSON（只查询，不订阅事件），tools/gen_lua_types.py 据此生成 LuaLS 的主题注解
target("topic_dump")
    add_rules("dd.native")
    set_values("dd.dkutil", "off")
    set_values("dd.rc", "off")
    set_default(false)
    set_group("tests")
    add_files("plugins/topic_dump.cpp")
target_end()

-- 串链验证插件：和 hello_native hook 同一目标
target("hello_chain")
    add_rules("dd.native")
    set_values("dd.dkutil", "off")
    set_values("dd.rc", "off")
    set_default(false)
    set_group("tests")
    add_files("plugins/hello_chain.cpp")
target_end()
