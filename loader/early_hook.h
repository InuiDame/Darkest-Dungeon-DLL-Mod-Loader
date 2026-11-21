// 早期 hook：DllMain 里定位 Darkest::CreateAppSystems 并打补丁，主线程进入时加载 DDCore。
// 本文件被 dllmain.cpp 包含一次，用到那边的 Log()、路径工具。
//
// 这里的代码尽量精简，协议处理全部在 DDCore 里：dbghelp.dll 是最难更新的文件。
// 特征码来自 core/src/game/signatures.inc，和 DDCore、tools/dd_update.py 是同一份。
// 加载器不读布局清单（layout.inc）：这里只用 CreateAppSystems 的特征码、偷取字节数和 DD_ENTRY，运行时的核对是下面的
// 「唯一命中 + 命中位置是函数入口 + 入口字节和 DD_ENTRY 一致」，偷取字节数在编译期核对，不成立时不装早期 hook（U1b，
// ADR 0036；函数入口和偷取字节数是 U1 修复 FA 加的，审核 A 的 S1、备注 4）。清单里唯一依赖 loader 的是 detour 的原型
// （ProtoCreateAppSystems，没有静态依据），加载器核对不了，ADR 0036 列为局限。

#include "../common/boot_abi.h"
#include "../common/pattern.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dd_early {

// 只用 CreateAppSystems 这一项（特征码、偷取字节数和 DD_ENTRY 给出的入口字节）
#define DD_SIG(id, kind, req, steal, capture, pattern) \
    inline constexpr const char kPattern_##id[] = pattern; \
    inline constexpr unsigned kSteal_##id = steal;
#define DD_ENTRY(id, bytes) inline constexpr const char kEntry_##id[] = bytes;
#define DD_KNOWN_BUILD(sha, label)
#include "signatures.inc"
#undef DD_SIG
#undef DD_ENTRY
#undef DD_KNOWN_BUILD

// 下面写入 5 字节的 jmp rel32，偷取范围至少要 5 字节（DDCore 的 hook 引擎会拒绝更短的，这里没有引擎，编译期核对）
static_assert(kSteal_CreateAppSystems >= 5, "CreateAppSystems 的偷取字节数小于 5，放不下 jmp rel32");

using CreateAppSystemsFn = void*(__fastcall*)(void*);

// 命中位置必须是函数入口（和 DDCore 的 check::CheckHookEntry、dd_update.py verify 同一条）：异常目录里包含它的表项要从它开始，
// 而且不是链式分块（UNW_FLAG_CHAININFO）或间接表项。不在任何表项里时不拦（和 Python 一样），只有入口字节的核对
static bool IsFunctionEntry(const std::uint8_t* target) {
    DWORD64 imageBase = 0;
    const PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(target), &imageBase, nullptr);
    if (!entry) {
        return true;
    }
    if (imageBase + entry->BeginAddress != reinterpret_cast<DWORD64>(target) || (entry->UnwindData & 1)) {
        return false;
    }
    const auto* unwind = reinterpret_cast<const std::uint8_t*>(imageBase + entry->UnwindData);
    return ((unwind[0] >> 3) & UNW_FLAG_CHAININFO) == 0;
}

static CreateAppSystemsFn g_gate = nullptr;
static std::wstring g_gameRoot;
static std::wstring g_binDir;
static void* g_createAppSystems = nullptr;
static bool g_installed = false;

static HMODULE g_core = nullptr;
static dd::common::CoreBootFn g_coreBoot = nullptr;
static dd::common::CoreAppSystemsCreatedFn g_coreAfter = nullptr;

// ---- 跳板 ----

static std::uint8_t* AllocNear(std::uintptr_t target, std::size_t size) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const std::uintptr_t gran = si.dwAllocationGranularity;
    const std::uintptr_t floor = target > 0x7FFF0000 ? target - 0x7FFF0000 : gran;
    const std::uintptr_t ceil = target + 0x7FFF0000;
    for (int direction = 0; direction < 2; ++direction) {
        std::uintptr_t p = (target & ~(gran - 1)) + (direction ? gran : 0);
        while (p > floor && p < ceil) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) &&
                mbi.State == MEM_FREE && mbi.RegionSize >= size) {
                void* block = VirtualAlloc(reinterpret_cast<void*>(p), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                if (block) return static_cast<std::uint8_t*>(block);
            }
            p += direction ? gran : (std::uintptr_t)0 - gran;
        }
    }
    return nullptr;
}

