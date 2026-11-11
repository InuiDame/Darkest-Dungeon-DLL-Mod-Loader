// hello_native：演示 P1 的全部服务（log、task、lifecycle、hook、game_info）。
//
// 日志写到 DDModLoader\log\hello_native.log。主菜单阶段可以验证 Early、AppReady、
// AssetsReady、Tick 的顺序和线程；CampaignLoad、CampaignUnload 要进存档才能验证。

#include <dd/dd.hpp>

#include <windows.h>

#include <atomic>
#include <thread>
#include <vector>

static dd::Runtime g_dd;

// ---- 阶段顺序校验 ----

static std::vector<DDPhase> g_seen;
static std::uint64_t g_ticks = 0;
static bool g_orderOk = true;

static const char* PhaseName(std::uint32_t phase)
{
    static const char* const names[] = {
        "Early", "AppReady", "AssetsReady", "CampaignLoad", "Tick", "CampaignUnload"
    };
    return phase < DD_PHASE_COUNT ? names[phase] : "?";
}

static void OnPhase(const DDPhaseEvent& event)
{
    const bool main = g_dd.tasks().is_main_thread();
    if (event.phase == DD_PHASE_TICK) {
        ++g_ticks;
        if (g_ticks == 1) {
            g_seen.push_back(DD_PHASE_TICK);
            g_dd.log().info("首次 Tick，帧 {}，主线程 {}", event.frame, main);
        }
        if (g_ticks % 300 == 0) {
            g_dd.log().info("Tick {}，帧 {}", g_ticks, event.frame);
        }
        return;
    }

    g_dd.log().info("阶段 {}（帧 {}，存档序号 {}，late={}），主线程 {}", PhaseName(event.phase), event.frame,
        event.campaign_serial, event.late, main);
    if (!main) {
        g_orderOk = false;
    }
    // 校验一次性阶段的先后
    if (event.phase <= DD_PHASE_ASSETS_READY && !g_seen.empty()) {
        for (DDPhase previous : g_seen) {
            if (static_cast<std::uint32_t>(previous) >= event.phase && previous != DD_PHASE_TICK) {
                g_orderOk = false;
            }
        }
    }
    g_seen.push_back(static_cast<DDPhase>(event.phase));
}

// ---- hook 示例 ----

static const char* const kUpdateLogicPattern =
    "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 C8 FC FF FF 48 81 EC F8 03 00 00";

using UpdateLogicFn = void(__fastcall*)(void*, float);
static UpdateLogicFn g_origUpdateLogic = nullptr;
static std::atomic<std::uint64_t> g_logicHits{ 0 };

static void __fastcall DetourUpdateLogic(void* app, float dt)
{
    if (++g_logicHits == 120) {
        g_dd.log().info("内联 hook 已命中 120 帧：dt={:.4f}", dt);
    }
    g_origUpdateLogic(app, dt);
}

// Hero 虚表 hook 示例（Steam 版）。用虚表地址 + 校验期望值，避开函数体内联 hook，
// 这是 P0 验证过的和 DDShield 共存的方式。实际插件应该用 RTTI 或特征码定位虚表。
using ApplyDamageFn = void*(*)(void*, void*, float, const int*, unsigned, const int*, bool, void*);
static ApplyDamageFn g_origApplyDamage = nullptr;
static std::atomic<std::uint64_t> g_damageHits{ 0 };

static void* DetourApplyDamage(void* self, void* prng, float damage, const int* type, unsigned source, const int* status,
    bool flag, void* attacker)
{
    if (g_damageHits++ == 0) {
        g_dd.log().info("虚表 hook 首次命中：ApplyHealthDamageInternal damage={:.2f}", damage);
    }
    return g_origApplyDamage(self, prng, damage, type, source, status, flag, attacker);
}

