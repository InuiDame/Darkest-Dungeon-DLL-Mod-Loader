#include "vm.h"

#include <lddstep.h>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <random>
#include <stdexcept>
#include <vector>

#ifndef SOL_USING_CXX_LUA
#error "DDLua 要求 Lua 按 C++ 编译（xmake/lua.lua 导出 SOL_USING_CXX_LUA）"
#endif
#if !defined(LUA_DDPATCH) || LUA_DDPATCH < 8
#error "DDLua 要求 xmake/lua.lua 的 dd_patch 8 以上（lua_ddswitch_fn、lua_ddgcthreshold_fn 等）"
#endif

namespace ddlua {

std::string ClipUtf8(std::string_view text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes) {
        return std::string(text);
    }
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return std::string(text.substr(0, cut)) + "…（截断，共 " + std::to_string(text.size()) + " 字节）";
}

namespace {

// 正在预算入口里的 VM（每个入口压栈一次，嵌套的入口也压，结束时出栈）。没有 lua_State 的进度回调用栈顶的 VM。
// 两个会话（开发、存档）各有一个 VM，事件可以从一个 VM 的 Lua 代码同步派发到另一个，再回到第一个，所以是栈，
// 而且嵌套的入口也要压栈（第三轮审核「无法确认」的第三条：只压最外层时，回到第一个 VM 的那段工作会记到第二个头上）
std::vector<Vm*>& Entries()
{
    static auto* entries = new std::vector<Vm*>();  // 不析构，进程退出时不碰
    return *entries;
}

void RemoveEntry(Vm* vm)
{
    auto& entries = Entries();
    const auto it = std::find(entries.rbegin(), entries.rend(), vm);
    if (it != entries.rend()) {
        entries.erase(std::next(it).base());
    }
}

void RemoveAllEntries(Vm* vm)
{
    auto& entries = Entries();
    entries.erase(std::remove(entries.begin(), entries.end(), vm), entries.end());
}

std::uint64_t Random64()
{
    // MSVC 的 random_device 走 rand_s（RtlGenRandom），不可用时退回 QPC
    try {
        std::random_device device;
        return (static_cast<std::uint64_t>(device()) << 32) ^ device();
    } catch (...) {
        LARGE_INTEGER value;
        QueryPerformanceCounter(&value);
        return static_cast<std::uint64_t>(value.QuadPart) * 0x9E3779B97F4A7C15ull ^ GetCurrentProcessId();
    }
}

// lstate.c 的字符串哈希种子（lua_ddseed_fn）：每个 VM 一个系统随机数。原来的 luai_makeseed 用时间和几个地址，
// Mod 用 os.time、tostring(线程)、tostring(print) 能推出大部分
unsigned int RandomSeed()
{
    const std::uint64_t value = Random64();
    return static_cast<unsigned int>(value ^ (value >> 32));
}

// 第三轮 R3A-阻断1：整数、浮点数、指针键的哈希密钥（ltable.c 的 lua_ddhashkey），每个进程生成一次。
// 必须在第一个 lua_State 之前设好，之后不能再改（已有的表里键的位置按旧密钥算）。DDLua 只通过 Vm 创建 lua_State
void InitLuaPatches()
{
    static std::once_flag once;
    std::call_once(once, [] {
        for (auto& key : lua_ddhashkey) {
            std::uint64_t value = 0;
            while (value == 0) {
                value = Random64();
            }
            key = value;
        }
    });
}

std::int64_t QpcNow()
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

std::int64_t QpcFrequency()
{
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    return frequency;
}

// lddstep.h 的进度回调：库函数的长循环每做一定量的工作调用一次
void DdStep(lua_State* L)
{
    Vm::From(L)->CheckBudget(L);
}

struct VmPanic : std::runtime_error {
    VmPanic() : std::runtime_error("Lua panic（保护模式之外的错误），VM 已停用") {}
};

int ProtectedTrampoline(lua_State* L)
{
    const auto* fn = static_cast<const std::function<void(lua_State*)>*>(lua_touserdata(L, 1));
    lua_remove(L, 1);
    try {
        (*fn)(L);
    } catch (const std::exception& e) {
        // Lua 自己的错误不是 std::exception，会直接穿过这里
        return luaL_error(L, "C++ 异常：%s", e.what());
    }
    return lua_gettop(L);
}

// 错误值转文字，不调用 __tostring（不是字符串和数字时用类型名）。字符串先截断再复制（审核 R2A-应修1）
std::string ErrorText(lua_State* L, int index, std::size_t maxBytes = kMaxErrorText)
{
    switch (lua_type(L, index)) {
    case LUA_TSTRING: {
        std::size_t length = 0;
        const char* text = lua_tolstring(L, index, &length);
        return ClipUtf8(std::string_view(text, length), maxBytes);
    }
    case LUA_TNUMBER: {
        // 自己格式化，不在保护模式之外让 Lua 分配字符串
        char buffer[64];
        if (lua_isinteger(L, index)) {
            std::snprintf(buffer, sizeof(buffer), LUA_INTEGER_FMT, static_cast<LUAI_UACINT>(lua_tointeger(L, index)));
        } else {
            std::snprintf(buffer, sizeof(buffer), LUA_NUMBER_FMT, static_cast<LUAI_UACNUMBER>(lua_tonumber(L, index)));
        }
        return buffer;
    }
    default:
        return std::string("（错误对象是 ") + luaL_typename(L, index) + " 值）";
    }
}

}  // namespace

