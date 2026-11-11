#include "scheduler.h"

#include "events.h"
#include "sandbox.h"
#include "session.h"
#include "vm.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <utility>

namespace ddlua {

namespace {

constexpr const char* kTimerMeta = "dd.timer";
constexpr const char* kTaskMeta = "dd.task";

struct HandleUd {
    std::uint64_t id;
};

Scheduler& SchedulerOf(lua_State* L)
{
    return SessionOf(L).scheduler();
}

double CheckSeconds(lua_State* L, int index, const char* what)
{
    const lua_Number value = luaL_checknumber(L, index);
    if (!(value >= 0) || !std::isfinite(static_cast<double>(value))) {
        luaL_error(L, "%s 的秒数必须是非负的有限数", what);
    }
    return static_cast<double>(value);
}

// dd.wait / dd.wait_event 只能在调度器正在恢复的那个协程里直接调用
Scheduler::Task& RequireTask(lua_State* L, const char* what)
{
    Scheduler& scheduler = SchedulerOf(L);
    Scheduler::Task* task = scheduler.Current();
    if ((!task || task->co != L) && scheduler.FindTaskByThread(L)) {
        // 审核 B-N1：原来报「只能在 dd.spawn 创建的协程里调用」，误导
        luaL_error(L, "%s：这个 dd.spawn 的协程是被 coroutine.resume 手动恢复的，dd.wait、dd.wait_event 只能在调度器恢复它时调用"
                      "（不要手动恢复 dd.spawn 的协程）", what);
    }
    if (!lua_isyieldable(L) || !task) {
        luaL_error(L, "%s 只能在 dd.spawn 创建的协程里调用：事件回调、定时器回调和 main.lua 都在主线程上运行，不能挂起", what);
    }
    if (task->co != L) {
        luaL_error(L, "%s 只能在 dd.spawn 的协程里直接调用，不能在 coroutine.create / coroutine.wrap 创建的协程里调用", what);
    }
    return *task;  // 已请求取消的照常挂起，调度器在挂起后关闭它
}

// 挂起状态的协程可以关闭；没有调用栈的（已结束、或被 Mod 自己关闭的）关闭也无害；正在运行或 normal 的不能碰
bool Closable(lua_State* co)
{
    if (lua_status(co) == LUA_YIELD) {
        return true;
    }
    lua_Debug ar;
    return lua_status(co) == LUA_OK && lua_getstack(co, 0, &ar) == 0;
}

int LuaSpawn(lua_State* L)
{
    Mod& mod = CheckLiveMod(L, "dd.spawn");
    luaL_checktype(L, 1, LUA_TFUNCTION);
    const int nargs = lua_gettop(L) - 1;
    Scheduler& scheduler = SchedulerOf(L);
    const std::uint32_t limit = SessionOf(L).config().maxPerMod;
    if (scheduler.CountFor(&mod).tasks >= limit) {
        return luaL_error(L, "dd.spawn：协程数达到上限 %d（DDLua.cfg 的 max_per_mod）", static_cast<int>(limit));
    }
    auto* ud = static_cast<HandleUd*>(lua_newuserdatauv(L, sizeof(HandleUd), 0));
    ud->id = 0;
    luaL_setmetatable(L, kTaskMeta);
    const int handle = lua_gettop(L);
    lua_State* co = lua_newthread(L);  // 继承主线程的计数 hook（lua_newthread 复制创建者的 hook）
    if (!lua_checkstack(co, nargs + 1)) {
        return luaL_error(L, "dd.spawn 的参数太多");
    }
    lua_pushvalue(L, -1);
    const int threadRef = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, handle);
    const int handleRef = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);  // 线程
    for (int i = 1; i <= nargs + 1; ++i) {
        lua_pushvalue(L, i);
    }
    lua_xmove(L, co, nargs + 1);
    const std::uint64_t id = scheduler.AddTask(mod, co, threadRef, handleRef);
    ud->id = id;
    // 立即运行到第一次挂起（嵌套在调用者的预算入口里）
    scheduler.ResumeTask(*scheduler.FindTask(id), L, nargs);
    RethrowIfBudget(L);
    lua_pushvalue(L, handle);
    return 1;
}

