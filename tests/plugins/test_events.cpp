// test_events：订阅全部主题并记录每种事件的首次收到（线程、字段值），
// 战斗中把英雄受到的伤害减半、取消压力效果，并记录前后 HP/压力，让日志能判断修改是否真的生效。
// 日志：DDModLoader\log\test_events.log

#include <dd/dd.hpp>

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

static dd::Runtime g_dd;
static DWORD g_mainThread = 0;

struct TopicState {
    std::string name;
    std::vector<std::string> fieldNames;
    std::vector<DDEventFieldInfo> fields;
    std::uint64_t count = 0;
    std::uint64_t deferred = 0;
    DWORD firstThread = 0;
    std::string firstFields;
    bool first = true;
    DDEventSubId id = 0;
};

static std::vector<TopicState*> g_topics;
static int g_requested = 0;
static int g_subscribed = 0;

static const char* KindText(uint32_t kind)
{
    switch (kind) {
    case 0:
        return "native";
    case 1:
        return "query";
    case 2:
        return "semantic";
    case 3:
        return "custom";
    default:
        return "app";
    }
}

static std::string DescribeFields(DDEvent* ev, const TopicState& state)
{
    const auto& events = g_dd.events();
    std::string out;
    for (const auto& field : state.fields) {
        out += " ";
        out += field.name;
        out += "=";
        char buffer[160];
        switch (field.type) {
        case DD_FIELD_INT:
            std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(events.get_int(ev, field.name)));
            break;
        case DD_FIELD_FLOAT:
            std::snprintf(buffer, sizeof(buffer), "%.3f", events.get_float(ev, field.name));
            break;
        case DD_FIELD_BOOL:
            std::snprintf(buffer, sizeof(buffer), "%s", events.get_bool(ev, field.name) ? "true" : "false");
            break;
        case DD_FIELD_STRING:
            std::snprintf(buffer, sizeof(buffer), "'%s'", events.get_string(ev, field.name));
            break;
        default:
            std::snprintf(buffer, sizeof(buffer), "%p", events.get_pointer(ev, field.name));
            break;
        }
        out += buffer;
    }
    return out;
}

static void CountEvent(DDEvent* ev, TopicState& state, const char* tag)
{
    const auto& events = g_dd.events();
    ++state.count;
    const auto flags = events.flags(ev);
    if (flags & DD_EVENT_F_DEFERRED) {
        ++state.deferred;
    }
    if (state.first) {
        state.first = false;
        state.firstThread = events.origin_thread(ev);
        state.firstFields = DescribeFields(ev, state);
        g_dd.log().info("首次收到 {} topic={} 线程={} 派发线程={} 主线程={} 延迟={} 标志=0x{:X} 字段:{}", tag, state.name,
            GetCurrentThreadId(), state.firstThread, g_mainThread, (flags & DD_EVENT_F_DEFERRED) ? "是" : "否", flags,
            state.firstFields);
    }
}

// ---- 战斗修改验证 ----

// 游戏的实际掉血是 roundf(伤害 * (1 + [Actor+0xBE0]))，见 4836A0
constexpr std::size_t kActorDamageTaken = 0xBE0;

struct DamageRecord {
    void* target = nullptr;
    float original = 0;
    float applied = 0;
    float multiplier = 1;
    bool halved = false;
};
static DamageRecord g_lastDamage;
static int g_damageHalved = 0;
static int g_damagePass = 0;
static int g_damageMismatch = 0;
static int g_damageUnjudged = 0;
static int g_damageLogged = 0;

// 压力效果交替处理：单数次取消，双数次放行作对照。只有放行时压力上升、取消时不变，才说明取消真的起作用
static int g_stressSeen = 0;
static int g_effectCancelled = 0;
static int g_cancelStressChanged = 0;
static int g_controlPassed = 0;
static int g_controlRose = 0;
static int g_effectLogged = 0;

