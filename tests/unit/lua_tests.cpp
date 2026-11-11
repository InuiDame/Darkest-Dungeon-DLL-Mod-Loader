// DDLua 运行时的离线测试。P3a：配置、路径规范化、manifest 和加载顺序、会话的加载和拆除、
// 沙箱逃逸用例、执行预算（主 chunk、pcall 吞错、协程、模式匹配和其他 C 函数的长循环）、内存上限。
// P3b：事件绑定（读写、取消、优先级、filter、负载失效、延迟事件、作用域、连续失败）、调度器和协程、
// 热重载和 dd.state、文件监视、切换存档后订阅清零。
// P3c：actor 句柄（假的 game 服务，语义同 DDCore 的 core/src/game/actors.cpp）：字段读取、驻留和比较、
// 跨存档和跨探索失效、事件指针字段转句柄、按句柄过滤、actor 作用域、wait_event 快照里的句柄。
// P3d：Lua Mod 模板和示例的加载运行、LuaLS 声明和实际 API 的对照、场景 Mod 的离线预演（经过真实的 plugin.cpp，
// 主题表用 sdk/lua/topics.json，日志按 DDLua.log 的格式写到 build/p3d_rehearsal/，给 tools/p3_scenario_check.py 用）。
// 编译 lua/src 下的全部源码（P3d 起含 plugin.cpp），用假的 log 服务当宿主；P3b 的 events 服务直接包装 DDCore 的
// 事件中枢（hub.cpp、access.h）。Mod 夹具在 tests/lua/mods。
// 运行：xmake build dd_lua_tests && xmake run dd_lua_tests [-v]

#include "config.h"
#include "events/access.h"
#include "manifest.h"
#include "modfs.h"
#include "session.h"
#include "vm.h"

#include <lauxlib.h>
#include <lddstep.h>
#include <lualib.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifndef DD_LUA_FIXTURES
#error "DD_LUA_FIXTURES 未定义"
#endif

