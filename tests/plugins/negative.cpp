// 负面测试插件。NEG_KIND_* 选择行为（见 tests/xmake.lua）。
// 全部通过 DDModLoader\log\DDCore.log 观察结果。

#include <dd/dd.hpp>

#include <windows.h>

#include <cstdio>
#include <string_view>

// LoadAppAssets 入口：只用于验证回滚（hook 之后立刻失败）
static const char* const kRollbackTargetPattern =
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 88 48 81 EC 78 01 00 00 48 8B F9 45 33 F6";

using RollbackFn = void*(__fastcall*)(void*);
static RollbackFn g_orig = nullptr;
static void* __fastcall RollbackDetour(void* self)
{
    return g_orig(self);
}

static dd::Runtime g_dd;

// neg_hook_conflict 用：如果安装成功才会被调用
using ConflictFn = void*(*)(void*, void*, float, const int*, unsigned, const int*, bool, void*);
static ConflictFn g_origConflict = nullptr;
static void* ConflictDetour(void* self, void* prng, float damage, const int* type, unsigned source, const int* status, bool flag,
    void* attacker)
{
    return g_origConflict(self, prng, damage, type, source, status, flag, attacker);
}

static void DD_CALL TickMustNeverRun(const DDPhaseEvent*, void*)
{
    // 插件 Load 失败后回滚，这个回调绝不应该被调用
    OutputDebugStringA("NEG: rollback failed - tick still called\n");
    static const char kMsg[] = "BUG：已回滚的插件的 Tick 回调仍被调用";
    g_dd.log().write(DD_LOG_ERROR, kMsg);
}

// 安装一个 hook，让回滚有东西可回滚
static void InstallRollbackHook()
{
    void* target = nullptr;
    if (!g_dd.hooks().scan(kRollbackTargetPattern, &target)) {
        g_dd.log().warn("回滚测试目标的特征码未唯一命中，跳过 hook");
        return;
    }
    const auto result = g_dd.hooks().install_inline("neg_rollback", target,
        { 0x40, 0x55, 0x53, 0x56, 0x57 }, &RollbackDetour, &g_orig);
    g_dd.log().info("回滚测试 hook 安装结果：{}（{}）", static_cast<int>(result), g_dd.hooks().text(result));
}

#if defined(NEG_KIND_NEG_HOOK_BAD_ARGS)
// 非主线程安装内联 hook 应该返回 BAD_TIMING
static DWORD WINAPI OffThreadInstall(LPVOID)
{
    static const std::uint8_t kBytes[] = { 0x40, 0x55, 0x53, 0x56, 0x57 };
    void* original = nullptr;
    DDHookSpec spec{};
    spec.size = sizeof(spec);
    spec.kind = DD_HOOK_INLINE;
    spec.name = "off_thread";
    spec.target = g_dd.game()->image_base();
    spec.expected = kBytes;
    spec.expected_len = sizeof(kBytes);
    spec.detour = reinterpret_cast<void*>(&RollbackDetour);
    spec.original = &original;
    const auto result = g_dd.hooks().service()->install(g_dd.plugin(), &spec, nullptr);
    g_dd.log().info("非主线程安装内联 hook：{}（{}）", static_cast<int>(result), g_dd.hooks().text(result));
    return 0;
}
#endif

DD_EXPORT bool DD_CALL DDPlugin_Query(DDPluginInfo* out)
{
    out->abi_version = DD_ABI_VERSION;
    out->version = DD_VERSION_MAKE(1, 0, 0);

#if defined(NEG_KIND_NEG_ABI_MISMATCH)
    out->name = "neg_abi_mismatch";
    out->abi_version = DD_ABI_MAKE(99, 0);  // 主版本不匹配
#elif defined(NEG_KIND_NEG_MISSING_DEP)
    out->name = "neg_missing_dep";
    static const DDPluginDep deps[] = { { "no_such_plugin", 0, 0 } };
    out->deps = deps;
    out->dep_count = 1;
#elif defined(NEG_KIND_NEG_LOAD_FALSE)
    out->name = "neg_load_false";
#elif defined(NEG_KIND_NEG_LOAD_CRASH)
    out->name = "neg_load_crash";
#elif defined(NEG_KIND_NEG_QUERY_CRASH)
    out->name = "neg_query_crash";
    *reinterpret_cast<volatile int*>(0x10) = 1;
#elif defined(NEG_KIND_NEG_DEP_ON_FAILED)
    out->name = "neg_dep_on_failed";
    static const DDPluginDep deps[] = { { "neg_load_false", 0, 0 } };
    out->deps = deps;
    out->dep_count = 1;
#elif defined(NEG_KIND_NEG_CYCLE_A)
    out->name = "neg_cycle_a";
    static const DDPluginDep deps[] = { { "neg_cycle_b", 0, 0 } };
    out->deps = deps;
    out->dep_count = 1;
#elif defined(NEG_KIND_NEG_CYCLE_B)
    out->name = "neg_cycle_b";
    static const DDPluginDep deps[] = { { "neg_cycle_a", 0, 0 } };
    out->deps = deps;
    out->dep_count = 1;
#elif defined(NEG_KIND_NEG_HOOK_CONFLICT)
    out->name = "neg_hook_conflict";
#elif defined(NEG_KIND_NEG_ABI_UNSET)
    out->name = "neg_abi_unset";
    out->abi_version = 0;  // 模拟插件忘记填写
#elif defined(NEG_KIND_NEG_HOOK_BAD_ARGS)
    out->name = "neg_hook_bad_args";
#else
#error "未指定 NEG_KIND_*"
#endif
    return true;
}

