// DDLua.dll：Lua 运行时的 native 插件外壳。只通过 SDK 调用 DDCore（ADR 0003）。
//
// 没有启用 Lua Mod 时只有 lifecycle 订阅（CAMPAIGN_LOAD、CAMPAIGN_UNLOAD，开发用全局加载打开时加 ASSETS_READY），
// 不订阅事件、不装 hook、不创建 VM。有 Lua VM 时才订阅 lifecycle TICK（驱动定时器、协程、重载），
// 事件订阅全部由 Lua Mod 的 dd.on / dd.wait_event 产生。日志写 DDModLoader\log\DDLua.log，每行带 Mod id 或会话名。

#include "config.h"
#include "host.h"
#include "manifest.h"
#include "session.h"

#include <dd/dd_api.h>
#include <lua.h>

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kVersion = DD_VERSION_MAKE(1, 0, 0);

struct Plugin {
    ddlua::Host host;
    ddlua::Config config;
    std::unique_ptr<ddlua::Session> campaign;  // 当前存档的会话
    std::unique_ptr<ddlua::Session> dev;       // 开发用全局会话（ASSETS_READY 创建，保留到进程结束）
    DDSubscriptionId tick = 0;                 // lifecycle TICK，有 VM 时才订阅
};

// 不析构：直接退出游戏时不一定有 CAMPAIGN_UNLOAD，进程退出阶段不碰 VM
Plugin& P()
{
    static auto* plugin = new Plugin();
    return *plugin;
}

// CRT 遇到无效参数时默认终止进程；DDLua 静态链接自己的 CRT，这里只影响 DDLua（含 Lua 库）里的 CRT 调用，
// 让它们返回错误码而不是让游戏崩溃
void __cdecl IgnoreInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {}

std::string ReadTextFile(const std::wstring& path, bool* exists)
{
    *exists = false;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) {
        return {};
    }
    *exists = true;
    std::string text;
    char buffer[4096];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0 && text.size() < 1024 * 1024) {
        text.append(buffer, got);
    }
    std::fclose(file);
    return text;
}

void LoadConfig()
{
    auto& p = P();
    const std::string root = p.host.info->game_root();
    const std::wstring path = ddlua::Utf8ToWide(root) + L"\\DDModLoader\\cfg\\DDLua.cfg";
    bool exists = false;
    const std::string text = ReadTextFile(path, &exists);
    std::vector<std::string> warnings;
    p.config = ddlua::ParseConfig(text, &warnings);
    for (const auto& warning : warnings) {
        p.host.Log(DD_LOG_WARN, "[DDLua] DDLua.cfg：" + warning);
    }
    std::string dirs;
    for (const auto& dir : p.config.devGlobalDirs) {
        dirs += (dirs.empty() ? "" : "; ") + dir;
    }
    p.host.Log(DD_LOG_INFO, std::string("[DDLua] 配置") + (exists ? "（DDLua.cfg）" : "（没有 DDLua.cfg，用默认值）") +
        "：memory_limit_mb=" + std::to_string(p.config.memoryLimitMb) + " budget_ms=" + std::to_string(p.config.budgetMs) +
        " load_budget_ms=" + std::to_string(p.config.loadBudgetMs) + " max_failures=" + std::to_string(p.config.maxFailures) +
        " max_per_mod=" + std::to_string(p.config.maxPerMod) + " frame_budget_ms=" + std::to_string(p.config.frameBudgetMs) +
        " log_lines_per_sec=" + std::to_string(p.config.logLinesPerSec) + " log_max_mb=" + std::to_string(p.config.logMaxMb) +
        " teardown_budget_ms=" + std::to_string(p.config.teardownBudgetMs) +
        " dev_mode=" + (p.config.devMode ? "1" : "0") +
        " dev_global_dirs=" + (dirs.empty() ? "（关闭）" : dirs));
}