static void InstallHooks()
{
    void* updateLogic = nullptr;
    if (g_dd.hooks().scan(kUpdateLogicPattern, &updateLogic)) {
        const auto result = g_dd.hooks().install_inline("hello:UpdateLogic", updateLogic,
            { 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57 }, &DetourUpdateLogic, &g_origUpdateLogic);
        g_dd.log().info("内联 hook 结果：{}（{}）", static_cast<int>(result), g_dd.hooks().text(result));
    } else {
        g_dd.log().warn("UpdateLogicInternal 特征码未唯一命中，跳过内联 hook 示例");
    }

    // Steam 版的 Hero 虚表和 ApplyHealthDamageInternal 地址（RVA），只为演示虚表 hook
    if (std::string_view(g_dd.game()->build_label()) == "steam") {
        auto* base = static_cast<std::uint8_t*>(g_dd.game()->image_base());
        auto** slot = reinterpret_cast<void**>(base + 0xE8A288 + 50 * sizeof(void*));
        const auto result = g_dd.hooks().install_vtable("hello:Hero::ApplyDamage(vtable)", slot, base + 0x4836A0,
            &DetourApplyDamage, &g_origApplyDamage);
        g_dd.log().info("虚表 hook 结果：{}（{}）", static_cast<int>(result), g_dd.hooks().text(result));
    } else {
        g_dd.log().info("非 Steam 版，跳过虚表 hook 示例");
    }
}

// ---- task 示例 ----

static void DD_CALL DelayedTask(void*)
{
    g_dd.log().info("延时任务（1500ms）执行，主线程 {}", g_dd.tasks().is_main_thread());
    g_dd.log().info("阶段顺序校验：{}（主线程检查 {}）", g_orderOk ? "通过" : "失败", g_orderOk ? "通过" : "失败");
}

static void PostFromThread()
{
    std::thread([] {
        Sleep(2000);
        g_dd.tasks().post([] {
            g_dd.log().info("来自后台线程的任务在主线程执行：{}", g_dd.tasks().is_main_thread());
        });
    }).detach();
}

// ---- 插件入口 ----

DD_EXPORT bool DD_CALL DDPlugin_Query(DDPluginInfo* out)
{
    out->abi_version = DD_ABI_VERSION;
    out->name = "hello_native";
    out->version = DD_VERSION_MAKE(1, 0, 0);
    return true;
}

DD_EXPORT bool DD_CALL DDPlugin_Load(const DDApi* api)
{
    if (!g_dd.init(api)) {
        return false;
    }

    auto& game = g_dd.game();
    g_dd.log().info("Load：DDCore {}，build={} sha1={} svn={}", game->core_version(), game->build_label(),
        game->exe_sha1(), game->svn_revision());
    g_dd.log().info("路径：root={} mods={} log={}", game->game_root(), game->mods_dir(), game->log_dir());
    g_dd.log().info("本插件：{}，主线程 {}", game.plugin_path(), g_dd.tasks().is_main_thread());

    // 未实现的服务必须返回空
    const void* events = api->query("events", 1);
    g_dd.log().info("query(events,1) -> {}（P2 才实现，应为空）", events ? "非空" : "空");
    const void* tooNew = api->query("log", 9999);
    g_dd.log().info("query(log,9999) -> {}（版本过高，应为空）", tooNew ? "非空" : "空");

    for (auto phase : { DD_PHASE_EARLY, DD_PHASE_APP_READY, DD_PHASE_ASSETS_READY, DD_PHASE_CAMPAIGN_LOAD, DD_PHASE_TICK,
             DD_PHASE_CAMPAIGN_UNLOAD }) {
        g_dd.lifecycle().subscribe(phase, [](const DDPhaseEvent& event) { OnPhase(event); });
    }

    // 主线程任务（Load 时提交，第一帧执行）和延时任务
    g_dd.tasks().post([] { g_dd.log().info("主线程任务执行，主线程 {}", g_dd.tasks().is_main_thread()); });
    g_dd.tasks().service()->post_delayed(g_dd.plugin(), &DelayedTask, nullptr, 1500);
    PostFromThread();

    InstallHooks();
    return true;
}

DD_EXPORT void DD_CALL DDPlugin_Unload(void)
{
    g_dd.log().info("Unload");
}