static const char* JudgeDamage(const DamageRecord& record, float hpBefore, float hpAfter, float halfDrop, float fullDrop)
{
    const float dropped = hpBefore - hpAfter;
    const auto same = [](float a, float b) { return std::abs(a - b) < 0.01f; };
    const char* verdict = nullptr;
    if (hpAfter <= 0.0f) {
        verdict = "不判定（HP 归零，掉血被截断）";
    } else if (same(halfDrop, fullDrop)) {
        verdict = "不判定（伤害太小，减半前后相同）";
    } else if (same(dropped, halfDrop)) {
        ++g_damagePass;
        return "减半生效";
    } else if (same(dropped, fullDrop)) {
        ++g_damageMismatch;
        return "数值不符（没有减半）";
    } else if (dropped < halfDrop) {
        verdict = "不判定（可能被格挡或护盾吸收）";
    } else {
        ++g_damageMismatch;
        return "数值不符";
    }
    ++g_damageUnjudged;
    (void)record;
    return verdict;
}

static void DD_CALL OnDamagePre(DDEvent* ev, void* user)
{
    auto* state = static_cast<TopicState*>(user);
    CountEvent(ev, *state, "damage.pre");
    const auto& events = g_dd.events();
    if (!events.get_bool(ev, "is_hero")) {
        return;
    }
    const float before = static_cast<float>(events.get_float(ev, "damage"));
    const float next = before * 0.5f;
    const bool ok = events.set_float(ev, "damage", next);
    auto* target = events.get_pointer(ev, "target");
    const float stat = target ? *reinterpret_cast<const float*>(static_cast<const char*>(target) + kActorDamageTaken) : 0.0f;
    g_lastDamage = { target, before, next, 1.0f + stat, true };
    if (g_damageHalved < 40) {
        g_dd.log().info("damage.pre 英雄 target={} 原伤害={:.2f} 改为={:.2f} 写入成功={} hp_before={:.2f}", target, before, next, ok,
            events.get_float(ev, "hp_before"));
    }
    ++g_damageHalved;
}

static void DD_CALL OnDamagePost(DDEvent* ev, void* user)
{
    auto* state = static_cast<TopicState*>(user);
    CountEvent(ev, *state, "damage.post");
    const auto& events = g_dd.events();
    if (!events.get_bool(ev, "is_hero") || !g_lastDamage.halved || events.get_pointer(ev, "target") != g_lastDamage.target) {
        return;
    }
    g_lastDamage.halved = false;
    const float hpBefore = static_cast<float>(events.get_float(ev, "hp_before"));
    const float hpAfter = static_cast<float>(events.get_float(ev, "hp_after"));
    const float halfDrop = std::roundf(g_lastDamage.multiplier * g_lastDamage.applied);
    const float fullDrop = std::roundf(g_lastDamage.multiplier * g_lastDamage.original);
    const char* verdict = JudgeDamage(g_lastDamage, hpBefore, hpAfter, halfDrop, fullDrop);
    if (g_damageLogged < 40) {
        g_dd.log().info("damage.post 英雄 传给原函数={:.2f} 倍率={:.2f} 减半应掉={:.0f} 未减半应掉={:.0f} 实际掉血={:.2f}（hp {:.2f} -> {:.2f}） timeline={} 判定={}",
            events.get_float(ev, "damage"), g_lastDamage.multiplier, halfDrop, fullDrop, hpBefore - hpAfter, hpBefore, hpAfter,
            events.get_pointer(ev, "timeline"), verdict);
        ++g_damageLogged;
    }
}

static void DD_CALL OnEffectPre(DDEvent* ev, void* user)
{
    auto* state = static_cast<TopicState*>(user);
    CountEvent(ev, *state, "effect.pre");
    const auto& events = g_dd.events();
    if (events.get_float(ev, "stress_amount") <= 0.0) {
        return;
    }
    const bool wantCancel = ++g_stressSeen % 2 == 1;
    const bool cancelled = wantCancel && events.cancel(ev);
    g_effectCancelled += cancelled ? 1 : 0;
    if (wantCancel && !cancelled) {
        g_dd.log().warn("effect.pre cancel 失败");
    }
    if (g_effectLogged < 60) {
        g_dd.log().info("effect.pre 压力效果（{}） stress_amount={:.1f} target={} target_stress={:.1f}", cancelled ? "取消" : "放行作对照",
            events.get_float(ev, "stress_amount"), events.get_pointer(ev, "target"), events.get_float(ev, "target_stress"));
    }
}

