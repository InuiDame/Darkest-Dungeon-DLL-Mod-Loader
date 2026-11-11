// 串链验证插件：和 hello_native 内联 hook 同一个目标 UpdateLogicInternal，
// 验证「同一目标只打一次补丁，多个插件串成链」。依赖 hello_native，所以排在它后面加载。

#include <dd/dd.hpp>

#include <cstdint>

static dd::Runtime g_dd;

static const char* const kUpdateLogicPattern =
    "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 C8 FC FF FF 48 81 EC F8 03 00 00";

using UpdateLogicFn = void(__fastcall*)(void*, float);
static UpdateLogicFn g_orig = nullptr;
static std::uint64_t g_hits = 0;

static void __fastcall ChainDetour(void* app, float dt)
{
    if (++g_hits == 60) {
        g_dd.log().info("串链 detour 已被调用 60 次，通过 *original 转给下一环");
    }
    g_orig(app, dt);
}

DD_EXPORT bool DD_CALL DDPlugin_Query(DDPluginInfo* out)
{
    static const DDPluginDep deps[] = { { "hello_native", 0, 0 } };
    out->abi_version = DD_ABI_VERSION;
    out->name = "hello_chain";
    out->version = DD_VERSION_MAKE(1, 0, 0);
    out->deps = deps;
    out->dep_count = 1;
    return true;
}

DD_EXPORT bool DD_CALL DDPlugin_Load(const DDApi* api)
{
    if (!g_dd.init(api)) {
        return false;
    }
    void* target = nullptr;
    if (!g_dd.hooks().scan(kUpdateLogicPattern, &target)) {
        g_dd.log().error("UpdateLogicInternal 特征码未唯一命中");
        return false;
    }
    // 优先级 5，排在 hello_native（优先级 0）之后
    const auto result = g_dd.hooks().install_inline("hello_chain:UpdateLogic", target,
        { 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57 }, &ChainDetour, &g_orig, nullptr, 5);
    g_dd.log().info("串链 hook 安装结果：{}（{}）", static_cast<int>(result), g_dd.hooks().text(result));
    return result == DD_HOOK_OK;
}