// 审核 B-Y3 和主线的决定：lifecycle 回调里执行 Lua 代码（main.lua、定时器、协程、拆除时的 <close>）时发生原生异常
// （SEH），由 DDLua 自己接住：过滤器给正在运行的会话置损坏标记、记下异常码和地址，处理块里恢复栈的保护页（栈溢出时），
// 然后正常返回 DDCore。DDCore 对崩溃的 lifecycle 回调一律撤销订阅且不通知，让异常传到它那里，本次游戏运行就再也收不到
// CampaignLoad（不再加载 Lua Mod）；在这里接住，DDLua 的订阅不受影响，下次进存档照常新建 VM。
// C++ 异常（0xE06D7363）不接，交给外面的 Guarded。展开时中间的 C++ 对象不一定析构（按 /EHsc 编译），
// 所以出事的会话不再碰 VM，拆除时只撤销 SDK 订阅；拆除本身再出事就整个泄漏，不再析构。
// 事件回调里的原生异常仍由 DDCore 接住，经撤销通知（DD_EVENT_END_CRASH）报告，见 events.cpp 的 OnEnd
constexpr unsigned long kCxxException = 0xE06D7363;
ddlua::Session* g_running = nullptr;
unsigned long g_faultCode = 0;
void* g_faultAddress = nullptr;

int FilterNativeFault(EXCEPTION_POINTERS* info)
{
    const unsigned long code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    if (code == kCxxException) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_faultCode = code;
    g_faultAddress = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
    if (g_running) {
        g_running->NoteNativeFault("lifecycle 回调", g_faultCode, g_faultAddress);  // 只置标记，不分配
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// 函数里不能有需要析构的对象（__try 的限制）。接住了原生异常时返回 false
bool RunGuardedRaw(ddlua::Session* session, void (*fn)(void*), void* arg)
{
    g_running = session;
    __try {
        fn(arg);
    } __except (FilterNativeFault(GetExceptionInformation())) {
        if (g_faultCode == EXCEPTION_STACK_OVERFLOW) {
            _resetstkoflw();
        }
        g_running = nullptr;
        return false;
    }
    g_running = nullptr;
    return true;
}

std::string DescribeAddress(const void* address)
{
    HMODULE module = nullptr;
    char path[MAX_PATH] = {};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<const char*>(address), &module) ||
        !module || !GetModuleFileNameA(module, path, MAX_PATH)) {
        return "未知模块";
    }
    const char* slash = std::strrchr(path, '\\');
    char buffer[MAX_PATH + 32];
    std::snprintf(buffer, sizeof(buffer), "%s+0x%llX", slash ? slash + 1 : path,
        static_cast<unsigned long long>(static_cast<const char*>(address) - reinterpret_cast<const char*>(module)));
    return buffer;
}

// 在保护下执行 fn；接住原生异常时写日志、标记会话的 VM 损坏，返回 false
bool RunMarked(ddlua::Session* session, const char* what, void (*fn)(void*), void* arg)
{
    if (RunGuardedRaw(session, fn, arg)) {
        return true;
    }
    char code[96];
    std::snprintf(code, sizeof(code), "异常码 %08lX，地址 %p", g_faultCode, g_faultAddress);
    const std::string where = std::string(code) + "（" + DescribeAddress(g_faultAddress) + "）";
    P().host.Log(DD_LOG_ERROR, std::string("[DDLua] ") + what + " 时发生原生异常（SEH）：" + where +
        "。DDLua 已接住，DDCore 的 lifecycle 订阅不受影响");
    if (session) {
        session->MarkBroken(std::string("lifecycle 回调（") + what + "）里执行时发生了原生异常（SEH，" + code + "），DDLua 已接住");
    }
    return false;
}

template <class Fn>
void Guarded(const char* what, Fn&& fn)
{
    try {
        fn();
    } catch (const std::exception& e) {
        P().host.Log(DD_LOG_ERROR, std::string("[DDLua] ") + what + " 出错：" + e.what());
    } catch (...) {
        P().host.Log(DD_LOG_ERROR, std::string("[DDLua] ") + what + " 出错：未知的 C++ 异常");
    }
}

void TickSession(void* session)
{
    static_cast<ddlua::Session*>(session)->Tick();
}

void TeardownSession(void* session)
{
    static_cast<ddlua::Session*>(session)->Teardown();
}

// 拆除（运行 <close>）放在析构之外、受保护的调用里。拆除中发生原生异常时会话对象整个泄漏（不析构：析构函数还会
// 再拆除一次，状态已经不可信），它持有的 SDK 订阅由损坏 VM 的拆除路径先撤销，撤销不了的回调也只会看到损坏的 VM
void DropSession(std::unique_ptr<ddlua::Session>& session)
{
    if (!session) {
        return;
    }
    if (RunMarked(session.get(), "拆除", &TeardownSession, session.get())) {
        session.reset();
    } else {
        P().host.Log(DD_LOG_ERROR, "[" + session->name() + "] 拆除时发生原生异常，会话对象泄漏，不再触碰");
        session.release();
    }
}