Vm::Vm(std::size_t memoryLimit, void* owner) : owner_(owner), limit_(memoryLimit)
{
    // Lua 补丁的回调和密钥在第一个 lua_State 之前设好（lua_newstate 里就要用字符串哈希的种子）
    InitLuaPatches();
    lua_ddseed_fn = &RandomSeed;
    lua_ddswitch_fn = &Vm::Switch;
    lua_ddemergency_fn = &Vm::Emergency;
    lua_ddgcthreshold_fn = &Vm::GcThreshold;
    lua_ddatomic_fn = &Vm::Atomic;
    L_ = lua_newstate(&Vm::Alloc, this);
    if (!L_) {
        return;
    }
    lua_atpanic(L_, &Vm::Panic);
    running_ = L_;
    // 主线程上设置，之后创建的协程在 lua_newthread 里复制 hook
    lua_sethook(L_, &Vm::CountHook, LUA_MASKCOUNT, kHookInterval);
    lua_ddstep_fn = &DdStep;
    lua_ddstep_nol_fn = &Vm::StepNoState;
}

Vm::~Vm()
{
    Close();
    RemoveAllEntries(this);
}

Vm* Vm::Owner(lua_State* L)
{
    void* ud = nullptr;
    return lua_getallocf(L, &ud) == &Vm::Alloc ? static_cast<Vm*>(ud) : nullptr;
}

lua_State* Vm::Switch(lua_State* L)
{
    Vm* vm = Owner(L);
    if (!vm) {
        return nullptr;
    }
    lua_State* previous = vm->running_;
    vm->running_ = L;
    return previous;
}

int Vm::Emergency(lua_State* L, int phase)
{
    Vm* vm = Owner(L);
    if (!vm) {
        return 1;
    }
    const std::int64_t now = QpcNow();
    if (phase != 0) {
        // 核对审核应修 1：紧急 GC 是一整段停顿，和这个入口自己的分配量无关，补到截止时间上（有上限，见 CreditGcPause）
        if (vm->emergencyStart_ != 0) {
            vm->CreditGcPause(now - vm->emergencyStart_);
            vm->emergencyStart_ = 0;
        }
        // 完整 GC 之后仍然失败，或者这次分配成功了但离上限还不到 1/16：堆里基本都是活对象，回收的只够几次分配，
        // 再做也没用（审核者的用例里每个回调都做一次 GC，每次只回收上一个回调出错时留下的几 KB）
        const std::size_t room = vm->used_ < vm->limit_ ? vm->limit_ - vm->used_ : 0;
        if (phase == 1 || room < vm->limit_ / 16) {
            vm->emergencyFailedAt_ = now;
            if (vm->depth_ > 0) {
                vm->emergencyFailedInEntry_ = true;
            }
        }
        return 0;
    }
    bool skip = false;
    if (vm->depth_ > 0) {
        if (!vm->exceeded_ && now >= vm->deadline_) {
            vm->exceeded_ = true;  // 只置标记（这里不能报错），下一个检查点或入口结束时报告
        }
        skip = vm->emergencyFailedInEntry_ || vm->exceeded_;
    }
    if (!skip && vm->emergencyFailedAt_ != 0 &&
        now - vm->emergencyFailedAt_ < static_cast<std::int64_t>(kEmergencyCooldownMs) * QpcFrequency() / 1000) {
        skip = true;
    }
    if (skip) {
        ++vm->emergencySkips_;
        return 0;
    }
    ++vm->emergencyGcs_;
    vm->emergencyStart_ = now;
    vm->atomicStart_ = 0;  // 紧急 GC 打断了一轮增量 GC 也没关系：原子阶段不在别处单独计时
    return 1;
}

