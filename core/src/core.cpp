#include "core.h"

#include "../../common/pattern.h"
#include "events/access.h"
#include "events/system.h"
#include "game/actors.h"
#include "game/game.h"
#include "game/mods.h"
#include "game/phase_tracker.h"
#include "guard.h"
#include "hook/engine.h"
#include "log.h"
#include "logic/abi.h"
#include "logic/deps.h"
#include "plugin/discovery.h"
#include "runtime/lifecycle.h"
#include "runtime/task_queue.h"
#include "sha1.h"

#include <dd/dd_api.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ddc {

namespace {

constexpr const char kCoreVersion[] = "1.0.0";

enum class PluginState { Pending, Loading, Loaded, Failed, Crashed };

struct PluginRecord {
    std::uint32_t owner = 0;
    std::wstring path;
    std::string utf8Path;
    std::string name;
    std::uint32_t version = 0;
    std::uint32_t abi = 0;
    std::vector<PluginDep> deps;
    HMODULE module = nullptr;
    FARPROC loadProc = nullptr;
    std::atomic<PluginState> state{ PluginState::Pending };  // 服务函数可能在其他线程读取
    LogFile log;
    DDApi api{};
};

struct Core {
    std::wstring gameRoot;
    std::wstring binDir;
    std::wstring logDir;
    std::string utf8GameRoot;
    std::string utf8BinDir;
    std::string utf8ModsDir;
    std::string utf8LogDir;
    std::string exeSha1;
    std::string buildLabel = "unknown";
    std::string svnRevision;
    // 运行标识：本地启动时间加进程 id，写进 DDCore.log 和每个插件日志的第一行，
    // 场景检查脚本用它确认几份日志属于同一次运行（审核 C-S3）
    std::string runId;
    HMODULE exe = nullptr;
    std::uint64_t imageSize = 0;
    DWORD mainThread = 0;

    game::Addresses addr;
    std::unique_ptr<PhaseTracker> tracker;
    hook::Engine hooks;
    TaskQueue tasks;
    std::unique_ptr<Lifecycle> lifecycle;
    std::unique_ptr<events::System> events;
    std::vector<game::EnabledMod> enabledMods;  // CampaignLoad 时读取，CampaignUnload 后清空

    std::mutex registryMutex;
    std::vector<std::unique_ptr<PluginRecord>> plugins;
    std::uint32_t nextOwner = 1;

    bool running = false;
    bool assetsFired = false;
    std::uint64_t frame = 0;
    void(__fastcall* origUpdateState)(void*) = nullptr;
    std::uint64_t frameHookHandle = 0;
};

Core& C()
{
    static auto* core = new Core();  // 不析构：进程退出时序不确定
    return *core;
}

// ---- 工具 ----

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
        nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), 0);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring ToWide(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), 0);
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::uint64_t NowMs()
{
    return GetTickCount64();
}

std::string PluginName(std::uint32_t owner)
{
    auto& c = C();
    std::lock_guard lock(c.registryMutex);
    for (const auto& p : c.plugins) {
        if (p->owner == owner) {
            return p->name;
        }
    }
    return owner == hook::kCoreOwner ? "DDCore" : "?";
}

const char* PhaseName(std::uint32_t phase)
{
    static const char* const kNames[] = { "Early", "AppReady", "AssetsReady", "CampaignLoad", "Tick", "CampaignUnload" };
    return phase < DD_PHASE_COUNT ? kNames[phase] : "?";
}

// 只接受已知且未回滚的句柄
PluginRecord* FromHandle(DDPluginHandle handle)
{
    if (!handle) {
        return nullptr;
    }
    auto& c = C();
    std::lock_guard lock(c.registryMutex);
    for (const auto& p : c.plugins) {
        if (reinterpret_cast<DDPluginHandle>(p.get()) == handle &&
            (p->state == PluginState::Loading || p->state == PluginState::Loaded)) {
            return p.get();
        }
    }
    return nullptr;
}

// ---- SEH 隔离的调用 ----

bool InvokeLifecycleCallback(DDPhaseFn fn, const DDPhaseEvent* event, void* user, std::uint32_t owner)
{
    struct Call {
        DDPhaseFn fn;
        const DDPhaseEvent* event;
        void* user;
    } call{ fn, event, user };
    Fault fault;
    const bool ok = GuardedInvoke(
        [](void* raw) {
            auto* c = static_cast<Call*>(raw);
            c->fn(c->event, c->user);
        },
        &call, &fault);
    if (!ok) {
        LogError("插件 %s 的 %s 回调崩溃：%s，该订阅已取消", PluginName(owner).c_str(), PhaseName(event->phase),
            DescribeFault(fault).c_str());
    }
    return ok;
}

void InvokeTask(DDTaskFn fn, void* user, std::uint32_t owner, std::uint64_t id)
{
    struct Call {
        DDTaskFn fn;
        void* user;
    } call{ fn, user };
    Fault fault;
    const bool ok = GuardedInvoke(
        [](void* raw) {
            auto* c = static_cast<Call*>(raw);
            c->fn(c->user);
        },
        &call, &fault);
    if (!ok) {
        LogError("插件 %s 的任务 %llu 崩溃：%s", PluginName(owner).c_str(), static_cast<unsigned long long>(id),
            DescribeFault(fault).c_str());
    }
}

bool InvokeEventCallback(DDEventFn fn, DDEvent* event, void* user, std::uint32_t owner)
{
    struct Call {
        DDEventFn fn;
        DDEvent* event;
        void* user;
    } call{ fn, event, user };
    Fault fault;
    const bool ok = GuardedInvoke(
        [](void* raw) {
            auto* c = static_cast<Call*>(raw);
            c->fn(c->event, c->user);
        },
        &call, &fault);
    if (!ok) {
        auto* payload = reinterpret_cast<events::Payload*>(event);
        LogError("插件 %s 的事件回调（主题 %s）崩溃：%s，该订阅已取消", PluginName(owner).c_str(),
            payload && payload->topic ? payload->topic->name.c_str() : "?", DescribeFault(fault).c_str());
    }
    return ok;
}

void InvokeEventEnd(DDEventEndFn fn, std::uint64_t id, void* user, std::uint32_t reason, std::uint32_t owner)
{
    struct Call {
        DDEventEndFn fn;
        std::uint64_t id;
        void* user;
        std::uint32_t reason;
    } call{ fn, id, user, reason };
    Fault fault;
    if (!GuardedInvoke(
            [](void* raw) {
                auto* c = static_cast<Call*>(raw);
                c->fn(c->id, c->user, c->reason);
            },
            &call, &fault)) {
        LogError("插件 %s 的订阅撤销通知（订阅 %llu）崩溃：%s", PluginName(owner).c_str(),
            static_cast<unsigned long long>(id), DescribeFault(fault).c_str());
    }
}

bool GuardedQuery(FARPROC proc, DDPluginInfo* info, bool* result, Fault* fault)
{
    struct Call {
        bool(DD_CALL* fn)(DDPluginInfo*);
        DDPluginInfo* info;
        bool result;
    } call{ reinterpret_cast<bool(DD_CALL*)(DDPluginInfo*)>(proc), info, false };
    const bool ok = GuardedInvoke(
        [](void* raw) {
            auto* c = static_cast<Call*>(raw);
            c->result = c->fn(c->info);
        },
        &call, fault);
    *result = call.result;
    return ok;
}

bool GuardedLoad(FARPROC proc, const DDApi* api, bool* result, Fault* fault)
{
    struct Call {
        bool(DD_CALL* fn)(const DDApi*);
        const DDApi* api;
        bool result;
    } call{ reinterpret_cast<bool(DD_CALL*)(const DDApi*)>(proc), api, false };
    const bool ok = GuardedInvoke(
        [](void* raw) {
            auto* c = static_cast<Call*>(raw);
            c->result = c->fn(c->api);
        },
        &call, fault);
    *result = call.result;
    return ok;
}

