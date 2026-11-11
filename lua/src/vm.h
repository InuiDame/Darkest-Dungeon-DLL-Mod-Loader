#pragma once
// Lua VM：一个 lua_State、记账的分配器（内存上限）、执行预算。
//
// 执行预算：每次从 C++ 进入 Lua（Vm::Call）开一个 Entry，最外层的 Entry 设定截止时间（真实时间，
// QueryPerformanceCounter）。超时由三处发现：计数 hook（每 kHookInterval 条指令，主线程设置，协程创建时
// 继承）、库函数内部长循环的进度回调（lddstep.h 补丁）、分配器里的大块分配。超出后是粘滞的：直到最外层
// Entry 结束之前，每次检查都继续报错，沙箱里的 pcall/xpcall/load/coroutine.resume/close 捕获错误后
// 发现已超预算也会重新抛出，Mod 吞不掉。
//
// 第二轮审核 R2A-阻断1：Lua 内核里没有 lua_State 的长操作（长串比较、长串哈希、字符串转数字）通过 lua_ddstep_nol_fn 报告
// 工作量。超时后它不能报错，只置粘滞标记，并把正在运行的线程的 hook 间隔改成 1，让它在下一条指令进入计数 hook 报错；
// 计数 hook 每次把间隔恢复成 kHookInterval。
// 第三轮审核 R3A-阻断3：正在运行的线程由 Lua 补丁报告（lua_resume、lua_closethread 进出时调用 lua_ddswitch_fn），
// 不再用「最近一次计数 hook 看到的线程」：协程开始运行到第一次 hook 之前、让出后回到恢复方，那个线程都不是正在运行的。
// 第三轮审核 R3A-备注1：入口结束时再查一次截止时间，检查点之间跨过截止时间、入口在下一个检查点之前就结束的，同样算超预算。
// 第三轮审核 R3A-应修1：紧急 GC（分配失败时 Lua 做的完整 GC）在同一个入口里做过一次没起作用的（之后仍然失败，或者回收后
// 离上限不到 1/16）、或者入口已经超时，就不再做；没起作用的紧急 GC 之后 kEmergencyCooldownMs 之内（不分入口）也不再做，
// 堆里都是活对象时不让每个回调各付一次完整 GC。例外：没起作用的那个入口自己分配了不少（上限的 1/16 以上），它结束后
// 局部变量都成了垃圾，冷却取消，下一次分配失败照常紧急 GC。
// 核对审核应修 1（GC 停顿算到无辜的回调头上）：
// - 整段的 GC 停顿（紧急 GC、原子阶段）和当前入口自己的分配量无关，入口末尾的检查却会把它判给当时运行的回调。DDLua 给它们
//   计时（lua_ddemergency_fn 的第 0 阶段到第 1/2 阶段、lua_ddatomic_fn 的前后；原子阶段套在紧急 GC 里时只算紧急 GC 那一段），
//   补到当前入口的截止时间上。每个最外层入口最多补 max(一份预算, 这个入口里最长的一次停顿)：一次 GC 总能补全，反复触发 GC 的
//   入口补不了更多，入口最长是「预算 + 补的时间 + 截止前开始的最后一次停顿」。增量 GC 的每一小步和入口自己的分配量成正比，不补。
// - 下一轮 GC 的阈值（lua_ddgcthreshold_fn）压到 max(上限的 7/8, 活对象 + 上限的 1/16) 以下：活对象过半时原版的阈值
//   （活对象的 2 倍）超过上限，增量 GC 不再启动，每次都撞上上限做一次完整的紧急 GC。「活对象 + 1/16」保证一轮增量 GC 有足够
//   的余量做完，不会变成连续不停的 GC；活对象超过上限的 15/16 时阈值又到了上限以上，退回紧急 GC。
//
// 所有可能报错的 Lua API 调用都放在 Protected() 或 Call() 里，保护模式之外的错误会走 panic。
//
// 损坏（审核 B-Y3）：Lua 代码执行期间发生原生异常（SEH，比如访问违例）时，DDCore 的 SEH 隔离把栈展开到它自己，
// lua_State 的调用信息、errorJmp、预算入口的深度都停在异常发生时。之后只能把 VM 标记为损坏：ok() 为假，
// 所有入口都不再进入 Lua，Close 时不调用 lua_close（泄漏，不碰）。

#include <lua.h>
#include <lauxlib.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ddlua {

