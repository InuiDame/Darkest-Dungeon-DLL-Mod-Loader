#include "sandbox.h"

#include "actor.h"
#include "events.h"
#include "manifest.h"
#include "scheduler.h"
#include "session.h"
#include "vm.h"

#include <lauxlib.h>
#include <lddstep.h>
#include <lualib.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace ddlua {

// ---- Mod 归属（events.cpp、scheduler.cpp 共用）----

Session& SessionOf(lua_State* L)
{
    return *static_cast<Session*>(Vm::From(L)->owner());
}

Mod& CheckMod(lua_State* L)
{
    const auto token = static_cast<std::uint64_t>(lua_tointeger(L, lua_upvalueindex(1)));
    Mod* mod = SessionOf(L).FindToken(token);
    if (!mod) {
        luaL_error(L, "这个函数所属的 Mod 已经卸载");
    }
    return *mod;
}

Mod& CheckLiveMod(lua_State* L, const char* what)
{
    Mod& mod = CheckMod(L);
    if (mod.tearingDown) {
        luaL_error(L, "%s：Mod %s 正在拆除，不能再注册", what, mod.id().c_str());
    }
    return mod;
}

void RethrowIfBudget(lua_State* L)
{
    Vm* vm = Vm::From(L);
    if (vm->BudgetExceeded()) {
        vm->RaiseBudget(L);
    }
}

void CheckOwner(lua_State* L, const Mod* owner, const std::string& ownerId, const char* what)
{
    const Mod* current = SessionOf(L).CurrentMod();
    if (current && owner && current != owner) {
        luaL_error(L, "%s：它属于 Mod %s，只能由 %s 自己的代码撤销（现在执行的是 %s 的代码）", what, ownerId.c_str(), ownerId.c_str(),
            current->id().c_str());
    }
}

void SetClosure(lua_State* L, int table, const char* name, lua_CFunction fn, std::uint64_t token)
{
    lua_pushinteger(L, static_cast<lua_Integer>(token));
    lua_pushcclosure(L, fn, 1);
    lua_setfield(L, table, name);
}

namespace {

constexpr std::size_t kMaxLogLine = 4096;  // print / dd.log 一次最多写这么多字节
constexpr lua_Integer kMaxOsTime = 32535215999;  // 3000-12-31，MSVC 的 localtime/gmtime 的上限
char kRequireLoading = 0;  // require 缓存里「正在加载」的标记（取地址用）

// ---- 捕获错误的函数：已超预算时重新抛出 ----

int FinishPcall(lua_State* L, int status, lua_KContext extra)
{
    if (status != LUA_OK && status != LUA_YIELD) {
        RethrowIfBudget(L);
        lua_pushboolean(L, 0);
        lua_pushvalue(L, -2);
        return 2;
    }
    return lua_gettop(L) - static_cast<int>(extra);
}

int SafePcall(lua_State* L)
{
    luaL_checkany(L, 1);
    lua_pushboolean(L, 1);
    lua_insert(L, 1);
    const int status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, FinishPcall);
    return FinishPcall(L, status, 0);
}

// xpcall 的消息处理函数外面包一层：已超预算时不调用 Mod 的处理函数，原样返回错误对象。
// 计数 hook 里抛出的错误会在 hook 返回之前（allowhook 为 0）调用消息处理函数，这时运行的 Lua 代码
// 不再触发 hook，处理函数里的死循环就打断不了
int GuardedMessageHandler(lua_State* L)
{
    if (Vm::From(L)->BudgetExceeded()) {
        lua_settop(L, 1);
        return 1;
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}

int SafeXpcall(lua_State* L)
{
    const int n = lua_gettop(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, GuardedMessageHandler, 1);
    lua_replace(L, 2);
    lua_pushboolean(L, 1);
    lua_pushvalue(L, 1);
    lua_rotate(L, 3, 2);
    const int status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, FinishPcall);
    return FinishPcall(L, status, 2);
}

