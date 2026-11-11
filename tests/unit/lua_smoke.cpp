// Lua 本地包和 sol2 的冒烟测试：确认 DDLua 用的编译选项（dd.native：C++23、/MT、/permissive-）下
// 版本正确、绑定可用、错误能被捕获、计数 hook 能打断死循环。不依赖游戏。
// 按 C++ 编译的保证（审核 A-C3）：lua_error 是 C++ 异常，穿过 C 函数的栈帧时局部对象照常析构，
// 计数 hook 里抛出的错误也一样；DD 补丁的进度回调（lddstep.h）在库函数的长循环里被调用。
#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include <lddstep.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

int g_failed = 0;
int g_checks = 0;

void Check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("失败：%s\n", what);
    }
}

void BudgetHook(lua_State* L, lua_Debug*)
{
    luaL_error(L, "budget exceeded");
}

// 析构时记一笔，用来确认 lua_error 穿过 C++ 栈帧时析构函数运行了
struct DtorProbe {
    int* count;
    ~DtorProbe() { ++*count; }
};

int g_dtors = 0;

// C 函数里放一个局部对象，然后 luaL_error
int RaiseThroughRaii(lua_State* L)
{
    DtorProbe probe{ &g_dtors };
    std::string text(64, 'x');  // 带堆内存的局部对象（不析构就泄漏）
    luaL_error(L, "raised through a C++ frame (%d)", static_cast<int>(text.size()));
    return 0;
}

// C 函数里放一个局部对象，再调用栈上的 Lua 函数；Lua 函数死循环，计数 hook 抛错穿过这个栈帧
int CallLuaThroughRaii(lua_State* L)
{
    DtorProbe probe{ &g_dtors };
    lua_pushvalue(L, 1);
    lua_call(L, 0, 0);
    return 0;
}

int g_steps = 0;

void StepRaises(lua_State* L)
{
    if (++g_steps >= 4) {
        luaL_error(L, "ddstep raised");
    }
}

}  // namespace

int main()
{
    Check(LUA_VERSION_RELEASE_NUM == 50409, "LUA_VERSION_RELEASE_NUM 是 50409");
    Check(std::string(LUA_RELEASE) == "Lua 5.4.9", "LUA_RELEASE 是 Lua 5.4.9");

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math, sol::lib::coroutine,
        sol::lib::utf8);

    // 绑定 C++ 函数，脚本调用并回传
    lua.set_function("add", [](int a, int b) { return a + b; });
    auto sum = lua.safe_script("return add(40, 2)", sol::script_pass_on_error);
    Check(sum.valid() && sum.get<int>() == 42, "绑定函数返回 42");

    // 脚本错误被 protected_function 捕获，消息带出来
    auto bad = lua.safe_script("error('boom')", sol::script_pass_on_error);
    Check(!bad.valid(), "error() 被捕获");
    if (!bad.valid()) {
        const sol::error err = bad;
        Check(std::string(err.what()).find("boom") != std::string::npos, "错误消息含 boom");
    }

    // C++ 异常转成 Lua 错误，不越过 Lua 栈帧
    lua.set_function("throws", []() -> int { throw std::runtime_error("cpp failure"); });
    auto thrown = lua.safe_script("return throws()", sol::script_pass_on_error);
    Check(!thrown.valid(), "C++ 异常转成 Lua 错误");

    // 协程能挂起和恢复
    lua.safe_script("co = coroutine.create(function(x) local y = coroutine.yield(x + 1); return y * 2 end)");
    auto step1 = lua.safe_script("local ok, v = coroutine.resume(co, 1); return v", sol::script_pass_on_error);
    auto step2 = lua.safe_script("local ok, v = coroutine.resume(co, 5); return v", sol::script_pass_on_error);
    Check(step1.valid() && step1.get<int>() == 2 && step2.valid() && step2.get<int>() == 10, "协程挂起和恢复");

    // 计数 hook 打断死循环（指令预算的基础）
    lua_sethook(lua.lua_state(), BudgetHook, LUA_MASKCOUNT, 100000);
    auto loop = lua.safe_script("while true do end", sol::script_pass_on_error);
    lua_sethook(lua.lua_state(), nullptr, 0, 0);
    Check(!loop.valid(), "死循环被计数 hook 打断");
    if (!loop.valid()) {
        const sol::error err = loop;
        Check(std::string(err.what()).find("budget exceeded") != std::string::npos, "打断的消息是 budget exceeded");
    }

    // 打断之后 VM 仍可用
    auto after = lua.safe_script("return 1 + 1", sol::script_pass_on_error);
    Check(after.valid() && after.get<int>() == 2, "打断后 VM 仍可用");

    // 按 C++ 编译：luaL_error 穿过 C 函数的栈帧，局部对象析构（按 C 编译时 longjmp 会跳过析构）
    lua_State* L = lua.lua_state();
    g_dtors = 0;
    lua_pushcfunction(L, RaiseThroughRaii);
    int status = lua_pcall(L, 0, 0, 0);
    Check(status == LUA_ERRRUN && std::string(lua_tostring(L, -1)).find("raised through a C++ frame (64)") != std::string::npos,
        "C 函数里的 luaL_error 被 lua_pcall 接住，状态 LUA_ERRRUN");
    lua_pop(L, 1);
    Check(g_dtors == 1, "luaL_error 穿过 C 函数时局部对象析构了");

    // 计数 hook 里抛出的错误穿过调用 Lua 的 C 函数，局部对象同样析构
    g_dtors = 0;
    lua_sethook(L, BudgetHook, LUA_MASKCOUNT, 1000);
    lua_pushcfunction(L, CallLuaThroughRaii);
    luaL_loadstring(L, "while true do end");
    status = lua_pcall(L, 1, 0, 0);
    lua_sethook(L, nullptr, 0, 0);
    Check(status == LUA_ERRRUN && std::string(lua_tostring(L, -1)).find("budget exceeded") != std::string::npos,
        "计数 hook 里的错误穿过 C 函数被接住");
    lua_pop(L, 1);
    Check(g_dtors == 1, "计数 hook 的错误穿过 C 函数时局部对象析构了");

    // DD 补丁：模式匹配的长循环调用进度回调，回调可以报错打断它（DDLua 的预算检查就挂在这里）
    lua.safe_script("S = string.rep('a', 100000) P = S .. 'b'");
    g_steps = 0;
    lua_ddstep_fn = &StepRaises;
    auto stepped = lua.safe_script("return string.match(S, P)", sol::script_pass_on_error);
    lua_ddstep_fn = nullptr;
    Check(!stepped.valid() && g_steps >= 4, "模式匹配的长循环调用 lddstep 的进度回调，回调里的错误打断它");
    auto alive = lua.safe_script("return ('x'):rep(3)", sol::script_pass_on_error);
    Check(alive.valid() && alive.get<std::string>() == "xxx", "进度回调报错后 VM 仍可用");

    std::printf("%s，%d 项检查，失败 %d\n", LUA_RELEASE, g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