// 错误文字的上限（第二轮审核 R2A-应修1）：错误对象复制成 std::string 之前先截断，不受 Lua 内存上限约束的 CRT 堆上
// 不会出现几 MB 的副本。消息本身和「消息 + traceback」分别有上限
constexpr std::size_t kMaxErrorMessage = 2048;
constexpr std::size_t kMaxErrorText = 8192;

// 截到 maxBytes 字节以内（退到 UTF-8 字符边界），超出时加上「…（截断，共 N 字节）」
std::string ClipUtf8(std::string_view text, std::size_t maxBytes);

class Vm
{
public:
    static constexpr int kHookInterval = 1000;            // 计数 hook 的指令间隔
    static constexpr std::size_t kLargeAlloc = 64 * 1024; // 分配器只对不小于它的分配检查截止时间
    static constexpr int kEmergencyCooldownMs = 1000;     // 紧急 GC 失败之后多久之内不再做

    Vm(std::size_t memoryLimit, void* owner);
    ~Vm();
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

    static Vm* From(lua_State* L);

    lua_State* L() const { return L_; }
    void* owner() const { return owner_; }
    bool ok() const { return L_ != nullptr && !broken_; }
    bool broken() const { return broken_; }
    // 标记为损坏：只置标记，不分配、不碰 lua_State（可以在 SEH 过滤器里调用）
    void MarkBroken() { broken_ = true; }
    // DDLua 发起的 Lua 调用（Protected、Call、Resume、CloseThread）还没有返回。在帧边界（每帧 Tick、拆除）上为真，
    // 说明上一次调用被原生异常打断、栈已经被展开，VM 应当视为损坏
    bool Abandoned() const { return depth_ > 0 || protected_ > 0; }

    // 在保护模式下运行 C++ 代码，fn 里可以调用会报错的 Lua API。栈顶的 nargs 个值移进 fn 的栈帧（下标 1..nargs），
    // fn 结束时留在栈上的值作为结果留在调用方的栈上。失败时栈恢复到调用前（去掉 nargs 个值），error 写入原因。
    // fn 里只能操作 DDLua 自己的表（不触发 Mod 的元方法），调用 Mod 的代码要用 Call()
    bool Protected(const std::function<void(lua_State*)>& fn, std::string* error, int nargs = 0);
    // 同上，在指定线程上运行（在 Lua 调用的 C 函数里用当前线程，不碰别的线程的栈）
    bool ProtectedOn(lua_State* L, const std::function<void(lua_State*)>& fn, std::string* error, int nargs = 0);

    // 调用栈顶的函数（下面是 nargs 个参数），带 traceback 的错误处理，执行预算 budgetMs。
    // 成功时留下 nresults 个结果；失败时栈上不留东西，error 写入「消息 + traceback」，
    // budgetExceeded（可为空）指出是不是因为超预算
    bool Call(int nargs, int nresults, std::uint32_t budgetMs, std::string* error, bool* budgetExceeded = nullptr);

    // 恢复协程 co（co 的栈顶是 nargs 个参数）。from 是当前正在运行的线程：在 Lua 调用的 C 函数里是那个线程，
    // 从 C++ 顶层调用时是 L()。开一个预算入口（嵌套时沿用外层的截止时间）。
    // 返回 LUA_OK（协程结束）或 LUA_YIELD（挂起），co 的栈顶留着 *nresults 个返回或让出的值；
    // 出错时返回错误码，error 写「消息 + 协程的 traceback」，协程已在同一个预算入口里关闭（<close> 变量照常执行）
    int Resume(lua_State* co, lua_State* from, int nargs, int* nresults, std::uint32_t budgetMs, std::string* error,
        bool* budgetExceeded = nullptr);
    // 关闭挂起的协程（lua_closethread），<close> 变量在预算入口里执行。返回 lua_closethread 的状态，出错时写 error
    int CloseThread(lua_State* co, lua_State* from, std::uint32_t budgetMs, std::string* error);

    // 预算 -------------------------------------------------------------------
    bool InEntry() const { return depth_ > 0; }
    bool BudgetExceeded() const { return exceeded_; }
    std::uint32_t CurrentBudgetMs() const { return budgetMs_; }
    // 检查截止时间，超出时置粘滞标记并报错（不返回）
    void CheckBudget(lua_State* L);
    // 已超预算时抛出预算错误（不返回）；沙箱里捕获错误的函数用它重新抛出
    [[noreturn]] void RaiseBudget(lua_State* L);