DD_EXPORT bool DD_CALL DDPlugin_Load(const DDApi* api)
{
    if (!g_dd.init(api)) {
        return false;
    }

#if defined(NEG_KIND_NEG_LOAD_FALSE) || defined(NEG_KIND_NEG_LOAD_CRASH)
    // 注册 Tick 回调和 hook，让回滚有东西可回滚；回滚后回调不应被调用
    g_dd.lifecycle().service()->subscribe(g_dd.plugin(), DD_PHASE_TICK, &TickMustNeverRun, nullptr);
#endif

#if defined(NEG_KIND_NEG_HOOK_CONFLICT)
    // 和 DDShield 抢 Actor::ApplyHealthDamageInternal 的入口（Steam 版 RVA 4836A0，hooks.md）。
    // 旧式 DLL 先加载的情况下入口已是 E9，必须被拒绝并写明跳转去向，不破坏 DDShield。
    if (std::string_view(g_dd.game()->build_label()) == "steam") {
        auto* base = static_cast<std::uint8_t*>(g_dd.game()->image_base());
        const auto result = g_dd.hooks().install_inline("neg_conflict:ApplyHealthDamageInternal", base + 0x4836A0,
            { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10 }, &ConflictDetour, &g_origConflict);
        g_dd.log().info("抢占 DDShield 目标的结果：{}（{}）", static_cast<int>(result), g_dd.hooks().text(result));
    }
    return true;
#elif defined(NEG_KIND_NEG_HOOK_BAD_ARGS)
    // 三种坏指针都应该返回 BAD_ARGUMENT，插件不崩溃；之后 hook 服务要能继续用，证明没有留下锁
    static const std::uint8_t kBytes[] = { 0x40, 0x55, 0x53, 0x56, 0x57 };
    void* original = nullptr;
    const auto attempt = [&](const char* label, const char* name, const std::uint8_t* expected, void** out) {
        DDHookSpec spec{};
        spec.size = sizeof(spec);
        spec.kind = DD_HOOK_INLINE;
        spec.name = name;
        spec.target = g_dd.game()->image_base();
        spec.expected = expected;
        spec.expected_len = sizeof(kBytes);
        spec.detour = reinterpret_cast<void*>(&RollbackDetour);
        spec.original = out;
        const auto result = g_dd.hooks().service()->install(g_dd.plugin(), &spec, nullptr);
        g_dd.log().info("{}：{}（{}）", label, static_cast<int>(result), g_dd.hooks().text(result));
    };
    attempt("坏的 name", reinterpret_cast<const char*>(0x20), kBytes, &original);
    attempt("坏的 expected", "bad_expected", reinterpret_cast<const std::uint8_t*>(0x20), &original);
    attempt("坏的 original", "bad_original", kBytes, reinterpret_cast<void**>(0x20));
    if (HANDLE thread = CreateThread(nullptr, 0, &OffThreadInstall, nullptr, 0, nullptr)) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
    InstallRollbackHook();
    return false;  // 回滚，这样 negative 方案里 LoadAppAssets 入口仍然应该是原始字节
#elif defined(NEG_KIND_NEG_LOAD_FALSE)
    InstallRollbackHook();
    g_dd.log().info("Load 返回 false（测试回滚）");
    return false;
#elif defined(NEG_KIND_NEG_LOAD_CRASH)
    InstallRollbackHook();
    g_dd.log().info("即将在 Load 里崩溃（测试隔离）");
    *reinterpret_cast<volatile int*>(0x20) = 2;
    return true;
#else
    return true;
#endif
}
