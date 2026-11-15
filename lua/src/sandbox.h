#pragma once
// 沙箱：VM 的全局准备和每个 Mod 的 _ENV。清单见 docs/03-lua-system.md 的「沙箱和稳定性」。
//
// - 打开 base、coroutine、table、string、math、utf8、os、debug，不开 io、package。真实的全局表、注册表
//   对 Mod 不可达；真实库表里的危险函数也删掉（纵深防御）：dofile、loadfile、load、collectgarbage、
//   string.dump、os 除 time/clock/date 以外的函数、debug 除 traceback 以外的函数。
// - 字符串的元表设 __metatable = false，getmetatable("") 拿不到真实的 string 库。
// - 每个 Mod 一张新的 _ENV，库表是副本（改自己的 string/table 不影响别人），_G 指向自己。
// - load 强制文本模式，默认环境是调用者的 _ENV；setmetatable 拒绝带 __gc 的元表；collectgarbage 只留
//   "count"、"isrunning"；pcall/xpcall/load/coroutine.resume/coroutine.close 捕获错误后若已超预算就重新抛出。
// - require 只在 <mod>/lua/ 下找 <名字>.lua 或 <名字>/init.lua，每个 Mod 自己的缓存。
// - print、warn 写日志（带 Mod id），dd.fs 只读且限制在 Mod 目录内。

#include <lua.h>

#include <cstdint>
#include <string>

namespace ddlua {

class Session;
struct Mod;
class Vm;

// ---- C 函数共用的工具（events.cpp、scheduler.cpp 也用）----

// VM 所属的会话
Session& SessionOf(lua_State* L);
// 闭包的第 1 个上值是 Mod 的 token。Mod 已经拆除时报错（别的 Mod 可能还拿着它的函数）
Mod& CheckMod(lua_State* L);
// 同上，Mod 正在拆除（清理项、协程的 <close> 运行期间）时也报错：注册订阅、定时器、协程的函数用它。
// P4b：规则回调（dd.buff.rule）里也报错（规则回调只读）
Mod& CheckLiveMod(lua_State* L, const char* what);

// P4b：调用可能同步回到 Lua 的 SDK 函数（立即执行的写操作、读 actor 字段）之后调用。嵌套的回调里发生了原生异常时，
// DDCore 的隔离只把栈展开到派发处，当前这个 C 函数所在的 lua_State 已经乱了（调用信息、errorJmp 停在异常发生时），
// 不能再回到 Lua：抛一个 SEH 异常（kReentryAbandoned），把外层入口也交给它自己的隔离（DDCore 的事件隔离、DDLua 的 RunMarked）
constexpr unsigned long kReentryAbandoned = 0xE0444C52;
void AfterReentry(lua_State* L);
// 已超预算时重新抛出预算错误（捕获 Lua 错误的 C 函数用）
void RethrowIfBudget(lua_State* L);
// 撤销订阅、定时器、协程（:off、dd.off、:cancel）只能由所属 Mod 自己的代码进行：现在代表执行的 Mod
// （Session::CurrentMod）不是 owner 时报错（审核 B-N4）
void CheckOwner(lua_State* L, const Mod* owner, const std::string& ownerId, const char* what);
// table[name] = 带 token 上值的 C 闭包
void SetClosure(lua_State* L, int table, const char* name, lua_CFunction fn, std::uint64_t token);

// 打开标准库并清理，在 VM 创建后调用一次（保护模式内）
void PrepareGlobals(lua_State* L);

// 为 mod 构建 _ENV 并压栈（保护模式内调用，可能抛 Lua 错误）
void PushModEnv(lua_State* L, Session& session, Mod& mod);

// 加载 Mod 目录里的一个 Lua 文件（相对 <mod>/lua），环境设为 mod 的 _ENV，函数压栈。失败时抛 Lua 错误。
// 读取和语法分析都要在预算入口里（Vm::Call 调用的 C 函数里），否则不受预算约束
void LoadModFile(lua_State* L, Mod& mod, const std::string& rel);

// C 闭包（第 1 个上值是 Mod 的 token）：读取、语法分析并执行 main.lua，返回它的返回值（1 个）。
// Session::LoadMod 用 Vm::Call 调用它，三步共用 load_budget_ms 的预算入口（第二轮审核 R2A-阻断3）
int RunMainChunk(lua_State* L);

}  // namespace ddlua