    // 内存 -------------------------------------------------------------------
    std::size_t MemoryUsed() const { return used_; }
    std::size_t MemoryPeak() const { return peak_; }
    std::size_t MemoryLimit() const { return limit_; }
    std::uint64_t AllocFailures() const { return allocFailures_; }
    // 紧急 GC：做了几次、因为上面的规则跳过了几次
    std::uint64_t EmergencyGcs() const { return emergencyGcs_; }
    std::uint64_t EmergencySkips() const { return emergencySkips_; }
    // 补到入口截止时间上的 GC 停顿：次数、累计毫秒；入口里发生、但超过补偿上限没补全的次数
    std::uint64_t GcPauseCredits() const { return gcPauseCredits_; }
    double GcPauseCreditedMs() const;
    std::uint64_t GcPauseCapped() const { return gcPauseCapped_; }

    // 完整 GC，重复到内存不再下降（不受预算约束，只在 Lua 代码之外调用）
    void FullGc();

    // 关闭 lua_State，返回关闭后分配器上残留的字节数（应为 0）
    std::size_t Close();

private:
    friend struct VmAccess;
    static void* Alloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize);
    static void CountHook(lua_State* L, lua_Debug* ar);
    static int Panic(lua_State* L);
    static int MessageHandler(lua_State* L);
    // L 属于某个 Vm 时返回它（按分配器判断），否则为空（离线测试里 luaL_newstate 建的 state）
    static Vm* Owner(lua_State* L);
    // lddstep.h 的 lua_ddswitch_fn：L 开始运行，返回之前正在运行的线程
    static lua_State* Switch(lua_State* L);
    // lddstep.h 的 lua_ddemergency_fn：要不要做紧急 GC（phase 0），做完之后分配仍然失败（1）或成功（2）
    static int Emergency(lua_State* L, int phase);
    // lddstep.h 的 lua_ddgcthreshold_fn、lua_ddatomic_fn（dd_patch 8）
    static long long GcThreshold(lua_State* L, long long estimate, long long threshold);
    static void Atomic(lua_State* L, int phase);
    // 一段整段的 GC 停顿（QPC 计数），在入口里时按上限补到截止时间上
    void CreditGcPause(std::int64_t ticks);

    bool DeadlinePassed() const;
    void BeginEntry(std::uint32_t budgetMs);
    void EndEntry();
    // 入口结束前调用：已经超预算（粘滞标记），或者截止时间已过（检查点之间跨过、入口在下一个检查点之前结束）。
    // 为真时置粘滞标记，嵌套的外层入口随后照样报错
    bool ExceededAtEnd();
    std::string LateBudgetText() const;
    static void StepNoState();

    lua_State* L_ = nullptr;
    lua_State* running_ = nullptr;  // 正在运行的线程（lua_resume、lua_closethread 和 Call 维护）
    void* owner_ = nullptr;
    bool broken_ = false;

    std::size_t limit_ = 0;
    std::size_t used_ = 0;
    std::size_t peak_ = 0;
    std::uint64_t allocFailures_ = 0;
    std::uint64_t emergencyGcs_ = 0;
    std::uint64_t emergencySkips_ = 0;
    bool emergencyFailedInEntry_ = false;  // 这个入口里做过一次没起作用的紧急 GC
    std::size_t entryAllocated_ = 0;       // 最外层入口开始以来分配的字节数（只算增长）
    std::int64_t emergencyFailedAt_ = 0;   // 最近一次没起作用的紧急 GC（QPC），0 表示没有
    std::int64_t emergencyStart_ = 0;      // 正在进行的紧急 GC 开始的时间（QPC），0 表示没有
    std::int64_t atomicStart_ = 0;         // 正在进行的原子阶段开始的时间（QPC），0 表示没有
    std::int64_t gcCredit_ = 0;            // 这个最外层入口已经补上的 GC 停顿（QPC 计数）
    std::int64_t gcPauseMax_ = 0;          // 这个最外层入口里最长的一次 GC 停顿（QPC 计数）
    std::uint64_t gcPauseCredits_ = 0;
    std::int64_t gcPauseCreditedTotal_ = 0;
    std::uint64_t gcPauseCapped_ = 0;

    int depth_ = 0;
    int protected_ = 0;  // 正在进行的 ProtectedOn
    bool exceeded_ = false;
    std::int64_t deadline_ = 0;
    std::uint32_t budgetMs_ = 0;
};

}  // namespace ddlua