static void DD_CALL OnEffectPost(DDEvent* ev, void* user)
{
    auto* state = static_cast<TopicState*>(user);
    CountEvent(ev, *state, "effect.post");
    const auto& events = g_dd.events();
    if (events.get_float(ev, "stress_amount") <= 0.0) {
        return;
    }
    const double before = events.get_float(ev, "target_stress");
    const double after = events.get_float(ev, "target_stress_after");
    const bool cancelled = events.get_bool(ev, "cancelled");
    const bool changed = before != after;
    if (cancelled) {
        g_cancelStressChanged += changed ? 1 : 0;
    } else {
        ++g_controlPassed;
        g_controlRose += after > before ? 1 : 0;
    }
    if (g_effectLogged < 60) {
        ++g_effectLogged;
        g_dd.log().info("effect.post {} 压力 {:.1f} -> {:.1f} 判定={}", cancelled ? "已取消" : "放行对照", before, after,
            cancelled ? (changed ? "压力变化了！" : "压力未变") : (after > before ? "对照压力上升" : "对照压力未变（效果可能被抵抗）"));
    }
}

static void DD_CALL OnGeneric(DDEvent* ev, void* user)
{
    auto* state = static_cast<TopicState*>(user);
    CountEvent(ev, *state, "event");
}

static void DD_CALL OnBattleRound(DDEvent* ev, void* user)
{
    auto* state = static_cast<TopicState*>(user);
    CountEvent(ev, *state, "round");
    if (state->name == "battle.round.start") {
        g_dd.log().info("battle.round.start round={}", g_dd.events().get_int(ev, "round"));
    }
}

static void Summary()
{
    int received = 0;
    for (const auto* state : g_topics) {
        if (state->count > 0) {
            ++received;
        }
    }
    g_dd.log().info("==== 计数汇总：订阅 {}/{} 个主题，收到 {} 种事件 ====", g_subscribed, g_requested, received);
    for (const auto* state : g_topics) {
        if (state->count > 0) {
            g_dd.log().info("{} count={} deferred={} first_thread={}", state->name, state->count, state->deferred, state->firstThread);
        }
    }
    g_dd.log().info("damage 减半 {} 次：减半生效 {} 次，数值不符 {} 次（应为 0），不判定 {} 次", g_damageHalved, g_damagePass,
        g_damageMismatch, g_damageUnjudged);
    g_dd.log().info("压力效果 取消 {} 次（其中压力变化 {} 次，应为 0），放行对照 {} 次（其中压力上升 {} 次，应大于 0）", g_effectCancelled,
        g_cancelStressChanged, g_controlPassed, g_controlRose);
    g_dd.log().info("==== 汇总结束 ====");
}

static void SubscribeAll()
{
    const auto* service = g_dd.events().service();
    const std::uint32_t count = service->topic_count();
    g_requested = static_cast<int>(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        DDEventTopicInfo info{};
        if (!service->topic_at(i, &info)) {
            continue;
        }
        const char* reason = "";
        const bool available = service->topic_available(info.name, &reason);
        g_dd.log().info("主题 {} kind={} 字段数={} writable={} cancellable={} available={}{}{}", info.name, KindText(info.kind),
            info.field_count, info.writable, info.cancellable, available, available ? "" : " 原因: ", available ? "" : reason);
        if (!available) {
            continue;
        }
        auto* state = new TopicState();
        state->name = info.name;
        for (std::uint32_t f = 0; f < info.field_count; ++f) {
            DDEventFieldInfo field{};
            if (service->topic_field_at(info.name, f, &field)) {
                state->fields.push_back(field);
            }
        }
        DDEventFn fn = &OnGeneric;
        const std::string name = info.name;
        if (name == "damage.pre") {
            fn = &OnDamagePre;
        } else if (name == "damage.post") {
            fn = &OnDamagePost;
        } else if (name == "effect.pre") {
            fn = &OnEffectPre;
        } else if (name == "effect.post") {
            fn = &OnEffectPost;
        } else if (name == "battle.round.start" || name == "battle.round.end") {
            fn = &OnBattleRound;
        }
        const DDEventOptions options{ sizeof(DDEventOptions), 0, DD_EVENT_SCOPE_PLUGIN };
        state->id = service->subscribe(g_dd.plugin(), info.name, fn, state, &options);
        if (state->id) {
            ++g_subscribed;
            g_topics.push_back(state);
        } else {
            g_dd.log().warn("主题 {} 订阅失败", info.name);
            delete state;
        }
    }
    g_dd.log().info("订阅完成：{}/{} 个主题成功", g_subscribed, g_requested);
}