// 调用第 1 个上值里的原函数（不可让出），返回后检查预算
int CallOriginalThenCheck(lua_State* L)
{
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    RethrowIfBudget(L);
    return lua_gettop(L);
}

// ---- load：只收文本，默认环境是调用者的 _ENV（第 1 个上值）----

constexpr int kReaderSlot = 5;

// 词法分析在 C 里运行：长注释、空白的内容不保存，不分配内存，也不经过计数 hook。源码按块交给 lua_load，
// 每块最多 kLoadSlice 字节，交出下一块之前检查预算，两次检查之间最多分析一块（审核 A-B1：Mod 的 reader 每次返回
// 同一个几十 MB 的串，原来每 160 次调用才检查一次，一次 load 卡几秒）。
// 第二轮 R2A-备注4：语法分析不是线性的，深层嵌套加大量局部变量时每个全局名都要穿过所有层级查找，审核者测到 64 KB 要
// 249 ms；块改成 4 KB，一块最多十几毫秒。切块不复制源码，只是多几次 reader 调用
constexpr std::size_t kLoadSlice = 4 * 1024;

struct SliceSource {
    const char* data = nullptr;  // 当前这块源码（锚在栈上：字符串参数，或 Mod 的 reader 返回值所在的 kReaderSlot）
    std::size_t size = 0;
    std::size_t offset = 0;
    bool reader = false;  // 源码来自 Mod 的 reader 函数（栈上第 1 个参数），当前块用完时再调用它
};

const char* SlicedReader(lua_State* L, void* ud, std::size_t* size)
{
    auto& src = *static_cast<SliceSource*>(ud);
    Vm::From(L)->CheckBudget(L);
    if (src.offset >= src.size) {
        if (!src.reader) {
            *size = 0;
            return nullptr;
        }
        luaL_checkstack(L, 2, "too many nested functions");
        lua_pushvalue(L, 1);
        lua_call(L, 0, 1);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            *size = 0;
            return nullptr;
        }
        if (!lua_isstring(L, -1)) {
            luaL_error(L, "reader function must return a string");
        }
        lua_replace(L, kReaderSlot);
        src.data = lua_tolstring(L, kReaderSlot, &src.size);
        src.offset = 0;
        if (src.size == 0) {
            *size = 0;  // 空串表示结束（和 Lua 的 load 一样）
            return nullptr;
        }
        Vm::From(L)->CheckBudget(L);
    }
    const std::size_t n = (std::min)(src.size - src.offset, kLoadSlice);
    const char* piece = src.data + src.offset;
    src.offset += n;
    *size = n;
    return piece;
}

int SafeLoad(lua_State* L)
{
    const bool hasEnv = lua_gettop(L) >= 4;
    std::size_t length = 0;
    const char* source = lua_tolstring(L, 1, &length);
    int status;
    SliceSource src;
    if (source) {
        const char* chunkName = luaL_optstring(L, 2, source);
        src.data = source;
        src.size = length;
        status = lua_load(L, SlicedReader, &src, chunkName, "t");
    } else {
        const char* chunkName = luaL_optstring(L, 2, "=(load)");
        luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_settop(L, kReaderSlot);
        src.reader = true;
        status = lua_load(L, SlicedReader, &src, chunkName, "t");
    }
    if (status != LUA_OK) {
        RethrowIfBudget(L);  // reader 函数里的超预算错误被 lua_load 捕获了，这里重新抛出
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }
    if (hasEnv) {
        lua_pushvalue(L, 4);
    } else {
        lua_pushvalue(L, lua_upvalueindex(1));
    }
    if (!lua_setupvalue(L, -2, 1)) {
        lua_pop(L, 1);
    }
    return 1;
}

// ---- setmetatable：拒绝 __gc ----

