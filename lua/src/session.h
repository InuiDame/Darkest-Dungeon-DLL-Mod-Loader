#pragma once
// 一次加载会话：一个 VM 和其中的 Lua Mod。按存档的会话在 CAMPAIGN_LOAD 创建、CAMPAIGN_UNLOAD 销毁；
// 开发用的全局会话在 ASSETS_READY 创建，一直保留到进程结束。
//
// 加载：按来源顺序（启用列表）找 <dir>/lua/manifest.json，解析、依赖排序，没有可加载的 Mod 时不创建 VM。
// 每个 Mod 一张 _ENV（sandbox.cpp），执行 main.lua；出错的 Mod 撤销它注册的一切，依赖它的 Mod 不加载。
// 拆除：按加载的逆序撤销每个 Mod，完整 GC，日志写出拆除前后的内存，最后关闭 VM。
//
// P3b：会话持有事件绑定（events.cpp）和调度器（scheduler.cpp），每个 Mod 的订阅、等待者、定时器、协程都记在
// 它们那里，Mod 拆除时由一个清理项统一撤销。Tick() 每帧调用：执行排队的重载、开发模式的文件监视、定时器和协程。
// 热重载（开发模式）：拆除目标 Mod 和依赖它的 Mod（按加载的逆序），再按加载顺序重新执行 main.lua；
// dd.state 按 Mod id 存在会话里，跨重载保留，会话结束时随 VM 丢弃。
//
// 每帧总时间（审核 A-B2）：Tick 里的重载、定时器、协程恢复共用 frame_budget_ms，超出的顺延到下一帧，各 Mod 轮流先执行。
// 事件回调不能顺延，仍然只有单次预算。
// VM 损坏（审核 B-Y3）：Lua 代码执行期间发生原生异常（SEH）后，本会话停止一切 Lua 派发，拆除时不碰 VM、直接丢弃。

#include "combat.h"
#include "config.h"
#include "events.h"
#include "host.h"
#include "manifest.h"
#include "modfs.h"
#include "scheduler.h"
#include "store.h"
#include "vm.h"

#include <map>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ddlua {

struct ModSource {
    std::string label;  // 日志里的来源说明（启用列表的 name/source，或开发目录）
    std::string dir;    // Mod 目录的绝对路径（UTF-8）
};

enum class ModState { Loading, Loaded, Failed, TornDown };

// 内存统计（字节），拆除时填写，离线测试和日志用
struct MemoryStats {
    std::size_t baseline = 0;  // VM 创建并准备好全局之后、加载任何 Mod 之前
    std::size_t loaded = 0;    // 全部 Mod 加载之后
    std::size_t before = 0;    // 拆除之前
    std::size_t after = 0;     // 拆除全部 Mod 并完整 GC 之后（VM 关闭之前）
    std::size_t residual = 0;  // VM 关闭之后分配器上的残留
};

// lua/ 目录的修改标记（文件监视比较用）
struct WatchStamp {
    std::uint64_t latest = 0;  // 最新的修改时间（FILETIME）
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
    bool operator==(const WatchStamp&) const = default;
};

struct Mod {
    std::uint64_t token = 0;  // 会话内唯一，每次（重新）加载换一个；Lua 闭包用它找回 Mod（拆除后找不到，调用报错）
    Manifest manifest;
    std::string label;
    std::string dir;
    ModFs fs;     // dd.fs 的根：Mod 目录
    ModFs luaFs;  // require 和 main.lua 的根：<Mod 目录>/lua
    int envRef = LUA_NOREF;
    int requireRef = LUA_NOREF;
    int exportsRef = LUA_NOREF;
    int importsRef = LUA_NOREF;  // dd.import 的代理缓存：提供方的 token -> 代理（第二轮 R2A-备注1）
    ModState state = ModState::Loading;
    std::string failure;  // 加载失败的原因
    bool tearingDown = false;  // 拆除中：注册订阅、定时器、协程的函数报错
    std::uint32_t loads = 0;   // main.lua 执行过几次（热重载计数）

    WatchStamp watch;         // 上一次加载时 lua/ 的标记
    WatchStamp pendingWatch;  // 发现变化后等一轮确认（编辑器保存可能分几次写）
    bool watchPending = false;

    // 日志限速（print、warn、dd.log，以及 DDLua 替这个 Mod 写的错误、失败原因）：当前一秒窗口的起点、窗口里写了几行、
    // 丢弃了几行（还没写汇总的）。写日志的函数是 const 的，所以是 mutable
    mutable double logWindow = -1;
    mutable std::uint32_t logLines = 0;
    mutable std::uint64_t logDropped = 0;