long long Vm::GcThreshold(lua_State* L, long long estimate, long long threshold)
{
    Vm* vm = Owner(L);
    if (!vm || vm->limit_ == 0) {
        return threshold;
    }
    const long long limit = static_cast<long long>(vm->limit_);
    // 上限的 7/8 以下开始下一轮；活对象已经接近它时给一轮增量 GC 留出上限的 1/16 的余量（一轮做完只要分配活对象的约 1%，
    // 余量够用，不会连续不停地 GC）。活对象超过上限的 15/16 时这个值在上限以上，退回紧急 GC
    const long long cap = (std::max)(limit - limit / 8, estimate + limit / 16);
    return (std::min)(threshold, cap);
}

void Vm::Atomic(lua_State* L, int phase)
{
    Vm* vm = Owner(L);
    if (!vm || vm->emergencyStart_ != 0) {
        return;  // 套在紧急 GC 里的原子阶段：整个紧急 GC 一起计时
    }
    const std::int64_t now = QpcNow();
    if (phase == 0) {
        vm->atomicStart_ = now;
    } else if (vm->atomicStart_ != 0) {
        vm->CreditGcPause(now - vm->atomicStart_);
        vm->atomicStart_ = 0;
    }
}

// 每个最外层入口最多补 max(一份预算, 这个入口里最长的一次停顿)：一个回调碰上的一次 GC（128 MB 时 60–180 ms，比预算长）
// 总能补全；在一个入口里反复触发紧急 GC（比如活对象很多时反复分配大串，每次都能回收上一个）只补得上第一次，之后照常超时，
// 不会因为补偿而永远到不了截止时间
void Vm::CreditGcPause(std::int64_t ticks)
{
    if (depth_ == 0 || ticks <= 0) {
        return;
    }
    gcPauseMax_ = (std::max)(gcPauseMax_, ticks);
    const std::int64_t budgetTicks = static_cast<std::int64_t>(budgetMs_) * QpcFrequency() / 1000;
    const std::int64_t allowance = (std::max)(budgetTicks, gcPauseMax_) - gcCredit_;
    const std::int64_t credit = (std::min)(ticks, allowance);
    if (credit < ticks) {
        ++gcPauseCapped_;
    }
    if (credit > 0) {
        deadline_ += credit;
        gcCredit_ += credit;
        ++gcPauseCredits_;
        gcPauseCreditedTotal_ += credit;
    }
}

double Vm::GcPauseCreditedMs() const
{
    return static_cast<double>(gcPauseCreditedTotal_) * 1000.0 / static_cast<double>(QpcFrequency());
}

// 没有 lua_State 的进度回调（lddstep.h 的 lua_ddstep_nol_fn）：不能报错、不能碰任何栈。超时后置粘滞标记，
// 让正在运行的线程在下一条指令进入计数 hook（lua_sethook 可以在任何时候调用）
void Vm::StepNoState()
{
    const auto& entries = Entries();
    if (entries.empty()) {
        return;
    }
    Vm* vm = entries.back();
    if (!vm->ok() || vm->depth_ == 0) {
        return;
    }
    if (vm->exceeded_ || vm->DeadlinePassed()) {
        vm->exceeded_ = true;
        if (vm->running_) {
            lua_sethook(vm->running_, &Vm::CountHook, LUA_MASKCOUNT, 1);
        }
        // 下一个带 lua_State 的进度检查立即报错（table.sort 的比较、库函数的循环都在 C 里，不经过计数 hook）
        lua_ddstep_work = LUA_DDSTEP_QUANTUM;
    }
}