int SafeSetmetatable(lua_State* L)
{
    const int type = lua_type(L, 2);
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_argexpected(L, type == LUA_TNIL || type == LUA_TTABLE, 2, "nil or table");
    if (type == LUA_TTABLE) {
        // Lua 登记终结器时按原始访问检查 __gc（gfasttm），这里也用原始访问
        lua_pushliteral(L, "__gc");
        if (lua_rawget(L, 2) != LUA_TNIL) {
            return luaL_error(L, "沙箱不允许带 __gc 的元表");
        }
        lua_pop(L, 1);
    }
    if (luaL_getmetafield(L, 1, "__metatable") != LUA_TNIL) {
        return luaL_error(L, "cannot change a protected metatable");
    }
    lua_settop(L, 2);
    lua_setmetatable(L, 1);
    return 1;
}

// ---- collectgarbage：只留查询 ----

int SafeCollectgarbage(lua_State* L)
{
    const char* option = luaL_optstring(L, 1, "collect");
    if (std::strcmp(option, "count") == 0) {
        const int kb = lua_gc(L, LUA_GCCOUNT);
        const int bytes = lua_gc(L, LUA_GCCOUNTB);
        lua_pushnumber(L, static_cast<lua_Number>(kb) + static_cast<lua_Number>(bytes) / 1024);
        return 1;
    }
    if (std::strcmp(option, "isrunning") == 0) {
        lua_pushboolean(L, lua_gc(L, LUA_GCISRUNNING));
        return 1;
    }
    return luaL_error(L, "沙箱里的 collectgarbage 只支持 \"count\" 和 \"isrunning\"，不支持 \"%s\"", option);
}

// ---- os.date：限制时间范围（MSVC 的 localtime 对负数会调用无效参数处理器）----

int SafeOsDate(lua_State* L)
{
    if (!lua_isnoneornil(L, 2)) {
        int isNumber = 0;
        const lua_Integer t = lua_tointegerx(L, 2, &isNumber);
        if (!isNumber || t < 0 || t > kMaxOsTime) {
            return luaL_error(L, "os.date 的时间必须是 0 到 %I 之间的整数", static_cast<LUAI_UACINT>(kMaxOsTime));
        }
    }
    return CallOriginalThenCheck(L);
}

// ---- 日志 ----

// 追加到 kMaxLogLine 为止（审核 A-C4：原来先完整拼接再截断，一个 100 MB 的串会在 CRT 堆上复制一份，不受内存上限约束）。
// 截断点退到 UTF-8 字符的边界
void AppendCapped(std::string& line, const char* text, std::size_t length, bool& truncated)
{
    const std::size_t room = line.size() < kMaxLogLine ? kMaxLogLine - line.size() : 0;
    if (length > room) {
        length = room;
        while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) {
            --length;
        }
        truncated = true;
    }
    line.append(text, length);
}

std::string JoinArgs(lua_State* L, int first, const char* separator)
{
    std::string line;
    bool truncated = false;
    const int n = lua_gettop(L);
    for (int i = first; i <= n; ++i) {
        if (line.size() >= kMaxLogLine) {
            truncated = true;  // 后面的参数不再转换
            break;
        }
        std::size_t length = 0;
        const char* text = luaL_tolstring(L, i, &length);  // 和 print 一样会调用 __tostring（受预算约束）
        if (i > first) {
            AppendCapped(line, separator, std::strlen(separator), truncated);
        }
        AppendCapped(line, text, length, truncated);
        lua_pop(L, 1);
    }
    if (truncated) {
        line += "…（截断）";
    }
    return line;
}

template <int Level>
int LogFn(lua_State* L)
{
    Mod& mod = CheckMod(L);
    const std::string line = JoinArgs(L, 1, "\t");
    SessionOf(L).ModLog(Level, mod, line);
    return 0;
}