// ---- 服务实现 ----

// log ----------------------------------------------------------------------

void DD_CALL SvcLogWrite(DDPluginHandle plugin, int level, const char* message)
{
    auto* record = FromHandle(plugin);
    if (!record || !message) {
        return;
    }
    record->log.Write(level, message);
}

// task ---------------------------------------------------------------------

DDTaskId DD_CALL SvcTaskPost(DDPluginHandle plugin, DDTaskFn fn, void* user)
{
    auto* record = FromHandle(plugin);
    if (!record || !fn) {
        return 0;
    }
    return C().tasks.Post(record->owner, fn, user, NowMs(), 0);
}

DDTaskId DD_CALL SvcTaskPostDelayed(DDPluginHandle plugin, DDTaskFn fn, void* user, uint32_t delayMs)
{
    auto* record = FromHandle(plugin);
    if (!record || !fn) {
        return 0;
    }
    return C().tasks.Post(record->owner, fn, user, NowMs(), delayMs);
}

bool DD_CALL SvcTaskCancel(DDPluginHandle plugin, DDTaskId id)
{
    auto* record = FromHandle(plugin);
    return record && C().tasks.Cancel(record->owner, id);
}

bool DD_CALL SvcTaskIsMainThread()
{
    return GetCurrentThreadId() == C().mainThread;
}

// lifecycle ----------------------------------------------------------------

DDSubscriptionId DD_CALL SvcSubscribe(DDPluginHandle plugin, DDPhase phase, DDPhaseFn fn, void* user)
{
    auto* record = FromHandle(plugin);
    auto& c = C();
    if (!record || !fn || !c.lifecycle || static_cast<std::uint32_t>(phase) >= DD_PHASE_COUNT) {
        return 0;
    }
    return c.lifecycle->Subscribe(record->owner, static_cast<std::uint32_t>(phase), fn, user);
}

bool DD_CALL SvcUnsubscribe(DDPluginHandle plugin, DDSubscriptionId id)
{
    auto* record = FromHandle(plugin);
    auto& c = C();
    return record && c.lifecycle && c.lifecycle->Unsubscribe(record->owner, id);
}

uint32_t DD_CALL SvcCurrentPhase()
{
    auto& c = C();
    return c.lifecycle ? c.lifecycle->CurrentPhase() : static_cast<uint32_t>(DD_PHASE_COUNT);
}

bool DD_CALL SvcCampaignActive()
{
    auto& c = C();
    return c.lifecycle && c.lifecycle->CampaignActive();
}

// hook ---------------------------------------------------------------------

const char* DD_CALL SvcHookText(DDHookResult result)
{
    return hook::ResultText(static_cast<hook::Result>(result));
}

// 插件传来的指针在进入 hook 引擎之前先访问一遍：引擎持锁工作，访问冲突不能发生在锁内
struct HookSpecCopy {
    const DDHookSpec* spec;
    std::string name;
    std::uint8_t expected[32];
};

bool CopyHookSpec(HookSpecCopy& copy, Fault* fault)
{
    return GuardedInvoke(
        [](void* raw) {
            auto* c = static_cast<HookSpecCopy*>(raw);
            const DDHookSpec& spec = *c->spec;
            if (spec.name) {
                c->name = spec.name;
            }
            if (spec.kind == DD_HOOK_INLINE && spec.expected && spec.expected_len <= sizeof(c->expected)) {
                std::memcpy(c->expected, spec.expected, spec.expected_len);
            }
            if (spec.original) {
                // 原样写回，只确认可写。真正的写入留在引擎里，要早于 detour 生效
                auto* slot = const_cast<void* volatile*>(spec.original);
                *slot = *slot;
            }
        },
        &copy, fault);
}

// 内联补丁的写入不是原子的：只在主线程、且没有加载线程运行时修改
bool PatchTimingAllowed()
{
    auto& c = C();
    return GetCurrentThreadId() == c.mainThread && !(c.events && c.events->Loading());
}

DDHookResult DD_CALL SvcHookInstall(DDPluginHandle plugin, const DDHookSpec* spec, DDHookHandle* outHandle)
{
    auto* record = FromHandle(plugin);
    // 至少要覆盖当前用到的字段
    constexpr std::uint32_t kHookSpecV1Size = offsetof(DDHookSpec, original) + sizeof(void*);
    if (!record || !spec || spec->size < kHookSpecV1Size) {
        return DD_HOOK_E_BAD_ARGUMENT;
    }
    HookSpecCopy copy{ spec };
    Fault fault;
    if (!CopyHookSpec(copy, &fault)) {
        const std::string text = "hook 安装失败：参数中的指针不可访问（" + DescribeFault(fault) + "）";
        LogWarn("[%s] %s", record->name.c_str(), text.c_str());
        record->log.Write(DD_LOG_WARN, text);
        return DD_HOOK_E_BAD_ARGUMENT;
    }
    const char* name = copy.name.c_str();

    if (spec->kind == DD_HOOK_INLINE && !PatchTimingAllowed()) {
        const std::string text = std::string("hook 安装失败：") + name + "：" + hook::ResultText(hook::Result::BadTiming);
        LogWarn("[%s] %s", record->name.c_str(), text.c_str());
        record->log.Write(DD_LOG_WARN, text);
        return DD_HOOK_E_BAD_TIMING;
    }

    hook::InstallResult result;
    if (spec->kind == DD_HOOK_INLINE) {
        const hook::InlineRequest request{ name, spec->target, spec->expected ? copy.expected : nullptr,
            spec->expected_len, spec->priority, spec->detour, spec->original };
        result = C().hooks.InstallInline(record->owner, request);
    } else if (spec->kind == DD_HOOK_VTABLE) {
        const hook::VtableRequest request{ name, static_cast<void**>(spec->target), spec->expected_value,
            spec->priority, spec->detour, spec->original };
        result = C().hooks.InstallVtable(record->owner, request);
    } else {
        result.result = hook::Result::BadArgument;
        result.detail = "未知的 hook 类型";
    }

    if (result.result == hook::Result::Ok) {
        if (outHandle) {
            *outHandle = result.handle;
        }
        char text[512];
        std::snprintf(text, sizeof(text), "hook 已安装：%s，目标 %p，%s，链深度 %u", name, spec->target,
            result.freshPatch ? "新补丁" : "串入已有链", result.chainDepth);
        LogInfo("[%s] %s", record->name.c_str(), text);
        record->log.Write(DD_LOG_INFO, text);
    } else {
        char text[768];
        std::snprintf(text, sizeof(text), "hook 安装失败：%s，目标 %p：%s（%s）", name, spec->target,
            hook::ResultText(result.result), result.detail.c_str());
        LogWarn("[%s] %s", record->name.c_str(), text);
        record->log.Write(DD_LOG_WARN, text);
    }
    return static_cast<DDHookResult>(result.result);
}

DDHookResult DD_CALL SvcHookRemove(DDPluginHandle plugin, DDHookHandle handle)
{
    auto* record = FromHandle(plugin);
    if (!record) {
        return DD_HOOK_E_BAD_ARGUMENT;
    }
    if (!PatchTimingAllowed()) {
        const std::string text = std::string("hook 移除失败：") + hook::ResultText(hook::Result::BadTiming);
        LogWarn("[%s] %s", record->name.c_str(), text.c_str());
        record->log.Write(DD_LOG_WARN, text);
        return DD_HOOK_E_BAD_TIMING;
    }
    std::string detail;
    const auto result = C().hooks.Remove(record->owner, handle, &detail);
    if (result == hook::Result::Ok && !detail.empty()) {
        LogWarn("[%s] 移除 hook：%s", record->name.c_str(), detail.c_str());
        record->log.Write(DD_LOG_WARN, detail);
    }
    return static_cast<DDHookResult>(result);
}