extern "C" void* __fastcall DDEarly_CreateAppSystemsDetour(void* a1);

// 返回 true 表示已安装。失败时写明原因，不做任何修改，游戏和旧式 DLL 不受影响。
static bool Install(const std::wstring& gameRoot, const std::wstring& binDir) {
    HMODULE exe = GetModuleHandleW(nullptr);
    const std::uint8_t* text = nullptr;
    std::size_t textSize = 0;
    if (!dd::common::ModuleTextSection(exe, &text, &textSize)) {
        Log(L"[DDCore] 无法定位 Darkest.exe 的 .text 节，新协议功能不可用");
        return false;
    }

    std::vector<int> pattern;
    if (!dd::common::ParsePattern(kPattern_CreateAppSystems, pattern)) {
        Log(L"[DDCore] 特征码格式错误，新协议功能不可用");
        return false;
    }
    const std::uint8_t* hit = nullptr;
    const std::size_t hits = dd::common::ScanPattern(text, textSize, pattern, &hit);
    if (hits != 1) {
        Log(L"[DDCore] 特征码 CreateAppSystems 未唯一命中（命中 " + std::to_wstring(hits) +
            L" 次），不安装早期 hook，DDCore 不启动，新协议插件不可用；旧式 DLL 照常加载");
        return false;
    }

    auto* target = const_cast<std::uint8_t*>(hit);
    if (!IsFunctionEntry(target)) {
        Log(L"[DDCore] 特征码 CreateAppSystems 命中的位置不是函数入口，不安装早期 hook，DDCore 不启动，新协议插件不可用；旧式 DLL 照常加载");
        return false;
    }

    // 再校验入口字节：DD_ENTRY 给出的就是搬走的指令 mov [rsp+8],rcx，不含 rip 相对寻址（dd_update.py verify 校验过）
    std::vector<int> entry;
    const unsigned steal = kSteal_CreateAppSystems;
    bool entryOk = dd::common::ParsePattern(kEntry_CreateAppSystems, entry) && entry.size() == steal;
    for (std::size_t i = 0; entryOk && i < entry.size(); ++i) {
        entryOk = entry[i] >= 0 && target[i] == static_cast<std::uint8_t>(entry[i]);
    }
    if (!entryOk) {
        Log(L"[DDCore] CreateAppSystems 入口字节不符，不安装早期 hook");
        return false;
    }

    auto* page = AllocNear(reinterpret_cast<std::uintptr_t>(target), 0x1000);
    if (!page) {
        Log(L"[DDCore] 无法在 Darkest.exe 附近分配跳板页，不安装早期 hook");
        return false;
    }

    // page+0x00：长跳到 C++ detour
    page[0] = 0xFF;
    page[1] = 0x25;
    std::memset(page + 2, 0, 4);
    void* detour = reinterpret_cast<void*>(&DDEarly_CreateAppSystemsDetour);
    std::memcpy(page + 6, &detour, sizeof(detour));

    // page+0x20：gate = 搬走的入口指令 + 跳回 target+steal
    auto* gate = page + 0x20;
    std::memcpy(gate, target, steal);
    gate[steal] = 0xE9;
    const auto backRel = static_cast<std::int32_t>(
        reinterpret_cast<std::intptr_t>(target + steal) - reinterpret_cast<std::intptr_t>(gate + steal + 5));
    std::memcpy(gate + steal + 1, &backRel, 4);
    FlushInstructionCache(GetCurrentProcess(), page, 0x40);
    g_gate = reinterpret_cast<CreateAppSystemsFn>(gate);
    g_gameRoot = gameRoot;
    g_binDir = binDir;
    g_createAppSystems = target;

    DWORD old = 0;
    if (!VirtualProtect(target, steal, PAGE_EXECUTE_READWRITE, &old)) {
        Log(L"[DDCore] VirtualProtect 失败，不安装早期 hook");
        return false;
    }
    const auto rel = static_cast<std::int32_t>(
        reinterpret_cast<std::intptr_t>(page) - reinterpret_cast<std::intptr_t>(target + 5));
    target[0] = 0xE9;
    std::memcpy(target + 1, &rel, 4);
    DWORD restored = 0;
    VirtualProtect(target, steal, old, &restored);
    FlushInstructionCache(GetCurrentProcess(), target, steal);

    g_installed = true;
    return true;
}