void UpdateTick();

void DD_CALL OnTick(const DDPhaseEvent*, void*)
{
    bool faulted = false;
    Guarded("Tick", [&] {
        auto& p = P();
        if (p.dev) {
            faulted = !RunMarked(p.dev.get(), "Tick", &TickSession, p.dev.get()) || faulted;
        }
        if (p.campaign) {
            faulted = !RunMarked(p.campaign.get(), "Tick", &TickSession, p.campaign.get()) || faulted;
        }
    });
    if (faulted) {
        Guarded("Tick", [] { UpdateTick(); });  // 损坏的 VM 不再需要 TICK
    }
}

// 有会话持有可用的 VM 时订阅 TICK，没有时撤销：没有 Lua Mod 时每帧不进 DDLua
void UpdateTick()
{
    auto& p = P();
    const bool need = (p.campaign && p.campaign->vm() && !p.campaign->Broken()) || (p.dev && p.dev->vm() && !p.dev->Broken());
    if (need && !p.tick) {
        p.tick = p.host.lifecycle->subscribe(p.host.plugin, DD_PHASE_TICK, &OnTick, nullptr);
        p.host.Log(p.tick ? DD_LOG_INFO : DD_LOG_ERROR, p.tick ? "[DDLua] 订阅 lifecycle TICK（有 Lua VM，驱动定时器、协程和重载）"
                                                               : "[DDLua] 订阅 lifecycle TICK 失败，定时器和协程不会运行");
    } else if (!need && p.tick) {
        p.host.lifecycle->unsubscribe(p.host.plugin, p.tick);
        p.tick = 0;
        p.host.Log(DD_LOG_INFO, "[DDLua] 没有 Lua VM 了，撤销 lifecycle TICK");
    }
}

void DD_CALL OnCampaignLoad(const DDPhaseEvent* event, void*)
{
    Guarded("CampaignLoad", [&] {
        auto& p = P();
        if (p.campaign) {
            p.host.Log(DD_LOG_WARN, "[DDLua] 上一个存档的会话没有收到 CampaignUnload，先拆除");
            DropSession(p.campaign);
        }
        const std::string name = "存档#" + std::to_string(event->campaign_serial);
        if (!p.host.game || !p.host.game->mods_available()) {
            p.host.Log(DD_LOG_WARN, "[" + name + "] 启用 Mod 列表不可用（game 服务缺失或特征码不全），不加载 Lua Mod");
            return;
        }
        std::vector<ddlua::ModSource> sources;
        const std::uint32_t count = p.host.game->mod_count();
        std::uint32_t missing = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            DDModEntry entry{};
            entry.size = sizeof(entry);
            if (!p.host.game->mod_at(i, &entry)) {
                continue;
            }
            if (!entry.found) {
                ++missing;
                continue;
            }
            sources.push_back({ std::string(entry.name) + "（" + entry.source + "）", entry.dir });
        }
        p.host.Log(DD_LOG_INFO, "[" + name + "] 进入存档：启用 " + std::to_string(count) + " 个 Mod，" + std::to_string(missing) +
            " 个没有找到目录（见 DDCore.log）");
        p.campaign = std::make_unique<ddlua::Session>(p.host, p.config, name);
        struct LoadArgs {
            ddlua::Session* session;
            const std::vector<ddlua::ModSource>* sources;
        } args{ p.campaign.get(), &sources };
        RunMarked(p.campaign.get(), "CampaignLoad 加载 Lua Mod", [](void* raw) {
            auto* a = static_cast<LoadArgs*>(raw);
            a->session->Load(*a->sources);
        }, &args);
    });
    Guarded("CampaignLoad", [] { UpdateTick(); });
}

void DD_CALL OnCampaignUnload(const DDPhaseEvent* event, void*)
{
    Guarded("CampaignUnload", [&] {
        auto& p = P();
        if (p.campaign) {
            p.host.Log(DD_LOG_INFO, "[" + p.campaign->name() + "] 退出存档（序号 " + std::to_string(event->campaign_serial) + "），拆除");
            DropSession(p.campaign);
        }
    });
    Guarded("CampaignUnload", [] { UpdateTick(); });
}