bool DD_CALL SvcHookScan(const char* pattern, void** out)
{
    if (!pattern || !out) {
        return false;
    }
    std::vector<int> parsed;
    if (!dd::common::ParsePattern(pattern, parsed)) {
        return false;
    }
    const std::uint8_t* text = nullptr;
    std::size_t textSize = 0;
    if (!dd::common::ModuleTextSection(C().exe, &text, &textSize)) {
        return false;
    }
    // 在副本上扫描，并把 DDCore 自己打过补丁的入口还原成原始字节：
    // 先加载的插件 hook 过的函数，后加载的插件要能照样用特征码找到（第三方的补丁不还原）
    std::vector<std::uint8_t> view(text, text + textSize);
    C().hooks.RestoreOriginalBytes(text, view.data(), view.size());
    const std::uint8_t* first = nullptr;
    if (dd::common::ScanPattern(view.data(), view.size(), parsed, &first) != 1) {
        return false;
    }
    *out = const_cast<std::uint8_t*>(text) + (first - view.data());
    return true;
}

// game_info ----------------------------------------------------------------

const char* DD_CALL SvcCoreVersion() { return kCoreVersion; }
const char* DD_CALL SvcExeSha1() { return C().exeSha1.c_str(); }
const char* DD_CALL SvcBuildLabel() { return C().buildLabel.c_str(); }
const char* DD_CALL SvcSvnRevision() { return C().svnRevision.c_str(); }
const char* DD_CALL SvcGameRoot() { return C().utf8GameRoot.c_str(); }
const char* DD_CALL SvcExeDir() { return C().utf8BinDir.c_str(); }
const char* DD_CALL SvcModsDir() { return C().utf8ModsDir.c_str(); }
const char* DD_CALL SvcLogDir() { return C().utf8LogDir.c_str(); }
void* DD_CALL SvcImageBase() { return C().exe; }
uint64_t DD_CALL SvcImageSize() { return C().imageSize; }

const char* DD_CALL SvcPluginPath(DDPluginHandle plugin)
{
    auto* record = FromHandle(plugin);
    return record ? record->utf8Path.c_str() : "";
}

// events -------------------------------------------------------------------

