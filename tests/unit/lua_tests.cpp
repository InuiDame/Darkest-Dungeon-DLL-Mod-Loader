// DDLua 运行时的离线测试。P3a：配置、路径规范化、manifest 和加载顺序、会话的加载和拆除、
// 沙箱逃逸用例、执行预算（主 chunk、pcall 吞错、协程、模式匹配和其他 C 函数的长循环）、内存上限。
// P3b：事件绑定（读写、取消、优先级、filter、负载失效、延迟事件、作用域、连续失败）、调度器和协程、
// 热重载和 dd.state、文件监视、切换存档后订阅清零。
// P3c：actor 句柄（假的 game 服务，语义同 DDCore 的 core/src/game/actors.cpp）：字段读取、驻留和比较、
// 跨存档和跨探索失效、事件指针字段转句柄、按句柄过滤、actor 作用域、wait_event 快照里的句柄。
// P3d：Lua Mod 模板和示例的加载运行、LuaLS 声明和实际 API 的对照、场景 Mod 的离线预演（经过真实的 plugin.cpp，
// 主题表用 sdk/lua/topics.json，日志按 DDLua.log 的格式写到 build/p3d_rehearsal/，给 tools/p3_scenario_check.py 用）。
// P4b：假的 combat 服务（立即执行、排队、操作回调、拒绝、buff.* 事件、规则判定），dd.buff / dd.effect / actor 的 buff 方法、
// 脚本 Effect、脚本 Buff 的状态、规则的只读和停用、dd.fs 的写入、嵌套入口（同一个 VM、跨 VM、嵌套里的原生异常），
// 场景 Mod p4b_combat 的离线预演。
// P4c2：场景 Mod shield_lite、p4c_shield、p4c_persist 的离线预演（日志写到 build/p4c_rehearsal/，给 tools/p4_scenario_check.py 自测用）。
// 编译 lua/src 下的全部源码（P3d 起含 plugin.cpp），用假的 log 服务当宿主；P3b 的 events 服务直接包装 DDCore 的
// 事件中枢（hub.cpp、access.h）。Mod 夹具在 tests/lua/mods。
// 运行：xmake build dd_lua_tests && xmake run dd_lua_tests [-v]

#include "combat/logic.h"
#include "config.h"
#include "events/access.h"
#include "manifest.h"
#include "modfs.h"
#include "session.h"
#include "state_json.h"
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
    // 只差大小写的 id 也算重复（数据目录、存档旁路在 NTFS 上是同一个文件），依赖其中一个的也拒绝（F2，审核 B-S5、C-Y2）
    order = ddlua::ResolveLoadOrder({ M("Foo"), M("other"), M("foo"), M("needs", { D("foo") }) });
    CHECK(order.order == std::vector<std::size_t>({ 1 }));
    CHECK(Rejected(order, 0, "只差大小写") && Rejected(order, 2, "和启用列表里的 Foo 只差大小写") && Rejected(order, 3, "依赖 foo 不加载"));
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
    // F5 的枚举：队伍（Raid+8，只在有 Raid 时）、战斗状态（Battle+0x288，0 是没有战斗）
    std::vector<std::uintptr_t> party;
    std::int32_t battleState = 0;
} g_world;

// 在 g_fc（假的 combat 服务的状态，后面定义）里：有没有 Raid、加载线程在不在跑
bool FakeRaidNow();
bool FakeLoadingNow();

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

// 枚举（F5）：和 DDCore 一样，加载中、存档外返回 false；没有 Raid 时队伍、怪物都是 0 个
bool FakeList(const std::vector<std::uintptr_t>& pointers, DDActorRef* out, uint32_t capacity, uint32_t* outCount)
{
    if (outCount) {
        *outCount = 0;
    }
    if (!g_world.active || FakeLoadingNow()) {
        return false;
    }
    std::uint32_t n = 0;
    for (const auto p : FakeRaidNow() ? pointers : std::vector<std::uintptr_t>{}) {
        DDActorRef ref{};
        ref.size = sizeof(ref);
        if (!FakeFromPointer(P(p), &ref)) {
            continue;
        }
        if (out && n < capacity) {
            out[n] = ref;
        }
        ++n;
    }
    if (outCount) {
        *outCount = n;
    }
    return true;
}

bool DD_CALL FakePartyHeroes(DDActorRef* out, uint32_t capacity, uint32_t* outCount)
{
    std::vector<std::uintptr_t> heroes;
    for (const auto p : g_world.party) {
        if (g_world.heroes.count(p)) {
            heroes.push_back(p);
        }
    }
    return FakeList(heroes, out, capacity, outCount);
}

bool DD_CALL FakeBattleMonsters(DDActorRef* out, uint32_t capacity, uint32_t* outCount)
{
    std::vector<std::uintptr_t> monsters;
    for (const auto& [p, actor] : g_world.monsters) {
        monsters.push_back(p);
    }
    return FakeList(monsters, out, capacity, outCount);  // 预演在战斗之外清空 g_world.monsters（同游戏：战斗销毁时清空怪物表）
}

int32_t DD_CALL FakeBattleState()
{
    if (!g_world.active || FakeLoadingNow()) {
        return -1;
    }
    return FakeRaidNow() ? g_world.battleState : 0;
}

const DDGameService kFakeGame = { { sizeof(DDGameService), DD_SERVICE_GAME_VERSION }, &FakeModsAvailable, &FakeModCount, &FakeModAt,
    &FakeActorKinds, &FakeRaidSerial, &FakeFromPointer, &FakeFind, &FakeResolve, &FakeInfo, &FakeBattleSerial, &FakePartyHeroes,
    &FakeBattleMonsters, &FakeBattleState };

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

// P4b 的假 combat 服务（定义在文件后面）
void ResetCombat();
ddlua::Host MakeCombatHost();

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
            cls = name == "dd" ? "dd" : name == "dd.Actor" ? "Actor" : name == "dd.Event" ? "Event" : name == "dd.Subscription" ? "Subscription"
                : name == "dd.Effect" ? "Effect" : name == "dd.Skill" ? "Skill" : name == "dd.Rule" ? "Rule" : "";
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
            { "function Timer:", "Timer" }, { "function Task:", "Task" }, { "function Actor:", "Actor" }, { "function Event:", "Event" },
            { "function bufflib.", "buff" }, { "function effectlib.", "effect" }, { "function Effect:", "Effect" }, { "function Skill:", "Skill" },
            { "function Rule:", "Rule" } };
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
    for (const char* set : { "dd", "log", "fs", "actor", "Subscription", "Timer", "Task", "Actor", "Event", "buff", "effect", "Effect", "Skill", "Rule" }) {
        std::printf("  声明 %s：%zu 项\n", set, decl.sets[set].size());
        CHECK(!decl.sets[set].empty());
    }

    ResetCombat();
    const auto host = MakeCombatHost();
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
        same("dd.buff", dd.buff, DECL.buff)
        same("dd.effect", dd.effect, DECL.effect)
        -- P4b 的句柄：声明的字段和方法都能读；effect 句柄报错时列出的字段都有声明
        local e = assert(dd.effect.get("rakebuff_5"))
        for k in pairs(DECL.Effect) do assert(pcall(function() return e[k] end), "dd.Effect." .. k) end
        local _, fxerr = pcall(function() return e.nope end)
        local fxlist = assert(fxerr:match("可用：(.-)）"), fxerr)
        for name in fxlist:gmatch("[^、]+") do assert(DECL.Effect[name], "effect 句柄的字段 " .. name .. " 没有声明") end
        local r = assert(dd.buff.rule("decl_rule", function() return true end))
        for k in pairs(DECL.Rule) do assert(r[k] ~= nil, "dd.Rule." .. k) end
        r:off()
        dd.once("skill.pre", function(ev)
            local sk = ev.skill
            for k in pairs(DECL.Skill) do assert(pcall(function() return sk[k] end), "dd.Skill." .. k) end
            local _, skerr = pcall(function() return sk.nope end)
            for name in assert(skerr:match("可用：(.-)）"), skerr):gmatch("[^、]+") do assert(DECL.Skill[name], "技能句柄的字段 " .. name) end
            SKILL_OK = true
        end)
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
    Fire skill("skill.pre");
    skill.P("performer", P(0x10000)).P("skill", P(0x80000));
    skill.Go();
    CHECK(Lua(session, "ev_a", "assert(EV_OK, '负载的元信息和方法') assert(SKILL_OK, '技能句柄')"));
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

// P4c1：假的 persist 服务。load 按 key 返回预先设好的结果，save 只在模拟的 campaign.save 里接受（同 DDCore）
struct FakePersistKey {
    std::uint32_t result = DD_PERSIST_OK;
    std::uint32_t status = DD_PERSIST_EMPTY;
    std::string reason;
    std::string data;
    std::string version;
    bool raidPending = false;
    bool fromBackup = false;
};

struct FakePersistState {
    DDPersistSaveFn onSave = nullptr;  // plugin.cpp 的 DDPlugin_Load 声明时设
    DDPersistVerifyFn onVerify = nullptr;
    void* user = nullptr;
    int declares = 0;
    std::map<std::string, FakePersistKey> loads;
    std::map<std::string, std::string> saved;
    std::map<std::string, std::string> prevSaved;  // 上一代（save 时原来的当前一代轮换过去，同 DDCore 的 .prev）
    std::map<std::string, std::string> savedVersion;
    std::vector<std::string> blocked;  // 像 DDCore：raid 核对不一致之后这次进存档不再写（DD_PERSIST_E_INCONSISTENT）
    int loadCalls = 0;
    bool saving = false;
} g_fp;

// 每个测试前清掉读写记录（保留 plugin.cpp 声明的回调）
void ResetPersistFakes()
{
    g_fp.loads.clear();
    g_fp.saved.clear();
    g_fp.prevSaved.clear();
    g_fp.savedVersion.clear();
    g_fp.blocked.clear();
    g_fp.loadCalls = 0;
    g_fp.saving = false;
}

const char* DD_CALL FakePersistStatusText(uint32_t)
{
    return "?";
}

uint32_t DD_CALL FakePersistDeclare(DDPluginHandle plugin, DDPersistSaveFn onSave, DDPersistVerifyFn onVerify, void* user)
{
    if (plugin != kPlugin || !onSave) {
        return DD_PERSIST_E_INVALID;
    }
    g_fp.onSave = onSave;
    g_fp.onVerify = onVerify;
    g_fp.user = user;
    ++g_fp.declares;
    return DD_PERSIST_OK;
}

uint32_t DD_CALL FakePersistLoad(DDPluginHandle, const char* key, DDPersistData* out)
{
    ++g_fp.loadCalls;
    static const FakePersistKey kEmpty;
    const auto it = g_fp.loads.find(key ? key : "");
    const FakePersistKey& k = it == g_fp.loads.end() ? kEmpty : it->second;
    if (k.result != DD_PERSIST_OK) {
        return k.result;
    }
    const bool data = k.status == DD_PERSIST_LOADED || k.status == DD_PERSIST_PREV;
    out->status = k.status;
    out->reason = k.reason.c_str();
    out->data = data ? k.data.data() : nullptr;
    out->data_size = data ? static_cast<std::uint32_t>(k.data.size()) : 0;
    out->version = k.version.c_str();
    out->written_ms = data ? 1763100000123ull : 0;
    out->in_raid = data && k.raidPending;
    out->raid_pending = data && k.raidPending;
    out->from_backup = k.fromBackup;
    return DD_PERSIST_OK;
}

uint32_t DD_CALL FakePersistSave(DDPluginHandle, const char* key, const void* data, uint32_t size, const char* version)
{
    if (!g_fp.saving) {
        return DD_PERSIST_E_TIMING;
    }
    if (std::find(g_fp.blocked.begin(), g_fp.blocked.end(), key) != g_fp.blocked.end()) {
        return DD_PERSIST_E_INCONSISTENT;
    }
    if (const auto it = g_fp.saved.find(key); it != g_fp.saved.end()) {
        g_fp.prevSaved[key] = it->second;
    }
    g_fp.saved[key].assign(static_cast<const char*>(data), size);
    g_fp.savedVersion[key] = version ? version : "";
    return DD_PERSIST_OK;
}

bool DD_CALL FakePersistInfo(DDPersistInfo*)
{
    return false;
}

