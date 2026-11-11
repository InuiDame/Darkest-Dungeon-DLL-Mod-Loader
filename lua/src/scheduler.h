#pragma once
// 调度器：dd.spawn / dd.wait / dd.wait_event 的协程，dd.after / dd.every 的定时器，每帧由 Session::Tick 驱动。
//
// 时间基准是真实时间（Session::Now，单调时钟，秒），每帧检查一次，精度是一帧。游戏卡顿或加载时到点的
// 定时器和协程在下一帧一次性执行，every 不补发错过的次数。帧内创建的定时器、开始等待的协程最早下一帧才执行，
// dd.wait(0) 和 coroutine.yield() 都等到下一帧。
//
// 协程不跨越 hook 边界：事件回调、定时器回调、main.lua 都在主线程上运行，在那里调用 dd.wait 直接报错；
// dd.spawn 的协程只由调度器恢复（在帧边界，游戏的 hook 不在栈上）。dd.wait_event 给协程一份字段快照，
// 在下一帧恢复，不在派发中途交出活的负载。
//
// 每次恢复协程、执行定时器回调都开一个预算入口；协程出错或超预算时关闭它（<close> 变量照常执行，同样受预算
// 约束），定时器回调超预算立即停用，连续出错 max_failures 次停用。Mod 拆除时定时器撤销、协程关闭。
//
// 每帧总时间（审核 A-B2）：到点的定时器和协程按 Mod 分组，各 Mod 轮流执行一项（每帧换一个 Mod 先开始），
// 同一个 Mod 里最久没执行的先执行。本帧用时到了 frame_budget_ms（FrameBudget），剩下的顺延到下一帧，
// 它们仍然到点，下一帧排在前面。单项仍受 budget_ms 约束，所以一帧最多超出一项的预算。
// 已经结束的任务（Mod 用 coroutine.resume / coroutine.close 自己跑完或关掉的）每帧清理（审核 B-N1）。

#include <lauxlib.h>
#include <lua.h>

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ddlua {

class Session;
struct Mod;

// 一帧里调度器工作（重载、定时器、协程恢复）的总时间（DDLua.cfg 的 frame_budget_ms，真实时间）。
// 每项开始前问 Allow()：第一项总是可以，之后用时到了上限就不再开始新的
class FrameBudget
{
public:
    explicit FrameBudget(std::uint32_t limitMs);
    bool Allow();
    void Defer(std::size_t items) { deferred_ += items; }
    std::size_t Deferred() const { return deferred_; }
    double ElapsedMs() const;

private:
    double start_;
    double limitMs_;
    bool started_ = false;
    std::size_t deferred_ = 0;
};

class Scheduler
{
public:
    explicit Scheduler(Session& session);
    ~Scheduler();  // 释放引用，不运行 Lua 代码（正常情况下 Mod 拆除时已经关闭）
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    static void RegisterMetatables(lua_State* L);
    // dd.spawn、dd.wait、dd.wait_event、dd.after、dd.every、dd.time
    static void AddFunctions(lua_State* L, int dd, std::uint64_t token);

    // 每帧：清理已结束的任务，然后按 Mod 轮流执行到点的定时器和到点、被唤醒的协程，帧预算用完的顺延
    void Tick(FrameBudget& frame);
    // dd.wait_event 匹配（resultRef 是快照的注册表引用）或等待者被撤销（LUA_NOREF，协程收到 nil），下一帧恢复
    void WakeTask(std::uint64_t id, int resultRef);
    // 撤销 Mod 的定时器，关闭它的协程（<close> 变量在预算入口里执行）
    void ReleaseMod(const Mod& mod);

    struct Counts {
        std::size_t timers = 0;
        std::size_t tasks = 0;
    };
    Counts CountFor(const Mod* mod) const;

    struct Timer;
    struct Task;

    // 给 Lua 的 C 函数用
    Task* Current() const { return running_.empty() ? nullptr : running_.back(); }
    Task* FindTask(std::uint64_t id) const;
    Timer* FindTimer(std::uint64_t id) const;
    // 协程线程对应的任务（错误提示用：Mod 手动恢复了 dd.spawn 的协程）
    Task* FindTaskByThread(const lua_State* co) const;
    std::uint64_t AddTimer(const Mod& mod, double delay, double interval, bool repeat, int fnRef, int handleRef);
    bool CancelTimer(std::uint64_t id);
    std::uint64_t AddTask(const Mod& mod, lua_State* co, int threadRef, int handleRef);
    // 恢复任务（from 是当前线程），返回后处理挂起、结束、出错
    void ResumeTask(Task& task, lua_State* from, int nargs);
    // 取消任务：正在运行的标记取消（下一次挂起时关闭），挂起的立即关闭。返回任务原来是否存在
    bool CancelTask(std::uint64_t id, lua_State* from);
    std::uint64_t TickCount() const;
    double Now() const;

private:
    void FinishTask(std::uint64_t id);
    void CloseTask(Task& task, lua_State* from);
    void ReleaseTimer(Timer& timer);
    void SweepFinished();
    void RunTimer(std::uint64_t id, double now);
    void RunTask(std::uint64_t id);

    Session& session_;
    std::map<std::uint64_t, std::unique_ptr<Timer>> timers_;  // 按 id（创建顺序）
    std::map<std::uint64_t, std::unique_ptr<Task>> tasks_;
    std::vector<Task*> running_;  // 正在恢复的任务（嵌套 spawn 时多层）
    std::uint64_t nextId_ = 1;
    std::uint64_t rotation_ = 0;  // 每帧换一个 Mod 先执行
};

struct Scheduler::Timer {
    std::uint64_t id = 0;
    const Mod* mod = nullptr;
    std::string modId;
    int fnRef = LUA_NOREF;
    int handleRef = LUA_NOREF;
    double due = 0;
    double interval = 0;
    bool repeat = false;
    std::uint64_t armedTick = 0;  // 创建时的帧号，最早下一帧执行
    std::uint64_t lastRun = 0;    // 上次执行的帧号（顺延的排在前面）
    std::uint32_t failures = 0;
};

struct Scheduler::Task {
    enum class Wait {
        None,   // 正在运行，或刚创建
        Sleep,  // dd.wait
        Event,  // dd.wait_event
        Frame,  // coroutine.yield()：下一帧
    };
    std::uint64_t id = 0;
    const Mod* mod = nullptr;
    std::string modId;
    lua_State* co = nullptr;
    int threadRef = LUA_NOREF;
    int handleRef = LUA_NOREF;
    Wait wait = Wait::None;
    double due = std::numeric_limits<double>::infinity();
    std::uint64_t sinceTick = 0;  // 开始等待时的帧号，最早下一帧恢复
    std::uint64_t waiterKey = 0;  // wait_event 的等待者
    bool woken = false;           // wait_event 已匹配（或等待者被撤销）
    int resultRef = LUA_NOREF;    // 恢复时传给协程的快照
    bool cancelRequested = false;
    std::uint64_t lastRun = 0;    // 上次由调度器恢复的帧号
};

}  // namespace ddlua