int Warn(lua_State* L)
{
    Mod& mod = CheckMod(L);
    const int n = lua_gettop(L);
    luaL_checkstring(L, 1);
    for (int i = 2; i <= n; ++i) {
        luai_ddstep(L, 1);  // 第三轮 R3A-应修2 的排查：40 万个参数时一次调用几毫秒，反复调用要计步
        luaL_checkstring(L, i);
    }
    std::string message;
    bool truncated = false;
    for (int i = 1; i <= n && !truncated; ++i) {
        std::size_t length = 0;
        const char* text = lua_tolstring(L, i, &length);
        AppendCapped(message, text, length, truncated);
    }
    if (!message.empty() && message[0] == '@') {
        return 0;  // 控制消息（@on/@off）没有意义
    }
    if (truncated) {
        message += "…（截断）";
    }
    SessionOf(L).ModLog(DD_LOG_WARN, mod, message);
    return 0;
}

// ---- require ----

// 模块名：用点分隔的段，每段是字母、数字、_、-。转成相对路径 a/b
bool ModulePath(std::string_view name, std::string& out)
{
    if (name.empty() || name.size() > 200) {
        return false;
    }
    out.clear();
    std::size_t segment = 0;
    for (const char ch : name) {
        if (ch == '.') {
            if (segment == 0) {
                return false;
            }
            out += '/';
            segment = 0;
            continue;
        }
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!ok) {
            return false;
        }
        out += ch;
        ++segment;
    }
    return segment > 0;
}

int Require(lua_State* L)
{
    Mod& mod = CheckMod(L);
    std::size_t length = 0;
    const char* name = luaL_checklstring(L, 1, &length);
    std::string rel;
    if (!ModulePath(std::string_view(name, length), rel)) {
        return luaL_error(L, "require：模块名不合法：%s（只能是用点分隔的字母、数字、_、-）", name);
    }
    lua_settop(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, mod.requireRef);  // 2：缓存
    lua_pushvalue(L, 1);
    if (lua_rawget(L, 2) != LUA_TNIL) {
        if (lua_touserdata(L, -1) == &kRequireLoading) {
            return luaL_error(L, "require：模块 %s 循环引用（它还在加载中）", name);
        }
        return 1;
    }
    lua_pop(L, 1);

    std::string file = rel + ".lua";
    if (!mod.luaFs.Exists(file)) {
        file = rel + "/init.lua";
        if (!mod.luaFs.Exists(file)) {
            return luaL_error(L, "require：找不到模块 %s（lua/%s.lua 或 lua/%s/init.lua）", name, rel.c_str(), rel.c_str());
        }
    }
    LoadModFile(L, mod, file);  // 3：模块函数

    lua_pushvalue(L, 1);
    lua_pushlightuserdata(L, &kRequireLoading);
    lua_rawset(L, 2);
    lua_pushvalue(L, 1);
    lua_pushstring(L, file.c_str());
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {  // 3：结果或错误
        lua_pushvalue(L, 1);
        lua_pushnil(L);
        lua_rawset(L, 2);
        return lua_error(L);  // 原样抛出（超预算时也一样传上去）
    }
    if (lua_isnil(L, 3)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 3);
    lua_rawset(L, 2);
    return 1;
}

// ---- dd.fs ----

int FsRead(lua_State* L)
{
    Mod& mod = CheckMod(L);
    std::size_t length = 0;
    const char* path = luaL_checklstring(L, 1, &length);
    std::string error;
    if (!mod.fs.PushFile(L, std::string_view(path, length), &error)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s：%s", path, error.c_str());
        return 2;
    }
    return 1;
}

int FsExists(lua_State* L)
{
    Mod& mod = CheckMod(L);
    std::size_t length = 0;
    const char* path = luaL_checklstring(L, 1, &length);
    lua_pushboolean(L, mod.fs.Exists(std::string_view(path, length)));
    return 1;
}