const DDPersistService kFakePersist = { { sizeof(DDPersistService), DD_SERVICE_PERSIST_VERSION }, &FakePersistStatusText,
    &FakePersistDeclare, &FakePersistLoad, &FakePersistSave, &FakePersistInfo };

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
    if (service == DD_SERVICE_PERSIST) {
        return &kFakePersist;
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

// ================================================================ P4b：Effect 与 Buff

// ---------------------------------------------------------------- 假的 combat 服务
// 语义照 core/src/combat/combat.cpp：时机用同一份 DecideTiming（帧边界、派发深度、DDCore 自己的游戏调用层数），
// 立即执行时同步派发 buff.add / buff.remove（hook 已装上时），排队的在 FakeDrain（帧边界）执行后通知操作回调。
// buff 表按 actor 的假指针存，guid 每个 actor 只增不减。插件名：DDLua（kPlugin）和 other_native。
namespace fc = ddc::combat;

struct FakeBuff {
    std::uint32_t guid = 0;
    DDBuffDef def{};
    std::string plugin;  // 经 buff_add 加的插件名；游戏加的为空
    std::string tag;
};

struct FakeEffect {
    DDEffectInfo info{};
    std::vector<std::string> buffs;  // 施加时给目标加的 buff id
    DDPluginHandle owner = nullptr;  // 运行时定义的插件
};

struct FakeOp {
    std::uint64_t id = 0;
    DDPluginHandle plugin = nullptr;
    std::uint32_t kind = 0;
    DDActorRef actor{}, actor2{};
    bool hasTarget = false;
    std::string text, tag;
    DDBuffAddOptions add{};
    DDBuffRemoveSpec remove{};
    DDBuffDef def{};
};

struct FakeRuleReg {
    DDRuleId id = 0;
    DDPluginHandle plugin = nullptr;
    std::string name;
    DDRuleFn fn = nullptr;
    void* user = nullptr;
    bool disabled = false;
};

struct FakeCombatState {
    bool frame = true;   // 在帧边界的回调里（RunChunk、Tick 模拟 lifecycle）
    bool raid = true;    // 有 Raid::Current（走廊、战斗）
    bool hooks = true;   // buff.* 的 hook 已装上（DDCore 在订阅后的下一帧装）
    bool loading = false;
    bool pvp = false;
    int gameCall = 0;    // DDCore 自己调用游戏函数的层数
    int inRule = 0;
    int sleepMs = 0;     // buff_add 派发 buff.add 之前睡这么久（模拟游戏函数的耗时，不经过 Lua）
    std::map<std::uintptr_t, std::vector<FakeBuff>> tables;
    std::map<std::uintptr_t, std::uint32_t> nextGuid;
    std::map<std::string, DDBuffDef> library;
    std::map<std::string, DDPluginHandle> libOwner;
    std::map<std::string, FakeEffect> effects;
    std::vector<std::string> smiteEffects = { "Stun 1", "rakebuff_5" };  // 假技能 smite 的 .effect（P4c2 换成数据层圣骑士 Smite 的）
    std::vector<FakeOp> queue;
    std::uint64_t nextOp = 1;
    DDCombatOpFn handler = nullptr;
    void* handlerUser = nullptr;
    std::vector<FakeRuleReg> rules;
    DDRuleId nextRule = 1;
    std::uint64_t ruleCalls = 0;
    std::uint64_t ruleOriginal = 0;  // 回调交还原函数（DD_RULE_ORIGINAL）的次数
    std::string message;
    std::uint64_t adds = 0, removes = 0;
} g_fc;

bool FakeRaidNow()
{
    return g_fc.raid;
}

bool FakeLoadingNow()
{
    return g_fc.loading;
}

const DDPluginHandle kOtherPlugin = reinterpret_cast<DDPluginHandle>(static_cast<std::uintptr_t>(0x5678));

std::string FakePluginName(DDPluginHandle plugin)
{
    return plugin == kPlugin ? "DDLua" : "other_native";
}

fc::TimingDecision FakeTiming()
{
    fc::TimingState s;
    s.mainThread = true;
    s.inRule = g_fc.inRule > 0;
    s.campaignActive = g_world.active;
    s.loading = g_fc.loading;
    s.pvp = g_fc.pvp;
    s.inFrameBody = g_fc.frame;
    s.dispatchDepth = g_hub ? g_hub->Depth() : 0;
    s.gameCallDepth = g_fc.gameCall;
    return fc::DecideTiming(s, true);
}

const char* DD_CALL FakeResultText(uint32_t code)
{
    switch (code) {
    case DD_COMBAT_OK:
        return "成功";
    case DD_COMBAT_QUEUED:
        return "已排队";
    case DD_COMBAT_E_INVALID:
        return "参数无效";
    case DD_COMBAT_E_NO_CAMPAIGN:
        return "不在存档里";
    case DD_COMBAT_E_LOADING:
        return "加载线程正在运行";
    case DD_COMBAT_E_PVP:
        return "PvP 对局中";
    case DD_COMBAT_E_IN_RULE:
        return "规则回调里不能写";
    case DD_COMBAT_E_QUEUE_FULL:
        return "排队的操作太多";
    case DD_COMBAT_E_ACTOR:
        return "actor 句柄解析失败";
    case DD_COMBAT_E_NOT_FOUND:
        return "不存在";
    case DD_COMBAT_E_NO_RAID:
        return "需要 Raid::Current（城镇里没有）";
    case DD_COMBAT_E_EXISTS:
        return "已经存在";
    case DD_COMBAT_E_CANCELLED:
        return "排队的操作被丢弃";
    default:
        return "未知";
    }
}

uint32_t FakeOut(DDCombatOpResult* out, std::uint32_t kind, std::uint32_t code, std::string message, std::uint64_t op = 0,
    std::uint32_t guid = 0, std::uint32_t count = 0)
{
    g_fc.message = std::move(message);
    if (out && out->size >= sizeof(DDCombatOpResult)) {
        out->kind = kind;
        out->op = op;
        out->result = code;
        out->guid = guid;
        out->count = count;
        out->message = g_fc.message.c_str();
    }
    return code;
}

// 一条 buff 事件（hook 已装上、在存档里时）。deferred：加载线程上发生，复制后排到主线程
void FakeBuffEvent(const char* topicName, std::uintptr_t actor, FakeBuff b, const char* reason = nullptr, int before = 0, int after = 0,
    bool deferred = false)
{
    if (!g_fc.hooks || !g_world.active || !g_hub->Find(topicName)) {
        return;
    }
    const bool hero = g_world.heroes.count(actor) != 0;
    const std::uint32_t id = hero ? g_world.heroes[actor].id : g_world.monsters[actor].id;
    Fire f(topicName);
    f.P("actor", deferred ? nullptr : P(actor)).I("actor_kind", hero ? DD_ACTOR_HERO : DD_ACTOR_MONSTER).I("actor_id", id).I("guid", b.guid)
        .S("id", b.def.id).I("stat_type", b.def.stat_type).Fl("amount", b.def.amount).I("duration", b.def.duration)
        .I("duration_type", b.def.duration_type).I("source", b.def.source).I("sourcedata", b.def.sourcedata).I("rule_type", b.def.rule_type)
        .S("plugin", b.plugin.c_str()).S("tag", b.tag.c_str());
    if (reason) {
        f.S("reason", reason);
    }
    if (std::strcmp(topicName, "buff.tick") == 0) {
        f.I("duration_before", before).I("duration_after", after);
    }
    if (deferred) {
        f.Deferred();
    } else {
        f.Go();
    }
}

std::uintptr_t FakeActorPtr(const DDActorRef& ref)
{
    return reinterpret_cast<std::uintptr_t>(FakeResolve(&ref));
}

// 游戏自己加一条（技能、effect……）：plugin、tag 为空
std::uint32_t FakeGameAdd(std::uintptr_t actor, const char* id, bool event = true)
{
    FakeBuff b;
    b.guid = ++g_fc.nextGuid[actor];
    b.def = g_fc.library.at(id);
    g_fc.tables[actor].push_back(b);
    if (event) {
        ++g_fc.gameCall;
        FakeBuffEvent("buff.add", actor, b);
        --g_fc.gameCall;
    }
    return b.guid;
}

// 游戏删一条（到期、回城清理……）：先派发 buff.remove 再删
bool FakeGameRemove(std::uintptr_t actor, std::uint32_t guid, const char* reason, bool deferred = false)
{
    auto& table = g_fc.tables[actor];
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i].guid == guid) {
            ++g_fc.gameCall;
            FakeBuffEvent("buff.remove", actor, table[i], reason, 0, 0, deferred);
            --g_fc.gameCall;
            table.erase(table.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

// 持续时间递减一次：还在表里的派发 buff.tick，减到 0 的删掉（buff.remove expired）
void FakeTickBuff(std::uintptr_t actor, std::uint32_t guid)
{
    for (auto& b : g_fc.tables[actor]) {
        if (b.guid == guid && b.def.duration > 0) {
            const int before = b.def.duration--;
            if (b.def.duration == 0) {
                FakeGameRemove(actor, guid, "expired");
            } else {
                ++g_fc.gameCall;
                FakeBuffEvent("buff.tick", actor, b, nullptr, before, b.def.duration);
                --g_fc.gameCall;
            }
            return;
        }
    }
}

struct FakeOutcome {
    std::uint32_t code = DD_COMBAT_OK;
    std::uint32_t guid = 0;
    std::uint32_t count = 0;
    std::string message;
};

void FakeRefresh(std::uintptr_t actor);

FakeOutcome FakeAddNow(DDPluginHandle plugin, const DDActorRef& ref, const std::string& id, const DDBuffAddOptions& o, const std::string& tag)
{
    const std::uintptr_t actor = FakeActorPtr(ref);
    if (!actor) {
        return { DD_COMBAT_E_ACTOR, 0, 0, "actor 句柄解析失败" };
    }
    const auto it = g_fc.library.find(id);
    if (it == g_fc.library.end()) {
        return { DD_COMBAT_E_NOT_FOUND, 0, 0, "Buff 库里没有 " + id };
    }
    FakeBuff b;
    b.guid = ++g_fc.nextGuid[actor];
    b.def = it->second;
    if (o.flags & DD_BUFF_ADD_DURATION) {
        b.def.duration = o.duration;
    }
    if (o.flags & DD_BUFF_ADD_DURATION_TYPE) {
        b.def.duration_type = o.duration_type;
    }
    if (o.flags & DD_BUFF_ADD_SOURCE) {
        b.def.source = o.source;
    }
    if (o.flags & DD_BUFF_ADD_SOURCEDATA) {
        b.def.sourcedata = o.sourcedata;
    }
    b.plugin = FakePluginName(plugin);
    b.tag = tag;
    g_fc.tables[actor].push_back(b);
    ++g_fc.adds;
    ++g_fc.gameCall;
    if (g_fc.sleepMs) {
        Sleep(static_cast<DWORD>(g_fc.sleepMs));
    }
    // 槽位 13 带 refresh 时游戏在加完之后刷新属性（可能当场删掉 remove_if_not_active、规则不成立的这一条）；
    // DDCore 在原函数返回后按 guid 找条目派发 buff.add，找不到就不派发
    if (!(o.flags & DD_BUFF_ADD_NO_REFRESH)) {
        FakeRefresh(actor);
    }
    const auto& table = g_fc.tables[actor];
    if (std::any_of(table.begin(), table.end(), [&](const FakeBuff& x) { return x.guid == b.guid; })) {
        FakeBuffEvent("buff.add", actor, b);
    }
    --g_fc.gameCall;
    return { DD_COMBAT_OK, b.guid, 0, "" };
}

FakeOutcome FakeRemoveNow(DDPluginHandle plugin, const DDActorRef& ref, const DDBuffRemoveSpec& spec, const std::string& text)
{
    const std::uintptr_t actor = FakeActorPtr(ref);
    if (!actor) {
        return { DD_COMBAT_E_ACTOR, 0, 0, "actor 句柄解析失败" };
    }
    std::vector<std::uint32_t> hits;
    for (const auto& b : g_fc.tables[actor]) {
        const bool match = (spec.match == DD_BUFF_BY_GUID && b.guid == spec.guid) ||
            (spec.match == DD_BUFF_BY_TAG && b.plugin == FakePluginName(plugin) && b.tag == text) ||
            (spec.match == DD_BUFF_BY_ID && text == b.def.id) || (spec.match == DD_BUFF_BY_SOURCE && b.def.source == spec.source);
        if (match) {
            hits.push_back(b.guid);
        }
    }
    for (const auto guid : hits) {
        FakeGameRemove(actor, guid, "plugin");
        ++g_fc.removes;
    }
    if (!hits.empty()) {
        ++g_fc.gameCall;
        FakeRefresh(actor);  // 槽位 14 删到东西时刷新属性
        --g_fc.gameCall;
    }
    return { DD_COMBAT_OK, 0, static_cast<std::uint32_t>(hits.size()), "" };
}

FakeOutcome FakeApplyNow(const std::string& name, const DDActorRef& performer, const DDActorRef& target)
{
    if (!g_fc.raid) {
        return { DD_COMBAT_E_NO_RAID, 0, 0, "没有 Raid::Current（城镇里不能施加 effect）" };
    }
    const auto it = g_fc.effects.find(name);
    if (it == g_fc.effects.end()) {
        return { DD_COMBAT_E_NOT_FOUND, 0, 0, "没有 effect " + name };
    }
    const std::uintptr_t p = FakeActorPtr(performer);
    const std::uintptr_t t = FakeActorPtr(target);
    if (!p || !t) {
        return { DD_COMBAT_E_ACTOR, 0, 0, "performer 或 target 的句柄解析失败" };
    }
    // effect.pre 照常派发（脚本 Effect 建在它上面）
    Fire pre("effect.pre");
    pre.P("performer", P(p)).P("target", P(t)).P("effect", P(0x81000)).S("effect_name", name.c_str());
    ++g_fc.gameCall;
    const Payload payload = pre.Go();
    std::uint32_t count = 0;
    if (!payload.cancelled) {
        for (const auto& id : it->second.buffs) {
            FakeGameAdd(t, id.c_str());
            ++count;
        }
    }
    --g_fc.gameCall;
    return { DD_COMBAT_OK, 0, count, "" };
}

FakeOutcome FakeDefineNow(DDPluginHandle plugin, const DDBuffDef& def)
{
    const std::string id(def.id);
    const auto owner = g_fc.libOwner.find(id);
    if (g_fc.library.count(id) && (owner == g_fc.libOwner.end() || owner->second != plugin)) {
        return { DD_COMBAT_E_EXISTS, 0, 0, owner == g_fc.libOwner.end() ? "id " + id + " 是游戏数据，不能覆盖" : "id " + id + " 已经由别的插件注册" };
    }
    g_fc.library[id] = def;
    g_fc.libOwner[id] = plugin;
    return {};
}

DDBuffDef FakeDef(const char* id, const char* stat, const char* sub, float amount, int duration, const char* dtype, const char* source);

FakeOutcome FakeDefineEffectsNow(DDPluginHandle plugin, const std::string& text)
{
    std::vector<std::string> names;
    std::string why;
    if (!fc::ExtractEffectNames(text, &names, &why)) {
        return { DD_COMBAT_E_INVALID, 0, 0, why };
    }
    for (const auto& name : names) {
        const auto it = g_fc.effects.find(name);
        if (it != g_fc.effects.end() && it->second.owner != plugin) {
            return { DD_COMBAT_E_EXISTS, 0, 0, "effect " + name + (it->second.owner ? " 已经由别的插件定义" : " 是游戏数据，不能重新定义") };
        }
    }
    for (const auto& name : names) {
        FakeEffect e;
        e.info.size = sizeof(e.info);
        strcpy_s(e.info.name, name.c_str());
        e.info.runtime = true;
        e.owner = plugin;
        // 这个块里的 .buff_ids <id>（施加时加这个 buff）或 .combat_stat_buff 1（加一条生成的 buff fx_stat）
        const std::size_t at = text.find("\"" + name + "\"");
        const std::size_t next = text.find("effect:", at);
        const std::string block = text.substr(at, next == std::string::npos ? std::string::npos : next - at);
        if (const std::size_t ids = block.find(".buff_ids "); ids != std::string::npos) {
            const std::size_t from = ids + 10;
            e.buffs.push_back(block.substr(from, block.find_first_of(" \r\n", from) - from));
            e.info.buff_id_count = 1;
        } else if (block.find(".combat_stat_buff 1") != std::string::npos) {
            if (!g_fc.library.count("fx_stat")) {
                g_fc.library["fx_stat"] = FakeDef("fx_stat", "combat_stat_add", "speed_rating", 1, 1, "after_turn", "skill");
            }
            e.buffs.push_back("fx_stat");
        }
        g_fc.effects[name] = e;
    }
    return { DD_COMBAT_OK, 0, static_cast<std::uint32_t>(names.size()), "" };
}

FakeOutcome FakeExecute(const FakeOp& op)
{
    switch (op.kind) {
    case DD_COMBAT_OP_BUFF_ADD:
        return FakeAddNow(op.plugin, op.actor, op.text, op.add, op.tag);
    case DD_COMBAT_OP_BUFF_REMOVE:
        return FakeRemoveNow(op.plugin, op.actor, op.remove, op.text);
    case DD_COMBAT_OP_EFFECT_APPLY:
        return FakeApplyNow(op.text, op.actor, op.hasTarget ? op.actor2 : op.actor);
    case DD_COMBAT_OP_BUFF_DEFINE:
        return FakeDefineNow(op.plugin, op.def);
    default:
        return FakeDefineEffectsNow(op.plugin, op.text);
    }
}

// 立即执行或排队（和 DDCore 的 Submit 一样）
uint32_t FakeSubmit(FakeOp op, DDCombatOpResult* out)
{
    const fc::TimingDecision d = FakeTiming();
    if (d.timing == fc::Timing::Reject) {
        return FakeOut(out, op.kind, d.code, std::string("拒绝：") + FakeResultText(d.code));
    }
    if (d.timing == fc::Timing::Queue) {
        std::size_t mine = 0;
        for (const auto& q : g_fc.queue) {
            mine += q.plugin == op.plugin ? 1 : 0;
        }
        if (mine >= 256) {
            return FakeOut(out, op.kind, DD_COMBAT_E_QUEUE_FULL, "本插件排队的操作超过 256 个");
        }
        op.id = g_fc.nextOp++;
        const auto kind = op.kind;
        const auto id = op.id;
        g_fc.queue.push_back(std::move(op));
        return FakeOut(out, kind, DD_COMBAT_QUEUED, "", id);
    }
    const FakeOutcome o = FakeExecute(op);
    return FakeOut(out, op.kind, o.code, o.message, 0, o.guid, o.count);
}

void CallOpHandler(void* raw)
{
    auto* r = static_cast<DDCombatOpResult*>(raw);
    g_fc.handler(r, g_fc.handlerUser);
}

// 帧边界：执行排队的操作并通知（操作回调在安全点，里面的写操作立即执行）
std::size_t FakeDrain()
{
    std::vector<FakeOp> ops;
    ops.swap(g_fc.queue);
    const bool frame = g_fc.frame;
    g_fc.frame = true;
    for (const auto& op : ops) {
        const fc::TimingDecision d = FakeTiming();
        FakeOutcome o = d.timing == fc::Timing::Execute ? FakeExecute(op) : FakeOutcome{ DD_COMBAT_E_CANCELLED, 0, 0, "排队的操作没有执行" };
        DDCombatOpResult r{};
        r.size = sizeof(r);
        r.kind = op.kind;
        r.op = op.id;
        r.result = o.code;
        r.guid = o.guid;
        r.count = o.count;
        r.message = o.message.c_str();
        if (g_fc.handler && op.plugin == kPlugin) {
            SehCall(&CallOpHandler, &r);  // DDCore 在 SEH 隔离下调用，崩溃只写日志
        }
    }
    g_fc.frame = frame;
    return ops.size();
}

bool DD_CALL FakeSetOpHandler(DDPluginHandle plugin, DDCombatOpFn fn, void* user)
{
    if (plugin == kPlugin) {
        g_fc.handler = fn;
        g_fc.handlerUser = user;
    }
    return true;
}

bool DD_CALL FakeSafePoint()
{
    return FakeTiming().timing == fc::Timing::Execute;
}

// 同 DDCore 的 ReadSafe：加载线程在跑时读接口一律失败（A-S3），read_ready 为假（R2A-S1）
bool DD_CALL FakeReadReady()
{
    return !g_fc.loading;
}

uint32_t DD_CALL FakeEnumCount(uint32_t table)
{
    return fc::EnumCount(table);
}
const char* DD_CALL FakeEnumName(uint32_t table, int32_t value)
{
    return fc::EnumName(table, value);
}
int32_t DD_CALL FakeEnumValue(uint32_t table, const char* name)
{
    return name ? fc::EnumValue(table, name) : -1;
}
uint32_t DD_CALL FakeSourceFlags(int32_t source)
{
    return fc::SourceFlags(source);
}

bool DD_CALL FakeBuffList(const DDActorRef* ref, uint32_t* guids, uint32_t capacity, uint32_t* count)
{
    const std::uintptr_t actor = ref && !g_fc.loading ? FakeActorPtr(*ref) : 0;
    if (!actor) {
        return false;
    }
    const auto& table = g_fc.tables[actor];
    for (std::uint32_t i = 0; guids && i < capacity && i < table.size(); ++i) {
        guids[i] = table[i].guid;
    }
    if (count) {
        *count = static_cast<uint32_t>(table.size());
    }
    return true;
}

bool DD_CALL FakeBuffGet(const DDActorRef* ref, uint32_t guid, DDBuffInfo* out)
{
    const std::uintptr_t actor = ref && !g_fc.loading ? FakeActorPtr(*ref) : 0;
    if (!actor || !out || out->size < sizeof(DDBuffInfo)) {
        return false;
    }
    for (const auto& b : g_fc.tables[actor]) {
        if (b.guid != guid) {
            continue;
        }
        const std::uint32_t size = out->size;
        *out = {};
        out->size = size;
        out->guid = guid;
        strcpy_s(out->id, b.def.id);
        out->stat_type = b.def.stat_type;
        strcpy_s(out->stat_sub_type, b.def.stat_sub_type);
        out->amount = b.def.amount;
        out->duration = b.def.duration;
        out->duration_type = b.def.duration_type;
        out->source = b.def.source;
        out->source_flags = fc::SourceFlags(b.def.source);
        out->sourcedata = b.def.sourcedata;
        out->rule_type = b.def.rule_type;
        out->is_false_rule = b.def.is_false_rule;
        out->rule_float = b.def.rule_float;
        strcpy_s(out->rule_string, b.def.rule_string);
        out->by_plugin = !b.plugin.empty();
        strcpy_s(out->plugin, b.plugin.c_str());
        strcpy_s(out->tag, b.tag.c_str());
        return true;
    }
    return false;
}

bool DD_CALL FakeBuffDefGet(const char* id, DDBuffDef* out)
{
    if (!g_world.active || g_fc.loading || !id || !out) {
        return false;
    }
    const auto it = g_fc.library.find(id);
    if (it == g_fc.library.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

uint32_t DD_CALL FakeBuffAdd(DDPluginHandle plugin, const DDActorRef* actor, const char* id, const DDBuffAddOptions* options, DDCombatOpResult* out)
{
    DDBuffAddOptions o = options ? *options : DDBuffAddOptions{};
    const std::string tag = options && options->tag ? options->tag : "";
    std::string why;
    if (!actor || !id || !fc::ValidText(id, false) || !fc::ValidText(tag, true) || !fc::ValidateAddOptions(o, &why)) {
        return FakeOut(out, DD_COMBAT_OP_BUFF_ADD, DD_COMBAT_E_INVALID, why.empty() ? "id 或 tag 无效" : why);
    }
    FakeOp op;
    op.plugin = plugin;
    op.kind = DD_COMBAT_OP_BUFF_ADD;
    op.actor = *actor;
    op.text = id;
    op.tag = tag;
    o.tag = nullptr;
    op.add = o;
    return FakeSubmit(std::move(op), out);
}

uint32_t DD_CALL FakeBuffRemove(DDPluginHandle plugin, const DDActorRef* actor, const DDBuffRemoveSpec* spec, DDCombatOpResult* out)
{
    std::string why;
    if (!actor || !spec || !fc::ValidateRemoveSpec(*spec, &why)) {
        return FakeOut(out, DD_COMBAT_OP_BUFF_REMOVE, DD_COMBAT_E_INVALID, why);
    }
    FakeOp op;
    op.plugin = plugin;
    op.kind = DD_COMBAT_OP_BUFF_REMOVE;
    op.actor = *actor;
    op.remove = *spec;
    op.text = spec->text ? spec->text : "";
    op.remove.text = nullptr;
    return FakeSubmit(std::move(op), out);
}

uint32_t DD_CALL FakeBuffDefine(DDPluginHandle plugin, const DDBuffDef* def, DDCombatOpResult* out)
{
    std::string why;
    if (!def || !fc::ValidateBuffDef(*def, &why)) {
        return FakeOut(out, DD_COMBAT_OP_BUFF_DEFINE, DD_COMBAT_E_INVALID, why);
    }
    FakeOp op;
    op.plugin = plugin;
    op.kind = DD_COMBAT_OP_BUFF_DEFINE;
    op.def = *def;
    return FakeSubmit(std::move(op), out);
}

bool DD_CALL FakeEffectGet(const char* name, DDEffectInfo* out)
{
    if (!g_world.active || g_fc.loading || !name || !out) {
        return false;
    }
    const auto it = g_fc.effects.find(name);
    if (it == g_fc.effects.end()) {
        return false;
    }
    *out = it->second.info;
    return true;
}

uint32_t DD_CALL FakeEffectApply(DDPluginHandle plugin, const char* name, const DDActorRef* performer, const DDActorRef* target, DDCombatOpResult* out)
{
    if (!name || !performer || !fc::ValidText(name, false)) {
        return FakeOut(out, DD_COMBAT_OP_EFFECT_APPLY, DD_COMBAT_E_INVALID, "effect 名字无效");
    }
    FakeOp op;
    op.plugin = plugin;
    op.kind = DD_COMBAT_OP_EFFECT_APPLY;
    op.actor = *performer;
    op.hasTarget = target != nullptr;
    op.actor2 = target ? *target : *performer;
    op.text = name;
    return FakeSubmit(std::move(op), out);
}

uint32_t DD_CALL FakeEffectDefine(DDPluginHandle plugin, const char* text, DDCombatOpResult* out)
{
    std::vector<std::string> names;
    std::string why;
    if (!text || !fc::ExtractEffectNames(text, &names, &why)) {
        return FakeOut(out, DD_COMBAT_OP_EFFECT_DEFINE, DD_COMBAT_E_INVALID, why);
    }
    FakeOp op;
    op.plugin = plugin;
    op.kind = DD_COMBAT_OP_EFFECT_DEFINE;
    op.text = text;
    return FakeSubmit(std::move(op), out);
}

DDRuleId DD_CALL FakeRuleRegister(DDPluginHandle plugin, const char* name, DDRuleFn fn, void* user)
{
    if (g_fc.inRule || !name || !fn) {
        return 0;
    }
    for (const auto& r : g_fc.rules) {
        if (r.name == name || fc::GameHash(r.name) == fc::GameHash(name)) {
            return 0;
        }
    }
    g_fc.rules.push_back({ g_fc.nextRule, plugin, name, fn, user });
    return g_fc.nextRule++;
}

bool DD_CALL FakeRuleUnregister(DDPluginHandle plugin, DDRuleId id)
{
    if (g_fc.inRule) {
        return false;
    }
    const auto it = std::find_if(g_fc.rules.begin(), g_fc.rules.end(), [&](const FakeRuleReg& r) { return r.id == id && r.plugin == plugin; });
    if (it == g_fc.rules.end()) {
        return false;
    }
    g_fc.rules.erase(it);
    return true;
}

bool DD_CALL FakeRuleStats(DDRuleStats* out)
{
    if (!out) {
        return false;
    }
    out->rules = static_cast<uint32_t>(g_fc.rules.size());
    out->hits = g_fc.ruleCalls;
    return true;
}

bool DD_CALL FakeSkillFromPointer(const void* performer, const void* skill, DDSkillRef* out)
{
    DDActorRef ref{};
    ref.size = sizeof(ref);
    if (!out || skill != P(0x80000) || !FakeFromPointer(performer, &ref)) {
        return false;
    }
    *out = {};
    out->size = sizeof(DDSkillRef);
    out->actor = ref;
    strcpy_s(out->id, "smite");
    out->level = ref.kind == DD_ACTOR_HERO ? 2 : 0;
    return true;
}

bool DD_CALL FakeSkillInfo(const DDSkillRef* ref, DDSkillInfo* out)
{
    if (!ref || !out || !FakeResolve(&ref->actor) || std::strcmp(ref->id, "smite") != 0) {
        return false;
    }
    const std::uint32_t size = out->size;
    *out = {};
    out->size = size;
    strcpy_s(out->id, "smite");
    out->level = ref->level;
    strcpy_s(out->type, "melee");
    out->friendly = false;
    for (const auto& name : g_fc.smiteEffects) {
        if (out->effect_count < DD_SKILL_MAX_EFFECTS) {
            strcpy_s(out->effects[out->effect_count++], name.c_str());
        }
    }
    return true;
}

const DDCombatService kFakeCombat = { { sizeof(DDCombatService), DD_SERVICE_COMBAT_VERSION }, &FakeResultText, &FakeSetOpHandler, &FakeSafePoint,
    &FakeReadReady, &FakeEnumCount, &FakeEnumName, &FakeEnumValue, &FakeSourceFlags, &FakeBuffList, &FakeBuffGet, &FakeBuffDefGet, &FakeBuffAdd,
    &FakeBuffRemove, &FakeBuffDefine, &FakeEffectGet, &FakeEffectApply, &FakeEffectDefine, &FakeRuleRegister, &FakeRuleUnregister,
    &FakeRuleStats, &FakeSkillFromPointer, &FakeSkillInfo };

struct RuleCall {
    const FakeRuleReg* reg;
    const DDRuleContext* ctx;
    std::uint32_t raw;
};

void CallRule(void* raw)
{
    auto* call = static_cast<RuleCall*>(raw);
    call->raw = call->reg->fn(call->ctx, call->reg->user);
}

// 游戏判定一条带脚本规则的 buff（DDCore 的 RuleDetour）：命中注册表时调用回调（SEH 隔离，崩溃后停用），按 is_false_rule 取反。
// 返回 false 表示交给游戏（没有注册、停用、回调返回 DD_RULE_ORIGINAL）
bool FakeRuleEval(const char* name, std::uintptr_t self, std::uintptr_t target, int ruleType, bool falseRule, bool* result, const char* buffId = "lua_rule_buff")
{
    for (auto& reg : g_fc.rules) {
        if (reg.name != name || reg.disabled) {
            continue;
        }
        DDRuleContext ctx{};
        ctx.size = sizeof(ctx);
        ctx.rule = reg.name.c_str();
        ctx.actor = P(self);
        ctx.target = target ? P(target) : nullptr;
        ctx.subject = ruleType == 42 ? P(self) : ctx.target;
        ctx.rule_type = ruleType;
        ctx.is_false_rule = falseRule;
        ctx.rule_float = 0.5f;
        ctx.buff_id = buffId;
        RuleCall call{ &reg, &ctx, DD_RULE_ORIGINAL };
        ++g_fc.inRule;
        ++g_fc.ruleCalls;
        const bool ok = SehCall(&CallRule, &call);
        --g_fc.inRule;
        if (!ok) {
            reg.disabled = true;
            return false;
        }
        if (call.raw != DD_RULE_TRUE && call.raw != DD_RULE_FALSE) {
            ++g_fc.ruleOriginal;
            return false;  // 交还原函数
        }
        const bool holds = call.raw == DD_RULE_TRUE;
        *result = falseRule ? !holds : holds;
        return true;
    }
    return false;
}

// 游戏刷新属性（UpdateBuffsInternal 的第 2 步）：remove_if_not_active 为真、规则不成立的条目直接删掉（reason refresh）。
// 脚本规则按 FakeRuleEval；没有注册或交还原函数时按游戏原来的判定：认不出的状态字符串为假，地牢里再按 is_false_rule 取反，
// 城镇里（没有 Raid）规则 42 的前置检查直接返回假、不取反
void FakeRefresh(std::uintptr_t actor)
{
    std::vector<std::uint32_t> drop;
    for (const auto& b : g_fc.tables[actor]) {
        if (!b.def.remove_if_not_active || (b.def.rule_type != 42 && b.def.rule_type != 13)) {
            continue;
        }
        bool holds = false;
        if (!FakeRuleEval(b.def.rule_string, actor, 0, b.def.rule_type, b.def.is_false_rule, &holds, b.def.id)) {
            holds = g_fc.raid && b.def.rule_type == 42 ? b.def.is_false_rule : false;
        }
        if (!holds) {
            drop.push_back(b.guid);
        }
    }
    for (const auto guid : drop) {
        FakeGameRemove(actor, guid, "refresh");
    }
}

DDBuffDef FakeDef(const char* id, const char* stat, const char* sub, float amount, int duration, const char* dtype, const char* source)
{
    DDBuffDef def{};
    def.size = sizeof(def);
    strcpy_s(def.id, id);
    def.stat_type = fc::EnumValue(DD_ENUM_STAT, stat);
    strcpy_s(def.stat_sub_type, sub);
    def.amount = amount;
    def.duration = duration;
    def.duration_type = fc::EnumValue(DD_ENUM_DURATION, dtype);
    def.source = fc::EnumValue(DD_ENUM_SOURCE, source);
    return def;
}

void ResetCombat()
{
    g_fc = {};
    g_fc.library["SPD5"] = FakeDef("SPD5", "combat_stat_add", "speed_rating", 5, 3, "after_turn", "skill");
    g_fc.library["rakebuff_5"] = FakeDef("rakebuff_5", "combat_stat_add", "damage_low", 0.1f, 2, "combat_end", "skill");
    g_fc.library["QUIRK_X"] = FakeDef("QUIRK_X", "combat_stat_multiply", "protection_rating", 0.1f, -1, "till_removed", "quirk");
    for (const char* name : { "rakebuff_5", "Stun 1" }) {
        FakeEffect e;
        e.info.size = sizeof(e.info);
        strcpy_s(e.info.name, name);
        e.info.target = 1;
        e.info.chance = 1;
        e.info.on_hit = true;
        if (std::strcmp(name, "rakebuff_5") == 0) {
            e.buffs.push_back("rakebuff_5");
            e.info.buff_id_count = 1;
            e.info.buff_duration_type = fc::EnumValue(DD_ENUM_DURATION, "combat_end");
        } else {
            e.info.stun = 1;
        }
        g_fc.effects[name] = e;
    }
}

ddlua::Host MakeCombatHost()
{
    ddlua::Host host = MakeActorHost();
    host.combat = &kFakeCombat;
    host.info = &kFakeInfo;  // dd.fs 的数据目录在 g_fakeRoot\DDModLoader\data
    return host;
}

// 数据目录的根（测试前清空）
std::string P4bRoot()
{
    return ProjectFile("build/p4b_offline/game");
}

void RemoveTree(const std::wstring& dir)
{
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            const std::wstring path = dir + L"\\" + name;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                RemoveTree(path);
            } else if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirectoryW(path.c_str());  // 联接：只删联接本身
            } else {
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}

void ResetP4bRoot()
{
    const std::wstring root = ddlua::Utf8ToWide(P4bRoot());
    RemoveTree(root);
    CreateDirectoryW(ddlua::Utf8ToWide(ProjectFile("build")).c_str(), nullptr);
    CreateDirectoryW(ddlua::Utf8ToWide(ProjectFile("build/p4b_offline")).c_str(), nullptr);
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW((root + L"\\DDModLoader").c_str(), nullptr);
    g_fakeRoot = P4bRoot();
}

// 一个 P4b 测试的公共准备：主题表来自游戏（buff.*、damage.final、effect_name 都在），假世界、假 combat 服务
struct P4bSession {
    ddlua::Host host;
    std::unique_ptr<ddlua::Session> session;
    explicit P4bSession(std::initializer_list<const char*> mods, ddlua::Config config = TestConfig(), const char* name = "存档#P4b")
    {
        g_log.clear();
        CHECK(ResetHubFromDump());
        ResetWorld();
        ResetCombat();
        ResetP4bRoot();
        host = MakeCombatHost();
        session = std::make_unique<ddlua::Session>(host, config, name);
        g_now = 0;
        session->SetClock([] { return g_now; });
        session->Load(Sources(mods));
    }
    ddlua::Session& operator*() { return *session; }
    ddlua::Session* operator->() { return session.get(); }
};

// ---------------------------------------------------------------- F5：队伍、怪物、战斗状态的只读枚举
// dd.actor.party()、dd.actor.monsters() 是句柄数组（顺序同游戏的表），dd.actor.in_battle() 是布尔和战斗状态；
// 加载线程在跑时都返回 nil 和「加载中」开头的原因，城镇里（没有 Raid）是空表和 false
void TestActorEnum()
{
    P4bSession s({ "ev_a" });
    CHECK(s->LoadedCount() == 1);
    g_world.party = { 0x10100, 0x99999, 0x10000 };  // 0x99999 不是 actor：跳过（DDCore 按名册核对成员关系）
    CHECK(Lua(*s, "ev_a", R"lua(
        local p = dd.actor.party()
        assert(type(p) == "table" and #p == 2, tostring(p and #p))
        assert(rawequal(p[1], dd.actor.hero(13)) and rawequal(p[2], dd.actor.hero(12)), "顺序同队伍表，句柄驻留")
        local ms = dd.actor.monsters()
        assert(#ms == 2 and ms[1].id == 4500 and ms[2].id == 4501 and ms[1].is_monster and ms[1]:valid())
        local ib, st = dd.actor.in_battle()
        assert(ib == false and st == 0, tostring(ib))
        local function err(f) local ok, e = pcall(f) assert(not ok) return e end
        assert(type(dd.actor.party) == "function" and type(dd.actor.monsters) == "function")
    )lua"));
    g_world.battleState = 46;  // 读进战斗中的存档：游戏不派发 battle.start，状态已经非零
    CHECK(Lua(*s, "ev_a", "local ib, st = dd.actor.in_battle() assert(ib == true and st == 46)"));
    g_fc.loading = true;
    CHECK(Lua(*s, "ev_a", R"lua(
        local p, why = dd.actor.party()
        assert(p == nil and why:find("^加载中"), tostring(why))
        local ms, why2 = dd.actor.monsters()
        assert(ms == nil and why2:find("^加载中"))
        local ib, why3 = dd.actor.in_battle()
        assert(ib == nil and why3:find("^加载中"))
    )lua"));
    g_fc.loading = false;
    g_fc.raid = false;  // 城镇：没有 Raid
    g_world.battleState = 0;
    CHECK(Lua(*s, "ev_a", R"lua(
        local p = dd.actor.party()
        assert(type(p) == "table" and #p == 0)
        assert(#dd.actor.monsters() == 0 and dd.actor.in_battle() == false)
    )lua"));
    g_fc.raid = true;
    g_world.party.assign(80, 0x10000);  // 超过 64 个：只给前 64 个（DDCore 的上限是 16，这里只核对 DDLua 不越界）
    CHECK(Lua(*s, "ev_a", "assert(#dd.actor.party() == 64)"));
    g_world.active = false;  // 存档外（DDCore 返回 false）
    CHECK(Lua(*s, "ev_a", R"lua(
        local p, why = dd.actor.party()
        assert(p == nil and not why:find("^加载中") and why:find("读不到"), tostring(why))
        local ib, why2 = dd.actor.in_battle()
        assert(ib == nil and why2:find("读不到"))
    )lua"));
    g_world.active = true;
    s->Teardown();
}

// ---------------------------------------------------------------- 读：枚举、定义、快照、effect 句柄
void TestCombatReads()
{
    P4bSession s({ "ev_a", "ev_b" });
    CHECK(s->LoadedCount() == 2);
    CHECK(Lua(*s, "ev_a", R"lua(
        -- 枚举表
        assert(dd.buff.enum("stat", "combat_stat_add") == 2 and dd.buff.enum("stat", 2) == "combat_stat_add")
        assert(dd.buff.enum("source", "bsrc_trinket") == 6 and dd.buff.enum("source", "trinket") == 6 and dd.buff.enum("source", 1) == "skill")
        assert(#dd.buff.enum("rule") == 55 and dd.buff.enum("rule")[43] == "is_actor_status" and dd.buff.enum("duration", "after_turn") == 9)
        assert(dd.buff.enum("duration_type", 999) == nil and dd.buff.enum("stat", "nope") == nil)
        assert(not pcall(dd.buff.enum, "nope") and not pcall(dd.buff.enum, "stat", 1.5))
        -- 定义
        local def = assert(dd.buff.def("SPD5"))
        assert(def.stat == "combat_stat_add" and def.sub_stat == "speed_rating" and def.amount == 5 and def.duration == 3)
        assert(def.duration_type == "after_turn" and def.source == "skill" and def.rule == "always" and def.script == false and def.mod == nil)
        local none, why = dd.buff.def("nope")
        assert(none == nil and why:find("没有 nope"), why)
        -- effect 句柄
        local e = assert(dd.effect.get("rakebuff_5"))
        assert(e.name == "rakebuff_5" and e.target == 1 and e.chance == 1 and e.on_hit == true and e.buff_id_count == 1 and e.runtime == false)
        assert(e.buff_duration_type == "combat_end" and e:valid() and dd.effect.get("rakebuff_5") == e and rawequal(dd.effect.get("rakebuff_5"), e))
        assert(tostring(e) == "dd.effect(rakebuff_5)" and not pcall(function() e.chance = 2 end) and not pcall(function() return e.nope end))
        local missing, reason = dd.effect.get("nope")
        assert(missing == nil and reason:find("不存在"), reason)
        -- 快照：一开始没有 buff
        local h = dd.actor.hero(12)
        assert(#h:buffs() == 0 and h:buff(1) == nil)
    )lua"));
    // 游戏自己加的一条（读档恢复的这类条目不派发 buff.add）
    const std::uint32_t quirk = FakeGameAdd(0x10000, "QUIRK_X", false);
    CHECK(Lua(*s, "ev_a", ("QUIRK = " + std::to_string(quirk)).c_str()));
    CHECK(Lua(*s, "ev_a", R"lua(
        local h = dd.actor.hero(12)
        local list = h:buffs()
        assert(#list == 1)
        local b = list[1]
        assert(b.guid == QUIRK and b.id == "QUIRK_X" and b.stat == "combat_stat_multiply" and b.sub_stat == "protection_rating")
        assert(b.duration == -1 and b.duration_type == "till_removed" and b.source == "quirk" and b.rule == "always")
        assert(b.source_flags.saved == false and b.source_flags.town_clear == false and b.source_flags.tray == false)
        assert(b.plugin == nil and b.mod == nil and b.tag == nil and b.mine == false and b.script == false and b.actor == h)
        assert(h:buff(QUIRK).id == "QUIRK_X")
        assert(#h:buffs{ id = "QUIRK_X" } == 1 and #h:buffs{ id = "SPD5" } == 0 and #h:buffs{ source = "quirk" } == 1 and #h:buffs{ source = 5 } == 1)
        assert(#h:buffs{ stat = "combat_stat_multiply" } == 1 and #h:buffs{ mine = true } == 0 and #h:buffs{ mine = false } == 1)
        assert(not pcall(h.buffs, h, { nope = 1 }) and not pcall(h.buffs, h, { source = "nope" }) and not pcall(h.buffs, h, { id = 1 }))
        M = dd.actor.monster(4500)
        assert(M and #M:buffs() == 0)
    )lua"));
    // 失效的句柄：nil 和原因
    g_world.monsters.clear();
    CHECK(Lua(*s, "ev_a", R"lua(
        local list, why = M:buffs()
        assert(list == nil and why:find("解析失败"), tostring(why))
        assert(M:buff(1) == nil)
    )lua"));
    // 存档外：定义、effect 读不到
    g_world.active = false;
    CHECK(Lua(*s, "ev_a", R"lua(
        assert(dd.buff.def("SPD5") == nil and dd.effect.get("rakebuff_5") == nil)
    )lua"));
    g_world.active = true;
    s->Teardown();
    CHECK(CoreCount() == 0 && s->memory().residual == 0);
}

// ---------------------------------------------------------------- 写：定义、添加、删除、标签、拒绝、排队和 done
void TestCombatWrites()
{
    P4bSession s({ "ev_a", "ev_b" });
    CHECK(Lua(*s, "ev_a", R"lua(
        -- 定义：立即执行返回 true 和 id
        local ok, id = dd.buff.define{ id = "lua_spd", stat = "combat_stat_add", sub_stat = "speed_rating", amount = 2, duration = 3,
            duration_type = "after_turn" }
        assert(ok == true and id == "lua_spd", tostring(id))
        local def = dd.buff.def("lua_spd")
        assert(def.amount == 2 and def.source == "skill" and def.mod == "ev_a" and def.script == false)
        -- 同一个 Mod 重复定义：更新
        assert(dd.buff.define{ id = "lua_spd", stat = 2, sub_stat = "speed_rating", amount = 3, duration = 3, duration_type = 9 })
        assert(dd.buff.def("lua_spd").amount == 3)
        -- 游戏数据：拒绝
        local no, why = dd.buff.define{ id = "SPD5", stat = "combat_stat_add", sub_stat = "speed_rating" }
        assert(no == nil and why:find("游戏数据"), why)
        -- 参数写错：报错
        for _, bad in ipairs({ { stat = "combat_stat_add" }, { id = "x" }, { id = "x", stat = "nope" }, { id = "x", stat = 2, duration = 0 },
                { id = "x", stat = 2, nope = 1 }, { id = "x", stat = 2, amount = "1" }, { id = ("x"):rep(64), stat = 2 }, { id = "a\"b", stat = 2 },
                { id = "x", stat = 2, on_apply = 1 }, { id = "x", stat = 2, source = 99 } }) do
            assert(not pcall(dd.buff.define, bad))
        end
        -- C-Y3：sub_stat 按 stat 校验（DDCore 的 ValidateBuffDef），认不出返回 nil 和原因
        local no2, why2 = dd.buff.define{ id = "lua_typo", stat = "combat_stat_add", sub_stat = "speed" }
        assert(no2 == nil and why2:find("speed_rating"), tostring(why2))
        no2, why2 = dd.buff.define{ id = "lua_typo", stat = "resistance", sub_stat = "deathblow" }
        assert(no2 == nil and why2:find("death_blow"), tostring(why2))
        assert(dd.buff.define{ id = "lua_resist", stat = "resistance", sub_stat = "death_blow", amount = 0.1 })
        assert(dd.buff.define{ id = "Ab", stat = "tag" })
    )lua"));
    // 别的 Mod 定义同一个 id、哈希相同的 id：拒绝
    CHECK(Lua(*s, "ev_b", R"lua(
        local no, why = dd.buff.define{ id = "lua_spd", stat = "combat_stat_add", sub_stat = "speed_rating" }
        assert(no == nil and why:find("Lua Mod ev_a"), why)
        no, why = dd.buff.define{ id = "B-", stat = "tag" }
        assert(no == nil and why:find("哈希相同"), why)
    )lua"));
    CHECK(Lua(*s, "ev_a", R"lua(
        local h = dd.actor.hero(12)
        -- 添加：立即执行返回 guid
        local ok, g1 = h:add_buff("lua_spd", { tag = "round" })
        assert(ok == true and math.type(g1) == "integer", tostring(g1))
        local ok2, g2 = h:add_buff("lua_spd")
        local ok3, g3 = h:add_buff("SPD5", { duration = 7, source = "trinket", sourcedata = 4, tag = "round", refresh = false })
        assert(ok2 and ok3 and g2 > g1 and g3 > g2)
        local b = h:buff(g1)
        assert(b.id == "lua_spd" and b.mod == "ev_a" and b.tag == "round" and b.mine == true and b.plugin == "DDLua")
        assert(h:buff(g2).tag == nil and h:buff(g2).mine == true)
        local b3 = h:buff(g3)
        assert(b3.duration == 7 and b3.source == "trinket" and b3.sourcedata == 4)
        assert(#h:buffs{ tag = "round" } == 2 and #h:buffs{ mine = true } == 3 and #h:buffs{ id = "lua_spd" } == 2)
        G1, G2, G3 = g1, g2, g3
        -- 被拒绝：nil 和原因
        local no, why = h:add_buff("nope")
        assert(no == nil and why:find("没有 nope"), why)
        -- 参数写错：报错
        assert(not pcall(h.add_buff, h, "lua_spd", { nope = 1 }) and not pcall(h.add_buff, h, "lua_spd", { duration = 0 }))
        assert(not pcall(h.add_buff, h, "lua_spd", { state = {} }))  -- 不是脚本 Buff
        assert(not pcall(h.add_buff, h, "lua_spd", { tag = ("t"):rep(61) }))  -- "ev_a:" 加 tag 超过 63 字节
        assert(h:add_buff("lua_spd", { tag = ("t"):rep(58) }) and h:remove_buffs{ tag = ("t"):rep(58) })  -- 正好 63 字节
        assert(not pcall(h.add_buff, h, 1) and not pcall(h.add_buff, h, "lua_spd", { source = "nope" }))
        assert(not pcall(function() h.hp = 1 end))
    )lua"));
    // ev_b 用同样的标签：看不到、删不到 ev_a 的
    CHECK(Lua(*s, "ev_b", R"lua(
        local h = dd.actor.hero(12)
        assert(#h:buffs{ tag = "round" } == 0 and #h:buffs{ mine = true } == 0 and h:buff(G1 or 1).mine == false)
        local ok, n = h:remove_buffs{ tag = "round" }
        assert(ok == true and n == 0)
    )lua"));
    CHECK(Lua(*s, "ev_a", R"lua(
        local h = dd.actor.hero(12)
        local ok, n = h:remove_buffs{ tag = "round" }
        assert(ok and n == 2, n)
        ok, n = h:remove_buffs{ guid = G2 }
        assert(ok and n == 1)
        ok, n = h:remove_buffs{ guid = G2 }
        assert(ok and n == 0)
        assert(h:add_buff("SPD5") and h:add_buff("SPD5"))
        ok, n = h:remove_buffs{ id = "SPD5" }
        assert(ok and n == 2)
        assert(h:add_buff("SPD5", { source = "curio" }))
        ok, n = h:remove_buffs{ source = "curio" }
        assert(ok and n == 1 and #h:buffs() == 0)
        assert(not pcall(h.remove_buffs, h, {}) and not pcall(h.remove_buffs, h, { guid = 1, id = "x" }) and not pcall(h.remove_buffs, h, { nope = 1 }))
        assert(not pcall(h.remove_buffs, h))
    )lua"));
    // 存档外、加载中、PvP：拒绝
    g_fc.loading = true;
    CHECK(Lua(*s, "ev_a", R"lua(
        local no, why = dd.actor.hero(12):add_buff("SPD5")
        assert(no == nil and why:find("加载"), why)
        -- 读接口在加载中给出「加载中」开头的原因，和「没有」分开（审核 R2A-S1）
        local h = dd.actor.hero(12)
        local list, lwhy = h:buffs()
        assert(list == nil and lwhy:find("^加载中"), tostring(lwhy))
        local b, bwhy = h:buff(1)
        assert(b == nil and bwhy:find("^加载中"), tostring(bwhy))
        local d, dwhy = dd.buff.def("SPD5")
        assert(d == nil and dwhy:find("^加载中"), tostring(dwhy))
        local e, ewhy = dd.effect.get("rakebuff_5")
        assert(e == nil and ewhy:find("^加载中"), tostring(ewhy))
        local st, swhy = dd.buff.state(h, 424242)
        assert(st == nil and swhy:find("^加载中"), tostring(swhy))
    )lua"));
    g_fc.loading = false;
    CHECK(Lua(*s, "ev_a", R"lua(
        -- 加载结束：照常读；条目不在时只返回 nil（没有第二个返回值）
        local h = dd.actor.hero(12)
        assert(type(h:buffs()) == "table" and dd.buff.def("SPD5") ~= nil)
        assert(select("#", h:buff(424242)) == 1 and h:buff(424242) == nil)
        local d, dwhy = dd.buff.def("nope_def")
        assert(d == nil and not dwhy:find("加载中"), dwhy)
        assert(select("#", dd.buff.state(h, 424242)) == 1)
    )lua"));
    g_fc.pvp = true;
    CHECK(Lua(*s, "ev_a", R"lua(
        local no, why = dd.buff.define{ id = "lua_pvp", stat = 2, sub_stat = "speed_rating" }
        assert(no == nil and why:find("PvP"), why)
    )lua"));
    g_fc.pvp = false;
    g_world.active = false;
    CHECK(Lua(*s, "ev_a", R"lua(
        local no, why = dd.buff.define{ id = "lua_out", stat = 2, sub_stat = "speed_rating" }
        assert(no == nil and why:find("不在存档里"), why)
        local n2, w2 = dd.effect.define('effect: .name "lua_out_fx" .target "performer"\n')
        assert(n2 == nil and w2:find("不在存档里"), w2)
    )lua"));
    g_world.active = true;

    // 事件回调里：排队，结果经 done 在下一个帧边界送达；回调里写的 done 计入订阅
    CHECK(Lua(*s, "ev_a", R"lua(
        DONE = {}
        SUB = dd.on("damage.pre", function(ev)
            local h = dd.actor.hero(12)
            local ok, r = h:add_buff("lua_spd", { tag = "queued", done = function(ok, g) DONE.add = { ok, g } end })
            assert(ok == true and r == nil)
            ok, r = h:remove_buffs{ tag = "queued", done = function(ok, n) DONE.remove = { ok, n } end }
            assert(ok == true and r == nil)
            ok, r = dd.buff.define{ id = "lua_q", stat = 2, sub_stat = "speed_rating", done = function(ok, id) DONE.define = { ok, id } end }
            assert(ok == true and r == nil)
            ok, r = h:add_buff("nope", { done = function(ok, why) DONE.fail = { ok, why } end })
            assert(ok == true and r == nil)
            local e = dd.effect.get("rakebuff_5")
            ok, r = e:apply(h, nil, { done = function(ok, res) DONE.apply = { ok, res } end })
            assert(ok == true and r == nil)
            ok, r = dd.effect.define('effect: .name "lua_qfx" .target "performer"\n', { done = function(ok, n) DONE.fx = { ok, n } end })
            assert(ok == true and r == nil)
            assert(dd.stats().pending == 6, dd.stats().pending)
        end, { filter = { is_hero = true } })
    )lua"));
    Fire dmg("damage.pre");
    dmg.P("target", P(0x10000)).Fl("damage", 3).B("is_hero", true);
    dmg.Go();
    CHECK(g_fc.queue.size() == 6 && s->combat().PendingFor(nullptr) == 6);
    CHECK(Lua(*s, "ev_a", "assert(next(DONE) == nil) SUB:off()"));
    CHECK(FakeDrain() == 6);
    CHECK(Lua(*s, "ev_a", R"lua(
        assert(DONE.add[1] == true and math.type(DONE.add[2]) == "integer", tostring(DONE.add[2]))
        assert(DONE.remove[1] == true and DONE.remove[2] == 1)
        assert(DONE.define[1] == true and DONE.define[2] == "lua_q" and dd.buff.def("lua_q").mod == "ev_a")
        assert(DONE.fail[1] == nil and DONE.fail[2]:find("没有 nope"), DONE.fail[2])
        assert(DONE.apply[1] == true and DONE.apply[2].count == 1)
        assert(DONE.fx[1] == true and DONE.fx[2] == 1 and dd.effect.get("lua_qfx").runtime == true)
        assert(dd.stats().pending == 0)
    )lua"));
    CHECK(s->combat().PendingFor(nullptr) == 0);
    // 排队的定义失败（别的插件先注册了）：撤销登记
    g_fc.library["lua_race"] = FakeDef("lua_race", "combat_stat_add", "speed_rating", 1, 1, "after_turn", "skill");
    g_fc.libOwner["lua_race"] = kOtherPlugin;
    CHECK(Lua(*s, "ev_a", R"lua(
        RACE = {}
        dd.once("damage.pre", function()
            dd.buff.define{ id = "lua_race", stat = 2, sub_stat = "speed_rating", on_apply = function() end, done = function(ok, why) RACE = { ok, why } end }
        end)
    )lua"));
    dmg.Go();
    FakeDrain();
    CHECK(Lua(*s, "ev_a", R"lua(
        assert(RACE[1] == nil and RACE[2]:find("别的插件"), tostring(RACE[2]))
        assert(dd.buff.def("lua_race").mod == nil and dd.buff.def("lua_race").script == false)
    )lua"));
    // C-N5：排队的「更新已有定义」失败时，原来登记的回调保留（DDCore 里原来的定义还在）
    CHECK(Lua(*s, "ev_a", R"lua(
        assert(dd.buff.define{ id = "lua_keep", stat = 2, sub_stat = "speed_rating", on_apply = function() end })
        assert(dd.buff.def("lua_keep").script == true)
        KEEP = {}
        dd.once("damage.pre", function()
            dd.buff.define{ id = "lua_keep", stat = 2, sub_stat = "speed_rating", on_apply = function() end,
                done = function(ok, why) KEEP = { ok, why } end }
        end)
    )lua"));
    g_fc.libOwner["lua_keep"] = kOtherPlugin;  // 执行时 DDCore 拒绝（模拟失败）
    dmg.Go();
    FakeDrain();
    CHECK(Lua(*s, "ev_a", R"lua(
        assert(KEEP[1] == nil and KEEP[2], tostring(KEEP[2]))
        assert(dd.buff.def("lua_keep").mod == "ev_a" and dd.buff.def("lua_keep").script == true)
    )lua"));
    // R2A-N6：同一个新 id 排队定义两次、两次都失败：第一次把「新建」交给第二次，第二次撤销登记，
    // 不留下 DDCore 里没有对应定义的槽（别的 Mod 之后还能定义这个 id）
    g_fc.library["lua_twice"] = FakeDef("lua_twice", "combat_stat_add", "speed_rating", 1, 1, "after_turn", "skill");
    g_fc.libOwner["lua_twice"] = kOtherPlugin;
    CHECK(Lua(*s, "ev_a", R"lua(
        TWICE = {}
        dd.once("damage.pre", function()
            for i = 1, 2 do
                local ok, r = dd.buff.define{ id = "lua_twice", stat = 2, sub_stat = "speed_rating", on_apply = function() end,
                    done = function(okd, why) TWICE[#TWICE + 1] = { okd, why } end }
                assert(ok == true and r == nil)
            end
        end)
    )lua"));
    dmg.Go();
    FakeDrain();
    CHECK(Lua(*s, "ev_a", R"lua(
        assert(#TWICE == 2 and TWICE[1][1] == nil and TWICE[2][1] == nil, #TWICE)
        local d = dd.buff.def("lua_twice")
        assert(d.mod == nil and d.script == false, tostring(d.mod))
    )lua"));
    g_fc.library.erase("lua_twice");
    g_fc.libOwner.erase("lua_twice");
    CHECK(Lua(*s, "ev_b", R"lua(
        local ok, r = dd.buff.define{ id = "lua_twice", stat = 2, sub_stat = "speed_rating", on_apply = function() end }
        assert(ok == true and r == "lua_twice", tostring(r))
    )lua"));
    // 每个 Mod 最多排 128 个
    CHECK(Lua(*s, "ev_b", R"lua(
        dd.once("damage.pre", function()
            local h = dd.actor.hero(13)
            local accepted, refused = 0, nil
            for i = 1, 140 do
                local ok, why = h:add_buff("SPD5")
                if ok then accepted = accepted + 1 else refused = refused or why end
            end
            QFULL = { accepted, refused }
        end)
    )lua"));
    dmg.Go();
    CHECK(Lua(*s, "ev_b", "assert(QFULL[1] == 128 and QFULL[2]:find('超过 128'), tostring(QFULL[2]))"));
    FakeDrain();
    CHECK(g_fc.tables[0x10100].size() == 128);
    // 退出存档前没执行的：丢弃（会话拆除后操作结果不再送达）
    CHECK(Lua(*s, "ev_b", R"lua(
        dd.once("damage.pre", function() dd.actor.hero(13):add_buff("SPD5", { done = function() LATE = true end }) end)
    )lua"));
    dmg.Go();
    s->Teardown();
    FakeDrain();
    CHECK(CoreCount() == 0 && s->memory().residual == 0 && !LogHas("出错"));
}

// ---------------------------------------------------------------- effect：施加、文本定义、脚本 Effect、技能句柄
void TestCombatEffects()
{
    P4bSession s({ "ev_a", "ev_b" });
    CHECK(Lua(*s, "ev_a", R"lua(
        local h, h2 = dd.actor.hero(12), dd.actor.hero(13)
        -- 立即执行：结果是表，count 是新增的条数，数组是 performer、target 身上新增的条目
        local ok, r = dd.effect.get("rakebuff_5"):apply(h, h2)
        assert(ok == true and r.count == 1 and #r == 1 and r[1].actor == h2 and r[1].id == "rakebuff_5" and h2:buff(r[1].guid))
        ok, r = dd.effect.get("Stun 1"):apply(h)
        assert(ok and r.count == 0 and #r == 0)
        assert(not pcall(dd.effect.get("Stun 1").apply, dd.effect.get("Stun 1"), "x"))
        -- 文本定义
        ok, r = dd.effect.define('effect: .name "lua_fx_a" .target "performer" .buff_ids SPD5\r\neffect: .name "lua_fx_b" .target "target"\r\n')
        assert(ok and r == 2, tostring(r))
        assert(dd.effect.get("lua_fx_a").runtime and dd.effect.get("lua_fx_b"))
        ok, r = dd.effect.get("lua_fx_a"):apply(h)
        assert(ok and r.count == 1 and r[1].id == "SPD5")
        assert(dd.effect.define('effect: .name "lua_fx_a" .target "performer"\n'))  -- 重新定义自己的
        local no, why = dd.effect.define('effect: .name "rakebuff_5" .target "performer"\n')
        assert(no == nil and why:find("游戏数据"), why)
        assert(not pcall(dd.effect.define, "no blocks here") and not pcall(dd.effect.define, 'effect: .target "performer"\n'))
        assert(not pcall(dd.effect.define, 'effect: .name "a"\neffect: .name "a"\n') and not pcall(dd.effect.define, ""))
        assert(not pcall(dd.effect.define, 'effect: .name "a"\n', { nope = 1 }))
        -- A-S1：按游戏的规则取 .name（最后一次出现，紧跟引号也算），改写游戏数据的企图被认出来
        no, why = dd.effect.define('effect: .name "lua_mine" .target "performer" .name"rakebuff_5"')
        assert(no == nil and why:find("rakebuff_5"), why)
        -- A-B1：块达到 1024 字节、值里的冒号开出新块，在交给游戏之前报错
        local okb, err = pcall(dd.effect.define, 'effect: .name "lua_long" .target "performer"' .. (" "):rep(1000) .. "\n")
        assert(not okb and err:find("1023"), err)
        okb, err = pcall(dd.effect.define, 'effect: .name "lua_colon" .set_mode x:y\n')
        assert(not okb and err:find("不是 effect"), err)
    )lua"));
    CHECK(Lua(*s, "ev_b", R"lua(
        local no, why = dd.effect.define('effect: .name "lua_fx_b" .target "performer"\n')
        assert(no == nil and why:find("Lua Mod ev_a"), why)
    )lua"));
    // 城镇：施加被拒绝
    g_fc.raid = false;
    CHECK(Lua(*s, "ev_a", R"lua(
        local no, why = dd.effect.get("rakebuff_5"):apply(dd.actor.hero(12))
        assert(no == nil and why:find("城镇"), why)
    )lua"));
    g_fc.raid = true;

    // 脚本 Effect：名字匹配在 C++ 里做，不匹配的不进入 Lua
    CHECK(Lua(*s, "ev_a", R"lua(
        HITS = { exact = 0, prefix = 0, all = 0 }
        EXACT = dd.effect.register("shield_gain_37", function(ev)
            HITS.exact = HITS.exact + 1
            assert(ev.effect.name == "shield_gain_37" and ev.effect_name == "shield_gain_37" and ev.performer == dd.actor.hero(12))
        end)
        PREFIX = dd.effect.register("shield_gain_*", function(ev)
            HITS.prefix = HITS.prefix + 1
            LAST = ev.effect_name
            if ev.effect_name == "shield_gain_99" then ev:cancel() end
            -- 回调里的写操作排队
            local ok, r = ev.target:add_buff("SPD5")
            assert(ok == true and r == nil)
        end, { priority = 5 })
        ALL = dd.effect.register("*", function(ev) HITS.all = HITS.all + 1 end)
        assert(EXACT and PREFIX and ALL and PREFIX.topic == "effect.pre" and dd.subscriptions() == 3)
        for _, bad in ipairs({ "", "a*b", "**", ("x"):rep(64), 'a"b' }) do assert(not pcall(dd.effect.register, bad, function() end), bad) end
        assert(not pcall(dd.effect.register, "x", 1) and not pcall(dd.effect.register, "x", function() end, { nope = 1 }))
    )lua"));
    const auto fireEffect = [](const char* name) {
        Fire pre("effect.pre");
        pre.P("performer", P(0x10000)).P("target", P(0x10100)).P("effect", P(0x81000)).S("effect_name", name);
        return pre.Go();
    };
    fireEffect("shield_gain_37");
    fireEffect("shield_gain_12");
    const Payload cancelled = fireEffect("shield_gain_99");
    fireEffect("rakebuff_5");
    CHECK(cancelled.cancelled);
    CHECK(Lua(*s, "ev_a", R"lua(
        assert(HITS.exact == 1 and HITS.prefix == 3 and HITS.all == 4 and LAST == "shield_gain_99", HITS.prefix)
        PREFIX:off()
    )lua"));
    CHECK(g_fc.queue.size() == 3);
    FakeDrain();
    fireEffect("shield_gain_5");
    CHECK(Lua(*s, "ev_a", "assert(HITS.prefix == 3 and HITS.all == 5)"));
    // 出错的脚本 Effect 和普通订阅一样连续 3 次停用
    CHECK(Lua(*s, "ev_b", R"lua(BAD = dd.effect.register("boom_*", function() error("故意") end))lua"));
    for (int i = 0; i < 4; ++i) {
        fireEffect("boom_1");
    }
    CHECK(CountLog("dd.effect.register(\"boom_*\") 的回调出错") == 3 && LogHas("已停用该订阅"));
    CHECK(Lua(*s, "ev_b", "assert(not BAD:active())"));

    // 技能句柄（skill.pre 的 skill 字段）
    CHECK(Lua(*s, "ev_a", R"lua(
        SKILL = nil
        dd.once("skill.pre", function(ev)
            local sk = ev.skill
            assert(sk and sk.id == "smite" and sk.level == 2 and sk.type == "melee" and sk.friendly == false and sk.performer == dd.actor.hero(12))
            assert(#sk.effects == 2 and sk.effects[1] == "Stun 1" and sk:valid() and tostring(sk):find("smite"))
            assert(not pcall(function() sk.level = 3 end) and not pcall(function() return sk.nope end))
            SKILL = sk
        end)
        dd.spawn(function() SNAP = dd.wait_event("skill.post") end)
    )lua"));
    for (const char* name : { "skill.pre", "skill.post" }) {
        Fire f(name);
        f.P("performer", P(0x10000)).P("skill", P(0x80000)).P("target", P(0x20000)).I("target_count", 1).B("friendly", false);
        f.Go();
    }
    Tick(*s, 0.1);
    CHECK(Lua(*s, "ev_a", R"lua(
        assert(SKILL and SKILL.id == "smite")
        assert(SNAP and SNAP.skill and SNAP.skill.id == "smite" and SNAP.skill == SKILL)
    )lua"));
    // 不在职业技能表里的指针：nil
    CHECK(Lua(*s, "ev_a", R"lua(dd.once("skill.pre", function(ev) NOSKILL = ev.skill == nil end))lua"));
    Fire other("skill.pre");
    other.P("performer", P(0x10000)).P("skill", P(0x80100));
    other.Go();
    CHECK(Lua(*s, "ev_a", "assert(NOSKILL)"));
    s->Teardown();
    CHECK(CoreCount() == 0 && s->memory().residual == 0);
}

// ---------------------------------------------------------------- 脚本 Buff：状态的绑定、回调、丢弃
void TestScriptBuff()
{
    P4bSession s({ "ev_a", "ev_b" });
    CHECK(Lua(*s, "ev_a", R"lua(
        CALLS = {}
        local function log(kind, b, st, extra) CALLS[#CALLS + 1] = { kind = kind, guid = b.guid, id = b.id, actor = b.actor, state = st, extra = extra } end
        assert(dd.buff.define{ id = "lua_shield", stat = "combat_stat_add", sub_stat = "protection_rating", amount = 0, duration = 3,
            duration_type = "after_turn",
            on_apply = function(b, st) st.applied = (st.applied or 0) + 1 log("apply", b, st) end,
            on_remove = function(b, st, reason) log("remove", b, st, reason) end,
            on_tick = function(b, st, before, after) st.ticks = (st.ticks or 0) + 1 log("tick", b, st, { before, after }) end,
            on_damage = function(b, st, ev)
                log("damage", b, st, ev.damage)
                if st.shield and st.shield > 0 then
                    local absorbed = math.min(st.shield, ev.damage)
                    st.shield = st.shield - absorbed
                    ev.damage = ev.damage - absorbed
                end
            end })
        assert(dd.buff.def("lua_shield").script == true)
        function LAST() return CALLS[#CALLS] end
    )lua"));
    CHECK(s->combat().InternalSubscriptions() == 4);
    // 1. 立即执行的添加（hook 已装上）：同步派发的 buff.add 里绑定初始状态，on_apply 在 add_buff 返回之前
    CHECK(Lua(*s, "ev_a", R"lua(
        local h = dd.actor.hero(12)
        local ok, g = h:add_buff("lua_shield", { state = { shield = 4, tags = { "a", "b" } }, tag = "s1" })
        assert(ok and #CALLS == 1 and LAST().kind == "apply" and LAST().guid == g and LAST().state.shield == 4 and LAST().state.applied == 1)
        assert(dd.buff.state(h, g) == LAST().state and h:buff(g).script == true and dd.stats().states == 1)
        S1 = g
    )lua"));
    // 2. hook 还没装上（定义后的第一帧）：buff_add 返回之后绑定、调用 on_apply，只调用一次
    g_fc.hooks = false;
    CHECK(Lua(*s, "ev_a", R"lua(
        local ok, g = dd.actor.hero(12):add_buff("lua_shield", { state = { shield = 1 } })
        assert(ok and #CALLS == 2 and LAST().guid == g and LAST().state.shield == 1 and LAST().state.applied == 1)
        S2 = g
    )lua"));
    g_fc.hooks = true;
    // 3. 排队的添加：结果送达时绑定初始状态（buff.add 里先看到的那次不当作游戏加的）
    CHECK(Lua(*s, "ev_a", R"lua(
        dd.once("damage.post", function()
            local ok, r = dd.actor.hero(13):add_buff("lua_shield", { state = { shield = 9 }, done = function(ok, g) S3 = g end })
            assert(ok and r == nil)
        end)
    )lua"));
    Fire post("damage.post");
    post.P("target", P(0x10100)).Fl("damage", 1);
    post.Go();
    CHECK(Lua(*s, "ev_a", "assert(#CALLS == 2 and S3 == nil)"));
    FakeDrain();
    CHECK(Lua(*s, "ev_a", R"lua(
        assert(S3 and #CALLS == 3 and LAST().guid == S3 and LAST().state.shield == 9 and LAST().state.applied == 1, #CALLS)
        assert(dd.buff.state(dd.actor.hero(13), S3).shield == 9)
    )lua"));
    // 4. 游戏自己加的同 id 载体（数据层的 effect 引用了这个 id）：buff.add 里绑定空表
    const std::uint32_t game = FakeGameAdd(0x10100, "lua_shield");
    CHECK(Lua(*s, "ev_a", ("GAME = " + std::to_string(game)).c_str()));
    CHECK(Lua(*s, "ev_a", R"lua(
        assert(#CALLS == 4 and LAST().guid == GAME and LAST().state.applied == 1 and LAST().state.shield == nil)
    )lua"));
    // 5. 读档恢复的载体（不派发 buff.add）：第一次用到时绑定空表，不调用 on_apply
    const std::uint32_t loaded = FakeGameAdd(0x10000, "lua_shield", false);
    CHECK(Lua(*s, "ev_a", ("LOADED = " + std::to_string(loaded)).c_str()));
    CHECK(Lua(*s, "ev_a", R"lua(
        local st = dd.buff.state(dd.actor.hero(12), LOADED)
        assert(st and next(st) == nil and #CALLS == 4)
        assert(dd.buff.state(dd.actor.hero(12), 9999) == nil and dd.buff.state(dd.actor.hero(12), S1).shield == 4)
        assert(not pcall(dd.buff.state, "x", 1) and not pcall(dd.buff.state, dd.actor.hero(12), -1))
    )lua"));
    // 6. on_tick
    FakeTickBuff(0x10000, 1);  // S1 的 guid 是 1
    CHECK(Lua(*s, "ev_a", R"lua(
        assert(LAST().kind == "tick" and LAST().guid == S1 and LAST().extra[1] == 3 and LAST().extra[2] == 2 and LAST().state.ticks == 1)
    )lua"));
    // 7. on_damage：载体所在的 actor 受到伤害，可写（S1 护盾 4、S2 护盾 1，按表的顺序各吸收一次）
    Fire pre("damage.pre");
    pre.P("target", P(0x10000)).Fl("damage", 6).B("is_hero", true);
    pre.Go();
    CHECK(pre.At("damage").v.f == 1.0);
    CHECK(Lua(*s, "ev_a", R"lua(
        assert(dd.buff.state(dd.actor.hero(12), S1).shield == 0 and dd.buff.state(dd.actor.hero(12), S2).shield == 0)
        local n = 0 for _, c in ipairs(CALLS) do if c.kind == "damage" then n = n + 1 end end
        assert(n == 3, n)  -- hero#12 身上 S1、S2、LOADED 三条载体
    )lua"));
    // 8. 到期删除：on_remove(expired)，状态丢弃
    FakeTickBuff(0x10000, 1);
    FakeTickBuff(0x10000, 1);
    CHECK(Lua(*s, "ev_a", R"lua(
        assert(LAST().kind == "remove" and LAST().guid == S1 and LAST().extra == "expired" and LAST().state.shield == 0)
        assert(dd.buff.state(dd.actor.hero(12), S1) == nil and dd.actor.hero(12):buff(S1) == nil)
    )lua"));
    // 9. Lua 删除：on_remove(plugin)
    CHECK(Lua(*s, "ev_a", R"lua(
        local ok, n = dd.actor.hero(12):remove_buffs{ guid = S2 }
        assert(ok and n == 1 and LAST().kind == "remove" and LAST().guid == S2 and LAST().extra == "plugin" and LAST().state.shield == 0)
    )lua"));
    // 10. 延迟事件（加载线程上的删除）：actor 按 actor_kind、actor_id 找回句柄
    CHECK(Lua(*s, "ev_b", R"lua(dd.once("buff.remove", function(ev) DEFERRED = { ev.deferred, ev.actor, ev.id } end))lua"));
    FakeGameRemove(0x10100, game, "town_visit", true);
    CHECK(Lua(*s, "ev_a", R"lua(
        assert(LAST().kind == "remove" and LAST().guid == GAME and LAST().extra == "town_visit" and LAST().actor == dd.actor.hero(13))
    )lua"));
    CHECK(Lua(*s, "ev_b", "assert(DEFERRED[1] == true and DEFERRED[2] == dd.actor.hero(13) and DEFERRED[3] == 'lua_shield')"));
    // 11. 怪物身上的载体：战斗结束、怪物不再能解析后，每秒的清理调用 on_remove(gone)
    CHECK(Lua(*s, "ev_a", R"lua(
        local ok, g = dd.actor.monster(4500):add_buff("lua_shield", { state = { shield = 2 } })
        assert(ok) MON = g
    )lua"));
    Tick(*s, 0.5);
    g_world.monsters.clear();
    ++g_world.battle;
    Tick(*s, 1.6);
    CHECK(Lua(*s, "ev_a", R"lua(
        assert(LAST().kind == "remove" and LAST().guid == MON and LAST().extra == "gone" and LAST().state.shield == 2)
        assert(LAST().actor.kind == "monster" and LAST().id == "lua_shield")
    )lua"));
    // 12. 状态必须能保存成 JSON：添加时检查，报错说明位置
    CHECK(Lua(*s, "ev_a", R"lua(
        local h = dd.actor.hero(12)
        for _, bad in ipairs({ { f = print }, { a = { b = { 1, 2, nil, 4 } } }, { [1] = 1, x = 2 }, { [1.5] = 1 }, { x = 0/0 }, { x = 1/0 },
                { s = "\xff" }, { [true] = 1 } }) do
            local ok, err = pcall(h.add_buff, h, "lua_shield", { state = bad })
            assert(not ok and err:find("state"), tostring(err))
        end
        local ok, err = pcall(h.add_buff, h, "lua_shield", { state = { a = { b = print } } })
        assert(err:find("state%.a%.b") and err:find("function"), err)
        local cyc = {} cyc.self = cyc
        ok, err = pcall(h.add_buff, h, "lua_shield", { state = cyc })
        assert(not ok and err:find("嵌套"), err)
    )lua"));
    // 13. 给 P4c 的导出和导入
    std::vector<std::string> errors;
    auto records = s->combat().ExportStates(&errors);
    CHECK(errors.empty() && records.size() == 2);  // S3（hero#13）和 LOADED（hero#12）
    std::string exported;
    for (const auto& r : records) {
        exported += r.mod + "/" + r.buffId + "/" + std::to_string(r.actor.id) + "/" + std::to_string(r.guid) + "=" + r.json + "\n";
    }
    std::printf("  导出的状态：\n%s", exported.c_str());
    CHECK(exported.find("\"shield\":9") != std::string::npos && exported.find("\"applied\":1") != std::string::npos);
    CHECK(Lua(*s, "ev_a", R"lua(dd.buff.state(dd.actor.hero(13), S3).nested = { 1, 2.5, "三", { x = true } })lua"));
    records = s->combat().ExportStates(&errors);
    ddlua::CombatBinding::StateRecord loadedRecord;
    for (auto& r : records) {
        if (r.actor.id == 13) {
            CHECK(r.json.find("\"nested\":[1,2.5,\"三\",{\"x\":true}]") != std::string::npos);
        } else {
            loadedRecord = r;
        }
    }
    loadedRecord.json = R"({"shield":7,"list":[1,2,3],"f":1.0,"i":2})";
    std::string importError;
    CHECK(s->combat().ImportState(loadedRecord, &importError));
    CHECK(Lua(*s, "ev_a", R"lua(
        local st = dd.buff.state(dd.actor.hero(12), LOADED)
        assert(st.shield == 7 and #st.list == 3 and math.type(st.f) == "float" and math.type(st.i) == "integer")
    )lua"));
    auto badRecord = loadedRecord;
    badRecord.guid = 9999;
    CHECK(!s->combat().ImportState(badRecord, &importError) && importError.find("不存在") != std::string::npos);
    badRecord = loadedRecord;
    badRecord.mod = "ev_b";
    CHECK(!s->combat().ImportState(badRecord, &importError));
    badRecord = loadedRecord;
    badRecord.json = "not json";
    CHECK(!s->combat().ImportState(badRecord, &importError) && importError.find("解析") != std::string::npos);
    // 14. 回调出错：连续 3 次停用这个回调
    CHECK(Lua(*s, "ev_b", R"lua(
        assert(dd.buff.define{ id = "lua_bad", stat = 2, sub_stat = "speed_rating", duration = 9, on_apply = function() error("故意") end, on_remove = function() REMOVED = true end })
        local h = dd.actor.hero(13)
        for i = 1, 4 do assert(h:add_buff("lua_bad")) end
        assert(h:remove_buffs{ id = "lua_bad" } and REMOVED)
    )lua"));
    CHECK(CountLog("脚本 Buff lua_bad 的 on_apply 出错") == 3 && LogHas("已停用这个回调"));
    // 15. 会话中解雇英雄：每秒的清理对它身上的载体调用 on_remove(gone)（hero#13 身上 S3 和新加的一条）
    CHECK(Lua(*s, "ev_a", "assert(dd.actor.hero(13):add_buff('lua_shield')) BEFORE_FIRE = #CALLS"));
    g_world.heroes.erase(0x10100);  // 解雇 hero#13：句柄失效
    Tick(*s, 3.0);
    CHECK(Lua(*s, "ev_a", R"lua(
        local n = 0
        for i = BEFORE_FIRE + 1, #CALLS do
            local c = CALLS[i]
            assert(c.kind == "remove" and c.extra == "gone" and c.actor.kind == "hero" and c.actor.id == 13, c.kind)
            n = n + 1
        end
        assert(n == 2 and dd.buff.state(dd.actor.hero(12), LOADED) and dd.stats().states == 1, n)
    )lua"));
    // 16. 重载拆除 Mod：它的定义和状态直接丢弃，不调用 on_remove
    CHECK(Lua(*s, "ev_b", R"lua(
        assert(dd.buff.define{ id = "lua_reload", stat = 2, sub_stat = "speed_rating", duration = 9,
            on_remove = function(b, st, why) dd.log.info("lua_reload on_remove " .. why) end })
        assert(dd.actor.hero(12):add_buff("lua_reload") and dd.stats().states == 1)
    )lua"));
    std::string reloadError;
    CHECK(s->Reload("ev_b", &reloadError));
    CHECK(CountLog("lua_reload on_remove") == 0);
    CHECK(Lua(*s, "ev_b", "local d = dd.buff.def('lua_reload') assert(dd.stats().states == 0 and not (d and d.script))"));
    CHECK(Lua(*s, "ev_a", "assert(dd.stats().states == 1)"));
    // 17. 退出存档：DDCore 先把存档标成不活跃（英雄句柄都解析不了）再派发 CampaignUnload。拆除时不调用 on_remove、不按 gone 清扫，
    // 状态直接丢弃：英雄身上的、随战斗销毁但还没到每秒清理的怪物身上的都一样（F7）
    CHECK(Lua(*s, "ev_a", R"lua(
        assert(dd.buff.define{ id = "lua_exit", stat = 2, sub_stat = "speed_rating", duration = 9,
            on_remove = function(b, st, why) dd.log.info("lua_exit on_remove " .. why) end })
        assert(dd.actor.hero(12):add_buff("lua_exit"))
    )lua"));
    g_world.monsters[0x20000] = { 4500, 7 };
    CHECK(Lua(*s, "ev_a", "assert(dd.actor.monster(4500):add_buff('lua_exit') and dd.stats().states == 3)"));
    g_world.monsters.clear();  // 战斗销毁，还没到每秒的清理
    ++g_world.battle;
    g_world.active = false;    // core.cpp：SetCampaignActive(false) 在派发 CampaignUnload 之前
    const std::size_t goneBefore = CountLog("条载体的 actor 不再能解析");
    s->Teardown();
    CHECK(CountLog("lua_exit on_remove") == 0);
    CHECK(CountLog("条载体的 actor 不再能解析") == goneBefore && LogHas("退出存档，丢弃 3 条载体的状态"));
    CHECK(CoreCount() == 0 && s->memory().residual == 0 && ddlua::EventBinding::GlobalSdkCount() == 0);
}

// ---------------------------------------------------------------- 规则：只读、预算、停用、撤销
void TestRules()
{
    ddlua::Config config = TestConfig();
    config.ruleBudgetMs = 5;
    P4bSession s({ "ev_a", "ev_b" }, config);
    CHECK(Lua(*s, "ev_a", R"lua(
        RULE_ARGS = nil
        R1 = dd.buff.rule("lua_rule", function(self, target, buff)
            RULE_ARGS = { self = self, target = target, buff = buff }
            -- 只读：写操作、注册类函数都报错
            local h = dd.actor.hero(12)
            for name, fn in pairs({ add = function() h:add_buff("SPD5") end, remove = function() h:remove_buffs{ id = "SPD5" } end,
                    on = function() dd.on("app.tick", function() end) end, after = function() dd.after(1, function() end) end,
                    spawn = function() dd.spawn(function() end) end, write = function() dd.fs.write("x", "y") end,
                    define = function() dd.buff.define{ id = "x", stat = 2 } end, rule = function() dd.buff.rule("x", function() end) end,
                    fx = function() dd.effect.get("rakebuff_5"):apply(h) end, efx = function() dd.effect.define('effect: .name "x"\n') end,
                    off = function() R1:off() end }) do
                local ok, err = pcall(fn)
                assert(not ok and err:find("规则回调"), name .. "：" .. tostring(err))
            end
            -- 读照常
            assert(h.hp == 10 and #h:buffs() >= 0 and dd.buff.def("SPD5"))
            return RULE_RESULT
        end)
        assert(R1 and R1.name == "lua_rule" and R1:active() and tostring(R1) == "dd.rule(lua_rule)" and dd.stats().rules == 1)
        RULE_RESULT = true
        local no, why = dd.buff.rule("lua_rule", function() return true end)
        assert(no == nil and why:find("注册失败"), why)
        assert(not pcall(dd.buff.rule, "", function() end) and not pcall(dd.buff.rule, "x", 1) and not pcall(dd.buff.rule, ("x"):rep(64), print))
    )lua"));
    CHECK(g_fc.rules.size() == 1);
    bool result = false;
    CHECK(FakeRuleEval("lua_rule", 0x10000, 0x20000, 42, false, &result) && result);
    CHECK(Lua(*s, "ev_a", R"lua(
        assert(RULE_ARGS.self == dd.actor.hero(12) and RULE_ARGS.target == dd.actor.monster(4500))
        local b = RULE_ARGS.buff
        assert(b.id == "lua_rule_buff" and b.rule == "is_actor_status" and b.rule_string == "lua_rule" and b.is_false_rule == false)
        assert(b.subject == dd.actor.hero(12) and math.abs(b.rule_float - 0.5) < 1e-6)
    )lua"));
    // is_false_rule：DDCore 取反，回调不用管
    CHECK(FakeRuleEval("lua_rule", 0x10000, 0, 13, true, &result) && !result);
    CHECK(Lua(*s, "ev_a", "assert(RULE_ARGS.target == nil and RULE_ARGS.buff.rule == 'actorStatus' and RULE_ARGS.buff.subject == nil)"));
    // 返回值不是布尔、出错：交还原函数（P4c1 起，原来按假处理），连续 3 次停用，下一帧向 DDCore 撤销
    CHECK(Lua(*s, "ev_a", "RULE_RESULT = 1"));
    for (int i = 0; i < 3; ++i) {
        result = true;
        CHECK(!FakeRuleEval("lua_rule", 0x10000, 0, 42, false, &result) && result);
    }
    CHECK(LogHas("规则回调必须返回布尔") && LogHas("已停用：之后交还游戏原来的判定"));
    CHECK(Lua(*s, "ev_a", "assert(not R1:active())"));
    result = true;
    CHECK(!FakeRuleEval("lua_rule", 0x10000, 0, 42, false, &result) && result);  // 停用后、撤销前：交还原函数，不进 Lua
    CHECK(g_fc.rules.size() == 1);
    Tick(*s, 0.1);
    CHECK(g_fc.rules.empty() && LogHas("规则 lua_rule 已向 DDCore 撤销"));
    CHECK(!FakeRuleEval("lua_rule", 0x10000, 0, 42, false, &result));  // 交给游戏
    // 超预算：一次就停用；死循环最多卡 rule_budget_ms 左右
    CHECK(Lua(*s, "ev_b", R"lua(R2 = dd.buff.rule("lua_loop", function() while true do end end))lua"));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(!FakeRuleEval("lua_loop", 0x10100, 0, 42, false, &result));
    const double loopMs = Ms(t0);
    std::printf("  规则回调死循环 %.1f ms 后被打断（rule_budget_ms = 5）\n", loopMs);
    CHECK(loopMs < 100 && LogHas("超出 rule_budget_ms"));
    Tick(*s, 0.2);
    CHECK(g_fc.rules.empty());
    // 规则回调里的原生异常：DDCore 的隔离停用规则，VM 标记损坏
    CHECK(Lua(*s, "ev_b", R"lua(R3 = dd.buff.rule("lua_crash", function() dd.actor.hero(666) return true end))lua"));
    // rule:off() 由所属 Mod 撤销
    CHECK(Lua(*s, "ev_a", R"lua(
        R4 = dd.buff.rule("lua_off", function() return true end)
        assert(R4:off() == true and R4:off() == false and not R4:active())
    )lua"));
    CHECK(g_fc.rules.size() == 1);
    g_crashFind = true;
    CHECK(!FakeRuleEval("lua_crash", 0x10100, 0, 42, false, &result));
    g_crashFind = false;
    CHECK(s->Broken() && LogHas("combat 回调里执行 Lua 代码时发生了原生异常") == false);
    s->Teardown();
    CHECK(LogHas("Lua VM 已损坏") && g_fc.rules.empty() && CoreCount() == 0);
}

// ---------------------------------------------------------------- dd.fs 的写入（D10）
void TestDataFs()
{
    ddlua::Config config = TestConfig();
    config.dataMaxMb = 1;
    config.dataMaxFiles = 6;  // 条目数：文件和目录都算（C-Y1）
    P4bSession s({ "ev_a", "ev_b" }, config);
    CHECK(Lua(*s, "ev_a", R"lua(
        -- 还没有写过：读不到，列表为空
        assert(dd.fs.read("a.txt", { root = "data" }) == nil and not dd.fs.exists("a.txt", { root = "data" }))
        assert(#dd.fs.list{ root = "data" } == 0 and dd.fs.list("sub", { root = "data" }) == nil)
        local n, why = dd.fs.remove("a.txt")
        assert(n == nil and why, why)
        -- 写、读、覆盖、子目录
        assert(dd.fs.write("a.txt", "one") == true)
        assert(dd.fs.read("a.txt", { root = "data" }) == "one" and dd.fs.exists("a.txt", { root = "data" }))
        assert(dd.fs.read("a.txt") == nil)  -- 默认还是 Mod 目录
        assert(dd.fs.write("a.txt", "two\0bin") and dd.fs.read("a.txt", { root = "data" }) == "two\0bin")
        assert(dd.fs.write("sub/deep/b.json", "{}") and dd.fs.read("sub\\deep\\b.json", { root = "data" }) == "{}")
        local list = dd.fs.list{ root = "data" }
        assert(#list == 2 and list[1] == "a.txt" and list[2] == "sub/", table.concat(list, ","))
        assert(dd.fs.list("sub", { root = "data" })[1] == "deep/")
        -- 路径规则（含保留的设备名、超过 8 层）
        for _, bad in ipairs({ "../x", "..\\x", "/x", "C:/x", "C:x", "a/../b", "", "sub", "a:b", "x*", "con.", "nul.txt", "a/CON/b",
                "COM1", "lpt9.log", ("d/"):rep(8) .. "x" }) do
            local ok, err = dd.fs.write(bad, "x")
            assert(ok == nil and err, bad)
        end
        local ok, err = dd.fs.write(("d/"):rep(8) .. "x", "x")
        assert(err:find("8 层"), err)
        assert(dd.fs.write("com10.txt", "x") and dd.fs.remove("com10.txt"))  -- COM10 不是设备名
        assert(not pcall(dd.fs.write, "x") and not pcall(dd.fs.write, {}, "x") and not pcall(dd.fs.read, "x", { root = "nope" }))
        assert(not pcall(dd.fs.read, "x", { nope = 1 }))
        -- 上限：条目 6 个（a.txt、sub、deep、b.json 已经 4 个）、总大小 1 MB
        assert(dd.fs.write("c.txt", "c") and dd.fs.write("d.txt", "d"))
        ok, err = dd.fs.write("e.txt", "e")
        assert(ok == nil and err:find("data_max_files"), err)
        assert(dd.fs.write("c.txt", "cc"))  -- 覆盖已有的不加条目数
        ok, err = dd.fs.write("a.txt", ("x"):rep(1024 * 1024 + 1))
        assert(ok == nil and err:find("data_max_mb"), err)
        assert(dd.fs.write("a.txt", ("x"):rep(1024 * 1024 - 16)))
        ok, err = dd.fs.write("c.txt", ("y"):rep(100))
        assert(ok == nil and err:find("data_max_mb"), err)
        -- 删除后有空间
        assert(dd.fs.remove("a.txt") and not dd.fs.exists("a.txt", { root = "data" }))
        assert(dd.fs.write("e.txt", "e") and dd.fs.write("c.txt", ("y"):rep(1000)))
        -- 新建目录也算条目：差 2 个，什么都不建
        assert(dd.fs.remove("e.txt"))
        ok, err = dd.fs.write("n1/n2/x.txt", "x")
        assert(ok == nil and err:find("data_max_files"), err)
        assert(not dd.fs.exists("n1", { root = "data" }))
        -- 目录：不空时拒绝，空的可以删，数据目录本身不能删
        ok, err = dd.fs.remove("sub")
        assert(ok == nil and err:find("不是空的"), err)
        assert(dd.fs.remove("sub/deep/b.json") and dd.fs.remove("sub/deep") and dd.fs.remove("sub"))
        ok, err = dd.fs.remove("")
        assert(ok == nil and err:find("数据目录本身"), err)
        list = dd.fs.list{ root = "data" }
        assert(#list == 2 and list[1] == "c.txt" and list[2] == "d.txt", table.concat(list, ","))
        assert(dd.fs.write("n1/x.txt", "x") and dd.fs.write("e.txt", "e"))  -- 删掉的 3 个条目腾出来了
    )lua"));
    // 两个 Mod 的数据目录分开
    CHECK(Lua(*s, "ev_b", R"lua(
        assert(dd.fs.read("e.txt", { root = "data" }) == nil and dd.fs.write("e.txt", "b") and dd.fs.read("e.txt", { root = "data" }) == "b")
    )lua"));
    const std::wstring dataA = ddlua::Utf8ToWide(P4bRoot() + "/DDModLoader/data/ev_a");
    CHECK(GetFileAttributesW((dataA + L"\\e.txt").c_str()) != INVALID_FILE_ATTRIBUTES);
    // 联接逃逸：数据目录里的联接指向外面，写入被拒绝
    const std::wstring outside = ddlua::Utf8ToWide(P4bRoot() + "/outside");
    CreateDirectoryW(outside.c_str(), nullptr);
    std::string rootBack = P4bRoot();
    std::replace(rootBack.begin(), rootBack.end(), '/', '\\');
    const std::string mkFixed = "cmd /c mklink /J \"" + rootBack + "\\DDModLoader\\data\\ev_a\\jump\" \"" + rootBack + "\\outside\" >nul 2>nul";
    const auto linked = [&] {
        const DWORD attr = GetFileAttributesW((dataA + L"\\jump").c_str());
        return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT);
    };
    for (int attempt = 0; attempt < 5 && !linked(); ++attempt) {
        std::system(mkFixed.c_str());  // guest 上偶尔启动 cmd 失败，重试
        if (!linked()) {
            Sleep(300);
        }
    }
    if (linked()) {
        CHECK(Lua(*s, "ev_a", R"lua(
            assert(dd.fs.remove("e.txt"))
            local ok, err = dd.fs.write("jump/x.txt", "escape")
            assert(ok == nil and err:find("之外"), err)
            assert(dd.fs.read("jump/x.txt", { root = "data" }) == nil)
        )lua"));
        CHECK(GetFileAttributesW((outside + L"\\x.txt").c_str()) == INVALID_FILE_ATTRIBUTES);
    } else {
        std::printf("  （mklink /J 失败，跳过联接逃逸）\n");
    }
    // 没有临时文件留下
    CHECK(Lua(*s, "ev_a", R"lua(
        for _, name in ipairs(dd.fs.list{ root = "data" }) do assert(not name:find("ddtmp"), name) end
    )lua"));
    // 用量按已有文件扫描：新会话里接着算
    s->Teardown();
    {
        ddlua::Session again(s.host, config, "存档#P4b-2");
        again.Load(Sources({ "ev_a" }));
        CHECK(Lua(again, "ev_a", R"lua(
            -- 现有 c.txt、d.txt、n1/、n1/x.txt（e.txt 在联接测试时删了，没删就是 5 个条目；联接本身不算）
            assert(dd.fs.write("f.txt", "f"))
            local ok, err = dd.fs.write("g.txt", "g")
            assert(ok or err:find("data_max_files"), tostring(err))
        )lua"));
        again.Teardown();
    }
    // C-B2：循环里反复写入在预算到了之后很快被打断（每次调用返回前检查截止时间）
    {
        ddlua::Config tight = TestConfig();
        tight.budgetMs = 50;
        tight.dataMaxFiles = 1000;
        ddlua::Session loop(s.host, tight, "存档#P4b-3");
        loop.Load(Sources({ "ev_a" }));
        for (const char* chunk : { "while true do dd.fs.write('loop.txt', '') end",
                 "local p = ('l/'):rep(7) .. 'x.txt' while true do dd.fs.write(p, 'x') end",
                 "while true do dd.fs.exists('c.txt', { root = 'data' }) end" }) {
            std::string error;
            bool budget = false;
            const auto t0 = std::chrono::steady_clock::now();
            CHECK(!loop.RunChunk(*loop.FindMod("ev_a"), chunk, "=fsloop", &error, &budget) && budget);
            const double ms = Ms(t0);
            std::printf("  dd.fs 循环（%s）：%.0f ms 后被打断（预算 50 ms）\n", chunk, ms);
            CHECK(ms < 150);
        }
        loop.Teardown();
    }
    // R2A-N3：数据目录建不出来（DDModLoader\data\ev_b 是个文件）时 dd.fs.write 每次都失败，同样计入预算、返回前检查截止时间
    {
        const std::wstring dataB = ddlua::Utf8ToWide(P4bRoot() + "/DDModLoader/data/ev_b");
        DeleteFileW((dataB + L"\\e.txt").c_str());
        CHECK(RemoveDirectoryW(dataB.c_str()));
        HANDLE blocker = CreateFileW(dataB.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(blocker != INVALID_HANDLE_VALUE);
        if (blocker != INVALID_HANDLE_VALUE) {
            CloseHandle(blocker);
        }
        ddlua::Config tight = TestConfig();
        tight.budgetMs = 50;
        ddlua::Session loop(s.host, tight, "存档#P4b-4");
        loop.Load(Sources({ "ev_b" }));
        CHECK(Lua(loop, "ev_b", R"lua(local ok, why = dd.fs.write("x.txt", "x") assert(ok == nil and why, tostring(why)))lua"));
        std::string error;
        bool budget = false;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!loop.RunChunk(*loop.FindMod("ev_b"), "while true do dd.fs.write('x.txt', '') end", "=fsloop", &error, &budget) && budget);
        const double ms = Ms(t0);
        std::printf("  dd.fs 循环（数据目录建不出来）：%.0f ms 后被打断（预算 50 ms）\n", ms);
        CHECK(ms < 150);
        loop.Teardown();
        DeleteFileW(dataB.c_str());
    }
    CHECK(CoreCount() == 0);
}

// ---------------------------------------------------------------- 嵌套入口：预算、补时间、粘滞超时、崩溃隔离、跨 VM
void TestNestedEntries()
{
    ddlua::Config config = TestConfig();
    config.budgetMs = 50;
    {
        P4bSession s({ "ev_a", "ev_b" }, config);
        // B 的 buff.add 回调：按 SLOW 毫秒忙等
        CHECK(Lua(*s, "ev_b", R"lua(
            SLOW, CALLS = 40, 0
            SUB = dd.on("buff.add", function(ev)
                CALLS = CALLS + 1
                local t = os.clock() + SLOW / 1000
                while os.clock() < t do end
                if LOOP then while true do end end
                DEPTH = true
            end)
        )lua"));
        // 1. A 自己用 35 ms，立即执行的 add_buff 里 B 的回调用 40 ms：合计 75 ms 超过一份预算，但两个都不超预算（嵌套的时间补给 A）
        const std::uint64_t credits0 = s->vm()->NestCredits();
        std::string error;
        bool budget = false;
        const char* outer = R"lua(
            local t = os.clock() + 0.025
            while os.clock() < t do end
            assert(dd.actor.hero(12):add_buff("SPD5"))
            t = os.clock() + 0.01
            while os.clock() < t do end
            OUTER_DONE = true
        )lua";
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(s->RunChunk(*s->FindMod("ev_a"), outer, "=nested1", &error, &budget));
        std::printf("  同一个 VM 的嵌套：外层 %.0f ms（外层自己约 35 ms，嵌套 40 ms），嵌套入口补时间 %llu 次共 %.1f ms\n", Ms(t0),
            static_cast<unsigned long long>(s->vm()->NestCredits() - credits0), s->vm()->NestCreditedMs());
        CHECK(!budget && s->vm()->NestCredits() > credits0);
        CHECK(Lua(*s, "ev_b", "assert(CALLS == 1 and SUB:active())"));
        // 2. 嵌套的回调死循环：B 被停用（超预算），A 照常做完
        CHECK(Lua(*s, "ev_b", "LOOP = true SLOW = 0"));
        CHECK(s->RunChunk(*s->FindMod("ev_a"), "assert(dd.actor.hero(12):add_buff('SPD5')) local x = 0 for i = 1, 1000 do x = x + i end OK2 = true",
            "=nested2", &error, &budget));
        CHECK(!budget && LogHas("buff.add 的回调出错（超出执行预算），已停用该订阅"));
        CHECK(Lua(*s, "ev_b", "assert(not SUB:active())"));
        // 3. 外层的截止时间在游戏函数里（没有检查点）过去了：嵌套的回调按自己的预算照常运行，回到外层后外层超预算
        CHECK(Lua(*s, "ev_b", R"lua(
            FAST = 0
            dd.on("buff.add", function() FAST = FAST + 1 local t = os.clock() + 0.005 while os.clock() < t do end end)
        )lua"));
        g_fc.sleepMs = 30;
        const char* late = R"lua(
            local t = os.clock() + 0.04
            while os.clock() < t do end
            assert(dd.actor.hero(12):add_buff("SPD5"))  -- 游戏函数里睡 30 ms（不补时间），嵌套 5 ms（补）
            local x = 0 for i = 1, 100000 do x = x + i end
            NOT_REACHED = true
        )lua";
        CHECK(!s->RunChunk(*s->FindMod("ev_a"), late, "=nested3", &error, &budget) && budget);
        g_fc.sleepMs = 0;
        CHECK(Lua(*s, "ev_b", "assert(FAST == 1)"));
        CHECK(Lua(*s, "ev_a", "assert(NOT_REACHED == nil)"));
        CHECK(CountLog("已停用该订阅") == 1);  // 只有第 2 步的死循环；B 没有因为 A 过了截止时间被连累
        // 4. 外层反复触发嵌套回调：补的时间有上限（max(预算, 最长的一次)），外层照样超时，总时间有界
        CHECK(Lua(*s, "ev_b", R"lua(dd.on("buff.add", function() local t = os.clock() + 0.02 while os.clock() < t do end end))lua"));
        const auto t1 = std::chrono::steady_clock::now();
        CHECK(!s->RunChunk(*s->FindMod("ev_a"), "for i = 1, 100 do dd.actor.hero(12):add_buff('SPD5') end", "=nested4", &error, &budget) && budget);
        const double total = Ms(t1);
        std::printf("  外层循环 add_buff、每次嵌套 25 ms：%.0f ms 后外层超预算，嵌套补时间被截断 %llu 次\n", total,
            static_cast<unsigned long long>(s->vm()->NestCapped()));
        CHECK(total < 250 && s->vm()->NestCapped() > 0);
        s->Teardown();
        CHECK(CoreCount() == 0 && s->memory().residual == 0);
    }
    {
        // 5. 嵌套回调里的原生异常（同一个 VM）：假中枢接住异常、撤销订阅，回到外层的 add_buff 时 VM 已损坏，
        //    AfterReentry 抛 SEH 异常把外层也交给它自己的隔离（这里是测试的 SehCall，游戏里是 RunMarked / DDCore 的事件隔离）
        P4bSession s({ "ev_a", "ev_b" }, config);
        CHECK(Lua(*s, "ev_b", R"lua(dd.on("buff.add", function() dd.actor.hero(666) end))lua"));
        struct Run {
            ddlua::Session* session;
            bool ok;
        } run{ &*s, false };
        g_crashFind = true;
        const bool returned = SehCall(
            [](void* raw) {
                auto* r = static_cast<Run*>(raw);
                std::string error;
                r->ok = r->session->RunChunk(*r->session->FindMod("ev_a"), "dd.actor.hero(12):add_buff('SPD5') AFTER = true", "=crash", &error);
            },
            &run);
        g_crashFind = false;
        std::printf("  嵌套回调里的原生异常：外层%s\n", returned ? "正常返回（不应该）" : "被 AfterReentry 展开");
        CHECK(!returned && s->Broken());
        CHECK(LogHas("Lua VM 已损坏"));
        s->Teardown();
        CHECK(CoreCount() == 0 && ddlua::EventBinding::GlobalSdkCount() == 0);
    }
    {
        // 6. 跨 VM：开发会话的 Mod 立即执行 add_buff，存档会话的 Mod 在 buff.add 里用 40 ms，补到开发会话的入口上；
        //    存档会话的回调崩溃时只有它的 VM 损坏，开发会话照常，之后的预算检查也照常
        g_log.clear();
        CHECK(ResetHubFromDump());
        ResetWorld();
        ResetCombat();
        const auto host = MakeCombatHost();
        ddlua::Config big = config;
        big.memoryLimitMb = 512;
        ddlua::Session dev(host, big, "开发");
        ddlua::Session campaign(host, config, "存档#1");
        dev.Load(Sources({ "ev_a" }));
        campaign.Load(Sources({ "ev_b" }));
        CHECK(Lua(campaign, "ev_b", R"lua(
            CALLS = 0
            dd.on("buff.add", function() CALLS = CALLS + 1 local t = os.clock() + 0.04 while os.clock() < t do end if CRASH then dd.actor.hero(666) end end)
        )lua"));
        std::string error;
        bool budget = false;
        const std::uint64_t credits0 = dev.vm()->NestCredits();
        CHECK(dev.RunChunk(*dev.FindMod("ev_a"), "local t = os.clock() + 0.025 while os.clock() < t do end assert(dd.actor.hero(12):add_buff('SPD5')) "
                                                 "t = os.clock() + 0.01 while os.clock() < t do end",
            "=cross", &error, &budget));
        CHECK(!budget && dev.vm()->NestCredits() > credits0);
        CHECK(Lua(campaign, "ev_b", "assert(CALLS == 1) CRASH = true"));
        // 存档会话的回调崩溃之后，开发会话的同一个入口接着做长串比较：长串比较没有 lua_State，靠入口栈的栈顶找 VM，
        // 崩溃的 VM 没出栈的入口不能还占着栈顶（占着的话要等 1000 条指令的计数 hook，这里一次比较 32 MB，约 250 次要 1 秒以上）
        CHECK(dev.RunChunk(*dev.FindMod("ev_a"), "BIG_T = ('x'):rep(32 * 1024 * 1024) .. 'a' BIG_U = ('x'):rep(32 * 1024 * 1024) .. 'b'", "=big",
            &error, &budget, 5000));
        g_crashFind = true;
        const auto t2 = std::chrono::steady_clock::now();
        CHECK(!dev.RunChunk(*dev.FindMod("ev_a"), "assert(dd.actor.hero(12):add_buff('SPD5')) CROSS_OK = true "
                                                  "local t, u = BIG_T, BIG_U while true do local _ = (t == u) end",
                  "=cross2", &error, &budget) && budget);
        const double cmpMs = Ms(t2);
        g_crashFind = false;
        CHECK(campaign.Broken() && !dev.Broken());
        CHECK(Lua(dev, "ev_a", "assert(CROSS_OK) BIG_T, BIG_U = nil, nil"));
        std::printf("  跨 VM 嵌套里存档会话崩溃之后，开发会话的同一个入口 %.0f ms 被打断（预算 50 ms；崩溃的嵌套入口没有返回，不补时间）\n", cmpMs);
        CHECK(cmpMs < 400);
        campaign.Teardown();
        dev.Teardown();
        CHECK(CoreCount() == 0 && ddlua::EventBinding::GlobalSdkCount() == 0);
    }
}

// ---------------------------------------------------------------- 状态的 JSON 编解码
void TestStateJson()
{
    g_log.clear();
    ddlua::Vm vm(16 * 1024 * 1024, nullptr);
    std::string out, error;
    CHECK(vm.Protected([&](lua_State* L) {
        luaL_dostring(L, R"(return { a = 1, b = 2.5, c = "文字\n\"q\"", d = { 1, 2, { x = false } }, e = {}, big = 9007199254740993, neg = -0.0 })");
        CHECK(ddlua::state_json::Encode(L, -1, "state", out, &error));
        lua_pop(L, 1);
        CHECK(ddlua::state_json::Decode(L, out, &error));
        lua_getfield(L, -1, "big");
        CHECK(lua_isinteger(L, -1) && lua_tointeger(L, -1) == 9007199254740993ll);
        lua_pop(L, 1);
        lua_getfield(L, -1, "b");
        CHECK(!lua_isinteger(L, -1) && lua_tonumber(L, -1) == 2.5);
        lua_pop(L, 1);
        lua_getfield(L, -1, "c");
        CHECK(std::string(lua_tostring(L, -1)) == "文字\n\"q\"");
        lua_pop(L, 2);
    }, &error));
    std::printf("  编码：%s\n", out.c_str());
    CHECK(out.find("\"d\":[1,2,{\"x\":false}]") != std::string::npos && out.find("\"e\":{}") != std::string::npos);
    CHECK(vm.Protected([&](lua_State* L) {
        luaL_dostring(L, "return { list = { 1, 2, n = 3 } }");
        CHECK(!ddlua::state_json::Encode(L, -1, "state", out, &error) && error.find("state.list") != std::string::npos && error.find("混用") != std::string::npos);
        lua_pop(L, 1);
        luaL_dostring(L, "return { ['a b'] = { [3] = 1 } }");
        CHECK(!ddlua::state_json::Encode(L, -1, "state", out, &error) && error.find("state[\"a b\"]") != std::string::npos && error.find("空洞") != std::string::npos);
        lua_pop(L, 1);
        CHECK(!ddlua::state_json::Decode(L, "[1,", &error) && !ddlua::state_json::Decode(L, "3", &error));
    }, &error));
    vm.Close();
}

// ---------------------------------------------------------------- 场景 Mod p4b_combat 的离线预演
// 假世界里走一遍城镇、走廊、战斗（帧间执行排队的操作），场景 Mod 的日志里不能有「失败」，三轮都要做完
void P4bFrames(ddlua::Session& s, double& t, double seconds)
{
    for (const double end = t + seconds; t < end; t += 0.05) {
        FakeDrain();
        Tick(s, t);
    }
}

void P4bState(const char* to)
{
    Fire f("app.state");
    f.I("from", 1).I("to", 2).S("from_name", "x").S("to_name", to);
    f.Go();
}

void TestP4bScenarioMod()
{
    g_log.clear();
    CHECK(ResetHubFromDump());
    ResetWorld();
    ResetCombat();
    ResetP4bRoot();
    g_fc.raid = false;  // 城镇
    const auto host = MakeCombatHost();
    ddlua::Session s(host, TestConfig(), "存档#P4b 场景");
    g_now = 0;
    s.SetClock([] { return g_now; });
    s.Load({ { "场景 p4b_combat", std::string(DD_PROJECT_ROOT) + "/tests/lua/scenario/p4b_combat" } });
    CHECK(s.LoadedCount() == 1);
    double t = 0;
    P4bFrames(s, t, 3.5);
    // 城镇这一轮之后进地牢：场景 Mod 靠 dd.actor.party 认识队伍（F5，走廊里不用先有事件）
    P4bState("as_raid_update");
    g_fc.raid = true;
    g_world.party = { 0x10000 };
    bool ruled = false;
    CHECK(FakeRuleEval("p4b_rule", 0x10000, 0, 42, false, &ruled) && ruled);
    P4bFrames(s, t, 4.5);
    // 战斗：靠 dd.actor.in_battle（游戏的战斗状态）认出战斗，怪物靠 dd.actor.monsters
    g_world.battleState = 1;
    Fire start("battle.start");
    start.Go();
    Fire dmg("damage.pre");
    dmg.P("target", P(0x10000)).P("attacker", P(0x20000)).Fl("damage", 3).B("is_hero", true);
    dmg.Go();
    Fire skill("skill.pre");
    skill.P("performer", P(0x10000)).P("skill", P(0x80000)).P("target", P(0x20000));
    skill.Go();
    P4bFrames(s, t, 4.5);
    // 战斗结束：怪物随战斗销毁，怪物身上的脚本 Buff 状态按 gone 丢弃
    g_world.battleState = 0;
    g_world.monsters.clear();
    ++g_world.battle;
    Fire end("battle.end");
    end.Go();
    P4bFrames(s, t, 31);
    std::size_t passed = 0;
    std::size_t failed = 0;
    for (const auto& line : g_log) {
        if (line.rfind("[p4b_combat] 通过", 0) == 0) {
            ++passed;
        } else if (line.rfind("[p4b_combat] 失败", 0) == 0 || (line.rfind("[p4b_combat]", 0) == 0 && line.find("出错") != std::string::npos)) {
            ++failed;
            std::printf("    %s\n", line.c_str());
        }
    }
    std::printf("  p4b_combat 预演：通过 %zu 行，失败 %zu 行\n", passed, failed);
    CHECK(failed == 0 && passed >= 40);
    for (const char* tag : { "rounddone.town", "rounddone.corridor", "rounddone.battle", "effect.town", "effect.corridor", "effect.battle",
             "queue.done.town", "queue.done.battle", "script.remove.corridor", "monster.battle", "rule.readonly", "fs", "define.game" }) {
        if (CountLog(std::string("[p4b_combat] 通过 [") + tag + "]") != 1) {
            std::printf("    没有「通过 [%s]」\n", tag);
            CHECK(false);
        }
    }
    CHECK(LogHas("reason=gone") && LogHas("[p4b_combat] 信息 [summary]"));
    s.Teardown();
    CHECK(CoreCount() == 0 && s.memory().residual == 0);
}

// ================================================================ P4c1：dd.store 和脚本 Buff 状态随存档保存

// campaign.save 主题（DDCore 有插件声明持久化时可用；DDLua 总是声明）
void EnsureSaveTopic()
{
    Topic* topic = g_hub->Find("campaign.save");
    if (!topic) {
        using F = FieldDesc;
        constexpr auto R = ddc::events::kAccessR;
        topic = g_hub->AddTopic("campaign.save", TopicKind::App,
            { F{ "files", DD_FIELD_STRING, R }, F{ "in_raid", DD_FIELD_BOOL, R }, F{ "in_battle", DD_FIELD_BOOL, R }, F{ "main_thread", DD_FIELD_BOOL, R },
                F{ "serial", DD_FIELD_INT, R }, F{ "index", DD_FIELD_INT, R } },
            false, false);
    }
    g_hub->SetAvailability(topic, true, "");
}

// 像 DDCore 的 campaign.save：先派发主题（Lua 的 dd.on 回调），再调用保存回调（等同 plugin.cpp 的 OnPersistSave 调用会话的 PersistSave）
void FakeCampaignSave(ddlua::Session& session, const char* files, bool inRaid, bool inBattle, std::uint32_t index)
{
    EnsureSaveTopic();
    Fire f("campaign.save");
    f.S("files", files).B("in_raid", inRaid).B("in_battle", inBattle).B("main_thread", true).I("serial", 1).I("index", index);
    f.Go();
    const DDPersistSaveInfo info{ sizeof(DDPersistSaveInfo), files, inRaid, inBattle, true, 1, index };
    g_fp.saving = true;
    session.PersistSave(info);
    g_fp.saving = false;
}

void Verify(ddlua::Session& session, const char* key, std::uint32_t status, const char* reason)
{
    const DDPersistVerify v{ sizeof(DDPersistVerify), key, status, reason };
    session.PersistVerify(v);
}

// 一个 P4c1 的会话：fresh 为真时是新的世界（第一次进存档），为假时沿用上一个会话的世界（读档：buff 表还在）
struct P4cSession {
    ddlua::Host host;
    std::unique_ptr<ddlua::Session> session;
    P4cSession(std::initializer_list<const char*> mods, bool fresh, ddlua::Config config = TestConfig(), bool persistent = true, bool service = true)
    {
        if (fresh) {
            g_log.clear();
            CHECK(ResetHubFromDump());
            ResetWorld();
            ResetCombat();
            ResetP4bRoot();
        }
        EnsureSaveTopic();
        host = MakeCombatHost();
        host.persist = service ? &kFakePersist : nullptr;
        session = std::make_unique<ddlua::Session>(host, config, persistent ? "存档#P4c" : "开发");
        session->SetPersistent(persistent);
        g_now = 0;
        session->SetClock([] { return g_now; });
        session->Load(Sources(mods));
    }
    ddlua::Session& operator*() { return *session; }
    ddlua::Session* operator->() { return session.get(); }
};

const ddlua::store::BuffState* FindState(const ddlua::store::Payload& p, std::uint32_t kind, std::uint32_t id)
{
    for (const auto& b : p.buffs) {
        if (b.kind == kind && b.id == id) {
            return &b;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------- 旁路数据的格式
void TestStoreFormat()
{
    using ddlua::store::BuffState;
    ddlua::store::Payload p;
    p.store = "{\"a\":1,\"s\":\"line\\nbreak\"}";
    p.buffs.push_back(BuffState{ "id with space", DD_ACTOR_HERO, 12, 0, 5, "{\"x\":[1,2]}" });
    p.buffs.push_back(BuffState{ "m", DD_ACTOR_MONSTER, 4500, 0, 1, "{}" });
    const std::string bytes = ddlua::store::Encode(p);
    CHECK(bytes.rfind("DDLUA-STORE 1\nstore ", 0) == 0 && bytes.find("buff hero 12 0 5 11 id with space\n") != std::string::npos);
    ddlua::store::Payload back;
    std::string why;
    CHECK(ddlua::store::Decode(bytes, &back, &why) && back.store == p.store && back.buffs.size() == 2);
    CHECK(back.buffs[0].buffId == "id with space" && back.buffs[0].kind == DD_ACTOR_HERO && back.buffs[0].id == 12 && back.buffs[0].guid == 5 &&
        back.buffs[0].json == "{\"x\":[1,2]}");
    CHECK(back.buffs[1].kind == DD_ACTOR_MONSTER && back.buffs[1].id == 4500 && back.buffs[1].json == "{}");
    // 只有 dd.store
    CHECK(ddlua::store::Decode(ddlua::store::Encode({ "{}", {} }), &back, &why) && back.store == "{}" && back.buffs.empty());
    // 截断、头不对、段不对
    CHECK(!ddlua::store::Decode(bytes.substr(0, bytes.size() - 2), &back, &why));
    CHECK(!ddlua::store::Decode("DDLUA-STORE 2\nstore 2\n{}\n", &back, &why) && why.find("第一行") != std::string::npos);
    CHECK(!ddlua::store::Decode("DDLUA-STORE 1\nstore 5\n{}\n", &back, &why));
    CHECK(!ddlua::store::Decode("DDLUA-STORE 1\nother 1\n", &back, &why) && why.find("不认识") != std::string::npos);
    CHECK(!ddlua::store::Decode("DDLUA-STORE 1\n", &back, &why) && why.find("store") != std::string::npos);
    CHECK(!ddlua::store::Decode("DDLUA-STORE 1\nstore 2\n{}\nbuff ghost 1 0 1 2 x\n{}\n", &back, &why) && why.find("种类") != std::string::npos);
    CHECK(!ddlua::store::Decode("DDLUA-STORE 1\nstore 2\n{}\nbuff hero 1 0 1 2\n{}\n", &back, &why));
}

// ---------------------------------------------------------------- 存档、读档：dd.store、英雄和怪物的状态、核对
void TestStoreRoundTrip()
{
    ResetPersistFakes();
    std::string savedA, savedB;
    std::uint32_t heroGuid = 0, monsterGuid = 0, spdGuid = 0;
    {
        P4cSession s({ "store_a", "store_b" }, true);
        CHECK(s->LoadedCount() == 2 && g_fp.loadCalls == 2);
        CHECK(Lua(*s, "store_a", R"lua(
            assert(MAIN.status == "empty" and MAIN.reason == nil and MAIN.counter == nil and dd.store == MAIN.store)
            assert(dd.store_status() == "empty")
            dd.store.counter = 1
            dd.store.list = { 1, 2.5, "三", { deep = true } }
            local ok, g = dd.actor.hero(12):add_buff("lua_store_buff", { state = { shield = 4 } })
            assert(ok)
            local ok2 = dd.actor.monster(4500):add_buff("lua_store_buff", { state = { shield = 2, hits = { 1, 2 } } })
            assert(ok2 and APPLIED == 2)
        )lua"));
        CHECK(Lua(*s, "store_b", R"lua(assert(MAIN.status == "empty") dd.store.counter = 7)lua"));
        spdGuid = FakeGameAdd(0x10100, "SPD5", false);  // hero#13 身上一条别的 buff（读档时拿来构造「id 对不上」）
        // 地牢、战斗中存了一次档
        FakeCampaignSave(*s, "persist.map.json,persist.roster.json,persist.game.json,persist.raid.json", true, true, 1);
        CHECK(Lua(*s, "store_b", R"lua(
            assert(#SAVES == 1 and SAVES[1].files:find("persist.raid.json") and SAVES[1].in_raid and SAVES[1].in_battle and SAVES[1].index == 1)
        )lua"));
        CHECK(g_fp.saved.count("store_a") == 1 && g_fp.saved.count("store_b") == 1);
        CHECK(g_fp.savedVersion["store_a"] == "1.4.0" && g_fp.savedVersion["store_b"] == "2.0.1");
        ddlua::store::Payload a, b;
        std::string why;
        CHECK(ddlua::store::Decode(g_fp.saved["store_a"], &a, &why));
        CHECK(a.store.find("\"counter\":1") != std::string::npos && a.store.find("[1,2.5,\"三\",{\"deep\":true}]") != std::string::npos);
        const auto* hero = FindState(a, DD_ACTOR_HERO, 12);
        const auto* monster = FindState(a, DD_ACTOR_MONSTER, 4500);
        CHECK(a.buffs.size() == 2 && hero && monster);
        if (hero && monster) {
            heroGuid = hero->guid;
            monsterGuid = monster->guid;
            CHECK(hero->buffId == "lua_store_buff" && hero->json.find("\"shield\":4") != std::string::npos);
            CHECK(monster->json.find("\"shield\":2") != std::string::npos && monster->json.find("\"hits\":[1,2]") != std::string::npos);
        }
        // 快照在所有 campaign.save 回调之后取：回调里改的 dd.store 写进去了
        CHECK(ddlua::store::Decode(g_fp.saved["store_b"], &b, &why) && b.store.find("\"saved_in_callback\":1") != std::string::npos &&
            b.store.find("\"counter\":7") != std::string::npos && b.buffs.empty());
        CHECK(LogHas("campaign.save #1（persist.map.json,persist.roster.json,persist.game.json,persist.raid.json，地牢，战斗中）：写出 2 个 Mod"));
        savedA = g_fp.saved["store_a"];
        savedB = g_fp.saved["store_b"];
        // 战斗结束、怪物销毁之后再存：怪物的状态不再导出（还没被每秒的清理丢弃也一样）
        const FakeActor monsterActor = g_world.monsters[0x20000];
        g_world.monsters.erase(0x20000);
        FakeCampaignSave(*s, "persist.roster.json,persist.raid.json", true, false, 2);
        CHECK(ddlua::store::Decode(g_fp.saved["store_a"], &a, &why) && a.buffs.size() == 1 && FindState(a, DD_ACTOR_HERO, 12));
        g_world.monsters[0x20000] = monsterActor;  // 读档后战斗从存档恢复，怪物 guid 不变（P4-0D 0B-9）
    }  // 退出存档：载体还在游戏里，状态直接丢弃（不调用 on_remove）

    // 读第 1 次存的档：再加几条对不上的状态
    ddlua::store::Payload a;
    std::string why;
    CHECK(ddlua::store::Decode(savedA, &a, &why));
    a.buffs.push_back({ "lua_store_buff", DD_ACTOR_HERO, 12, 0, 9999, "{\"gone\":true}" });          // 条目不在
    a.buffs.push_back({ "lua_store_buff", DD_ACTOR_HERO, 13, 0, spdGuid, "{\"wrong\":true}" });      // guid 在，但 id 是 SPD5
    a.buffs.push_back({ "lua_store_buff", DD_ACTOR_HERO, 77, 0, 1, "{}" });                          // 英雄不在名册里
    a.buffs.push_back({ "lua_store_buff", DD_ACTOR_HERO, 12, 1, heroGuid, "{}" });                   // 备用名册
    g_fp.loads["store_a"] = { DD_PERSIST_OK, DD_PERSIST_LOADED, "地牢里的存档，raid 等探索存档读完再核对", ddlua::store::Encode(a), "1.4.0", true };
    g_fp.loads["store_b"] = { DD_PERSIST_OK, DD_PERSIST_PREV, "当前一代不一致（名册的哈希不同），用上一代；地牢里的存档，raid 等探索存档读完再核对", savedB, "2.0.1", true };
    {
        P4cSession s({ "store_a", "store_b" }, false);
        CHECK(s->LoadedCount() == 2);
        CHECK(Lua(*s, "store_a", R"lua(
            assert(MAIN.status == "loaded" and MAIN.reason:find("raid") and MAIN.counter == 1, tostring(MAIN.status))
            assert(dd.store.list[3] == "三" and math.type(dd.store.list[2]) == "float" and dd.store.list[4].deep == true)
            assert(APPLIED == 0)  -- 导入不调用 on_apply
        )lua"));
        CHECK(Lua(*s, "store_a", ("assert(dd.buff.state(dd.actor.hero(12), " + std::to_string(heroGuid) + ").shield == 4)").c_str()));
        CHECK(LogHas("脚本 Buff 状态（英雄）：从旁路恢复 1 条，丢弃 4 条"));
        CHECK(LogHas("lua_store_buff（hero#12，guid 9999）：条目不存在或 id 不一致") && LogHas("lua_store_buff（hero#77，guid 1）：英雄不在名册里"));
        CHECK(LogHas("（hero#13，guid " + std::to_string(spdGuid) + "）：条目不存在或 id 不一致") && LogHas("备用名册上的英雄不恢复"));
        CHECK(s->PendingStates() == 1);  // 怪物的等 raid 核对
        CHECK(Lua(*s, "store_b", R"lua(assert(MAIN.status == "prev" and MAIN.reason:find("上一代") and MAIN.counter == 7 and dd.store.saved_in_callback == 1))lua"));
        // 第三个返回值：等 raid 核对（审核 R2B 备注 6：上一代的原因里本来就可能有 raid，不能按原因判断）
        CHECK(Lua(*s, "store_a", R"lua(local st, why, pending = dd.store_status() assert(st == "loaded" and pending == true, tostring(pending)))lua"));
        CHECK(Lua(*s, "store_b", R"lua(local st, why, pending = dd.store_status() assert(st == "prev" and pending == true))lua"));
        // raid 一致：恢复怪物的状态，dd.store_status 的说明去掉「待核对」
        Verify(*s, "store_a", DD_PERSIST_LOADED, "");
        CHECK(s->PendingStates() == 0 && LogHas("[store_a] dd.store：raid 核对一致（loaded）") && LogHas("脚本 Buff 状态（怪物）：从旁路恢复 1 条，丢弃 0 条"));
        CHECK(Lua(*s, "store_a", ("local st = dd.buff.state(dd.actor.monster(4500), " + std::to_string(monsterGuid) +
                                     ") assert(st.shield == 2 and st.hits[2] == 2) local s1, w1, p1 = dd.store_status() assert(s1 == 'loaded' and w1 == nil and p1 == false)")
                                     .c_str()));
        // raid 不一致：状态改成 mismatch，原因写清楚
        Verify(*s, "store_b", DD_PERSIST_MISMATCH, "探索存档（persist.raid.json）的哈希不同");
        CHECK(Lua(*s, "store_b", R"lua(local st, why, pending = dd.store_status() assert(st == "mismatch" and why:find("persist.raid.json") and pending == false))lua"));
        CHECK(LogHas("[store_b] dd.store：raid 核对不一致（探索存档（persist.raid.json）的哈希不同）"));
        Verify(*s, "nobody", DD_PERSIST_MISMATCH, "x");  // 不认识的 key 忽略
        CHECK(LogHas("[store_b] dd.store：raid 核对不一致") && LogHas("这次进存档不再写这个 Mod 的旁路"));
        // 读档后再存：恢复的状态照常写出；raid 核对不一致的 store_b，DDCore 拒绝再写（F2，审核 S1），DDLua 只记一次、不报 ERROR
        g_fp.saved.erase("store_b");
        g_fp.blocked.push_back("store_b");
        FakeCampaignSave(*s, "persist.roster.json,persist.raid.json", true, true, 1);
        ddlua::store::Payload again;
        CHECK(ddlua::store::Decode(g_fp.saved["store_a"], &again, &why) && again.buffs.size() == 2 && again.store.find("\"counter\":1") != std::string::npos);
        FakeCampaignSave(*s, "persist.roster.json,persist.raid.json", true, true, 2);
        CHECK(g_fp.saved.count("store_b") == 0 && !LogHas("[store_b] persist.save 失败"));
        CHECK(CountLog("[store_b] dd.store：DDCore 这次进存档不再写这个 Mod 的旁路（raid 二次核对不一致，或归档原来的旁路失败，原因见 DDCore.log），之后的存档都不写，直到下次进存档") == 1);
        CHECK(LogHas("campaign.save #2（persist.roster.json,persist.raid.json，地牢，战斗中）：写出 1 个 Mod：store_a"));
        g_fp.blocked.clear();
    }
    // raid 没核对就退出：等着的怪物状态丢弃
    g_fp.loads["store_a"].data = savedA;
    {
        P4cSession s({ "store_a" }, false);
        CHECK(s->PendingStates() == 1);
        s->Teardown();
        CHECK(LogHas("还有 1 条旁路里的状态没有恢复（等 raid 核对的怪物状态），丢弃"));
    }
    // 城镇存档里有怪物状态（不该有）：直接丢弃
    g_fp.loads["store_a"].raidPending = false;
    {
        P4cSession s({ "store_a" }, false);
        CHECK(s->PendingStates() == 0 && LogHas("旁路里有怪物的状态，但存档不在地牢里"));
    }
    CHECK(CoreCount() == 0);
}

// ---------------------------------------------------------------- dd.store_status 的各种结果
void TestStoreStatus()
{
    ResetPersistFakes();
    g_fp.loads["store_a"] = { DD_PERSIST_OK, DD_PERSIST_MISMATCH, "当前一代：名册的哈希不同；上一代：没有文件" };
    g_fp.loads["store_b"] = { DD_PERSIST_OK, DD_PERSIST_LOADED, "", "garbage", "2.0.1" };  // 数据坏了
    {
        P4cSession s({ "store_a", "store_b" }, true);
        CHECK(Lua(*s, "store_a", R"lua(
            assert(MAIN.status == "mismatch" and MAIN.reason:find("名册") and MAIN.counter == nil and next(dd.store) == nil)
        )lua"));
        CHECK(Lua(*s, "store_b", R"lua(assert(MAIN.status == "mismatch" and MAIN.reason:find("解析失败") and next(dd.store) == nil))lua"));
        CHECK(LogHas("[store_a] dd.store：mismatch：当前一代：名册的哈希不同") && LogHas("[store_b] dd.store：旁路数据解析失败"));
        // 不一致之后照常写出新的（旁路保留一代，不自动修复）
        CHECK(Lua(*s, "store_a", "dd.store.fresh = true"));
        FakeCampaignSave(*s, "persist.roster.json", false, false, 1);
        CHECK(g_fp.saved["store_a"].find("\"fresh\":true") != std::string::npos);
    }
    // persist 服务拒绝（观察 hook 没装上）
    g_fp.loads["store_a"] = { DD_PERSIST_E_UNAVAILABLE };
    {
        P4cSession s({ "store_a" }, true);
        CHECK(Lua(*s, "store_a", R"lua(assert(MAIN.status == "unavailable" and MAIN.reason:find("persist.load 失败")))lua"));
    }
    // DDCore 没有 persist 服务
    {
        P4cSession s({ "store_a" }, true, TestConfig(), true, false);
        CHECK(Lua(*s, "store_a", R"lua(assert(MAIN.status == "unavailable" and MAIN.reason:find("没有 persist 服务")) dd.store.x = 1)lua"));
        FakeCampaignSave(*s, "persist.roster.json", false, false, 1);  // 不写
    }
    // 开发会话：dd.store 只在内存里
    ResetPersistFakes();
    {
        P4cSession s({ "store_a", "store_b" }, true, TestConfig(), false);
        CHECK(g_fp.loadCalls == 0);
        CHECK(Lua(*s, "store_b", R"lua(assert(MAIN.status == "empty" and MAIN.reason:find("开发会话")) dd.store.x = 1)lua"));
        FakeCampaignSave(*s, "persist.roster.json", false, false, 1);
        CHECK(g_fp.saved.empty() && s->SaveCount() == 0);
        CHECK(Lua(*s, "store_b", "assert(#SAVES == 1)"));  // 订阅照常收到
    }
    CHECK(CoreCount() == 0);
}

// ---------------------------------------------------------------- 上限、整体赋值、不能保存的数据
void TestStoreLimits()
{
    ResetPersistFakes();
    ddlua::Config config = TestConfig();
    config.storeMaxKb = 1;
    P4cSession s({ "store_a", "store_b" }, true, config);
    CHECK(Lua(*s, "store_a", R"lua(dd.store.big = string.rep("x", 2000))lua"));
    CHECK(Lua(*s, "store_b", R"lua(dd.store = { replaced = true, n = 3 })lua"));  // 整体赋值也认
    FakeCampaignSave(*s, "persist.roster.json", false, false, 1);
    CHECK(g_fp.saved.count("store_a") == 0 && LogHas("[store_a] 随存档写出的数据") && LogHas("超过 store_max_kb（1 KB），这次不写"));
    CHECK(LogHas("dd.store.big：字符串超过 1 KB"));  // 编码到上限就停下，说出位置（F2，审核 B-N6）
    CHECK(g_fp.saved["store_b"].find("\"replaced\":true") != std::string::npos && g_fp.saved["store_b"].find("\"n\":3") != std::string::npos);
    CHECK(Lua(*s, "store_b", R"lua(dd.store.later = 1)lua"));
    FakeCampaignSave(*s, "persist.roster.json", false, false, 2);
    CHECK(g_fp.saved["store_b"].find("\"later\":1") != std::string::npos && g_fp.saved["store_b"].find("\"replaced\":true") != std::string::npos);
    // 不能保存的 dd.store：这次不写、ERROR，说出位置
    CHECK(Lua(*s, "store_a", R"lua(dd.store.big = nil dd.store.f = print)lua"));
    FakeCampaignSave(*s, "persist.roster.json", false, false, 3);
    CHECK(g_fp.saved.count("store_a") == 0 && LogHas("[store_a] dd.store 不能保存") && LogHas("dd.store.f"));
    // 状态表后来变得不能保存：这一条跳过，其余照常
    CHECK(Lua(*s, "store_a", R"lua(
        dd.store.f = nil
        local h = dd.actor.hero(12)
        local _, g1 = h:add_buff("lua_store_buff", { state = { ok = 1 } })
        local _, g2 = h:add_buff("lua_store_buff", { state = { ok = 2 } })
        dd.buff.state(h, g2).bad = print
    )lua"));
    FakeCampaignSave(*s, "persist.roster.json", false, false, 4);
    ddlua::store::Payload a;
    std::string why;
    CHECK(ddlua::store::Decode(g_fp.saved["store_a"], &a, &why) && a.buffs.size() == 1 && a.buffs[0].json == "{\"ok\":1}");
    CHECK(LogHas("脚本 Buff 的状态不能保存，这一条这次跳过"));
    // 脚本 Buff 的状态累计超过 store_max_kb：编码到上限就停下，这个 Mod 这次不写（F2，审核 B-N6），别的 Mod 照常
    CHECK(Lua(*s, "store_a", R"lua(
        local h = dd.actor.hero(12)
        for i = 1, 3 do h:add_buff("lua_store_buff", { state = { pad = string.rep("y", 400) } }) end
    )lua"));
    g_fp.saved.erase("store_a");
    FakeCampaignSave(*s, "persist.roster.json", false, false, 5);
    CHECK(g_fp.saved.count("store_a") == 0 && LogHas("[store_a] 随存档写出的数据超过 store_max_kb（1 KB），这次不写") && LogHas("脚本 Buff 的状态就已经超过"));
    CHECK(g_fp.saved.count("store_b") == 1);
    // campaign.save 之外 DDCore 拒绝 save（这里假服务的 saving 为假）
    const DDPersistSaveInfo info{ sizeof(DDPersistSaveInfo), "persist.roster.json", false, false, true, 1, 5 };
    s->PersistSave(info);
    CHECK(LogHas("persist.save 失败：时机不对"));
}

// ---------------------------------------------------------------- 经过 plugin.cpp：声明、保存回调、核对回调都送到存档会话
void TestStorePlugin()
{
    // DDPlugin_Load 在场景预演（TestScenarioRehearsal）里调用过一次，那时向假服务声明了持久化；单独运行本组时在这里加载
    if (g_fp.declares == 0) {
        const DDApi api{ sizeof(DDApi), DD_ABI_VERSION, &FakeQuery, kPlugin };
        CHECK(DDPlugin_Load(&api));
    }
    CHECK(g_fp.declares == 1 && g_fp.onSave && g_fp.onVerify);
    if (!g_fp.onSave || !g_fp.onVerify) {
        return;
    }
    g_log.clear();
    ResetPersistFakes();
    CHECK(ResetHubFromDump());
    EnsureSaveTopic();
    ResetWorld();
    g_fp.loads["store_b"] = { DD_PERSIST_OK, DD_PERSIST_LOADED, "", ddlua::store::Encode({ "{\"counter\":41}", {} }), "2.0.1" };
    g_fakeModsAvailable = true;
    g_fakeMods = { { "DDLua Test Store B", "mod_local_source", Fixture("store_b") } };
    FirePhase(DD_PHASE_CAMPAIGN_LOAD, 61);
    CHECK(LogHas("[store_b] dd.store：loaded") && g_fp.loadCalls == 1);
    Fire f("campaign.save");
    f.S("files", "persist.roster.json").B("in_raid", false).B("in_battle", false).B("main_thread", true).I("serial", 61).I("index", 1);
    f.Go();
    const DDPersistSaveInfo info{ sizeof(DDPersistSaveInfo), "persist.roster.json", false, false, true, 61, 1 };
    g_fp.saving = true;
    g_fp.onSave(&info, g_fp.user);
    g_fp.saving = false;
    CHECK(g_fp.saved["store_b"].find("\"counter\":41") != std::string::npos && g_fp.saved["store_b"].find("\"saved_in_callback\":1") != std::string::npos);
    const DDPersistVerify verify{ sizeof(DDPersistVerify), "store_b", DD_PERSIST_MISMATCH, "测试：raid 不一致" };
    g_fp.onVerify(&verify, g_fp.user);
    CHECK(LogHas("[store_b] dd.store：raid 核对不一致（测试：raid 不一致）"));
    FirePhase(DD_PHASE_CAMPAIGN_UNLOAD, 61);
    // 没有存档会话时回调什么都不做
    g_fp.saving = true;
    g_fp.onSave(&info, g_fp.user);
    g_fp.saving = false;
    g_fp.onVerify(&verify, g_fp.user);
    g_fakeModsAvailable = false;
    g_fakeMods.clear();
    CHECK(TickSubs() == 0 && CoreCount() == 0);
}

// ---------------------------------------------------------------- 规则回调交还原函数（P4c1 的遗留修正）
void TestRuleOriginal()
{
    P4bSession s({ "ev_a" });
    CHECK(Lua(*s, "ev_a", R"lua(
        MODE = "true"
        R = dd.buff.rule("lua_orig", function()
            if MODE == "error" then error("故意") end
            if MODE == "nil" then return nil end
            return MODE == "true"
        end)
    )lua"));
    bool result = false;
    CHECK(FakeRuleEval("lua_orig", 0x10000, 0, 42, false, &result) && result);
    CHECK(Lua(*s, "ev_a", "MODE = 'false'"));
    CHECK(FakeRuleEval("lua_orig", 0x10000, 0, 42, true, &result) && result);  // 不成立、is_false_rule 取反
    // 出错：交还原函数（城镇里规则 42 的前置检查就返回假、不取反，和没有 Lua Mod 时一样），不是「不成立再取反」
    CHECK(Lua(*s, "ev_a", "MODE = 'error'"));
    result = true;
    CHECK(!FakeRuleEval("lua_orig", 0x10000, 0, 42, true, &result) && result);
    CHECK(LogHas("这次交还游戏原来的判定") && g_fc.ruleOriginal == 1);
    // Y6：返回 nil（或不返回）是有意交还原函数，不算出错，连续多次也不停用
    CHECK(Lua(*s, "ev_a", "MODE = 'nil'"));
    for (int i = 0; i < 5; ++i) {
        result = true;
        CHECK(!FakeRuleEval("lua_orig", 0x10000, 0, 42, true, &result) && result);
    }
    CHECK(g_fc.ruleOriginal == 6 && !LogHas("返回了 nil"));  // max_failures = 3：算出错的话已经停用了
    CHECK(Lua(*s, "ev_a", "assert(R:active()) MODE = 'true'"));
    CHECK(FakeRuleEval("lua_orig", 0x10000, 0, 42, false, &result) && result);
    s->Teardown();

    // C-Y7：每个 Mod 每帧的 combat 回调总时间（combat_frame_ms）用完后，这一帧余下的规则交还原函数，下一帧重新计
    ddlua::Config config = TestConfig();
    config.combatFrameMs = 2;
    P4bSession f({ "ev_a" }, config);
    CHECK(Lua(*f, "ev_a", R"lua(
        CALLS = 0
        dd.buff.rule("lua_slow", function()
            CALLS = CALLS + 1
            local t = os.clock() + 0.0008
            while os.clock() < t do end
            return true
        end)
    )lua"));
    int answered = 0, passed = 0;
    for (int i = 0; i < 10; ++i) {
        result = false;
        FakeRuleEval("lua_slow", 0x10000, 0, 42, false, &result) ? ++answered : ++passed;
    }
    std::printf("  combat_frame_ms = 2：一帧里 10 次判定（每次约 0.8 ms），进 Lua %d 次，交还原函数 %d 次\n", answered, passed);
    CHECK(answered >= 2 && answered <= 4 && passed == 10 - answered && LogHas("combat_frame_ms"));
    Tick(*f, 1.0);  // 帧边界清零
    CHECK(FakeRuleEval("lua_slow", 0x10000, 0, 42, false, &result) && result);
    CHECK(Lua(*f, "ev_a", ("assert(CALLS == " + std::to_string(answered + 1) + ")").c_str()));
    f->Teardown();

    // C-Y7：每个 Mod 最多 max_per_mod 条载体有状态
    ddlua::Config few = TestConfig();
    few.maxPerMod = 2;
    P4bSession st({ "ev_a" }, few);
    CHECK(Lua(*st, "ev_a", R"lua(
        APPLIED = 0
        assert(dd.buff.define{ id = "lua_cap", stat = "combat_stat_add", sub_stat = "speed_rating", duration = 3,
            on_apply = function() APPLIED = APPLIED + 1 end })
        local h = dd.actor.hero(12)
        assert(h:add_buff("lua_cap", { state = { n = 1 } }) and h:add_buff("lua_cap", { state = { n = 2 } }))
        local no, why = h:add_buff("lua_cap", { state = { n = 3 } })
        assert(no == nil and why:find("max_per_mod"), tostring(why))
        assert(APPLIED == 2 and dd.stats().states == 2, tostring(dd.stats().states))
    )lua"));
}

// ================================================================ P4c2：场景 Mod 的离线预演
// shield_lite（DDShield 的 Lua 复刻）、p4c_shield（驱动护盾规则）、p4c_persist（dd.store 和脚本 Buff 状态随存档）、p4b_combat，
// 在假世界里按 docs/10-p4-scenario.md 的流程走一遍：城镇存档（招募）、读档；走廊存档、读档（raid 核对，加载线程上的延迟 buff.remove）；
// 战斗中存档、读档（死门上的吸收）；打完战斗、撤退一场、回城、再读城镇存档（解雇）；改过的旁路（不一致）；一个没有 Lua Mod 的存档。
// DDLua.log、DDCore.log（格式照 core.cpp、persist.cpp）和 rig_tamper.log（格式照 rig.py tamper-persist，带运行标识）写到
// build/p4c_rehearsal/，tools/p4_scenario_check.py --selftest 用它们自测。
// 假世界的结算照 DDCore：damage.final 的 absorb 全部吸收时不扣血，部分吸收时先补回再扣（死门上不补）；加删 buff 后刷新属性时
// 删掉 remove_if_not_active、规则不成立的条目（FakeRefresh）
constexpr std::uintptr_t kP4cHero = 0x10000;   // hero#12（假世界里英雄的职业都是 crusader）
constexpr std::uintptr_t kP4cHero2 = 0x10100;  // hero#13
constexpr const char* kP4cRunId = "20261114-100000-5150";
constexpr const char* kP4cRoster = "0123456789ABCDEF";
constexpr const char* kP4cTampered = "0123456789ABCDE0";
FILE* g_p4cCore = nullptr;
double g_p4cT = 0;
std::uint32_t g_p4cSerial = 0;
std::uint32_t g_p4cSaveIndex = 0;       // 整次运行的 campaign.save 次数（hook 报告、退到桌面那一行）
std::uint32_t g_p4cSessionSave = 0;     // 本次进存档的第几次（campaign.save #k，DDCore 每次 CampaignLoad 从 0 数起）
std::uint32_t g_p4cWrites = 0;        // 观察到的存档写入次数（hook 报告里的「写 N 次」）
bool g_p4cRosterWritten = false;      // 这次进存档写过名册了没有（DDCore 只记第一次）
int g_p4cRound = 0;

void P4cCore(const std::string& text, int level = DD_LOG_INFO)
{
    g_logClock = g_p4cT;
    if (g_p4cCore) {
        WriteLogLine(g_p4cCore, level, text);
    }
}

std::vector<ddlua::ModSource> P4cSources()
{
    const std::string root = std::string(DD_PROJECT_ROOT) + "/";
    return { { "DDLua Test P4b Combat（mod_local_source）", root + "tests/lua/scenario/p4b_combat" },
        { "DDLua Example Shield Lite（mod_local_source）", root + "examples/lua/shield_lite" },
        { "DDLua Test P4c Shield（mod_local_source）", root + "tests/lua/scenario/p4c_shield" },
        { "DDLua Test P4c Persist（mod_local_source）", root + "tests/lua/scenario/p4c_persist" } };
}

// 游戏按数据层的 effects 文件加载 shield_lite 的 effect（本身不改属性，没有 buff）
void P4cLoadShieldEffects()
{
    FILE* file = nullptr;
    _wfopen_s(&file, ddlua::Utf8ToWide(std::string(DD_PROJECT_ROOT) + "/examples/lua/shield_lite/effects/shield_lite.effects.darkest").c_str(), L"rb");
    CHECK(file != nullptr);
    if (!file) {
        return;
    }
    char line[1024];
    std::size_t n = 0;
    while (std::fgets(line, sizeof(line), file)) {
        const std::string text(line);
        const std::size_t at = text.find(".name \"");
        if (text.rfind("effect:", 0) != 0 || at == std::string::npos) {
            continue;
        }
        const std::size_t from = at + 7;
        const std::string name = text.substr(from, text.find('"', from) - from);
        FakeEffect e;
        e.info.size = sizeof(e.info);
        strcpy_s(e.info.name, name.c_str());
        e.info.target = text.find("\"performer\"") != std::string::npos ? 0 : 1;
        e.info.chance = 1;
        e.info.on_hit = true;
        g_fc.effects[name] = e;
        ++n;
    }
    std::fclose(file);
    CHECK(n >= 20);
}

// 游戏判定带脚本规则的 buff：每帧对身上有这些 buff 的角色判定一次
void P4cRules()
{
    for (const auto& [actor, table] : g_fc.tables) {
        if (!g_world.heroes.count(actor) && !g_world.monsters.count(actor)) {
            continue;
        }
        for (const auto& b : table) {
            const std::string id = b.def.id;
            const char* rule = id == "p4b_rule_buff" ? "p4b_rule" : id == "p4c_shield_probe" ? "shielded" : nullptr;
            bool holds = false;
            if (rule) {
                FakeRuleEval(rule, actor, 0, 42, false, &holds, id.c_str());
            }
        }
    }
}

void P4cFrames(ddlua::Session& s, double seconds)
{
    for (const double end = g_p4cT + seconds; g_p4cT < end; g_p4cT += 0.05) {
        g_logClock = g_p4cT;
        FakeDrain();
        P4cRules();
        Tick(s, g_p4cT);
    }
    g_logClock = g_p4cT;
}

void P4cFire(const char* topic)
{
    g_logClock = g_p4cT;
    Fire f(topic);
    f.Go();
}

void P4cRoundStart()
{
    ++g_p4cRound;
    g_logClock = g_p4cT;
    Fire f("battle.round.start");
    f.P("battle", P(0x60000)).I("round", g_p4cRound);
    f.Go();
}

void P4cTurn(std::uintptr_t actor)
{
    g_logClock = g_p4cT;
    Fire f("battle.turn.start");
    f.P("actor", P(actor));
    f.Go();
}

// 怪物死亡（actor.death，p4c_persist 据此区分打完和撤退）
void P4cMonsterDeath(std::int64_t guid)
{
    g_logClock = g_p4cT;
    Fire f("actor.death");
    f.I("hero_id", 0).B("is_monster", true).I("monster_guid", guid).I("attacker_id", 0).I("damage_type", 0);
    f.Go();
}

// 技能：skill.pre → 技能自带的 effect（effect.pre）→ skill.post。圣骑士的 Smite 在数据层带 shield_self_gain_5
void P4cSkill(std::uintptr_t performer, std::uintptr_t target, const char* effect)
{
    g_logClock = g_p4cT;
    Fire pre("skill.pre");
    pre.P("performer", P(performer)).P("skill", P(0x80000)).P("target", P(target)).I("target_count", 1).B("friendly", false);
    pre.Go();
    if (effect) {
        Fire e("effect.pre");
        e.P("performer", P(performer)).P("target", P(performer)).P("effect", P(0x81000)).S("effect_name", effect);
        e.Go();
    }
    Fire post("skill.post");
    post.P("performer", P(performer)).P("skill", P(0x80000)).P("target", P(target)).I("target_count", 1).B("friendly", false);
    post.Go();
}

// 一次伤害：damage.pre → damage.final（读回 absorb，照 DDCore 的 ConsumeBlockDetour 结算）→ damage.post
void P4cHit(std::uintptr_t target, std::uintptr_t attacker, double damage)
{
    g_logClock = g_p4cT;
    FakeActor& actor = ActorAt(target);
    const bool hero = g_world.heroes.count(target) != 0;
    const double before = actor.hp;
    Fire pre("damage.pre");
    pre.P("target", P(target)).P("attacker", P(attacker)).Fl("damage", damage).I("damage_type", 0).I("source_id", 1).I("source_type", 0)
        .B("crit", false).Fl("hp_before", before).B("is_hero", hero);
    pre.Go();
    const double dmg = pre.At("damage").v.f;
    const bool deathsDoor = hero && before <= 0;
    Fire fin("damage.final");
    fin.P("target", P(target)).P("attacker", P(attacker)).Fl("damage", dmg).I("damage_type", 0).B("blocked", false).B("deaths_door", deathsDoor)
        .Fl("hp_before", before).B("is_hero", hero).Fl("absorb", 0);
    fin.Go();
    const double absorb = (std::min)((std::max)(fin.At("absorb").v.f, 0.0), dmg);
    double after = before;
    if (absorb < dmg && !deathsDoor) {
        after = before + absorb - dmg;
        if (hero && after < 0) {
            after = 0;
        }
    }
    actor.hp = static_cast<float>(after);
    Fire post("damage.post");
    post.P("target", P(target)).P("attacker", P(attacker)).Fl("damage", dmg).I("damage_type", 0).I("source_id", 1).I("source_type", 0)
        .B("crit", false).Fl("hp_before", before).B("is_hero", hero).Fl("hp_after", actor.hp).P("timeline", P(0x90000)).B("cancelled", false);
    post.Go();
}

// 技能的另一种时序（P4 场景日志里 Smite 的实际情况：skill.pre 后 0.1 秒结算伤害，约 2 秒后才施加自带的 effect）：
// skill.pre → 伤害 → skill.post，过 wait 秒（技能演出）再施加 effect。返回 shield_lite 给这次施加记的技能（SHIELD apply 行的
// skill 字段），没有 apply 行时返回 "(无)"
std::string P4cSkillLate(ddlua::Session& s, std::uintptr_t performer, std::uintptr_t target, const char* effect, double damage, double wait)
{
    g_logClock = g_p4cT;
    Fire pre("skill.pre");
    pre.P("performer", P(performer)).P("skill", P(0x80000)).P("target", P(target)).I("target_count", 1).B("friendly", false);
    pre.Go();
    P4cHit(target, performer, damage);
    Fire post("skill.post");
    post.P("performer", P(performer)).P("skill", P(0x80000)).P("target", P(target)).I("target_count", 1).B("friendly", false);
    post.Go();
    P4cFrames(s, wait);
    const std::size_t mark = g_log.size();
    Fire e("effect.pre");
    e.P("performer", P(performer)).P("target", P(performer)).P("effect", P(0x81000)).S("effect_name", effect);
    e.Go();
    for (std::size_t i = mark; i < g_log.size(); ++i) {
        const std::string& line = g_log[i];
        const std::size_t at = line.find("\"skill\":\"");
        if (line.rfind("[shield_lite] SHIELD ", 0) == 0 && line.find("\"ev\":\"apply\"") != std::string::npos && at != std::string::npos) {
            const std::size_t from = at + 9;
            return line.substr(from, line.find('"', from) - from);
        }
    }
    return "(无)";
}

void P4cState(const char* to)
{
    g_logClock = g_p4cT;
    Fire f("app.state");
    f.I("from", 1).I("to", 2).S("from_name", "x").S("to_name", to);
    f.Go();
}

void P4cHookReport(const char* title, std::size_t subs, std::size_t semantic)
{
    P4cCore(std::string("======== hook 报告（") + title + "），共 5 个目标 ========");
    P4cCore("#1 内联 Darkest.exe+0x3DCE10 偷取 5 字节，状态 正常");
    P4cCore("事件系统：已注册 listener 0 个，语义 hook " + std::to_string(semantic) + " 个，订阅 " + std::to_string(subs) + " 个");
    P4cCore("persist：观察 hook 4 个（已装），声明的插件 1 个，观察到存档读 0 次、写 " + std::to_string(g_p4cWrites) + " 次，campaign.save 派发 " +
        std::to_string(g_p4cSaveIndex) + " 次，旁路写盘 " + std::to_string(g_p4cSaveIndex * 4) + " 个（合并 0 次，失败 0 次，重试 0 次）");
    P4cCore("======== 报告结束 ========");
}

// 游戏写完一组存档：DDCore 的 campaign.save 行，再派发主题、调用 DDLua 的保存回调
void P4cSave(ddlua::Session& s, bool inRaid, bool inBattle)
{
    ++g_p4cSaveIndex;
    ++g_p4cSessionSave;
    const char* files = inRaid ? "persist.map.json,persist.roster.json,persist.game.json,persist.raid.json"
                               : "persist.roster.json,persist.town.json,persist.estate.json,persist.game.json";
    g_logClock = g_p4cT;
    g_p4cWrites += 4;
    if (!g_p4cRosterWritten) {
        g_p4cRosterWritten = true;
        P4cCore(std::string("persist：观察到写存档：设备 steam_profile_7，persist.roster.json，4096 字节，哈希 ") + kP4cRoster + "，" +
            (inRaid ? "其他" : "主") + "线程（进存档后第一次写名册）");
    }
    FakeCampaignSave(s, files, inRaid, inBattle, g_p4cSessionSave);
    P4cCore("persist：campaign.save #" + std::to_string(g_p4cSessionSave) + "（帧边界）：文件 " + files + "；地牢 " + (inRaid ? "1" : "0") + "，战斗 " +
        (inBattle ? "1" : "0") + "，都在主线程；名册 " + kP4cRoster + "/4096" + (inRaid ? "，raid 00000000DEADBEEF/2048" : "") + "；提交旁路文件 " +
        std::to_string(g_fp.saved.size()) + " 个");
}

struct P4cRun {
    ddlua::Host host;
    std::unique_ptr<ddlua::Session> session;
    std::uint32_t serial = 0;
};

// 进存档：DDCore 的 CampaignLoad 和 persist 行，会话加载 4 个 Mod，再派发 campaign.load。status 是 DD_PERSIST_EMPTY 时是新战役；
// DD_PERSIST_MISMATCH、DD_PERSIST_PREV 只给 p4c_persist（场景里改的是它的旁路），PREV 交上一代的数据，别的 Mod 照常 loaded
std::unique_ptr<P4cRun> P4cEnter(std::uint32_t status, bool inRaid, const std::string& reason = "")
{
    auto run = std::make_unique<P4cRun>();
    run->serial = ++g_p4cSerial;
    ++g_world.campaign;  // 每次进存档 actor 句柄换代（英雄的状态按 hero id 导入）
    g_world.active = true;
    g_p4cRound = 0;
    g_p4cSessionSave = 0;
    g_p4cRosterWritten = false;
    g_fp.loads.clear();
    const std::string raidReason = inRaid ? "地牢里的存档，raid 等探索存档读完再核对" : "";
    for (const auto& [key, data] : g_fp.saved) {
        FakePersistKey k{ DD_PERSIST_OK, status, status == DD_PERSIST_LOADED ? raidReason : reason, data, "1.0.0", inRaid };
        if ((status == DD_PERSIST_MISMATCH || status == DD_PERSIST_PREV) && key != "p4c_persist") {
            k.status = DD_PERSIST_LOADED;
            k.reason = raidReason;
        }
        if (k.status == DD_PERSIST_PREV) {
            k.data = g_fp.prevSaved[key];
        }
        g_fp.loads[key] = k;
    }
    const std::string serial = std::to_string(run->serial);
    P4cCore("阶段 CampaignLoad，帧 " + std::to_string(1000 * run->serial) + "，存档序号 " + serial + "，线程 1，Campaign=0000000000070000，" +
        "Buff::Library=0000000000071000，状态 last=2 next=" + (inRaid ? "6" : "19"));
    P4cCore("启用 Mod 列表 4 项（已安装 300 个），找到目录 4 项");
    P4cCore(std::string("persist：进存档：设备 steam_profile_7，账号 10000001，名册 ") + (status == DD_PERSIST_MISMATCH ? "1111111111111111" : kP4cRoster) +
        "/4096，存档在" + (inRaid ? "地牢里" : "城镇") + "；旁路目录 \\\\?\\D:\\rig\\DDModLoader\\save\\10000001\\steam_profile_7");
    if (status == DD_PERSIST_EMPTY) {
        for (const char* key : { "p4b_combat", "p4c_persist", "p4c_shield", "shield_lite" }) {
            P4cCore(std::string("persist：DDLua/") + key + "：empty：没有旁路文件");
        }
    }
    for (const auto& [key, k] : g_fp.loads) {
        std::string text = "persist：DDLua/" + key + "：";
        if (k.status == DD_PERSIST_MISMATCH) {
            P4cCore(text + "mismatch：" + k.reason, DD_LOG_WARN);
        } else {
            const bool prev = k.status == DD_PERSIST_PREV;
            P4cCore(text + (prev ? "prev（上一代" : "loaded（当前一代") + "，版本 1.0.0，写于 1763100000123，数据 " + std::to_string(k.data.size()) + " 字节）" +
                (k.reason.empty() ? "" : "：" + k.reason));
        }
    }
    g_logClock = g_p4cT;
    WriteLogLine(g_logSink, DD_LOG_INFO, "[存档#" + serial + "] 进入存档：启用 4 个 Mod，0 个没有找到目录（见 DDCore.log）");
    g_log.emplace_back("[存档#" + serial + "] 进入存档：启用 4 个 Mod，0 个没有找到目录（见 DDCore.log）");
    EnsureSaveTopic();
    run->host = MakeCombatHost();
    run->host.persist = &kFakePersist;
    ddlua::Config config = TestConfig();
    config.loadBudgetMs = 500;
    config.logLinesPerSec = 100000;  // 预演把几十秒的流程压在一两秒里，按真实时间限速会丢行
    run->session = std::make_unique<ddlua::Session>(run->host, config, "存档#" + serial);
    run->session->SetPersistent(true);
    g_now = g_p4cT;
    run->session->SetClock([] { return g_now; });
    run->session->Load(P4cSources());
    CHECK(run->session->LoadedCount() == 4);
    P4cHookReport("CampaignLoad", g_hub->ExternalSubscriberCount(), 12);
    g_logClock = g_p4cT;
    Fire load("campaign.load");
    load.I("serial", run->serial).P("campaign", P(0x70000));
    load.Go();
    return run;
}

// 地牢存档：as_raid_start 加载结束后核对 raid（DDCore 的 OnFrameBoundary → on_verify）
void P4cVerifyRaid(P4cRun& run, bool ok)
{
    for (const auto& [key, k] : g_fp.loads) {
        if (k.status != DD_PERSIST_LOADED) {
            continue;
        }
        if (ok) {
            P4cCore("persist：DDLua/" + key + "：raid 核对一致（00000000DEADBEEF/2048）");
            Verify(*run.session, key.c_str(), DD_PERSIST_LOADED, "");
        }
    }
}

void P4cLeave(std::unique_ptr<P4cRun>& run)
{
    g_logClock = g_p4cT;
    P4cCore("阶段 CampaignUnload，帧 " + std::to_string(1000 * run->serial + 500) + "，存档序号 " + std::to_string(run->serial) + "，线程 1");
    if (g_fc.ruleCalls) {
        P4cCore("规则统计（退出存档时）：命中 " + std::to_string(g_fc.ruleCalls) + " 次（交还原函数 " + std::to_string(g_fc.ruleOriginal) +
            " 次），跳过 0 次，崩溃 0 次，累计 0.4 ms，单次最长 6.0 us，单帧峰值 6.2 us / 9 次");
    }
    // 同 core.cpp：先 SetCampaignActive(false)（英雄句柄解析不了），再派发 CampaignUnload。拆除时不调用 on_remove（F7）
    g_world.active = false;
    const std::size_t goneBefore = CountLog("条载体的 actor 不再能解析");
    run->session->Teardown();
    if (CountLog("条载体的 actor 不再能解析") != goneBefore) {
        std::printf("    存档#%u 退出时脚本 Buff 的载体按 gone 调用了 on_remove\n", run->serial);
        CHECK(false);
    }
    g_hub->EndScope(DD_EVENT_SCOPE_CAMPAIGN);
    run.reset();
    g_fc.queue.clear();
    g_p4cT += 1;
    // 退出存档时游戏删掉 Raid：下一次进存档时（地牢里的存档要等 load_raid 线程）才重新有队伍、战斗状态。
    // 怪物和它们的 buff 表留在假世界里，读战斗中的存档时由 P4cRaidLoading 「恢复」
    g_fc.raid = false;
    g_world.party.clear();
    g_world.battleState = 0;
}

// 游戏删掉 remove_on_battle_complete 的 buff（战斗结束），或回城时队伍英雄身上来源 skill 的 buff（town_visit）
void P4cGameRemoveWhere(bool battleComplete)
{
    for (auto& [actor, table] : g_fc.tables) {
        std::vector<std::uint32_t> guids;
        for (const auto& b : table) {
            const bool hit = battleComplete ? b.def.remove_on_battle_complete
                                            : (b.def.source == 1 && g_world.heroes.count(actor) != 0);
            if (hit) {
                guids.push_back(b.guid);
            }
        }
        for (const auto guid : guids) {
            FakeGameRemove(actor, guid, battleComplete ? "other" : "town_visit");
        }
    }
}

std::size_t P4cTagCount(const char* who, const char* tag)
{
    return CountLog(std::string("[") + who + "] 通过 [" + tag + "]");
}

// p4b_combat 在某个场合做了几轮（「信息 [round] where=…」）
std::size_t P4cRounds(const char* where)
{
    return CountLog(std::string("[p4b_combat] 信息 [round] where=") + where + " ");
}

// 读地牢里的存档（审核 R3 之后的真实时序）：CampaignLoad 时还没有 Raid（上一次退出存档时删掉了），之后 as_raid_start 停留期间
// load_raid 线程新建 Raid、读档线程重建战斗（加载中：读接口、枚举都读不到），结束后才有队伍和战斗状态
void P4cRaidLoading(P4cRun& run, double seconds, std::int32_t battleState)
{
    P4cState("as_raid_start");
    g_fc.loading = true;
    P4cFrames(*run.session, seconds);
    g_fc.loading = false;
    g_fc.raid = true;
    g_world.party = { kP4cHero, kP4cHero2 };
    g_world.battleState = battleState;
    P4cState("as_raid_update");
}

// 一场战斗里 p4b_combat 不能做走廊、城镇那一轮（审核 R3-B3：读进战斗中的存档时游戏不派发 battle.start，旧的场景 Mod
// 把这场战斗当成走廊）；开战前记下，战斗结束时核对
struct P4cBattleGuard {
    std::size_t corridor = 0, town = 0;
};

P4cBattleGuard P4cGuardNow()
{
    return { P4cRounds("corridor"), P4cRounds("town") };
}

void P4cGuardCheck(const P4cBattleGuard& g, const char* what)
{
    if (P4cRounds("corridor") != g.corridor || P4cRounds("town") != g.town) {
        std::printf("    %s：p4b_combat 在战斗里做了走廊或城镇那一轮（审核 R3-B3）\n", what);
        CHECK(false);
    }
}

// 开战：Battle::Create 填好怪物表、置 bs_create，再派发 battle.start。之后 5～9 秒才有第一次出手（探针日志，审核 R3-B1），
// 这段时间里游戏不派发带角色的事件，调用方用 P4cFrames 空等
P4cBattleGuard P4cBattleStart(std::initializer_list<std::pair<std::uintptr_t, FakeActor>> monsters)
{
    for (const auto& [p, actor] : monsters) {
        g_world.monsters[p] = actor;
    }
    ++g_world.battle;
    g_world.battleState = 1;
    g_p4cRound = 0;
    const P4cBattleGuard guard = P4cGuardNow();
    P4cFire("battle.start");
    P4cRoundStart();
    return guard;
}

// 战斗结束（打完或撤退）：游戏先把状态清零（撤退时 5A93B3），删掉 remove_on_battle_complete 的 buff，清空怪物表，再派发 battle.end
void P4cBattleEnd(const P4cBattleGuard& guard, const char* what)
{
    P4cGuardCheck(guard, what);
    g_world.battleState = 0;
    P4cGameRemoveWhere(true);
    g_world.monsters.clear();
    ++g_world.battle;
    P4cFire("battle.end");
    g_hub->EndScope(DD_EVENT_SCOPE_BATTLE);
}

void TestP4cScenarioRehearsal()
{
    g_log.clear();
    ResetPersistFakes();
    CHECK(ResetHubFromDump());
    ResetWorld();
    ResetCombat();
    ResetP4bRoot();
    P4cLoadShieldEffects();
    g_fc.smiteEffects = { "Unholy Killer 1", "shield_self_gain_5" };  // shield_lite 的数据层给圣骑士的 Smite 加上 shield_self_gain_5
    g_world.heroes[kP4cHero].hp = 30;
    g_world.heroes[kP4cHero2].hp = 30;
    g_world.monsters.clear();
    g_p4cT = 0;
    g_p4cSerial = 0;
    g_p4cSaveIndex = 0;
    const std::string outDir = ProjectFile("build/p4c_rehearsal");
    CreateDirectoryW(ddlua::Utf8ToWide(ProjectFile("build")).c_str(), nullptr);
    CreateDirectoryW(ddlua::Utf8ToWide(outDir).c_str(), nullptr);
    FILE* luaLog = nullptr;
    _wfopen_s(&luaLog, ddlua::Utf8ToWide(outDir + "/DDLua.log").c_str(), L"wb");
    _wfopen_s(&g_p4cCore, ddlua::Utf8ToWide(outDir + "/DDCore.log").c_str(), L"wb");
    CHECK(luaLog && g_p4cCore);
    if (!luaLog || !g_p4cCore) {
        return;
    }
    std::fputs("\xEF\xBB\xBF", luaLog);
    std::fputs("\xEF\xBB\xBF", g_p4cCore);
    g_logSink = luaLog;
    P4cCore(std::string("DDCore 1.0.0 启动，运行标识 ") + kP4cRunId + "，主线程 1，游戏根目录 （离线预演）");
    P4cCore("插件 DDLua 加载成功（版本 1.0.0）");
    WriteLogLine(luaLog, DD_LOG_INFO, std::string("日志开始，DDCore 1.0.0，运行标识 ") + kP4cRunId + "，插件路径 （离线预演）");
    WriteLogLine(luaLog, DD_LOG_INFO, "[DDLua] DDLua 1.0.0（Lua 5.4.9，按 C++ 编译；Lua API 1）加载，DDCore 1.0.0");
    WriteLogLine(luaLog, DD_LOG_INFO, "[DDLua] 配置（没有 DDLua.cfg，用默认值）：memory_limit_mb=128 budget_ms=50 load_budget_ms=500 dev_mode=0");
    WriteLogLine(luaLog, DD_LOG_INFO, "[DDLua] 只订阅了 lifecycle（CampaignLoad、CampaignUnload），没有订阅事件主题，没有安装 hook");
    P4cCore("persist：插件 DDLua 声明了存档旁路，观察 hook 已安装 4 个（排队写文件、读文件、选槽复制 backup、退到桌面），只读不改游戏行为");
    P4cCore("阶段 AssetsReady，帧 2，线程 1");
    P4cHookReport("AssetsReady", 0, 0);

    // ---- 1. 城镇：新战役（没有旁路）；p4c_persist 给英雄加两条同 id 的载体，招募一个英雄（存档），退回主菜单
    g_fc.raid = false;
    auto run = P4cEnter(DD_PERSIST_EMPTY, false);
    P4cFrames(*run->session, 4.5);
    CHECK(P4cTagCount("p4c_persist", "stack") == 1);
    P4cFire("hero.recruit");                             // 招募：游戏写名册
    P4cSave(*run->session, false, false);
    P4cLeave(run);

    // ---- 2. 读城镇存档：核对；出发（load_raid 线程在跑时是加载中），进地牢后先停 5 秒（p4b_combat 的走廊一轮：走廊里游戏不派发
    // 带角色的事件，场景 Mod 靠 dd.actor.party 认识队伍，审核 R3-B3），再走几步（每步存档）
    run = P4cEnter(DD_PERSIST_LOADED, false);
    P4cFrames(*run->session, 4.0);
    CHECK(P4cTagCount("p4c_persist", "persist.town") == 1);
    P4cRaidLoading(*run, 1.0, 0);
    {
        const std::size_t before = P4cRounds("corridor");
        P4cFrames(*run->session, 5.0);
        if (P4cRounds("corridor") != before + 1) {
            std::printf("    第一次进走廊、停 5 秒后 p4b_combat 没有做走廊那一轮（审核 R3-B3）\n");
            CHECK(false);
        }
    }
    for (int step = 0; step < 4; ++step) {
        P4cFrames(*run->session, 1.0);
        P4cFire("raid.step");
        P4cSave(*run->session, true, false);
    }
    P4cFrames(*run->session, 1.0);
    P4cLeave(run);

    // ---- 3. 读走廊存档（地牢里：先按名册给数据，load_raid 线程跑完、raid 核对后判定），读档后等 10 秒；
    // 打一场战斗，战斗中存档后退回主菜单
    run = P4cEnter(DD_PERSIST_LOADED, true);
    {
        // load_raid 线程上游戏删了一条 buff：DDCore 复制字段、排到主线程作为延迟事件派发
        const std::uint32_t guid = FakeGameAdd(kP4cHero2, "SPD5", false);
        FakeGameRemove(kP4cHero2, guid, "other", true);
    }
    P4cRaidLoading(*run, 1.0, 0);
    P4cFrames(*run->session, 0.5);
    P4cVerifyRaid(*run, true);
    P4cFrames(*run->session, 8.5);
    CHECK(P4cTagCount("p4c_persist", "persist.corridor") == 1);
    constexpr std::uintptr_t kMonster = 0x20000;
    constexpr std::uintptr_t kMonster2 = 0x20100;
    // 开战后 6 秒没有事件（p4c_shield 驱动护盾规则，p4c_persist、p4b_combat 加战斗的载体，都靠 dd.actor 的枚举），之后一次一个角色
    P4cBattleGuard guard = P4cBattleStart({ { kMonster, { 4500, 12 } }, { kMonster2, { 4501, 9 } } });
    P4cFrames(*run->session, 6.0);
    P4cTurn(kP4cHero);
    // 圣骑士的 Smite：自然获得护盾，按游戏的时序 skill.post 之后约 1.5 秒才施加 shield_self_gain_5（修复 F6：shield_lite 原来只认
    // skill.pre 到 skill.post 之间，这里归因是空的）。伤害是怪物一侧的全部吸收（驱动给的 10 点护盾）
    {
        const std::string skill = P4cSkillLate(*run->session, kP4cHero, kMonster, "shield_self_gain_5", 6, 1.5);
        if (skill != "smite") {
            std::printf("    skill.post 之后才施加的 shield_self_gain_5 归给了「%s」，应为 smite\n", skill.c_str());
            CHECK(false);
        }
    }
    P4cFrames(*run->session, 0.1);
    P4cTurn(kMonster);
    P4cSkill(kMonster, kP4cHero, nullptr);
    P4cHit(kP4cHero, kMonster, 4);                       // 驱动里这个怪物对 h 的穿透 50%：英雄一侧的部分吸收
    P4cFrames(*run->session, 1.5);
    P4cTurn(kP4cHero2);
    P4cSkill(kP4cHero2, kMonster, nullptr);
    P4cHit(kMonster, kP4cHero2, 7);                      // 怪物一侧的部分吸收（剩 4，生命 12 → 9）
    P4cFrames(*run->session, 1.5);
    P4cTurn(kMonster2);
    P4cSkill(kMonster2, kP4cHero, nullptr);
    P4cHit(kP4cHero, kMonster2, 3);                      // 英雄一侧的全部吸收
    P4cFrames(*run->session, 1.0);
    P4cRoundStart();                                     // 第 2 回合：再生、衰减、穿透清掉（第 1 回合不结算）
    P4cFrames(*run->session, 1.0);
    P4cTurn(kMonster2);
    P4cSkill(kMonster2, kP4cHero2, nullptr);
    P4cHit(kP4cHero2, kMonster2, 13);                    // 部分吸收（护盾不够）
    P4cFrames(*run->session, 1.0);
    P4cSave(*run->session, true, true);
    P4cGuardCheck(guard, "第一场战斗（读档前）");
    P4cLeave(run);

    // ---- 4. 读战斗中的存档：游戏不派发 battle.start（读档线程把战斗状态写成非零、重建怪物表）；英雄的状态马上核对，怪物的等
    // raid 核对。读档后等 10 秒（p4c_shield 驱动这一场，p4b_combat 做战斗那一轮），之后一次一个角色，打完这场
    run = P4cEnter(DD_PERSIST_LOADED, true);
    g_p4cRound = 2;                                      // 战斗从存档恢复，回合号接着走
    guard = P4cGuardNow();
    P4cRaidLoading(*run, 1.0, 46);
    P4cFrames(*run->session, 0.5);
    P4cVerifyRaid(*run, true);
    P4cFrames(*run->session, 8.5);
    CHECK(P4cTagCount("p4c_persist", "persist.battle") == 1);
    P4cTurn(kMonster);
    P4cSkill(kMonster, kP4cHero, nullptr);
    P4cHit(kP4cHero, kMonster, 3);
    P4cFrames(*run->session, 1.5);
    P4cTurn(kP4cHero);
    P4cSkill(kP4cHero, kMonster, "shield_self_gain_5");
    P4cHit(kMonster, kP4cHero, 20);
    P4cFrames(*run->session, 1.0);
    P4cRoundStart();
    P4cFrames(*run->session, 0.5);
    P4cTurn(kMonster2);
    P4cSkill(kMonster2, kP4cHero2, nullptr);
    P4cHit(kP4cHero2, kMonster2, 30);
    P4cFrames(*run->session, 1.0);
    P4cTurn(kMonster);
    P4cSkill(kMonster, kP4cHero2, nullptr);
    P4cHit(kP4cHero2, kMonster, 40);                     // hero#13 被打到死门（生命 0）
    P4cFrames(*run->session, 1.0);
    P4cTurn(kP4cHero2);
    P4cSkill(kP4cHero2, kMonster, "shield_self_gain_5"); // 死门上获得护盾
    P4cFrames(*run->session, 0.2);                       // shield_lite：生命 ≤ 0 时护盾清零
    P4cHit(kP4cHero2, kMonster, 5);                      // 死门上没有护盾：p4c_shield 写一半的吸收量（不补生命）
    P4cFrames(*run->session, 1.0);
    P4cHit(kP4cHero2, kMonster, 5);                      // 第二次写全部（按格挡处理）
    P4cFrames(*run->session, 1.5);
    P4cMonsterDeath(4500);                               // 打完：怪物都死了
    P4cMonsterDeath(4501);
    const std::size_t corridorBefore = P4cRounds("corridor");
    P4cBattleEnd(guard, "读进来的战斗");                  // 游戏删掉 remove_on_battle_complete 的载体，清空怪物表
    P4cFrames(*run->session, 2.5);                       // 每秒的清理：怪物身上的载体 gone
    CHECK(P4cTagCount("p4c_persist", "bend.remove") == 1 && P4cTagCount("p4c_persist", "monster.gone") >= 1);
    {
        // 第 7 步：打完后在走廊里停 5 秒，这次进存档的走廊那一轮在走廊里做（不是在刚才读进来的战斗里，审核 R3-B3）
        P4cFrames(*run->session, 2.5);
        if (P4cRounds("corridor") != corridorBefore + 1) {
            std::printf("    读进来的战斗打完后，p4b_combat 没有在走廊里做走廊那一轮\n");
            CHECK(false);
        }
    }
    P4cFire("raid.step");
    P4cSave(*run->session, true, false);
    P4cFrames(*run->session, 1.0);
    // 第二场战斗（撤退）：开战后不打，英雄先手、轮到英雄时直接撤退：这场一个怪物都没出过手（审核 R3-B1 的第 3 点）。
    // p4c_persist 加这一场的载体、p4c_shield 驱动（这次进存档的第 2 场）都靠枚举
    constexpr std::uintptr_t kMonster3 = 0x20200;
    guard = P4cBattleStart({ { kMonster3, { 4502, 15 } } });
    P4cFrames(*run->session, 5.0);
    P4cTurn(kP4cHero);
    P4cFrames(*run->session, 1.0);
    P4cBattleEnd(guard, "撤退的战斗");                    // 撤退：没有怪物死亡
    P4cFrames(*run->session, 2.5);
    CHECK(P4cTagCount("p4c_persist", "bend.retreat") == 1);
    // 回城（as_raid_finish 里依次 RaidFinish、TownVisitStart、存档）：游戏删掉队伍英雄身上来源 skill 的 buff（town_visit），存一次城镇；
    // 之后是结算后的过渡和 load_town 线程（as_town_visit_start，加载中：读接口返回「加载中」、写操作被拒绝，审核 R2A-S1），
    // 场景 Mod 在这两段里不做城镇那一轮，到 as_town_visit_update 才做
    g_fc.raid = false;
    g_world.party.clear();
    P4cGameRemoveWhere(false);
    P4cFire("raid.finish");
    g_hub->EndScope(DD_EVENT_SCOPE_RAID);
    P4cFrames(*run->session, 0.5);
    P4cSave(*run->session, false, false);
    P4cState("as_raid_result_to_town_transition");
    P4cFrames(*run->session, 1.0);
    P4cState("as_town_visit_start");
    g_fc.loading = true;
    P4cFrames(*run->session, 4.0);                       // 比两个场景 Mod 的等待（2、3 秒）长：不看加载中就会在这里加载体、做一轮
    g_fc.loading = false;
    CHECK(CountLog("加载线程正在运行") == 0);
    P4cState("as_town_visit_update");
    P4cFrames(*run->session, 5.0);                       // p4b_combat 的城镇一轮（本次进存档第一次在城镇）
    CHECK(P4cTagCount("p4c_persist", "town_visit") >= 1);
    P4cLeave(run);

    // ---- 5. 回城后再读城镇存档：回城时城镇的载体被清掉了，p4c_persist 重新加两条；招募、解雇一次（存档），改旁路时名册里有带状态的标记
    run = P4cEnter(DD_PERSIST_LOADED, false);
    P4cFrames(*run->session, 4.0);
    CHECK(P4cTagCount("p4c_persist", "persist.town") == 2);
    P4cFire("hero.dismiss");                             // 解雇：游戏写名册
    P4cSave(*run->session, false, false);
    P4cFrames(*run->session, 0.5);
    P4cLeave(run);

    // ---- 6. rig.py tamper-persist p4c_persist 改过旁路之后读档：两代都不一致
    {
        FILE* tamper = nullptr;
        _wfopen_s(&tamper, ddlua::Utf8ToWide(outDir + "/rig_tamper.log").c_str(), L"wb");
        CHECK(tamper != nullptr);
        if (tamper) {
            std::fprintf(tamper, "{\"time\": 0, \"run\": \"%s\", \"game_running\": true, \"key\": \"p4c_persist\", \"what\": \"roster\", "
                                 "\"current_only\": false, \"device\": \"steam_profile_7\", "
                                 "\"changed\": [\"p4c_persist.ddsave %s->%s\", \"p4c_persist.ddsave.prev %s->%s\"]}\n",
                kP4cRunId, kP4cRoster, kP4cTampered, kP4cRoster, kP4cTampered);
            std::fclose(tamper);
        }
    }
    run = P4cEnter(DD_PERSIST_MISMATCH, false, std::string("当前一代：名册的哈希不同（旁路 ") + kP4cTampered + "/4096 字节，存档 1111111111111111/4096 字节）；上一代：名册的哈希不同（旁路 " +
        kP4cTampered + "/4096 字节，存档 1111111111111111/4096 字节）");
    P4cFrames(*run->session, 0.1);
    P4cSave(*run->session, false, false);                // 读城镇存档后游戏马上写 persist.narration.json（审核 R2B 备注 1）
    P4cFrames(*run->session, 4.5);                       // 名册扫完（约 2 秒），p4b_combat 的城镇一轮做完再退出（中途退出会留下测试 buff）
    CHECK(P4cTagCount("p4c_persist", "persist.mismatch") == 1);
    P4cLeave(run);

    // ---- 6b. 可选部分 D（第 12 步之后）：再读一次，读档后的那次存档让旁路有两代；只改当前一代，再读档是 prev（上一代是更早的那次存档，
    // persist.store 按 last 找本次运行里那次存档核对，审核 R2B-1）
    run = P4cEnter(DD_PERSIST_LOADED, false);
    P4cFrames(*run->session, 0.1);
    P4cSave(*run->session, false, false);
    P4cFrames(*run->session, 4.5);
    P4cLeave(run);
    {
        FILE* tamper = nullptr;
        _wfopen_s(&tamper, ddlua::Utf8ToWide(outDir + "/rig_tamper.log").c_str(), L"ab");
        CHECK(tamper != nullptr);
        if (tamper) {
            std::fprintf(tamper, "{\"time\": 0, \"run\": \"%s\", \"game_running\": true, \"key\": \"p4c_persist\", \"what\": \"roster\", "
                                 "\"current_only\": true, \"device\": \"steam_profile_7\", \"changed\": [\"p4c_persist.ddsave %s->%s\"]}\n",
                kP4cRunId, kP4cRoster, kP4cTampered);
            std::fclose(tamper);
        }
    }
    run = P4cEnter(DD_PERSIST_PREV, false, std::string("当前一代不一致（名册的哈希不同（旁路 ") + kP4cTampered + "/4096 字节，存档 " + kP4cRoster +
        "/4096 字节）），用上一代");
    P4cFrames(*run->session, 0.1);
    P4cSave(*run->session, false, false);
    P4cFrames(*run->session, 4.0);
    CHECK(LogHas("[p4c_persist] 信息 [store] status=prev"));
    P4cLeave(run);

    // ---- 7. 一个没有 Lua Mod 的存档（可选的 #6）：不创建 VM，hook 报告和主菜单相同
    {
        const std::string serial = std::to_string(++g_p4cSerial);
        P4cCore("阶段 CampaignLoad，帧 9000，存档序号 " + serial + "，线程 1，Campaign=0000000000070000，Buff::Library=0000000000071000，状态 last=2 next=19");
        P4cCore("启用 Mod 列表 0 项（已安装 300 个），找到目录 0 项");
        WriteLogLine(luaLog, DD_LOG_INFO, "[存档#" + serial + "] 进入存档：启用 0 个 Mod，0 个没有找到目录（见 DDCore.log）");
        WriteLogLine(luaLog, DD_LOG_INFO, "[存档#" + serial + "] 没有可加载的 Lua Mod，不创建 VM");
        P4cHookReport("CampaignLoad", 0, 0);
        g_p4cT += 5;
        P4cCore("阶段 CampaignUnload，帧 9500，存档序号 " + serial + "，线程 1");
    }
    P4cCore("persist：退到桌面：后台写盘已写完（本次运行写盘 " + std::to_string(g_p4cSaveIndex * 4) + " 个，合并 0 次，失败 0 次，重试 0 次）");

    g_logSink = nullptr;
    std::fclose(luaLog);
    std::fclose(g_p4cCore);
    g_p4cCore = nullptr;

    // 判定（tools/p4_scenario_check.py 对同一份日志逐条核对公式）
    std::size_t passed = 0, failed = 0, shields = 0;
    for (const auto& line : g_log) {
        const bool scenario = line.rfind("[p4c_", 0) == 0 || line.rfind("[p4b_combat]", 0) == 0 || line.rfind("[shield_lite]", 0) == 0;
        passed += scenario && line.find("] 通过 ") != std::string::npos ? 1 : 0;
        shields += line.rfind("[shield_lite] SHIELD ", 0) == 0 ? 1 : 0;
        if (scenario && (line.find("] 失败") != std::string::npos || line.find("出错") != std::string::npos)) {
            ++failed;
            std::printf("    %s\n", line.c_str());
        }
    }
    std::printf("  P4c2 场景预演：通过 %zu 行，失败 %zu 行，SHIELD %zu 行；日志样本在 %s\n", passed, failed, shields, outDir.c_str());
    CHECK(failed == 0 && passed >= 60);
    for (const char* ev : { "\"ev\":\"init\"", "\"ev\":\"apply\"", "\"ev\":\"absorb\"", "\"ev\":\"hit\"", "\"ev\":\"round\"", "\"ev\":\"clear\"", "\"ev\":\"carrier\"",
             "\"ev\":\"save\"", "\"ev\":\"restore\"", "\"ev\":\"rule\"", "\"kind\":\"full\"", "\"kind\":\"partial\"", "\"skill\":\"smite\"", "\"op\":\"regen\"",
             "\"op\":\"decay\"", "\"op\":\"max\"", "\"op\":\"full\"", "\"op\":\"break\"", "\"op\":\"set\"", "\"op\":\"pierce\"", "\"ev\":\"zero\"",
             "\"skip\":true", "\"result\":\"original\"" }) {
        if (CountLog(ev) == 0) {
            std::printf("    shield_lite 没有写出 %s\n", ev);
            CHECK(false);
        }
    }
    for (const char* tag : { "drive.1", "define", "rule.live" }) {
        if (P4cTagCount("p4c_shield", tag) == 0) {
            std::printf("    p4c_shield 没有「通过 [%s]」\n", tag);
            CHECK(false);
        }
    }
    CHECK(CountLog("[p4c_shield] 信息 [deathsdoor] kind=partial") == 1 && CountLog("[p4c_shield] 信息 [deathsdoor] kind=full") == 1);
    for (const char* tag : { "rounddone.town", "rounddone.corridor", "rounddone.battle", "effect.town", "effect.corridor", "effect.battle",
             "queue.done.town", "queue.done.corridor", "queue.done.battle", "rule.live.town", "rule.live.corridor", "rule.live.battle",
             "monster.battle", "monster.remove.battle", "define.game" }) {
        if (P4cTagCount("p4b_combat", tag) == 0) {
            std::printf("    p4b_combat 没有「通过 [%s]」\n", tag);
            CHECK(false);
        }
    }
    for (const char* tag : { "bend.remove", "bend.retreat", "restore.noadd", "remove.match", "town_visit", "monster.gone", "persist.mismatch" }) {
        if (P4cTagCount("p4c_persist", tag) == 0) {
            std::printf("    p4c_persist 没有「通过 [%s]」\n", tag);
            CHECK(false);
        }
    }
    CHECK(TickSubs() == 0 && CoreCount() == 0);
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
        { "F5 队伍、怪物、战斗状态的只读枚举（dd.actor.party / monsters / in_battle）", TestActorEnum },
        { "P4b 读：枚举、定义、快照、effect 句柄", TestCombatReads },
        { "P4b 写：定义、添加、删除、标签、拒绝、排队和 done", TestCombatWrites },
        { "P4b effect：施加、文本定义、脚本 Effect、技能句柄", TestCombatEffects },
        { "P4b 脚本 Buff：状态的绑定、回调、丢弃、导出导入", TestScriptBuff },
        { "P4b 规则：只读、预算、停用、撤销", TestRules },
        { "P4b dd.fs 的写入", TestDataFs },
        { "P4b 嵌套入口：预算、补时间、粘滞超时、崩溃隔离、跨 VM", TestNestedEntries },
        { "P4b 状态的 JSON 编解码", TestStateJson },
        { "P4b 场景 Mod p4b_combat 的离线预演", TestP4bScenarioMod },
        { "P4c1 旁路数据的格式", TestStoreFormat },
        { "P4c1 dd.store 和脚本 Buff 状态的存档、读档、核对", TestStoreRoundTrip },
        { "P4c1 dd.store_status 的各种结果", TestStoreStatus },
        { "P4c1 上限、整体赋值、不能保存的数据", TestStoreLimits },
        { "P4c1 经过 plugin.cpp 的声明和回调", TestStorePlugin },
        { "P4c1 规则回调交还原函数", TestRuleOriginal },
        { "P4c2 场景 Mod（shield_lite、p4c_shield、p4c_persist、p4b_combat）的离线预演", TestP4cScenarioRehearsal },
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