// 回调里取消订阅并再次订阅：验证派发中增删
static DDEventSubId g_selfCancelId = 0;
static void DD_CALL OnSelfCancelTick(DDEvent* ev, void*)
{
    (void)ev;
    static int ticks = 0;
    if (++ticks == 120 && g_selfCancelId) {
        const bool off = g_dd.events().off(g_selfCancelId);
        g_dd.log().info("app.tick：在回调里取消订阅（结果 {}），下一帧应不再收到", off);
        g_selfCancelId = 0;
    }
}

// 自定义主题：验证 publish 和回写
static void DD_CALL OnCustomPre(DDEvent* ev, void*)
{
    const auto& events = g_dd.events();
    const bool wrote = events.set_int(ev, "value", events.get_int(ev, "value") + 100);
    events.cancel(ev);
    g_dd.log().info("custom.test.pre 收到 value={} 写入成功={} 标志=0x{:X}", events.get_int(ev, "value"), wrote, events.flags(ev));
}

static void CustomTopicTest()
{
    const auto* service = g_dd.events().service();
    const DDEventFieldInfo fields[] = { { "value", DD_FIELD_INT, DD_FIELD_READ | DD_FIELD_WRITE } };
    const bool ok = service->register_topic(g_dd.plugin(), "custom.test.pre", fields, 1);
    g_dd.log().info("自定义主题注册结果 {}", ok);
    g_dd.events().on("custom.test.pre", [](DDEvent* ev) { OnCustomPre(ev, nullptr); });
    g_dd.lifecycle().subscribe(DD_PHASE_ASSETS_READY, [](const DDPhaseEvent&) {
        DDEventValue value{};
        value.name = "value";
        value.type = DD_FIELD_INT;
        value.v.i = 7;
        bool cancelled = false;
        const bool published = g_dd.events().service()->publish(g_dd.plugin(), "custom.test.pre", &value, 1, &cancelled);
        g_dd.log().info("publish 结果 {}，回写 value={}（应为 107），取消={}", published, value.v.i, cancelled);
    });
}

DD_EXPORT bool DD_CALL DDPlugin_Query(DDPluginInfo* out)
{
    out->abi_version = DD_ABI_VERSION;
    out->name = "test_events";
    out->version = DD_VERSION_MAKE(1, 0, 0);
    return true;
}

DD_EXPORT bool DD_CALL DDPlugin_Load(const DDApi* api)
{
    if (!g_dd.init(api) || !g_dd.events()) {
        return false;
    }
    g_mainThread = GetCurrentThreadId();
    SubscribeAll();
    CustomTopicTest();
    // 派发中取消自己的订阅（用 lifecycle 的 Tick 驱动太早，这里用 app.tick）
    g_selfCancelId = g_dd.events().on("app.tick", [](DDEvent* ev) { OnSelfCancelTick(ev, nullptr); }, -5);

    g_dd.lifecycle().subscribe(DD_PHASE_TICK, [](const DDPhaseEvent& event) {
        static std::uint64_t ticks = 0;
        if (++ticks == 1 || ticks % 3600 == 0) {
            Summary();
        }
        (void)event;
    });
    g_dd.lifecycle().subscribe(DD_PHASE_CAMPAIGN_UNLOAD, [](const DDPhaseEvent&) { Summary(); });
    return true;
}

DD_EXPORT void DD_CALL DDPlugin_Unload(void) {}