    // P4b 数据目录（dd.fs.write）：<游戏根目录>\DDModLoader\data\<mod id>\，第一次用到时打开（写入时创建）。
    // 用量第一次写入前扫描一次，之后按写入、删除增减
    ModFs dataFs;
    bool dataUsageKnown = false;
    std::uint64_t dataBytes = 0;
    std::uint64_t dataFiles = 0;

    struct Cleanup {
        std::uint64_t id;
        std::function<void()> fn;
    };
    std::vector<Cleanup> cleanups;  // 拆除时逆序执行（P3b 的订阅、定时器、协程都挂在这里）

    const std::string& id() const { return manifest.id; }
};

class Session
{
public:
    Session(const Host& host, const Config& config, std::string name);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void Load(const std::vector<ModSource>& sources);
    void Teardown();

    const std::string& name() const { return name_; }
    const Config& config() const { return config_; }
    const Host& host() const { return host_; }
    Vm* vm() const { return vm_.get(); }

    // 发现的 Lua Mod（有 manifest.json 的），按加载顺序；没进入加载的（manifest 无效、被排序拒绝）在后面
    const std::vector<std::unique_ptr<Mod>>& mods() const { return mods_; }
    Mod* FindMod(std::string_view id) const;      // 只找 Loading / Loaded 的
    Mod* FindToken(std::uint64_t token) const;    // 只找 Loading / Loaded 的
    std::size_t LoadedCount() const;
    const MemoryStats& memory() const { return memory_; }

    // 在已加载 Mod 的环境里执行一段代码（离线测试用），预算 budget_ms；budgetMs 不为 0 时用它（准备测试数据，比如建 100 MB 的串）
    bool RunChunk(Mod& mod, std::string_view code, std::string_view chunkName, std::string* error, bool* budgetExceeded = nullptr,
        std::uint32_t budgetMs = 0);

    // 事件绑定、调度器和 combat 绑定（P4b），VM 存在期间有效
    EventBinding& events() { return *events_; }
    Scheduler& scheduler() { return *scheduler_; }
    CombatBinding& combat() { return *combat_; }
    bool HasRuntime() const { return events_ && scheduler_ && combat_; }
    // 事件绑定还在（combat 绑定析构时 combat_ 已经置空，HasRuntime 为假，撤销内部订阅只看这个，C-N4）
    bool HasEvents() const { return events_ != nullptr; }
    // max_per_mod 的计数：dd.on / dd.once / dd.effect.register、dd.wait_event、带 done 的排队写操作、dd.buff.rule
    std::size_t SubscriptionLoad(const Mod& mod) const;

    // 规则回调（dd.buff.rule）正在执行：只读，写操作和注册类的函数报错
    bool InRule() const { return ruleDepth_ > 0; }
    struct RuleScope {
        explicit RuleScope(Session& session) : session_(session) { ++session_.ruleDepth_; }
        ~RuleScope() { --session_.ruleDepth_; }
        RuleScope(const RuleScope&) = delete;
        RuleScope& operator=(const RuleScope&) = delete;
        Session& session_;
    };

    // 调度用的时钟（秒，单调）。默认 QueryPerformanceCounter，离线测试可以换成假时钟
    double Now() const;
    void SetClock(std::function<double()> clock) { clock_ = std::move(clock); }

    // 每帧调用（lifecycle TICK）：排队的重载、文件监视（开发模式，每 0.5 秒）、定时器、协程
    void Tick();
    std::uint64_t TickCount() const { return ticks_; }

    // 热重载。Request 排到下一次 Tick（dd.reload 总是走这里），Reload 立即执行，不能在 Lua 代码运行时调用。
    // 只有通过了 manifest 和依赖检查的 Mod 能重载（含 main.lua 出错的）
    bool RequestReload(std::string_view id, std::string* error);
    bool Reload(std::string_view id, std::string* error);
    // 扫描 lua/ 目录，有变化（并且连续两轮一致）的 Mod 立即重载；离线测试直接调用（Tick 里改为排队，受帧预算约束）
    void PollWatch();

    // dd.state 的总表（注册表引用，id -> 表）
    int stateRef() const { return stateRef_; }