// 把插件传来的选项读出来，指针非法时返回 false。按 size 读认识的字段：不含 on_end 的结构照样读 priority 和 scope
bool ReadEventOptions(const DDEventOptions* options, std::int32_t* priority, std::uint32_t* scope, DDEventEndFn* onEnd)
{
    __try {
        if (options && options->size >= offsetof(DDEventOptions, scope) + sizeof(options->scope)) {
            *priority = options->priority;
            *scope = options->scope;
        }
        if (options && options->size >= offsetof(DDEventOptions, on_end) + sizeof(options->on_end)) {
            *onEnd = options->on_end;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string ReadPluginString(const char* text)
{
    struct Call {
        const char* text;
        std::string out;
    } call{ text, {} };
    Fault fault;
    if (!GuardedInvoke(
            [](void* raw) {
                auto* c = static_cast<Call*>(raw);
                if (c->text) {
                    c->out = c->text;
                }
            },
            &call, &fault)) {
        return {};
    }
    return call.out;
}

DDEventSubId DD_CALL SvcEvSubscribe(DDPluginHandle plugin, const char* topic, DDEventFn fn, void* user,
    const DDEventOptions* options)
{
    auto* record = FromHandle(plugin);
    auto& c = C();
    if (!record || !fn || !c.events) {
        return 0;
    }
    std::int32_t priority = 0;
    std::uint32_t scope = DD_EVENT_SCOPE_PLUGIN;
    DDEventEndFn onEnd = nullptr;
    if (!ReadEventOptions(options, &priority, &scope, &onEnd)) {
        LogWarn("[%s] events.subscribe：options 指针不可访问", record->name.c_str());
        return 0;
    }
    const std::string name = ReadPluginString(topic);
    std::string reason;
    const auto id = c.events->hub().Subscribe(record->owner, name, fn, user, priority, scope, &reason, false, onEnd);
    if (id == 0) {
        const std::string text = "订阅失败：主题 " + name + " —— " + reason;
        LogWarn("[%s] %s", record->name.c_str(), text.c_str());
        record->log.Write(DD_LOG_WARN, text);
    }
    return id;
}

bool DD_CALL SvcEvUnsubscribe(DDPluginHandle plugin, DDEventSubId id)
{
    auto* record = FromHandle(plugin);
    auto& c = C();
    return record && c.events && c.events->hub().Unsubscribe(record->owner, id);
}

void FillTopicInfo(const events::Topic& topic, DDEventTopicInfo* out)
{
    out->name = topic.name.c_str();
    out->kind = static_cast<std::uint32_t>(topic.kind);
    out->field_count = static_cast<std::uint32_t>(topic.fields.size());
    out->writable = topic.writable;
    out->cancellable = topic.cancellable;
    out->available = topic.available;
}

uint32_t DD_CALL SvcEvTopicCount()
{
    auto& c = C();
    return c.events ? c.events->hub().TopicCount() : 0;
}

bool DD_CALL SvcEvTopicAt(uint32_t index, DDEventTopicInfo* out)
{
    auto& c = C();
    if (!c.events || !out) {
        return false;
    }
    auto* topic = c.events->hub().TopicAt(index);
    if (!topic) {
        return false;
    }
    FillTopicInfo(*topic, out);
    return true;
}

bool DD_CALL SvcEvTopicInfo(const char* name, DDEventTopicInfo* out)
{
    auto& c = C();
    if (!c.events || !name || !out) {
        return false;
    }
    auto* topic = c.events->hub().Find(name);
    if (!topic) {
        return false;
    }
    FillTopicInfo(*topic, out);
    return true;
}

bool DD_CALL SvcEvTopicAvailable(const char* name, const char** reason)
{
    auto& c = C();
    if (!c.events || !name) {
        return false;
    }
    auto* topic = c.events->hub().Find(name);
    if (!topic) {
        if (reason) {
            *reason = "主题不存在";
        }
        return false;
    }
    if (reason) {
        *reason = topic->available ? "" : topic->unavailableReason.c_str();
    }
    return topic->available;
}

bool DD_CALL SvcEvFieldAt(const char* name, uint32_t index, DDEventFieldInfo* out)
{
    auto& c = C();
    if (!c.events || !name || !out) {
        return false;
    }
    auto* topic = c.events->hub().Find(name);
    if (!topic || index >= topic->fields.size()) {
        return false;
    }
    const auto& field = topic->fields[index];
    out->name = field.name.c_str();
    out->type = field.type;
    out->access = field.access;
    return true;
}

bool IsReservedTopic(const std::string& name)
{
    static const char* const kReserved[] = { "native.", "query.", "damage.", "effect.", "skill.", "hit.", "stress.",
        "heal.", "battle.", "actor.", "raid.", "town.", "hero.", "inventory.", "estate.", "app.", "campaign.", "spine.",
        "buff." };
    for (const char* prefix : kReserved) {
        if (name.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return name.empty();
}

bool DD_CALL SvcEvRegisterTopic(DDPluginHandle plugin, const char* name, const DDEventFieldInfo* fields, uint32_t count)
{
    auto* record = FromHandle(plugin);
    auto& c = C();
    if (!record || !c.events || (!fields && count)) {
        return false;
    }
    struct Call {
        const char* name;
        const DDEventFieldInfo* fields;
        uint32_t count;
        std::string nameCopy;
        std::vector<events::FieldDesc> copy;
    } call{ name, fields, count, {}, {} };
    Fault fault;
    if (!GuardedInvoke(
            [](void* raw) {
                auto* c = static_cast<Call*>(raw);
                if (c->name) {
                    c->nameCopy = c->name;
                }
                for (uint32_t i = 0; i < c->count; ++i) {
                    events::FieldDesc desc;
                    desc.name = c->fields[i].name ? c->fields[i].name : "";
                    desc.type = static_cast<DDEventFieldType>(c->fields[i].type);
                    desc.access = c->fields[i].access;
                    c->copy.push_back(std::move(desc));
                }
            },
            &call, &fault)) {
        LogWarn("[%s] register_topic：参数不可访问", record->name.c_str());
        return false;
    }
    if (IsReservedTopic(call.nameCopy)) {
        record->log.Write(DD_LOG_WARN, "register_topic 失败：名字为空或使用了保留前缀：" + call.nameCopy);
        return false;
    }
    const bool writable = call.nameCopy.size() >= 4 && call.nameCopy.compare(call.nameCopy.size() - 4, 4, ".pre") == 0;
    for (auto& field : call.copy) {
        if (!writable) {
            field.access &= ~static_cast<std::uint32_t>(DD_FIELD_WRITE);
        }
    }
    auto* topic = c.events->hub().AddTopic(call.nameCopy, events::TopicKind::Custom, call.copy, writable, writable);
    if (!topic) {
        record->log.Write(DD_LOG_WARN, "register_topic 失败：主题已存在：" + call.nameCopy);
        return false;
    }
    return true;
}

bool DD_CALL SvcEvPublish(DDPluginHandle plugin, const char* name, DDEventValue* values, uint32_t count, bool* outCancelled)
{
    auto* record = FromHandle(plugin);
    auto& c = C();
    if (!record || !c.events || GetCurrentThreadId() != c.mainThread || (!values && count)) {
        return false;
    }
    auto& hub = c.events->hub();
    auto* topic = hub.Find(name);
    if (!topic || topic->kind != events::TopicKind::Custom) {
        return false;
    }
    std::vector<events::Slot> slots(topic->fields.size());
    std::vector<std::string> strings;
    strings.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::uint32_t index = 0;
        if (!events::Hub::FindField(topic, values[i].name, &index) || topic->fields[index].type != static_cast<DDEventFieldType>(values[i].type)) {
            record->log.Write(DD_LOG_WARN, "publish：字段不存在或类型不符：" + std::string(values[i].name ? values[i].name : "?"));
            return false;
        }
        auto& slot = slots[index];
        slot.set = true;
        switch (values[i].type) {
        case DD_FIELD_INT:
            slot.v.i = values[i].v.i;
            break;
        case DD_FIELD_FLOAT:
            slot.v.f = values[i].v.f;
            break;
        case DD_FIELD_BOOL:
            slot.v.b = values[i].v.b;
            break;
        case DD_FIELD_STRING:
            strings.push_back(values[i].v.s ? values[i].v.s : "");
            slot.v.s = strings.back().c_str();
            break;
        default:
            slot.v.p = values[i].v.p;
            break;
        }
    }
    events::Payload payload;
    payload.slots = slots.data();
    payload.originThread = GetCurrentThreadId();
    payload.allowWrite = topic->writable;
    payload.flags = topic->phase;
    if (!hub.Dispatch(topic, payload)) {
        return false;
    }
    if (outCancelled) {
        *outCancelled = payload.cancelled;
    }
    if (topic->writable) {
        for (uint32_t i = 0; i < count; ++i) {
            std::uint32_t index = 0;
            if (!events::Hub::FindField(topic, values[i].name, &index) || values[i].type == DD_FIELD_STRING) {
                continue;
            }
            auto& slot = slots[index];
            switch (values[i].type) {
            case DD_FIELD_INT:
                values[i].v.i = slot.v.i;
                break;
            case DD_FIELD_FLOAT:
                values[i].v.f = slot.v.f;
                break;
            case DD_FIELD_BOOL:
                values[i].v.b = slot.v.b;
                break;
            default:
                values[i].v.p = slot.v.p;
                break;
            }
        }
    }
    return true;
}

DDEventResult DD_CALL SvcEvGetInt(const DDEvent* event, const char* field, int64_t* out)
{
    return events::access::GetInt(event, field, out);
}

DDEventResult DD_CALL SvcEvGetFloat(const DDEvent* event, const char* field, double* out)
{
    return events::access::GetFloat(event, field, out);
}

DDEventResult DD_CALL SvcEvGetBool(const DDEvent* event, const char* field, bool* out)
{
    return events::access::GetBool(event, field, out);
}

DDEventResult DD_CALL SvcEvGetString(const DDEvent* event, const char* field, const char** out)
{
    return events::access::GetString(event, field, out);
}

DDEventResult DD_CALL SvcEvGetPointer(const DDEvent* event, const char* field, void** out)
{
    return events::access::GetPointer(event, field, out);
}

DDEventResult DD_CALL SvcEvSetInt(DDEvent* event, const char* field, int64_t value)
{
    return events::access::SetInt(event, field, value);
}

DDEventResult DD_CALL SvcEvSetFloat(DDEvent* event, const char* field, double value)
{
    return events::access::SetFloat(event, field, value);
}

DDEventResult DD_CALL SvcEvSetBool(DDEvent* event, const char* field, bool value)
{
    return events::access::SetBool(event, field, value);
}

DDEventResult DD_CALL SvcEvSetString(DDEvent* event, const char* field, const char* value)
{
    return events::access::SetString(event, field, value);
}

DDEventResult DD_CALL SvcEvSetPointer(DDEvent* event, const char* field, void* value)
{
    return events::access::SetPointer(event, field, value);
}

DDEventResult DD_CALL SvcEvCancel(DDEvent* event)
{
    return events::access::Cancel(event);
}

uint32_t DD_CALL SvcEvFlags(const DDEvent* event)
{
    return events::access::Flags(event);
}

const char* DD_CALL SvcEvTopic(const DDEvent* event)
{
    return events::access::TopicName(event);
}

uint32_t DD_CALL SvcEvOriginThread(const DDEvent* event)
{
    return events::access::OriginThread(event);
}

uint32_t DD_CALL SvcEvSubscriptionCount(DDPluginHandle plugin)
{
    auto* record = FromHandle(plugin);
    auto& c = C();
    return record && c.events ? static_cast<uint32_t>(c.events->hub().OwnerCount(record->owner)) : 0;
}

// game ---------------------------------------------------------------------

bool DD_CALL SvcGameModsAvailable()
{
    auto& c = C();
    return GetCurrentThreadId() == c.mainThread && c.addr.userServices && c.addr.appliedUgcVector;
}

uint32_t DD_CALL SvcGameModCount()
{
    auto& c = C();
    return GetCurrentThreadId() == c.mainThread ? static_cast<uint32_t>(c.enabledMods.size()) : 0;
}

bool DD_CALL SvcGameModAt(uint32_t index, DDModEntry* out)
{
    auto& c = C();
    if (GetCurrentThreadId() != c.mainThread || !out || out->size < sizeof(DDModEntry) || index >= c.enabledMods.size()) {
        return false;
    }
    const auto& mod = c.enabledMods[index];
    out->name = mod.name.c_str();
    out->source = mod.source.c_str();
    out->dir = mod.dir.c_str();
    out->found = mod.found;
    return true;
}

// 取样当前战斗，维护战斗序号（审核 B-B1）。每帧的状态机取样和每次 actor 服务调用时各一次：
// battle.start 回调里的服务调用发生在下一次每帧取样之前，靠服务调用这一次拿到新序号
void ObserveBattle(const char* where)
{
    auto& c = C();
    if (!c.tracker || !c.addr.raidCurrent) {
        return;
    }
    const BattleSample sample = game::ReadBattleSample(c.addr.raidCurrent);
    if (c.tracker->ObserveBattle(sample)) {
        LogInfo("新战斗：战斗序号 %u（探索序号 %u，状态 %d，怪物 %u 个，guid 计数 %u，帧 %llu，%s发现）", c.tracker->BattleSerial(),
            c.tracker->RaidSerial(), sample.state, sample.monsters, sample.guidCounter, static_cast<unsigned long long>(c.frame), where);
    }
}

game::ActorEnv ActorEnvNow()
{
    auto& c = C();
    ObserveBattle("actor 服务调用时");
    game::ActorEnv env;
    env.heroVtable = c.addr.heroVtable;
    env.monsterVtable = c.addr.monsterVtable;
    env.campaignCurrent = c.addr.campaignCurrent;
    env.raidCurrent = c.addr.raidCurrent;
    env.maxStat = c.addr.actorMaxStat;
    if (c.tracker) {
        env.campaignActive = c.tracker->CampaignActive();
        env.campaignSerial = c.tracker->CampaignSerial();
        env.raidSerial = c.tracker->RaidSerial();
        env.battleSerial = c.tracker->BattleSerial();
    }
    return env;
}

bool OnMainThread()
{
    return GetCurrentThreadId() == C().mainThread;
}

uint32_t DD_CALL SvcGameActorKinds()
{
    return OnMainThread() ? game::ActorKinds(ActorEnvNow()) : 0;
}

uint32_t DD_CALL SvcGameRaidSerial()
{
    auto& c = C();
    return OnMainThread() && c.tracker ? c.tracker->RaidSerial() : 0;
}

bool DD_CALL SvcGameActorFromPointer(const void* actor, DDActorRef* out)
{
    return OnMainThread() && game::ActorFromPointer(ActorEnvNow(), actor, out);
}

bool DD_CALL SvcGameActorFind(uint32_t kind, uint32_t id, DDActorRef* out)
{
    return OnMainThread() && game::ActorFind(ActorEnvNow(), kind, id, out);
}

void* DD_CALL SvcGameActorResolve(const DDActorRef* ref)
{
    return OnMainThread() && ref ? game::ActorResolve(ActorEnvNow(), *ref) : nullptr;
}

bool DD_CALL SvcGameActorInfo(const DDActorRef* ref, DDActorInfo* out)
{
    return OnMainThread() && ref && game::ActorReadInfo(ActorEnvNow(), *ref, out);
}

uint32_t DD_CALL SvcGameBattleSerial()
{
    return OnMainThread() ? ActorEnvNow().battleSerial : 0;
}

const DDGameService kGameService = { { sizeof(DDGameService), DD_SERVICE_GAME_VERSION }, &SvcGameModsAvailable,
    &SvcGameModCount, &SvcGameModAt, &SvcGameActorKinds, &SvcGameRaidSerial, &SvcGameActorFromPointer, &SvcGameActorFind,
    &SvcGameActorResolve, &SvcGameActorInfo, &SvcGameBattleSerial };

// 进入存档时读一次启用 Mod 列表，复制成自己的字符串
void ReadEnabledModList()
{
    auto& c = C();
    c.enabledMods.clear();
    if (!c.addr.userServices || !c.addr.appliedUgcVector) {
        LogWarn("启用 Mod 列表不可用：特征码 UserServices / AppliedUgcVector 缺失");
        return;
    }
    auto result = game::ReadEnabledMods(c.addr.userServices, c.addr.appliedUgcVector, c.utf8GameRoot);
    if (!result.ok) {
        LogWarn("启用 Mod 列表读取失败：%s", result.error.c_str());
        return;
    }
    std::size_t found = 0;
    for (const auto& mod : result.mods) {
        found += mod.found ? 1 : 0;
    }
    LogInfo("启用 Mod 列表 %zu 项（已安装 %zu 个），找到目录 %zu 项", result.mods.size(), result.installedCount, found);
    for (std::size_t i = 0; i < result.mods.size(); ++i) {
        const auto& mod = result.mods[i];
        if (!mod.found) {
            LogInfo("  [%zu] name='%s' source='%s' 没有找到目录%s%s", i, mod.name.c_str(), mod.source.c_str(),
                mod.rawPath.empty() ? "（未安装）" : "，记录的路径 ", mod.rawPath.c_str());
        }
    }
    c.enabledMods = std::move(result.mods);
}

const DDLogService kLogService = { { sizeof(DDLogService), DD_SERVICE_LOG_VERSION }, &SvcLogWrite };

const DDTaskService kTaskService = { { sizeof(DDTaskService), DD_SERVICE_TASK_VERSION }, &SvcTaskPost,
    &SvcTaskPostDelayed, &SvcTaskCancel, &SvcTaskIsMainThread };

const DDLifecycleService kLifecycleService = { { sizeof(DDLifecycleService), DD_SERVICE_LIFECYCLE_VERSION },
    &SvcSubscribe, &SvcUnsubscribe, &SvcCurrentPhase, &SvcCampaignActive };

const DDHookService kHookService = { { sizeof(DDHookService), DD_SERVICE_HOOK_VERSION }, &SvcHookInstall,
    &SvcHookRemove, &SvcHookText, &SvcHookScan };

const DDGameInfoService kGameInfoService = { { sizeof(DDGameInfoService), DD_SERVICE_GAME_INFO_VERSION },
    &SvcCoreVersion, &SvcExeSha1, &SvcBuildLabel, &SvcSvnRevision, &SvcGameRoot, &SvcExeDir, &SvcModsDir, &SvcLogDir,
    &SvcPluginPath, &SvcImageBase, &SvcImageSize };

const DDEventsService kEventsService = { { sizeof(DDEventsService), DD_SERVICE_EVENTS_VERSION }, &SvcEvSubscribe,
    &SvcEvUnsubscribe, &SvcEvTopicCount, &SvcEvTopicAt, &SvcEvTopicInfo, &SvcEvTopicAvailable, &SvcEvFieldAt,
    &SvcEvRegisterTopic, &SvcEvPublish, &SvcEvGetInt, &SvcEvGetFloat, &SvcEvGetBool, &SvcEvGetString, &SvcEvGetPointer,
    &SvcEvSetInt, &SvcEvSetFloat, &SvcEvSetBool, &SvcEvSetString, &SvcEvSetPointer, &SvcEvCancel, &SvcEvFlags,
    &SvcEvTopic, &SvcEvOriginThread, &SvcEvSubscriptionCount };

const void* DD_CALL ServiceQuery(const char* service, uint32_t minVersion)
{
    if (!service) {
        return nullptr;
    }
    const struct {
        const char* name;
        const void* table;
        std::uint32_t version;
    } kServices[] = {
        { DD_SERVICE_LOG, &kLogService, DD_SERVICE_LOG_VERSION },
        { DD_SERVICE_TASK, &kTaskService, DD_SERVICE_TASK_VERSION },
        { DD_SERVICE_LIFECYCLE, &kLifecycleService, DD_SERVICE_LIFECYCLE_VERSION },
        { DD_SERVICE_HOOK, &kHookService, DD_SERVICE_HOOK_VERSION },
        { DD_SERVICE_GAME_INFO, &kGameInfoService, DD_SERVICE_GAME_INFO_VERSION },
        { DD_SERVICE_EVENTS, &kEventsService, DD_SERVICE_EVENTS_VERSION },
        { DD_SERVICE_GAME, &kGameService, DD_SERVICE_GAME_VERSION },
    };
    for (const auto& entry : kServices) {
        if (std::strcmp(entry.name, service) == 0) {
            if (entry.table == &kEventsService && !C().events) {
                return nullptr;  // 事件系统没有初始化成功
            }
            return minVersion <= entry.version ? entry.table : nullptr;
        }
    }
    return nullptr;  // 未实现的服务
}

// ---- hook 报告 ----

void HookReport(const char* title)
{
    auto& c = C();
    const auto targets = c.hooks.Snapshot();
    LogInfo("======== hook 报告（%s），共 %zu 个目标 ========", title, targets.size());
    int index = 0;
    for (const auto& target : targets) {
        ++index;
        const auto log = target.intact ? &LogInfo : &LogWarn;  // 补丁失效要能用 WARN 搜到
        log("#%d %s %s 偷取 %u 字节，状态 %s", index, target.isInline ? "内联" : "虚表",
            DescribeAddress(target.address).c_str(), target.stealLength,
            target.orphaned ? "已停用（恢复不了原始字节，只保留直通）" : (target.intact ? "正常" : "补丁已被他人覆盖！"));
        for (const auto& node : target.chain) {
            LogInfo("    %s: %s（优先级 %d）", PluginName(node.owner).c_str(), node.name.c_str(), node.priority);
        }
    }
    if (c.events) {
        LogInfo("%s", c.events->HookReportSummary().c_str());
    }
    LogInfo("======== 报告结束 ========");
}

// ---- 每帧逻辑 ----

void OnFrameBody()
{
    auto& c = C();
    ++c.frame;

    GameSample sample;
    sample.campaign = c.addr.campaignCurrent ? *c.addr.campaignCurrent : nullptr;
    sample.nextState = *c.addr.stateNext;
    sample.lastState = *c.addr.stateLast;
    const std::uint32_t raidBefore = c.tracker->RaidSerial();
    const PhaseEvents events = c.tracker->Step(sample);
    const std::uint32_t serial = c.tracker->CampaignSerial();
    if (c.tracker->RaidSerial() != raidBefore) {
        LogInfo("进入 as_raid_start（新建 Raid），探索序号 %u，帧 %llu", c.tracker->RaidSerial(),
            static_cast<unsigned long long>(c.frame));
    }
    ObserveBattle("每帧取样时");

    if (events.assetsReady) {
        LogInfo("阶段 AssetsReady，帧 %llu，线程 %lu", static_cast<unsigned long long>(c.frame), GetCurrentThreadId());
        c.assetsFired = true;
        c.lifecycle->Fire(DD_PHASE_ASSETS_READY, c.frame, serial);
        if (c.events) {
            c.events->LogOrderSummary("AssetsReady");
        }
        // 再核对一次补丁是否还在。旧式 DLL 这时不一定加载完，所以 CampaignLoad 还会报告
        HookReport("AssetsReady");
    }
    if (events.campaignUnload) {
        LogInfo("阶段 CampaignUnload，帧 %llu，存档序号 %u，线程 %lu%s",
            static_cast<unsigned long long>(c.frame), serial, GetCurrentThreadId(),
            events.unloadAfterDestroy ? "（销毁之后才发现，异常路径）" : "");
        c.lifecycle->SetCampaignActive(false);
        c.lifecycle->Fire(DD_PHASE_CAMPAIGN_UNLOAD, c.frame, serial);
        if (c.events) {
            c.events->OnCampaignUnload(serial);
        }
        c.enabledMods.clear();
    }
    if (events.campaignLoad) {
        void* buff = c.addr.buffLibrary ? *c.addr.buffLibrary : nullptr;
        LogInfo("阶段 CampaignLoad，帧 %llu，存档序号 %u，线程 %lu，Campaign=%p，Buff::Library=%p，状态 last=%d next=%d%s",
            static_cast<unsigned long long>(c.frame), serial, GetCurrentThreadId(), sample.campaign, buff,
            sample.lastState, sample.nextState, events.loadWasFallback ? "（兜底路径）" : "");
        ReadEnabledModList();
        {
            const std::uint32_t kinds = game::ActorKinds(ActorEnvNow());
            LogInfo("actor 句柄：英雄%s，怪物%s，最大生命%s；探索序号 %u，战斗序号 %u", (kinds & (1u << DD_ACTOR_HERO)) ? "可用" : "不可用",
                (kinds & (1u << DD_ACTOR_MONSTER)) ? "可用" : "不可用", c.addr.actorMaxStat ? "可用" : "不可用（特征码 ActorMaxStat 缺失）",
                c.tracker->RaidSerial(), c.tracker->BattleSerial());
        }
        c.lifecycle->SetCampaignActive(true);
        c.lifecycle->Fire(DD_PHASE_CAMPAIGN_LOAD, c.frame, serial);
        if (c.events) {
            c.events->OnCampaignLoad(serial);
        }
        HookReport("CampaignLoad");
    }

    c.lifecycle->ProcessLate(c.frame, serial);
    c.tasks.Drain(NowMs(), &InvokeTask);
    if (c.events) {
        c.events->FrameBoundary(sample, c.frame, serial, c.assetsFired);
    }

    if (c.assetsFired) {
        c.lifecycle->Fire(DD_PHASE_TICK, c.frame, serial);
    }
}

void __fastcall UpdateStateDetour(void* app)
{
    auto& c = C();
    if (c.running) {
        Fault fault;
        if (!GuardedInvoke([](void*) { OnFrameBody(); }, nullptr, &fault)) {
            static bool logged = false;  // 每帧崩溃时只记一次，避免刷屏
            if (!logged) {
                logged = true;
                LogError("每帧逻辑崩溃：%s", DescribeFault(fault).c_str());
            }
        }
    }
    c.origUpdateState(app);
}

// ---- 插件加载和回滚 ----

void Rollback(PluginRecord& record)
{
    auto& c = C();
    const auto lifecycleRemoved = c.lifecycle ? c.lifecycle->RemoveOwner(record.owner) : 0;
    const auto tasksRemoved = c.tasks.RemoveOwner(record.owner);
    std::vector<std::string> details;
    const auto eventsRemoved = c.events ? c.events->hub().RemoveOwner(record.owner) : 0;
    const auto hooksRemoved = c.hooks.RemoveOwner(record.owner, &details);
    LogInfo("回滚插件 %s：移除回调 %zu 个、任务 %zu 个、事件订阅 %zu 个、hook %zu 个", record.name.c_str(), lifecycleRemoved,
        tasksRemoved, eventsRemoved, hooksRemoved);
    for (const auto& detail : details) {
        LogInfo("  %s", detail.c_str());
    }
}

bool LoadOnePlugin(PluginRecord& record)
{
    auto& c = C();
    record.api.size = sizeof(DDApi);
    record.api.abi_version = DD_ABI_VERSION;
    record.api.query = &ServiceQuery;
    record.api.plugin = reinterpret_cast<DDPluginHandle>(&record);

    record.state = PluginState::Loading;
    bool result = false;
    Fault fault;
    if (!GuardedLoad(record.loadProc, &record.api, &result, &fault)) {
        record.state = PluginState::Crashed;
        LogError("插件 %s 的 DDPlugin_Load 崩溃：%s。回滚并隔离，模块保持映射但不再调用", record.name.c_str(),
            DescribeFault(fault).c_str());
        record.log.Write(DD_LOG_ERROR, "DDPlugin_Load 崩溃，已被隔离");
        Rollback(record);
        return false;
    }
    if (!result) {
        record.state = PluginState::Failed;
        LogWarn("插件 %s 的 DDPlugin_Load 返回失败，回滚已注册的回调和 hook", record.name.c_str());
        record.log.Write(DD_LOG_WARN, "DDPlugin_Load 返回 false，已回滚");
        Rollback(record);
        return false;
    }
    record.state = PluginState::Loaded;
    LogInfo("插件 %s 加载成功（版本 %u.%u.%u）", record.name.c_str(), record.version >> 24,
        (record.version >> 12) & 0xFFF, record.version & 0xFFF);
    return true;
}

void LoadPlugins(const std::vector<PluginCandidate>& candidates)
{
    auto& c = C();
    std::vector<PluginRecord*> queried;

    for (const auto& candidate : candidates) {
        const std::string utf8Path = ToUtf8(candidate.path);
        LogInfo("加载插件 %s", utf8Path.c_str());

        // LOAD_WITH_ALTERED_SEARCH_PATH 让插件的依赖从它自己的目录解析，不碰全局的 SetDllDirectory，
        // 旧式加载线程可能正在用 SetDllDirectory。LoadLibrary 放在 SEH 里，防止插件的 DllMain 崩溃
        struct LoadCall {
            const wchar_t* path;
            HMODULE module;
            DWORD error;
        } loadCall{ candidate.path.c_str(), nullptr, 0 };
        Fault loadFault;
        const bool loadOk = GuardedInvoke(
            [](void* raw) {
                auto* c = static_cast<LoadCall*>(raw);
                c->module = LoadLibraryExW(c->path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                if (!c->module) {
                    c->error = GetLastError();
                }
            },
            &loadCall, &loadFault);
        if (!loadOk) {
            LogError("加载插件 %s 时 DllMain 崩溃：%s", utf8Path.c_str(), DescribeFault(loadFault).c_str());
            continue;
        }
        HMODULE module = loadCall.module;
        if (!module) {
            LogError("插件加载失败（LoadLibrary 错误码 %lu）：%s", loadCall.error, utf8Path.c_str());
            continue;
        }

        FARPROC queryProc = GetProcAddress(module, "DDPlugin_Query");
        FARPROC loadProc = GetProcAddress(module, "DDPlugin_Load");
        if (!queryProc || !loadProc) {
            LogError("插件缺少必要导出（Query/Load）：%s", utf8Path.c_str());
            FreeLibrary(module);
            continue;
        }

        // 多留空间，容纳未来版本插件结构里追加的字段
        alignas(DDPluginInfo) char buffer[512] = {};
        auto* info = reinterpret_cast<DDPluginInfo*>(buffer);
        info->size = sizeof(DDPluginInfo);

        bool queryResult = false;
        Fault fault;
        if (!GuardedQuery(queryProc, info, &queryResult, &fault)) {
            LogError("插件 %s 的 DDPlugin_Query 崩溃：%s。已跳过", utf8Path.c_str(), DescribeFault(fault).c_str());
            continue;  // 不 FreeLibrary：崩溃后插件状态未知
        }
        if (!queryResult) {
            LogError("插件 %s 的 DDPlugin_Query 返回失败，已跳过", utf8Path.c_str());
            FreeLibrary(module);
            continue;
        }

        // 校验和复制都在 SEH 里：Query 写进来的指针可能是坏的
        struct ValidateCall {
            DDPluginInfo* info;
            std::string reason;
            std::string name;
            std::vector<PluginDep> deps;
            bool ok;
        } validateCall{ info, {}, {}, {}, false };
        Fault validateFault;
        const bool validateOk = GuardedInvoke(
            [](void* raw) {
                auto* c = static_cast<ValidateCall*>(raw);
                if (!ValidatePluginInfo(*c->info, DD_ABI_VERSION, &c->reason)) {
                    return;
                }
                c->name = c->info->name;
                for (std::uint32_t i = 0; i < c->info->dep_count; ++i) {
                    c->deps.push_back({ c->info->deps[i].name, c->info->deps[i].min_version });
                }
                c->ok = true;
            },
            &validateCall, &validateFault);
        if (!validateOk) {
            LogError("拒绝加载 %s：DDPlugin_Query 写入的结构无效（%s）", utf8Path.c_str(), DescribeFault(validateFault).c_str());
            continue;
        }
        if (!validateCall.ok) {
            LogError("拒绝加载 %s：%s", utf8Path.c_str(), validateCall.reason.c_str());
            FreeLibrary(module);
            continue;
        }

        const std::string name = validateCall.name;
        bool duplicate = false;
        {
            std::lock_guard lock(c.registryMutex);
            for (const auto& existing : c.plugins) {
                if (existing->name == name && existing->state != PluginState::Failed &&
                    existing->state != PluginState::Crashed) {
                    duplicate = true;
                    break;
                }
            }
        }
        if (duplicate) {
            LogError("拒绝加载 %s：插件名 %s 已被其他 DLL 使用", utf8Path.c_str(), name.c_str());
            FreeLibrary(module);
            continue;
        }

        auto record = std::make_unique<PluginRecord>();
        record->owner = c.nextOwner++;
        record->path = candidate.path;
        record->utf8Path = utf8Path;
        record->name = name;
        record->version = info->version;
        record->abi = info->abi_version;
        record->module = module;
        record->loadProc = loadProc;
        record->deps = std::move(validateCall.deps);
        record->log.Open(c.logDir + L"\\" + ToWide(name) + L".log");
        record->log.Write(DD_LOG_INFO, "日志开始，DDCore " + std::string(kCoreVersion) + "，运行标识 " + c.runId + "，插件路径 " + utf8Path);

        auto* raw = record.get();
        {
            std::lock_guard lock(c.registryMutex);
            c.plugins.push_back(std::move(record));
        }
        queried.push_back(raw);
    }

    // 依赖排序
    std::vector<PluginNode> nodes;
    for (const auto* record : queried) {
        nodes.push_back({ record->name, record->version, record->deps });
    }
    const DepResult deps = ResolveDependencies(nodes);
    for (const auto& rejected : deps.rejected) {
        auto* record = queried[rejected.index];
        record->state = PluginState::Failed;
        LogError("拒绝加载插件 %s：%s", record->name.c_str(), rejected.reason.c_str());
        record->log.Write(DD_LOG_ERROR, "加载被拒绝：" + rejected.reason);
    }

    std::string orderText;
    for (std::size_t index : deps.order) {
        orderText += queried[index]->name + " ";
    }
    LogInfo("加载顺序：%s", orderText.empty() ? "（无）" : orderText.c_str());

    for (std::size_t index : deps.order) {
        auto* record = queried[index];
        // 依赖在本轮可能已经失败（Load 崩溃或返回 false）
        std::string failedDep;
        for (const auto& dep : record->deps) {
            bool ok = false;
            for (const auto* other : queried) {
                if (other->name == dep.name && other->state == PluginState::Loaded) {
                    ok = true;
                    break;
                }
            }
            if (!ok) {
                failedDep = dep.name;
                break;
            }
        }
        if (!failedDep.empty()) {
            record->state = PluginState::Failed;
            LogError("拒绝加载插件 %s：依赖 %s 没有加载成功", record->name.c_str(), failedDep.c_str());
            record->log.Write(DD_LOG_ERROR, "依赖 " + failedDep + " 加载失败");
            continue;
        }
        LoadOnePlugin(*record);
    }
}

bool InstallFrameHook()
{
    auto& c = C();
    std::vector<std::uint8_t> expected;
    if (!game::HookEntryBytes("UpdateStateInternal", expected)) {
        LogError("每帧 hook 的入口字节无法从特征码得出");
        return false;
    }
    hook::InlineRequest request{};
    request.name = "per_frame(UpdateStateInternal)";
    request.target = c.addr.updateState;
    request.expected = expected.data();
    request.expectedLength = static_cast<std::uint32_t>(expected.size());
    request.priority = -1000;  // 排在最前，保证在别的插件之前看到状态
    request.detour = reinterpret_cast<void*>(&UpdateStateDetour);
    request.original = reinterpret_cast<void**>(&c.origUpdateState);
    auto result = c.hooks.InstallInline(hook::kCoreOwner, request);
    if (result.result != hook::Result::Ok) {
        LogError("每帧 hook 安装失败：%s（%s）", hook::ResultText(result.result), result.detail.c_str());
        return false;
    }
    c.frameHookHandle = result.handle;
    LogInfo("每帧 hook 已安装：UpdateStateInternal=%p", c.addr.updateState);
    return true;
}

}  // namespace

int CoreBoot(const dd::common::CoreBootInfo* info)
{
    auto& c = C();
    // 至少要覆盖当前用到的字段。开发期不保证新旧版本搭配（ADR 0018）
    constexpr std::uint32_t kBootInfoV1Size = offsetof(dd::common::CoreBootInfo, createAppSystems) + sizeof(void*);
    if (!info || info->size < kBootInfoV1Size || info->version != dd::common::kCoreBootVersion) {
        OutputDebugStringA("[DDCore] 启动信息版本不匹配，DDCore 停用\n");
        return dd::common::kCoreBootDisabled;
    }
    c.mainThread = GetCurrentThreadId();
    c.exe = GetModuleHandleW(nullptr);
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char id[48];
        std::snprintf(id, sizeof(id), "%04u%02u%02u-%02u%02u%02u-%lu", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
            now.wSecond, GetCurrentProcessId());
        c.runId = id;
    }
    c.gameRoot = info->gameRoot ? info->gameRoot : L"";
    c.binDir = info->binDir ? info->binDir : L"";
    c.logDir = c.gameRoot + L"\\DDModLoader\\log";
    c.utf8GameRoot = ToUtf8(c.gameRoot);
    c.utf8BinDir = ToUtf8(c.binDir);
    c.utf8ModsDir = c.utf8GameRoot + "\\mods";
    c.utf8LogDir = ToUtf8(c.logDir);
    CreateDirectoryW((c.gameRoot + L"\\DDModLoader").c_str(), nullptr);
    CreateDirectoryW(c.logDir.c_str(), nullptr);
    CoreLogInit(c.logDir);

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(c.exe);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const std::uint8_t*>(c.exe) + dos->e_lfanew);
    c.imageSize = nt->OptionalHeader.SizeOfImage;

    {
        FILE* revision = nullptr;
        if (_wfopen_s(&revision, (c.gameRoot + L"\\svn_revision.txt").c_str(), L"rb") == 0 && revision) {
            unsigned char buffer[64] = {};
            const size_t got = fread(buffer, 1, sizeof(buffer), revision);
            fclose(revision);
            // 文件内容是 16 字节的二进制，不是文本，转成十六进制
            char hex[3];
            for (size_t i = 0; i < got; ++i) {
                std::snprintf(hex, sizeof(hex), "%02x", buffer[i]);
                c.svnRevision += hex;
            }
        }
    }

    LogInfo("DDCore %s 启动，运行标识 %s，主线程 %lu，游戏根目录 %s", kCoreVersion, c.runId.c_str(), c.mainThread, c.utf8GameRoot.c_str());

    DiscoveryResult found = DiscoverPlugins(c.gameRoot);
    for (const auto& note : found.notes) {
        LogInfo("%s", ToUtf8(note).c_str());
    }
    if (found.candidates.empty()) {
        LogInfo("未发现新协议插件，DDCore 保持休眠，未安装任何 hook");
        return dd::common::kCoreBootDormant;
    }

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(c.exe, exePath, MAX_PATH);
    c.exeSha1 = FileSha1(exePath);

    game::ResolveReport report;
    if (!game::Resolve(c.exe, c.addr, report)) {
        for (const auto& missing : report.missing) {
            LogError("缺少特征码：%s", missing.c_str());
        }
        for (const auto& error : report.layoutErrors) {
            LogError("布局校验失败：%s", error.c_str());
        }
        LogError("DDCore 已整体停用（exe SHA1 %s），下列新协议插件不会加载：", c.exeSha1.c_str());
        for (const auto& candidate : found.candidates) {
            LogError("  %s", ToUtf8(candidate.path).c_str());
        }
        return dd::common::kCoreBootDisabled;
    }
    for (const auto& note : report.notes) {
        LogWarn("%s", note.c_str());
    }

    if (const char* label = game::BuildLabel(c.exeSha1)) {
        c.buildLabel = label;
    } else {
        c.buildLabel = "unknown";
        LogWarn("未知的 Darkest.exe 版本（SHA1 %s），但特征码全部命中，继续运行", c.exeSha1.c_str());
    }
    LogInfo("Darkest.exe %s，SHA1 %s，svn_revision=%s", c.buildLabel.c_str(), c.exeSha1.c_str(), c.svnRevision.c_str());

    c.tracker = std::make_unique<PhaseTracker>(c.addr.states);
    c.lifecycle = std::make_unique<Lifecycle>(&InvokeLifecycleCallback);

    if (c.addr.eventManager && c.addr.eventTypeCount && c.addr.eventClassNames && c.addr.listenerAdd) {
        events::SystemDeps deps;
        deps.addr = &c.addr;
        deps.hooks = &c.hooks;
        deps.mainThread = c.mainThread;
        deps.exe = c.exe;
        deps.invoke = &InvokeEventCallback;
        deps.log = [](int level, const std::string& text) {
            (level >= DD_LOG_WARN ? (level >= DD_LOG_ERROR ? LogError : LogWarn) : LogInfo)("%s", text.c_str());
        };
        Fault fault;
        struct InitArgs {
            events::SystemDeps* deps;
            std::unique_ptr<events::System>* out;
        } args{ &deps, &c.events };
        if (!GuardedInvoke(
                [](void* raw) {
                    auto* a = static_cast<InitArgs*>(raw);
                    *a->out = std::make_unique<events::System>(std::move(*a->deps));
                },
                &args, &fault)) {
            LogError("事件系统初始化崩溃：%s，events 服务不可用", DescribeFault(fault).c_str());
            c.events.reset();
        }
        if (c.events) {
            c.events->hub().SetEndInvoke(&InvokeEventEnd);
        }
    }

    if (!InstallFrameHook()) {
        LogError("每帧 hook 不可用，DDCore 已整体停用");
        return dd::common::kCoreBootDisabled;
    }

    LoadPlugins(found.candidates);

    std::size_t loaded = 0;
    {
        std::lock_guard lock(c.registryMutex);
        for (const auto& plugin : c.plugins) {
            if (plugin->state == PluginState::Loaded) {
                ++loaded;
            }
        }
    }
    if (loaded == 0) {
        std::string detail;
        c.hooks.Remove(hook::kCoreOwner, c.frameHookHandle, &detail);
        LogInfo("没有插件加载成功，DDCore 保持休眠，每帧 hook 已移除 %s", detail.c_str());
        return dd::common::kCoreBootDormant;
    }

    c.running = true;
    LogInfo("阶段 Early，%zu 个插件已加载", loaded);
    c.lifecycle->Fire(DD_PHASE_EARLY, c.frame, 0);
    return dd::common::kCoreBootRunning;
}

void CoreAppSystemsCreated()
{
    auto& c = C();
    if (!c.running) {
        return;
    }
    LogInfo("阶段 AppReady，线程 %lu", GetCurrentThreadId());
    if (c.events) {
        struct Args {
            events::System* system;
        } args{ c.events.get() };
        Fault fault;
        if (!GuardedInvoke([](void* raw) { static_cast<Args*>(raw)->system->OnAppReady(); }, &args, &fault)) {
            LogError("事件系统 OnAppReady 崩溃：%s", DescribeFault(fault).c_str());
        }
    }
    c.lifecycle->Fire(DD_PHASE_APP_READY, c.frame, 0);
    HookReport("AppReady");
}

}  // namespace ddc