void DD_CALL OnAssetsReady(const DDPhaseEvent*, void*)
{
    Guarded("AssetsReady", [&] {
        auto& p = P();
        const std::string root = p.host.info->game_root();
        std::vector<ddlua::ModSource> sources;
        for (const auto& dir : p.config.devGlobalDirs) {
            const bool absolute = dir.size() >= 2 && (dir[1] == ':' || (dir[0] == '\\' && dir[1] == '\\'));
            sources.push_back({ "开发目录 " + dir, absolute ? dir : root + "\\" + dir });
        }
        p.host.Log(DD_LOG_WARN, "[开发] 开发用全局加载已打开（DDLua.cfg 的 dev_global_dirs），这不是按存档加载的路径");
        p.dev = std::make_unique<ddlua::Session>(p.host, p.config, "开发");
        struct LoadArgs {
            ddlua::Session* session;
            const std::vector<ddlua::ModSource>* sources;
        } args{ p.dev.get(), &sources };
        RunMarked(p.dev.get(), "AssetsReady 加载开发目录", [](void* raw) {
            auto* a = static_cast<LoadArgs*>(raw);
            a->session->Load(*a->sources);
        }, &args);
    });
    Guarded("AssetsReady", [] { UpdateTick(); });
}

}  // namespace

DD_EXPORT bool DD_CALL DDPlugin_Query(DDPluginInfo* out)
{
    out->abi_version = DD_ABI_VERSION;
    out->name = "DDLua";
    out->version = kVersion;
    return true;
}

DD_EXPORT bool DD_CALL DDPlugin_Load(const DDApi* api)
{
    if (!api || api->size < sizeof(DDApi) || !api->query) {
        return false;
    }
    _set_invalid_parameter_handler(&IgnoreInvalidParameter);

    auto& p = P();
    auto& host = p.host;
    host.plugin = api->plugin;
    host.log = static_cast<const DDLogService*>(api->query(DD_SERVICE_LOG, DD_SERVICE_LOG_VERSION));
    host.info = static_cast<const DDGameInfoService*>(api->query(DD_SERVICE_GAME_INFO, DD_SERVICE_GAME_INFO_VERSION));
    host.lifecycle = static_cast<const DDLifecycleService*>(api->query(DD_SERVICE_LIFECYCLE, DD_SERVICE_LIFECYCLE_VERSION));
    host.game = static_cast<const DDGameService*>(api->query(DD_SERVICE_GAME, DD_SERVICE_GAME_VERSION));
    // events：dd.on / dd.wait_event 用；没有 Lua Mod 时 DDLua 不订阅任何事件主题
    host.events = static_cast<const DDEventsService*>(api->query(DD_SERVICE_EVENTS, DD_SERVICE_EVENTS_VERSION));
    host.task = static_cast<const DDTaskService*>(api->query(DD_SERVICE_TASK, DD_SERVICE_TASK_VERSION));
    if (!host.log || !host.info || !host.lifecycle) {
        return false;
    }
    host.Log(DD_LOG_INFO, "[DDLua] DDLua 1.0.0（" LUA_RELEASE "，按 C++ 编译；Lua API " + std::to_string(ddlua::kLuaApiVersion) +
        "）加载，DDCore " + host.info->core_version());
    if (!host.game) {
        host.Log(DD_LOG_WARN, "[DDLua] DDCore 没有 game 服务，按存档加载不可用");
    }
    LoadConfig();

    bool ok = host.lifecycle->subscribe(host.plugin, DD_PHASE_CAMPAIGN_LOAD, &OnCampaignLoad, nullptr) != 0;
    ok = host.lifecycle->subscribe(host.plugin, DD_PHASE_CAMPAIGN_UNLOAD, &OnCampaignUnload, nullptr) != 0 && ok;
    if (!p.config.devGlobalDirs.empty()) {
        ok = host.lifecycle->subscribe(host.plugin, DD_PHASE_ASSETS_READY, &OnAssetsReady, nullptr) != 0 && ok;
    }
    host.Log(DD_LOG_INFO, std::string("[DDLua] 只订阅了 lifecycle（CampaignLoad、CampaignUnload") +
        (p.config.devGlobalDirs.empty() ? "" : "、AssetsReady") + "），没有订阅事件主题，没有安装 hook");
    return ok;
}