    // ---- P4c1：存档旁路（dd.store、脚本 Buff 状态随存档保存） ----
    // 按存档的会话由 plugin.cpp 在 Load 之前打开；开发会话的 dd.store 只在内存里，不随存档保存
    void SetPersistent(bool on) { persistent_ = on; }
    bool Persistent() const { return persistent_; }
    // dd.store 的总表（注册表引用，id -> 表）。Load 时在 main.lua 之前从旁路读好，跨重载保留
    int storeRef() const { return storeRef_; }
    struct StoreInfo {
        std::string status = "empty";  // dd.store_status()：loaded / prev / empty / mismatch / unavailable
        std::string reason;
        bool raidPending = false;      // 地牢里的存档：等 DDCore 核对 raid
        bool saveBlocked = false;      // raid 核对不一致后 DDCore 拒绝再写（DD_PERSIST_E_INCONSISTENT），已经记过日志
    };
    const StoreInfo* StoreInfoOf(std::string_view id) const;
    // DDCore 的保存回调（campaign.save 的订阅都派发完之后）：每个加载成功的 Mod 写出 dd.store 和它的脚本 Buff 状态
    void PersistSave(const DDPersistSaveInfo& info);
    // 地牢存档的 raid 核对结果：一致时恢复怪物的脚本 Buff 状态；不一致时状态改成 mismatch，丢弃没恢复的怪物状态
    void PersistVerify(const DDPersistVerify& verify);
    std::uint32_t SaveCount() const { return saveCount_; }
    std::size_t PendingStates() const { return pendingStates_.size(); }

    // 订阅数核对：本会话的记录、DDLua 全部的 SDK 订阅、DDCore 统计的该插件订阅数
    void LogSubscriptionCounts(const char* when) const;

    // 给 Mod 挂一个拆除时执行的清理函数，返回 id
    std::uint64_t AddCleanup(Mod& mod, std::function<void()> fn);

    // 日志：mod 为空时前缀是会话名，多行文本逐行加前缀。
    // 带 Mod 的日志（Mod 自己写的 print、warn、dd.log.*，和 DDLua 替它写的回调错误、traceback、失败原因、协程被手动结束等）
    // 都经过同一个出口（审核 A-C4、第二轮 R2A-应修1）：一条最多 kMaxModEntry 字节；每个 Mod 每秒最多 log_lines_per_sec 行
    // （多行文本按行数算），超出的丢弃，之后（下一次写日志、每秒的检查或拆除时）补一行汇总；一个会话里这些日志的总量
    // 最多 log_max_mb，超出后不再写入，只在拆除时写一行丢弃的行数
    static constexpr std::size_t kMaxModEntry = 16 * 1024;
    void Log(int level, const Mod* mod, std::string_view text) const;
    void ModLog(int level, Mod& mod, std::string_view text) { Log(level, &mod, text); }

    // 现在代表哪个 Mod 执行 Lua 代码：DDLua 从 C++ 进入 Lua 时（main.lua、事件回调、定时器、协程的恢复和关闭）
    // 按入口所属的 Mod 压栈。Mod 直接调用别的 Mod 导出的函数时不变（那段代码代表调用者执行）。
    // 撤销订阅、定时器、协程时按它检查归属（审核 B-N4）
    const Mod* CurrentMod() const { return running_.empty() ? nullptr : running_.back(); }
    struct Running {
        Running(Session& session, const Mod* mod) : session_(session) { session_.running_.push_back(mod); }
        ~Running() { session_.running_.pop_back(); }
        Running(const Running&) = delete;
        Running& operator=(const Running&) = delete;
        Session& session_;
    };

    // VM 损坏（原生异常之后）：写一条错误日志，之后不再进入 Lua（事件回调、定时器、协程、重载），拆除时不碰 VM
    void MarkBroken(std::string_view reason);
    // SEH 过滤器里调用：只置标记、记下位置和异常码（不分配、不写日志），下一次进入 DDLua 时补日志。
    // where 必须是字符串字面量
    void NoteNativeFault(const char* where = "lifecycle 回调", unsigned long code = 0, const void* address = nullptr);

