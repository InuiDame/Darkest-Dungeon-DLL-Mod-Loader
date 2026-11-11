// native 插件模板。ABI 详见 sdk/include/dd/dd_api.h，示例见 examples/hello_native。
// 导出 DDPlugin_Query 和 DDPlugin_Load 的 DLL 由 DDCore 加载；缺任一导出按旧式 DLL 处理。

#include <dd/dd.hpp>

static dd::Runtime g_dd;

// Query 只填写信息，不要做别的事
DD_EXPORT bool DD_CALL DDPlugin_Query(DDPluginInfo* out)
{
    out->abi_version = DD_ABI_VERSION;
    out->name = "my_plugin";                   // 改成你的插件名
    out->version = DD_VERSION_MAKE(1, 0, 0);
    // 依赖其他新协议插件时填写，下面的例子表示依赖 some_plugin 且版本不低于 1.2.0：
    // static const DDPluginDep deps[] = { { "some_plugin", DD_VERSION_MAKE(1, 2, 0), 0 } };
    // out->deps = deps;
    // out->dep_count = 1;
    return true;
}

// Load 在主线程执行，早于 CreateAppSystems；返回 false 时 DDCore 回滚本插件注册的回调和 hook
DD_EXPORT bool DD_CALL DDPlugin_Load(const DDApi* api)
{
    if (!g_dd.init(api)) {
        return false;
    }

    g_dd.log().info("Load：DDCore {}", g_dd.game()->core_version());

    g_dd.lifecycle().subscribe(DD_PHASE_APP_READY, [](const DDPhaseEvent&) {
        g_dd.log().info("游戏系统已创建");
    });

    return true;
}
