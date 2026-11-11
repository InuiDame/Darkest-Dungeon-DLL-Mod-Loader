// hello_events：演示 events 服务的订阅、读字段、修改、取消和自定义主题。
//
// 日志写到 DDModLoader\log\hello_events.log。
// 进入存档后，战斗中英雄受到的伤害减半，每次回合开始和奇遇事件记一行。

#include <dd/dd.hpp>

static dd::Runtime g_dd;

static void OnDamagePre(DDEvent* ev)
{
    const auto& events = g_dd.events();
    if (!events.get_bool(ev, "is_hero")) {
        return;
    }
    const double before = events.get_float(ev, "damage");
    events.set_float(ev, "damage", before * 0.5);  // .pre 主题可写，原函数拿到减半后的伤害
    g_dd.log().info("damage.pre：英雄受到 {:.1f}，减半为 {:.1f}", before, events.get_float(ev, "damage"));
}

static void OnEffectPre(DDEvent* ev)
{
    const auto& events = g_dd.events();
    if (events.get_float(ev, "stress_amount") > 0.0) {
        events.cancel(ev);  // .pre 可取消：跳过这次压力效果
        g_dd.log().info("effect.pre：取消了一个压力效果（stress_amount={:.1f}）", events.get_float(ev, "stress_amount"));
    }
}

static void OnRoundStart(DDEvent* ev)
{
    g_dd.log().info("第 {} 回合开始", g_dd.events().get_int(ev, "round"));
}

static void OnCustomPre(DDEvent* ev)
{
    g_dd.log().info("收到自定义主题 demo.value.pre，value={}", g_dd.events().get_int(ev, "value"));
    g_dd.events().set_int(ev, "value", 42);
}

DD_EXPORT bool DD_CALL DDPlugin_Query(DDPluginInfo* out)
{
    out->abi_version = DD_ABI_VERSION;
    out->name = "hello_events";
    out->version = DD_VERSION_MAKE(1, 0, 0);
    return true;
}

DD_EXPORT bool DD_CALL DDPlugin_Load(const DDApi* api)
{
    if (!g_dd.init(api) || !g_dd.events()) {
        return false;
    }
    const auto& events = g_dd.events();
    events.on("damage.pre", &OnDamagePre);
    events.on("effect.pre", &OnEffectPre);
    events.on("battle.round.start", &OnRoundStart);

    // 自定义主题：演示注册和发布，.pre 主题可以被回调修改
    const DDEventFieldInfo fields[] = { { "value", DD_FIELD_INT, DD_FIELD_READ | DD_FIELD_WRITE } };
    if (events.service()->register_topic(g_dd.plugin(), "demo.value.pre", fields, 1)) {
        events.on("demo.value.pre", &OnCustomPre);
        g_dd.lifecycle().subscribe(DD_PHASE_ASSETS_READY, [](const DDPhaseEvent&) {
            DDEventValue value{};
            value.name = "value";
            value.type = DD_FIELD_INT;
            value.v.i = 7;
            g_dd.events().service()->publish(g_dd.plugin(), "demo.value.pre", &value, 1, nullptr);
            g_dd.log().info("自定义主题发布完毕，回调写回 value={}", value.v.i);
        });
    }
    return true;
}

DD_EXPORT void DD_CALL DDPlugin_Unload(void) {}