int LuaWait(lua_State* L)
{
    CheckMod(L);
    Scheduler::Task& task = RequireTask(L, "dd.wait");
    const double seconds = CheckSeconds(L, 1, "dd.wait");
    Scheduler& scheduler = SchedulerOf(L);
    task.wait = Scheduler::Task::Wait::Sleep;
    task.due = scheduler.Now() + seconds;
    task.sinceTick = scheduler.TickCount();
    return lua_yield(L, 0);
}

int LuaWaitEvent(lua_State* L)
{
    CheckLiveMod(L, "dd.wait_event");
    Scheduler::Task& task = RequireTask(L, "dd.wait_event");
    luaL_checkstring(L, 1);
    int filterIndex = 0;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        filterIndex = 2;
    }
    double timeout = std::numeric_limits<double>::infinity();
    if (!lua_isnoneornil(L, 3)) {
        timeout = CheckSeconds(L, 3, "dd.wait_event");
    }
    Session& session = SessionOf(L);
    Scheduler& scheduler = session.scheduler();
    std::string reason;
    // 等待者记在协程所属的 Mod 名下，Mod 拆除时和协程一起撤销
    const std::uint64_t key = session.events().AddWaiter(L, *task.mod, 1, filterIndex, task.id, &reason);
    if (!key) {
        session.LogId(DD_LOG_WARN, task.modId, "dd.wait_event 失败：" + reason);
        lua_pushnil(L);
        lua_pushstring(L, reason.c_str());
        return 2;
    }
    task.wait = Scheduler::Task::Wait::Event;
    task.waiterKey = key;
    task.woken = false;
    task.due = scheduler.Now() + timeout;
    task.sinceTick = scheduler.TickCount();
    return lua_yield(L, 0);  // 恢复时的参数（快照或 nil）就是 dd.wait_event 的返回值
}

int AddTimer(lua_State* L, bool repeat)
{
    const char* what = repeat ? "dd.every" : "dd.after";
    Mod& mod = CheckLiveMod(L, what);
    const double seconds = CheckSeconds(L, 1, what);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const std::uint32_t limit = SessionOf(L).config().maxPerMod;
    if (SchedulerOf(L).CountFor(&mod).timers >= limit) {
        return luaL_error(L, "%s：定时器数达到上限 %d（DDLua.cfg 的 max_per_mod）", what, static_cast<int>(limit));
    }
    auto* ud = static_cast<HandleUd*>(lua_newuserdatauv(L, sizeof(HandleUd), 0));
    ud->id = 0;
    luaL_setmetatable(L, kTimerMeta);
    lua_pushvalue(L, 2);
    const int fnRef = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, -1);
    const int handleRef = luaL_ref(L, LUA_REGISTRYINDEX);
    ud->id = SchedulerOf(L).AddTimer(mod, seconds, seconds, repeat, fnRef, handleRef);
    return 1;
}

int LuaAfter(lua_State* L)
{
    return AddTimer(L, false);
}

int LuaEvery(lua_State* L)
{
    return AddTimer(L, true);
}

int LuaTime(lua_State* L)
{
    CheckMod(L);
    lua_pushnumber(L, static_cast<lua_Number>(SchedulerOf(L).Now()));
    return 1;
}

// ---- 句柄 ----

int TimerCancel(lua_State* L)
{
    auto* ud = static_cast<HandleUd*>(luaL_checkudata(L, 1, kTimerMeta));
    Scheduler& scheduler = SchedulerOf(L);
    if (const Scheduler::Timer* timer = scheduler.FindTimer(ud->id)) {
        CheckOwner(L, timer->mod, timer->modId, "timer:cancel()");
    }
    lua_pushboolean(L, scheduler.CancelTimer(ud->id));
    return 1;
}

int TimerActive(lua_State* L)
{
    auto* ud = static_cast<HandleUd*>(luaL_checkudata(L, 1, kTimerMeta));
    lua_pushboolean(L, SchedulerOf(L).FindTimer(ud->id) != nullptr);
    return 1;
}

