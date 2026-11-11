#include "session.h"

#include "actor.h"
#include "sandbox.h"

#include <lauxlib.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>

namespace ddlua {

namespace {

std::string Kb(std::size_t bytes)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    return buffer;
}

// lua/ 目录的修改标记：文件数、总大小、最新的修改时间（目录的修改时间也算，删文件会改它）。
// 不跟联接和符号链接，深度和文件数有上限
void ScanDir(const std::wstring& dir, WatchStamp& stamp, int depth)
{
    if (depth > 8 || stamp.files > 4096) {
        return;
    }
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L".." || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            continue;
        }
        const std::uint64_t written = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
            data.ftLastWriteTime.dwLowDateTime;
        stamp.latest = written > stamp.latest ? written : stamp.latest;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanDir(dir + L"\\" + name, stamp, depth + 1);
        } else {
            ++stamp.files;
            stamp.bytes += (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

WatchStamp ScanLuaDir(const Mod& mod)
{
    WatchStamp stamp;
    if (mod.luaFs.IsOpen()) {
        ScanDir(mod.luaFs.RootFinal(), stamp, 0);
    }
    return stamp;
}

}  // namespace

Session::Session(const Host& host, const Config& config, std::string name)
    : host_(host), config_(config), name_(std::move(name))
{
}

Session::~Session()
{
    Teardown();
}

void Session::Log(int level, const Mod* mod, std::string_view text) const
{
    if (!mod) {
        Write(level, name_, text);
        return;
    }
    // 一条的长度：错误对象、traceback 在 Vm 里已经各自截断，这里是总的兜底
    std::string clipped;
    if (text.size() > kMaxModEntry) {
        clipped = ClipUtf8(text, kMaxModEntry);
        text = clipped;
    }
    // 每个 Mod 每秒的行数
    const double now = Now();
    if (mod->logWindow < 0 || now - mod->logWindow >= 1.0 || now < mod->logWindow) {
        FlushLogDrops(*mod);
        mod->logWindow = now;
        mod->logLines = 0;
    }
    const std::uint32_t limit = config_.logLinesPerSec;
    std::uint32_t lines = 1;
    for (const char ch : text) {
        lines += ch == '\n' ? 1 : 0;
    }
    if (mod->logLines >= limit) {
        mod->logDropped += lines;
        return;
    }
    const std::uint32_t room = limit - mod->logLines;
    if (lines > room) {
        // 多行文本只写到额度为止
        std::size_t cut = 0;
        for (std::uint32_t n = 0; n < room; ++n) {
            cut = text.find('\n', cut) + 1;
        }
        mod->logDropped += lines - room;
        text = text.substr(0, cut > 0 ? cut - 1 : 0);
        lines = room;
    }
    mod->logLines += lines;
    // 会话的总量
    const std::size_t total = static_cast<std::size_t>(config_.logMaxMb) * 1024 * 1024;
    if (modLogBytes_ + text.size() > total) {
        capDropped_ += lines;
        if (!capNoted_) {
            capNoted_ = true;
            Write(DD_LOG_WARN, name_, "和 Mod 有关的日志本会话已写 " + std::to_string(modLogBytes_ / 1024) + " KB，到了 log_max_mb（" +
                std::to_string(config_.logMaxMb) + " MB），之后不再写入，拆除时写出丢弃的行数");
        }
        return;
    }
    modLogBytes_ += text.size();
    Write(level, mod->id(), text);
}

void Session::LogId(int level, std::string_view id, std::string_view text) const
{
    // Mod 可能已经拆除，只留下 id：按 id 找回 Mod 对象（会话期间不删除），找到的走限速
    for (const auto& mod : mods_) {
        if (mod->id() == id) {
            Log(level, mod.get(), text);
            return;
        }
    }
    Write(level, id, text);
}

void Session::Write(int level, std::string_view id, std::string_view text) const
{
    const std::string prefix = "[" + std::string(id) + "] ";
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        host_.Log(level, prefix + std::string(line));
        if (end == text.size()) {
            break;
        }
        start = end + 1;
    }
}