Vm* Vm::From(lua_State* L)
{
    void* ud = nullptr;
    lua_getallocf(L, &ud);
    return static_cast<Vm*>(ud);
}

void* Vm::Alloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize)
{
    auto* vm = static_cast<Vm*>(ud);
    const std::size_t old = ptr ? osize : 0;  // ptr 为空时 osize 是对象类型，不是大小
    if (nsize == 0) {
        if (ptr) {
            vm->used_ -= old;
            std::free(ptr);
        }
        return nullptr;
    }
    if (nsize > old) {
        const std::size_t grow = nsize - old;
        if (vm->used_ + grow > vm->limit_ || vm->used_ + grow < vm->used_) {
            ++vm->allocFailures_;
            return nullptr;  // Lua 先做紧急 GC 再重试，仍然失败就报内存错误
        }
        // 大块分配（字符串拼接、string.rep 的结果、表扩容）顺便检查预算，超时就让分配失败
        if (nsize >= kLargeAlloc && vm->depth_ > 0 && (vm->exceeded_ || vm->DeadlinePassed())) {
            vm->exceeded_ = true;
            ++vm->allocFailures_;
            return nullptr;
        }
    }
    void* block = std::realloc(ptr, nsize);
    if (!block) {
        if (nsize <= old) {
            return ptr;  // 缩小失败时原块仍然可用（Lua 要求缩小不能失败）
        }
        ++vm->allocFailures_;
        return nullptr;
    }
    vm->used_ = vm->used_ - old + nsize;
    if (nsize > old) {
        vm->entryAllocated_ += nsize - old;
    }
    if (vm->used_ > vm->peak_) {
        vm->peak_ = vm->used_;
    }
    return block;
}

bool Vm::DeadlinePassed() const
{
    return QpcNow() >= deadline_;
}

void Vm::CheckBudget(lua_State* L)
{
    if (depth_ == 0) {
        return;
    }
    if (exceeded_ || DeadlinePassed()) {
        exceeded_ = true;
        RaiseBudget(L);
    }
}

void Vm::RaiseBudget(lua_State* L)
{
    luaL_error(L, "执行超出预算（%d 毫秒），已中断", static_cast<int>(budgetMs_));
    std::abort();  // luaL_error 不返回
}

void Vm::CountHook(lua_State* L, lua_Debug*)
{
    Vm* vm = From(L);
    if (lua_gethookcount(L) != kHookInterval) {
        lua_sethook(L, &Vm::CountHook, LUA_MASKCOUNT, kHookInterval);  // StepNoState 改成了 1
    }
    vm->CheckBudget(L);
}

int Vm::Panic(lua_State* L)
{
    if (auto* vm = From(L)) {
        vm->broken_ = true;
    }
    throw VmPanic();
}

int Vm::MessageHandler(lua_State* L)
{
    // lua_load 的保护调用沿用当前的消息处理函数：语法分析期间 reader 里报的错（超预算）已经在这里加过一次 traceback，
    // LoadModFile 重新抛出时不再加第二次
    if (lua_type(L, 1) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(L, 1, &length);
        if (std::string_view(text, length).find("\nstack traceback:") != std::string_view::npos) {
            lua_settop(L, 1);
            return 1;
        }
    }
    const std::string message = ErrorText(L, 1, kMaxErrorMessage);
    luaL_traceback(L, L, message.c_str(), 1);
    return 1;
}

void Vm::BeginEntry(std::uint32_t budgetMs)
{
    if (depth_++ == 0) {
        exceeded_ = false;
        emergencyFailedInEntry_ = false;
        entryAllocated_ = 0;
        gcCredit_ = 0;
        gcPauseMax_ = 0;
        budgetMs_ = budgetMs;
        deadline_ = QpcNow() + static_cast<std::int64_t>(budgetMs) * QpcFrequency() / 1000;
        lua_ddstep_work = 0;
    }
    Entries().push_back(this);
}