int TimerIndex(lua_State* L)
{
    luaL_checkudata(L, 1, kTimerMeta);
    const char* key = lua_tostring(L, 2);
    if (key && std::strcmp(key, "cancel") == 0) {
        lua_pushcfunction(L, TimerCancel);
    } else if (key && std::strcmp(key, "active") == 0) {
        lua_pushcfunction(L, TimerActive);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int TimerToString(lua_State* L)
{
    auto* ud = static_cast<HandleUd*>(luaL_checkudata(L, 1, kTimerMeta));
    lua_pushfstring(L, "dd.timer(%s)", SchedulerOf(L).FindTimer(ud->id) ? "有效" : "已撤销");
    return 1;
}

int TaskCancel(lua_State* L)
{
    auto* ud = static_cast<HandleUd*>(luaL_checkudata(L, 1, kTaskMeta));
    Scheduler& scheduler = SchedulerOf(L);
    if (const Scheduler::Task* task = scheduler.FindTask(ud->id)) {
        CheckOwner(L, task->mod, task->modId, "task:cancel()");
    }
    const bool existed = scheduler.CancelTask(ud->id, L);
    RethrowIfBudget(L);  // 关闭协程时 <close> 超预算
    lua_pushboolean(L, existed);
    return 1;
}

const char* TaskStatus(Scheduler& scheduler, std::uint64_t id)
{
    Scheduler::Task* task = scheduler.FindTask(id);
    if (!task) {
        return "dead";
    }
    if (task->wait == Scheduler::Task::Wait::None) {
        return "running";
    }
    return "waiting";
}

int TaskStatusFn(lua_State* L)
{
    auto* ud = static_cast<HandleUd*>(luaL_checkudata(L, 1, kTaskMeta));
    lua_pushstring(L, TaskStatus(SchedulerOf(L), ud->id));
    return 1;
}

int TaskActive(lua_State* L)
{
    auto* ud = static_cast<HandleUd*>(luaL_checkudata(L, 1, kTaskMeta));
    lua_pushboolean(L, SchedulerOf(L).FindTask(ud->id) != nullptr);
    return 1;
}

int TaskIndex(lua_State* L)
{
    luaL_checkudata(L, 1, kTaskMeta);
    const char* key = lua_tostring(L, 2);
    if (key && std::strcmp(key, "cancel") == 0) {
        lua_pushcfunction(L, TaskCancel);
    } else if (key && std::strcmp(key, "status") == 0) {
        lua_pushcfunction(L, TaskStatusFn);
    } else if (key && std::strcmp(key, "active") == 0) {
        lua_pushcfunction(L, TaskActive);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int TaskToString(lua_State* L)
{
    auto* ud = static_cast<HandleUd*>(luaL_checkudata(L, 1, kTaskMeta));
    lua_pushfstring(L, "dd.task(%s)", TaskStatus(SchedulerOf(L), ud->id));
    return 1;
}

void NewHandleMeta(lua_State* L, const char* name, lua_CFunction index, lua_CFunction tostring)
{
    luaL_newmetatable(L, name);
    lua_pushcfunction(L, index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
}

}  // namespace

// ---- FrameBudget ----

namespace {

double MonotonicMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

FrameBudget::FrameBudget(std::uint32_t limitMs) : start_(MonotonicMs()), limitMs_(static_cast<double>(limitMs)) {}

bool FrameBudget::Allow()
{
    if (!started_) {
        started_ = true;  // 每帧至少做一项，保证前进
        return true;
    }
    return ElapsedMs() < limitMs_;
}

double FrameBudget::ElapsedMs() const
{
    return MonotonicMs() - start_;
}

Scheduler::Scheduler(Session& session) : session_(session) {}

Scheduler::~Scheduler()
{
    Vm* vm = session_.vm();
    lua_State* L = vm && vm->ok() ? vm->L() : nullptr;
    const auto unref = [&](int& ref) {
        if (L && ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
        }
        ref = LUA_NOREF;
    };
    for (auto& [id, timer] : timers_) {
        unref(timer->fnRef);
        unref(timer->handleRef);
    }
    for (auto& [id, task] : tasks_) {
        unref(task->threadRef);
        unref(task->handleRef);
        unref(task->resultRef);
    }
}

void Scheduler::RegisterMetatables(lua_State* L)
{
    NewHandleMeta(L, kTimerMeta, TimerIndex, TimerToString);
    NewHandleMeta(L, kTaskMeta, TaskIndex, TaskToString);
}

void Scheduler::AddFunctions(lua_State* L, int dd, std::uint64_t token)
{
    SetClosure(L, dd, "spawn", LuaSpawn, token);
    SetClosure(L, dd, "wait", LuaWait, token);
    SetClosure(L, dd, "wait_event", LuaWaitEvent, token);
    SetClosure(L, dd, "after", LuaAfter, token);
    SetClosure(L, dd, "every", LuaEvery, token);
    SetClosure(L, dd, "time", LuaTime, token);
}

double Scheduler::Now() const
{
    return session_.Now();
}

std::uint64_t Scheduler::TickCount() const
{
    return session_.TickCount();
}

Scheduler::Task* Scheduler::FindTask(std::uint64_t id) const
{
    const auto it = tasks_.find(id);
    return it == tasks_.end() ? nullptr : it->second.get();
}

Scheduler::Timer* Scheduler::FindTimer(std::uint64_t id) const
{
    const auto it = timers_.find(id);
    return it == timers_.end() ? nullptr : it->second.get();
}

Scheduler::Task* Scheduler::FindTaskByThread(const lua_State* co) const
{
    for (const auto& [id, task] : tasks_) {
        if (task->co == co) {
            return task.get();
        }
    }
    return nullptr;
}

std::uint64_t Scheduler::AddTimer(const Mod& mod, double delay, double interval, bool repeat, int fnRef, int handleRef)
{
    auto timer = std::make_unique<Timer>();
    timer->id = nextId_++;
    timer->mod = &mod;
    timer->modId = mod.id();
    timer->fnRef = fnRef;
    timer->handleRef = handleRef;
    timer->due = Now() + delay;
    timer->interval = interval;
    timer->repeat = repeat;
    timer->armedTick = TickCount();
    const std::uint64_t id = timer->id;
    timers_.emplace(id, std::move(timer));
    return id;
}

void Scheduler::ReleaseTimer(Timer& timer)
{
    Vm* vm = session_.vm();
    if (vm && vm->ok()) {
        luaL_unref(vm->L(), LUA_REGISTRYINDEX, timer.fnRef);
        luaL_unref(vm->L(), LUA_REGISTRYINDEX, timer.handleRef);
    }
    timer.fnRef = LUA_NOREF;
    timer.handleRef = LUA_NOREF;
}

bool Scheduler::CancelTimer(std::uint64_t id)
{
    const auto it = timers_.find(id);
    if (it == timers_.end()) {
        return false;
    }
    ReleaseTimer(*it->second);
    timers_.erase(it);
    return true;
}

std::uint64_t Scheduler::AddTask(const Mod& mod, lua_State* co, int threadRef, int handleRef)
{
    auto task = std::make_unique<Task>();
    task->id = nextId_++;
    task->mod = &mod;
    task->modId = mod.id();
    task->co = co;
    task->threadRef = threadRef;
    task->handleRef = handleRef;
    task->sinceTick = TickCount();
    const std::uint64_t id = task->id;
    tasks_.emplace(id, std::move(task));
    return id;
}

void Scheduler::FinishTask(std::uint64_t id)
{
    const auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return;
    }
    Task& task = *it->second;
    if (task.waiterKey && session_.HasRuntime()) {
        session_.events().RemoveWaiter(task.waiterKey);
    }
    Vm* vm = session_.vm();
    if (vm && vm->ok()) {
        for (int* ref : { &task.threadRef, &task.handleRef, &task.resultRef }) {
            luaL_unref(vm->L(), LUA_REGISTRYINDEX, *ref);
            *ref = LUA_NOREF;
        }
    }
    tasks_.erase(it);
}

void Scheduler::CloseTask(Task& task, lua_State* from)
{
    if (task.waiterKey && session_.HasRuntime()) {
        session_.events().RemoveWaiter(task.waiterKey);
        task.waiterKey = 0;
    }
    Vm* vm = session_.vm();
    if (!vm || !vm->ok() || !Closable(task.co)) {
        return;
    }
    if (!session_.CloseAllowed()) {
        return;  // 这次拆除或重载的总时间用完（第二轮 R2A-备注3）：不关闭，调用方随后丢弃引用，<close> 不运行
    }
    std::string error;
    const Session::Running running(session_, task.mod);  // <close> 代表任务所属的 Mod 执行
    if (vm->CloseThread(task.co, from, session_.config().budgetMs, &error) != LUA_OK) {
        session_.LogId(DD_LOG_ERROR, task.modId, "关闭协程时 <close> 出错：" + error);
    }
}

void Scheduler::ResumeTask(Task& task, lua_State* from, int nargs)
{
    Vm* vm = session_.vm();
    const std::uint64_t id = task.id;
    running_.push_back(&task);
    task.wait = Task::Wait::None;
    task.lastRun = TickCount();
    int results = 0;
    std::string error;
    bool budget = false;
    int status;
    {
        const Session::Running running(session_, task.mod);
        status = vm->Resume(task.co, from, nargs, &results, session_.config().budgetMs, &error, &budget);
    }
    running_.pop_back();
    if (status == LUA_YIELD) {
        lua_pop(task.co, results);  // 让出的值不用
        if (task.wait == Task::Wait::None) {
            task.wait = Task::Wait::Frame;  // coroutine.yield()：下一帧继续
            task.sinceTick = TickCount();
        }
        if (task.cancelRequested) {
            CloseTask(task, from);
            FinishTask(id);
        }
        return;
    }
    if (status == LUA_OK) {
        lua_settop(task.co, 0);
        FinishTask(id);
        return;
    }
    session_.LogId(DD_LOG_ERROR, task.modId, std::string(budget ? "协程超出执行预算，已终止：" : "协程出错，已终止：") + error);
    FinishTask(id);
}

bool Scheduler::CancelTask(std::uint64_t id, lua_State* from)
{
    Task* task = FindTask(id);
    if (!task) {
        return false;
    }
    const bool running = std::find(running_.begin(), running_.end(), task) != running_.end();
    if (running || !Closable(task->co)) {
        task->cancelRequested = true;  // 下一次挂起（或调度器下次处理它）时关闭
        return true;
    }
    CloseTask(*task, from);
    FinishTask(id);
    return true;
}

void Scheduler::WakeTask(std::uint64_t id, int resultRef)
{
    Task* task = FindTask(id);
    Vm* vm = session_.vm();
    if (!task || task->wait != Task::Wait::Event) {
        if (resultRef != LUA_NOREF && vm && vm->ok()) {
            luaL_unref(vm->L(), LUA_REGISTRYINDEX, resultRef);
        }
        return;
    }
    task->waiterKey = 0;  // 等待者已撤销
    task->woken = true;
    if (task->resultRef != LUA_NOREF && vm && vm->ok()) {
        luaL_unref(vm->L(), LUA_REGISTRYINDEX, task->resultRef);
    }
    task->resultRef = resultRef;
}

void Scheduler::SweepFinished()
{
    // Mod 用 coroutine.running() 拿到 dd.spawn 的线程，自己 coroutine.resume 到结束、出错或 coroutine.close：
    // 不在等待里的任务（比如 dd.wait_event 没设超时）原来要等到 Mod 拆除才清理，一直占着等待者订阅和 max_per_mod 的名额
    std::vector<std::uint64_t> dead;
    for (const auto& [id, task] : tasks_) {
        if (std::find(running_.begin(), running_.end(), task.get()) != running_.end()) {
            continue;
        }
        if (lua_status(task->co) != LUA_YIELD) {
            dead.push_back(id);
        }
    }
    for (const auto id : dead) {
        if (Task* task = FindTask(id)) {
            session_.LogId(DD_LOG_WARN, task->modId,
                "dd.spawn 的协程被 Mod 自己恢复到结束或关闭了，调度器不再管它（不要用 coroutine.resume / coroutine.close 操作 dd.spawn 的协程）");
            FinishTask(id);
        }
    }
}

void Scheduler::RunTimer(std::uint64_t id, double now)
{
    Vm* vm = session_.vm();
    Timer* timer = FindTimer(id);
    if (!timer || !vm || !vm->ok()) {
        return;  // 被前面的回调取消了
    }
    lua_State* L = vm->L();
    const int top = lua_gettop(L);
    std::string error;
    const bool pushed = vm->Protected(
        [&](lua_State* L) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, timer->fnRef);
            lua_rawgeti(L, LUA_REGISTRYINDEX, timer->handleRef);
        },
        &error);
    if (!pushed) {
        session_.LogId(DD_LOG_ERROR, timer->modId, "定时器回调没能开始：" + error);
        return;
    }
    timer->lastRun = TickCount();
    const bool repeat = timer->repeat;
    const std::string modId = timer->modId;
    const Mod* mod = timer->mod;
    if (repeat) {
        double next = timer->due + timer->interval;
        if (next <= now) {
            next = now + timer->interval;  // 不补发错过的次数
        }
        timer->due = next;
    } else {
        CancelTimer(id);  // 函数已经在栈上
    }
    bool budget = false;
    bool ok;
    {
        const Session::Running running(session_, mod);
        ok = vm->Call(1, 0, session_.config().budgetMs, &error, &budget);
    }
    if (!vm->ok()) {
        return;  // 原生异常之后不再碰 VM（正常走不到这里：异常已经把栈展开到 DDCore）
    }
    lua_settop(L, top);
    Timer* after = FindTimer(id);  // 回调里可能取消了自己
    if (ok) {
        if (after) {
            after->failures = 0;
        }
        return;
    }
    std::string text = repeat ? "dd.every 的回调出错" : "dd.after 的回调出错";
    if (after) {
        ++after->failures;
        const std::uint32_t limit = session_.config().maxFailures;
        if (budget || after->failures >= limit) {
            text += budget ? "（超出执行预算），已停用该定时器" : "（连续 " + std::to_string(after->failures) + " 次），已停用该定时器";
            CancelTimer(id);
        } else {
            text += "（连续第 " + std::to_string(after->failures) + " 次，" + std::to_string(limit) + " 次后停用）";
        }
    } else if (budget) {
        text += "（超出执行预算）";  // dd.after 在调用前已经撤销，这里只说明原因
    }
    session_.LogId(DD_LOG_ERROR, modId, text + "：" + error);
}

void Scheduler::RunTask(std::uint64_t id)
{
    Vm* vm = session_.vm();
    Task* task = FindTask(id);
    if (!task || !vm || !vm->ok()) {
        return;
    }
    lua_State* L = vm->L();
    if (task->cancelRequested) {
        CloseTask(*task, L);
        FinishTask(id);
        return;
    }
    if (lua_status(task->co) != LUA_YIELD) {
        // Mod 用 coroutine.running() 拿到线程，自己恢复到结束或关闭了它（本帧前面的代码做的）
        session_.LogId(DD_LOG_WARN, task->modId, "dd.spawn 的协程被 Mod 自己恢复到结束或关闭了，调度器不再管它");
        FinishTask(id);
        return;
    }
    int nargs = 0;
    if (task->wait == Task::Wait::Event) {
        if (task->waiterKey) {
            session_.events().RemoveWaiter(task->waiterKey);  // 超时
            task->waiterKey = 0;
        }
        if (!lua_checkstack(task->co, 1)) {
            session_.LogId(DD_LOG_ERROR, task->modId, "协程的栈空间不足，已终止");
            CloseTask(*task, L);
            FinishTask(id);
            return;
        }
        if (task->resultRef != LUA_NOREF) {
            lua_rawgeti(task->co, LUA_REGISTRYINDEX, task->resultRef);
            luaL_unref(L, LUA_REGISTRYINDEX, task->resultRef);
            task->resultRef = LUA_NOREF;
        } else {
            lua_pushnil(task->co);
        }
        nargs = 1;
    }
    task->woken = false;
    ResumeTask(*task, L, nargs);
}

void Scheduler::Tick(FrameBudget& frame)
{
    Vm* vm = session_.vm();
    if (!vm || !vm->ok()) {
        return;
    }
    const double now = Now();
    const std::uint64_t tick = TickCount();
    SweepFinished();

    // 本帧到点的工作：定时器（本帧回调里新建的下一帧才执行），到点的 dd.wait、已匹配或超时的 dd.wait_event、
    // coroutine.yield() 的下一帧、请求了取消的协程
    struct Work {
        bool timer;
        std::uint64_t id;
        std::uint64_t lastRun;
        double order;  // 定时器按到点时间，协程按创建顺序
    };
    std::map<std::string, std::vector<Work>> byMod;  // 按 Mod id（帧之间顺序稳定）
    std::size_t total = 0;
    for (const auto& [id, timer] : timers_) {
        if (timer->armedTick < tick && timer->due <= now) {
            byMod[timer->modId].push_back({ true, id, timer->lastRun, timer->due });
            ++total;
        }
    }
    for (const auto& [id, task] : tasks_) {
        if (task->sinceTick >= tick) {
            continue;
        }
        const bool go = (task->wait == Task::Wait::Sleep && task->due <= now) || task->wait == Task::Wait::Frame ||
            (task->wait == Task::Wait::Event && (task->woken || task->due <= now)) || task->cancelRequested;
        if (go) {
            byMod[task->modId].push_back({ false, id, task->lastRun, static_cast<double>(id) });
            ++total;
        }
    }
    if (total == 0) {
        return;
    }
    // 同一个 Mod 里：最久没执行的先（顺延的排在前面），同样久的定时器在协程前
    std::vector<std::vector<Work>*> queues;
    for (auto& [modId, queue] : byMod) {
        std::sort(queue.begin(), queue.end(), [](const Work& a, const Work& b) {
            if (a.lastRun != b.lastRun) {
                return a.lastRun < b.lastRun;
            }
            if (a.timer != b.timer) {
                return a.timer;
            }
            return a.order < b.order;
        });
        queues.push_back(&queue);
    }
    // Mod 之间轮流：每轮每个 Mod 一项，每帧换一个 Mod 先开始，帧预算用完时谁也不会一直排在后面
    const std::size_t first = static_cast<std::size_t>(rotation_++ % queues.size());
    std::size_t remaining = total;
    for (std::size_t round = 0; remaining > 0; ++round) {
        for (std::size_t k = 0; k < queues.size() && remaining > 0; ++k) {
            const auto& queue = *queues[(first + k) % queues.size()];
            if (round >= queue.size()) {
                continue;
            }
            if (!frame.Allow()) {
                frame.Defer(remaining);
                return;
            }
            --remaining;
            const Work& work = queue[round];
            if (work.timer) {
                RunTimer(work.id, now);
            } else {
                RunTask(work.id);
            }
            if (!vm->ok()) {
                return;
            }
        }
    }
}

void Scheduler::ReleaseMod(const Mod& mod)
{
    std::vector<std::uint64_t> timers;
    for (const auto& [id, timer] : timers_) {
        if (timer->mod == &mod) {
            timers.push_back(id);
        }
    }
    for (const auto id : timers) {
        CancelTimer(id);
    }
    std::vector<std::uint64_t> tasks;
    for (const auto& [id, task] : tasks_) {
        if (task->mod == &mod) {
            tasks.push_back(id);
        }
    }
    Vm* vm = session_.vm();
    for (const auto id : tasks) {
        Task* task = FindTask(id);
        if (!task) {
            continue;
        }
        if (std::find(running_.begin(), running_.end(), task) != running_.end()) {
            task->cancelRequested = true;  // 不会发生：拆除不在 Lua 代码里进行
            continue;
        }
        if (vm && vm->ok()) {
            CloseTask(*task, vm->L());
        }
        FinishTask(id);
    }
}

Scheduler::Counts Scheduler::CountFor(const Mod* mod) const
{
    Counts counts;
    for (const auto& [id, timer] : timers_) {
        counts.timers += !mod || timer->mod == mod ? 1 : 0;
    }
    for (const auto& [id, task] : tasks_) {
        counts.tasks += !mod || task->mod == mod ? 1 : 0;
    }
    return counts;
}

}  // namespace ddlua