namespace {

int g_checks = 0;
int g_failures = 0;
bool g_verbose = false;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

// ---------------------------------------------------------------- 假宿主

std::vector<std::string> g_log;
FILE* g_logSink = nullptr;  // 设置时按 DDCore 日志的格式另写一份（P3d 场景预演）
double g_logClock = 0;      // 写进日志的时间（秒，从 10:00:00 起）

const char* LevelName(int level)
{
    switch (level) {
    case DD_LOG_DEBUG:
        return "DEBUG";
    case DD_LOG_INFO:
        return "INFO";
    case DD_LOG_WARN:
        return "WARN";
    case DD_LOG_ERROR:
        return "ERROR";
    default:
        return "TRACE";
    }
}

void WriteLogLine(FILE* file, int level, const std::string& message)
{
    const auto ms = static_cast<long long>(g_logClock * 1000);
    std::fprintf(file, "[%02lld:%02lld:%02lld.%03lld] [%s] %s\r\n", 10 + ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000,
        LevelName(level), message.c_str());
}

void DD_CALL FakeLogWrite(DDPluginHandle, int level, const char* message)
{
    g_log.emplace_back(message);
    if (g_logSink) {
        WriteLogLine(g_logSink, level, message);
    }
    if (g_verbose) {
        std::printf("    [log %d] %s\n", level, message);
    }
}

const DDLogService kFakeLog = { { sizeof(DDLogService), DD_SERVICE_LOG_VERSION }, &FakeLogWrite };

ddlua::Host MakeHost()
{
    ddlua::Host host;
    host.log = &kFakeLog;
    return host;
}

bool LogHas(const std::string& needle)
{
    for (const auto& line : g_log) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string Fixture(const char* name)
{
    return std::string(DD_LUA_FIXTURES) + "/mods/" + name;
}

std::vector<ddlua::ModSource> Sources(std::initializer_list<const char*> names)
{
    std::vector<ddlua::ModSource> out;
    for (const char* name : names) {
        out.push_back({ std::string("夹具 ") + name, Fixture(name) });
    }
    return out;
}

ddlua::Config TestConfig()
{
    ddlua::Config config;
    config.budgetMs = 50;
    config.loadBudgetMs = 200;
    config.memoryLimitMb = 64;
    // 原有的用例按「一帧做完全部到点的工作」写断言；帧预算（默认 20 毫秒）由 TestFrameBudget、TestReloadBudget 单独测
    config.frameBudgetMs = 60000;
    return config;
}

double Ms(std::chrono::steady_clock::time_point since)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

// ---------------------------------------------------------------- 配置

void TestConfigParse()
{
    std::vector<std::string> warnings;
    auto c = ddlua::ParseConfig("", &warnings);
    CHECK(c.memoryLimitMb == 128 && c.budgetMs == 50 && c.loadBudgetMs == 500 && !c.devMode && c.devGlobalDirs.empty());
    CHECK(c.maxFailures == 3 && c.maxPerMod == 1000);
    CHECK(warnings.empty());

    c = ddlua::ParseConfig("\xEF\xBB\xBF# 注释\nbudget_ms = 20  # 行尾注释\nmemory_limit_mb=32\ndev_mode = 1\n"
                           "dev_global_dirs = D:\\a ; mods\\b;;\nload_budget_ms = 0\nfoo = 1\nbroken line\n",
        &warnings);
    CHECK(c.budgetMs == 20 && c.memoryLimitMb == 32 && c.devMode);
    CHECK(c.devGlobalDirs.size() == 2 && c.devGlobalDirs[0] == "D:\\a" && c.devGlobalDirs[1] == "mods\\b");
    CHECK(c.loadBudgetMs == 500);  // 0 不合法，保留默认
    CHECK(warnings.size() == 3);   // load_budget_ms、未知键、没有 '='

    std::vector<std::string> warnings2;
    c = ddlua::ParseConfig("max_failures = 5\nmax_per_mod = 64\nmax_failures = 0\n", &warnings2);
    CHECK(c.maxFailures == 5 && c.maxPerMod == 64 && warnings2.size() == 1);
    std::vector<std::string> warnings3;
    c = ddlua::ParseConfig("log_max_mb = 8\nteardown_budget_ms = 0\n", &warnings3);
    CHECK(c.logMaxMb == 8 && c.teardownBudgetMs == 200 && warnings3.size() == 1);
}

// ---------------------------------------------------------------- 路径

void TestNormalize()
{
    std::wstring out;
    CHECK(ddlua::NormalizeRelative("", out, nullptr) && out.empty());
    CHECK(ddlua::NormalizeRelative("a/b\\c.txt", out, nullptr) && out == L"a\\b\\c.txt");
    CHECK(ddlua::NormalizeRelative("./a//b/", out, nullptr) && out == L"a\\b");
    CHECK(ddlua::NormalizeRelative("数据/说明.txt", out, nullptr) && out == L"数据\\说明.txt");
    const char* rejected[] = { "../x", "a/../b", "..", "/abs", "\\abs", "\\\\server\\share", "C:/Windows", "c:x", "a:stream",
        "file.txt:ads", "a/*.lua", "a?", "a\"b", "a<b", "a|b", "a/...", "a/. ", "trail.", "trail ", "x\x01y" };
    for (const char* path : rejected) {
        std::string error;
        const bool ok = ddlua::NormalizeRelative(path, out, &error);
        CHECK(!ok);
        if (ok) {
            std::printf("    没有拒绝：%s\n", path);
        }
    }
    CHECK(!ddlua::NormalizeRelative(std::string_view("a\0b", 3), out, nullptr));
    CHECK(!ddlua::NormalizeRelative("\xFF\xFE", out, nullptr));
}

// ---------------------------------------------------------------- manifest 和顺序

ddlua::Manifest M(const char* id, std::vector<ddlua::ManifestDep> deps = {}, const char* version = "1.0.0", int api = 1)
{
    ddlua::Manifest m;
    m.id = id;
    ddlua::Version::Parse(version, m.version);
    m.versionText = version;
    m.api = api;
    m.deps = std::move(deps);
    return m;
}

ddlua::ManifestDep D(const char* id, const char* min = "0")
{
    ddlua::ManifestDep dep;
    dep.id = id;
    ddlua::Version::Parse(min, dep.minVersion);
    return dep;
}

bool Rejected(const ddlua::LoadOrder& order, std::size_t index, const char* reason)
{
    for (const auto& [i, text] : order.rejected) {
        if (i == index && text.find(reason) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void TestManifest()
{
    ddlua::Version v;
    CHECK(ddlua::Version::Parse("1", v) && v.major == 1 && v.minor == 0 && v.patch == 0);
    CHECK(ddlua::Version::Parse("1.2.3", v) && v.major == 1 && v.minor == 2 && v.patch == 3);
    CHECK(!ddlua::Version::Parse("1.2.3.4", v) && !ddlua::Version::Parse("1..2", v) && !ddlua::Version::Parse("v1", v) &&
          !ddlua::Version::Parse("", v) && !ddlua::Version::Parse("1.-2", v));
    CHECK(ddlua::ValidModId("my_mod-1.x") && !ddlua::ValidModId("") && !ddlua::ValidModId(".x") &&
          !ddlua::ValidModId("a b") && !ddlua::ValidModId("中文") && !ddlua::ValidModId(std::string(64, 'a')));

    ddlua::Manifest m;
    std::string error;
    CHECK(ddlua::ParseManifest(R"({"id":"a","version":"1.2","api":1,"dependencies":["b",{"id":"c","version":"2.0.1"}],"name":"x"})", m, &error));
    CHECK(m.id == "a" && m.version.minor == 2 && m.api == 1 && m.deps.size() == 2 && m.deps[1].id == "c" && m.deps[1].minVersion.patch == 1);
    const char* bad[] = { "", "[]", "{", R"({"version":"1","api":1})", R"({"id":"a b","version":"1","api":1})",
        R"({"id":"a","api":1})", R"({"id":"a","version":"x","api":1})", R"({"id":"a","version":"1"})",
        R"({"id":"a","version":"1","api":"1"})", R"({"id":"a","version":"1","api":1,"dependencies":{}})",
        R"({"id":"a","version":"1","api":1,"dependencies":[1]})", R"({"id":"a","version":"1","api":1,"dependencies":[{"version":"1"}]})",
        R"({"id":"a","version":"1","api":1,"dependencies":[{"id":"b","version":"x"}]})" };
    for (const char* text : bad) {
        CHECK(!ddlua::ParseManifest(text, m, &error));
    }

    // 同层按输入顺序：c 依赖 a，结果 a b（第一层）然后 c
    auto order = ddlua::ResolveLoadOrder({ M("c", { D("a") }), M("a"), M("b") });
    CHECK(order.rejected.empty() && order.order == std::vector<std::size_t>({ 1, 2, 0 }));
    // 多层：d -> c -> b -> a
    order = ddlua::ResolveLoadOrder({ M("d", { D("c") }), M("c", { D("b") }), M("b", { D("a") }), M("a") });
    CHECK(order.order == std::vector<std::size_t>({ 3, 2, 1, 0 }));
    // id 重复：两个都拒绝，依赖它的也拒绝
    order = ddlua::ResolveLoadOrder({ M("x"), M("dup"), M("dup"), M("needs", { D("dup") }) });
    CHECK(order.order == std::vector<std::size_t>({ 0 }));
    CHECK(Rejected(order, 1, "重复") && Rejected(order, 2, "重复") && Rejected(order, 3, "id 重复"));
    // api 不符、缺依赖、版本不够、依赖被拒绝、依赖自己
    order = ddlua::ResolveLoadOrder({ M("api", {}, "1.0.0", 2), M("miss", { D("nope") }), M("base", {}, "1.5.0"),
        M("low", { D("base", "1.6") }), M("ok", { D("base", "1.5") }), M("chain", { D("low") }), M("self", { D("self") }) });
    CHECK(order.order == std::vector<std::size_t>({ 2, 4 }));
    CHECK(Rejected(order, 0, "api 2") && Rejected(order, 1, "缺少依赖 nope") && Rejected(order, 3, "低于要求的 1.6.0") &&
          Rejected(order, 5, "依赖 low 不加载") && Rejected(order, 6, "依赖自己"));
    // 循环和依赖环的
    order = ddlua::ResolveLoadOrder({ M("a", { D("b") }), M("b", { D("a") }), M("z", { D("a") }), M("free") });
    CHECK(order.order == std::vector<std::size_t>({ 3 }));
    CHECK(Rejected(order, 0, "依赖循环：a -> b -> a") && Rejected(order, 1, "依赖循环") && Rejected(order, 2, "依赖循环"));
}

// ---------------------------------------------------------------- 会话：加载、失败隔离、拆除

ddlua::ModState StateOf(const ddlua::Session& session, const char* label)
{
    for (const auto& mod : session.mods()) {
        if (mod->label == std::string("夹具 ") + label) {
            return mod->state;
        }
    }
    return ddlua::ModState::TornDown;
}

bool HasMod(const ddlua::Session& session, const char* label)
{
    for (const auto& mod : session.mods()) {
        if (mod->label == std::string("夹具 ") + label) {
            return true;
        }
    }
    return false;
}

void TestSessionLoad()
{
    using ddlua::ModState;
    g_log.clear();
    const auto host = MakeHost();
    {
        // other 排在 basic 前面，依赖排序后 basic 先加载
        ddlua::Session session(host, TestConfig(), "测试");
        session.Load(Sources({ "other", "not_lua", "broken", "needs_broken", "bad_json", "bad_api", "dup_a", "dup_b",
            "missing_dep", "low_version", "cycle_a", "cycle_b", "syntax_error", "loop_main", "basic" }));
        CHECK(session.vm() != nullptr);
        CHECK(StateOf(session, "basic") == ModState::Loaded);
        CHECK(StateOf(session, "other") == ModState::Loaded);
        CHECK(StateOf(session, "broken") == ModState::Failed);
        CHECK(StateOf(session, "needs_broken") == ModState::Failed);
        CHECK(StateOf(session, "bad_api") == ModState::Failed);
        CHECK(StateOf(session, "dup_a") == ModState::Failed && StateOf(session, "dup_b") == ModState::Failed);
        CHECK(StateOf(session, "missing_dep") == ModState::Failed && StateOf(session, "low_version") == ModState::Failed);
        CHECK(StateOf(session, "cycle_a") == ModState::Failed && StateOf(session, "cycle_b") == ModState::Failed);
        CHECK(StateOf(session, "syntax_error") == ModState::Failed && StateOf(session, "loop_main") == ModState::Failed);
        CHECK(!HasMod(session, "not_lua") && !HasMod(session, "bad_json"));
        CHECK(session.LoadedCount() == 2);

        CHECK(LogHas("[basic] hello from basic\t1\ttrue"));
        // 第一层（没有依赖或依赖已排出的）按启用列表顺序，第二层是 other、needs_broken
        CHECK(LogHas("加载顺序：broken syntax_error loop_main basic other needs_broken"));
        // 错误信息带 Mod id、文件、行号和 traceback
        CHECK(LogHas("[broken] 加载失败：broken/main.lua:4: boom from broken"));
        CHECK(LogHas("[broken] stack traceback:"));
        CHECK(LogHas("[needs_broken] 不加载：依赖 broken 加载失败"));
        CHECK(LogHas("lua/manifest.json 无效"));
        CHECK(LogHas("[bad_api] 不加载：api 99"));
        CHECK(LogHas("[dup] 不加载：id dup 重复"));
        CHECK(LogHas("[missing_dep] 不加载：缺少依赖 nope"));
        CHECK(LogHas("[low_version] 不加载：依赖 basic 的版本 1.2.0 低于要求的 2.0.0"));
        CHECK(LogHas("依赖循环：cycle_a -> cycle_b -> cycle_a"));
        CHECK(LogHas("[syntax_error] 加载失败：syntax_error/main.lua:1:"));
        CHECK(LogHas("[loop_main] main.lua 超出预算，加载失败：执行超出预算（200 毫秒）"));

        // 失败的 Mod 撤销了一切：导出拿不到，环境已释放
        auto* basic = session.FindMod("basic");
        CHECK(basic != nullptr);
        std::string error;
        CHECK(session.RunChunk(*basic, "assert(dd.import('broken') == nil) assert(dd.import('other') == nil) "
                                       "assert(dd.import('basic').value == 42)", "=t", &error));
        for (const auto& mod : session.mods()) {
            if (mod->state == ModState::Failed) {
                CHECK(mod->envRef == LUA_NOREF && mod->requireRef == LUA_NOREF && mod->exportsRef == LUA_NOREF);
            }
        }
        // 失败 Mod 的清理函数在加载失败时执行（P3b 的订阅挂在这里）
        session.Teardown();
        CHECK(session.memory().residual == 0);
        CHECK(LogHas("拆除 2 个 Lua Mod"));
        CHECK(LogHas("[other] 已拆除") && LogHas("[basic] 已拆除"));
    }
    {
        // 没有 Lua Mod 时不创建 VM
        g_log.clear();
        ddlua::Session session(host, TestConfig(), "空");
        session.Load(Sources({ "not_lua" }));
        CHECK(session.vm() == nullptr);
        CHECK(LogHas("不创建 VM"));
    }
}

void TestTeardownAndRebuild()
{
    const auto host = MakeHost();
    std::size_t firstBaseline = 0;
    for (int round = 0; round < 2; ++round) {
        // 模拟切换存档：每次一个新会话，拆除后内存回落、VM 关闭后没有残留
        g_log.clear();
        auto config = TestConfig();
        config.budgetMs = 5000;  // 填充大量数据，不测预算
        ddlua::Session session(host, config, "存档#" + std::to_string(round + 1));
        session.Load(Sources({ "basic", "other", "sandbox" }));
        CHECK(session.LoadedCount() == 3);
        auto* sandbox = session.FindMod("sandbox");
        bool cleaned = false;
        session.AddCleanup(*sandbox, [&] { cleaned = true; });
        std::string error;
        CHECK(session.RunChunk(*sandbox, "BIG = {} for i = 1, 200000 do BIG[i] = { i, tostring(i) } end", "=fill", &error));
        const std::size_t filled = session.vm()->MemoryUsed();
        session.Teardown();
        const auto& memory = session.memory();
        CHECK(cleaned);
        CHECK(memory.before == filled && filled > memory.baseline + 8 * 1024 * 1024);
        CHECK(memory.after <= memory.baseline + 16 * 1024);  // 拆除全部 Mod 并 GC 后回到 VM 刚创建时的水平
        CHECK(memory.residual == 0);
        if (g_verbose || round == 0) {
            std::printf("  内存：VM 创建 %zu，加载后 %zu，拆除前 %zu，拆除并 GC 后 %zu，关闭后残留 %zu（字节）\n", memory.baseline,
                memory.loaded, memory.before, memory.after, memory.residual);
        }
        if (round == 0) {
            firstBaseline = memory.baseline;
        } else {
            CHECK(memory.baseline == firstBaseline);
        }
    }
}

// ---------------------------------------------------------------- 沙箱逃逸

struct Case {
    const char* name;
    const char* code;  // 在 sandbox Mod 里执行，必须成功（断言写在代码里）
};

const Case kSandboxCases[] = {
    { "io、package、dofile、loadfile 不存在", "assert(io == nil and package == nil and dofile == nil and loadfile == nil)" },
    { "string.dump 不存在（副本和方法查找都没有）", "assert(string.dump == nil and (''). dump == nil)" },
    { "getmetatable('') 拿不到真实的字符串元表", "assert(getmetatable('') == false)" },
    { "字符串元表不可替换", "assert(not pcall(setmetatable, '', {}))" },
    { "load 拒绝二进制 chunk", "local f, e = load('\\27Lua\\84\\0\\25\\147\\13\\10\\26\\10') assert(f == nil and e:find('binary'))" },
    { "load 的模式参数不能打开二进制", "local f, e = load('\\27LuaT', 'x', 'b') assert(f == nil and e:find('binary'))" },
    { "load 默认环境是调用者的 _ENV", "LOADED_X = nil load('LOADED_X = 5')() assert(LOADED_X == 5) assert(load('return _G')() == _G)" },
    { "load 的 reader 函数", "local parts = { 'return ', '1 + 1' } local i = 0 "
                              "assert(load(function() i = i + 1 return parts[i] end)() == 2)" },
    { "load 显式环境", "local env = {} load('y = 1', 'c', 't', env)() assert(env.y == 1 and y == nil)" },
    { "_G 是自己的环境", "assert(_G == _ENV and rawget(_G, 'dd') == dd)" },
    { "setmetatable 拒绝 __gc", "local ok, e = pcall(setmetatable, {}, { __gc = function() end }) assert(not ok and e:find('__gc'))" },
    { "setmetatable 拒绝值为 false 的 __gc", "assert(not pcall(setmetatable, {}, { __gc = false }))" },
    { "__gc 藏在元表的 __index 里也拒绝不了（原始访问看不到，Lua 也不会登记）",
        "local mt = setmetatable({}, { __index = { __gc = function() end } }) assert(pcall(setmetatable, {}, mt))" },
    { "__metatable 保护照常", "local t = setmetatable({}, { __metatable = 'locked' }) assert(not pcall(setmetatable, t, {}))" },
    { "collectgarbage 只留查询", "assert(not pcall(collectgarbage) and not pcall(collectgarbage, 'collect') and "
                                "not pcall(collectgarbage, 'step') and not pcall(collectgarbage, 'stop') and "
                                "not pcall(collectgarbage, 'generational') and not pcall(collectgarbage, 'incremental') and "
                                "type(collectgarbage('count')) == 'number' and collectgarbage('isrunning') == true)" },
    { "os 只留 time/clock/date/difftime", "assert(os.execute == nil and os.exit == nil and os.getenv == nil and os.remove == nil "
                                          "and os.rename == nil and os.tmpname == nil and os.setlocale == nil and "
                                          "type(os.time()) == 'number' and type(os.clock()) == 'number' and type(os.date()) == 'string')" },
    { "os.date 拒绝越界时间（MSVC 无效参数）", "assert(not pcall(os.date, '%c', -1) and not pcall(os.date, '%c', 1e15) "
                                              "and os.date('!%Y', 0) == '1970')" },
    { "os.date 拒绝非法格式", "assert(not pcall(os.date, '%Ez') and not pcall(os.date, '%'))" },
    { "os.time 的极端值不崩溃", "pcall(os.time, { year = 1000, month = 1, day = 1 }) pcall(os.time, { year = 100000, month = 1, day = 1 }) "
                                "pcall(os.time, { year = -5, month = -100, day = 1e9 })" },
    { "debug 只留 traceback", "assert(debug.getinfo == nil and debug.getlocal == nil and debug.getregistry == nil and "
                              "debug.getupvalue == nil and debug.setupvalue == nil and debug.sethook == nil and "
                              "debug.setmetatable == nil and debug.getmetatable == nil and debug.upvaluejoin == nil and "
                              "type(debug.traceback()) == 'string')" },
    { "require 拒绝 ..、绝对路径、盘符、斜杠", "for _, n in ipairs({ '../x', '..', 'a..b', '.a', 'a.', 'C:/x', 'C:\\\\x', '/abs', "
                                              "'\\\\abs', 'a/b', 'a\\\\b', 'a b', '' }) do "
                                              "local ok, e = pcall(require, n) assert(not ok and e:find('不合法'), n) end" },
    { "require 只在自己的 lua/ 下找", "assert(require('lib.util').name == 'sandbox util') "
                                      "local ok, e = pcall(require, 'main_not_here') assert(not ok and e:find('找不到'))" },
    { "require 找不到别的 Mod 的模块", "assert(not pcall(require, 'pkg'))" },
    { "require 不能加载 C 模块和 manifest", "assert(not pcall(require, 'manifest'))" },
    { "dd.fs 只读、限制在 Mod 目录内", "assert(dd.fs.read('data/info.txt') == 'sandbox data\\n') "
                                     "assert(dd.fs.exists('lua/main.lua') and dd.fs.exists('data') and not dd.fs.exists('nope')) "
                                     "for _, p in ipairs({ '../basic/data/info.txt', '..\\\\basic\\\\lua\\\\main.lua', 'C:/Windows/win.ini', "
                                     "'C:\\\\Windows\\\\win.ini', '/Windows/win.ini', '\\\\\\\\localhost\\\\c$\\\\Windows\\\\win.ini', "
                                     "'data/info.txt:stream', 'data/info.txt::$DATA', 'CON', 'NUL', 'data/../lua/main.lua', "
                                     "'data/info.txt\\0x', 'data/*' }) do "
                                     "local c, e = dd.fs.read(p) assert(c == nil and type(e) == 'string', p) "
                                     "assert(dd.fs.exists(p) == false, p) end "
                                     "assert(dd.fs.read('data') == nil) assert(dd.fs.list('data/info.txt') == nil) "
                                     "assert(dd.fs.list('..') == nil and dd.fs.list('*') == nil and dd.fs.list('C:/') == nil)" },
    { "dd.fs.list", "local l = dd.fs.list() assert(l[1] == 'data/' and l[2] == 'lua/') "
                    "assert(dd.fs.list('data')[1] == 'info.txt')" },
    { "dd 表的基本字段", "assert(dd.id == 'sandbox' and dd.version == '1.0.0' and dd.api == 1)" },
    { "coroutine.running 是主线程，不能 resume", "local co, main = coroutine.running() assert(main and not coroutine.resume(co))" },
    { "pcall/xpcall/error 的普通语义", "assert(select('#', pcall(error)) == 2) local ok, e = pcall(error, { code = 1 }) "
                                     "assert(not ok and e.code == 1) assert(xpcall(function(...) return ... end, print, 1) == true) "
                                     "local ok2, v = xpcall(error, function(m) return 'h:' .. tostring(m) end, 'x') assert(not ok2 and v == 'h:x')" },
    { "协程里 yield 穿过 pcall", "local co = coroutine.wrap(function() local ok, v = pcall(function() return coroutine.yield(1) + 1 end) "
                               "return ok and v end) assert(co() == 1 and co(41) == 42)" },
    { "<close> 变量照常", "local closed = false do local x <close> = setmetatable({}, { __close = function() closed = true end }) end "
                          "assert(closed)" },
};

void TestSandbox()
{
    g_log.clear();
    const auto host = MakeHost();
    ddlua::Session session(host, TestConfig(), "沙箱");
    session.Load(Sources({ "sandbox", "sandbox2", "basic" }));
    auto* sandbox = session.FindMod("sandbox");
    auto* sandbox2 = session.FindMod("sandbox2");
    CHECK(sandbox && sandbox2 && session.LoadedCount() == 3);
    if (!sandbox || !sandbox2) {
        return;
    }
    int passed = 0;
    for (const auto& c : kSandboxCases) {
        std::string error;
        const bool ok = session.RunChunk(*sandbox, c.code, "=case", &error);
        CHECK(ok);
        passed += ok ? 1 : 0;
        std::printf("  %s 沙箱：%s%s%s\n", ok ? "PASS" : "FAIL", c.name, ok ? "" : " —— ", ok ? "" : error.c_str());
    }
    std::printf("  沙箱用例 %d/%zu 通过\n", passed, std::size(kSandboxCases));

    std::string error;
    // Mod 之间：全局、库表、require 缓存互相隔离；方法查找不受 Mod 改库表影响
    CHECK(session.RunChunk(*sandbox, "SHARED = 1 string.upper = nil table.insert = nil math.floor = 'x' "
                                     "assert(('x'):upper() == 'X') coroutine.resume = nil", "=a", &error));
    CHECK(session.RunChunk(*sandbox2, "assert(SHARED == nil and type(string.upper) == 'function' and type(table.insert) == 'function' "
                                      "and type(math.floor) == 'function' and type(coroutine.resume) == 'function')", "=b", &error));
    CHECK(session.RunChunk(*sandbox2, "assert(require('lib.util').name == 'sandbox2 util')", "=c", &error));
    CHECK(session.RunChunk(*sandbox2, "LOADED_X = nil assert(load('return LOADED_X')() == nil)", "=d", &error));
    // 后加的 __gc 不登记终结器
    CHECK(session.RunChunk(*sandbox, "FINALIZED = nil local mt = {} local t = setmetatable({}, mt) "
                                     "mt.__gc = function() FINALIZED = true end t = nil", "=gc1", &error));
    session.vm()->FullGc();
    session.vm()->FullGc();
    CHECK(session.RunChunk(*sandbox, "assert(FINALIZED == nil)", "=gc2", &error));
    // 错误对象不是字符串时不调用 __tostring
    g_log.clear();
    CHECK(!session.RunChunk(*sandbox, "CALLED = nil error(setmetatable({}, { __tostring = function() CALLED = true return 'x' end }))",
        "=ts", &error));
    CHECK(error.find("table") != std::string::npos);
    CHECK(session.RunChunk(*sandbox, "assert(CALLED == nil)", "=ts2", &error));
    // require 的循环和模块出错
    auto* basic = session.FindMod("basic");
    CHECK(session.RunChunk(*basic, "local ok, e = pcall(require, 'lib.cycle_a') assert(not ok and e:find('循环'))", "=r1", &error));
    CHECK(session.RunChunk(*basic, "local ok, e = pcall(require, 'lib.raises') assert(not ok and e:find('module raises')) "
                                   "ok, e = pcall(require, 'lib.raises') assert(not ok and e:find('module raises'))", "=r2", &error));
    // 错误信息带 Mod id 和文件
    CHECK(!session.RunChunk(*basic, "local m = require('lib.raises')", "=r3", &error));
    CHECK(error.find("basic/lib/raises.lua:1: module raises") != std::string::npos);
    // print 写日志，带 Mod id
    g_log.clear();
    CHECK(session.RunChunk(*sandbox2, "print('x', nil, 1.5) dd.log.warn('w') warn('a', 'b') warn('@on')", "=p", &error));
    CHECK(LogHas("[sandbox2] x\tnil\t1.5") && LogHas("[sandbox2] w") && LogHas("[sandbox2] ab") && !LogHas("@on"));
}

// 用 mklink /J 做一个指向 Mod 目录外的联接，dd.fs 必须拒绝
void TestJunctionEscape()
{
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    const std::string root = std::string(temp) + "dd_lua_tests_" + std::to_string(GetCurrentProcessId());
    const std::string mod = root + "\\escape";
    const std::string outside = root + "\\outside";
    CreateDirectoryA(root.c_str(), nullptr);
    CreateDirectoryA(mod.c_str(), nullptr);
    CreateDirectoryA((mod + "\\lua").c_str(), nullptr);
    CreateDirectoryA(outside.c_str(), nullptr);
    const auto write = [](const std::string& path, const char* text) {
        FILE* f = nullptr;
        fopen_s(&f, path.c_str(), "wb");
        if (f) {
            std::fputs(text, f);
            std::fclose(f);
        }
    };
    write(mod + "\\lua\\manifest.json", R"({"id":"escape","version":"1.0.0","api":1})");
    write(mod + "\\lua\\main.lua", "return {}");
    write(outside + "\\secret.txt", "secret");
    const std::string command = "cmd /c mklink /J \"" + mod + "\\link\" \"" + outside + "\" >nul";
    // guest 上偶尔启动 cmd 失败（刚构建的测试进程被杀软扫描时），重试几次
    bool linked = false;
    for (int attempt = 0; attempt < 5 && !linked; ++attempt) {
        const int rc = std::system(command.c_str());
        linked = rc == 0 && (GetFileAttributesA((mod + "\\link").c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
            GetFileAttributesA((mod + "\\link").c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!linked) {
            std::printf("  联接逃逸：mklink 第 %d 次失败（返回 %d），重试\n", attempt + 1, rc);
            Sleep(500);
        }
    }
    CHECK(linked);

    const auto host = MakeHost();
    {
        ddlua::Session session(host, TestConfig(), "联接");
        session.Load({ { "临时 escape", mod } });
        auto* m = session.FindMod("escape");
        CHECK(m != nullptr);
        std::string error;
        if (m && linked) {
            const bool ok = session.RunChunk(*m, "local c, e = dd.fs.read('link/secret.txt') assert(c == nil and e:find('之外'), e) "
                                                 "assert(not dd.fs.exists('link/secret.txt') and not dd.fs.exists('link')) "
                                                 "assert(dd.fs.list('link') == nil)", "=j", &error);
            CHECK(ok);
            std::printf("  %s 沙箱：dd.fs 拒绝联接逃逸%s%s\n", ok ? "PASS" : "FAIL", ok ? "" : " —— ", error.c_str());
        }
    }
    RemoveDirectoryA((mod + "\\link").c_str());
    DeleteFileA((outside + "\\secret.txt").c_str());
    DeleteFileA((mod + "\\lua\\manifest.json").c_str());
    DeleteFileA((mod + "\\lua\\main.lua").c_str());
    RemoveDirectoryA((mod + "\\lua").c_str());
    RemoveDirectoryA(mod.c_str());
    RemoveDirectoryA(outside.c_str());
    RemoveDirectoryA(root.c_str());
}

// ---------------------------------------------------------------- 执行预算

const Case kBudgetCases[] = {
    { "死循环", "while true do end" },
    { "pcall 吞掉预算错误后继续循环", "while true do pcall(function() while true do end end) end" },
    { "pcall(error) 的紧循环", "while true do pcall(error, 'x') end" },
    { "xpcall 吞错", "while true do xpcall(function() while true do end end, function(e) return e end) end" },
    { "xpcall 的消息处理函数死循环", "xpcall(error, function() while true do end end)" },
    { "hook 抛错时 xpcall 的消息处理函数死循环", "xpcall(function() while true do end end, function() while true do end end)" },
    { "被 hook 杀死的协程，wrap 关闭它时 __close 死循环",
        "coroutine.wrap(function() local x <close> = setmetatable({}, { __close = function() while true do end end }) while true do end end)()" },
    { "被 hook 杀死的协程，coroutine.close 时 __close 死循环",
        "local co = coroutine.create(function() local x <close> = setmetatable({}, { __close = function() while true do end end }) "
        "while true do end end) coroutine.resume(co) coroutine.close(co)" },
    { "协程里的死循环（hook 继承）", "coroutine.wrap(function() while true do end end)()" },
    { "coroutine.resume 吞错", "while true do coroutine.resume(coroutine.create(function() while true do end end)) end" },
    { "协程反复 yield", "local co = coroutine.wrap(function() while true do coroutine.yield() end end) while true do co() end" },
    { "coroutine.close 吞 __close 里的错误",
        "while true do local co = coroutine.create(function() local x <close> = setmetatable({}, { __close = function() while true do end end }) "
        "coroutine.yield() end) coroutine.resume(co) coroutine.close(co) end" },
    { "load 的 reader 吞错", "while true do load(function() while true do end end) end" },
    { "require 里的死循环（通过 pcall 吞错）", "while true do pcall(require, 'lib.util') pcall(function() while true do end end) end" },
    { "<close> 里的死循环", "do local x <close> = setmetatable({}, { __close = function() while true do end end }) end" },
    { "错误处理中的 <close> 死循环", "do local x <close> = setmetatable({}, { __close = function() while true do end end }) error('e') end" },
    { "模式匹配回溯（find）", "string.find(string.rep('a', 50000), '.-.-.-.-.-b')" },
    { "模式匹配回溯（match）", "string.match(string.rep('a', 50000), '^(.-)(.-)(.-)(.-)b$')" },
    { "模式匹配回溯（gsub）", "string.gsub(string.rep('a', 50000), '.-.-.-b', '')" },
    { "模式匹配回溯（gmatch）", "for _ in string.gmatch(string.rep('a', 50000), '.-.-.-b') do end" },
    { "%b 平衡匹配的平方复杂度", "string.find(string.rep('(', 300000), '%b()')" },
    { "捕获回引用", "string.find(string.rep('a', 100000), '(a*)%1%1%1b')" },
    { "plain find 的 n*m", "string.find(string.rep('a', 2000000), string.rep('a', 100000) .. 'b', 1, true)" },
    // 审核 A-A1：match() 里 goto init 的逐字推进和 [...] 字符集的扫描原来不计步
    { "模式匹配：长字面串（match，n*m）", "local s = string.rep('a', 30000) string.match(s, s .. 'b')" },
    { "模式匹配：大字符集 [...]（find）", "string.find(string.rep('a', 1e4), '[' .. string.rep('b', 4e5) .. ']')" },
    { "模式匹配：2 MB 的长字面串（审核者修复前 127 秒）", "local s = string.rep('a', 1000000) string.match(s, s .. 'b')" },
    { "模式匹配：长字面串（gsub）", "local s = string.rep('a', 200000) string.gsub(s, s .. 'b', '')" },
    { "模式匹配：长字面串（gmatch）", "local s = string.rep('a', 200000) for _ in string.gmatch(s, s .. 'b') do end" },
    { "模式匹配：单字符类的长链（find）", "local s = string.rep('a', 200000) string.find(s, string.rep('%a', 200000) .. '%d')" },
    { "模式匹配：大字符集加 *（max_expand）", "string.find(string.rep('a', 2e5), '[' .. string.rep('b', 1e5) .. 'a]*c')" },
    { "模式匹配：%f 的大字符集", "string.find(string.rep('a', 1e4), '%f[' .. string.rep('b', 4e5) .. ']')" },
    { "模式匹配：可空项的长链（a* 接受空匹配）", "local p = string.rep('b*', 300000) .. 'c' string.find(string.rep('a', 1e5), p)" },
    // 第二轮审核 R2A-阻断1：gsub 的替换串（add_s）每次匹配扫一遍，'%0'、空捕获不产生输出，内存上限管不住
    { "gsub 替换串：'%0' x 4e6，源 50 字节（审核者测的修复前 921 ms）", "string.gsub(string.rep('x', 50), '', string.rep('%0', 4000000))" },
    { "gsub 替换串：空捕获 '%1' x 4e6，源 50 字节（修复前 1594 ms）", "string.gsub(string.rep('x', 50), '(y*)', string.rep('%1', 4000000))" },
    { "string.format 的大量 %99.99f", "local t = {} for i = 1, 60000 do t[i] = 1e308 end string.format(string.rep('%99.99f', 60000), table.unpack(t))" },
    { "string.rep 的大循环", "while true do local s = string.rep('ab', 4000000, ',') end" },
    { "大字符串拼接", "local s = string.rep('x', 1000000) while true do local t = s .. s end" },
    { "table.move 的超大区间", "table.move({}, 1, math.maxinteger - 1, 2)" },
    { "table.insert 配合假的 __len", "table.insert(setmetatable({}, { __len = function() return math.maxinteger - 1 end }), 1, 0)" },
    { "table.remove 配合假的 __len", "table.remove(setmetatable({}, { __len = function() return math.maxinteger - 1 end }), 1)" },
    { "table.concat 配合假的 __len 和 __index",
        "table.concat(setmetatable({}, { __len = function() return 1e15 end, __index = function() return '' end }))" },
    { "table.unpack 的超大区间", "while true do pcall(table.unpack, {}, 1, 1e6) end" },
    { "table.sort 的比较函数死循环", "local t = {} for i = 1, 1000 do t[i] = i end table.sort(t, function(a, b) while true do end end)" },
    { "table.sort 的大数组", "local t = {} for i = 1, 3000000 do t[i] = (i * 7919) % 1000003 end while true do table.sort(t) end" },
    { "tostring 的 __tostring 死循环", "tostring(setmetatable({}, { __tostring = function() while true do end end }))" },
    { "__index 元方法里的死循环", "local t = setmetatable({}, { __index = function() while true do end end }) local _ = t.a" },
    { "__newindex、__eq、__concat 里的死循环",
        "local mt = { __newindex = function() while true do end end } local t = setmetatable({}, mt) pcall(function() t.x = 1 end) "
        "mt.__eq = mt.__newindex mt.__concat = mt.__newindex pcall(function() return t == setmetatable({}, mt) end) local _ = t .. 'x'" },
};

void TestBudget()
{
    g_log.clear();
    const auto host = MakeHost();
    auto config = TestConfig();
    config.memoryLimitMb = 512;  // 预算用例里有大字符串和大表，内存上限放宽，免得先撞上内存错误
    ddlua::Session session(host, config, "预算");
    session.Load(Sources({ "sandbox", "basic" }));
    auto* sandbox = session.FindMod("sandbox");
    auto* basic = session.FindMod("basic");
    CHECK(sandbox && basic);
    if (!sandbox || !basic) {
        return;
    }
    int passed = 0;
    for (const auto& c : kBudgetCases) {
        std::string error;
        bool budget = false;
        auto* target = std::strstr(c.code, "require") ? basic : sandbox;
        const auto start = std::chrono::steady_clock::now();
        const bool ok = session.RunChunk(*target, c.code, "=budget", &error, &budget);
        const double elapsed = Ms(start);
        // 预算 50 ms，GC 和大块分配会拖长一点（实测都在 55 ms 以内）；没被打断的用例要跑几分钟到几年。
        // 上限 150 ms（第二轮收紧，原来 500）：修复前单次调用就能多跑几十到几百毫秒（string.upper 169 ms、表查找 550 ms），500 ms 会放过它们
        const bool interrupted = !ok && budget && error.find("执行超出预算") != std::string::npos && elapsed < 150;
        std::string after;
        const bool alive = session.RunChunk(*sandbox, "assert(1 + 1 == 2)", "=alive", &after);
        CHECK(interrupted);
        CHECK(alive);
        passed += interrupted && alive ? 1 : 0;
        std::printf("  %s 预算：%s（%.0f ms）%s%s\n", interrupted && alive ? "PASS" : "FAIL", c.name, elapsed,
            interrupted ? "" : " —— ", interrupted ? "" : (ok ? "没有被打断" : error.substr(0, 200).c_str()));
    }
    std::printf("  预算用例 %d/%zu 通过\n", passed, std::size(kBudgetCases));

    std::string error;
    // 单次 C 调用里的长循环：数据先在另一个入口里准备好（准备本身不能超预算），再测这一次调用能不能被打断
    struct Prepared {
        const char* name;
        const char* prep;
        const char* code;
    };
    const Prepared prepared[] = {
        // string.pack/unpack/packsize 的格式串：空白选项不产生输出
        { "string.packsize 的 100 MB 格式串", "DATA = string.rep(string.rep(' ', 4096), 25000)", "string.packsize(DATA)" },
        // 审核 A-B1：load 的 reader 每次返回同一个大串（第一块打开长注释），词法分析不分配内存、不经过 hook
        { "load 的 Lua reader 每次返回 8 MB", "DATA = string.rep(string.rep('x', 1024), 8192)",
            "local first = true load(function() if first then first = false return '--[[' end return DATA end)" },
        { "load 的 C reader（coroutine.wrap）每次返回 8 MB", "DATA = string.rep(string.rep('x', 1024), 8192)",
            "load(coroutine.wrap(function() coroutine.yield('--[[') while true do coroutine.yield(DATA) end end))" },
        { "load 一次给 64 MB 的注释", "DATA = string.rep('--[[' .. string.rep('x', 1020), 65536)", "load(DATA)" },
        // 第二轮审核 R2A-阻断1 的排查：单次调用里和数据量成正比、原来不计步的循环。计数 hook 每 1000 条指令才检查一次，
        // 一条指令里的调用慢到毫秒级，反复调用时两次检查之间就是几百次调用
        { "string.format('%q') 的 28 MB 控制字符", "DATA = string.rep(string.rep('\\1', 4096), 7000)", "string.format('%q', DATA)" },
        { "string.upper 的 100 MB 串", "DATA = string.rep(string.rep('a', 4096), 25000)", "string.upper(DATA)" },
        { "string.reverse 的 100 MB 串", "DATA = string.rep(string.rep('a', 4096), 25000)", "string.reverse(DATA)" },
        { "plain find 在 100 MB 串里找不到，反复调用", "DATA = string.rep(string.rep('a', 4096), 25000)", "while true do string.find(DATA, 'b', 1, true) end" },
        { "string.byte 一次取 99 万个，反复调用", "DATA = string.rep('a', 990000)", "while true do string.byte(DATA, 1, -1) end" },
        { "utf8.len 的 100 MB 串，反复调用", "DATA = string.rep(string.rep('a', 4096), 25000)", "while true do utf8.len(DATA) end" },
        { "utf8.offset 往回跳过 100 MB 的后续字节，反复调用", "DATA = string.rep(string.rep('\\x80', 4096), 25000)", "while true do utf8.offset(DATA, 0, #DATA) end" },
        { "utf8.codes 的迭代函数跳过 100 MB 的后续字节，反复调用", "DATA = string.rep(string.rep('\\x80', 4096), 25000)", "local f = utf8.codes('a') while true do f(DATA, 0) end" },
        { "utf8.codepoint 一次取 99 万个，反复调用", "DATA = string.rep('a', 990000)", "while true do utf8.codepoint(DATA, 1, -1) end" },
        { "table.unpack 一次 99 万个，反复调用", "DATA = { string.byte(string.rep('a', 990000), 1, -1) }", "while true do table.unpack(DATA, 1, 990000) end" },
        { "两个 32 MB 的等值长串反复比较（==）", "DATA = string.rep(string.rep('a', 4096), 8192) DATA2 = string.rep(string.rep('a', 4096), 8192)",
            "while true do local _ = DATA == DATA2 end" },
        { "两个 1 MB 的等值长串反复比较（<）", "DATA = string.rep(string.rep('a', 4096), 256) DATA2 = string.rep(string.rep('a', 4096), 256)",
            "while true do local _ = DATA < DATA2 end" },
        { "以 32 MB 长串为键的表，用等值的另一个串反复查找",
            "DATA = string.rep(string.rep('a', 4096), 8192) DATA2 = string.rep(string.rep('a', 4096), 8192) DATA3 = { [DATA] = 1 }",
            "while true do local _ = DATA3[DATA2] end" },
        { "tonumber 的 32 MB 串，反复调用", "DATA = string.rep(string.rep('a', 4096), 8192)", "while true do tonumber(DATA) end" },
        { "tonumber(s, 36) 的 32 MB 串，反复调用", "DATA = string.rep(string.rep('a', 4096), 8192)", "while true do tonumber(DATA, 36) end" },
        { "next 跳过 99 万个空的数组槽，反复调用", "DATA = { string.byte(string.rep('a', 990000), 1, -1) } for i = 1, 990000 do DATA[i] = nil end",
            "while true do next(DATA) end" },
        { "string.find 的 100 MB 普通模式，反复调用", "DATA = string.rep(string.rep('a', 4096), 25000)", "while true do string.find('a', DATA) end" },
        { "table.sort 的等值长串", "DATA = string.rep(string.rep('a', 4096), 256) DATA2 = {} for i = 1, 2000 do DATA2[i] = DATA end",
            "table.sort(DATA2)" },
    };
    for (const auto& c : prepared) {
        // 准备数据本身可能超过 50 ms（建 100 MB 的串、算 32 MB 长串的哈希），用宽松的预算
        CHECK(session.RunChunk(*sandbox, c.prep, "=prep", &error, nullptr, 5000));
        bool budget = false;
        auto start = std::chrono::steady_clock::now();
        const bool ok = session.RunChunk(*sandbox, c.code, "=prepared", &error, &budget);
        const double elapsed = Ms(start);
        const bool interrupted = !ok && budget && elapsed < 150;
        CHECK(interrupted);
        std::printf("  %s 预算：%s（%.0f ms）%s\n", interrupted ? "PASS" : "FAIL", c.name, elapsed,
            interrupted ? "" : (ok ? " —— 没有被打断" : (" —— " + error.substr(0, 200)).c_str()));
        CHECK(session.RunChunk(*sandbox, "DATA = nil DATA2 = nil DATA3 = nil", "=free", &error, nullptr, 5000));
        session.vm()->FullGc();
    }

    // 不该触发预算的：string.rep 的空结果立即返回（以前会空转 n 次）
    auto start = std::chrono::steady_clock::now();
    CHECK(session.RunChunk(*sandbox, "assert(string.rep('', 1e18) == '' and string.rep('', 1e18, '') == '' and ('').rep('', 1e15) == '')",
        "=rep", &error));
    CHECK(Ms(start) < 100);
    // 正常的工作量不受影响
    CHECK(session.RunChunk(*sandbox, "local n = 0 for i = 1, 100000 do n = n + i end assert(n == 5000050000) "
                                     "assert(select(2, string.gsub(string.rep('ab', 10000), 'b', 'c')) == 10000)", "=normal", &error));
    // 嵌套：预算只在最外层设定，超出后粘滞到最外层结束
    auto* vm = session.vm();
    CHECK(!vm->InEntry() && !vm->BudgetExceeded());
}

// ---------------------------------------------------------------- 内存上限

void TestMemoryLimit()
{
    g_log.clear();
    const auto host = MakeHost();
    auto config = TestConfig();
    config.memoryLimitMb = 16;
    config.budgetMs = 2000;
    ddlua::Session session(host, config, "内存");
    session.Load(Sources({ "sandbox", "sandbox2" }));
    auto* sandbox = session.FindMod("sandbox");
    auto* sandbox2 = session.FindMod("sandbox2");
    CHECK(sandbox && sandbox2);
    if (!sandbox || !sandbox2) {
        return;
    }
    const char* hogs[] = {
        "local t = {} for i = 1, 1e9 do t[i] = string.rep('x', 1000) .. i end",
        "local s = 'x' while true do s = s .. s end",
        "string.rep('x', 1e9)",
        "local t = {} for i = 1, 1e9 do t[i] = i end",
        "local t = {} for i = 1, 1e9 do t[i] = { i } end",
    };
    for (const char* code : hogs) {
        std::string error;
        bool budget = false;
        const bool ok = session.RunChunk(*sandbox, code, "=hog", &error, &budget);
        CHECK(!ok && !budget && error.find("内存超过上限 16 MB") != std::string::npos);
        CHECK(session.vm()->MemoryUsed() <= session.vm()->MemoryLimit());
        std::printf("  %s 内存：%s\n", !ok && !budget ? "PASS" : "FAIL", code);
    }
    // pcall 能接住内存错误，VM 和别的 Mod 照常
    std::string error;
    bool ran = session.RunChunk(*sandbox, "local ok, e = pcall(string.rep, 'x', 1e9) assert(not ok and e:find('memory'))", "=pc", &error);
    CHECK(ran);
    if (!ran) {
        std::printf("    %s\n", error.substr(0, 300).c_str());
    }
    ran = session.RunChunk(*sandbox2, "local t = {} for i = 1, 1000 do t[i] = tostring(i) end assert(#t == 1000)", "=ok", &error);
    CHECK(ran);
    if (!ran) {
        std::printf("    %s\n", error.substr(0, 300).c_str());
    }
    CHECK(session.vm()->AllocFailures() > 0);
    // 一个 Mod 用全局变量占满内存：单 VM 共享上限，别的 Mod 的操作也会报内存错误，但不崩溃
    CHECK(!session.RunChunk(*sandbox, "HOG = {} for i = 1, 1e9 do HOG[i] = { i } end", "=hold", &error));
    const bool starved = !session.RunChunk(*sandbox2, "local t = {} for i = 1, 100000 do t[i] = { i } end", "=starved", &error);
    std::printf("  INFO 内存：全局占满后另一个 Mod 的大分配%s\n", starved ? "报内存错误（预期，单 VM 共享上限）" : "仍然成功");
    // 内存耗尽后拆除照常，VM 关闭后没有残留
    session.Teardown();
    CHECK(session.memory().residual == 0);
}

// ================================================================ P3b：事件绑定、调度、热重载

// ---------------------------------------------------------------- 假 events 服务：直接包装 DDCore 的事件中枢

using ddc::events::FieldDesc;
using ddc::events::Hub;
using ddc::events::Payload;
using ddc::events::Slot;
using ddc::events::Topic;
using ddc::events::TopicKind;
namespace access = ddc::events::access;

constexpr std::uint32_t kOwner = 7;       // DDLua 在假中枢里的 owner
constexpr std::uint32_t kOtherOwner = 99; // 别的 native 插件
const DDPluginHandle kPlugin = reinterpret_cast<DDPluginHandle>(static_cast<std::uintptr_t>(0x1234));
std::unique_ptr<Hub> g_hub;
bool g_crashNext = false;
bool g_crashFind = false;  // 假 game 服务的 actor_find 在 id 666 上触发访问违例（模拟 Lua 回调里的原生异常）

// 像 DDCore 的 GuardedInvoke 一样在 SEH 里调用回调：原生异常展开到这里，返回 false（函数里不能有需要析构的对象）
bool SehInvoke(DDEventFn fn, DDEvent* event, void* user)
{
    __try {
        fn(event, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehCall(void (*fn)(void*), void* arg)
{
    __try {
        fn(arg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

DDEventSubId DD_CALL FakeSubscribe(DDPluginHandle plugin, const char* topic, DDEventFn fn, void* user, const DDEventOptions* options)
{
    std::int32_t priority = 0;
    std::uint32_t scope = DD_EVENT_SCOPE_PLUGIN;
    DDEventEndFn onEnd = nullptr;
    if (options && options->size >= sizeof(DDEventOptions)) {
        priority = options->priority;
        scope = options->scope;
        onEnd = options->on_end;
    }
    std::string reason;
    return g_hub->Subscribe(plugin == kPlugin ? kOwner : kOtherOwner, topic ? topic : "", fn, user, priority, scope, &reason,
        false, onEnd);
}

bool DD_CALL FakeUnsubscribe(DDPluginHandle plugin, DDEventSubId id)
{
    return g_hub->Unsubscribe(plugin == kPlugin ? kOwner : kOtherOwner, id);
}

void FillInfo(const Topic& topic, DDEventTopicInfo* out)
{
    out->name = topic.name.c_str();
    out->kind = static_cast<std::uint32_t>(topic.kind);
    out->field_count = static_cast<std::uint32_t>(topic.fields.size());
    out->writable = topic.writable;
    out->cancellable = topic.cancellable;
    out->available = topic.available;
}

uint32_t DD_CALL FakeTopicCount()
{
    return g_hub->TopicCount();
}

bool DD_CALL FakeTopicAt(uint32_t index, DDEventTopicInfo* out)
{
    const Topic* topic = g_hub->TopicAt(index);
    if (!topic || !out) {
        return false;
    }
    FillInfo(*topic, out);
    return true;
}

bool DD_CALL FakeTopicInfo(const char* name, DDEventTopicInfo* out)
{
    const Topic* topic = name ? g_hub->Find(name) : nullptr;
    if (!topic || !out) {
        return false;
    }
    FillInfo(*topic, out);
    return true;
}

bool DD_CALL FakeTopicAvailable(const char* name, const char** reason)
{
    const Topic* topic = name ? g_hub->Find(name) : nullptr;
    if (!topic) {
        if (reason) {
            *reason = "主题不存在";
        }
        return false;
    }
    if (reason) {
        *reason = topic->available ? "" : topic->unavailableReason.c_str();
    }
    return topic->available;
}

bool DD_CALL FakeFieldAt(const char* name, uint32_t index, DDEventFieldInfo* out)
{
    const Topic* topic = name ? g_hub->Find(name) : nullptr;
    if (!topic || !out || index >= topic->fields.size()) {
        return false;
    }
    out->name = topic->fields[index].name.c_str();
    out->type = topic->fields[index].type;
    out->access = topic->fields[index].access;
    return true;
}

uint32_t DD_CALL FakeSubCount(DDPluginHandle plugin)
{
    return plugin == kPlugin ? static_cast<uint32_t>(g_hub->OwnerCount(kOwner)) : 0;
}

const DDEventsService kFakeEvents = { { sizeof(DDEventsService), DD_SERVICE_EVENTS_VERSION }, &FakeSubscribe, &FakeUnsubscribe,
    &FakeTopicCount, &FakeTopicAt, &FakeTopicInfo, &FakeTopicAvailable, &FakeFieldAt, nullptr, nullptr, &access::GetInt,
    &access::GetFloat, &access::GetBool, &access::GetString, &access::GetPointer, &access::SetInt, &access::SetFloat,
    &access::SetBool, &access::SetString, &access::SetPointer, &access::Cancel, &access::Flags, &access::TopicName,
    &access::OriginThread, &FakeSubCount };

// 每个测试一个新中枢，主题仿照 DDCore 的真实定义（system.cpp）
void NewHub()
{
    g_crashNext = false;
    g_hub = std::make_unique<Hub>(
        [](DDEventFn fn, DDEvent* event, void* user, std::uint32_t) {
            if (g_crashNext) {
                g_crashNext = false;
                return false;  // 模拟 SEH 捕获到崩溃（回调没有真正执行）
            }
            return SehInvoke(fn, event, user);
        },
        [](int, const std::string& message) { g_log.push_back("[hub] " + message); });
}

// 写入的取值范围和 DDCore 一致（system.cpp 的 RangeOf / SemanticRange）：游戏主题的数字字段是 float / int32，应用主题不限
void ApplyGameRanges()
{
    for (Topic* topic : g_hub->Topics()) {
        if (topic->kind == TopicKind::App || topic->kind == TopicKind::Custom) {
            continue;
        }
        for (auto& field : topic->fields) {
            field.range = field.type == DD_FIELD_FLOAT ? ddc::events::FieldRange::F32
                : field.type == DD_FIELD_INT           ? ddc::events::FieldRange::I32
                                                       : ddc::events::FieldRange::Full;
        }
    }
}

void ResetHub()
{
    NewHub();
    constexpr auto R = ddc::events::kAccessR;
    constexpr auto RW = ddc::events::kAccessRW;
    using F = FieldDesc;
    g_hub->AddTopic("damage.pre", TopicKind::Semantic,
        { F{ "target", DD_FIELD_POINTER, R }, F{ "attacker", DD_FIELD_POINTER, R }, F{ "damage", DD_FIELD_FLOAT, RW },
            F{ "damage_type", DD_FIELD_INT, R }, F{ "source_id", DD_FIELD_INT, R }, F{ "crit", DD_FIELD_BOOL, R },
            F{ "hp_before", DD_FIELD_FLOAT, R }, F{ "is_hero", DD_FIELD_BOOL, R } },
        true, true);
    g_hub->AddTopic("damage.post", TopicKind::Semantic,
        { F{ "target", DD_FIELD_POINTER, R }, F{ "damage", DD_FIELD_FLOAT, R }, F{ "hp_after", DD_FIELD_FLOAT, R },
            F{ "cancelled", DD_FIELD_BOOL, R } },
        false, false);
    g_hub->AddTopic("skill.pre", TopicKind::Semantic,
        { F{ "performer", DD_FIELD_POINTER, R }, F{ "skill", DD_FIELD_POINTER, RW }, F{ "target", DD_FIELD_POINTER, RW },
            F{ "target_count", DD_FIELD_INT, R }, F{ "friendly", DD_FIELD_BOOL, R } },
        true, false);
    g_hub->AddTopic("hit.pre", TopicKind::Semantic,
        { F{ "attacker", DD_FIELD_POINTER, R }, F{ "hit", DD_FIELD_BOOL, RW }, F{ "crit", DD_FIELD_BOOL, RW },
            F{ "damage", DD_FIELD_FLOAT, RW } },
        true, false);
    g_hub->AddTopic("app.tick", TopicKind::App, { F{ "frame", DD_FIELD_INT, R } }, false, false);
    // P3c 的场景测试 Mod（tests/lua/scenario/p3c_actor）用到的主题
    g_hub->AddTopic("battle.start", TopicKind::Semantic, {}, false, false);
    g_hub->AddTopic("battle.end", TopicKind::Semantic, {}, false, false);
    g_hub->AddTopic("heal.post", TopicKind::Semantic,
        { F{ "target", DD_FIELD_POINTER, R }, F{ "healer", DD_FIELD_POINTER, R }, F{ "amount", DD_FIELD_FLOAT, R },
            F{ "hp_before", DD_FIELD_FLOAT, R }, F{ "applied", DD_FIELD_FLOAT, R }, F{ "hp_after", DD_FIELD_FLOAT, R } },
        false, false);
    g_hub->AddTopic("stress.pre", TopicKind::Semantic,
        { F{ "target", DD_FIELD_POINTER, R }, F{ "source", DD_FIELD_POINTER, R }, F{ "amount", DD_FIELD_FLOAT, RW } }, true, false);
    g_hub->AddTopic("hero.quirk", TopicKind::Semantic,
        { F{ "hero_id", DD_FIELD_INT, R }, F{ "kind", DD_FIELD_INT, R }, F{ "quirk", DD_FIELD_STRING, R } }, false, false);
    g_hub->AddTopic("query.CampingPointBuffQuery", TopicKind::Query,
        { F{ "name", DD_FIELD_STRING, R }, F{ "hero", DD_FIELD_POINTER, R }, F{ "points", DD_FIELD_INT, RW } }, true, false);
    g_hub->AddTopic("native.RaidStartEvent", TopicKind::Native,
        { F{ "name", DD_FIELD_STRING, R }, F{ "time", DD_FIELD_INT, R }, F{ "raid", DD_FIELD_POINTER, R } }, false, false);
    g_hub->AddTopic("native.Unavailable", TopicKind::Native, {}, false, false);
    g_hub->SetAvailability(g_hub->Find("native.Unavailable"), false, "测试：没有虚表");
    ApplyGameRanges();
}

// 构造一次派发
struct Fire {
    Topic* topic;
    std::vector<Slot> slots;

    explicit Fire(const char* name) : topic(g_hub->Find(name)), slots(topic ? topic->fields.size() : 0) {}

    Slot& At(const char* field)
    {
        std::uint32_t index = 0;
        Hub::FindField(topic, field, &index);
        return slots[index];
    }
    Fire& I(const char* field, std::int64_t value)
    {
        At(field).v.i = value;
        At(field).set = true;
        return *this;
    }
    Fire& Fl(const char* field, double value)
    {
        At(field).v.f = value;
        At(field).set = true;
        return *this;
    }
    Fire& B(const char* field, bool value)
    {
        At(field).v.b = value;
        At(field).set = true;
        return *this;
    }
    Fire& S(const char* field, const char* value)
    {
        At(field).v.s = value;
        At(field).set = true;
        return *this;
    }
    Fire& P(const char* field, void* value)
    {
        At(field).v.p = value;
        At(field).set = true;
        return *this;
    }
    // 主线程派发，.pre 和 query.* 可写（DDCore 在非 PvP 的主线程上就是这样）
    Payload Go()
    {
        Payload payload;
        payload.slots = slots.data();
        payload.originThread = 1;
        payload.allowWrite = topic->writable;
        g_hub->Dispatch(topic, payload);
        return payload;
    }
    // 加载线程上发生：复制后排队，主线程派发（只读、指针清零）
    void Deferred()
    {
        g_hub->EnqueueDeferred(topic, slots.data(), 2);
        g_hub->DrainDeferred();
    }
};

ddlua::Host MakeEventHost()
{
    ddlua::Host host = MakeHost();
    host.plugin = kPlugin;
    host.events = &kFakeEvents;
    return host;
}

bool Lua(ddlua::Session& session, const char* id, const char* code)
{
    ddlua::Mod* mod = session.FindMod(id);
    if (!mod) {
        std::printf("    没有加载 Mod %s\n", id);
        return false;
    }
    std::string error;
    const bool ok = session.RunChunk(*mod, code, "=test", &error);
    if (!ok) {
        std::printf("    Lua 失败（%s）：%s\n", id, error.c_str());
    }
    return ok;
}

std::size_t CoreCount()
{
    return g_hub->OwnerCount(kOwner);
}

void DD_CALL NativeAdd100(DDEvent* event, void*)
{
    double damage = 0;
    access::GetFloat(event, "damage", &damage);
    access::SetFloat(event, "damage", damage + 100);
}

// ---------------------------------------------------------------- 事件绑定

void TestEventBinding()
{
    g_log.clear();
    ResetHub();
    const auto host = MakeEventHost();
    ddlua::Session session(host, TestConfig(), "事件");
    session.Load(Sources({ "ev_a", "ev_b" }));
    CHECK(session.LoadedCount() == 2);

    // 读、写 .pre 字段；dd.subscriptions 和 DDCore 一侧的计数
    CHECK(Lua(session, "ev_a", R"(
        SUB = dd.on("damage.pre", function(ev) SEEN = ev.damage; ev.damage = ev.damage * 2 end)
        assert(SUB and dd.subscriptions() == 1 and SUB:active() and SUB.topic == "damage.pre")
        assert(tostring(SUB):find("damage.pre"))
    )"));
    CHECK(CoreCount() == 1);
    Fire hit("damage.pre");
    hit.Fl("damage", 10).I("damage_type", 2).B("crit", true).P("target", reinterpret_cast<void*>(0x1000));
    hit.Go();
    CHECK(hit.At("damage").v.f == 20.0);
    CHECK(Lua(session, "ev_a", "assert(SEEN == 10)"));
    CHECK(Lua(session, "ev_a", "assert(SUB:off() == true and SUB:off() == false and not SUB:active() and dd.subscriptions() == 0)"));
    CHECK(CoreCount() == 0);

    // 优先级和别的 native 插件交错：ev_b(-5) 先，native(0)，ev_a(10) 后
    std::string reason;
    const auto native = g_hub->Subscribe(kOtherOwner, "damage.pre", &NativeAdd100, nullptr, 0, 0, &reason);
    CHECK(Lua(session, "ev_b", R"(B = dd.on("damage.pre", function(ev) SEEN_B = ev.damage; ev.damage = ev.damage + 1 end, { priority = -5 }))"));
    CHECK(Lua(session, "ev_a", R"(A = dd.on("damage.pre", function(ev) SEEN_A = ev.damage end, { priority = 10 }))"));
    Fire order("damage.pre");
    order.Fl("damage", 10);
    order.Go();
    CHECK(Lua(session, "ev_b", "assert(SEEN_B == 10)") && Lua(session, "ev_a", "assert(SEEN_A == 111)"));
    CHECK(order.At("damage").v.f == 111.0);
    g_hub->Unsubscribe(kOtherOwner, native);
    CHECK(Lua(session, "ev_a", "A:off()") && Lua(session, "ev_b", "dd.off(B)"));

    // 取消：后面的订阅看到 ev.cancelled；DDCore 的负载被标记
    CHECK(Lua(session, "ev_a", R"(
        C1 = dd.on("damage.pre", function(ev) assert(ev.cancelled == false) ev:cancel() end, { priority = 1 })
        C2 = dd.on("damage.pre", function(ev) SAW_CANCEL = ev.cancelled end, { priority = 2 })
    )"));
    Fire cancel("damage.pre");
    const Payload cancelled = cancel.Go();
    CHECK(cancelled.cancelled && (cancelled.flags & DD_EVENT_F_CANCELLED));
    CHECK(Lua(session, "ev_a", "assert(SAW_CANCEL == true) C1:off() C2:off()"));

    // 写入的各种错误、元信息、指针字段在 P3b 不可见
    CHECK(Lua(session, "ev_a", R"(
        RES = {}
        local function try(name, fn, ...) local ok, e = pcall(fn, ...) RES[name] = ok and "没有报错" or e end
        dd.on("damage.pre", function(ev)
            try("只读字段", function() ev.damage_type = 1 end)
            try("类型不符", function() ev.damage = "x" end)
            try("nan", function() ev.damage = 0/0 end)
            try("超出 float", function() ev.damage = 1e39 end)
            try("算出来超出 float", function() ev.damage = ev.damage * 1e38 end)
            RES.after_range = ev.damage  -- 被拒绝的写入不改值
            try("没有的字段", function() ev.nope = 1 end)
            try("读没有的字段", function() return ev.nope end)
            try("元信息", function() ev.topic = 1 end)
            try("指针字段", function() ev.target = 1 end)
            RES.target = ev.target
            RES.topic = ev.topic
            RES.writable = ev.writable
            RES.deferred = ev.deferred
            RES.crit = ev.crit
            RES.type = ev.damage_type
            RES.str = tostring(ev)
            RES.meta = getmetatable(ev)
            ev.damage = 3
        end, { priority = -100 })
        dd.on("skill.pre", function(ev)
            try("skill.pre 写指针", function() ev.target = ev.skill end)
            try("skill.pre 取消", function() ev:cancel() end)
        end)
        dd.on("hit.pre", function(ev)
            try("布尔字段写数字", function() ev.hit = 1 end)
            ev.hit = false
            ev.damage = ev.damage + 0.5
        end)
        dd.on("damage.post", function(ev)
            try("post 写字段", function() ev.damage = 1 end)
            try("post 取消", function() ev:cancel() end)
            RES.post_cancelled = ev.cancelled  -- damage.post 自己有 cancelled 字段，字段优先
        end)
    )"));
    Fire errors("damage.pre");
    errors.Fl("damage", 7).I("damage_type", 4).B("crit", true).P("target", reinterpret_cast<void*>(0x2000));
    errors.Go();
    CHECK(errors.At("damage").v.f == 3.0);
    Fire skill("skill.pre");
    skill.P("skill", reinterpret_cast<void*>(0x3000));
    skill.Go();
    Fire hitPre("hit.pre");
    hitPre.B("hit", true).Fl("damage", 4);
    hitPre.Go();
    CHECK(!hitPre.At("hit").v.b && hitPre.At("damage").v.f == 4.5);
    Fire post("damage.post");
    post.B("cancelled", true);
    post.Go();
    // 内容里有 )"，原始字符串换一个分隔符
    const char* const writeChecks = R"lua(
        assert(RES["只读字段"]:find("只读"), RES["只读字段"])
        assert(RES["类型不符"]:find("数字"), RES["类型不符"])
        assert(RES["nan"]:find("有限"), RES["nan"])
        assert(RES["超出 float"]:find("32 位浮点数") and RES["超出 float"]:find("超出范围"), RES["超出 float"])
        assert(RES["算出来超出 float"]:find("超出范围"), RES["算出来超出 float"])
        assert(RES.after_range == 7, RES.after_range)
        assert(RES["布尔字段写数字"]:find("布尔"), RES["布尔字段写数字"])
        assert(RES["没有的字段"]:find("没有字段 nope"), RES["没有的字段"])
        assert(RES["读没有的字段"]:find("没有字段 nope"), RES["读没有的字段"])
        assert(RES["元信息"]:find("元信息"), RES["元信息"])
        assert(RES["指针字段"]:find("只读"), RES["指针字段"])  -- damage.pre 的 target 是只读的指针
        assert(RES["skill.pre 写指针"]:find("指针"), RES["skill.pre 写指针"])
        assert(RES["skill.pre 取消"]:find("不能取消"), RES["skill.pre 取消"])
        assert(RES["post 写字段"]:find("只读"), RES["post 写字段"])
        assert(RES["post 取消"]:find("不能取消"), RES["post 取消"])
        assert(RES.target == nil and RES.topic == "damage.pre" and RES.writable == true and RES.deferred == false)
        assert(RES.crit == true and RES.type == 4 and math.type(RES.type) == "integer")
        assert(RES.str == "dd.event(damage.pre)" and RES.meta == false)
        assert(RES.post_cancelled == true)
    )lua";
    CHECK(Lua(session, "ev_a", writeChecks));

    // 回调返回后负载失效，任何访问都报错
    CHECK(Lua(session, "ev_b", R"(dd.once("app.tick", function(ev) KEPT = ev; FRAME = ev.frame end))"));
    Fire tick("app.tick");
    tick.I("frame", 42);
    tick.Go();
    CHECK(Lua(session, "ev_b", R"(
        assert(FRAME == 42)
        local ok, e = pcall(function() return KEPT.frame end) assert(not ok and e:find("失效"), e)
        ok, e = pcall(function() return KEPT.topic end) assert(not ok and e:find("失效"))
        ok, e = pcall(function() KEPT.frame = 1 end) assert(not ok and e:find("失效"))
        assert(tostring(KEPT):find("已失效"))
    )"));

    // query.* 可写、不可取消
    CHECK(Lua(session, "ev_b", R"(Q = dd.on("query.CampingPointBuffQuery", function(ev)
        QBIG = {}
        for _, v in ipairs({ 2^31, 3e9, 2^32 + 5, math.maxinteger, -2^31 - 1, 2^63 }) do
            local ok, e = pcall(function() ev.points = v end) QBIG[#QBIG + 1] = ok and "没有报错" or e
        end
        QNAME = ev.name; ev.points = ev.points + 5
        local ok, e = pcall(ev.cancel, ev) QCANCEL = e
        ok, e = pcall(function() ev.points = 1.5 end) QFRAC = e end))"));
    Fire query("query.CampingPointBuffQuery");
    query.S("name", "CampingPointBuffQuery").I("points", 12);
    query.Go();
    CHECK(query.At("points").v.i == 17);
    CHECK(Lua(session, "ev_b", "assert(QNAME == 'CampingPointBuffQuery' and QCANCEL:find('不能取消') and QFRAC:find('整数'), QFRAC) Q:off()"));
    // 超出 int32 的整数（含值为整数的浮点、超出 int64 的浮点）由 DDCore 拒绝，Lua 报「超出范围」，原值不变
    CHECK(Lua(session, "ev_b", R"(assert(#QBIG == 6) for i, e in ipairs(QBIG) do assert(e:find("32 位整数") and e:find("超出范围"), i .. ": " .. e) end)"));

    // 延迟事件：指针字段为 nil，只读，不能取消
    CHECK(Lua(session, "ev_b", R"(D = dd.on("native.RaidStartEvent", function(ev)
        DEF = { deferred = ev.deferred, raid = ev.raid, name = ev.name, writable = ev.writable }
        local ok, e = pcall(ev.cancel, ev) DEF.cancel = e end)
        DQ = dd.on("query.CampingPointBuffQuery", function(ev) local ok, e = pcall(function() ev.points = 1 end) DEFQ = e end))"));
    Fire raid("native.RaidStartEvent");
    raid.S("name", "RaidStartEvent").P("raid", reinterpret_cast<void*>(0x4000));
    raid.Deferred();
    Fire deferredQuery("query.CampingPointBuffQuery");
    deferredQuery.I("points", 1);
    deferredQuery.Deferred();
    CHECK(Lua(session, "ev_b", R"(
        assert(DEF.deferred == true and DEF.raid == nil and DEF.name == "RaidStartEvent" and DEF.writable == false)
        assert(DEF.cancel:find("延迟事件"), DEF.cancel)
        assert(DEFQ:find("延迟事件"), DEFQ)
        D:off() DQ:off()
    )"));

    // filter：在 C++ 里比较，不匹配的不进入 Lua
    CHECK(Lua(session, "ev_a", R"(
        HITS = 0
        F = dd.on("hero.quirk", function(ev) HITS = HITS + 1 LASTQ = ev.quirk end, { filter = { hero_id = 3, quirk = "brave" } })
        local function bad(opts) local ok, e = pcall(dd.on, "hero.quirk", function() end, opts) assert(not ok) return e end
        assert(bad({ filter = { nope = 1 } }):find("没有字段 nope"))
        assert(bad({ filter = { hero_id = "3" } }):find("整数"))
        assert(bad({ filter = { hero_id = 1.5 } }):find("整数"))
        assert(bad({ filter = { [1] = 3 } }):find("字段名"))
        assert(bad({ filter = 3 }):find("必须是表"))
        assert(bad({ nope = 1 }):find("没有选项 nope"))
        assert(bad({ priority = 1.5 }):find("priority"))
        assert(bad({ scope = "forever" }):find("scope"))
        assert(bad({ scope = {} }):find("actor 句柄"))
        local ok, e = pcall(dd.on, "damage.pre", function() end, { filter = { target = 1 } }) assert(not ok and e:find("指针"))
        assert(dd.subscriptions() == 5)  -- F 和前面错误用例的 damage.pre、skill.pre、hit.pre、damage.post
    )"));
    Fire q1("hero.quirk");
    q1.I("hero_id", 3).S("quirk", "timid");
    q1.Go();
    Fire q2("hero.quirk");
    q2.I("hero_id", 4).S("quirk", "brave");
    q2.Go();
    Fire q3("hero.quirk");
    q3.I("hero_id", 3).S("quirk", "brave");
    q3.Go();
    CHECK(Lua(session, "ev_a", "assert(HITS == 1 and LASTQ == 'brave', HITS) F:off()"));

    // once：只调用一次；回调里撤销自己、新增订阅下一次才生效
    CHECK(Lua(session, "ev_b", R"(
        ONCE = 0 SELF = 0 LATE = 0
        O = dd.once("app.tick", function() ONCE = ONCE + 1 end)
        local s
        s = dd.on("app.tick", function() SELF = SELF + 1 s:off() dd.on("app.tick", function() LATE = LATE + 1 end) end)
    )"));
    Fire t1("app.tick");
    t1.Go();
    Fire t2("app.tick");
    t2.Go();
    CHECK(Lua(session, "ev_b", "assert(ONCE == 1 and SELF == 1 and LATE == 1 and not O:active(), LATE)"));

    // 不存在、不可用的主题：返回 nil 和原因
    CHECK(Lua(session, "ev_b", R"(
        local s, e = dd.on("no.such.topic", function() end) assert(s == nil and e:find("主题不存在"))
        s, e = dd.on("native.Unavailable", function() end) assert(s == nil and e:find("不可用") and e:find("没有虚表"))
        assert(not pcall(dd.on, "app.tick") and not pcall(dd.on, {}, print) and not pcall(dd.off, {}))
    )"));

    // 核对：DDLua 记录的和 DDCore 统计的一致
    const auto counts = session.events().CountFor(nullptr);
    CHECK(counts.subscriptions == CoreCount() && ddlua::EventBinding::GlobalSdkCount() == CoreCount());
    session.Teardown();
    CHECK(CoreCount() == 0 && ddlua::EventBinding::GlobalSdkCount() == 0);
    CHECK(session.memory().residual == 0);
    CHECK(LogHas("订阅核对（拆除后）") && LogHas("DDLua 持有 SDK 订阅共 0 个，DDCore 统计 DDLua 的订阅 0 个"));
}

// 连续出错停用、超预算立即停用、一个 Mod 出错不影响别的 Mod
void TestEventFailures()
{
    g_log.clear();
    ResetHub();
    const auto host = MakeEventHost();
    ddlua::Session session(host, TestConfig(), "失败");
    session.Load(Sources({ "ev_a", "ev_b" }));
    CHECK(Lua(session, "ev_a", R"(
        N = 0
        FAIL = dd.on("app.tick", function(ev) N = N + 1 if ev.frame ~= 3 then error("第 " .. N .. " 次出错") end end)
    )"));
    CHECK(Lua(session, "ev_b", R"(OK_TICKS = 0 dd.on("app.tick", function() OK_TICKS = OK_TICKS + 1 end))"));
    // 出错、出错、成功（计数清零）、出错、出错、出错 -> 停用
    for (int frame = 1; frame <= 7; ++frame) {
        Fire tick("app.tick");
        tick.I("frame", frame);
        tick.Go();
    }
    CHECK(Lua(session, "ev_a", "assert(N == 6 and not FAIL:active(), N)"));
    CHECK(Lua(session, "ev_b", "assert(OK_TICKS == 7)"));
    CHECK(LogHas("[ev_a] app.tick 的回调出错（连续第 2 次，3 次后停用）：test:3: 第 2 次出错"));
    CHECK(LogHas("app.tick 的回调出错（连续 3 次），已停用该订阅"));
    CHECK(LogHas("[ev_a] stack traceback:"));
    CHECK(LogHas("已停用该订阅：test:3: 第 6 次出错"));

    // 超预算：第一次就停用，游戏不卡（这里是测试进程不卡），别的 Mod 照常
    CHECK(Lua(session, "ev_a", R"(LOOP = dd.on("app.tick", function() while true do end end, { priority = -1 }))"));
    const auto start = std::chrono::steady_clock::now();
    Fire tick("app.tick");
    tick.I("frame", 100);
    tick.Go();
    const double elapsed = Ms(start);
    CHECK(elapsed < 2000);
    std::printf("  预算：事件回调里的死循环在 %.0f ms 被打断\n", elapsed);
    CHECK(Lua(session, "ev_a", "assert(not LOOP:active())"));
    CHECK(Lua(session, "ev_b", "assert(OK_TICKS == 8)"));
    CHECK(LogHas("app.tick 的回调出错（超出执行预算），已停用该订阅"));
    // pcall 吞不掉回调里的预算错误
    CHECK(Lua(session, "ev_a", R"(SWALLOW = dd.on("app.tick", function() while true do pcall(function() while true do end end) end end))"));
    Fire tick2("app.tick");
    tick2.Go();
    CHECK(Lua(session, "ev_a", "assert(not SWALLOW:active())"));
    CHECK(!session.vm()->InEntry() && !session.vm()->BudgetExceeded());

    // DDCore 因为崩溃撤销订阅（DD_EVENT_END_CRASH）的处理见「VM 损坏（原生异常）」
    session.Teardown();
    CHECK(CoreCount() == 0 && session.memory().residual == 0);
}

// 作用域：DDCore 按作用域撤销时通知 DDLua，Lua 函数引用随之释放
void TestEventScopes()
{
    g_log.clear();
    ResetHub();
    const auto host = MakeEventHost();
    ddlua::Session session(host, TestConfig(), "作用域");
    session.Load(Sources({ "ev_a" }));
    CHECK(Lua(session, "ev_a", R"(
        WEAK = setmetatable({}, { __mode = "v" })
        local f = function() end
        WEAK[1] = f
        SB = dd.on("app.tick", f, { scope = "battle" })
        f = nil
        SR = dd.on("app.tick", function() end, { scope = "raid" })
        SC = dd.on("app.tick", function() end, { scope = "campaign" })
        SM = dd.on("app.tick", function() end, { scope = "mod" })
        assert(dd.subscriptions() == 4)
    )"));
    CHECK(CoreCount() == 4 && g_hub->ScopedCount(DD_EVENT_SCOPE_BATTLE) == 1);
    CHECK(g_hub->EndScope(DD_EVENT_SCOPE_BATTLE) == 1);
    session.vm()->FullGc();
    CHECK(Lua(session, "ev_a", "assert(not SB:active() and SR:active() and dd.subscriptions() == 3) assert(WEAK[1] == nil, '函数引用没有释放')"));
    CHECK(g_hub->EndScope(DD_EVENT_SCOPE_RAID) == 1 && g_hub->EndScope(DD_EVENT_SCOPE_CAMPAIGN) == 1);
    CHECK(Lua(session, "ev_a", "assert(not SR:active() and not SC:active() and SM:active() and dd.subscriptions() == 1)"));
    CHECK(CoreCount() == 1 && session.events().CountFor(nullptr).subscriptions == 1);
    CHECK(LogHas("被 DDCore 撤销（作用域结束，scope=battle）"));
    session.Teardown();
    CHECK(CoreCount() == 0);
}

// ---------------------------------------------------------------- 调度器和协程

double g_now = 0;

void Tick(ddlua::Session& session, double now)
{
    g_now = now;
    session.Tick();
}

void TestScheduler()
{
    g_log.clear();
    ResetHub();
    const auto host = MakeEventHost();
    ddlua::Session session(host, TestConfig(), "调度");
    g_now = 100;
    session.SetClock([] { return g_now; });
    session.Load(Sources({ "ev_a", "ev_b" }));

    // 定时器：after 一次，every 重复（回调收到句柄），every(0) 每帧但不在创建的那一帧
    CHECK(Lua(session, "ev_a", R"(
        AFTER = 0 EVERY = 0 ZERO = 0
        TA = dd.after(1, function(t) AFTER = AFTER + 1 assert(t == TA) end)
        TE = dd.every(0.5, function(t) EVERY = EVERY + 1 if EVERY == 3 then t:cancel() end end)
        TZ = dd.every(0, function() ZERO = ZERO + 1 end)
        assert(dd.time() == 100 and TA:active() and dd.stats().timers == 3)
        assert(not pcall(dd.after, -1, print) and not pcall(dd.after, 0/0, print) and not pcall(dd.every, 1))
    )"));
    Tick(session, 100.4);
    CHECK(Lua(session, "ev_a", "assert(AFTER == 0 and EVERY == 0 and ZERO == 1, ZERO)"));
    Tick(session, 100.5);
    Tick(session, 101.0);
    CHECK(Lua(session, "ev_a", "assert(AFTER == 1 and EVERY == 2 and ZERO == 3 and not TA:active())"));
    Tick(session, 101.6);
    Tick(session, 102.2);
    CHECK(Lua(session, "ev_a", "assert(EVERY == 3 and not TE:active() and ZERO == 5) TZ:cancel() assert(dd.stats().timers == 0)"));

    // spawn 立即运行到第一次挂起；wait 跟随时钟；coroutine.yield 等一帧；参数传进去
    CHECK(Lua(session, "ev_a", R"(
        STEP = 0
        T1 = dd.spawn(function(a, b) STEP = a + b dd.wait(1) STEP = 10 coroutine.yield() STEP = 11 dd.wait(0) STEP = 12 end, 2, 3)
        assert(STEP == 5 and T1:status() == "waiting" and dd.stats().tasks == 1)
        T2 = dd.spawn(function() DONE = true end)
        assert(DONE and T2:status() == "dead" and not T2:active())
    )"));
    Tick(session, 102.5);
    CHECK(Lua(session, "ev_a", "assert(STEP == 5)"));
    Tick(session, 103.3);
    CHECK(Lua(session, "ev_a", "assert(STEP == 10)"));
    Tick(session, 103.31);
    CHECK(Lua(session, "ev_a", "assert(STEP == 11)"));
    Tick(session, 103.32);
    CHECK(Lua(session, "ev_a", "assert(STEP == 12 and T1:status() == 'dead' and dd.stats().tasks == 0)"));

    // 协程不跨越 hook 边界：事件回调、定时器回调、main.lua（这里是 RunChunk）里不能 wait；
    // 用户自己的协程里也不能；pcall 里可以
    CHECK(Lua(session, "ev_a", R"(
        local ok, e = pcall(dd.wait, 1) assert(not ok and e:find("只能在 dd.spawn"), e)
        dd.once("app.tick", function() local ok, e = pcall(dd.wait, 1) CB_WAIT = e local ok2, e2 = pcall(dd.wait_event, "app.tick") CB_WAIT2 = e2 end)
        dd.after(0, function() local ok, e = pcall(dd.wait, 1) TIMER_WAIT = e end)
        dd.spawn(function()
            local co = coroutine.wrap(function() local ok, e = pcall(dd.wait, 1) return e end)
            NESTED = co()
            local ok = pcall(dd.wait, 0.1) PCALL_WAIT = ok
        end)
    )"));
    Fire tick("app.tick");
    tick.Go();
    Tick(session, 103.4);
    Tick(session, 103.6);
    CHECK(Lua(session, "ev_a", R"(
        assert(CB_WAIT:find("只能在 dd.spawn") and CB_WAIT2:find("只能在 dd.spawn"), CB_WAIT)
        assert(TIMER_WAIT:find("只能在 dd.spawn"), TIMER_WAIT)
        assert(NESTED:find("coroutine.create"), NESTED)
        assert(PCALL_WAIT == true)
    )"));

    // wait_event：快照在下一帧交给协程；filter；.pre 的快照是最终值；超时返回 nil；主题不存在直接返回
    CHECK(Lua(session, "ev_b", R"(
        dd.spawn(function()
            local ev = dd.wait_event("hero.quirk", { hero_id = 5 })
            QUIRK = ev
            local after = pcall(function() return ev.quirk end)
            local dmg = dd.wait_event("damage.pre")
            DMG = dmg.damage
            local none = dd.wait_event("app.tick", nil, 2)
            TIMEOUT = (none == nil)
            local r, e = dd.wait_event("no.such.topic")
            MISSING = e
            RAID = dd.wait_event("native.RaidStartEvent")
        end)
        DOUBLE = dd.on("damage.pre", function(ev) ev.damage = ev.damage * 2 end)
        assert(dd.stats().waiters == 1)
    )"));
    Fire w1("hero.quirk");
    w1.I("hero_id", 4).S("quirk", "x");
    w1.Go();
    Fire w2("hero.quirk");
    w2.I("hero_id", 5).S("quirk", "brave");
    w2.Go();
    CHECK(Lua(session, "ev_b", "assert(QUIRK == nil and dd.stats().waiters == 0)"));  // 匹配后立即撤销，下一帧才恢复
    Tick(session, 104.0);
    CHECK(Lua(session, "ev_b", R"(
        assert(QUIRK.hero_id == 5 and QUIRK.quirk == "brave" and QUIRK.topic == "hero.quirk" and QUIRK.deferred == false)
        assert(dd.stats().waiters == 1)
    )"));
    Fire w3("damage.pre");
    w3.Fl("damage", 10);
    w3.Go();
    Tick(session, 104.1);
    CHECK(Lua(session, "ev_b", "assert(DMG == 20, DMG) assert(dd.stats().waiters == 1)"));
    Tick(session, 105.0);
    CHECK(Lua(session, "ev_b", "assert(TIMEOUT == nil)"));
    Tick(session, 106.2);
    CHECK(Lua(session, "ev_b", "assert(TIMEOUT == true and MISSING:find('主题不存在') and dd.stats().waiters == 1)"));
    Fire raid("native.RaidStartEvent");
    raid.S("name", "RaidStartEvent").P("raid", reinterpret_cast<void*>(0x5000));
    raid.Deferred();
    Tick(session, 106.3);
    CHECK(Lua(session, "ev_b", "assert(RAID.deferred == true and RAID.raid == nil and RAID.name == 'RaidStartEvent') DOUBLE:off()"));
    CHECK(Lua(session, "ev_b", "assert(dd.stats().tasks == 0 and dd.stats().waiters == 0)"));

    // 回调里 spawn：立即运行，之后由帧恢复；保存的负载在协程里也已失效
    CHECK(Lua(session, "ev_a", R"(dd.once("app.tick", function(ev)
        dd.spawn(function() SPAWNED = ev.frame dd.wait(0) local ok, e = pcall(function() return ev.frame end) STALE = e end)
    end))"));
    Fire t5("app.tick");
    t5.I("frame", 5);
    t5.Go();
    CHECK(Lua(session, "ev_a", "assert(SPAWNED == 5 and STALE == nil)"));
    Tick(session, 106.4);
    CHECK(Lua(session, "ev_a", "assert(STALE:find('失效'), STALE)"));

    // 取消：挂起的协程立即关闭（<close> 照常执行）；协程取消自己时在下一次挂起时关闭
    CHECK(Lua(session, "ev_a", R"(
        CLOSED = 0
        local t = dd.spawn(function() local x <close> = setmetatable({}, { __close = function() CLOSED = CLOSED + 1 end }) dd.wait(100) end)
        assert(t:cancel() == true and CLOSED == 1 and t:status() == "dead" and t:cancel() == false)
        local self
        self = dd.spawn(function() local x <close> = setmetatable({}, { __close = function() CLOSED = CLOSED + 1 end })
            dd.wait(0) self:cancel() SELF_AFTER = true dd.wait(0) NOT_REACHED = true end)
    )"));
    Tick(session, 106.5);
    CHECK(Lua(session, "ev_a", "assert(SELF_AFTER and CLOSED == 2 and NOT_REACHED == nil and dd.stats().tasks == 0)"));

    // Mod 自己用 coroutine.running() 拿到线程并恢复它：调度器发现后放手，不崩溃
    CHECK(Lua(session, "ev_a", R"(
        dd.spawn(function() CO = coroutine.running() dd.wait(100) MANUAL = true end)
        assert(coroutine.resume(CO) and MANUAL)
    )"));
    Tick(session, 300);
    CHECK(Lua(session, "ev_a", "assert(dd.stats().tasks == 0)"));
    CHECK(LogHas("被 Mod 自己恢复到结束或关闭了"));

    // 错误和预算：协程出错写日志带 traceback；死循环被打断，<close> 里的死循环也被打断；定时器超预算立即停用
    CHECK(Lua(session, "ev_b", R"(
        dd.spawn(function() dd.wait(0) error("协程里的错误") end)
        dd.spawn(function() dd.wait(0) while true do end end)
        dd.spawn(function() local x <close> = setmetatable({}, { __close = function() while true do end end }) dd.wait(0) while true do end end)
        LOOPT = dd.every(0, function() while true do end end)
        ERRT = dd.every(0, function() error("定时器的错误") end)
    )"));
    const auto start = std::chrono::steady_clock::now();
    Tick(session, 301);
    const double elapsed = Ms(start);
    std::printf("  预算：一帧里 1 个定时器死循环 + 2 个协程死循环（其一的 <close> 也死循环）共 %.0f ms\n", elapsed);
    CHECK(elapsed < 5000);
    CHECK(LogHas("协程出错，已终止：test:2: 协程里的错误"));
    CHECK(LogHas("协程超出执行预算，已终止"));
    CHECK(LogHas("dd.every 的回调出错（超出执行预算），已停用该定时器"));
    Tick(session, 302);
    Tick(session, 303);
    CHECK(Lua(session, "ev_b", "assert(not LOOPT:active() and not ERRT:active() and dd.stats().tasks == 0)"));
    CHECK(LogHas("dd.every 的回调出错（连续 3 次），已停用该定时器"));
    CHECK(!session.vm()->InEntry() && !session.vm()->BudgetExceeded());

    // spawn 无限递归：每层都在 C 栈上嵌套一次 lua_resume，到 Lua 的 C 调用上限（LUAI_MAXCCALLS）报错，不会撑爆 C 栈
    CHECK(Lua(session, "ev_a", R"(
        DEPTH = 0
        local function nest() DEPTH = DEPTH + 1 dd.spawn(nest) end
        dd.spawn(nest)
        print("spawn 递归深度 " .. DEPTH)
        assert(DEPTH > 20 and DEPTH < 1000, DEPTH)
        assert(dd.stats().tasks == 0)
    )"));
    CHECK(LogHas("C stack overflow"));

    // spawn 在 main.lua / RunChunk 里超预算：预算错误传给调用者
    std::string error;
    bool budget = false;
    CHECK(!session.RunChunk(*session.FindMod("ev_a"), "dd.spawn(function() while true do end end)", "=spawnloop", &error, &budget));
    CHECK(budget);

    // 拆除时协程关闭，<close> 照常执行（含死循环的被预算打断），定时器撤销
    CHECK(Lua(session, "ev_a", R"(
        dd.spawn(function() local x <close> = setmetatable({}, { __close = function() print("拆除时关闭了协程") end }) dd.wait(1000) end)
        dd.spawn(function() local x <close> = setmetatable({}, { __close = function() while true do end end }) dd.wait(1000) end)
        dd.spawn(function() local x <close> = setmetatable({}, { __close = function() local ok, e = pcall(dd.on, "app.tick", print) print("拆除中注册：" .. tostring(e)) end }) dd.wait(1000) end)
        dd.every(1, function() end)
        dd.spawn(function() dd.wait_event("app.tick") end)
    )"));
    CHECK(CoreCount() == 1);
    session.Teardown();
    CHECK(LogHas("[ev_a] 拆除时关闭了协程"));
    CHECK(LogHas("关闭协程时 <close> 出错：") && LogHas("正在拆除，不能再注册"));
    CHECK(CoreCount() == 0 && session.memory().residual == 0);
}

// ---------------------------------------------------------------- 热重载和 dd.state

void TestReload()
{
    g_log.clear();
    ResetHub();
    const auto host = MakeEventHost();
    auto config = TestConfig();
    config.devMode = true;
    {
        ddlua::Session session(host, config, "重载");
        g_now = 0;
        session.SetClock([] { return g_now; });
        session.Load(Sources({ "rl_other", "rl_dep", "rl_base" }));
        CHECK(session.LoadedCount() == 3);
        CHECK(LogHas("加载顺序：rl_other rl_base rl_dep"));
        const std::size_t before = CoreCount();
        CHECK(before == 3);
        CHECK(Lua(session, "rl_base", "assert(dd.state.loads == 1 and dd.stats().timers == 1 and dd.stats().tasks == 1 and dd.dev == true)"));
        CHECK(Lua(session, "rl_dep", "assert(BASE_VALUE == 1)"));

        // 直接重载：rl_base 和依赖它的 rl_dep 拆除再执行，rl_other 不受影响
        std::string error;
        CHECK(session.Reload("rl_base", &error));
        CHECK(LogHas("重载 rl_base：拆除 2 个，重新执行 2 个，成功 2 个"));
        CHECK(LogHas("rl_base 的协程被关闭（第 1 次加载的）"));
        CHECK(CoreCount() == before);
        CHECK(Lua(session, "rl_base", R"(
            assert(dd.state.loads == 2 and BASE_TICKS == nil and dd.stats().timers == 1 and dd.stats().tasks == 1 and dd.stats().loads == 2)
            local ok, e = pcall(dd.state.old_on, "app.tick", print) assert(not ok and e:find("已经卸载"), e)
        )"));
        CHECK(Lua(session, "rl_dep", "assert(BASE_VALUE == 2)"));
        CHECK(Lua(session, "rl_other", "assert(OTHER_LOADED == 1 and dd.state.loads == nil)"));
        CHECK(LogHas("订阅核对（重载 rl_base 之后）：本会话 dd.on 3 个") && LogHas("DDCore 统计 DDLua 的订阅 3 个"));

        // dd.reload 从回调里请求：排到下一帧
        CHECK(Lua(session, "rl_other", R"(dd.once("app.tick", function() assert(dd.reload("rl_base") == true) end))"));
        Fire tick("app.tick");
        tick.Go();
        CHECK(Lua(session, "rl_base", "assert(dd.state.loads == 2)"));
        Tick(session, 1);
        CHECK(Lua(session, "rl_base", "assert(dd.state.loads == 3)"));
        CHECK(CoreCount() == before);
        // 自己重载自己（默认 id）、未知 id
        CHECK(Lua(session, "rl_dep", R"(assert(dd.reload() == true) local ok, e = dd.reload("nope") assert(ok == false and e:find("nope")))"));
        Tick(session, 2);
        CHECK(Lua(session, "rl_base", "assert(dd.state.loads == 3)"));
        CHECK(LogHas("重载 rl_dep：拆除 1 个，重新执行 1 个，成功 1 个"));
        CHECK(CoreCount() == before);

        // 多次重载后内存不增长（残留会累积）
        const std::size_t memoryBefore = session.vm()->MemoryUsed();
        for (int i = 0; i < 20; ++i) {
            CHECK(session.Reload("rl_base", &error));
        }
        session.vm()->FullGc();
        const std::size_t memoryAfter = session.vm()->MemoryUsed();
        std::printf("  重载 20 次：Lua 内存 %zu -> %zu 字节，SDK 订阅 %zu -> %zu\n", memoryBefore, memoryAfter, before, CoreCount());
        CHECK(memoryAfter < memoryBefore + 8 * 1024);
        CHECK(CoreCount() == before);
        session.Teardown();
        CHECK(CoreCount() == 0 && session.memory().residual == 0);
    }
    {
        // 非开发模式：dd.reload 不可用
        auto normal = TestConfig();
        ddlua::Session session(host, normal, "非开发");
        session.Load(Sources({ "rl_other" }));
        CHECK(Lua(session, "rl_other", R"(local ok, e = dd.reload() assert(ok == false and e:find("开发模式")) assert(dd.dev == false))"));
    }
    {
        // dd.state 在会话结束时丢弃：新会话从头开始
        ddlua::Session session(host, config, "新存档");
        session.Load(Sources({ "rl_base" }));
        CHECK(Lua(session, "rl_base", "assert(dd.state.loads == 1)"));
    }
    CHECK(CoreCount() == 0);
}

// 文件监视：改动 lua/ 下的文件，两轮一致后重载；修好出错的 main.lua 后重载成功
void TestWatch()
{
    g_log.clear();
    ResetHub();
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    const std::string root = std::string(temp) + "dd_lua_watch_" + std::to_string(GetCurrentProcessId());
    const std::string mod = root + "\\watched";
    CreateDirectoryA(root.c_str(), nullptr);
    CreateDirectoryA(mod.c_str(), nullptr);
    CreateDirectoryA((mod + "\\lua").c_str(), nullptr);
    const auto write = [](const std::string& path, const char* text) {
        FILE* f = nullptr;
        fopen_s(&f, path.c_str(), "wb");
        if (f) {
            std::fputs(text, f);
            std::fclose(f);
        }
    };
    write(mod + "\\lua\\manifest.json", R"({"id":"watched","version":"1.0.0","api":1})");
    write(mod + "\\lua\\main.lua", "VERSION = 1 dd.on('app.tick', function() end)");
    const auto host = MakeEventHost();
    auto config = TestConfig();
    config.devMode = true;
    {
        ddlua::Session session(host, config, "监视");
        session.Load({ { "临时 watched", mod } });
        CHECK(Lua(session, "watched", "assert(VERSION == 1)"));
        session.PollWatch();
        session.PollWatch();
        CHECK(!LogHas("文件监视"));
        // 改坏：重载后加载失败，订阅撤销
        write(mod + "\\lua\\main.lua", "VERSION = 2 error('写坏了')");
        session.PollWatch();
        CHECK(!LogHas("文件监视"));  // 第一轮只记下
        session.PollWatch();
        CHECK(LogHas("文件监视：watched 的 lua/ 有变化，重载"));
        CHECK(LogHas("[watched] 加载失败：watched/main.lua:1: 写坏了"));
        CHECK(session.FindMod("watched") == nullptr && CoreCount() == 0);
        // 修好：再次重载成功（新加的文件也算变化）
        write(mod + "\\lua\\main.lua", "VERSION = 3 dd.on('app.tick', function() end) require('extra')");
        write(mod + "\\lua\\extra.lua", "EXTRA = true");
        session.PollWatch();
        session.PollWatch();
        CHECK(Lua(session, "watched", "assert(VERSION == 3 and EXTRA)"));
        CHECK(CoreCount() == 1);
        session.Teardown();
        CHECK(CoreCount() == 0 && session.memory().residual == 0);
    }
    DeleteFileA((mod + "\\lua\\extra.lua").c_str());
    DeleteFileA((mod + "\\lua\\main.lua").c_str());
    DeleteFileA((mod + "\\lua\\manifest.json").c_str());
    RemoveDirectoryA((mod + "\\lua").c_str());
    RemoveDirectoryA(mod.c_str());
    RemoveDirectoryA(root.c_str());
}

// 切换存档：每个会话都有订阅、定时器、协程，拆除后 DDCore 一侧清零，内存回落；没有 Lua Mod 时什么都不订阅
// 每个 Mod 的订阅、定时器、协程数有上限（max_per_mod），防止用大量回调把单次预算放大
void TestPerModLimits()
{
    g_log.clear();
    ResetHub();
    const auto host = MakeEventHost();
    auto config = TestConfig();
    config.maxPerMod = 20;
    ddlua::Session session(host, config, "上限");
    session.Load(Sources({ "ev_a", "ev_b" }));
    CHECK(Lua(session, "ev_a", R"(
        for i = 1, 20 do assert(dd.on("app.tick", function() end)) end
        local ok, e = pcall(dd.on, "app.tick", print) assert(not ok and e:find("上限 20"), e)
        for i = 1, 20 do dd.every(1, print) end
        ok, e = pcall(dd.after, 1, print) assert(not ok and e:find("定时器数达到上限"), e)
        for i = 1, 20 do dd.spawn(function() dd.wait(100) end) end
        ok, e = pcall(dd.spawn, print) assert(not ok and e:find("协程数达到上限"), e)
        assert(dd.stats().subscriptions == 20 and dd.stats().timers == 20 and dd.stats().tasks == 20)
    )"));
    // 上限按 Mod 计算，别的 Mod 不受影响
    CHECK(Lua(session, "ev_b", "assert(dd.on('app.tick', function() end) and dd.after(1, print))"));
    CHECK(CoreCount() == 21);
    session.Teardown();
    CHECK(CoreCount() == 0 && session.memory().residual == 0);
}

void TestSessionSwitch()
{
    g_log.clear();
    ResetHub();
    const auto host = MakeEventHost();
    for (int round = 0; round < 3; ++round) {
        ddlua::Session session(host, TestConfig(), "存档#" + std::to_string(round + 1));
        session.Load(Sources({ "rl_other", "rl_base", "rl_dep" }));
        CHECK(CoreCount() == 3);
        CHECK(Lua(session, "rl_base", "dd.on('damage.pre', function(ev) ev.damage = 0 end, { scope = 'battle' })"));
        session.Teardown();
        CHECK(CoreCount() == 0 && ddlua::EventBinding::GlobalSdkCount() == 0);
        CHECK(session.memory().residual == 0 && session.memory().after <= session.memory().baseline + 16 * 1024);
    }
    {
        ddlua::Session session(host, TestConfig(), "没有 Lua Mod");
        session.Load(Sources({ "not_lua" }));
        CHECK(session.vm() == nullptr && CoreCount() == 0 && g_hub->ExternalSubscriberCount() == 0);
    }
}

// ---------------------------------------------------------------- actor 句柄（P3c）

// 假的游戏世界：名册里的英雄、当前战斗里的怪物，指针是假地址（DDLua 从不解引用，只交给 game 服务）。
// 解析规则和 DDCore 相同：存档序号要相同；英雄按 id 在名册里找；怪物还要探索序号、战斗序号相同，按 guid 在战斗里找
// （guid 每场战斗重新计数，审核 B-B1）
struct FakeActor {
    std::uint32_t id = 0;
    float hp = 0;
    float stress = 42.5f;
};

struct FakeWorld {
    bool active = true;
    std::uint32_t campaign = 1;
    std::uint32_t raid = 1;
    std::uint32_t battle = 1;
    std::map<std::uintptr_t, FakeActor> heroes;
    std::map<std::uintptr_t, FakeActor> monsters;
} g_world;

void* P(std::uintptr_t value)
{
    return reinterpret_cast<void*>(value);
}

std::uintptr_t FindIn(const std::map<std::uintptr_t, FakeActor>& table, std::uint32_t id)
{
    for (const auto& [pointer, actor] : table) {
        if (actor.id == id) {
            return pointer;
        }
    }
    return 0;
}

void FillRef(DDActorRef* out, std::uint32_t kind, std::uint32_t id)
{
    out->size = sizeof(DDActorRef);
    out->kind = kind;
    out->id = id;
    out->campaign_serial = g_world.campaign;
    out->raid_serial = kind == DD_ACTOR_MONSTER ? g_world.raid : 0;
    out->battle_serial = kind == DD_ACTOR_MONSTER ? g_world.battle : 0;
    out->roster = 0;
}

// 启用 Mod 列表（P3d 的场景预演经过 plugin.cpp 读它），默认不可用
struct FakeModEntry {
    std::string name;
    std::string source;
    std::string dir;
    bool found = true;
};
bool g_fakeModsAvailable = false;
std::vector<FakeModEntry> g_fakeMods;

bool DD_CALL FakeModsAvailable() { return g_fakeModsAvailable; }
uint32_t DD_CALL FakeModCount() { return static_cast<uint32_t>(g_fakeMods.size()); }
bool DD_CALL FakeModAt(uint32_t index, DDModEntry* out)
{
    if (index >= g_fakeMods.size() || !out || out->size < sizeof(DDModEntry)) {
        return false;
    }
    const auto& mod = g_fakeMods[index];
    out->name = mod.name.c_str();
    out->source = mod.source.c_str();
    out->dir = mod.found ? mod.dir.c_str() : "";
    out->found = mod.found;
    return true;
}
uint32_t DD_CALL FakeActorKinds() { return (1u << DD_ACTOR_HERO) | (1u << DD_ACTOR_MONSTER); }
uint32_t DD_CALL FakeRaidSerial() { return g_world.raid; }

bool DD_CALL FakeFromPointer(const void* pointer, DDActorRef* out)
{
    const auto p = reinterpret_cast<std::uintptr_t>(pointer);
    if (!g_world.active || !out || out->size < sizeof(DDActorRef)) {
        return false;
    }
    if (const auto it = g_world.heroes.find(p); it != g_world.heroes.end()) {
        FillRef(out, DD_ACTOR_HERO, it->second.id);
        return true;
    }
    if (const auto it = g_world.monsters.find(p); it != g_world.monsters.end()) {
        FillRef(out, DD_ACTOR_MONSTER, it->second.id);
        return true;
    }
    return false;
}

bool DD_CALL FakeFind(uint32_t kind, uint32_t id, DDActorRef* out)
{
    if (g_crashFind && id == 666) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);  // 服务里的缺陷：原生异常穿过 Lua 的栈帧
    }
    if (!g_world.active || !out || out->size < sizeof(DDActorRef)) {
        return false;
    }
    const auto& table = kind == DD_ACTOR_HERO ? g_world.heroes : g_world.monsters;
    if ((kind != DD_ACTOR_HERO && kind != DD_ACTOR_MONSTER) || !FindIn(table, id)) {
        return false;
    }
    FillRef(out, kind, id);
    return true;
}

void* DD_CALL FakeResolve(const DDActorRef* ref)
{
    if (!ref || !g_world.active || ref->campaign_serial != g_world.campaign || ref->roster != 0) {
        return nullptr;
    }
    if (ref->kind == DD_ACTOR_HERO && ref->raid_serial == 0 && ref->battle_serial == 0) {
        return P(FindIn(g_world.heroes, ref->id));
    }
    if (ref->kind == DD_ACTOR_MONSTER && ref->raid_serial == g_world.raid && ref->battle_serial == g_world.battle) {
        return P(FindIn(g_world.monsters, ref->id));
    }
    return nullptr;
}

bool DD_CALL FakeInfo(const DDActorRef* ref, DDActorInfo* out)
{
    const auto p = reinterpret_cast<std::uintptr_t>(FakeResolve(ref));
    if (!p || !out || out->size < sizeof(DDActorInfo)) {
        return false;
    }
    const bool hero = ref->kind == DD_ACTOR_HERO;
    const FakeActor& actor = hero ? g_world.heroes[p] : g_world.monsters[p];
    *out = {};
    out->size = sizeof(DDActorInfo);
    out->kind = ref->kind;
    out->id = ref->id;
    out->hp = actor.hp;
    out->max_hp = 30;
    out->has_max_hp = true;
    out->stress = hero ? actor.stress : 0;
    out->has_stress = hero;
    strcpy_s(out->class_id, hero ? "crusader" : "skeleton_soldier");
    return true;
}

uint32_t DD_CALL FakeBattleSerial() { return g_world.battle; }

const DDGameService kFakeGame = { { sizeof(DDGameService), DD_SERVICE_GAME_VERSION }, &FakeModsAvailable, &FakeModCount, &FakeModAt,
    &FakeActorKinds, &FakeRaidSerial, &FakeFromPointer, &FakeFind, &FakeResolve, &FakeInfo, &FakeBattleSerial };

void ResetWorld()
{
    g_world = {};
    g_world.heroes[0x10000] = { 12, 10 };
    g_world.heroes[0x10100] = { 13, 20 };
    g_world.monsters[0x20000] = { 4500, 7 };
    g_world.monsters[0x20100] = { 4501, 8 };
}

ddlua::Host MakeActorHost()
{
    ddlua::Host host = MakeEventHost();
    host.game = &kFakeGame;
    return host;
}

// 句柄本身：字段、驻留、比较、失效
void TestActorHandles()
{
    g_log.clear();
    ResetHub();
    ResetWorld();
    const auto host = MakeActorHost();
    ddlua::Session session(host, TestConfig(), "actor");
    session.Load(Sources({ "ev_a", "ev_b" }));
    session.SetClock([] { return g_now; });
    g_now = 0;

    CHECK(Lua(session, "ev_a", R"lua(
        H = dd.actor.hero(12)
        assert(dd.actor.is(H) and not dd.actor.is({}) and not dd.actor.is(12))
        assert(H.kind == "hero" and H.id == 12 and H.is_hero and not H.is_monster and H:valid())
        assert(H.hp == 10 and H.max_hp == 30 and H.stress == 42.5 and H.class == "crusader", tostring(H.hp))
        assert(tostring(H) == "dd.actor(hero#12)", tostring(H))
        assert(rawequal(dd.actor.hero(12), H), "同一个英雄应该是同一个 userdata（驻留）")
        assert(dd.actor.hero(13) ~= H and dd.actor.hero(999) == nil)
        M = dd.actor.monster(4500)
        assert(M.kind == "monster" and M.id == 4500 and M.is_monster and M.hp == 7 and M.stress == nil)
        assert(M.class == "skeleton_soldier" and tostring(M):find("monster#4500"))
        assert(dd.actor.monster(12) == nil and dd.actor.hero(4500) == nil)
        local keys = { [H] = "h", [M] = "m" }
        assert(keys[dd.actor.hero(12)] == "h" and keys[dd.actor.monster(4500)] == "m", "句柄可以当表的键")
        local function err(f) local ok, e = pcall(f) assert(not ok) return e end
        assert(err(function() H.hp = 1 end):find("只读"))
        assert(err(function() return H.nope end):find("没有字段 nope"))
        assert(err(function() return dd.actor.hero(-1) end):find("非负整数"))
        assert(err(function() return dd.actor.hero("12") end):find("非负整数"))
        assert(getmetatable(H) == false)
    )lua"));

    // 跨回调保存：在回调里拿到的句柄，回调返回后仍然可用
    CHECK(Lua(session, "ev_b", R"lua(
        SAVED = {}
        dd.on("damage.pre", function(ev) SAVED.target = ev.target SAVED.attacker = ev.attacker SAVED.ev = ev end)
    )lua"));
    Fire d1("damage.pre");
    d1.P("target", P(0x10000)).P("attacker", P(0x20100)).Fl("damage", 3);
    d1.Go();
    CHECK(Lua(session, "ev_b", R"lua(
        local h = dd.actor.hero(12)
        assert(rawequal(SAVED.target, h) and SAVED.target == h, "事件里的英雄就是 dd.actor.hero 的那个句柄")
        assert(SAVED.attacker.kind == "monster" and SAVED.attacker.id == 4501 and SAVED.attacker.hp == 8)
        assert(not pcall(function() return SAVED.ev.target end), "负载在回调返回后失效，句柄不受影响")
    )lua"));
    // 不是 actor 的指针：nil
    Fire d2("damage.pre");
    d2.P("target", P(0x99999)).Fl("damage", 1);
    d2.Go();
    CHECK(Lua(session, "ev_b", "assert(SAVED.target == nil and SAVED.attacker == nil)"));

    // 跨协程保存：协程挂起前拿到的句柄，恢复后照常使用（这里恢复时战斗已经结束）
    CHECK(Lua(session, "ev_a", R"lua(
        dd.spawn(function() local m = M CO_BEFORE = m.hp coroutine.yield() CO_HP = m.hp CO_KIND = m.kind CO_DONE = true end)
        assert(CO_BEFORE == 7)
    )lua"));

    // 战斗结束（怪物表清空）：怪物句柄失效，字段读出 nil；英雄不受影响
    g_world.monsters.clear();
    CHECK(Lua(session, "ev_a", R"lua(
        assert(not M:valid() and M.hp == nil and M.max_hp == nil and M.class == nil)
        assert(M.kind == "monster" and M.id == 4500, "标识字段总能读")
        assert(tostring(M):find("已失效"), tostring(M))
        assert(H:valid() and H.hp == 10)
    )lua"));
    // 同一次探索的下一场战斗（审核 B-B1）：游戏每场战斗重新给怪物编 guid，同一个 guid 又出现了。
    // 旧句柄失效，不能指到新的怪物；新句柄是另一个 userdata，== 为假，当表的键也分开
    g_world.battle = 2;
    g_world.monsters[0x28000] = { 4500, 9 };
    CHECK(Lua(session, "ev_a", R"lua(
        assert(not M:valid() and M.hp == nil, "上一场战斗的旧句柄不能指到这一场同 guid 的怪物")
        local m2 = dd.actor.monster(4500)
        assert(m2 and m2:valid() and m2.hp == 9 and m2 ~= M and not rawequal(m2, M), "同 guid 的新怪物是另一个句柄")
        local keys = { [M] = "旧" }
        assert(keys[m2] == nil, "表的键区分两场战斗的同 guid 怪物")
        keys[m2] = "新"
        assert(keys[M] == "旧" and keys[dd.actor.monster(4500)] == "新")
        assert(tostring(M):find("战斗 1") and tostring(m2):find("战斗 2"), tostring(m2))
        assert(H:valid() and H.hp == 10, "英雄句柄跨战斗有效")
        M2 = m2
    )lua"));
    g_world.monsters.clear();
    // 下一次探索：同一个 guid 又出现了，旧句柄仍然失效，新句柄是另一个
    g_world.raid = 2;
    g_world.battle = 3;
    g_world.monsters[0x30000] = { 4500, 5 };
    CHECK(Lua(session, "ev_a", R"lua(
        assert(not M:valid() and M.hp == nil and not M2:valid(), "跨探索的旧句柄不能指到新的怪物")
        local m2 = dd.actor.monster(4500)
        assert(m2 and m2:valid() and m2.hp == 5 and m2 ~= M and not rawequal(m2, M) and m2 ~= M2)
        assert(tostring(m2):find("探索 2"))
    )lua"));
    // 英雄离开名册
    g_world.heroes.erase(0x10100);
    CHECK(Lua(session, "ev_a", "assert(dd.actor.hero(13) == nil)"));
    // 换存档：同一个 hero id 还在，旧句柄失效
    g_world.campaign = 2;
    CHECK(Lua(session, "ev_a", R"lua(
        assert(not H:valid() and H.hp == nil, "跨存档的旧句柄失效")
        local h2 = dd.actor.hero(12)
        assert(h2 and h2:valid() and h2 ~= H)
    )lua"));
    // 存档外：什么都解析不了
    g_world.active = false;
    CHECK(Lua(session, "ev_a", "assert(dd.actor.hero(12) == nil and dd.actor.monster(4500) == nil)"));
    g_world.active = true;
    Tick(session, 1.0);
    Tick(session, 1.1);
    CHECK(Lua(session, "ev_a", "assert(CO_DONE and CO_HP == nil and CO_KIND == 'monster', '协程里保存的怪物句柄在战斗结束后失效')"));

    // 没有 actor 函数的 game 服务（或没有 game 服务）：句柄函数返回 nil，事件指针字段是 nil
    session.Teardown();
    CHECK(CoreCount() == 0 && session.memory().residual == 0);
    ddlua::Host bare = MakeEventHost();
    ddlua::Session plain(bare, TestConfig(), "没有 game");
    plain.Load(Sources({ "ev_a" }));
    CHECK(Lua(plain, "ev_a", "assert(dd.actor.hero(12) == nil) dd.on('damage.pre', function(ev) BARE = ev.target == nil end)"));
    Fire d3("damage.pre");
    d3.P("target", P(0x10000)).Fl("damage", 1);
    d3.Go();
    CHECK(Lua(plain, "ev_a", "assert(BARE == true)"));
    plain.Teardown();
}

// 事件里的句柄：filter、actor 作用域、wait_event 快照、延迟事件
void TestActorEvents()
{
    g_log.clear();
    ResetHub();
    ResetWorld();
    const auto host = MakeActorHost();
    ddlua::Session session(host, TestConfig(), "actor 事件");
    session.Load(Sources({ "ev_a", "ev_b" }));
    session.SetClock([] { return g_now; });
    g_now = 10;

    // filter 按句柄：只有 target 是 hero#12 的进入 Lua
    CHECK(Lua(session, "ev_a", R"lua(
        H = dd.actor.hero(12)
        M = dd.actor.monster(4500)
        HITS = 0
        F = dd.on("damage.pre", function(ev) HITS = HITS + 1 end, { filter = { target = H } })
        local ok, e = pcall(dd.on, "damage.pre", function() end, { filter = { target = 1 } })
        assert(not ok and e:find("actor 句柄"), e)
        ok, e = pcall(dd.on, "damage.pre", function() end, { filter = { target = {} } })
        assert(not ok and e:find("actor 句柄"), e)
    )lua"));
    for (std::uintptr_t target : { 0x10000u, 0x10100u, 0x20000u, 0x10000u, 0u }) {
        Fire f("damage.pre");
        f.P("target", P(target)).Fl("damage", 1);
        f.Go();
    }
    CHECK(Lua(session, "ev_a", "assert(HITS == 2, HITS) F:off()"));

    // actor 作用域：怪物 → SDK battle 作用域；英雄 → campaign 作用域
    CHECK(Lua(session, "ev_a", R"lua(
        MT, HT, ET = 0, 0, 0
        SM = dd.on("app.tick", function() MT = MT + 1 end, { scope = M })
        SH = dd.on("app.tick", function() HT = HT + 1 end, { scope = H })
        SE = dd.on("app.tick", function() ET = ET + 1 end, { scope = dd.actor.monster(4501) })
        assert(dd.subscriptions() == 3)
    )lua"));
    CHECK(g_hub->ScopedCount(DD_EVENT_SCOPE_BATTLE) == 2 && g_hub->ScopedCount(DD_EVENT_SCOPE_CAMPAIGN) == 1);
    Fire t1("app.tick");
    t1.Go();
    CHECK(Lua(session, "ev_a", "assert(MT == 1 and HT == 1 and ET == 1)"));
    // 怪物 4500 死掉被移出战斗：下一次派发时发现，撤销，不再调用
    g_world.monsters.erase(0x20000);
    Fire t2("app.tick");
    t2.Go();
    CHECK(Lua(session, "ev_a", "assert(MT == 1 and HT == 2 and not SM:active() and SH:active() and dd.subscriptions() == 2)"));
    CHECK(LogHas("monster#4500（探索 1，战斗 1） 已失效，撤销（派发时发现）"));
    // 英雄离开名册、没有派发：Tick 的定期检查撤销
    g_world.heroes.erase(0x10000);
    Tick(session, 11.5);
    CHECK(Lua(session, "ev_a", "assert(not SH:active() and dd.subscriptions() == 1)"));
    CHECK(LogHas("hero#12 已失效，撤销（定期检查）"));
    // 战斗销毁：DDCore 按 battle 作用域撤销，通知里写 actor
    CHECK(g_hub->EndScope(DD_EVENT_SCOPE_BATTLE) == 1);
    CHECK(Lua(session, "ev_a", "assert(not SE:active() and dd.subscriptions() == 0)"));
    CHECK(LogHas("作用域结束，scope=actor monster#4501"));
    // 已经失效的句柄不能作为 scope
    CHECK(Lua(session, "ev_a", R"lua(
        local s, why = dd.on("app.tick", function() end, { scope = H })
        assert(s == nil and why:find("已经失效"), why)
    )lua"));
    CHECK(CoreCount() == 0);

    // wait_event 的快照：指针字段是句柄，filter 可以用句柄
    g_world.heroes[0x10000] = { 12, 10 };
    g_world.monsters[0x20000] = { 4500, 7 };
    CHECK(Lua(session, "ev_b", R"lua(
        local h = dd.actor.hero(12)
        dd.spawn(function()
            local ev = dd.wait_event("damage.pre", { target = h })
            SNAP = ev
            SNAP_SAME = rawequal(ev.target, dd.actor.hero(12))
        end)
    )lua"));
    Fire w1("damage.pre");
    w1.P("target", P(0x10100)).P("attacker", P(0x20000)).Fl("damage", 1);
    w1.Go();  // target 不是 hero#12（而且 13 号已不在名册），不匹配
    Fire w2("damage.pre");
    w2.P("target", P(0x10000)).P("attacker", P(0x20000)).Fl("damage", 4);
    w2.Go();
    Tick(session, 12.0);
    CHECK(Lua(session, "ev_b", R"lua(
        assert(SNAP and SNAP.damage == 4 and SNAP_SAME, "快照里的 target 是句柄")
        assert(SNAP.attacker.kind == "monster" and SNAP.attacker.id == 4500 and SNAP.attacker.hp == 7)
    )lua"));
    // 按句柄过滤跨战斗（审核 B-B1）：上一场的怪物句柄不匹配下一场同 guid 的怪物（就算对象地址被复用）
    CHECK(Lua(session, "ev_b", R"lua(
        OLD = dd.actor.monster(4500)
        OLD_HITS = 0
        dd.on("damage.pre", function() OLD_HITS = OLD_HITS + 1 end, { filter = { attacker = OLD } })
    )lua"));
    Fire o1("damage.pre");
    o1.P("target", P(0x10000)).P("attacker", P(0x20000)).Fl("damage", 1);
    o1.Go();
    g_world.battle = 2;
    Fire o2("damage.pre");
    o2.P("target", P(0x10000)).P("attacker", P(0x20000)).Fl("damage", 1);
    o2.Go();
    CHECK(Lua(session, "ev_b", R"lua(
        assert(OLD_HITS == 1, OLD_HITS)
        assert(not OLD:valid() and dd.actor.monster(4500) ~= OLD and dd.actor.monster(4500):valid())
    )lua"));
    // 延迟事件：指针字段恒为 nil（就算原来指向英雄）
    CHECK(Lua(session, "ev_b", "dd.on('native.RaidStartEvent', function(ev) DEF_RAID = ev.raid DEF_SEEN = true end)"));
    Fire raid("native.RaidStartEvent");
    raid.S("name", "RaidStartEvent").P("raid", P(0x10000));
    raid.Deferred();
    CHECK(Lua(session, "ev_b", "assert(DEF_SEEN and DEF_RAID == nil)"));
    // 主线程上同一个主题、同一个指针：转成句柄（说明 nil 来自延迟）
    Fire raid2("native.RaidStartEvent");
    raid2.S("name", "RaidStartEvent").P("raid", P(0x10000));
    raid2.Go();
    CHECK(Lua(session, "ev_b", "assert(DEF_RAID and DEF_RAID.id == 12)"));

    session.Teardown();
    CHECK(CoreCount() == 0 && ddlua::EventBinding::GlobalSdkCount() == 0 && session.memory().residual == 0);
}

// 场景测试 Mod 的离线预演：用假世界走一遍两场战斗，日志里不能有「失败」
void TestActorScenarioMod()
{
    g_log.clear();
    ResetHub();
    ResetWorld();
    const auto host = MakeActorHost();
    ddlua::Session session(host, TestConfig(), "场景预演");
    session.SetClock([] { return g_now; });
    g_now = 0;
    session.Load({ { "场景 p3c_actor", std::string(DD_LUA_FIXTURES) + "/scenario/p3c_actor" } });
    CHECK(session.LoadedCount() == 1);
    // 前两场在同一次探索里，怪物的 guid 相同（游戏每场战斗重新编号，审核 B-B1），第三场在下一次探索
    for (int round = 0; round < 3; ++round) {
        g_world.monsters[0x20000 + round] = { 4500u, 7 };
        ++g_world.battle;
        Fire start("battle.start");
        start.Go();
        Fire pre("damage.pre");
        pre.P("target", P(0x10000)).P("attacker", P(0x20000 + round)).Fl("damage", 3).Fl("hp_before", 10);
        pre.Go();
        Fire post("damage.post");
        post.P("target", P(0x10000)).Fl("damage", 3).Fl("hp_after", 10);
        post.Go();
        Fire heal("heal.post");
        heal.P("target", P(0x10000)).Fl("hp_after", 10);
        heal.Go();
        Tick(session, g_now + 0.1);
        Tick(session, g_now + 0.1);
        g_world.monsters.clear();  // 游戏先清空怪物表，再派发 BattleDestroyedEvent
        Fire end("battle.end");
        end.Go();
        g_hub->EndScope(DD_EVENT_SCOPE_BATTLE);
        Tick(session, g_now + 2.5);
        if (round == 1) {
            g_world.raid = 2;  // 第三场在下一次探索
        }
    }
    std::size_t passed = 0;
    std::size_t failed = 0;
    for (const auto& line : g_log) {
        if (line.find("[p3c_actor] 通过") != std::string::npos) {
            ++passed;
        }
        if (line.find("[p3c_actor] 失败") != std::string::npos || line.find("出错") != std::string::npos) {
            ++failed;
            std::printf("    %s\n", line.c_str());
        }
    }
    std::printf("  场景预演：通过 %zu 行，失败 %zu 行\n", passed, failed);
    CHECK(failed == 0 && passed >= 20);
    CHECK(LogHas("怪物作用域的订阅在战斗结束后已撤销") && LogHas("wait_event 快照的 target 就是保存的英雄句柄"));
    // 第二场（同一次探索、同一个 guid）：上一场的句柄失效，同 guid 的新怪物是另一个句柄
    CHECK(LogHas("[p3c_actor] 通过 第 2 场战斗开始：上一场的怪物句柄 1 个，仍有效 0 个（应为 0）"));
    // 第二轮审核 R2B-应修1：DDLua 一侧的 B-B1 检查逐项计数，带新句柄的探索、战斗序号
    CHECK(LogHas("[p3c_actor] 通过 第 2 场（探索 1，战斗 3）：上一场的怪物里和本场怪物 guid 相同的 1 个；旧句柄失效 1/1，== 不相等 1/1，"
                 "不是同一个对象 1/1，表键分开 1/1，新句柄驻留 1/1"));
    session.Teardown();
    CHECK(CoreCount() == 0 && session.memory().residual == 0);
}

// ================================================================ P3d：模板、示例、LuaLS 声明、场景预演

std::string ProjectFile(const std::string& rel)
{
    return std::string(DD_PROJECT_ROOT) + "/" + rel;
}

std::string ReadText(const std::string& path)
{
    std::string text;
    FILE* file = nullptr;
    if (_wfopen_s(&file, ddlua::Utf8ToWide(path).c_str(), L"rb") != 0 || !file) {
        return text;
    }
    char buffer[65536];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        text.append(buffer, got);
    }
    std::fclose(file);
    return text;
}

std::size_t CountLog(const std::string& needle)
{
    std::size_t n = 0;
    for (const auto& line : g_log) {
        n += line.find(needle) != std::string::npos ? 1 : 0;
    }
    return n;
}

// 主题表换成 sdk/lua/topics.json：topic_dump 在游戏里写出的 DDCore 主题表，和 LuaLS 注解同一个来源
bool ResetHubFromDump()
{
    NewHub();
    const auto json = nlohmann::json::parse(ReadText(ProjectFile("sdk/lua/topics.json")), nullptr, false);
    if (json.is_discarded() || !json.contains("topics")) {
        std::printf("  读不了 sdk/lua/topics.json\n");
        return false;
    }
    for (const auto& t : json["topics"]) {
        std::vector<FieldDesc> fields;
        for (const auto& f : t["fields"]) {
            const std::string type = f["type"].get<std::string>();
            const DDEventFieldType ft = type == "int" ? DD_FIELD_INT
                : type == "float"                     ? DD_FIELD_FLOAT
                : type == "bool"                      ? DD_FIELD_BOOL
                : type == "string"                    ? DD_FIELD_STRING
                                                      : DD_FIELD_POINTER;
            fields.push_back(FieldDesc{ f["name"].get<std::string>(), ft, f["write"].get<bool>() ? ddc::events::kAccessRW : ddc::events::kAccessR });
        }
        const std::string kind = t["kind"].get<std::string>();
        const TopicKind k = kind == "native" ? TopicKind::Native
            : kind == "query"                ? TopicKind::Query
            : kind == "app"                  ? TopicKind::App
            : kind == "custom"               ? TopicKind::Custom
                                             : TopicKind::Semantic;
        Topic* topic = g_hub->AddTopic(t["name"].get<std::string>(), k, std::move(fields), t["writable"].get<bool>(), t["cancellable"].get<bool>());
        if (topic && !t["available"].get<bool>()) {
            g_hub->SetAvailability(topic, false, t["reason"].get<std::string>());
        }
    }
    ApplyGameRanges();
    return g_hub->TopicCount() == json["topics"].size();
}

// Lua Mod 模板（templates/lua）和示例（examples/lua/hello_lua）：加载、事件、协程、定时器、导出都真的跑一遍
void TestTemplateAndExample()
{
    g_log.clear();
    CHECK(ResetHubFromDump());
    ResetWorld();
    const auto host = MakeActorHost();
    ddlua::Session session(host, TestConfig(), "模板和示例");
    session.SetClock([] { return g_now; });
    g_now = 0;
    session.Load({ { "模板", ProjectFile("templates/lua") }, { "示例", ProjectFile("examples/lua/hello_lua") } });
    CHECK(session.LoadedCount() == 2);
    CHECK(LogHas("[my_lua_mod] 已加载，版本 1.0.0"));
    CHECK(LogHas("[hello_lua] 已加载（版本 1.0.0）：Ruin has come to our family."));
    g_world.monsters[0x20000] = { 4500, 7 };
    Fire start("battle.start");
    start.Go();
    Tick(session, 0.1);
    Fire post("damage.post");
    post.P("target", P(0x10000)).P("attacker", P(0x20000)).Fl("damage", 3).Fl("hp_after", 7).Fl("hp_before", 10).B("is_hero", true);
    post.Go();
    CHECK(LogHas("[my_lua_mod] dd.actor(hero#12) 受到 3.0 点伤害，剩余生命 7.0"));
    CHECK(LogHas("[hello_lua] 战斗开始：本次进入存档后的第 1 场"));
    CHECK(LogHas("[hello_lua] 英雄#12（crusader） 受到 3.0 点伤害，生命 10.0/30.0"));
    g_world.monsters.clear();
    Fire end("battle.end");
    end.Go();
    g_hub->EndScope(DD_EVENT_SCOPE_BATTLE);
    Tick(session, 0.2);
    Tick(session, 0.3);
    CHECK(LogHas("[hello_lua] 战斗结束：英雄受到伤害 1 次；本 Mod 现在有订阅 1 个"));
    Tick(session, 3.5);
    CHECK(LogHas("[hello_lua] 进入存档 3 秒了（dd.time() = 3.5）"));
    CHECK(Lua(session, "my_lua_mod", "assert(dd.import('hello_lua').battles() == 1)"));
    CHECK(CountLog("出错") == 0 && CountLog("失败") == 0);
    session.Teardown();
    CHECK(CoreCount() == 0 && session.memory().residual == 0);
}

// LuaLS 声明（sdk/lua/dd.lua、dd_topics.lua、sandbox.lua）和实际 API 对照
struct Declared {
    std::map<std::string, std::vector<std::string>> sets;  // 表名 -> 名字
    void Add(const std::string& set, const std::string& name) { sets[set].push_back(name); }
    std::string ToLua() const
    {
        std::string out = "{";
        for (const auto& [set, names] : sets) {
            out += set + " = {";
            for (const auto& name : names) {
                out += name + " = true, ";
            }
            out += "}, ";
        }
        return out + "}";
    }
};

void ParseDeclarations(const std::string& text, Declared& decl)
{
    std::string cls;  // 正在收集 ---@field 的类
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto word = [](const std::string& s, std::size_t at) {
            std::size_t e = at;
            while (e < s.size() && (std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '_')) {
                ++e;
            }
            return s.substr(at, e - at);
        };
        if (line.rfind("---@class ", 0) == 0) {
            const std::string name = line.substr(10, line.find(' ', 10) == std::string::npos ? std::string::npos : line.find(' ', 10) - 10);
            cls = name == "dd" ? "dd" : name == "dd.Actor" ? "Actor" : name == "dd.Event" ? "Event" : name == "dd.Subscription" ? "Subscription" : "";
            continue;
        }
        if (line.rfind("---@field ", 0) == 0) {
            if (!cls.empty()) {
                decl.Add(cls, word(line, 10));
            }
            continue;
        }
        if (line.rfind("---", 0) == 0) {
            continue;
        }
        cls.clear();
        static const std::pair<const char*, const char*> kFunctions[] = { { "function dd.", "dd" }, { "function loglib.", "log" },
            { "function fslib.", "fs" }, { "function actorlib.", "actor" }, { "function Subscription:", "Subscription" },
            { "function Timer:", "Timer" }, { "function Task:", "Task" }, { "function Actor:", "Actor" }, { "function Event:", "Event" } };
        for (const auto& [prefix, set] : kFunctions) {
            if (line.rfind(prefix, 0) == 0) {
                decl.Add(set, word(line, std::strlen(prefix)));
            }
        }
    }
}

void TestApiDeclarations()
{
    g_log.clear();
    CHECK(ResetHubFromDump());
    ResetWorld();
    // 三个声明文件都是合法的 Lua（LuaLS 按 Lua 解析它们）
    Declared decl;
    for (const char* file : { "sdk/lua/dd.lua", "sdk/lua/dd_topics.lua", "sdk/lua/sandbox.lua" }) {
        const std::string text = ReadText(ProjectFile(file));
        lua_State* L = luaL_newstate();
        const bool ok = !text.empty() && luaL_loadbufferx(L, text.data(), text.size(), file, "t") == LUA_OK;
        if (!ok) {
            std::printf("    %s：%s\n", file, lua_tostring(L, -1) ? lua_tostring(L, -1) : "读不到文件");
        }
        lua_close(L);
        CHECK(ok);
        ParseDeclarations(text, decl);
    }
    // dd_topics.lua 覆盖主题表的每个主题
    const std::string topics = ReadText(ProjectFile("sdk/lua/dd_topics.lua"));
    std::size_t missing = 0;
    for (std::uint32_t i = 0; i < g_hub->TopicCount(); ++i) {
        const std::string name = g_hub->TopicAt(i)->name;
        missing += topics.find("---| \"" + name + "\"") == std::string::npos || topics.find("---@class dd.ev." + name + " ") == std::string::npos;
    }
    std::printf("  主题表 %u 个，dd_topics.lua 缺 %zu 个\n", g_hub->TopicCount(), missing);
    CHECK(missing == 0);
    for (const char* set : { "dd", "log", "fs", "actor", "Subscription", "Timer", "Task", "Actor", "Event" }) {
        std::printf("  声明 %s：%zu 项\n", set, decl.sets[set].size());
        CHECK(!decl.sets[set].empty());
    }

    const auto host = MakeActorHost();
    ddlua::Session session(host, TestConfig(), "声明对照");
    session.SetClock([] { return g_now; });
    g_now = 0;
    session.Load(Sources({ "ev_a" }));
    const std::string code = "DECL = " + decl.ToLua() + R"lua(
        local function same(name, actual, declared)
            for k in pairs(actual) do assert(declared[k], name .. "." .. tostring(k) .. " 没有 LuaLS 声明") end
            for k in pairs(declared) do assert(actual[k] ~= nil, "声明了 " .. name .. "." .. k .. "，实际没有") end
        end
        same("dd", dd, DECL.dd)
        same("dd.log", dd.log, DECL.log)
        same("dd.fs", dd.fs, DECL.fs)
        same("dd.actor", dd.actor, DECL.actor)
        local sub = dd.on("app.tick", function() end)
        for k in pairs(DECL.Subscription) do assert(sub[k] ~= nil, "dd.Subscription." .. k) end
        sub:off()
        local timer = dd.after(100, function() end)
        for k in pairs(DECL.Timer) do assert(timer[k] ~= nil, "dd.Timer." .. k) end
        timer:cancel()
        local task = dd.spawn(function() coroutine.yield() end)
        for k in pairs(DECL.Task) do assert(task[k] ~= nil, "dd.Task." .. k) end
        task:cancel()
        local h = dd.actor.hero(12)
        for k in pairs(DECL.Actor) do assert(pcall(function() return h[k] end), "dd.Actor." .. k) end
        local ok, e = pcall(function() return h.nope end)
        local listed = assert(e:match("可用：(.-)）"), e)
        local n = 0
        for name in listed:gmatch("[^、]+") do n = n + 1 assert(DECL.Actor[name], "句柄的字段 " .. name .. " 没有声明") end
        assert(n >= 9, listed)
        dd.once("damage.pre", function(ev)
            for k in pairs(DECL.Event) do assert(pcall(function() return ev[k] end), "dd.Event." .. k) end
            EV_OK = true
        end)
    )lua";
    CHECK(Lua(session, "ev_a", code.c_str()));
    Fire pre("damage.pre");
    pre.P("target", P(0x10000)).Fl("damage", 1);
    pre.Go();
    CHECK(Lua(session, "ev_a", "assert(EV_OK, '负载的元信息和方法')"));
    session.Teardown();
    CHECK(CoreCount() == 0);
}

// ---------------------------------------------------------------- 场景预演：经过真实的 plugin.cpp

struct FakePhaseSub {
    DDSubscriptionId id;
    DDPhase phase;
    DDPhaseFn fn;
    void* user;
    bool alive;
};
std::vector<FakePhaseSub> g_phaseSubs;
DDSubscriptionId g_nextPhaseId = 1;
std::uint64_t g_frame = 1;

DDSubscriptionId DD_CALL FakeLcSubscribe(DDPluginHandle, DDPhase phase, DDPhaseFn fn, void* user)
{
    g_phaseSubs.push_back({ g_nextPhaseId, phase, fn, user, true });
    return g_nextPhaseId++;
}

bool DD_CALL FakeLcUnsubscribe(DDPluginHandle, DDSubscriptionId id)
{
    for (auto& sub : g_phaseSubs) {
        if (sub.id == id && sub.alive) {
            sub.alive = false;
            return true;
        }
    }
    return false;
}

uint32_t DD_CALL FakeLcPhase() { return DD_PHASE_ASSETS_READY; }
bool DD_CALL FakeLcActive() { return g_world.active; }

const DDLifecycleService kFakeLifecycle = { { sizeof(DDLifecycleService), DD_SERVICE_LIFECYCLE_VERSION }, &FakeLcSubscribe,
    &FakeLcUnsubscribe, &FakeLcPhase, &FakeLcActive };

std::string g_fakeRoot;
const char* DD_CALL FakeCoreVersion() { return "1.0.0（离线预演）"; }
const char* DD_CALL FakeText() { return "test"; }
const char* DD_CALL FakeRoot() { return g_fakeRoot.c_str(); }
const char* DD_CALL FakePluginPath(DDPluginHandle) { return ""; }
void* DD_CALL FakeImageBase() { return nullptr; }
uint64_t DD_CALL FakeImageSize() { return 0; }

const DDGameInfoService kFakeInfo = { { sizeof(DDGameInfoService), DD_SERVICE_GAME_INFO_VERSION }, &FakeCoreVersion, &FakeText,
    &FakeText, &FakeText, &FakeRoot, &FakeRoot, &FakeRoot, &FakeRoot, &FakePluginPath, &FakeImageBase, &FakeImageSize };

const void* DD_CALL FakeQuery(const char* name, uint32_t)
{
    const std::string_view service(name ? name : "");
    if (service == DD_SERVICE_LOG) {
        return &kFakeLog;
    }
    if (service == DD_SERVICE_GAME_INFO) {
        return &kFakeInfo;
    }
    if (service == DD_SERVICE_LIFECYCLE) {
        return &kFakeLifecycle;
    }
    if (service == DD_SERVICE_GAME) {
        return &kFakeGame;
    }
    if (service == DD_SERVICE_EVENTS) {
        return &kFakeEvents;
    }
    return nullptr;
}

// 像 DDCore 的 Lifecycle::Fire：在 SEH 里调用回调，崩溃的订阅被撤销，不通知插件
struct PhaseCall {
    const FakePhaseSub* sub;
    const DDPhaseEvent* event;
};
std::size_t g_phaseCrashes = 0;

void CallPhase(void* raw)
{
    auto* call = static_cast<PhaseCall*>(raw);
    call->sub->fn(call->event, call->sub->user);
}

void FirePhase(DDPhase phase, std::uint32_t serial)
{
    const DDPhaseEvent event{ sizeof(DDPhaseEvent), static_cast<std::uint32_t>(phase), g_frame++, serial, 0 };
    const auto snapshot = g_phaseSubs;
    for (const auto& sub : snapshot) {
        bool alive = false;
        for (const auto& live : g_phaseSubs) {
            alive = alive || (live.id == sub.id && live.alive);
        }
        if (alive && sub.phase == phase) {
            PhaseCall call{ &sub, &event };
            if (!SehCall(&CallPhase, &call)) {
                ++g_phaseCrashes;
                for (auto& live : g_phaseSubs) {
                    live.alive = live.alive && live.id != sub.id;
                }
            }
        }
    }
}

std::size_t PhaseSubs(DDPhase phase)
{
    std::size_t n = 0;
    for (const auto& sub : g_phaseSubs) {
        n += sub.alive && sub.phase == phase ? 1 : 0;
    }
    return n;
}

std::size_t TickSubs()
{
    std::size_t n = 0;
    for (const auto& sub : g_phaseSubs) {
        n += sub.alive && sub.phase == DD_PHASE_TICK ? 1 : 0;
    }
    return n;
}

// DDCore.log 的样本：格式照 core.cpp（阶段、启用 Mod 列表、hook 报告），数字取自假中枢
FILE* g_coreSink = nullptr;
constexpr const char* kRehearsalRunId = "20261111-100000-4242";  // p3_scenario_check.py 的自测给探针样本用同一个
std::chrono::steady_clock::time_point g_rehearsalStart;

void SyncClock()
{
    g_logClock = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_rehearsalStart).count();
}

template <class... Args>
void CoreLog(const char* format, Args... args)
{
    SyncClock();
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer), format, args...);
    if (g_coreSink) {
        WriteLogLine(g_coreSink, DD_LOG_INFO, buffer);
    }
}

void CoreHookReport(const char* title)
{
    CoreLog("======== hook 报告（%s），共 11 个目标 ========", title);
    CoreLog("事件系统：已注册 listener 4 个，语义 hook 10 个，订阅 %zu 个", g_hub->ExternalSubscriberCount());
    CoreLog("======== 报告结束 ========");
}

// 每帧 lifecycle TICK，持续 seconds 秒真实时间（plugin.cpp 建的会话用真实时钟）
void Frames(double seconds, std::uint32_t serial)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    do {
        SyncClock();
        FirePhase(DD_PHASE_TICK, serial);
        Sleep(15);
    } while (std::chrono::steady_clock::now() < until);
    SyncClock();
}

FakeActor& ActorAt(std::uintptr_t p)
{
    return g_world.heroes.count(p) ? g_world.heroes[p] : g_world.monsters[p];
}

// 游戏侧的结算：.pre 派发后按最终值结算，再派发 .post（和 system.cpp 的 DamageImpl 等同序）
void SimDamage(std::uintptr_t target, std::uintptr_t attacker, double damage)
{
    SyncClock();
    FakeActor& actor = ActorAt(target);
    const bool hero = g_world.heroes.count(target) != 0;
    const double before = actor.hp;
    Fire pre("damage.pre");
    pre.P("target", P(target)).P("attacker", P(attacker)).Fl("damage", damage).I("damage_type", 0).I("source_id", 1).I("source_type", 0)
        .B("crit", false).Fl("hp_before", before).B("is_hero", hero);
    const Payload payload = pre.Go();
    const double amount = payload.cancelled ? 0 : pre.At("damage").v.f;
    actor.hp = static_cast<float>((std::max)(before - amount, 0.0));
    Fire post("damage.post");
    post.P("target", P(target)).P("attacker", P(attacker)).Fl("damage", amount).I("damage_type", 0).I("source_id", 1).I("source_type", 0)
        .B("crit", false).Fl("hp_before", before).B("is_hero", hero).Fl("hp_after", actor.hp).P("timeline", P(0x90000))
        .B("cancelled", payload.cancelled);
    post.Go();
}

void SimHit(std::uintptr_t attacker, std::uintptr_t target, double damage)
{
    Fire pre("hit.pre");
    pre.P("attacker", P(attacker)).P("target", P(target)).P("skill", P(0x80000)).B("hit", true).B("crit", false).Fl("damage", damage);
    pre.Go();
    const bool hit = pre.At("hit").v.b;
    const double dmg = pre.At("damage").v.f;
    if (hit) {
        SimDamage(target, attacker, dmg);
    }
    Fire post("hit.post");
    post.P("attacker", P(attacker)).P("target", P(target)).P("skill", P(0x80000)).B("hit", hit).B("crit", false).Fl("damage", dmg);
    post.Go();
}

void SimHeal(std::uintptr_t target, std::uintptr_t healer, double amount)
{
    FakeActor& actor = ActorAt(target);
    const double before = actor.hp;
    Fire pre("heal.pre");
    pre.P("target", P(target)).P("healer", P(healer)).Fl("amount", amount).I("type", 0).B("crit", false).Fl("hp_before", before);
    pre.Go();
    const double value = pre.At("amount").v.f;
    const double applied = (std::max)((std::min)(before + value, 30.0) - before, 0.0);
    actor.hp = static_cast<float>(before + applied);
    Fire post("heal.post");
    post.P("target", P(target)).P("healer", P(healer)).Fl("amount", value).I("type", 0).B("crit", false).Fl("hp_before", before)
        .Fl("applied", applied).Fl("hp_after", actor.hp);
    post.Go();
}

void SimStress(std::uintptr_t target, std::uintptr_t source, double amount)
{
    FakeActor& actor = ActorAt(target);
    const double before = actor.stress;
    Fire pre("stress.pre");
    pre.P("target", P(target)).P("source", P(source)).Fl("amount", amount).I("type", 0).Fl("stress_before", before);
    pre.Go();
    const double value = pre.At("amount").v.f;
    const double after = (std::min)((std::max)(before + value, 0.0), 200.0);
    actor.stress = static_cast<float>(after);
    Fire post("stress.post");
    post.P("target", P(target)).P("source", P(source)).Fl("amount", value).I("type", 0).Fl("stress_before", before)
        .Fl("applied", after - before).Fl("stress_after", after);
    post.Go();
}

void SimSkill(std::uintptr_t performer, std::uintptr_t target)
{
    for (const char* name : { "skill.pre", "skill.post" }) {
        Fire f(name);
        f.P("performer", P(performer)).P("skill", P(0x80000)).P("target", P(target)).I("target_count", 1).B("friendly", false);
        f.Go();
    }
}

void SimState(const char* from, const char* to)
{
    Fire f("app.state");
    f.I("from", 1).I("to", 2).S("from_name", from).S("to_name", to);
    f.Go();
}

// 游戏每场战斗把怪物 guid 的计数清零（审核 B-B1），同一次探索里的每场战斗都用同样的 guid；对象地址不同。
// shortBattle：只有一次怪物打英雄（C-B1 的触发条件：p3d_faulty 停用之后，本场 damage.post 少于 3 次）
void SimBattle(std::uint32_t serial, int index, double extraSeconds, bool shortBattle = false)
{
    constexpr std::uintptr_t kHero = 0x10000;
    constexpr std::uintptr_t kHero2 = 0x10100;
    const std::uintptr_t monster = 0x20000 + static_cast<std::uintptr_t>(index);
    g_world.monsters.clear();
    g_world.monsters[monster] = { 4500u + serial * 10, 12 };
    ++g_world.battle;  // DDCore 在 Battle::Create 置 bs_create 之后、派发 BattleCreatedEvent 之前就能看到新战斗
    SimState("as_raid_update", "as_combat");
    CoreLog("新战斗：战斗序号 %u（探索序号 %u，状态 1，怪物 1 个，guid 计数 1，帧 %llu，actor 服务调用时发现）", g_world.battle, g_world.raid,
        static_cast<unsigned long long>(g_frame));
    Fire start("battle.start");
    start.Go();
    if (shortBattle) {
        Frames(0.05, serial);
        SimHit(monster, kHero, 1);
        Frames(extraSeconds, serial);
        g_world.monsters.clear();
        Fire end("battle.end");
        end.Go();
        g_hub->EndScope(DD_EVENT_SCOPE_BATTLE);
        SimState("as_combat", "as_raid_update");
        Frames(2.3, serial);
        return;
    }
    // 每个动作之间过几帧（游戏里也是这样；p3c_actor 的 wait_event 快照在下一帧和句柄读数比较）
    const auto step = [&] { Frames(0.05, serial); };
    step();
    SimSkill(monster, kHero);
    step();
    SimHit(monster, kHero, 4);  // 本次进存档第一次命中英雄：p3d_core 改成未命中
    step();
    SimHit(monster, kHero, 4);  // 第一次英雄受伤：减半
    step();
    SimHit(monster, kHero2, 3);  // 第二次：取消
    step();
    SimSkill(kHero, monster);
    SimHit(kHero, monster, 5);  // 英雄打怪物：不改
    step();
    SimHeal(kHero, kHero2, 5);  // 第一次治疗没满血的英雄：改成 0
    step();
    SimStress(kHero, monster, 5);  // 第一次加压力：改成 0
    step();
    SimStress(kHero, monster, 3);
    step();
    SimHeal(kHero, kHero2, 2);
    Frames(extraSeconds, serial);
    g_world.monsters.clear();  // 游戏先清空怪物表，再派发 BattleDestroyedEvent，DDCore 随后撤销 battle 作用域
    Fire end("battle.end");
    end.Go();
    g_hub->EndScope(DD_EVENT_SCOPE_BATTLE);
    SimState("as_combat", "as_raid_update");
    Frames(2.3, serial);  // scope=battle 和 actor 作用域在 2 秒后检查
}

void SimCampaign(std::uint32_t serial, const std::vector<FakeModEntry>& mods, int battles)
{
    g_fakeMods = mods;
    std::size_t found = 0;
    for (const auto& mod : mods) {
        found += mod.found ? 1 : 0;
    }
    g_world.campaign = serial;
    g_world.active = true;
    CoreLog("阶段 CampaignLoad，帧 %llu，存档序号 %u，线程 1，Campaign=0000000000070000，Buff::Library=0000000000071000，状态 last=1 next=1",
        static_cast<unsigned long long>(g_frame), serial);
    CoreLog("启用 Mod 列表 %zu 项（已安装 300 个），找到目录 %zu 项", mods.size(), found);
    for (std::size_t i = 0; i < mods.size(); ++i) {
        if (!mods[i].found) {
            CoreLog("  [%zu] name='%s' source='%s' 没有找到目录（未安装）", i, mods[i].name.c_str(), mods[i].source.c_str());
        }
    }
    CoreLog("actor 句柄：英雄可用，怪物可用，最大生命可用；探索序号 %u，战斗序号 %u", g_world.raid, g_world.battle);
    FirePhase(DD_PHASE_CAMPAIGN_LOAD, serial);
    Fire load("campaign.load");
    load.I("serial", serial).P("campaign", P(0x70000));
    load.Go();
    CoreHookReport("CampaignLoad");
    Frames(0.2, serial);
    if (battles > 0) {
        SimState("as_town", "as_raid_start");
        ++g_world.raid;
        CoreLog("进入 as_raid_start（新建 Raid），探索序号 %u，帧 %llu", g_world.raid, static_cast<unsigned long long>(g_frame));
        Fire raid("raid.start");  // 加载线程上发生：复制后排队到主线程，只读、指针清零
        raid.P("raid", P(0x50000));
        raid.Deferred();
        Fire inventory("inventory.add");
        inventory.P("inventory", P(0x52000)).P("item", P(0x53000)).I("value", 1);
        inventory.Deferred();
        SimState("as_raid_start", "as_raid_update");
        Frames(0.2, serial);
        // 走廊里的陷阱（C-B1）：战斗之外英雄受到伤害，p3d_faulty 的 damage.post 计数从这里开始
        SimDamage(0x10000, 0, 1);
        Frames(0.1, serial);
        for (int i = 0; i < battles; ++i) {
            SimBattle(serial, static_cast<int>(serial) * 10 + i, 0.3);  // p3d_faulty 的定时器（1 秒）、协程（2 秒）在之后的帧里
        }
        if (battles >= 2) {
            // 第三场是只有一次伤害的短战斗（C-B1：修复前 isolation 用累计出错次数比本场次数，这里会误报失败）
            SimBattle(serial, static_cast<int>(serial) * 10 + battles, 0.3, true);
        }
        Fire finish("raid.finish");
        finish.P("results", P(0x51000));
        finish.Go();
        g_hub->EndScope(DD_EVENT_SCOPE_RAID);
        Frames(2.3, serial);
    }
    CoreLog("阶段 CampaignUnload，帧 %llu，存档序号 %u，线程 1", static_cast<unsigned long long>(g_frame), serial);
    FirePhase(DD_PHASE_CAMPAIGN_UNLOAD, serial);
    g_hub->EndScope(DD_EVENT_SCOPE_CAMPAIGN);
    g_world.active = false;
    Frames(0.1, serial);
}

// 场景 Mod 的离线预演：经过真实的 plugin.cpp（启用列表 → 发现 → 加载 → 拆除），主题表来自游戏，
// 走两次进存档（第一次两场战斗，第二次一场）和一次没有 Lua Mod 的存档。DDLua.log 和 DDCore.log 的样本写到
// build/p3d_rehearsal/，tools/p3_scenario_check.py 用它们自测
void TestScenarioRehearsal()
{
    g_log.clear();
    CHECK(ResetHubFromDump());
    ResetWorld();
    const std::string outDir = ProjectFile("build/p3d_rehearsal");
    CreateDirectoryW(ddlua::Utf8ToWide(ProjectFile("build")).c_str(), nullptr);
    CreateDirectoryW(ddlua::Utf8ToWide(outDir).c_str(), nullptr);
    FILE* luaLog = nullptr;
    FILE* coreLog = nullptr;
    _wfopen_s(&luaLog, ddlua::Utf8ToWide(outDir + "/DDLua.log").c_str(), L"wb");
    _wfopen_s(&coreLog, ddlua::Utf8ToWide(outDir + "/DDCore.log").c_str(), L"wb");
    CHECK(luaLog && coreLog);
    if (!luaLog || !coreLog) {
        return;
    }
    std::fputs("\xEF\xBB\xBF", luaLog);
    std::fputs("\xEF\xBB\xBF", coreLog);
    g_logSink = luaLog;
    g_coreSink = coreLog;
    g_rehearsalStart = std::chrono::steady_clock::now();
    g_fakeRoot = outDir + "/game";  // 没有 DDLua.cfg：默认配置（预算 50 毫秒，main.lua 500 毫秒）

    // 探针一样的 native 插件：11 个订阅
    for (const char* topic : { "battle.start", "battle.end", "damage.pre", "hit.pre", "skill.pre", "heal.pre", "stress.pre", "damage.post",
             "heal.post", "stress.post", "actor.death", "hero.dismiss" }) {
        g_hub->Subscribe(kOtherOwner, topic, [](DDEvent*, void*) {}, nullptr, 0, DD_EVENT_SCOPE_PLUGIN, nullptr);
    }
    // 格式照 core.cpp：首行带运行标识（审核 C-S3），插件日志的首行由 DDCore 写，同一个运行标识
    CoreLog("DDCore 1.0.0 启动，运行标识 %s，主线程 1（离线预演的样本，格式照 core.cpp）", kRehearsalRunId);
    CoreLog("插件 DDLua 加载成功（版本 1.0.0）");
    CoreLog("插件 probe_p3c 加载成功（版本 1.0.0）");
    WriteLogLine(luaLog, DD_LOG_INFO, std::string("日志开始，DDCore 1.0.0，运行标识 ") + kRehearsalRunId + "，插件路径 （离线预演）");
    CoreLog("阶段 AssetsReady，帧 2，线程 1");
    CoreHookReport("AssetsReady");

    const DDApi api{ sizeof(DDApi), DD_ABI_VERSION, &FakeQuery, kPlugin };
    CHECK(DDPlugin_Load(&api));
    CHECK(LogHas("[DDLua] 只订阅了 lifecycle（CampaignLoad、CampaignUnload），没有订阅事件主题，没有安装 hook"));
    g_fakeModsAvailable = true;
    const std::string mods = std::string(DD_PROJECT_ROOT) + "/";
    const std::vector<FakeModEntry> enabled = {
        { "DDLua Test P3d Faulty", "mod_local_source", mods + "tests/lua/scenario/p3d_faulty" },
        { "DDLua Test P3d Broken", "mod_local_source", mods + "tests/lua/scenario/p3d_broken" },
        { "Arena Bosses", "mod_local_source", mods + "tests/lua/mods/not_lua" },
        { "DDLua Example Hello", "mod_local_source", mods + "examples/lua/hello_lua" },
        { "DDLua Test P3c Actor", "mod_local_source", mods + "tests/lua/scenario/p3c_actor" },
        { "DDLua Test P3d Core", "mod_local_source", mods + "tests/lua/scenario/p3d_core" },
        { "1234567890", "Steam", "", false },
    };
    SimCampaign(1, enabled, 2);
    CHECK(TickSubs() == 0);
    SimCampaign(2, enabled, 1);
    SimCampaign(3, { { "Arena Bosses", "mod_local_source", mods + "tests/lua/mods/not_lua" } }, 0);

    g_logSink = nullptr;
    g_coreSink = nullptr;
    std::fclose(luaLog);
    std::fclose(coreLog);
    g_fakeModsAvailable = false;
    g_fakeMods.clear();

    // 判定（tools/p3_scenario_check.py 对同一份日志做更细的判定）
    std::size_t passed = 0;
    std::size_t failed = 0;
    for (const auto& line : g_log) {
        const bool scenario = line.find("[p3d_") == 0 || line.find("[p3c_actor]") == 0 || line.find("[hello_lua]") == 0;
        passed += scenario && line.find("] 通过 ") != std::string::npos ? 1 : 0;
        if (scenario && line.find("] 失败") != std::string::npos) {
            ++failed;
            std::printf("    %s\n", line.c_str());
        }
    }
    std::printf("  场景预演：通过 %zu 行，失败 %zu 行；日志样本在 %s\n", passed, failed, outDir.c_str());
    CHECK(failed == 0 && passed >= 40);
    for (const char* tag : { "damage.halve", "damage.cancel", "damage.cancel.seen", "hit.miss", "heal.zero", "stress.zero", "skill.pre",
             "cancel.error", "deferred.raid", "deferred.snapshot", "deferred.inventory", "scope.battle", "scope.raid", "payload.expired",
             "campaign.load", "isolation", "budget", "broken.import" }) {
        const std::size_t n = CountLog(std::string("[p3d_core] 通过 [") + tag + "]");
        if (n < 2) {
            std::printf("    p3d_core 的 [%s] 通过 %zu 次（两次进存档应至少各一次）\n", tag, n);
        }
        CHECK(n >= 2);
    }
    CHECK(CountLog("[存档#1] 进入存档：启用 7 个 Mod，1 个没有找到目录") == 1 && CountLog("[存档#2] 进入存档") == 1);
    // 依赖排序：p3d_faulty 在启用列表里排第一，但依赖 p3d_core，排到它后面
    CHECK(LogHas("[存档#1] 加载顺序：p3d_broken hello_lua p3c_actor p3d_core p3d_faulty") &&
        CountLog("[存档#1] 加载完成：成功 4 个，共 5 个 Lua Mod") == 1);
    CHECK(CountLog("[p3d_broken] 加载失败：p3d_broken/main.lua:") == 2 && CountLog("[p3d_broken] 失败") == 0);
    CHECK(CountLog("[p3d_faulty] battle.start 的回调出错（超出执行预算），已停用该订阅") == 2);
    CHECK(CountLog("[p3d_faulty] dd.after 的回调出错（超出执行预算）：执行超出预算") == 2);
    CHECK(CountLog("[p3d_faulty] 协程超出执行预算，已终止") == 2);
    CHECK(CountLog("[p3d_faulty] damage.post 的回调出错（连续 3 次），已停用该订阅") == 2);
    CHECK(CountLog("分配器残留 0 字节") == 2 && CountLog("DDLua 持有 SDK 订阅共 0 个，DDCore 统计 DDLua 的订阅 0 个") == 2);
    CHECK(CountLog("不一致") == 0);
    CHECK(CountLog("[存档#3] 没有可加载的 Lua Mod，不创建 VM") == 1);
    CHECK(CountLog("[hello_lua] 战斗开始") == 4 && CountLog("[hello_lua] 战斗结束") == 4);
    // C-B1：走廊陷阱之后、短战斗里都不误报 isolation 失败（上面的「失败」行为 0 已经覆盖，这里再点名核对）
    CHECK(CountLog("[p3d_core] 失败 [isolation]") == 0 && CountLog("[p3d_core] 通过 [isolation]") >= 4);
    CHECK(LogHas("本场 0 次（已被停用），本 Mod 本场的 damage.post 照常收到 1 次"));
    // B-B1：同一次探索的第二场、第三场战斗，怪物 guid 和上一场相同，旧句柄全部失效（第二轮 R2B-应修1：逐项计数）
    for (const char* line : { "[p3c_actor] 通过 第 2 场（探索 2，战斗 3）：上一场的怪物里和本场怪物 guid 相同的 1 个；",
             "[p3c_actor] 通过 第 3 场（探索 2，战斗 4）：上一场的怪物里和本场怪物 guid 相同的 1 个；" }) {
        CHECK(CountLog(std::string(line) + "旧句柄失效 1/1，== 不相等 1/1，不是同一个对象 1/1，表键分开 1/1，新句柄驻留 1/1") == 1);
    }
    CHECK(TickSubs() == 0 && CoreCount() == 0 && ddlua::EventBinding::GlobalSdkCount() == 0);
}

// 第二轮审核 R2B-应修2：p3c_actor 的 wait_event 快照等的是正在 damage.pre 里挨打的英雄，不依赖第一个见到的英雄。
// 修复前它只对第一个 remember 到的英雄（这里是先出手的 hero#13）建等待者，600 秒内它不挨打就没有快照的判定行
void TestActorScenarioSnapshot()
{
    g_log.clear();
    CHECK(ResetHubFromDump());  // 游戏的主题表：damage.pre/post 带 hp_before、is_hero、cancelled
    ResetWorld();
    const auto host = MakeActorHost();
    // 假世界里英雄的生命不变（hero#12 是 10，hero#13 是 20），damage.post 的 hp_after 和句柄读数一致
    const auto damage = [](std::uintptr_t target, bool post) {
        const double hp = g_world.heroes[target].hp;
        Fire pre("damage.pre");
        pre.P("target", P(target)).P("attacker", P(0x20000)).Fl("damage", 3).Fl("hp_before", hp).B("is_hero", true);
        pre.Go();
        if (post) {
            Fire done("damage.post");
            done.P("target", P(target)).P("attacker", P(0x20000)).Fl("damage", 3).Fl("hp_before", hp).Fl("hp_after", hp).B("is_hero", true);
            done.Go();
        }
    };
    {
        ddlua::Session session(host, TestConfig(), "快照");
        session.SetClock([] { return g_now; });
        g_now = 0;
        session.Load({ { "场景 p3c_actor", std::string(DD_LUA_FIXTURES) + "/scenario/p3c_actor" } });
        CHECK(session.LoadedCount() == 1);
        Fire start("battle.start");
        start.Go();
        // 先出手的是 hero#13（第一个被见到的英雄），挨打的是 hero#12
        Fire skill("skill.pre");
        skill.P("performer", P(0x10100)).P("target", P(0x20000)).P("skill", P(0x80000)).I("target_count", 1);
        skill.Go();
        damage(0x10000, true);
        Tick(session, 0.1);
        Tick(session, 0.2);
        CHECK(LogHas("[p3c_actor] 通过 wait_event 快照的 target 就是保存的英雄句柄（dd.actor(hero#12)）"));
        CHECK(LogHas("[p3c_actor] 通过 快照 hp_after=10.0，下一帧句柄读到 hp=10.0"));
        session.Teardown();
        CHECK(CoreCount() == 0 && session.memory().residual == 0);
    }
    {
        // damage.pre 之后没有 damage.post（超时）：放弃这个英雄，下一个挨打的英雄重新等
        g_log.clear();
        ddlua::Session session(host, TestConfig(), "快照重等");
        session.SetClock([] { return g_now; });
        g_now = 0;
        session.Load({ { "场景 p3c_actor", std::string(DD_LUA_FIXTURES) + "/scenario/p3c_actor" } });
        damage(0x10100, false);
        Tick(session, 1);
        Tick(session, 62);
        CHECK(LogHas("的 damage.pre 之后 60 秒内没有 damage.post，换下一个挨打的英雄重新等 wait_event 快照"));
        damage(0x10000, true);
        Tick(session, 62.1);
        Tick(session, 62.2);
        CHECK(LogHas("[p3c_actor] 通过 wait_event 快照的 target 就是保存的英雄句柄（dd.actor(hero#12)）"));
        CHECK(CountLog("[p3c_actor] 失败") == 0);
        session.Teardown();
        CHECK(CoreCount() == 0 && session.memory().residual == 0);
    }
}

// 第二轮审核 R2B-备注4：p3d_core 的 damage.halve 把受到伤害的倍率算进去。游戏按 roundf(伤害 × k) 扣血（483733-483747），
// k = 1 + [Actor+0xBE0]，Lua 读不到。修复前的判定是「掉血少于原值」：原值 2、k = 1.5 时掉血 roundf(1.5) = 2，判失败；
// k = 2、原值为偶数时一律判失败
void TestScenarioHalveMultiplier()
{
    CHECK(ResetHubFromDump());  // 游戏的主题表：damage.pre/post 带 hp_before、is_hero、cancelled
    ResetWorld();
    const auto host = MakeActorHost();
    struct Hp {
        double hp = 30;
    };
    // 一次伤害：damage.pre 之后按最终值和倍率 k 结算，再派发 damage.post。ignore 为真时模拟游戏没用写入值（按原值结算）
    const auto hit = [](Hp& hero, std::uintptr_t target, double damage, double k, bool ignore = false) {
        Fire pre("damage.pre");
        pre.P("target", P(target)).P("attacker", P(0x20000)).Fl("damage", damage).Fl("hp_before", hero.hp).B("is_hero", true);
        const Payload payload = pre.Go();
        const double written = payload.cancelled ? 0 : pre.At("damage").v.f;
        const double loss = payload.cancelled ? 0 : std::round((ignore ? damage : written) * k);
        const double before = hero.hp;
        hero.hp = (std::max)(before - loss, 0.0);
        Fire post("damage.post");
        post.P("target", P(target)).P("attacker", P(0x20000)).Fl("damage", written).Fl("hp_before", before).Fl("hp_after", hero.hp)
            .B("is_hero", true).B("cancelled", payload.cancelled);
        post.Go();
    };
    const auto run = [&](const char* name, const std::function<void()>& battles) {
        g_log.clear();
        ddlua::Session session(host, TestConfig(), name);
        session.SetClock([] { return g_now; });
        g_now = 0;
        session.Load({ { "场景 p3d_core", std::string(DD_LUA_FIXTURES) + "/scenario/p3d_core" } });
        CHECK(session.LoadedCount() == 1);
        battles();
        session.Teardown();
        CHECK(CoreCount() == 0 && session.memory().residual == 0);
    };
    const auto battle = [](const std::function<void()>& body) {
        Fire start("battle.start");
        start.Go();
        body();
        Fire end("battle.end");
        end.Go();
        g_hub->EndScope(DD_EVENT_SCOPE_BATTLE);
    };
    // 审核者的例子：原值 2、k = 1.5，掉血 2（不少于原值）；本场另一次伤害 4 → 掉血 6，估出 k 在 1.375～1.625
    run("倍率 1.5", [&] {
        Hp a;
        battle([&] {
            hit(a, 0x10000, 2, 1.5);  // 减半：写入 1，掉血 roundf(1.5) = 2
            hit(a, 0x10000, 3, 1.5);  // 取消
            hit(a, 0x10000, 4, 1.5);  // 估倍率的样本
        });
    });
    CHECK(CountLog("[p3d_core] 通过 [damage.halve]") == 1 && CountLog("[p3d_core] 失败 [damage.halve]") == 0);
    CHECK(LogHas("掉血 2.00；按本场这个英雄另外 1 次伤害估出受到伤害的倍率 1.") && LogHas("掉血符合写入值 × 倍率，不符合原值 × 倍率"));
    CHECK(CountLog("[p3d_core] 通过 [damage.cancel]") == 1);
    // k = 2、原值 4（偶数）：掉血 4 等于原值
    run("倍率 2", [&] {
        Hp a;
        battle([&] {
            hit(a, 0x10000, 4, 2);
            hit(a, 0x10000, 3, 2);
            hit(a, 0x10000, 3, 2);
        });
    });
    CHECK(CountLog("[p3d_core] 通过 [damage.halve]") == 1 && CountLog("[p3d_core] 失败 [damage.halve]") == 0);
    // 游戏没用写入值（按原值结算）、倍率 1：仍然判失败
    run("写入没有生效", [&] {
        Hp a;
        battle([&] {
            hit(a, 0x10000, 4, 1, true);
            hit(a, 0x10000, 3, 1);
            hit(a, 0x10000, 3, 1);
        });
    });
    CHECK(CountLog("[p3d_core] 失败 [damage.halve]") == 1 && LogHas("掉血符合原值 × 倍率，不符合写入值 × 倍率：写入没有生效"));
    // 本场没有别的伤害、掉血不等于写入值：只写信息，下一场再减半一次；下一场没有修正，按写入值判通过
    run("判不了", [&] {
        Hp a;
        battle([&] {
            hit(a, 0x10000, 4, 1.5);  // 写入 2，掉血 3
        });
        battle([&] {
            hit(a, 0x10000, 4, 1);  // 再减半一次：写入 2，掉血 2
        });
    });
    CHECK(CountLog("[p3d_core] 信息 [damage.halve]") == 1 && LogHas("这个英雄本场没有别的伤害可以估倍率，判不了，下一场再试"));
    CHECK(CountLog("[p3d_core] 通过 [damage.halve]") == 1 && CountLog("[p3d_core] 失败 [damage.halve]") == 0);
    CHECK(LogHas("掉血 2.00，正好是写入值（本场没有别的伤害可以估倍率，按没有受到伤害的百分比修正判断）"));
    // 第三轮 R3B-备注2（H2 的建议用例）：倍率样本每场战斗重置，上一场英雄 H 的倍率 0.5 不能带到走廊里。
    // 修复前的 main.lua 用上一场的样本估倍率，走廊里按写入值结算的伤害被判「写入没有生效」
    run("走廊", [&] {
        Hp a, h;
        battle([&] {
            hit(a, 0x10000, 4, 1.5);  // 对英雄 A 减半：写入 2，掉血 3，A 本场没有别的伤害，判不了
            hit(a, 0x10000, 3, 1.5);  // 取消
            hit(h, 0x10100, 4, 0.5);  // 英雄 H 本场的样本（倍率 0.5）
        });
        hit(h, 0x10100, 4, 1);  // 走廊：H 挨打时再减半一次，写入 2，掉血 2
        battle([] {});          // 下一场开始时判定走廊里的减半
    });
    CHECK(CountLog("[p3d_core] 通过 [damage.halve]") == 1 && CountLog("[p3d_core] 失败 [damage.halve]") == 0);
    CHECK(LogHas("掉血 2.00，正好是写入值"));
}

// ---------------------------------------------------------------- 审核修复 F1

// B-Y3：Lua 回调里发生原生异常（SEH）后 VM 标记为损坏：停止一切派发，撤销通知不碰 VM，拆除时直接丢弃，下一个会话照常
void TestVmBroken()
{
    g_log.clear();
    ResetHub();
    ResetWorld();
    const auto host = MakeActorHost();
    {
        ddlua::Session session(host, TestConfig(), "损坏");
        g_now = 0;
        session.SetClock([] { return g_now; });
        session.Load(Sources({ "ev_a", "ev_b" }));
        CHECK(Lua(session, "ev_a", R"(
            dd.on("app.tick", function() print("ev_a 事件") if CRASH then dd.actor.hero(666) end end)
            dd.every(0, function() print("ev_a 定时器") end)
            dd.spawn(function() local x <close> = setmetatable({}, { __close = function() print("ev_a 的 <close>") end }) dd.wait(1000) end)
        )"));
        CHECK(Lua(session, "ev_b", R"(dd.on("app.tick", function() print("ev_b 事件") end, { priority = 10 }))"));
        Fire t1("app.tick");
        t1.Go();
        Tick(session, 1);
        CHECK(CountLog("ev_a 事件") == 1 && CountLog("ev_b 事件") == 1 && CountLog("ev_a 定时器") == 1);
        CHECK(CoreCount() == 2);

        // ev_a 的回调里调用的服务发生访问违例：异常穿过 Lua，展开到「DDCore」（SehInvoke），DDCore 撤销订阅并通知 CRASH
        CHECK(Lua(session, "ev_a", "CRASH = true"));
        g_crashFind = true;
        Fire t2("app.tick");
        t2.Go();
        g_crashFind = false;
        CHECK(session.Broken() && session.vm()->Abandoned());
        CHECK(LogHas("Lua VM 已损坏（订阅 app.tick 的回调里发生了原生异常（SEH）"));
        CHECK(LogHas("被 DDCore 撤销（回调崩溃）"));
        // 之后不再进入 Lua：同一主题的别的订阅、定时器、RunChunk 都不执行
        Fire t3("app.tick");
        t3.Go();
        Tick(session, 2);
        Tick(session, 3);
        CHECK(CountLog("ev_b 事件") == 1 && CountLog("ev_a 定时器") == 1);
        std::string error;
        CHECK(!session.RunChunk(*session.FindMod("ev_a"), "print('不该执行')", "=dead", &error) && !LogHas("不该执行"));
        CHECK(CountLog("Lua VM 已损坏（") == 1);  // 只写一次
        // 拆除：不碰 VM（<close> 不运行），撤销 SDK 订阅，VM 泄漏
        session.Teardown();
        CHECK(!LogHas("ev_a 的 <close>"));
        CHECK(LogHas("Lua VM 已损坏，直接丢弃") && CoreCount() == 0 && ddlua::EventBinding::GlobalSdkCount() == 0);
        CHECK(session.memory().residual > 0);
        std::printf("  损坏的 VM 丢弃时泄漏 %zu 字节\n", session.memory().residual);
    }
    {
        // 模拟的崩溃通知（回调没有真正执行）同样标记损坏
        ddlua::Session session(host, TestConfig(), "损坏 2");
        session.Load(Sources({ "ev_b" }));
        CHECK(Lua(session, "ev_b", R"(CRASH_SUB = dd.on("hero.quirk", function() end))"));
        g_crashNext = true;
        Fire quirk("hero.quirk");
        quirk.Go();
        CHECK(session.Broken() && LogHas("被 DDCore 撤销（回调崩溃）"));
        session.Teardown();
        CHECK(CoreCount() == 0);
    }
    {
        // 下一个会话新建 VM，照常
        g_log.clear();
        ddlua::Session session(host, TestConfig(), "下一个存档");
        session.Load(Sources({ "ev_a" }));
        CHECK(Lua(session, "ev_a", "dd.on('app.tick', function() print('新 VM 的事件') end)"));
        Fire t("app.tick");
        t.Go();
        CHECK(LogHas("新 VM 的事件") && !session.Broken());
        session.Teardown();
        CHECK(CoreCount() == 0 && session.memory().residual == 0);
    }
}

// A-B2：调度器每帧的总时间（frame_budget_ms），超出的顺延到下一帧，Mod 之间轮流，日志限速
void TestFrameBudget()
{
    g_log.clear();
    ResetHub();
    const auto host = MakeEventHost();
    auto config = TestConfig();
    config.frameBudgetMs = 20;
    ddlua::Session session(host, config, "帧预算");
    g_now = 0;
    session.SetClock([] { return g_now; });
    session.Load(Sources({ "ev_a", "ev_b" }));
    // ev_a：10 个每帧定时器，每个忙 25 毫秒（单个在 50 毫秒预算内，从不被停用）；ev_b：1 个很快的每帧定时器
    CHECK(Lua(session, "ev_a", R"(
        RUNS = {}
        for i = 1, 10 do
            RUNS[i] = 0
            dd.every(0, function() local t = os.clock() while os.clock() - t < 0.025 do end RUNS[i] = RUNS[i] + 1 end)
        end
    )"));
    CHECK(Lua(session, "ev_b", "B = 0 dd.every(0, function() B = B + 1 end)"));
    double worst = 0;
    double total = 0;
    for (int frame = 1; frame <= 20; ++frame) {
        const auto start = std::chrono::steady_clock::now();
        Tick(session, frame * 0.02);
        const double ms = Ms(start);
        worst = (std::max)(worst, ms);
        total += ms;
    }
    std::printf("  帧预算：20 帧最长 %.0f ms、平均 %.0f ms（不限时每帧 10 x 25 = 250 ms）\n", worst, total / 20);
    CHECK(worst < 120);  // 帧预算 20 毫秒加最多一项（25 毫秒），留出余量
    CHECK(Lua(session, "ev_b", "assert(B >= 9, '轮转：ev_b 不会被 ev_a 饿死，B = ' .. B)"));
    CHECK(Lua(session, "ev_a", R"(
        local n = 0 for i = 1, 10 do assert(RUNS[i] >= 1, '同一个 Mod 里最久没执行的先执行，定时器 ' .. i .. ' 没执行过') n = n + RUNS[i] end
        assert(n >= 15 and n <= 45, n)
        assert(dd.stats().timers == 10, '顺延不停用定时器')
    )"));
    CHECK(CountLog("顺延到下一帧") == 1);  // 限速：第一次写，之后每 10 秒最多一条
    Tick(session, 11);  // 10 秒后再写一条，带上中间的累计
    CHECK(CountLog("顺延到下一帧") == 2 && LogHas("帧顺延"));
    session.Teardown();
    CHECK(CoreCount() == 0);
}

// A-B2：排队的重载也受帧预算约束：一帧重载一个，剩下的顺延
void TestReloadBudget()
{
    g_log.clear();
    ResetHub();
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    const std::string root = std::string(temp) + "dd_lua_reload_budget_" + std::to_string(GetCurrentProcessId());
    CreateDirectoryA(root.c_str(), nullptr);
    const auto write = [](const std::string& path, const std::string& text) {
        FILE* f = nullptr;
        fopen_s(&f, path.c_str(), "wb");
        if (f) {
            std::fputs(text.c_str(), f);
            std::fclose(f);
        }
    };
    std::vector<ddlua::ModSource> sources;
    for (const char* id : { "slow_a", "slow_b" }) {
        const std::string dir = root + "\\" + id;
        CreateDirectoryA(dir.c_str(), nullptr);
        CreateDirectoryA((dir + "\\lua").c_str(), nullptr);
        write(dir + "\\lua\\manifest.json", std::string(R"({"id":")") + id + R"(","version":"1.0.0","api":1})");
        write(dir + "\\lua\\main.lua", "local t = os.clock() while os.clock() - t < 0.03 do end");
        sources.push_back({ std::string("临时 ") + id, dir });
    }
    const auto host = MakeEventHost();
    auto config = TestConfig();
    config.devMode = true;
    config.frameBudgetMs = 20;
    {
        ddlua::Session session(host, config, "重载预算");
        g_now = 0;
        session.SetClock([] { return g_now; });
        session.Load(sources);
        CHECK(session.LoadedCount() == 2);
        CHECK(Lua(session, "slow_a", "assert(dd.reload('slow_a') and dd.reload('slow_b'))"));
        Tick(session, 0.1);
        CHECK(LogHas("重载 slow_a：") && !LogHas("重载 slow_b："));
        Tick(session, 0.2);
        CHECK(LogHas("重载 slow_b："));
        session.Teardown();
    }
    for (const char* id : { "slow_a", "slow_b" }) {
        const std::string dir = root + "\\" + id;
        DeleteFileA((dir + "\\lua\\manifest.json").c_str());
        DeleteFileA((dir + "\\lua\\main.lua").c_str());
        RemoveDirectoryA((dir + "\\lua").c_str());
        RemoveDirectoryA(dir.c_str());
    }
    RemoveDirectoryA(root.c_str());
}

// 把 from 环境里的全局 name 复制到 to 环境的 as（模拟 Mod 通过导出拿到别的 Mod 的句柄）
void CopyGlobal(ddlua::Session& session, const char* from, const char* to, const char* name, const char* as)
{
    ddlua::Mod* a = session.FindMod(from);
    ddlua::Mod* b = session.FindMod(to);
    std::string error;
    CHECK(a && b && session.vm()->Protected(
        [&](lua_State* L) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, a->envRef);
            lua_getfield(L, -1, name);
            lua_rawgeti(L, LUA_REGISTRYINDEX, b->envRef);
            lua_insert(L, -2);
            lua_setfield(L, -2, as);
            lua_pop(L, 2);
        },
        &error));
}

// B-N4、B-N1、A-B3、A-C2、A-C4
void TestModBoundaries()
{
    g_log.clear();
    ResetHub();
    const auto host = MakeEventHost();
    auto config = TestConfig();
    config.logLinesPerSec = 50;
    ddlua::Session session(host, config, "边界");
    g_now = 0;
    session.SetClock([] { return g_now; });
    session.Load(Sources({ "basic", "other", "ev_a", "ev_b" }));
    CHECK(session.LoadedCount() == 4);

    // B-N4：撤销只能由所属 Mod 自己的代码进行
    CHECK(Lua(session, "ev_a", R"(
        SUB = dd.on("app.tick", function() end)
        TIMER = dd.every(1, function() end)
        TASK = dd.spawn(function() dd.wait(100) end)
    )"));
    CopyGlobal(session, "ev_a", "ev_b", "SUB", "FOREIGN_SUB");
    CopyGlobal(session, "ev_a", "ev_b", "TIMER", "FOREIGN_TIMER");
    CopyGlobal(session, "ev_a", "ev_b", "TASK", "FOREIGN_TASK");
    CHECK(Lua(session, "ev_b", R"(
        local function denied(fn, ...) local ok, e = pcall(fn, ...) assert(not ok and e:find("属于 Mod ev_a"), tostring(e)) end
        denied(FOREIGN_SUB.off, FOREIGN_SUB)
        denied(dd.off, FOREIGN_SUB)
        denied(FOREIGN_TIMER.cancel, FOREIGN_TIMER)
        denied(FOREIGN_TASK.cancel, FOREIGN_TASK)
        assert(FOREIGN_SUB:active() and FOREIGN_TIMER:active() and FOREIGN_TASK:active(), "读状态不受限制")
    )"));
    CHECK(Lua(session, "ev_a", "assert(SUB:off() and TIMER:cancel() and TASK:cancel()) assert(not SUB:off())"));
    CHECK(Lua(session, "ev_b", "assert(FOREIGN_SUB:off() == false, '已经无效的返回 false，不报错')"));

    // B-N1：Mod 手动恢复或关闭 dd.spawn 的协程，没有超时的 wait_event 也会被清理（原来要等到 Mod 拆除）
    CHECK(Lua(session, "ev_a", R"(
        dd.spawn(function() CLOSED_CO = coroutine.running() dd.wait_event("hero.quirk") end)
        dd.spawn(function() FINISHED_CO = coroutine.running() dd.wait_event("hero.quirk") end)
        dd.spawn(function() WAITING_CO = coroutine.running() dd.wait(100) dd.wait(1) end)
        assert(dd.stats().tasks == 3 and dd.stats().waiters == 2)
        assert(coroutine.close(CLOSED_CO))
        assert(coroutine.resume(FINISHED_CO))
        local ok, e = coroutine.resume(WAITING_CO)
        assert(not ok and e:find("手动恢复"), e)
    )"));
    Tick(session, 1);
    CHECK(Lua(session, "ev_a", "assert(dd.stats().tasks == 0 and dd.stats().waiters == 0, dd.stats().tasks)"));
    CHECK(CountLog("被 Mod 自己恢复到结束或关闭了") == 3);

    // A-B3：dd.import 返回只读的浅代理
    CHECK(Lua(session, "other", R"(
        local b = dd.import("basic")
        assert(b.value == 42 and b.util.name == "basic util")
        local ok, e = pcall(function() b.value = 1 end) assert(not ok and e:find("只读") and e:find("basic"), e)
        ok, e = pcall(function() b.extra = 1 end) assert(not ok, "新键也不能加")
        local keys = {} for k, v in pairs(b) do keys[#keys + 1] = k end table.sort(keys)
        assert(#keys == 2 and keys[1] == "util" and keys[2] == "value", table.concat(keys, ","))
        local _, state = pairs(b) assert(rawequal(state, b), "pairs 交出的是代理，拿不到原表")
        assert(getmetatable(b) == false and not pcall(setmetatable, b, nil))
        rawset(b, "value", 7) assert(b.value == 7 and dd.import("basic") == b, "rawset 只改自己那份代理（按使用方缓存，第二轮 R2A-备注1）")
        b.util.touched = true  -- 更深层的表仍然共享（文档写明）
    )"));
    CHECK(Lua(session, "basic", "assert(dd.import('basic').util.touched == true)"));

    // A-C2：math.random 每个 Mod 一份状态
    CHECK(Lua(session, "ev_a", "math.randomseed(42) R1 = { math.random(1 << 40), math.random(1 << 40) } math.randomseed(42) R2 = math.random(1 << 40)"));
    CHECK(Lua(session, "ev_b", "math.randomseed(7) for i = 1, 10 do math.random() end B_FIRST = math.random(0)"));
    CHECK(Lua(session, "ev_a", "assert(R2 == R1[1]) assert(math.random(1 << 40) == R1[2], '别的 Mod 设种子不影响本 Mod 的序列')"));
    CHECK(Lua(session, "ev_b", "math.randomseed(7) for i = 1, 10 do math.random() end assert(math.random(0) == B_FIRST)"));
    CHECK(Lua(session, "basic", "BA = math.random(0)") && Lua(session, "other", "OA = math.random(0)"));
    CopyGlobal(session, "basic", "other", "BA", "BA");
    CHECK(Lua(session, "other", "assert(BA ~= OA, '初始种子每个 Mod 不同')"));

    // A-C4：print 只追加到 4096 字节，不先拼完整串；每个 Mod 每秒的行数有上限，超出的写一行汇总
    g_log.clear();
    CHECK(Lua(session, "ev_a", "local big = string.rep(string.rep('x', 1024), 20 * 1024) print('头', big, '尾') warn(big, big)"));
    std::size_t longest = 0;
    for (const auto& line : g_log) {
        longest = (std::max)(longest, line.size());
    }
    CHECK(longest < 4096 + 64 && CountLog("…（截断）") == 2 && !LogHas("尾"));
    CHECK(Lua(session, "ev_a", "print(string.rep('中', 2000))"));  // 6000 字节的 UTF-8，截在字符边界
    CHECK(g_log.back().size() <= 4096 + 64 && g_log.back().find("…（截断）") != std::string::npos);
    g_log.clear();
    g_now = 10;  // 新的一秒
    CHECK(Lua(session, "ev_b", "for i = 1, 200 do print('第 ' .. i .. ' 行') end print('a\\nb\\nc')"));
    CHECK(CountLog("[ev_b] 第 ") == 50 && LogHas("[ev_b] 第 50 行") && !LogHas("[ev_b] 第 51 行"));
    CHECK(!LogHas("日志限速"));
    g_now = 11.5;
    CHECK(Lua(session, "ev_b", "print('下一秒')"));
    CHECK(LogHas("[ev_b] 日志限速：有 153 行日志没有写入（每个 Mod 每秒最多 50 行") && LogHas("[ev_b] 下一秒"));
    CHECK(Lua(session, "ev_a", "print('别的 Mod 不受影响')") && LogHas("[ev_a] 别的 Mod 不受影响"));
    session.Teardown();
    CHECK(CoreCount() == 0 && session.memory().residual == 0);
}

// B-Y3 和主线的决定：lifecycle 回调（TICK、CampaignLoad 执行 main.lua）里的原生异常由 DDLua 自己接住（plugin.cpp 的
// RunMarked）：标记 VM 损坏、写日志，然后正常返回。「DDCore」（这里是 FirePhase，像 Lifecycle::Fire 一样撤销崩溃的订阅）
// 看不到异常，DDLua 的 lifecycle 订阅都还在；退出存档时丢弃 VM，下次进存档新建
void TestPluginNativeFault()
{
    g_log.clear();
    ResetWorld();
    g_fakeModsAvailable = true;
    g_fakeMods = { { "DDLua Test Fault", "mod_local_source", Fixture("fault") } };
    const std::size_t crashes = g_phaseCrashes;
    const std::size_t loads = PhaseSubs(DD_PHASE_CAMPAIGN_LOAD);
    const std::size_t unloads = PhaseSubs(DD_PHASE_CAMPAIGN_UNLOAD);
    CHECK(loads == 1 && unloads == 1);
    FirePhase(DD_PHASE_CAMPAIGN_LOAD, 41);
    CHECK(LogHas("[fault] main.lua 执行完毕") && TickSubs() == 1);
    FirePhase(DD_PHASE_TICK, 41);
    CHECK(CountLog("[fault] 定时器") == 1);
    g_crashFind = true;
    FirePhase(DD_PHASE_TICK, 41);  // 定时器里调用的服务访问违例
    g_crashFind = false;
    CHECK(g_phaseCrashes == crashes);  // 异常没有传到「DDCore」
    CHECK(LogHas("[DDLua] Tick 时发生原生异常（SEH）：异常码 C0000005") && LogHas("DDLua 已接住，DDCore 的 lifecycle 订阅不受影响"));
    CHECK(LogHas("Lua VM 已损坏（lifecycle 回调（Tick）里执行时发生了原生异常（SEH，异常码 C0000005") && CountLog("Lua VM 已损坏（") == 1);
    CHECK(TickSubs() == 0 && LogHas("没有 Lua VM 了，撤销 lifecycle TICK"));  // 损坏的 VM 不再需要 TICK
    FirePhase(DD_PHASE_TICK, 41);
    CHECK(CountLog("[fault] 定时器") == 2);  // 出事的那一帧打印过一次，之后不再执行
    FirePhase(DD_PHASE_CAMPAIGN_UNLOAD, 41);
    CHECK(LogHas("Lua VM 已损坏，直接丢弃") && TickSubs() == 0 && CoreCount() == 0);
    // 下次进存档新建 VM
    FirePhase(DD_PHASE_CAMPAIGN_LOAD, 42);
    FirePhase(DD_PHASE_TICK, 42);
    CHECK(CountLog("[fault] 定时器") == 3);
    FirePhase(DD_PHASE_CAMPAIGN_UNLOAD, 42);
    CHECK(CountLog("分配器残留 0 字节") == 1 && TickSubs() == 0 && CoreCount() == 0);

    // 执行 main.lua 时的原生异常：修复前 DDCore 撤销 DDLua 的 CampaignLoad 订阅，本次游戏运行不再加载 Lua Mod
    g_log.clear();
    g_fakeMods = { { "DDLua Test Fault Load", "mod_local_source", Fixture("fault_load") } };
    g_crashFind = true;
    FirePhase(DD_PHASE_CAMPAIGN_LOAD, 43);
    g_crashFind = false;
    CHECK(g_phaseCrashes == crashes && PhaseSubs(DD_PHASE_CAMPAIGN_LOAD) == loads && PhaseSubs(DD_PHASE_CAMPAIGN_UNLOAD) == unloads);
    CHECK(LogHas("[DDLua] CampaignLoad 加载 Lua Mod 时发生原生异常（SEH）：异常码 C0000005") && CountLog("Lua VM 已损坏（") == 1);
    CHECK(!LogHas("[fault_load] main.lua 执行完毕") && !LogHas("[fault_load] 加载正常") && TickSubs() == 0);
    FirePhase(DD_PHASE_TICK, 43);
    FirePhase(DD_PHASE_CAMPAIGN_UNLOAD, 43);
    CHECK(LogHas("Lua VM 已损坏，直接丢弃") && CoreCount() == 0 && TickSubs() == 0);
    FirePhase(DD_PHASE_CAMPAIGN_LOAD, 44);  // 订阅还在：下次进存档照常加载，新的 VM
    CHECK(CountLog("[fault_load] main.lua 执行完毕") == 1 && CountLog("[fault_load] 加载正常") == 1 && TickSubs() == 1);
    FirePhase(DD_PHASE_CAMPAIGN_UNLOAD, 44);
    CHECK(CountLog("分配器残留 0 字节") == 1 && TickSubs() == 0 && CoreCount() == 0 && g_phaseCrashes == crashes);
    g_fakeModsAvailable = false;
    g_fakeMods.clear();
}

// ---------------------------------------------------------------- 第二轮审核 A（G1）

// 临时 Mod 目录（%TEMP% 下），析构时删掉
struct TempMods {
    std::string root;
    std::vector<std::string> dirs;
    explicit TempMods(const char* tag)
    {
        char temp[MAX_PATH];
        GetTempPathA(MAX_PATH, temp);
        root = std::string(temp) + "dd_lua_" + tag + "_" + std::to_string(GetCurrentProcessId());
        CreateDirectoryA(root.c_str(), nullptr);
    }
    static void Write(const std::string& path, const std::string& text)
    {
        FILE* f = nullptr;
        fopen_s(&f, path.c_str(), "wb");
        if (f) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
    }
    ddlua::ModSource Add(const char* id, const std::string& mainLua)
    {
        const std::string dir = root + "\\" + id;
        CreateDirectoryA(dir.c_str(), nullptr);
        CreateDirectoryA((dir + "\\lua").c_str(), nullptr);
        Write(dir + "\\lua\\manifest.json", std::string(R"({"id":")") + id + R"(","version":"1.0.0","api":1})");
        Write(dir + "\\lua\\main.lua", mainLua);
        dirs.push_back(dir);
        return { std::string("临时 ") + id, dir };
    }
    ~TempMods()
    {
        for (const auto& dir : dirs) {
            DeleteFileA((dir + "\\lua\\manifest.json").c_str());
            DeleteFileA((dir + "\\lua\\main.lua").c_str());
            RemoveDirectoryA((dir + "\\lua").c_str());
            RemoveDirectoryA(dir.c_str());
        }
        RemoveDirectoryA(root.c_str());
    }
};

std::size_t LogBytes()
{
    std::size_t n = 0;
    for (const auto& line : g_log) {
        n += line.size();
    }
    return n;
}

// 审核者的语法分析用例：深层嵌套的函数，每层很多局部变量，函数体里每个全局名都要穿过全部层级查找
std::string NestedSource(int levels, int locals, std::size_t bodyBytes, const char* token = "x=x ")
{
    std::string s;
    for (int k = 0; k < levels; ++k) {
        s += "local ";
        for (int i = 0; i < locals; ++i) {
            s += (i ? ",v" : "v") + std::to_string(k) + "_" + std::to_string(i);
        }
        s += "=1 return function() ";
    }
    const std::size_t start = s.size();
    while (s.size() - start < bodyBytes) {
        s += token;
    }
    for (int k = 0; k < levels; ++k) {
        s += "end ";
    }
    return s;
}

// R2A-阻断2：沙箱里只有弱键的表（ephemeron）按强引用表处理，审核者的 40000 项链不再让 GC 原子阶段变成平方级
void TestWeakKeyGc()
{
    g_log.clear();
    ResetHub();
    const auto host = MakeEventHost();
    {
        ddlua::Session session(host, TestConfig(), "弱键");
        session.Load(Sources({ "ev_a", "ev_b" }));
        CHECK(Lua(session, "ev_a", "W = setmetatable({}, { __mode = 'k' }) local keys = {} for i = 1, 40000 do keys[i] = {} end "
                                   "for i = 1, 39999 do W[keys[i]] = keys[i + 1] end ROOT = keys[1]"));
        double worst = 0;
        int failed = 0;
        for (int i = 0; i < 4; ++i) {
            std::string error;
            const auto start = std::chrono::steady_clock::now();
            failed += session.RunChunk(*session.FindMod("ev_b"), "for i = 1, 200000 do local t = {} end", "=alloc", &error) ? 0 : 1;
            worst = (std::max)(worst, Ms(start));
        }
        auto start = std::chrono::steady_clock::now();
        session.vm()->FullGc();
        const double full = Ms(start);
        std::printf("  弱键链 40000 项：别的 Mod 分配 20 万个临时表最长 %.1f ms（%d 次超预算），FullGc %.1f ms（审核者测的修复前 2714 ms、5424 ms）\n",
            worst, failed, full);
        CHECK(failed == 0 && full < 200);
        // 语义：只有弱键的表，键不回收；'v'、'kv' 照常
        CHECK(Lua(session, "ev_a", "W = nil ROOT = nil WK = setmetatable({}, { __mode = 'k' }) WK[{}] = 1 "
                                   "WV = setmetatable({}, { __mode = 'v' }) WV[1] = {} "
                                   "WKV = setmetatable({}, { __mode = 'kv' }) WKV[{}] = 1 WKV[2] = {} "
                                   "WK2 = setmetatable({}, { __mode = ' k ' }) WK2[{}] = 1"));
        session.vm()->FullGc();
        CHECK(Lua(session, "ev_a", "assert(next(WK) ~= nil, '只有弱键的表按强引用处理，键不回收') "
                                   "assert(next(WK2) ~= nil, '__mode 里带别的字符也一样') "
                                   "assert(WV[1] == nil, '弱值照常回收') assert(next(WKV) == nil, 'kv 照常回收')"));
        session.Teardown();
        CHECK(session.memory().residual == 0);
    }
    {
        // 大堆的 GC 停顿（不受预算约束，只记实测数据，写进 03）：约 60 MB 的小表，Mod 一边分配一边改大表
        auto config = TestConfig();
        config.memoryLimitMb = 128;
        config.budgetMs = 5000;
        ddlua::Session session(host, config, "大堆");
        session.Load(Sources({ "ev_a" }));
        CHECK(Lua(session, "ev_a", "BIG = {} for i = 1, 600000 do BIG[i] = { i } end"));
        CHECK(Lua(session, "ev_a", R"(
            local clock = os.clock
            local last, worst = clock(), 0
            for i = 1, 3000000 do
                BIG[i % 600000 + 1] = { i }
                if i % 256 == 0 then local now = clock() if now - last > worst then worst = now - last end last = now end
            end
            dd.log.info(string.format('大堆：600000 个小表，一边分配一边改，256 次分配之间最长 %.1f ms，内存 %.1f MB', worst * 1000, collectgarbage('count') / 1024))
        )"));
        auto start = std::chrono::steady_clock::now();
        session.vm()->FullGc();
        std::printf("  %s；FullGc %.1f ms\n", g_log.back().c_str(), Ms(start));
        session.Teardown();
    }
}

// R2A-阻断3 和备注4：main.lua 的读取和语法分析在 load_budget_ms 的预算入口里；load 每块交给语法分析的源码更小
void TestParseBudget()
{
    g_log.clear();
    TempMods temp("parse_budget");
    const auto big = temp.Add("rv_bigmain", NestedSource(90, 190, 1024 * 1024));
    const auto host = MakeHost();
    {
        auto config = TestConfig();  // load_budget_ms = 200
        ddlua::Session session(host, config, "大 main.lua");
        const auto start = std::chrono::steady_clock::now();
        session.Load({ big });
        const double ms = Ms(start);
        std::printf("  审核者的 1.1 MB 深层嵌套 main.lua：Session::Load %.1f ms（load_budget_ms = %u，审核者测的修复前 3738 ms 并加载成功）\n", ms,
            config.loadBudgetMs);
        CHECK(session.LoadedCount() == 0 && LogHas("[rv_bigmain] main.lua 超出预算，加载失败"));
        CHECK(ms < config.loadBudgetMs + 150);
        CHECK(CountLog("[rv_bigmain] stack traceback:") == 1);  // 语法分析里的错误只加一次 traceback
        session.Teardown();
    }
    {
        // Mod 代码里的 load：两次检查之间最多分析一块
        ddlua::Session session(host, TestConfig(), "load");
        session.Load(Sources({ "sandbox" }));
        ddlua::Mod* mod = session.FindMod("sandbox");
        for (const char* token : { "x=x ", "f(x,x,x,x,x,x,x,x,x,x) " }) {
            std::string error;
            CHECK(session.vm()->Protected(
                [&](lua_State* L) {
                    const std::string src = NestedSource(84, 190, 1024 * 1024, token);
                    lua_rawgeti(L, LUA_REGISTRYINDEX, mod->envRef);
                    lua_pushlstring(L, src.data(), src.size());
                    lua_setfield(L, -2, "PSRC");
                    lua_pop(L, 1);
                },
                &error));
            bool budget = false;
            const auto start = std::chrono::steady_clock::now();
            const bool ok = session.RunChunk(*mod, "local f, e = load(PSRC) assert(f, e)", "=psrc", &error, &budget);
            const double ms = Ms(start);
            std::printf("  load 1 MB 的深层嵌套源码（函数体 '%s'）：%.1f ms 被打断（预算 50 ms，审核者测的修复前 64.6 ms，每 64 KB 约 249 ms）\n", token,
                ms);
            CHECK(!ok && budget && ms < 90);
            CHECK(session.RunChunk(*mod, "PSRC = nil", "=free", &error));
        }
        session.Teardown();
    }
}

// R2A-应修1：错误信息、traceback、dd.on / dd.wait_event 的失败原因截断，并计入按 Mod 的限速
void TestErrorLogCaps()
{
    g_log.clear();
    ResetHub();
    const auto host = MakeEventHost();
    auto config = TestConfig();
    ddlua::Session session(host, config, "错误日志");
    g_now = 0;
    session.SetClock([] { return g_now; });
    session.Load(Sources({ "ev_a", "ev_b" }));
    std::size_t before = LogBytes();
    CHECK(Lua(session, "ev_a", "local ok, why = dd.on(string.rep('y', 8000000), print) assert(ok == nil and #why < 1024, #why)"));
    std::printf("  dd.on(8 MB 的主题名)：日志增加 %zu 字节（审核者测的修复前 8000041）\n", LogBytes() - before);
    CHECK(LogBytes() - before < 2048);
    before = LogBytes();
    CHECK(Lua(session, "ev_a", "dd.spawn(function() local ok, why = dd.wait_event(string.rep('q', 8000000)) assert(ok == nil and #why < 1024, #why) end)"));
    CHECK(LogBytes() - before < 2048);
    // 回调的错误对象 1 MB：一个出错的定时器
    CHECK(Lua(session, "ev_a", "dd.every(0, function() error(string.rep('z', 1000000)) end)"));
    before = LogBytes();
    Tick(session, 1);
    std::printf("  定时器 error(1 MB)：一次失败日志增加 %zu 字节\n", LogBytes() - before);
    CHECK(LogBytes() - before < 16 * 1024);
    // 一次错误带 20 万个换行
    CHECK(Lua(session, "ev_b", "dd.every(0, function() error(string.rep('w\\n', 200000)) end)"));
    std::size_t lines = g_log.size();
    Tick(session, 3);
    std::printf("  error 带 20 万个换行：一次失败写了 %zu 行（log_lines_per_sec = %u，审核者测的修复前 200036 行）\n", g_log.size() - lines,
        config.logLinesPerSec);
    CHECK(g_log.size() - lines <= config.logLinesPerSec + 8);
    session.Teardown();
    // main.lua 出错：错误对象同样截断
    g_log.clear();
    TempMods temp("error_caps");
    const auto bad = temp.Add("rv_bad", "error(string.rep('m', 1000000))");
    ddlua::Session s2(host, config, "main.lua 出错");
    s2.Load({ bad });
    CHECK(LogHas("[rv_bad] 加载失败") && LogBytes() < 16 * 1024);
    s2.Teardown();
    // 会话的总量（log_max_mb）：DDLua 替 Mod 写的错误行也算在里面，到了以后不再写入，拆除时写一行丢弃的行数
    g_log.clear();
    ResetHub();
    auto capped = TestConfig();
    capped.logMaxMb = 1;
    capped.logLinesPerSec = 100000;
    ddlua::Session s3(host, capped, "总量");
    g_now = 0;
    s3.SetClock([] { return g_now; });
    s3.Load(Sources({ "ev_a" }));
    CHECK(Lua(s3, "ev_a", "for i = 1, 200 do print(string.rep('p', 4000)) end"));
    CHECK(Lua(s3, "ev_a", "for i = 1, 200 do dd.every(0, function() error(string.rep('e', 4000)) end) end"));
    Tick(s3, 1);
    Tick(s3, 2);
    const std::size_t written = LogBytes();
    std::printf("  log_max_mb = 1：写了 %zu 字节\n", written);
    CHECK(written < 1024 * 1024 + 64 * 1024 && CountLog("到了 log_max_mb") == 1);
    s3.Teardown();
    CHECK(LogHas("本会话因为 log_max_mb 有 "));
}

// R2A-应修2：dd.once 的回调（或回调里先撤销了自己）出原生异常时没有撤销通知，DDLua 的事件跳板用 SEH 过滤器立即标记损坏
void TestOnceNativeFault()
{
    for (const bool selfOff : { false, true }) {
        g_log.clear();
        ResetHub();
        ResetWorld();
        const auto host = MakeActorHost();
        ddlua::Session session(host, TestConfig(), selfOff ? "先撤销自己" : "dd.once");
        session.Load(Sources({ "ev_a", "ev_b" }));
        CHECK(Lua(session, "ev_a", selfOff ? "SUB = dd.on('app.tick', function() SUB:off() dd.actor.hero(666) end)"
                                           : "dd.once('app.tick', function() dd.actor.hero(666) end)"));
        CHECK(Lua(session, "ev_b", "dd.on('app.tick', function() print('ev_b 事件') end, { priority = 10 })"));
        g_crashFind = true;
        Fire t1("app.tick");
        t1.Go();
        g_crashFind = false;
        std::printf("  %s：崩溃之后 Broken=%d，同一次派发里 ev_b 运行 %zu 次（审核者测的修复前 Broken=0、1 次）\n",
            selfOff ? "回调里先 SUB:off()" : "dd.once", session.Broken() ? 1 : 0, CountLog("ev_b 事件"));
        CHECK(session.Broken() && CountLog("ev_b 事件") == 0);
        CHECK(CountLog("Lua VM 已损坏（") == 1);
        Fire t2("app.tick");
        t2.Go();
        std::string error;
        CHECK(CountLog("ev_b 事件") == 0 && !session.RunChunk(*session.FindMod("ev_b"), "print('ev_b 事件')", "=dead", &error));
        session.Teardown();
        CHECK(LogHas("Lua VM 已损坏，直接丢弃") && CoreCount() == 0);
    }
}

// R2A-备注1：dd.import 的代理按（使用方、提供方）缓存
void TestImportCache()
{
    g_log.clear();
    ResetHub();
    const auto host = MakeEventHost();
    ddlua::Session session(host, TestConfig(), "导入");
    session.Load(Sources({ "basic", "other", "ev_a" }));
    CHECK(Lua(session, "other", "local a, b = dd.import('basic'), dd.import('basic') assert(a == b and rawequal(a, b), '同一个使用方拿到同一个代理') "
                                "local t = {} t[a] = 1 assert(t[dd.import('basic')] == 1) rawset(a, 'value', 7) OTHER_P = a"));
    CopyGlobal(session, "other", "ev_a", "OTHER_P", "OTHER_P");
    CHECK(Lua(session, "ev_a", "local p = dd.import('basic') assert(p ~= OTHER_P, '不同的使用方各自一份') "
                               "assert(p.value == 42 and OTHER_P.value == 7, 'rawset 只改到那个使用方自己的代理')"));
    session.Teardown();
}

// R2A-备注3：一次拆除或重载里 <close> 的总时间有上限，超过后剩下的协程直接丢弃
void TestTeardownCloseCap()
{
    const char* spawn40 = "for i = 1, 40 do dd.spawn(function() local c <close> = setmetatable({}, { __close = function() "
                          "CLOSED = (CLOSED or 0) + 1 local t = os.clock() while os.clock() - t < 0.04 do end end }) dd.wait(1000) end) end";
    g_log.clear();
    ResetHub();
    const auto host = MakeEventHost();
    {
        ddlua::Session session(host, TestConfig(), "拆除上限");
        session.Load(Sources({ "ev_a" }));
        CHECK(Lua(session, "ev_a", spawn40));
        const auto start = std::chrono::steady_clock::now();
        session.Teardown();
        const double ms = Ms(start);
        std::printf("  拆除：40 个协程的 <close> 各忙 40 ms，用时 %.1f ms（审核者测的修复前 1611 ms）\n", ms);
        CHECK(ms < 600 && LogHas("没有关闭") && session.memory().residual == 0);
    }
    {
        g_log.clear();
        auto config = TestConfig();
        config.devMode = true;
        ddlua::Session session(host, config, "重载上限");
        g_now = 0;
        session.SetClock([] { return g_now; });
        session.Load(Sources({ "ev_a" }));
        CHECK(Lua(session, "ev_a", spawn40));
        CHECK(Lua(session, "ev_a", "assert(dd.reload())"));
        const auto start = std::chrono::steady_clock::now();
        Tick(session, 1);
        const double ms = Ms(start);
        std::printf("  重载：同样的 40 个协程，执行重载的那一帧 %.1f ms（修复前 1608 ms）\n", ms);
        CHECK(ms < 600 && LogHas("没有关闭") && LogHas("重载 ev_a："));
        session.Teardown();
        CHECK(session.memory().residual == 0);
    }
}

// R2A-备注5：回引用指向位置捕获时，计步不能是负数
void TestPositionCaptureStep()
{
    lua_State* L = luaL_newstate();
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    const auto saved = lua_ddstep_fn;
    lua_ddstep_fn = nullptr;
    const auto delta = [&](const char* pattern) {
        lua_ddstep_work = 1000;
        lua_getglobal(L, LUA_STRLIBNAME);
        lua_getfield(L, -1, "find");
        lua_pushliteral(L, "a");
        lua_pushstring(L, pattern);
        lua_call(L, 2, 0);
        lua_pop(L, 1);
        return static_cast<long long>(lua_ddstep_work) - 1000;
    };
    const long long plain = delta("()");
    const long long backref = delta("()%1");
    std::printf("  计步：string.find('a', '()') %lld，'()%%1' %lld\n", plain, backref);
    CHECK(backref > plain);
    lua_ddstep_fn = saved;
    lua_close(L);
}

// ---------------------------------------------------------------- 第三轮审核 R3A
// 审核者的标准：用例要在「预算加 100 ms」以内结束（打断或者本来就很快），修复前它们卡几百毫秒到几秒，有的还被判为成功。
// 复现脚本和修复前的数据在 guest 的 build/review3_a/

struct R3Case {
    const char* name;
    const char* prep;  // 在另一个入口里准备数据（可为空）
    const char* code;
};

enum class R3Expect { Interrupted, Finished, Either };

// 跑一个用例，返回是否符合预期。Interrupted：因超预算失败；Finished：成功；Either：两者都行。三种都要求用时在 limitMs 以内
bool RunR3(ddlua::Session& session, ddlua::Mod& mod, const R3Case& c, R3Expect expect, double limitMs, double* elapsedOut = nullptr)
{
    std::string error;
    if (c.prep && !session.RunChunk(mod, c.prep, "=prep", &error, nullptr, 5000)) {
        std::printf("    准备失败：%s\n", error.substr(0, 200).c_str());
    }
    bool budget = false;
    const auto start = std::chrono::steady_clock::now();
    const bool ok = session.RunChunk(mod, c.code, "=r3", &error, &budget);
    const double elapsed = Ms(start);
    if (elapsedOut) {
        *elapsedOut = elapsed;
    }
    const bool interrupted = !ok && budget && error.find("执行超出预算") != std::string::npos;
    bool pass = elapsed < limitMs;
    switch (expect) {
    case R3Expect::Interrupted:
        pass = pass && interrupted;
        break;
    case R3Expect::Finished:
        pass = pass && ok;
        break;
    case R3Expect::Either:
        pass = pass && (ok || interrupted);
        break;
    }
    std::printf("  %s %s（%.0f ms，%s）%s\n", pass ? "PASS" : "FAIL", c.name, elapsed, ok ? "成功" : (interrupted ? "超预算" : "出错"),
        pass || ok ? "" : (" —— " + error.substr(0, 160)).c_str());
    return pass;
}

void FreeR3(ddlua::Session& session, ddlua::Mod& mod)
{
    std::string error;
    session.RunChunk(mod, "DATA = nil DATA2 = nil DATA3 = nil HT = nil SRC = nil T = nil Z = nil", "=free", &error, nullptr, 5000);
    session.vm()->FullGc();
}

// R3A-阻断1：整数键、浮点键的哈希没有随机化，Mod 能构造全部落在同一个主位置的键；扩容时 reinsert 逐个查找再插入，是平方级
void TestHashFlood()
{
    g_log.clear();
    const auto host = MakeHost();
    auto config = TestConfig();
    config.memoryLimitMb = 512;
    ddlua::Session session(host, config, "哈希冲突");
    session.Load(Sources({ "sandbox" }));
    ddlua::Mod* mod = session.FindMod("sandbox");
    CHECK(mod);
    if (!mod) {
        return;
    }
    const double limit = config.budgetMs + 100.0;
    const R3Case finished[] = {
        // 审核者：3829 ms，判为成功
        { "整数键 i*(2^17-1)，n=65537，建一次表", nullptr, "local M = (1 << 17) - 1 local t = {} for i = 1, 65537 do t[i * M] = true end" },
        // 原来的 l_hashfloat 只看指数和尾数的高 31 位，1 + i*2^-50 全部落在同一个主位置
        { "浮点键 1+i*2^-50，n=65537，建一次表", nullptr, "local d = 2.0^-50 local t = {} for i = 1, 65537 do t[1 + i * d] = true end" },
        // 审核者：961 ms，判为成功（语法分析的常量表 ls->h 也是 Lua 表）
        { "load('return {M,2M,...}')，32769 个常量，一次",
            "local M = (1 << 16) - 1 local p = {} for i = 1, 32769 do p[i] = tostring(i * M) end SRC = 'return {' .. table.concat(p, ',') .. '}'",
            "local f = assert(load(SRC)) assert(#f() == 32769)" },
    };
    for (const auto& c : finished) {
        CHECK(RunR3(session, *mod, c, R3Expect::Finished, limit));
    }
    const R3Case loops[] = {
        { "整数键 i*(2^17-1)，n=65537，反复建表", nullptr,
            "local M = (1 << 17) - 1 while true do local t = {} for i = 1, 65537 do t[i * M] = true end end" },
        { "浮点键 1+i*2^-50，n=65537，反复建表", nullptr,
            "local d = 2.0^-50 while true do local t = {} for i = 1, 65537 do t[1 + i * d] = true end end" },
        // 审核者：1030 ms（每次读最多跳 2000 次 __index，每跳走一遍冲突链）
        { "n=8193 的冲突表，__index 指向自己，pcall 里反复读",
            "local M = (1 << 14) - 1 HT = {} for i = 1, 8193 do HT[i * M] = true end setmetatable(HT, { __index = HT })",
            "local f = function() return HT[0] end while true do pcall(f) end" },
        { "load('return {M,2M,...}')，32769 个常量，反复",
            "local M = (1 << 16) - 1 local p = {} for i = 1, 32769 do p[i] = tostring(i * M) end SRC = 'return {' .. table.concat(p, ',') .. '}'",
            "while true do assert(load(SRC)) end" },
    };
    for (const auto& c : loops) {
        CHECK(RunR3(session, *mod, c, R3Expect::Interrupted, limit));
        FreeR3(session, *mod);
    }

    // 语义：各种键照常存取、遍历，删除和扩容、数组部分和哈希部分之间的迁移
    CHECK(Lua(session, "sandbox", R"(
        local t, n = {}, 0
        local function put(k, v) if t[k] == nil then n = n + 1 end t[k] = v end
        for i = 1, 5000 do put(i * 65535, i) put(1 + i * 2.0^-50, -i) put(-i, i) put(i, i) end
        local keys = {} for i = 1, 500 do keys[i] = {} put(keys[i], i) end
        local f = function() end
        put(true, 'T') put(false, 'F') put(1/0, 'inf') put(-1/0, '-inf') put(2^63, 'big') put(math.mininteger, 'min')
        put(math.maxinteger, 'max') put(0.5, 'half') put(f, 'fn') put(print, 'cfn') put('s', 'str') put(string.rep('L', 100), 'long')
        for i = 1, 5000 do assert(t[i * 65535] == i and t[1 + i * 2.0^-50] == -i and t[-i] == i and t[i] == i) end
        for i = 1, 500 do assert(t[keys[i]] == i) end
        assert(t[true] == 'T' and t[false] == 'F' and t[1/0] == 'inf' and t[-1/0] == '-inf' and t[2^63] == 'big')
        assert(t[math.mininteger] == 'min' and t[-2^63] == 'min' and t[math.maxinteger] == 'max' and t[0.5] == 'half')
        assert(t[f] == 'fn' and t[print] == 'cfn' and t.s == 'str' and t[string.rep('L', 100)] == 'long')
        assert(t[3.0] == 3 and t[2^53] == nil and t[0] == nil and t[{}] == nil and t[0.25] == nil)
        local c = 0 for k, v in pairs(t) do c = c + 1 assert(t[k] == v) end
        assert(c == n, '遍历 ' .. c .. ' 项，应为 ' .. n)
        local i = 0 for k in pairs(t) do i = i + 1 if i % 2 == 0 then t[k] = nil end end
        local c2 = 0 for k, v in pairs(t) do c2 = c2 + 1 assert(v ~= nil) end
        assert(c2 == n - n // 2, '删除一半后遍历 ' .. c2)
        for k = 1, 8000 do t[k * 7 + 0.5] = k end
        for k = 1, 8000 do assert(t[k * 7 + 0.5] == k) end
        local a = {} for k = 1, 20000 do a[k] = k end
        for k = 1, 20000, 3 do a[k] = nil end
        for k = 20001, 30000 do a[k] = k end
        for k = 1, 30000 do local v = a[k] if k <= 20000 and k % 3 == 1 then assert(v == nil) else assert(v == k) end end
        local b = {} for k = 1, 1000 do b[#b + 1] = k end
        assert(#b == 1000) table.remove(b, 1) assert(#b == 999 and b[1] == 2)
        local s = {} for k = 1, 200 do s[k] = (k * 7919) % 1009 end table.sort(s) for k = 2, 200 do assert(s[k - 1] <= s[k]) end
    )"));
    // 每张表的整数键位置不同（种子混入了表自己的地址）：同样的键按同样的顺序放进两张表，遍历顺序不同
    CHECK(Lua(session, "sandbox", R"(
        local function order(t) local s = {} for k in pairs(t) do s[#s + 1] = tostring(k) end return table.concat(s, ',') end
        local same = 0
        for r = 1, 8 do
            local a, b = {}, {}
            for i = 1, 64 do a[i * 1000] = true b[i * 1000] = true end
            if order(a) == order(b) then same = same + 1 end
        end
        assert(same < 8, '整数键的位置和表无关：哈希没有加种子')
    )"));
    session.Teardown();
    CHECK(session.memory().residual == 0);

    // main.lua 就是常量冲突的表（审核者：65537 个常量、712 KB，Session::Load 3846 ms 后加载失败）
    {
        TempMods temp("r3hash");
        std::string src = "return {";
        for (long long i = 1; i <= 65537; ++i) {
            src += std::to_string(i * ((1LL << 17) - 1)) + ",";
        }
        src += "}";
        const auto source = temp.Add("r3_hashmain", src);
        g_log.clear();
        ddlua::Session s2(host, TestConfig(), "哈希冲突 main.lua");
        const auto start = std::chrono::steady_clock::now();
        s2.Load({ source });
        const double ms = Ms(start);
        const double loadLimit = TestConfig().loadBudgetMs + 100.0;
        std::printf("  %s main.lua = return {M,2M,...}（65537 个常量，%zu 字节）：Session::Load %.0f ms，加载%s\n",
            ms < loadLimit ? "PASS" : "FAIL", src.size(), ms, s2.LoadedCount() == 1 ? "成功" : "失败");
        CHECK(ms < loadLimit);
        CHECK(s2.LoadedCount() == 1);
        s2.Teardown();
    }
#if defined(LUA_DDPATCH) && LUA_DDPATCH >= 6
    // 字符串哈希的种子来自系统随机数（lstate.c 的补丁），数字和指针键的密钥每个进程随机生成一次
    CHECK(lua_ddseed_fn != nullptr);
    if (lua_ddseed_fn) {
        const unsigned a = lua_ddseed_fn(), b = lua_ddseed_fn(), c = lua_ddseed_fn();
        CHECK(!(a == b && b == c));
    }
    // 核对审核备注 1：只查「非零」时，密钥还是 ltable.c 的默认常量（InitLuaPatches 没被调用）也能通过。和默认常量逐个比较，
    // 三个都是系统随机数时和常量相同的概率是 2^-64
    constexpr unsigned long long kDefaultHashKey[3] = { 0xa0761d6478bd642fULL, 0xe7037ed1a0b428dbULL, 0x8ebc6af09c88c6e3ULL };
    for (int i = 0; i < 3; ++i) {
        CHECK(lua_ddhashkey[i] != kDefaultHashKey[i]);
    }
#endif
}

// R3A-阻断2：l_str2int 跳过任意多的空白、十六进制数字、前导零都不计步；tonumber(s, base) 的 strspn 也一样
void TestStr2NumSteps()
{
    g_log.clear();
    const auto host = MakeHost();
    auto config = TestConfig();
    config.memoryLimitMb = 512;
    ddlua::Session session(host, config, "字符串转数字");
    session.Load(Sources({ "sandbox" }));
    ddlua::Mod* mod = session.FindMod("sandbox");
    CHECK(mod);
    if (!mod) {
        return;
    }
    const double limit = config.budgetMs + 100.0;
    const char* spaces1 = "DATA = string.rep(string.rep(' ', 4096), 8192) .. '1'";
    const R3Case cases[] = {
        // 审核者的数据（32 MB 的串）：1805、1809、1445、1450、1826、5864 ms
        { "tonumber(32 MB 空白 .. '1')", spaces1, "while true do tonumber(DATA) end" },
        { "DATA + 0（字符串算术）", spaces1, "while true do local _ = DATA + 0 end" },
        { "math.abs(DATA)（库函数参数）", spaces1, "while true do local _ = math.abs(DATA) end" },
        { "('x'):rep(DATA)（整数参数）", "DATA = string.rep(string.rep(' ', 4096), 8192) .. '0'", "while true do local _ = ('x'):rep(DATA) end" },
        { "tonumber('1' .. 32 MB 空白)（尾部空白）", "DATA = '1' .. string.rep(string.rep(' ', 4096), 8192)", "while true do tonumber(DATA) end" },
        { "tonumber('0x' .. 32 MB 'f')", "DATA = '0x' .. string.rep(string.rep('f', 4096), 8192)", "while true do tonumber(DATA) end" },
        // 下面两个是排查时补的：十进制的前导零不会溢出；tonumber(s, base) 用 strspn 跳过空白
        { "tonumber(32 MB '0' .. '1')（前导零）", "DATA = string.rep(string.rep('0', 4096), 8192) .. '1'", "while true do tonumber(DATA) end" },
        { "tonumber(32 MB 空白 .. '1', 10)", spaces1, "while true do tonumber(DATA, 10) end" },
    };
    for (const auto& c : cases) {
        CHECK(RunR3(session, *mod, c, R3Expect::Interrupted, limit));
        FreeR3(session, *mod);
    }
    CHECK(Lua(session, "sandbox", R"(
        assert(tonumber('  12  ') == 12 and ('  3') + 1 == 4 and tonumber('0x10') == 16 and tonumber(' -0x10 ') == -16)
        assert(tonumber(' 0x1p4 ') == 16.0 and tonumber('1e2') == 100.0 and tonumber('  .5  ') == 0.5 and tonumber('0x.8') == 0.5)
        assert(tonumber(' 10 ', 16) == 16 and tonumber('z', 36) == 35 and tonumber(' -ff ', 16) == -255)
        assert(tonumber('') == nil and tonumber('0x') == nil and tonumber('1 2') == nil and tonumber('1e') == nil and tonumber(' ') == nil)
        assert(tonumber('inf') == nil and tonumber('nan') == nil and tonumber('9223372036854775807') == math.maxinteger)
        assert(math.type(tonumber('9223372036854775808')) == 'float' and tonumber('0x7fffffffffffffff') == math.maxinteger)
        assert(tonumber('000000000000000000000000000001') == 1 and ('x'):rep(' 3 ') == 'xxx')
    )"));
}

// R3A-阻断3：没有 lua_State 的进度回调要落在真正在运行的线程上（协程开始运行、让出后回到恢复方、嵌套、coroutine.close）
void TestNolThreads()
{
    g_log.clear();
    ResetHub();
    const auto host = MakeEventHost();
    auto config = TestConfig();
    config.memoryLimitMb = 512;
    ddlua::Session session(host, config, "nol 和协程");
    g_now = 0;
    session.SetClock([] { return g_now; });
    session.Load(Sources({ "ev_a" }));
    ddlua::Mod* mod = session.FindMod("ev_a");
    CHECK(mod);
    if (!mod) {
        return;
    }
    const double limit = config.budgetMs + 100.0;
    std::string prepError;
    CHECK(session.RunChunk(*mod, "DATA = string.rep(string.rep('a', 4096), 8192) DATA2 = string.rep(string.rep('a', 4096), 8192)", "=prep",
        &prepError, nullptr, 5000));
    const R3Case cases[] = {
        // 审核者的 corono.out：主线程 53 ms，下面依次 431、445、557、2331、417 ms
        { "主线程反复 ==（对照）", nullptr, "while true do local _ = DATA == DATA2 end" },
        { "coroutine.wrap 里反复 ==", nullptr, "coroutine.wrap(function() while true do local _ = DATA == DATA2 end end)()" },
        { "dd.spawn 里反复 ==", nullptr, "dd.spawn(function() while true do local _ = DATA == DATA2 end end)" },
        { "协程里展开的 ==", nullptr,
            "coroutine.wrap(function() local a, b = DATA, DATA2 while true do local _ = a == b == (a == b) == (a == b) == (a == b) end end)()" },
        { "协程里反复 <", nullptr, "coroutine.wrap(function() while true do local _ = DATA < DATA2 end end)()" },
        { "协程先跑满一次 hook 再让出，主线程接着比较", nullptr,
            "local co = coroutine.wrap(function() for i = 1, 3000 do end coroutine.yield() end) co() while true do local _ = DATA == DATA2 end" },
        // 下面是补的：嵌套协程、让出后回到外层协程、coroutine.close 运行 <close>
        { "嵌套协程里反复 ==", nullptr,
            "coroutine.wrap(function() coroutine.wrap(function() while true do local _ = DATA == DATA2 end end)() end)()" },
        { "内层协程让出后，外层协程接着比较", nullptr,
            "coroutine.wrap(function() local inner = coroutine.wrap(function() for i = 1, 3000 do end coroutine.yield() end) inner() "
            "while true do local _ = DATA == DATA2 end end)()" },
        { "coroutine.close 时 <close> 里反复 ==", nullptr,
            "local co = coroutine.create(function() local x <close> = setmetatable({}, { __close = function() while true do local _ = DATA == DATA2 end end }) "
            "coroutine.yield() end) coroutine.resume(co) coroutine.close(co)" },
    };
    for (const auto& c : cases) {
        CHECK(RunR3(session, *mod, c, R3Expect::Interrupted, limit));
    }
    // 调度器从 C++ 恢复 dd.spawn 的协程（Tick 里）
    g_log.clear();
    CHECK(Lua(session, "ev_a", "dd.spawn(function() dd.wait(0) for i = 1, 3000 do end while true do local _ = DATA == DATA2 end end)"));
    const auto start = std::chrono::steady_clock::now();
    Tick(session, 1);
    const double tickMs = Ms(start);
    const bool tickOk = tickMs < limit && LogHas("协程超出执行预算");
    std::printf("  %s 调度器在 Tick 里恢复协程，协程里反复 ==（%.0f ms）\n", tickOk ? "PASS" : "FAIL", tickMs);
    CHECK(tickOk);
    std::string error;
    const auto next = std::chrono::steady_clock::now();
    CHECK(session.RunChunk(*mod, "local x = 0 for i = 1, 100000 do x = x + i end", "=next", &error));
    CHECK(Ms(next) < 50);
    FreeR3(session, *mod);
    session.Teardown();
    CHECK(session.memory().residual == 0);
}

// R3A-应修1：内存到上限、而且都是活对象时，每次分配失败都先做一次完整的紧急 GC（审核者：一个回调卡 1619 ms）
void TestEmergencyGc()
{
    g_log.clear();
    ResetHub();
    const auto host = MakeEventHost();
    auto config = TestConfig();
    config.memoryLimitMb = 128;
    ddlua::Session session(host, config, "紧急 GC");
    g_now = 0;
    session.SetClock([] { return g_now; });
    session.Load(Sources({ "ev_a" }));
    ddlua::Mod* mod = session.FindMod("ev_a");
    CHECK(mod);
    if (!mod) {
        return;
    }
    ddlua::Vm* vm = session.vm();
    // 对照（修复前的行为）：环境变量 DD_LUA_AB=1 时去掉 lmem.c 补丁的回调，tryagain 和原版一样每次失败都做完整 GC
    char ab[8] = {};
    const bool compare = GetEnvironmentVariableA("DD_LUA_AB", ab, sizeof(ab)) > 0 && ab[0] == '1';
    if (compare) {
        lua_ddemergency_fn = nullptr;
        std::printf("  （对照：DD_LUA_AB=1，不限制紧急 GC，等于修复前）\n");
    }
    // DROP 定时器在 dd.time() 到 5 秒时放掉引用：到上限以后编译新的代码块也要分配，只能用事先登记的回调
    CHECK(Lua(session, "ev_a", "F = function() local t = {} end G = function() HEAD = { HEAD } end "
                               "LOOPFN = function() local f, pc = F, pcall while true do pc(f) end end "
                               "dd.every(0, function() if dd.time() >= 5 then BIG = nil HEAD = nil end end)"));
    std::string error;
    for (int round = 0; round < 400 && vm->MemoryUsed() < 126u * 1024 * 1024; ++round) {
        session.RunChunk(*mod, "BIG = BIG or {} local n = #BIG for i = n + 1, n + 20000 do BIG[i] = { i } end", "=fill", &error);
    }
    // 一次完整 GC 的时间（这时几乎都是活对象）
    auto start = std::chrono::steady_clock::now();
    lua_gc(vm->L(), LUA_GCCOLLECT);
    const double gcMs = Ms(start);
    // 审核者的用例：定时器回调里反复分配（先登记，填满之后的那一帧运行）。登记三个，模拟同一帧里几个 Mod 的回调
    // （到上限以后编译新的代码块也要分配，所以不用 RunChunk 测）
    CHECK(Lua(session, "ev_a", "for i = 1, 3 do dd.after(0, LOOPFN) end"));
    // 填到分配失败为止（最后一次分配在紧急 GC 之后仍然失败），没有可回收的垃圾
    for (int k = 0; k < 50; ++k) {
        if (!session.RunChunk(*mod, "while pcall(G) do end", "=fill2", &error) || vm->MemoryUsed() + 4096 > vm->MemoryLimit()) {
            break;
        }
    }
    std::printf("  Lua 内存 %.3f MB / %u MB，完整 GC 一次 %.0f ms\n", vm->MemoryUsed() / 1048576.0, config.memoryLimitMb, gcMs);
    Sleep(1100);  // 等过填充时那次紧急 GC 的冷却时间：这一帧的第一个回调照常可以做一次
    const std::uint64_t gcsBefore = vm->EmergencyGcs();
    start = std::chrono::steady_clock::now();
    Tick(session, 1);
    const double tickMs = Ms(start);
    const std::uint64_t gcs = vm->EmergencyGcs() - gcsBefore;
    // 每个回调一份预算，三个回调里最多一次完整 GC（第一个回调做了仍然失败，之后冷却）
    const double limit = 3.0 * config.budgetMs + gcMs + 100.0;
    const bool tickOk = tickMs < limit && gcs <= 1 && LogHas("超出执行预算");
    std::printf("  %s 三个定时器回调里反复分配，那一帧 Tick %.0f ms（上限 %.0f ms），紧急 GC %llu 次、跳过共 %llu 次\n", tickOk ? "PASS" : "FAIL",
        tickMs, limit, static_cast<unsigned long long>(gcs), static_cast<unsigned long long>(vm->EmergencySkips()));
    CHECK(tickOk);
    // 放掉引用之后还能恢复：DROP 定时器放掉引用，等过紧急 GC 的冷却时间，编译下一个代码块时分配失败、紧急 GC 回收
    Sleep(1100);
    Tick(session, 5);
    CHECK(Lua(session, "ev_a", "assert(BIG == nil) local t = {} for i = 1, 100000 do t[i] = { i } end"));
    std::printf("  放掉引用后：Lua 内存 %.1f MB\n", vm->MemoryUsed() / 1048576.0);
    CHECK(vm->MemoryUsed() < 64u * 1024 * 1024);
    session.Teardown();
    CHECK(session.memory().residual == 0);
    if (compare) {
        ddlua::Vm restore(1024 * 1024, nullptr);  // 构造函数重新设好回调
    }
}

// R3A-应修2：单次调用里不计步的循环：math.min/max 的参数、含大量 \0 的串的比较和 string.find 的普通模式检查
void TestUncountedCalls()
{
    g_log.clear();
    ResetHub();
    const auto host = MakeEventHost();
    auto config = TestConfig();
    config.memoryLimitMb = 512;
    ddlua::Session session(host, config, "单次调用");
    session.Load(Sources({ "ev_a" }));
    ddlua::Mod* mod = session.FindMod("ev_a");
    CHECK(mod);
    if (!mod) {
        return;
    }
    const double limit = config.budgetMs + 100.0;
    const char* args = "T = {} for i = 1, 400000 do T[i] = i end";
    const char* sargs = "T = {} for i = 1, 400000 do T[i] = 'x' end";
    const char* zeros = "Z = string.rep(string.rep('\\0', 4096), 14000)";  // 54.7 MB
    const R3Case loops[] = {
        // 审核者：486.6、474.0 ms；54.7 MB 的 \0 串反复 < 513.2 ms
        { "math.max(...) 40 万个参数，反复", args, "local function f(...) while true do local n = math.max(...) end end f(table.unpack(T))" },
        { "math.min(...) 40 万个参数，反复", args, "local function f(...) while true do local n = math.min(...) end end f(table.unpack(T))" },
        { "54.7 MB 的 \\0 串反复 <", zeros, "while true do local _ = Z < Z end" },
        { "string.find('x', 54.7 MB 的 \\0 串) 反复", zeros, "while true do string.find('x', Z) end" },
        // 排查时补的：DDLua 自己的 warn 逐个检查参数；pcall 移动全部参数
        { "warn(...) 40 万个参数，反复", sargs, "local function f(...) while true do warn(...) end end f(table.unpack(T))" },
        { "pcall(g, ...) 40 万个参数，反复", args, "local g = function() end local function f(...) while true do pcall(g, ...) end end f(table.unpack(T))" },
    };
    for (const auto& c : loops) {
        CHECK(RunR3(session, *mod, c, R3Expect::Interrupted, limit));
        FreeR3(session, *mod);
    }
    // 单次：审核者测的 Z < Z 一条指令 256.5 ms、string.find 一次 242.0 ms，都判为成功
    CHECK(RunR3(session, *mod, { "54.7 MB 的 \\0 串，Z < Z 一次", zeros, "local _ = Z < Z" }, R3Expect::Either, limit));
    CHECK(RunR3(session, *mod, { "string.find('x', 54.7 MB 的 \\0 串) 一次", nullptr, "string.find('x', Z)" }, R3Expect::Either, limit));
    FreeR3(session, *mod);
    CHECK(Lua(session, "ev_a", R"(
        assert('a\0b' < 'a\0c' and not ('a\0' < 'a') and 'a' < 'a\0' and 'a\0\1' > 'a\0' and '\0' > '' and not ('' < ''))
        assert('b' > 'a\255' and 'Z' < 'a' and '\255' > '\127' and 'abc' < 'abd' and 'ab' < 'abc' and 'a\0\0' <= 'a\0\0')
        assert(not ('a\0b' < 'a\0b') and 'a\0b' <= 'a\0b' and '\0\0' < '\0\1' and '\1' > '\0\255')
        local t = { 'b\0', 'a', 'b', 'a\0\0', 'a\0', '' } table.sort(t)
        assert(table.concat(t, '|') == '|a|a\0|a\0\0|b|b\0')
        assert(string.find('x\0y', '\0y') == 2 and string.find('x\0y', '\0z') == nil and string.find('a.b', '.', 1, true) == 2)
        assert(string.find('x\0y\0', 'y\0') == 3 and string.find('x\0y', '.\0') == 1 and string.find('ab', 'b') == 2)
        assert(math.max(3, 1, 2) == 3 and math.min(3, 1, 2) == 1 and math.max(1) == 1 and math.type(math.max(1, 2.5)) == 'float')
        assert(not pcall(math.max) and not pcall(math.min, 1, 'x'))
    )"));
    session.Teardown();
}

// R3A-备注1：检查点之间跨过截止时间的那次操作，在入口末尾也要报告（原来 busy 40 ms 后 return tonumber(32 MB) 判为成功）
void TestEntryEndOverrun()
{
    g_log.clear();
    ResetHub();
    const auto host = MakeEventHost();
    auto config = TestConfig();
    config.memoryLimitMb = 512;
    ddlua::Session session(host, config, "入口末尾");
    g_now = 0;
    session.SetClock([] { return g_now; });
    session.Load(Sources({ "ev_a" }));
    ddlua::Mod* mod = session.FindMod("ev_a");
    CHECK(mod);
    if (!mod) {
        return;
    }
    const double limit = config.budgetMs + 100.0;
    std::string prepError;
    CHECK(session.RunChunk(*mod, "DATA3 = string.rep(string.rep('a', 4096), 8192)", "=prep", &prepError, nullptr, 5000));
    const R3Case cases[] = {
        { "忙 45 ms 后 return tonumber(32 MB)（尾调用）", nullptr, "local t = os.clock() while os.clock() - t < 0.045 do end return tonumber(DATA3)" },
        { "忙 45 ms 后 local x = tonumber(32 MB) return x", nullptr,
            "local t = os.clock() while os.clock() - t < 0.045 do end local x = tonumber(DATA3) return x" },
    };
    for (const auto& c : cases) {
        CHECK(RunR3(session, *mod, c, R3Expect::Interrupted, limit));
    }
    std::string error;
    CHECK(session.RunChunk(*mod, "local x = 0 for i = 1, 100000 do x = x + i end", "=next", &error));
    // 定时器回调：入口末尾超时同样算超预算，定时器停用
    CHECK(Lua(session, "ev_a", "dd.every(0, function() local t = os.clock() while os.clock() - t < 0.045 do end return tonumber(DATA3) end)"));
    Tick(session, 1);
    CHECK(LogHas("超出执行预算），已停用该定时器"));
    // 事件回调：同样停用订阅
    CHECK(Lua(session, "ev_a", "dd.on('app.tick', function() local t = os.clock() while os.clock() - t < 0.045 do end return tonumber(DATA3) end)"));
    Fire("app.tick").Go();
    CHECK(LogHas("超出执行预算），已停用该订阅"));
    CHECK(Lua(session, "ev_a", "DATA3 = nil"));
    session.Teardown();
    CHECK(session.memory().residual == 0);
}

// R3A-备注2：大量挂起的协程拉长 GC 原子阶段（只记实测数据，写进 03）
void TestGcThreadsMeasure()
{
    ResetHub();
    const auto host = MakeEventHost();
    for (int n : { 20000, 80000 }) {
        g_log.clear();
        auto config = TestConfig();
        config.memoryLimitMb = 128;
        ddlua::Session session(host, config, "挂起的协程");
        session.Load(Sources({ "ev_a", "ev_b" }));
        ddlua::Mod* a = session.FindMod("ev_a");
        ddlua::Mod* b = session.FindMod("ev_b");
        CHECK(a && b);
        if (!a || !b) {
            return;
        }
        std::string error;
        for (int k = 0; k < n / 5000; ++k) {
            session.RunChunk(*a,
                "COS = COS or {} for i = 1, 5000 do local co = coroutine.create(function() coroutine.yield() end) coroutine.resume(co) COS[#COS + 1] = co end",
                "=cos", &error);
        }
        double worst = 0;
        int failed = 0;
        for (int i = 0; i < 6; ++i) {
            const auto start = std::chrono::steady_clock::now();
            failed += session.RunChunk(*b, "for i = 1, 200000 do local t = {} end", "=alloc", &error) ? 0 : 1;
            worst = (std::max)(worst, Ms(start));
        }
        const auto start = std::chrono::steady_clock::now();
        session.vm()->FullGc();
        std::printf("  %d 个挂起的协程（Lua 内存 %.1f MB）：别的 Mod 分配 20 万个临时表最长 %.1f ms（6 次里 %d 次超预算），完整 GC %.1f ms\n", n,
            session.vm()->MemoryUsed() / 1048576.0, worst, failed, Ms(start));
        session.Teardown();
        CHECK(session.memory().residual == 0);
    }
}

// 核对审核应修 1：活对象超过内存上限的一半以后，原版的 GC 阈值（活对象的 2 倍）到不了，每次都是分配撞上上限、做一次完整的
// 紧急 GC（60–100 ms）；入口末尾的检查把这段停顿判给当时运行的回调，停用的是无辜的 Mod（审核者：8 万个协程时 dd.every(0)
// 的小回调第 978 帧停用，dd.spawn 的协程第 2392 帧终止）。修复：整段的 GC 停顿（紧急 GC、原子阶段）补到入口的截止时间上，
// 每个入口最多补 max(一份预算, 最长的一次停顿)；GC 阈值压到上限以下，大堆也走增量 GC。
// 审核者的复现在 guest 的 build/review3v/（gcblame、gcblame2），修复前后的数据在 build/final/
void TestGcPauseCredit()
{
    const auto host = MakeEventHost();
    struct Heap {
        const char* name;
        const char* prep;  // 在 ev_a 里运行 rounds 次
        int rounds;
    } heaps[] = {
        { "90 万个小表", "BIG = BIG or {} local n = #BIG for i = n + 1, n + 50000 do BIG[i] = { i } end", 18 },
        { "8 万个挂起的协程",
            "COS = COS or {} for i = 1, 5000 do local co = coroutine.create(function() coroutine.yield() end) coroutine.resume(co) COS[#COS + 1] = co end",
            16 },
    };
    constexpr int kFrames = 8000;
    for (const auto& heap : heaps) {
        g_log.clear();
        ResetHub();
        auto config = TestConfig();
        config.memoryLimitMb = 128;
        ddlua::Session session(host, config, "GC 停顿的补偿");
        g_now = 0;
        session.SetClock([] { return g_now; });
        session.Load(Sources({ "ev_a", "ev_b" }));
        ddlua::Mod* a = session.FindMod("ev_a");
        ddlua::Mod* b = session.FindMod("ev_b");
        CHECK(a && b);
        if (!a || !b) {
            return;
        }
        ddlua::Vm* vm = session.vm();
        CHECK(lua_ddgcthreshold_fn != nullptr && lua_ddatomic_fn != nullptr);  // VM 的构造函数设好的
        std::string error;
        for (int k = 0; k < heap.rounds; ++k) {
            CHECK(session.RunChunk(*a, heap.prep, "=heap", &error, nullptr, 5000));
        }
        vm->FullGc();
        const double live = vm->MemoryUsed() / 1048576.0;
        auto start = std::chrono::steady_clock::now();
        lua_gc(vm->L(), LUA_GCCOLLECT);
        const double gcMs = Ms(start);
        CHECK(Lua(session, "ev_b", "F = function() local t = {} for i = 1, 200 do t[i] = { i } end end"));
        // 1. 另一个 Mod 的 dd.every(0) 回调每帧分配 200 个小表（平时 0.05 ms）
        CHECK(Lua(session, "ev_b", "TIMER = dd.every(0, F)"));
        std::uint64_t gcs0 = vm->EmergencyGcs(), credits0 = vm->GcPauseCredits();
        double credited0 = vm->GcPauseCreditedMs(), worst = 0;
        int frames = 0;
        double now = 0;
        for (; frames < kFrames && !LogHas("已停用该定时器"); ++frames) {
            start = std::chrono::steady_clock::now();
            Tick(session, now += 0.016);
            worst = (std::max)(worst, Ms(start));
        }
        const bool timerOk = frames == kFrames && !LogHas("已停用该定时器");
        std::printf("  %s %s（%.1f MB，完整 GC %.0f ms）：dd.every(0) 回调分配 200 个小表 × %d 帧，%s；最长一帧 %.1f ms，"
                    "紧急 GC %llu 次，补偿 %llu 次共 %.0f ms\n",
            timerOk ? "PASS" : "FAIL", heap.name, live, gcMs, kFrames,
            timerOk ? "没有停用" : ("第 " + std::to_string(frames) + " 帧停用").c_str(), worst,
            static_cast<unsigned long long>(vm->EmergencyGcs() - gcs0), static_cast<unsigned long long>(vm->GcPauseCredits() - credits0),
            vm->GcPauseCreditedMs() - credited0);
        CHECK(timerOk);
        CHECK(vm->GcPauseCredits() > credits0);  // 确实碰上了要补的停顿
        CHECK(Lua(session, "ev_b", "TIMER:cancel()"));
        // 2. dd.spawn 的协程每帧分配一点再 dd.wait(0)
        g_log.clear();
        CHECK(Lua(session, "ev_b", "dd.spawn(function() while true do F() dd.wait(0) end end)"));
        gcs0 = vm->EmergencyGcs();
        credits0 = vm->GcPauseCredits();
        credited0 = vm->GcPauseCreditedMs();
        worst = 0;
        for (frames = 0; frames < kFrames && !LogHas("协程超出执行预算"); ++frames) {
            start = std::chrono::steady_clock::now();
            Tick(session, now += 0.016);
            worst = (std::max)(worst, Ms(start));
        }
        const bool spawnOk = frames == kFrames && !LogHas("协程超出执行预算");
        std::printf("  %s %s：dd.spawn 的协程每帧分配 200 个小表 × %d 帧，%s；最长一帧 %.1f ms，紧急 GC %llu 次，补偿 %llu 次共 %.0f ms\n",
            spawnOk ? "PASS" : "FAIL", heap.name, kFrames, spawnOk ? "没有终止" : ("第 " + std::to_string(frames) + " 帧终止").c_str(), worst,
            static_cast<unsigned long long>(vm->EmergencyGcs() - gcs0), static_cast<unsigned long long>(vm->GcPauseCredits() - credits0),
            vm->GcPauseCreditedMs() - credited0);
        CHECK(spawnOk);
        // 3. 卡死仍然被拦住（审核者设想的情形）：活对象补到约 98 MB（其中 A 是 10 MB 的串），在一个入口里反复 A .. A：每次一块
        //    20 MB 的分配，上一个成了垃圾，撞上上限时紧急 GC 回收它，回收后离上限约 10 MB，算「起作用」、不进冷却。不限补偿的话
        //    这个入口永远到不了截止时间；现在一个入口最多补 max(预算, 最长一次停顿)，入口最长是预算 + 两次完整 GC
        vm->FullGc();
        Sleep(1100);  // 前面如果有没起作用的紧急 GC，等过冷却
        const double pad = 98.0 * 1048576.0 - static_cast<double>(vm->MemoryUsed()) - 10.0 * 1048576.0;
        std::string prep = "A = string.rep('x', 10 * 2^20)";
        if (pad > 0) {
            prep += " PAD = string.rep('y', " + std::to_string(static_cast<long long>(pad)) + ")";
        }
        CHECK(session.RunChunk(*b, prep, "=prep", &error, nullptr, 5000));
        vm->FullGc();
        const double limit = config.budgetMs + 2.0 * gcMs + 100.0;
        const std::uint64_t capped0 = vm->GcPauseCapped();
        gcs0 = vm->EmergencyGcs();
        credits0 = vm->GcPauseCredits();
        credited0 = vm->GcPauseCreditedMs();
        const double liveAttack = vm->MemoryUsed() / 1048576.0;
        CHECK(RunR3(session, *b, { "活对象约 98 MB 时一个入口里反复 A .. A（每次 20 MB）", nullptr, "while true do local s = A .. A end" },
            R3Expect::Interrupted, limit));
        std::printf("    （活对象 %.1f MB，上限 %.0f ms；这个入口里紧急 GC %llu 次，补偿 %llu 次共 %.0f ms，超过补偿上限没补全的停顿 %llu 次）\n",
            liveAttack, limit, static_cast<unsigned long long>(vm->EmergencyGcs() - gcs0),
            static_cast<unsigned long long>(vm->GcPauseCredits() - credits0), vm->GcPauseCreditedMs() - credited0,
            static_cast<unsigned long long>(vm->GcPauseCapped() - capped0));
        std::string next;
        CHECK(session.RunChunk(*b, "A = nil PAD = nil F()", "=next", &next));
        session.Teardown();
        CHECK(session.memory().residual == 0);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // 和 DDLua 的 DDPlugin_Load 一样：CRT 的无效参数返回错误码而不是终止进程
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {});
    const char* only = nullptr;  // 只运行名字里含这个子串的测试组（调试用）
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0) {
            g_verbose = true;
        } else {
            only = argv[i];
        }
    }
    struct Test {
        const char* name;
        void (*fn)();
    } tests[] = {
        { "配置", TestConfigParse },
        { "路径规范化", TestNormalize },
        { "manifest 和加载顺序", TestManifest },
        { "会话加载和失败隔离", TestSessionLoad },
        { "拆除、内存回落、重建", TestTeardownAndRebuild },
        { "沙箱", TestSandbox },
        { "联接逃逸", TestJunctionEscape },
        { "执行预算", TestBudget },
        { "内存上限", TestMemoryLimit },
        { "事件绑定", TestEventBinding },
        { "事件回调出错和预算", TestEventFailures },
        { "事件作用域", TestEventScopes },
        { "调度器和协程", TestScheduler },
        { "热重载和 dd.state", TestReload },
        { "文件监视", TestWatch },
        { "每个 Mod 的上限", TestPerModLimits },
        { "切换存档（订阅）", TestSessionSwitch },
        { "actor 句柄", TestActorHandles },
        { "actor 句柄和事件", TestActorEvents },
        { "actor 场景 Mod 预演", TestActorScenarioMod },
        { "Lua Mod 模板和示例", TestTemplateAndExample },
        { "LuaLS 声明和实际 API 对照", TestApiDeclarations },
        { "P3d 场景预演（经过 plugin.cpp）", TestScenarioRehearsal },
        { "VM 损坏（原生异常，审核 B-Y3）", TestVmBroken },
        { "帧预算（审核 A-B2）", TestFrameBudget },
        { "重载的帧预算（审核 A-B2）", TestReloadBudget },
        { "Mod 之间的边界（审核 B-N4、B-N1、A-B3、A-C2、A-C4）", TestModBoundaries },
        { "弱键表的 GC（第二轮 R2A-阻断2）", TestWeakKeyGc },
        { "main.lua 和 load 的语法分析预算（第二轮 R2A-阻断3、备注4）", TestParseBudget },
        { "错误日志的截断和限速（第二轮 R2A-应修1）", TestErrorLogCaps },
        { "dd.once 回调里的原生异常（第二轮 R2A-应修2）", TestOnceNativeFault },
        { "dd.import 的代理缓存（第二轮 R2A-备注1）", TestImportCache },
        { "拆除和重载时 <close> 的总时间（第二轮 R2A-备注3）", TestTeardownCloseCap },
        { "位置捕获的计步（第二轮 R2A-备注5）", TestPositionCaptureStep },
        { "lifecycle 回调里的原生异常（经过 plugin.cpp，审核 B-Y3）", TestPluginNativeFault },
        { "p3c_actor 的 wait_event 快照不依赖第一个英雄（第二轮 R2B-应修2）", TestActorScenarioSnapshot },
        { "p3d_core 的 damage.halve 和受到伤害的倍率（第二轮 R2B-备注4）", TestScenarioHalveMultiplier },
        { "哈希冲突（第三轮 R3A-阻断1）", TestHashFlood },
        { "字符串转数字的计步（第三轮 R3A-阻断2）", TestStr2NumSteps },
        { "没有 lua_State 的进度回调和协程（第三轮 R3A-阻断3）", TestNolThreads },
        { "内存上限附近的紧急 GC（第三轮 R3A-应修1）", TestEmergencyGc },
        { "单次调用里的长循环（第三轮 R3A-应修2）", TestUncountedCalls },
        { "入口末尾的超时（第三轮 R3A-备注1）", TestEntryEndOverrun },
        { "挂起的协程和 GC 停顿（第三轮 R3A-备注2，只记数据）", TestGcThreadsMeasure },
        { "GC 停顿的补偿和阈值（核对审核应修 1）", TestGcPauseCredit },
    };
    int index = 0;
    for (const auto& test : tests) {
        // 参数是数字时按序号（从 1 开始，命令行的中文在 ANSI 代码页下对不上），否则按名字的子串
        ++index;
        if (only && (std::isdigit(static_cast<unsigned char>(only[0])) ? std::atoi(only) != index : !std::strstr(test.name, only))) {
            continue;
        }
        const int before = g_failures;
        std::printf("[%s]\n", test.name);
        test.fn();
        std::printf("  -> %s\n", g_failures == before ? "通过" : "失败");
    }
    std::printf("%s，%d 项检查，失败 %d\n", LUA_RELEASE, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