    // 拆除和重载的总时间（第二轮 R2A-备注3）：一次拆除或重载里关闭协程（运行 <close>）的总时间不超过 teardown_budget_ms，
    // 超过后剩下的协程不再关闭，直接丢弃引用（不运行 <close>），结束时写一行 WARN。嵌套时沿用最外层的时钟
    struct TeardownClock {
        TeardownClock(Session& session, const char* what);
        ~TeardownClock();
        TeardownClock(const TeardownClock&) = delete;
        TeardownClock& operator=(const TeardownClock&) = delete;
        Session& session_;
    };
    // 调度器关闭协程之前问一次：还有时间时返回 true；超时返回 false 并计数
    bool CloseAllowed();
    // 进入 DDLua 时的检查：过滤器记下的原生异常补日志；frameBoundary（每帧 Tick、拆除，不可能嵌套在 Lua 调用里）时
    // 还检查有没有调用没返回（Vm::Abandoned），有就标记损坏。返回 VM 是否可用
    bool CheckVmHealth(bool frameBoundary);
    bool Broken() const { return vm_ && vm_->broken(); }
    // 同上，前缀是给定的 id（Mod 可能已经拆除，只留下 id）
    void LogId(int level, std::string_view id, std::string_view text) const;

private:
    bool LoadMod(Mod& mod);
    void TeardownMod(Mod& mod);
    // P4c1：创建 VM 之后、执行 main.lua 之前读旁路（dd.store 解码成表，脚本 Buff 状态留到 main.lua 之后导入）
    void LoadPersisted();
    // 导入还没恢复的脚本 Buff 状态：kind 是 DD_ACTOR_HERO / DD_ACTOR_MONSTER，onlyMod 不为空时只导入这个 Mod 的
    void ImportStates(std::uint32_t kind, const std::string* onlyMod);
    // dd.store 的表压栈：Mod 环境里 dd.store 是表就用它（允许整体赋值），否则用会话里的
    void PushStore(lua_State* L, const Mod& mod);
    bool CreateVm();
    Mod* FindLoadable(std::string_view id) const;
    // 文件监视：变化（连续两轮一致）的 Mod id
    std::vector<std::string> ScanWatch();
    void FlushLogDrops(const Mod& mod) const;
    // 不限速的写入（会话自己的日志），多行文本逐行加前缀
    void Write(int level, std::string_view id, std::string_view text) const;
    // 帧预算顺延的日志（限速：第一次和之后每 10 秒最多一行）
    void NoteDeferred(std::size_t items, double elapsedMs);

    const Host& host_;
    Config config_;
    std::string name_;
    std::unique_ptr<Vm> vm_;
    std::vector<std::unique_ptr<Mod>> mods_;
    std::vector<Mod*> loadOrder_;  // 实际执行过 main.lua 的顺序（拆除按逆序，重载不改变位置）
    std::vector<Mod*> loadable_;   // 通过了 manifest 和依赖检查的 Mod，按加载顺序（重载用）
    std::uint64_t nextToken_ = 1;
    std::uint64_t nextCleanup_ = 1;
    bool tornDown_ = false;
    MemoryStats memory_;

    std::unique_ptr<EventBinding> events_;
    std::unique_ptr<Scheduler> scheduler_;
    std::unique_ptr<CombatBinding> combat_;  // 持有内部订阅，要先于 events_ 销毁
    int ruleDepth_ = 0;
    std::function<double()> clock_;
    std::uint64_t ticks_ = 0;
    struct PendingReload {
        std::string id;
        bool fromWatch = false;  // 文件监视发现的：已经作为别的 Mod 的依赖者重载过的跳过
    };
    std::vector<PendingReload> pendingReloads_;
    std::vector<const Mod*> running_;  // CurrentMod 的栈
    bool faultNoted_ = false;          // SEH 过滤器记下了原生异常（还没写日志）
    const char* faultWhere_ = "";
    unsigned long faultCode_ = 0;
    const void* faultAddress_ = nullptr;
    // 和 Mod 有关的日志的总量（log_max_mb）
    mutable std::size_t modLogBytes_ = 0;
    mutable std::uint64_t capDropped_ = 0;
    mutable bool capNoted_ = false;
    // 拆除和重载的时钟
    int teardownDepth_ = 0;
    double teardownDeadline_ = 0;  // 毫秒（steady_clock）
    std::size_t skippedCloses_ = 0;
    const char* teardownWhat_ = "";
    double nextDeferLog_ = 0;          // 帧预算顺延日志的限速
    std::size_t deferFrames_ = 0;      // 上一行顺延日志之后又有几帧顺延、共几项
    std::size_t deferItems_ = 0;
    double nextWatch_ = 0;
    double nextActorSweep_ = 0;
    int stateRef_ = LUA_NOREF;
    // P4c1
    bool persistent_ = false;
    int storeRef_ = LUA_NOREF;
    std::map<std::string, StoreInfo> storeInfo_;
    struct PendingState {
        std::string mod;
        store::BuffState state;
    };
    std::vector<PendingState> pendingStates_;
    std::uint32_t saveCount_ = 0;
};

}  // namespace ddlua