void Session::FlushLogDrops(const Mod& mod) const
{
    if (mod.logDropped == 0) {
        return;
    }
    const std::uint64_t dropped = mod.logDropped;
    mod.logDropped = 0;
    if (modLogBytes_ >= static_cast<std::size_t>(config_.logMaxMb) * 1024 * 1024) {
        capDropped_ += dropped;  // 总量已满，汇总也不写
        return;
    }
    modLogBytes_ += 128;
    Write(DD_LOG_WARN, mod.id(), "日志限速：有 " + std::to_string(dropped) + " 行日志没有写入（每个 Mod 每秒最多 " +
        std::to_string(config_.logLinesPerSec) + " 行，超出的丢弃；DDLua.cfg 的 log_lines_per_sec）");
}

void Session::NoteNativeFault(const char* where, unsigned long code, const void* address)
{
    if (!faultNoted_) {
        faultWhere_ = where;
        faultCode_ = code;
        faultAddress_ = address;
    }
    faultNoted_ = true;
    if (vm_) {
        vm_->MarkBroken();
    }
}

void Session::MarkBroken(std::string_view reason)
{
    if (!vm_) {
        return;
    }
    const bool already = vm_->broken() && !faultNoted_;
    vm_->MarkBroken();
    faultNoted_ = false;
    if (already) {
        return;
    }
    Log(DD_LOG_ERROR, nullptr, "Lua VM 已损坏（" + std::string(reason) + "）：本会话停止一切 Lua 派发（事件回调、定时器、协程、重载），" +
        "退出存档时不再触碰 VM、直接丢弃（Lua 内存约 " + Kb(vm_->MemoryUsed()) + " 不释放），下次进存档新建 VM");
}

bool Session::CheckVmHealth(bool frameBoundary)
{
    if (!vm_) {
        return false;
    }
    if (faultNoted_) {
        char reason[256];
        std::snprintf(reason, sizeof(reason), "%s里执行 Lua 代码时发生了原生异常（SEH，异常码 %08lX，地址 %p）", faultWhere_, faultCode_,
            faultAddress_);
        MarkBroken(reason);
    } else if (frameBoundary && !vm_->broken() && vm_->Abandoned()) {
        MarkBroken("上一次进入 Lua 没有正常返回，多半是执行期间发生了原生异常（SEH），栈已被展开");
    }
    return vm_->ok();
}