void Vm::EndEntry()
{
    RemoveEntry(this);
    if (--depth_ == 0) {
        exceeded_ = false;  // 控制权回到 DDLua，粘滞标记清除
        running_ = L_;
        // 这个入口里紧急 GC 没起作用，但入口自己分配了不少：那些多半是它的局部变量，现在成了垃圾，下一次照常紧急 GC
        if (emergencyFailedInEntry_ && entryAllocated_ >= limit_ / 16) {
            emergencyFailedAt_ = 0;
        }
    }
}

bool Vm::ExceededAtEnd()
{
    if (!exceeded_ && DeadlinePassed()) {
        exceeded_ = true;
    }
    return exceeded_;
}

std::string Vm::LateBudgetText() const
{
    return "执行超出预算（" + std::to_string(budgetMs_) + " 毫秒），已中断（入口结束时发现：最后一段工作跨过了截止时间，中间没有检查点）";
}

bool Vm::Protected(const std::function<void(lua_State*)>& fn, std::string* error, int nargs)
{
    return ProtectedOn(L_, fn, error, nargs);
}

bool Vm::ProtectedOn(lua_State* L, const std::function<void(lua_State*)>& fn, std::string* error, int nargs)
{
    if (!ok() || !L) {
        if (error) {
            *error = "VM 不可用";
        }
        return false;
    }
    const int top = lua_gettop(L) - nargs;
    if (top < 0 || !lua_checkstack(L, 2)) {
        if (top >= 0) {
            lua_settop(L, top);
        }
        if (error) {
            *error = "Lua 栈空间不足";
        }
        return false;
    }
    lua_pushcfunction(L, &ProtectedTrampoline);
    lua_pushlightuserdata(L, const_cast<std::function<void(lua_State*)>*>(&fn));
    lua_rotate(L, top + 1, 2);  // [参数..., 跳板, fn] -> [跳板, fn, 参数...]
    ++protected_;
    const int status = lua_pcall(L, nargs + 1, LUA_MULTRET, 0);
    --protected_;
    if (status != LUA_OK) {
        if (error) {
            *error = ErrorText(L, -1);
        }
        lua_settop(L, top);
        return false;
    }
    return true;
}

bool Vm::Call(int nargs, int nresults, std::uint32_t budgetMs, std::string* error, bool* budgetExceeded)
{
    if (budgetExceeded) {
        *budgetExceeded = false;
    }
    const int base = lua_gettop(L_) - nargs;  // 被调函数的位置
    if (!ok() || base < 1 || !lua_checkstack(L_, 2)) {
        if (base >= 1) {
            lua_settop(L_, base - 1);
        }
        if (error) {
            *error = ok() ? "Lua 栈空间不足" : "VM 不可用";
        }
        return false;
    }
    lua_pushcfunction(L_, &Vm::MessageHandler);
    lua_insert(L_, base);
    BeginEntry(budgetMs);
    // 嵌套时（协程里的 C 函数同步调用回 Lua）主线程上的这段代码才是正在运行的
    lua_State* const outer = running_;
    running_ = L_;
    const int status = lua_pcall(L_, nargs, nresults, base);
    running_ = outer;
    const bool exceeded = ExceededAtEnd();
    const std::string late = status == LUA_OK && exceeded ? LateBudgetText() : std::string();
    EndEntry();
    if (status == LUA_OK && exceeded) {
        // 第三轮 R3A-备注1：入口在下一个检查点之前就结束了，同样算超预算（回调停用、main.lua 加载失败）
        if (error) {
            *error = late;
        }
        if (budgetExceeded) {
            *budgetExceeded = true;
        }
        lua_settop(L_, base - 1);
        return false;
    }
    if (status != LUA_OK) {
        if (error) {
            *error = ErrorText(L_, -1);
            if (status == LUA_ERRMEM && !exceeded) {
                *error += "（内存超过上限 " + std::to_string(limit_ / (1024 * 1024)) + " MB）";
            }
        }
        if (exceeded && error && error->find("执行超出预算") == std::string::npos) {
            // 超预算由分配器发现时，错误是 "not enough memory"，补上真正的原因
            *error = "执行超出预算（" + std::to_string(budgetMs) + " 毫秒），已中断；原始错误：" + *error;
        }
        if (budgetExceeded) {
            *budgetExceeded = exceeded;
        }
        lua_settop(L_, base - 1);
        return false;
    }
    lua_remove(L_, base);
    return true;
}