int FsList(lua_State* L)
{
    Mod& mod = CheckMod(L);
    std::size_t length = 0;
    const char* path = luaL_optlstring(L, 1, "", &length);
    std::vector<std::string> names;
    std::string error;
    if (!mod.fs.List(std::string_view(path, length), names, &error)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s：%s", path, error.c_str());
        return 2;
    }
    lua_createtable(L, static_cast<int>(names.size()), 0);
    for (std::size_t i = 0; i < names.size(); ++i) {
        lua_pushlstring(L, names[i].data(), names[i].size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

// ---- dd.import ----

// 导出是表时，导入方拿到一张只读的浅代理（审核 A-B3：原来所有导入方和提供方共用同一张表，依赖方能替换提供方的
// 导出函数）。读经过 __index，赋值报错，pairs、# 照常；代理本身是空表，每个使用方一份（按提供方缓存），rawset 只改到
// 自己那份。更深层的表和导出函数返回的对象仍然共享（单 VM，ADR 0013）：提供方的类元表要设 __metatable，不要返回
// 导出表本身（第二轮 R2A-备注1，见 docs/03-lua-system.md）
int ProxyNewIndex(lua_State* L)
{
    const char* key = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : luaL_typename(L, 2);
    return luaL_error(L, "dd.import 返回的导出是只读的：不能给 Mod %s 的导出赋值（键 %s）", lua_tostring(L, lua_upvalueindex(1)), key);
}

int ProxyNext(lua_State* L)
{
    lua_settop(L, 2);  // 代理, 键
    if (lua_next(L, lua_upvalueindex(1))) {
        return 2;
    }
    lua_pushnil(L);
    return 1;
}

int ProxyPairs(lua_State* L)
{
    lua_pushvalue(L, lua_upvalueindex(1));  // ProxyNext（导出表在它的上值里，不交给 Mod）
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    return 3;
}

int ProxyLen(lua_State* L)
{
    lua_len(L, lua_upvalueindex(1));
    return 1;
}

void PushReadOnlyProxy(lua_State* L, int exports, const char* id)
{
    exports = lua_absindex(L, exports);
    lua_newtable(L);           // 代理
    lua_createtable(L, 0, 5);  // 元表
    lua_pushvalue(L, exports);
    lua_setfield(L, -2, "__index");
    lua_pushstring(L, id);
    lua_pushcclosure(L, ProxyNewIndex, 1);
    lua_setfield(L, -2, "__newindex");
    lua_pushvalue(L, exports);
    lua_pushcclosure(L, ProxyNext, 1);
    lua_pushcclosure(L, ProxyPairs, 1);
    lua_setfield(L, -2, "__pairs");
    lua_pushvalue(L, exports);
    lua_pushcclosure(L, ProxyLen, 1);
    lua_setfield(L, -2, "__len");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
}

int Import(lua_State* L)
{
    Mod& self = CheckMod(L);
    const char* id = luaL_checkstring(L, 1);
    Mod* target = SessionOf(L).FindMod(id);
    if (!target || target->state != ModState::Loaded || target->exportsRef == LUA_NOREF) {
        lua_pushnil(L);
        lua_pushfstring(L, "Mod %s 没有加载（或还没加载完）", id);
        return 2;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, target->exportsRef);
    if (lua_type(L, -1) != LUA_TTABLE) {
        return 1;  // 不是表的导出（函数、数字……）本来就改不了，原样返回
    }
    // 代理按（使用方、提供方）缓存（第二轮 R2A-备注1）：同一个使用方 dd.import(x) == dd.import(x)，可以当表的键；
    // rawset 只改到这个使用方自己的那份。键是提供方的 token，提供方重载后 token 变了，旧代理不再命中
    if (self.importsRef == LUA_NOREF) {
        lua_newtable(L);
        self.importsRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, self.importsRef);  // 导出, 缓存
    if (lua_rawgeti(L, -1, static_cast<lua_Integer>(target->token)) == LUA_TTABLE) {
        return 1;
    }
    lua_pop(L, 1);
    PushReadOnlyProxy(L, -2, target->id().c_str());  // 导出, 缓存, 代理
    lua_pushvalue(L, -1);
    lua_rawseti(L, -3, static_cast<lua_Integer>(target->token));
    return 1;
}

// ---- dd.reload、dd.stats ----

// dd.reload([id])：开发模式下把重载排到下一帧（不能在 Lua 代码运行时拆除 Mod），默认重载自己
int Reload(lua_State* L)
{
    Mod& mod = CheckMod(L);
    const char* id = luaL_optstring(L, 1, mod.id().c_str());
    std::string error;
    if (!SessionOf(L).RequestReload(id, &error)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// dd.stats()：本 Mod 的订阅、等待者、定时器、协程数
int Stats(lua_State* L)
{
    Mod& mod = CheckMod(L);
    Session& session = SessionOf(L);
    const auto events = session.events().CountFor(&mod);
    const auto sched = session.scheduler().CountFor(&mod);
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, static_cast<lua_Integer>(events.subscriptions));
    lua_setfield(L, -2, "subscriptions");
    lua_pushinteger(L, static_cast<lua_Integer>(events.waiters));
    lua_setfield(L, -2, "waiters");
    lua_pushinteger(L, static_cast<lua_Integer>(sched.timers));
    lua_setfield(L, -2, "timers");
    lua_pushinteger(L, static_cast<lua_Integer>(sched.tasks));
    lua_setfield(L, -2, "tasks");
    lua_pushinteger(L, static_cast<lua_Integer>(mod.loads));
    lua_setfield(L, -2, "loads");
    return 1;
}

// ---- 构建 ----

// math.random / math.randomseed 换成每个 Mod 自己的随机数状态（审核 A-C2：原来是 luaopen_math 的上值，整个 VM 共享，
// 一个 Mod 调用 randomseed 会改变所有 Mod 的序列）。函数还是 lmathlib 的，只把上值换成新的状态；
// 初始种子用高精度时钟和 Mod 的 token，同时加载的两个 Mod 也不同
void GiveOwnRandom(lua_State* L, int math, std::uint64_t token)
{
    math = lua_absindex(L, math);
    lua_getglobal(L, LUA_MATHLIBNAME);
    lua_getfield(L, -1, "random");
    lua_getfield(L, -2, "randomseed");
    const lua_CFunction random = lua_tocfunction(L, -2);
    const lua_CFunction seed = lua_tocfunction(L, -1);
    std::size_t stateSize = 0;
    if (lua_getupvalue(L, -2, 1)) {
        stateSize = lua_type(L, -1) == LUA_TUSERDATA ? lua_rawlen(L, -1) : 0;
        lua_pop(L, 1);
    }
    lua_pop(L, 3);
    if (!random || !seed || stateSize == 0) {
        return;  // 布局不认识（不会发生）：保留共享的状态
    }
    lua_newuserdatauv(L, stateSize, 0);
    lua_pushvalue(L, -1);
    lua_pushcclosure(L, random, 1);
    lua_setfield(L, math, "random");
    lua_pushcclosure(L, seed, 1);
    lua_pushvalue(L, -1);
    lua_setfield(L, math, "randomseed");
    const auto now = static_cast<lua_Integer>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    lua_pushinteger(L, now);
    lua_pushinteger(L, static_cast<lua_Integer>(token * 0x9E3779B97F4A7C15ull));
    lua_call(L, 2, 0);
}

void ClearFields(lua_State* L, const char* global, const std::vector<const char*>& keep)
{
    if (lua_getglobal(L, global) != LUA_TTABLE) {
        lua_pop(L, 1);
        return;
    }
    std::vector<std::string> remove;
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        lua_pop(L, 1);
        if (lua_type(L, -1) == LUA_TSTRING) {
            const char* key = lua_tostring(L, -1);
            bool kept = false;
            for (const char* name : keep) {
                kept = kept || std::strcmp(name, key) == 0;
            }
            if (!kept) {
                remove.emplace_back(key);
            }
        }
    }
    for (const auto& key : remove) {
        lua_pushnil(L);
        lua_setfield(L, -2, key.c_str());
    }
    lua_pop(L, 1);
}

// 把真实库表 global 的字段复制到新表，压栈
void CopyLib(lua_State* L, const char* global)
{
    lua_newtable(L);
    if (lua_getglobal(L, global) == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            lua_rawset(L, -5);
        }
    }
    lua_pop(L, 1);
}

}  // namespace

void PrepareGlobals(lua_State* L)
{
    static const luaL_Reg kLibs[] = {
        { LUA_GNAME, luaopen_base },
        { LUA_COLIBNAME, luaopen_coroutine },
        { LUA_TABLIBNAME, luaopen_table },
        { LUA_STRLIBNAME, luaopen_string },
        { LUA_MATHLIBNAME, luaopen_math },
        { LUA_UTF8LIBNAME, luaopen_utf8 },
        { LUA_OSLIBNAME, luaopen_os },
        { LUA_DBLIBNAME, luaopen_debug },
    };
    for (const auto& lib : kLibs) {
        luaL_requiref(L, lib.name, lib.func, 1);
        lua_pop(L, 1);
    }
    // 纵深防御：真实库表本来就不可达，危险函数也一并删掉
    for (const char* name : { "dofile", "loadfile", "load", "collectgarbage", "require", "print", "warn" }) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
    ClearFields(L, LUA_OSLIBNAME, { "time", "clock", "date", "difftime" });
    ClearFields(L, LUA_DBLIBNAME, { "traceback" });
    lua_getglobal(L, LUA_STRLIBNAME);
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);

    // 字符串元表：__index 是真实的 string 表，挡住 getmetatable("")
    lua_pushliteral(L, "");
    if (lua_getmetatable(L, -1)) {
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "__metatable");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

void PushModEnv(lua_State* L, Session& session, Mod& mod)
{
    lua_createtable(L, 0, 40);
    const int env = lua_gettop(L);

    for (const char* name : { "assert", "error", "getmetatable", "ipairs", "next", "pairs", "rawequal", "rawget", "rawlen",
             "rawset", "select", "tonumber", "tostring", "type", "_VERSION" }) {
        lua_getglobal(L, name);
        lua_setfield(L, env, name);
    }
    lua_pushcfunction(L, SafePcall);
    lua_setfield(L, env, "pcall");
    lua_pushcfunction(L, SafeXpcall);
    lua_setfield(L, env, "xpcall");
    lua_pushcfunction(L, SafeSetmetatable);
    lua_setfield(L, env, "setmetatable");
    lua_pushcfunction(L, SafeCollectgarbage);
    lua_setfield(L, env, "collectgarbage");
    lua_pushvalue(L, env);
    lua_pushcclosure(L, SafeLoad, 1);
    lua_setfield(L, env, "load");
    SetClosure(L, env, "print", LogFn<DD_LOG_INFO>, mod.token);
    SetClosure(L, env, "warn", Warn, mod.token);
    SetClosure(L, env, "require", Require, mod.token);
    lua_pushvalue(L, env);
    lua_setfield(L, env, "_G");

    // 库的副本
    for (const char* name : { LUA_STRLIBNAME, LUA_TABLIBNAME, LUA_MATHLIBNAME, LUA_UTF8LIBNAME, LUA_COLIBNAME }) {
        CopyLib(L, name);
        lua_setfield(L, env, name);
    }
    lua_getfield(L, env, LUA_MATHLIBNAME);
    GiveOwnRandom(L, -1, mod.token);
    lua_pop(L, 1);
    lua_getfield(L, env, LUA_COLIBNAME);
    for (const char* name : { "resume", "close" }) {
        lua_getfield(L, -1, name);
        lua_pushcclosure(L, CallOriginalThenCheck, 1);
        lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);

    lua_createtable(L, 0, 4);  // os
    lua_getglobal(L, LUA_OSLIBNAME);
    for (const char* name : { "time", "clock", "difftime" }) {
        lua_getfield(L, -1, name);
        lua_setfield(L, -3, name);
    }
    lua_getfield(L, -1, "date");
    lua_pushcclosure(L, SafeOsDate, 1);
    lua_setfield(L, -3, "date");
    lua_pop(L, 1);
    lua_setfield(L, env, LUA_OSLIBNAME);

    lua_createtable(L, 0, 1);  // debug
    lua_getglobal(L, LUA_DBLIBNAME);
    lua_getfield(L, -1, "traceback");
    lua_setfield(L, -3, "traceback");
    lua_pop(L, 1);
    lua_setfield(L, env, LUA_DBLIBNAME);

    // dd
    lua_createtable(L, 0, 24);
    const int dd = lua_gettop(L);
    lua_pushstring(L, mod.id().c_str());
    lua_setfield(L, dd, "id");
    lua_pushstring(L, mod.manifest.versionText.c_str());
    lua_setfield(L, dd, "version");
    lua_pushinteger(L, kLuaApiVersion);
    lua_setfield(L, dd, "api");
    lua_pushboolean(L, session.config().devMode);
    lua_setfield(L, dd, "dev");
    SetClosure(L, dd, "import", Import, mod.token);
    SetClosure(L, dd, "reload", Reload, mod.token);
    SetClosure(L, dd, "stats", Stats, mod.token);
    EventBinding::AddFunctions(L, dd, mod.token);
    Scheduler::AddFunctions(L, dd, mod.token);
    actor::AddFunctions(L, dd, mod.token);

    // dd.state：按 Mod id 存在会话里，跨重载保留
    lua_rawgeti(L, LUA_REGISTRYINDEX, session.stateRef());
    lua_pushstring(L, mod.id().c_str());
    if (lua_rawget(L, -2) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, mod.id().c_str());
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    lua_setfield(L, dd, "state");
    lua_pop(L, 1);

    lua_createtable(L, 0, 4);
    SetClosure(L, lua_gettop(L), "debug", LogFn<DD_LOG_DEBUG>, mod.token);
    SetClosure(L, lua_gettop(L), "info", LogFn<DD_LOG_INFO>, mod.token);
    SetClosure(L, lua_gettop(L), "warn", LogFn<DD_LOG_WARN>, mod.token);
    SetClosure(L, lua_gettop(L), "error", LogFn<DD_LOG_ERROR>, mod.token);
    lua_setfield(L, dd, "log");

    lua_createtable(L, 0, 3);
    SetClosure(L, lua_gettop(L), "read", FsRead, mod.token);
    SetClosure(L, lua_gettop(L), "exists", FsExists, mod.token);
    SetClosure(L, lua_gettop(L), "list", FsList, mod.token);
    lua_setfield(L, dd, "fs");

    lua_setfield(L, env, "dd");
}

void LoadModFile(lua_State* L, Mod& mod, const std::string& rel)
{
    std::string error;
    if (!mod.luaFs.PushFile(L, rel, &error)) {
        luaL_error(L, "读取 lua/%s 失败：%s", rel.c_str(), error.c_str());
    }
    SliceSource src;
    src.data = lua_tolstring(L, -1, &src.size);
    const std::string chunkName = "@" + mod.id() + "/" + rel;
    if (lua_load(L, SlicedReader, &src, chunkName.c_str(), "t") != LUA_OK) {
        lua_error(L);  // 语法错误（或 require 里超预算），消息已带文件和行号
    }
    lua_remove(L, -2);
    lua_rawgeti(L, LUA_REGISTRYINDEX, mod.envRef);
    if (!lua_setupvalue(L, -2, 1)) {  // 主 chunk 的第 1 个上值是 _ENV
        lua_pop(L, 1);
    }
}

int RunMainChunk(lua_State* L)
{
    Mod& mod = CheckMod(L);
    lua_settop(L, 0);
    LoadModFile(L, mod, "main.lua");
    lua_call(L, 0, 1);
    return 1;
}

}  // namespace ddlua