namespace {

double MonotonicMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

Session::TeardownClock::TeardownClock(Session& session, const char* what) : session_(session)
{
    if (session_.teardownDepth_++ == 0) {
        session_.teardownDeadline_ = MonotonicMs() + static_cast<double>(session_.config_.teardownBudgetMs);
        session_.skippedCloses_ = 0;
        session_.teardownWhat_ = what;
    }
}

Session::TeardownClock::~TeardownClock()
{
    if (--session_.teardownDepth_ != 0) {
        return;
    }
    const std::size_t skipped = session_.skippedCloses_;
    session_.skippedCloses_ = 0;
    if (skipped == 0) {
        return;
    }
    try {
        session_.Write(DD_LOG_WARN, session_.name_, std::string(session_.teardownWhat_) + "：关闭协程（<close>）的总时间到了 teardown_budget_ms（" +
            std::to_string(session_.config_.teardownBudgetMs) + " 毫秒），剩下 " + std::to_string(skipped) +
            " 个协程没有关闭，直接丢弃（不运行它们的 <close>）");
    } catch (...) {
    }
}

bool Session::CloseAllowed()
{
    if (teardownDepth_ == 0 || MonotonicMs() < teardownDeadline_) {
        return true;
    }
    ++skippedCloses_;
    return false;
}

Mod* Session::FindMod(std::string_view id) const
{
    for (const auto& mod : mods_) {
        if (mod->id() == id && (mod->state == ModState::Loading || mod->state == ModState::Loaded)) {
            return mod.get();
        }
    }
    return nullptr;
}

Mod* Session::FindToken(std::uint64_t token) const
{
    for (const auto& mod : mods_) {
        if (mod->token == token && (mod->state == ModState::Loading || mod->state == ModState::Loaded)) {
            return mod.get();
        }
    }
    return nullptr;
}

std::size_t Session::LoadedCount() const
{
    return static_cast<std::size_t>(std::count_if(mods_.begin(), mods_.end(),
        [](const auto& mod) { return mod->state == ModState::Loaded; }));
}

std::uint64_t Session::AddCleanup(Mod& mod, std::function<void()> fn)
{
    const std::uint64_t id = nextCleanup_++;
    mod.cleanups.push_back({ id, std::move(fn) });
    return id;
}

double Session::Now() const
{
    if (clock_) {
        return clock_();
    }
    static const double frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return static_cast<double>(value.QuadPart) / frequency;
}

bool Session::CreateVm()
{
    const std::size_t limit = static_cast<std::size_t>(config_.memoryLimitMb) * 1024 * 1024;
    vm_ = std::make_unique<Vm>(limit, this);
    std::string error;
    const bool prepared = vm_->ok() && vm_->Protected(
        [&](lua_State* L) {
            PrepareGlobals(L);
            EventBinding::RegisterMetatables(L);
            Scheduler::RegisterMetatables(L);
            actor::RegisterMetatables(L);
            lua_newtable(L);  // dd.state 的总表：id -> 表
            stateRef_ = luaL_ref(L, LUA_REGISTRYINDEX);
        },
        &error);
    if (!prepared) {
        Log(DD_LOG_ERROR, nullptr, "创建 Lua VM 失败：" + (error.empty() ? std::string("lua_newstate 返回空") : error));
        vm_.reset();
        return false;
    }
    events_ = std::make_unique<EventBinding>(*this);
    scheduler_ = std::make_unique<Scheduler>(*this);
    Log(DD_LOG_INFO, nullptr, "Lua VM 已创建（" LUA_RELEASE "，内存上限 " + std::to_string(config_.memoryLimitMb) +
        " MB，预算 " + std::to_string(config_.budgetMs) + " ms，main.lua 预算 " + std::to_string(config_.loadBudgetMs) +
        " ms），基础占用 " + Kb(vm_->MemoryUsed()));
    memory_.baseline = vm_->MemoryUsed();
    return true;
}

void Session::Load(const std::vector<ModSource>& sources)
{
    // 发现：有 <dir>/lua/manifest.json 的才是 Lua Mod
    std::vector<std::unique_ptr<Mod>> found;
    std::size_t scanned = 0;
    for (const auto& source : sources) {
        if (source.dir.empty()) {
            continue;
        }
        ++scanned;
        auto mod = std::make_unique<Mod>();
        mod->label = source.label;
        mod->dir = source.dir;
        std::string error;
        if (!mod->fs.Open(source.dir, &error)) {
            Log(DD_LOG_WARN, nullptr, source.label + "：目录打不开（" + error + "）：" + source.dir);
            continue;
        }
        if (!mod->fs.Exists("lua/manifest.json")) {
            continue;  // 不是 Lua Mod
        }
        std::string text;
        if (!mod->fs.ReadSmall("lua/manifest.json", text, &error)) {
            Log(DD_LOG_ERROR, nullptr, source.label + "：读取 lua/manifest.json 失败（" + error + "），不加载：" + source.dir);
            continue;
        }
        if (!ParseManifest(text, mod->manifest, &error)) {
            Log(DD_LOG_ERROR, nullptr, source.label + "：lua/manifest.json 无效（" + error + "），不加载：" + source.dir);
            continue;
        }
        if (!mod->luaFs.Open(source.dir + "\\lua", &error)) {
            Log(DD_LOG_ERROR, &*mod, "lua 目录打不开（" + error + "），不加载");
            continue;
        }
        mod->token = nextToken_++;
        Log(DD_LOG_INFO, &*mod, "发现 Lua Mod，版本 " + mod->manifest.versionText + "，来源 " + source.label + "，目录 " + source.dir);
        found.push_back(std::move(mod));
    }
    Log(DD_LOG_INFO, nullptr, "检查了 " + std::to_string(scanned) + " 个 Mod 目录，发现 " + std::to_string(found.size()) + " 个 Lua Mod");

    std::vector<Manifest> manifests;
    for (const auto& mod : found) {
        manifests.push_back(mod->manifest);
    }
    const LoadOrder order = ResolveLoadOrder(manifests);
    for (const auto& [index, reason] : order.rejected) {
        found[index]->state = ModState::Failed;
        found[index]->failure = reason;
        Log(DD_LOG_ERROR, found[index].get(), "不加载：" + reason);
    }
    std::string orderText;
    for (const auto index : order.order) {
        orderText += found[index]->id() + " ";
        mods_.push_back(std::move(found[index]));
    }
    for (auto& mod : found) {
        if (mod) {
            mods_.push_back(std::move(mod));
        }
    }
    if (order.order.empty()) {
        Log(DD_LOG_INFO, nullptr, "没有可加载的 Lua Mod，不创建 VM");
        return;
    }
    Log(DD_LOG_INFO, nullptr, "加载顺序：" + orderText);
    if (!CreateVm()) {
        for (auto& mod : mods_) {
            if (mod->state == ModState::Loading) {
                mod->state = ModState::Failed;
                mod->failure = "VM 创建失败";
            }
        }
        return;
    }

    const std::size_t loadCount = order.order.size();
    for (std::size_t i = 0; i < loadCount; ++i) {
        loadable_.push_back(mods_[i].get());
    }
    for (std::size_t i = 0; i < loadCount; ++i) {
        Mod& mod = *mods_[i];
        std::string failedDep;
        for (const auto& dep : mod.manifest.deps) {
            const auto it = std::find_if(mods_.begin(), mods_.end(),
                [&](const auto& other) { return other->id() == dep.id && other->state == ModState::Loaded; });
            if (it == mods_.end()) {
                failedDep = dep.id;
                break;
            }
        }
        if (!failedDep.empty()) {
            mod.state = ModState::Failed;
            mod.failure = "依赖 " + failedDep + " 加载失败";
            Log(DD_LOG_ERROR, &mod, "不加载：" + mod.failure);
            continue;
        }
        LoadMod(mod);
    }
    memory_.loaded = vm_->MemoryUsed();
    Log(DD_LOG_INFO, nullptr, "加载完成：成功 " + std::to_string(LoadedCount()) + " 个，共 " + std::to_string(mods_.size()) +
        " 个 Lua Mod，Lua 内存 " + Kb(vm_->MemoryUsed()));
    LogSubscriptionCounts("加载完成");
}

bool Session::LoadMod(Mod& mod)
{
    if (std::find(loadOrder_.begin(), loadOrder_.end(), &mod) == loadOrder_.end()) {
        loadOrder_.push_back(&mod);  // 重载不改变位置，拆除顺序仍按依赖
    }
    mod.state = ModState::Loading;
    mod.failure.clear();
    mod.tearingDown = false;
    ++mod.loads;
    if (config_.devMode) {
        mod.watch = ScanLuaDir(mod);
        mod.watchPending = false;
    }
    // 订阅、等待者、定时器、协程统一由这个清理项撤销（第一个登记，最后执行）：先撤销订阅，再关闭协程
    AddCleanup(mod, [this, &mod] {
        if (events_) {
            events_->ReleaseMod(mod);
        }
        if (scheduler_) {
            scheduler_->ReleaseMod(mod);
        }
    });
    const std::size_t before = vm_->MemoryUsed();
    std::string error;
    // 环境、require 缓存；main.lua 的读取、语法分析和执行都在 RunMainChunk 里，共用 load_budget_ms 的预算入口
    // （第二轮 R2A-阻断3：原来读取和语法分析在 Protected 里，不受任何预算约束）
    const bool prepared = vm_->Protected(
        [&](lua_State* L) {
            PushModEnv(L, *this, mod);
            mod.envRef = luaL_ref(L, LUA_REGISTRYINDEX);
            lua_newtable(L);
            mod.requireRef = luaL_ref(L, LUA_REGISTRYINDEX);
            lua_pushinteger(L, static_cast<lua_Integer>(mod.token));
            lua_pushcclosure(L, &RunMainChunk, 1);
        },
        &error);
    bool budget = false;
    bool called = false;
    if (prepared) {
        const Running running(*this, &mod);
        called = vm_->Call(0, 1, config_.loadBudgetMs, &error, &budget);
    }
    if (called) {
        // main.lua 的返回值是给 dd.import 用的导出
        if (vm_->Protected([&](lua_State* L) { mod.exportsRef = luaL_ref(L, LUA_REGISTRYINDEX); }, &error, 1)) {
            mod.state = ModState::Loaded;
            const auto subs = events_->CountFor(&mod);
            const auto sched = scheduler_->CountFor(&mod);
            Log(DD_LOG_INFO, &mod, std::string(mod.loads > 1 ? "main.lua 重新执行完毕（第 " + std::to_string(mod.loads) + " 次）" : "main.lua 执行完毕") +
                "，占用 Lua 内存约 " + Kb(vm_->MemoryUsed() > before ? vm_->MemoryUsed() - before : 0) + "；订阅 " +
                std::to_string(subs.subscriptions) + " 个、定时器 " + std::to_string(sched.timers) + " 个、协程 " +
                std::to_string(sched.tasks) + " 个");
            return true;
        }
    }
    mod.state = ModState::Failed;
    mod.failure = error;
    Log(DD_LOG_ERROR, &mod, std::string(budget ? "main.lua 超出预算，加载失败：" : "加载失败：") + error);
    {
        const TeardownClock clock(*this, "加载失败后的拆除");
        TeardownMod(mod);
    }
    mod.state = ModState::Failed;
    return false;
}

void Session::TeardownMod(Mod& mod)
{
    mod.tearingDown = true;
    std::size_t cleaned = 0;
    for (auto it = mod.cleanups.rbegin(); it != mod.cleanups.rend(); ++it) {
        try {
            it->fn();
            ++cleaned;
        } catch (const std::exception& e) {
            Log(DD_LOG_ERROR, &mod, std::string("清理时出错：") + e.what());
        }
    }
    mod.cleanups.clear();
    FlushLogDrops(mod);
    if (vm_ && vm_->ok()) {
        std::string error;
        vm_->Protected(
            [&](lua_State* L) {
                for (int* ref : { &mod.importsRef, &mod.exportsRef, &mod.requireRef, &mod.envRef }) {
                    luaL_unref(L, LUA_REGISTRYINDEX, *ref);
                    *ref = LUA_NOREF;
                }
            },
            &error);
    }
    if (mod.state != ModState::Failed) {
        mod.state = ModState::TornDown;
    }
    Log(DD_LOG_INFO, &mod, "已拆除（清理项 " + std::to_string(cleaned) + " 个）");
}

void Session::Teardown()
{
    if (tornDown_) {
        return;
    }
    tornDown_ = true;
    if (!vm_) {
        return;
    }
    if (!CheckVmHealth(true)) {
        // 损坏的 VM 不再触碰：不运行清理项和 <close>，不释放 Lua 引用，不 GC、不 lua_close。
        // 只撤销 SDK 订阅（C++ 记录，不碰 Lua），然后丢弃 VM（泄漏）
        const std::size_t leaked = vm_->MemoryUsed();
        for (auto& mod : mods_) {
            mod->cleanups.clear();
            if (mod->state == ModState::Loaded || mod->state == ModState::Loading) {
                mod->state = ModState::TornDown;
            }
        }
        scheduler_.reset();
        events_.reset();
        LogSubscriptionCounts("拆除后");
        vm_->Close();
        memory_.before = leaked;
        memory_.after = leaked;
        memory_.residual = leaked;
        Log(DD_LOG_ERROR, nullptr, "Lua VM 已损坏，直接丢弃：撤销了 SDK 订阅，没有运行清理项和 <close>，Lua 内存约 " + Kb(leaked) +
            " 没有释放（泄漏）");
        vm_.reset();
        return;
    }
    const std::size_t before = vm_->MemoryUsed();
    std::size_t count = 0;
    {
        const TeardownClock clock(*this, "拆除");
        for (auto it = loadOrder_.rbegin(); it != loadOrder_.rend(); ++it) {
            if ((*it)->state == ModState::Loaded) {
                TeardownMod(**it);
                ++count;
            }
        }
    }
    // 兜底：撤销 Mod 拆除后还剩下的（不应该有），然后释放 dd.state
    scheduler_.reset();
    events_.reset();
    LogSubscriptionCounts("拆除后");
    if (vm_->ok()) {
        luaL_unref(vm_->L(), LUA_REGISTRYINDEX, stateRef_);
    }
    stateRef_ = LUA_NOREF;
    vm_->FullGc();
    const std::size_t after = vm_->MemoryUsed();
    const std::size_t residual = vm_->Close();
    memory_.before = before;
    memory_.after = after;
    memory_.residual = residual;
    if (capDropped_ > 0) {
        Write(DD_LOG_WARN, name_, "本会话因为 log_max_mb 有 " + std::to_string(capDropped_) + " 行和 Mod 有关的日志没有写入");
    }
    Log(DD_LOG_INFO, nullptr, "拆除 " + std::to_string(count) + " 个 Lua Mod：拆除前 Lua 内存 " + Kb(before) + "，拆除并完整 GC 后 " +
        Kb(after) + "（VM 刚创建时 " + Kb(memory_.baseline) + "），峰值 " + Kb(vm_->MemoryPeak()) + "；VM 已关闭，分配器残留 " + std::to_string(residual) + " 字节");
    vm_.reset();
}

bool Session::RunChunk(Mod& mod, std::string_view code, std::string_view chunkName, std::string* error, bool* budgetExceeded,
    std::uint32_t budgetMs)
{
    if (!vm_ || mod.state != ModState::Loaded) {
        if (error) {
            *error = "Mod 没有加载";
        }
        return false;
    }
    const std::string name(chunkName);
    const bool loaded = vm_->Protected(
        [&](lua_State* L) {
            if (luaL_loadbufferx(L, code.data(), code.size(), name.c_str(), "t") != LUA_OK) {
                lua_error(L);
            }
            lua_rawgeti(L, LUA_REGISTRYINDEX, mod.envRef);
            lua_setupvalue(L, -2, 1);
        },
        error);
    if (!loaded) {
        return false;
    }
    const Running running(*this, &mod);
    return vm_->Call(0, 0, budgetMs ? budgetMs : config_.budgetMs, error, budgetExceeded);
}

Mod* Session::FindLoadable(std::string_view id) const
{
    for (Mod* mod : loadable_) {
        if (mod->id() == id) {
            return mod;
        }
    }
    return nullptr;
}

void Session::Tick()
{
    if (!vm_ || !HasRuntime() || !CheckVmHealth(true)) {
        return;
    }
    ++ticks_;
    FrameBudget frame(config_.frameBudgetMs);
    // 文件监视（开发模式，每 0.5 秒）：发现的变化排进重载队列
    if (config_.devMode && Now() >= nextWatch_) {
        nextWatch_ = Now() + 0.5;
        for (auto& id : ScanWatch()) {
            if (std::none_of(pendingReloads_.begin(), pendingReloads_.end(), [&](const PendingReload& r) { return r.id == id; })) {
                Log(DD_LOG_INFO, nullptr, "文件监视：" + id + " 的 lua/ 有变化，重载");
                pendingReloads_.push_back({ std::move(id), true });
            }
        }
    }
    // 排队的重载：一次一个，帧预算用完的顺延到下一帧
    while (!pendingReloads_.empty()) {
        if (!frame.Allow()) {
            frame.Defer(pendingReloads_.size());
            break;
        }
        const PendingReload reload = pendingReloads_.front();
        pendingReloads_.erase(pendingReloads_.begin());
        const Mod* mod = FindLoadable(reload.id);
        if (reload.fromWatch && mod && mod->watch == mod->pendingWatch) {
            continue;  // 作为别的 Mod 的依赖者已经重载过了
        }
        std::string error;
        if (!Reload(reload.id, &error)) {
            Log(DD_LOG_WARN, nullptr, "重载 " + reload.id + " 失败：" + error);
        }
        if (!vm_->ok()) {
            return;
        }
    }
    // 每秒：actor 作用域的订阅（派发时检查之外，英雄离开名册、怪物在战斗外，没有派发也能撤销），日志限速的汇总
    if (Now() >= nextActorSweep_) {
        nextActorSweep_ = Now() + 1.0;
        events_->SweepActorScopes();
        for (auto& mod : mods_) {
            if (mod->logDropped && Now() - mod->logWindow >= 1.0) {
                FlushLogDrops(*mod);
            }
        }
    }
    scheduler_->Tick(frame);
    if (frame.Deferred() > 0) {
        NoteDeferred(frame.Deferred(), frame.ElapsedMs());
    }
}

void Session::NoteDeferred(std::size_t items, double elapsedMs)
{
    ++deferFrames_;
    deferItems_ += items;
    const double now = Now();
    if (now < nextDeferLog_) {
        return;
    }
    nextDeferLog_ = now + 10.0;
    char used[32];
    std::snprintf(used, sizeof(used), "%.0f", elapsedMs);
    Log(DD_LOG_WARN, nullptr, std::string("调度器：本帧的重载、定时器、协程用了 ") + used + " 毫秒，到了 frame_budget_ms（" +
        std::to_string(config_.frameBudgetMs) + " 毫秒），" + std::to_string(items) + " 项顺延到下一帧" +
        (deferFrames_ > 1 ? "（上一条这类日志之后共 " + std::to_string(deferFrames_) + " 帧顺延、" + std::to_string(deferItems_) + " 项次；" +
                "之后每 10 秒最多写一条）"
                          : "（之后每 10 秒最多写一条）"));
    deferFrames_ = 0;
    deferItems_ = 0;
}

bool Session::RequestReload(std::string_view id, std::string* error)
{
    if (!config_.devMode) {
        *error = "dd.reload 只在开发模式可用（DDLua.cfg 的 dev_mode = 1）";
        return false;
    }
    if (!FindLoadable(id)) {
        *error = "没有可以重载的 Mod：" + std::string(id) + "（没有发现，或没有通过 manifest 和依赖检查）";
        return false;
    }
    const auto queued = std::find_if(pendingReloads_.begin(), pendingReloads_.end(), [&](const PendingReload& r) { return r.id == id; });
    if (queued == pendingReloads_.end()) {
        pendingReloads_.push_back({ std::string(id), false });
        Log(DD_LOG_INFO, nullptr, "请求重载 " + std::string(id) + "，下一帧执行");
    } else {
        queued->fromWatch = false;  // 明确请求的一定重载
    }
    return true;
}

bool Session::Reload(std::string_view id, std::string* error)
{
    if (!vm_ || !vm_->ok() || !HasRuntime()) {
        *error = "VM 不可用";
        return false;
    }
    if (vm_->InEntry()) {
        *error = "Lua 代码正在运行，重载要排到下一帧";
        return false;
    }
    Mod* target = FindLoadable(id);
    if (!target) {
        *error = "没有可以重载的 Mod：" + std::string(id);
        return false;
    }
    // 受影响的：目标和（传递地）依赖它的
    std::vector<Mod*> affected{ target };
    const auto isAffected = [&](const Mod* mod) { return std::find(affected.begin(), affected.end(), mod) != affected.end(); };
    for (bool grew = true; grew;) {
        grew = false;
        for (Mod* mod : loadable_) {
            if (isAffected(mod)) {
                continue;
            }
            for (const auto& dep : mod->manifest.deps) {
                const Mod* depMod = FindLoadable(dep.id);
                if (depMod && isAffected(depMod)) {
                    affected.push_back(mod);
                    grew = true;
                    break;
                }
            }
        }
    }
    const std::size_t before = vm_->MemoryUsed();
    std::size_t torn = 0;
    {
        const TeardownClock clock(*this, "重载");
        for (auto it = loadable_.rbegin(); it != loadable_.rend(); ++it) {
            if (isAffected(*it) && (*it)->state == ModState::Loaded) {
                TeardownMod(**it);
                ++torn;
            }
        }
    }
    vm_->FullGc();
    const std::size_t collected = vm_->MemoryUsed();
    std::size_t attempted = 0;
    std::size_t loaded = 0;
    for (Mod* mod : loadable_) {
        if (!isAffected(mod)) {
            continue;
        }
        std::string failedDep;
        for (const auto& dep : mod->manifest.deps) {
            const Mod* depMod = FindLoadable(dep.id);
            if (!depMod || depMod->state != ModState::Loaded) {
                failedDep = dep.id;
                break;
            }
        }
        if (!failedDep.empty()) {
            mod->state = ModState::Failed;
            mod->failure = "依赖 " + failedDep + " 加载失败";
            Log(DD_LOG_ERROR, mod, "重载：不加载：" + mod->failure);
            continue;
        }
        mod->token = nextToken_++;  // 旧实例的函数从此找不到 Mod
        ++attempted;
        loaded += LoadMod(*mod) ? 1 : 0;
    }
    Log(DD_LOG_INFO, nullptr, "重载 " + std::string(id) + "：拆除 " + std::to_string(torn) + " 个，重新执行 " +
        std::to_string(attempted) + " 个，成功 " + std::to_string(loaded) + " 个；Lua 内存 " + Kb(before) + "，拆除并 GC 后 " +
        Kb(collected) + "，重载后 " + Kb(vm_->MemoryUsed()));
    LogSubscriptionCounts(("重载 " + std::string(id) + " 之后").c_str());
    return true;
}

std::vector<std::string> Session::ScanWatch()
{
    std::vector<std::string> changed;
    for (Mod* mod : loadable_) {
        const WatchStamp stamp = ScanLuaDir(*mod);
        if (stamp == mod->watch) {
            mod->watchPending = false;
            continue;
        }
        if (mod->watchPending && stamp == mod->pendingWatch) {
            changed.push_back(mod->id());  // 连续两轮一致：写完了
        } else {
            mod->pendingWatch = stamp;
            mod->watchPending = true;
        }
    }
    return changed;
}

void Session::PollWatch()
{
    for (const auto& id : ScanWatch()) {
        Mod* mod = FindLoadable(id);
        if (!mod || mod->watch == mod->pendingWatch) {
            continue;  // 作为别的 Mod 的依赖者已经重载过了
        }
        Log(DD_LOG_INFO, nullptr, "文件监视：" + id + " 的 lua/ 有变化，重载");
        std::string error;
        if (!Reload(id, &error)) {
            Log(DD_LOG_WARN, nullptr, "重载 " + id + " 失败：" + error);
        }
    }
}

void Session::LogSubscriptionCounts(const char* when) const
{
    const EventBinding::Counts subs = events_ ? events_->CountFor(nullptr) : EventBinding::Counts{};
    const Scheduler::Counts sched = scheduler_ ? scheduler_->CountFor(nullptr) : Scheduler::Counts{};
    const std::size_t mine = EventBinding::GlobalSdkCount();
    std::string core = "（DDCore 没有提供 subscription_count）";
    bool match = true;
    const DDEventsService* events = host_.events;
    if (events && events->header.size >= offsetof(DDEventsService, subscription_count) + sizeof(void*) &&
        events->subscription_count) {
        const std::uint32_t count = events->subscription_count(host_.plugin);
        core = "，DDCore 统计 DDLua 的订阅 " + std::to_string(count) + " 个";
        match = count == mine;
    }
    Log(match ? DD_LOG_INFO : DD_LOG_ERROR, nullptr, std::string("订阅核对（") + when + "）：本会话 dd.on " +
        std::to_string(subs.subscriptions) + " 个、wait_event " + std::to_string(subs.waiters) + " 个、定时器 " +
        std::to_string(sched.timers) + " 个、协程 " + std::to_string(sched.tasks) + " 个；DDLua 持有 SDK 订阅共 " +
        std::to_string(mine) + " 个" + core + (match ? "" : "，不一致！"));
}

}  // namespace ddlua