// DllMain 里调用：SEH 兜底，任何异常都不能阻止游戏启动
static bool InstallGuarded(const std::wstring& gameRoot, const std::wstring& binDir) {
    __try {
        return Install(gameRoot, binDir);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- 主线程上加载 DDCore ----

struct BootOutcome {
    HMODULE core = nullptr;
    DWORD loadError = 0;
    int result = dd::common::kCoreBootDisabled;
    bool crashed = false;
};

static void LoadCoreRaw(const wchar_t* path, BootOutcome& outcome, const dd::common::CoreBootInfo& info) {
    __try {
        // 插件在 mods\DD Mod Lib 下，用 ALTERED_SEARCH_PATH 从那里解析依赖
        HMODULE core = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!core) {
            outcome.loadError = GetLastError();
            return;
        }
        outcome.core = core;
        g_coreBoot = reinterpret_cast<dd::common::CoreBootFn>(GetProcAddress(core, dd::common::kCoreBootExport));
        g_coreAfter = reinterpret_cast<dd::common::CoreAppSystemsCreatedFn>(
            GetProcAddress(core, dd::common::kCoreAppSystemsCreatedExport));
        if (!g_coreBoot || !g_coreAfter) {
            outcome.loadError = ERROR_PROC_NOT_FOUND;
            return;
        }
        outcome.result = g_coreBoot(&info);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outcome.crashed = true;
    }
}

static void AfterCoreRaw(bool* crashed) {
    __try {
        g_coreAfter();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *crashed = true;
    }
}

}  // namespace dd_early

// hook 触发时已在主线程，不再受加载器锁约束
extern "C" void* __fastcall DDEarly_CreateAppSystemsDetour(void* a1) {
    using namespace dd_early;
    Log(L"[DDCore] CreateAppSystems 命中，线程 " + std::to_wstring(GetCurrentThreadId()) + L"，启动 DDCore");

    const std::wstring path = g_gameRoot + L"\\mods\\DD Mod Lib\\DDCore.dll";
    dd::common::CoreBootInfo info{};
    info.size = sizeof(info);
    info.version = dd::common::kCoreBootVersion;
    info.gameRoot = g_gameRoot.c_str();
    info.binDir = g_binDir.c_str();
    info.createAppSystems = g_createAppSystems;

    BootOutcome outcome;
    if (PathFileExistsW(path.c_str())) {
        LoadCoreRaw(path.c_str(), outcome, info);
    } else {
        outcome.loadError = ERROR_FILE_NOT_FOUND;
    }

    const bool usable = g_coreBoot && g_coreAfter && !outcome.crashed && outcome.result != dd::common::kCoreBootDisabled;
    if (outcome.crashed) {
        Log(L"[DDCore] DDCore 启动时发生异常，已隔离，游戏继续运行");
    } else if (!outcome.core) {
        Log(L"[DDCore] 未加载 DDCore.dll（错误码 " + std::to_wstring(outcome.loadError) + L"），新协议功能不可用");
    } else if (!usable) {
        Log(L"[DDCore] DDCore 启动失败或已停用，游戏继续运行，详情见 DDCore.log");
    } else {
        Log(outcome.result == dd::common::kCoreBootRunning ? L"[DDCore] DDCore 已启动" : L"[DDCore] DDCore 休眠（没有新协议插件）");
    }

    void* result = g_gate(a1);

    if (usable) {
        bool crashed = false;
        AfterCoreRaw(&crashed);
        if (crashed) {
            Log(L"[DDCore] DDCore 的 AppSystemsCreated 发生异常，已隔离");
        }
    }
    return result;
}