int Vm::Resume(lua_State* co, lua_State* from, int nargs, int* nresults, std::uint32_t budgetMs, std::string* error,
    bool* budgetExceeded)
{
    if (budgetExceeded) {
        *budgetExceeded = false;
    }
    *nresults = 0;
    if (!ok() || !co || !from) {
        if (error) {
            *error = "VM 不可用";
        }
        return LUA_ERRRUN;
    }
    BeginEntry(budgetMs);
    int status = lua_resume(co, from, nargs, nresults);  // 补丁在进出时更新 running_
    bool exceeded = ExceededAtEnd();
    std::string message;
    if ((status == LUA_OK || status == LUA_YIELD) && exceeded) {
        // 第三轮 R3A-备注1：协程结束或让出时才发现超时，按超预算出错处理，协程在同一个入口里关闭
        lua_pop(co, *nresults);
        *nresults = 0;
        message = LateBudgetText();
        status = LUA_ERRRUN;
    } else if (status != LUA_OK && status != LUA_YIELD) {
        message = ErrorText(co, -1, kMaxErrorMessage);
        if (status == LUA_ERRMEM && !exceeded) {
            message += "（内存超过上限 " + std::to_string(limit_ / (1024 * 1024)) + " MB）";
        }
        if (exceeded && message.find("执行超出预算") == std::string::npos) {
            message = "执行超出预算（" + std::to_string(budgetMs_) + " 毫秒），已中断；原始错误：" + message;
        }
    }
    if (status != LUA_OK && status != LUA_YIELD) {
        // traceback 在当前线程上生成（要分配），协程停在出错的位置，层级还在
        std::string traceback;
        ProtectedOn(
            from,
            [&](lua_State* L) {
                luaL_traceback(L, co, nullptr, 0);
                std::size_t length = 0;
                const char* text = lua_tolstring(L, -1, &length);
                traceback.assign(text, length);
                lua_pop(L, 1);
            },
            nullptr);
        // 关闭协程：执行待关闭变量（仍在这个预算入口里），状态复位
        lua_closethread(co, from);
        lua_settop(co, 0);
        exceeded = exceeded || exceeded_;
        if (error) {
            *error = traceback.empty() ? message : ClipUtf8(message + "\n" + traceback, kMaxErrorText);
        }
        *nresults = 0;
    }
    EndEntry();
    if (budgetExceeded) {
        *budgetExceeded = exceeded;
    }
    return status;
}

int Vm::CloseThread(lua_State* co, lua_State* from, std::uint32_t budgetMs, std::string* error)
{
    if (!ok() || !co) {
        return LUA_OK;
    }
    BeginEntry(budgetMs);
    int status = lua_closethread(co, from);  // 补丁在进出时更新 running_
    const bool exceeded = ExceededAtEnd();
    if (status != LUA_OK && error) {
        *error = ErrorText(co, -1, kMaxErrorMessage);
        if (exceeded && error->find("执行超出预算") == std::string::npos) {
            *error = "执行超出预算（" + std::to_string(budgetMs_) + " 毫秒），已中断；原始错误：" + *error;
        }
    } else if (status == LUA_OK && exceeded) {
        if (error) {
            *error = LateBudgetText();  // <close> 都执行完了，但跨过了截止时间
        }
        status = LUA_ERRRUN;
    }
    lua_settop(co, 0);
    EndEntry();
    return status;
}

void Vm::FullGc()
{
    if (!ok() || depth_ != 0) {
        return;
    }
    // 字符串表等每轮 GC 只缩小一半，重复到内存不再下降
    for (int round = 0; round < 16; ++round) {
        const std::size_t before = used_;
        lua_gc(L_, LUA_GCCOLLECT);
        if (used_ >= before) {
            break;
        }
    }
}

std::size_t Vm::Close()
{
    if (L_) {
        if (Abandoned()) {
            broken_ = true;  // 有调用没返回（原生异常展开了栈）
        }
        if (!broken_) {
            lua_close(L_);
        }
        // panic 或原生异常之后状态不一致，宁可泄漏也不再碰它
        L_ = nullptr;
        running_ = nullptr;
    }
    RemoveAllEntries(this);  // 原生异常展开栈时没有出栈的入口
    return used_;
}

}  // namespace ddlua
