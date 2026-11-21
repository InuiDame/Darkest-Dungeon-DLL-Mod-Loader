// 不依赖游戏的单元测试：依赖排序、ABI 协商、PE 导出解析、入口解码、hook 链（假函数）、
// 任务队列、生命周期订阅、状态机判定（含探索序号）、actor 句柄的解析（假内存布局）、combat 逻辑、persist 旁路
// （格式、合并、原子写入、一致性判断、backup 同步、标签表）。
// 运行：xmake build dd_core_tests && xmake run dd_core_tests [DLL 目录]
// 给出 DLL 目录时额外校验真实构建出来的 DLL 的协议判定。

#include "../../common/pattern.h"
#include "../../common/pe_exports.h"
#include "../../core/src/combat/logic.h"
#include "../../core/src/events/access.h"
#include "../../core/src/events/churn_log.h"
#include "../../core/src/events/hub.h"
#include "../../core/src/game/actors.h"
#include "../../core/src/game/layout_check.h"
#include "../../core/src/game/phase_tracker.h"
#include "../../core/src/hook/engine.h"
#include "../../core/src/hook/prologue.h"
#include "../../core/src/logic/abi.h"
#include "../../core/src/logic/deps.h"
#include "../../core/src/persist/store.h"
#include "../../core/src/runtime/lifecycle.h"
#include "../../core/src/runtime/task_queue.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                               \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(cond)) {                                                            \
            ++g_failures;                                                         \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                         \
    } while (0)

// ---------------------------------------------------------------- 依赖排序
static void TestDeps()
{
    using ddc::PluginNode;
    {
        std::vector<PluginNode> nodes = { { "a", 1, {} }, { "b", 1, { { "a" } } }, { "c", 1, { { "b" } } } };
        auto r = ddc::ResolveDependencies(nodes);
        CHECK(r.rejected.empty());
        CHECK(r.order.size() == 3 && r.order[0] == 0 && r.order[1] == 1 && r.order[2] == 2);
    }
    {
        // 依赖排在后面的，排序后移到它的依赖之后；其余保持输入顺序
        std::vector<PluginNode> nodes = { { "c", 1, { { "b" } } }, { "a", 1, {} }, { "b", 1, { { "a" } } } };
        auto r = ddc::ResolveDependencies(nodes);
        CHECK(r.rejected.empty());
        CHECK(r.order.size() == 3 && r.order[0] == 1 && r.order[1] == 2 && r.order[2] == 0);
    }
    {
        std::vector<PluginNode> nodes = { { "x", 1, { { "nope" } } }, { "y", 1, {} } };
        auto r = ddc::ResolveDependencies(nodes);
        CHECK(r.order.size() == 1 && r.order[0] == 1);
        CHECK(r.rejected.size() == 1 && r.rejected[0].index == 0 && r.rejected[0].reason.find("缺少依赖") != std::string::npos);
    }
    {
        // 循环：a -> b -> a，z 依赖循环中的 a 也被拒绝
        std::vector<PluginNode> nodes = { { "a", 1, { { "b" } } }, { "b", 1, { { "a" } } }, { "z", 1, { { "a" } } }, { "ok", 1, {} } };
        auto r = ddc::ResolveDependencies(nodes);
        CHECK(r.order.size() == 1 && r.order[0] == 3);
        CHECK(r.rejected.size() == 3);
        bool cycleNoted = false;
        for (const auto& rej : r.rejected) {
            if (rej.reason.find("依赖循环") != std::string::npos) {
                cycleNoted = true;
            }
        }
        CHECK(cycleNoted);
    }
    {
        // 依赖链：d 依赖被拒绝的 e，e 缺依赖
        std::vector<PluginNode> nodes = { { "d", 1, { { "e" } } }, { "e", 1, { { "missing" } } } };
        auto r = ddc::ResolveDependencies(nodes);
        CHECK(r.order.empty());
        CHECK(r.rejected.size() == 2);
    }
    {
        // 依赖版本不足
        std::vector<PluginNode> nodes = { { "base", DD_VERSION_MAKE(1, 2, 0), {} },
            { "user", 1, { { "base", DD_VERSION_MAKE(2, 0, 0) } } } };
        auto r = ddc::ResolveDependencies(nodes);
        CHECK(r.order.size() == 1 && r.order[0] == 0);
        CHECK(r.rejected.size() == 1 && r.rejected[0].reason.find("低于要求") != std::string::npos);
    }
    {
        // 自依赖
        std::vector<PluginNode> nodes = { { "self", 1, { { "self" } } } };
        auto r = ddc::ResolveDependencies(nodes);
        CHECK(r.order.empty() && r.rejected.size() == 1);
    }
}

// ---------------------------------------------------------------- ABI 协商
static void TestAbi()
{
    const std::uint32_t core = DD_ABI_MAKE(1, 3);
    std::string reason;
    CHECK(ddc::AbiCompatible(DD_ABI_MAKE(1, 3), core, &reason));
    CHECK(ddc::AbiCompatible(DD_ABI_MAKE(1, 0), core, &reason));
    CHECK(!ddc::AbiCompatible(DD_ABI_MAKE(1, 4), core, &reason) && reason.find("只提供") != std::string::npos);
    CHECK(!ddc::AbiCompatible(DD_ABI_MAKE(2, 0), core, &reason) && reason.find("主版本") != std::string::npos);
    CHECK(!ddc::AbiCompatible(DD_ABI_MAKE(0, 9), core, &reason));

    DDPluginInfo info{};
    info.abi_version = DD_ABI_VERSION;
    info.name = "valid_name-1.2";
    info.version = DD_VERSION_MAKE(1, 0, 0);
    CHECK(ddc::ValidatePluginInfo(info, DD_ABI_VERSION, &reason));

    info.name = "bad name";
    CHECK(!ddc::ValidatePluginInfo(info, DD_ABI_VERSION, &reason));
    info.name = "";
    CHECK(!ddc::ValidatePluginInfo(info, DD_ABI_VERSION, &reason));
    info.name = nullptr;
    CHECK(!ddc::ValidatePluginInfo(info, DD_ABI_VERSION, &reason));
    info.name = "ok";

    info.abi_version = 0;
    CHECK(!ddc::ValidatePluginInfo(info, DD_ABI_VERSION, &reason) && reason.find("abi_version") != std::string::npos);
    info.abi_version = DD_ABI_MAKE(DD_ABI_MAJOR + 1, 0);
    CHECK(!ddc::ValidatePluginInfo(info, DD_ABI_VERSION, &reason) && reason.find("主版本") != std::string::npos);
    info.abi_version = DD_ABI_VERSION;

    DDPluginDep dep{ "base", 0, 7 };
    info.deps = &dep;
    info.dep_count = 1;
    CHECK(!ddc::ValidatePluginInfo(info, DD_ABI_VERSION, &reason) && reason.find("reserved") != std::string::npos);
    dep.reserved = 0;
    CHECK(ddc::ValidatePluginInfo(info, DD_ABI_VERSION, &reason));
    info.deps = nullptr;
    CHECK(!ddc::ValidatePluginInfo(info, DD_ABI_VERSION, &reason));
}

// ---------------------------------------------------------------- PE 导出解析

// 构造最小的 PE32+：一个节，放导出表
static std::vector<std::uint8_t> MakePe(const std::vector<std::string>& exports, WORD machine = IMAGE_FILE_MACHINE_AMD64,
    WORD magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC, bool withExportDir = true)
{
    constexpr DWORD kSectionVa = 0x1000;
    constexpr DWORD kFileAlign = 0x200;
    std::vector<std::uint8_t> file(0x200, 0);

    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 0x80;
    std::memcpy(file.data(), &dos, sizeof(dos));

    IMAGE_NT_HEADERS64 nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = machine;
    nt.FileHeader.NumberOfSections = 1;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(nt.OptionalHeader);
    nt.OptionalHeader.Magic = magic;
    nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;

    // 节内容：导出目录 + 名字指针表 + 名字字符串
    std::vector<std::uint8_t> section(sizeof(IMAGE_EXPORT_DIRECTORY));
    const DWORD namePtrRva = kSectionVa + static_cast<DWORD>(section.size());
    section.resize(section.size() + exports.size() * sizeof(DWORD));
    std::vector<DWORD> nameRvas;
    for (const auto& name : exports) {
        nameRvas.push_back(kSectionVa + static_cast<DWORD>(section.size()));
        section.insert(section.end(), name.begin(), name.end());
        section.push_back(0);
    }
    std::memcpy(section.data() + sizeof(IMAGE_EXPORT_DIRECTORY), nameRvas.data(), nameRvas.size() * sizeof(DWORD));

    IMAGE_EXPORT_DIRECTORY exportDir{};
    exportDir.NumberOfNames = static_cast<DWORD>(exports.size());
    exportDir.AddressOfNames = namePtrRva;
    std::memcpy(section.data(), &exportDir, sizeof(exportDir));

    if (withExportDir) {
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress = kSectionVa;
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size = static_cast<DWORD>(section.size());
    }
    std::memcpy(file.data() + dos.e_lfanew, &nt, sizeof(nt));

    IMAGE_SECTION_HEADER header{};
    std::memcpy(header.Name, ".edata", 6);
    header.VirtualAddress = kSectionVa;
    header.Misc.VirtualSize = static_cast<DWORD>(section.size());
    header.SizeOfRawData = static_cast<DWORD>(section.size());
    header.PointerToRawData = kFileAlign;
    std::memcpy(file.data() + dos.e_lfanew + sizeof(nt), &header, sizeof(header));

    file.resize(kFileAlign);
    file.insert(file.end(), section.begin(), section.end());
    return file;
}

static void TestPeExports(const char* artifactDir)
{
    using dd::common::PeIsNewProtocol;
    auto both = MakePe({ "DDPlugin_Load", "DDPlugin_Query", "Other" });
    CHECK(PeIsNewProtocol(both.data(), both.size()));

    auto onlyQuery = MakePe({ "DDPlugin_Query" });
    CHECK(!PeIsNewProtocol(onlyQuery.data(), onlyQuery.size()));
    auto onlyLoad = MakePe({ "DDPlugin_Load" });
    CHECK(!PeIsNewProtocol(onlyLoad.data(), onlyLoad.size()));
    auto noExport = MakePe({ "DDPlugin_Query", "DDPlugin_Load" }, IMAGE_FILE_MACHINE_AMD64, IMAGE_NT_OPTIONAL_HDR64_MAGIC, false);
    CHECK(!PeIsNewProtocol(noExport.data(), noExport.size()));
    auto x86 = MakePe({ "DDPlugin_Query", "DDPlugin_Load" }, IMAGE_FILE_MACHINE_I386);
    CHECK(!PeIsNewProtocol(x86.data(), x86.size()));
    auto notMagic = MakePe({ "DDPlugin_Query", "DDPlugin_Load" }, IMAGE_FILE_MACHINE_AMD64, IMAGE_NT_OPTIONAL_HDR32_MAGIC);
    CHECK(!PeIsNewProtocol(notMagic.data(), notMagic.size()));

    // 畸形输入：任意截断都不能崩溃或越界。只有覆盖到两个名字末尾的长度才可能判定成功
    const std::string needle = "DDPlugin_Query";
    const auto found = std::search(both.begin(), both.end(), needle.begin(), needle.end());
    CHECK(found != both.end());
    const std::size_t needEnd = static_cast<std::size_t>(found - both.begin()) + needle.size() + 1;
    int truncatedTrueEarly = 0;
    for (std::size_t len = 0; len < both.size(); ++len) {
        const bool r = PeIsNewProtocol(both.data(), len);
        if (r && len < needEnd) {
            ++truncatedTrueEarly;
        }
    }
    CHECK(truncatedTrueEarly == 0);
    CHECK(PeIsNewProtocol(both.data(), both.size()));

    // 乱写的头：比如 e_lfanew 指向文件之外
    auto corrupt = both;
    corrupt[0x3C] = 0xFF;
    corrupt[0x3D] = 0xFF;
    CHECK(!PeIsNewProtocol(corrupt.data(), corrupt.size()));
    auto corrupt2 = both;
    std::memset(corrupt2.data() + 0x200, 0xFF, 8);  // 破坏导出目录内容
    (void)PeIsNewProtocol(corrupt2.data(), corrupt2.size());

    // 全 0 和随机内容
    std::vector<std::uint8_t> zeros(4096, 0);
    CHECK(!PeIsNewProtocol(zeros.data(), zeros.size()));
    std::vector<std::uint8_t> noise(4096);
    for (std::size_t i = 0; i < noise.size(); ++i) {
        noise[i] = static_cast<std::uint8_t>(i * 37 + 11);
    }
    CHECK(!PeIsNewProtocol(noise.data(), noise.size()));

    if (artifactDir && artifactDir[0]) {
        const auto path = [&](const char* name) {
            wchar_t wide[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, artifactDir, -1, wide, MAX_PATH);
            return std::wstring(wide) + L"\\" + std::wstring(name, name + std::strlen(name));
        };
        CHECK(dd::common::DllIsNewProtocol(path("hello_native.dll").c_str()));
        CHECK(!dd::common::DllIsNewProtocol(path("DDModLoader.dll").c_str()));
        CHECK(!dd::common::DllIsNewProtocol(path("DDCore.dll").c_str()));
        CHECK(!dd::common::DllIsNewProtocol(L"C:\\Windows\\System32\\kernel32.dll"));
        CHECK(!dd::common::DllIsNewProtocol(L"C:\\no\\such\\file.dll"));
    } else {
        std::printf("  （跳过真实 DLL 判定，未给出 DLL 目录）\n");
    }
}

// ---------------------------------------------------------------- 特征码扫描
static void TestPattern()
{
    using namespace dd::common;
    std::vector<int> p;
    CHECK(ParsePattern("48 8B ?? 05", p) && p.size() == 4 && p[0] == 0x48 && p[2] == -1);
    CHECK(!ParsePattern("48 8", p));
    CHECK(!ParsePattern("GG", p));
    const std::uint8_t data[] = { 0x00, 0x48, 0x8B, 0xFF, 0x05, 0x48, 0x8B, 0x00, 0x05, 0x90 };
    ParsePattern("48 8B ?? 05", p);
    const std::uint8_t* first = nullptr;
    CHECK(ScanPattern(data, sizeof(data), p, &first) == 2 && first == data + 1);
    ParsePattern("05 90", p);
    CHECK(ScanPattern(data, sizeof(data), p, &first) == 1 && first == data + 8);
    ParsePattern("11 22", p);
    CHECK(ScanPattern(data, sizeof(data), p, &first) == 0);
}

// ---------------------------------------------------------------- 入口解码
static void TestPrologue()
{
    using ddc::hook::ComputeStealLength;
    using ddc::hook::DecodeInstruction;

    const auto steal = [](std::initializer_list<std::uint8_t> code, std::uint32_t min = 5) {
        std::vector<std::uint8_t> v(code);
        const char* reason = nullptr;
        return ComputeStealLength(v.data(), static_cast<std::uint32_t>(v.size()), min, &reason);
    };

    // 游戏里的真实入口
    CHECK(steal({ 0x48, 0x89, 0x4C, 0x24, 0x08, 0x53 }) == 5);                      // CreateAppSystems
    CHECK(steal({ 0x40, 0x55, 0x53, 0x56, 0x57, 0x41 }) == 5);                      // UpdateStateInternal
    CHECK(steal({ 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57 }) == 5);                // UpdateLogicInternal
    CHECK(steal({ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 }) == 5);
    CHECK(steal({ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 }) == 6);
    CHECK(steal({ 0x4C, 0x8B, 0xDC, 0x57, 0x41, 0x55, 0x41, 0x57 }, 5) == 6);       // spAnimationState_apply，3+1+2
    CHECK(steal({ 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x01 }) == 7);
    CHECK(steal({ 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00, 0x33, 0xC0 }) == 7);
    CHECK(steal({ 0xF3, 0x0F, 0x10, 0x05, 0x00, 0x00, 0x00, 0x00, 0x90 }, 1) == 0); // movss xmm0,[rip+0]

    // 带 SIB 和 disp32 的 mov
    CHECK(steal({ 0x48, 0x8B, 0x84, 0x24, 0x10, 0x00, 0x00, 0x00, 0x90, 0x90 }, 5) == 8);

    const auto rejected = [](std::initializer_list<std::uint8_t> code, const char* mention) {
        std::vector<std::uint8_t> v(code);
        const char* reason = nullptr;
        const auto n = ComputeStealLength(v.data(), static_cast<std::uint32_t>(v.size()), 5, &reason);
        CHECK(n == 0);
        if (reason) {
            CHECK(std::strstr(reason, mention) != nullptr);
        } else {
            CHECK(false);
        }
    };
    rejected({ 0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x90 }, "rip");   // mov rax,[rip+..]
    rejected({ 0xE9, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90 }, "控制流");
    rejected({ 0xE8, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90 }, "控制流");
    rejected({ 0xEB, 0x10, 0x90, 0x90, 0x90, 0x90, 0x90 }, "控制流");
    rejected({ 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }, "控制流");
    rejected({ 0x48, 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x90 }, "跳转");
    rejected({ 0xFF, 0xE0, 0x90, 0x90, 0x90, 0x90 }, "跳转");
    rejected({ 0xF4, 0x90, 0x90, 0x90, 0x90, 0x90 }, "不支持");
    // 期望字节没有覆盖到指令边界
    {
        std::vector<std::uint8_t> partial = { 0x40, 0x53, 0x48, 0x83, 0xEC };
        const char* reason = nullptr;
        CHECK(ComputeStealLength(partial.data(), 5, 5, &reason) == 0);
    }
}

// ---------------------------------------------------------------- hook 引擎

namespace hooktest {

static std::vector<char> g_trace;
using Fn = int(__cdecl*)(int);

static void* g_page = nullptr;
static std::size_t g_pageUsed = 0;

// 生成假函数 int f(int x) { return x + add; }，入口 push rbx; sub rsp,20h
static std::uint8_t* MakeFake(int add)
{
    if (!g_page) {
        g_page = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }
    auto* code = static_cast<std::uint8_t*>(g_page) + g_pageUsed;
    g_pageUsed += 0x40;
    std::size_t i = 0;
    const std::uint8_t prologue[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 };
    std::memcpy(code, prologue, sizeof(prologue));
    i += sizeof(prologue);
    code[i++] = 0x8D;  // lea eax, [rcx+disp32]
    code[i++] = 0x81;
    std::memcpy(code + i, &add, 4);
    i += 4;
    const std::uint8_t epilogue[] = { 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3 };
    std::memcpy(code + i, epilogue, sizeof(epilogue));
    return code;
}

static const std::uint8_t kExpected[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 };

static Fn g_origA, g_origB, g_origC;
static int __cdecl DetourA(int x) { g_trace.push_back('A'); return g_origA(x) + 100; }
static int __cdecl DetourB(int x) { g_trace.push_back('B'); return g_origB(x) + 1000; }
static int __cdecl DetourC(int x) { g_trace.push_back('C'); return g_origC(x) + 10000; }

static ddc::hook::InstallResult Install(ddc::hook::Engine& e, std::uint32_t owner, void* target, const char* name,
    int priority, void* detour, void** original, const std::uint8_t* expected = kExpected, std::uint32_t expectedLen = 6)
{
    ddc::hook::InlineRequest r{};
    r.name = name;
    r.target = target;
    r.expected = expected;
    r.expectedLength = expectedLen;
    r.priority = priority;
    r.detour = detour;
    r.original = original;
    return e.InstallInline(owner, r);
}

static void Run()
{
    using namespace ddc::hook;

    {
        // 只打一次补丁，多个 detour 按优先级串链
        Engine e;
        auto* f = MakeFake(7);
        CHECK(reinterpret_cast<Fn>(f)(1) == 8);

        auto a = Install(e, 1, f, "a", 10, reinterpret_cast<void*>(&DetourA), reinterpret_cast<void**>(&g_origA));
        CHECK(a.result == Result::Ok && a.freshPatch && a.chainDepth == 1);
        CHECK(f[0] == 0xE9);
        auto b = Install(e, 2, f, "b", -5, reinterpret_cast<void*>(&DetourB), reinterpret_cast<void**>(&g_origB));
        CHECK(b.result == Result::Ok && !b.freshPatch && b.chainDepth == 2);
        auto c = Install(e, 3, f, "c", 10, reinterpret_cast<void*>(&DetourC), reinterpret_cast<void**>(&g_origC));
        CHECK(c.result == Result::Ok && c.chainDepth == 3);

        g_trace.clear();
        const int result = reinterpret_cast<Fn>(f)(1);
        CHECK(result == 8 + 100 + 1000 + 10000);
        CHECK(std::string(g_trace.data(), g_trace.size()) == "BAC");  // 优先级小的先调用，同优先级按注册先后

        auto snapshot = e.Snapshot();
        CHECK(snapshot.size() == 1 && snapshot[0].isInline && snapshot[0].stealLength == 6 && snapshot[0].intact);
        CHECK(snapshot[0].chain.size() == 3 && snapshot[0].chain[0].name == "b");

        // 移除中间一环，其余不受影响
        std::string detail;
        CHECK(e.Remove(1, a.handle, &detail) == Result::Ok);
        g_trace.clear();
        CHECK(reinterpret_cast<Fn>(f)(1) == 8 + 1000 + 10000);
        CHECK(std::string(g_trace.data(), g_trace.size()) == "BC");

        // 移除者身份校验
        CHECK(e.Remove(9, c.handle, &detail) == Result::NotOwner);
        CHECK(e.Remove(1, 424242, &detail) == Result::NotFound);

        // 移除头部
        CHECK(e.Remove(2, b.handle, &detail) == Result::Ok);
        g_trace.clear();
        CHECK(reinterpret_cast<Fn>(f)(1) == 8 + 10000);
        CHECK(std::string(g_trace.data(), g_trace.size()) == "C");

        // 全部移除后恢复原始字节
        CHECK(e.Remove(3, c.handle, &detail) == Result::Ok);
        CHECK(std::memcmp(f, kExpected, 6) == 0);
        CHECK(reinterpret_cast<Fn>(f)(1) == 8);
        CHECK(e.Snapshot().empty());
    }

    {
        // 拒绝：已被 hook、字节不符、不能搬移，全部不改动目标
        Engine e;
        auto* jumped = MakeFake(0);
        jumped[0] = 0xE9;
        int fake = 0x12345678;
        std::memcpy(jumped + 1, &fake, 4);
        const auto before = std::vector<std::uint8_t>(jumped, jumped + 12);
        auto r1 = Install(e, 1, jumped, "x", 0, reinterpret_cast<void*>(&DetourA), reinterpret_cast<void**>(&g_origA));
        CHECK(r1.result == Result::AlreadyHooked);
        CHECK(r1.detail.find("跳转") != std::string::npos);
        CHECK(std::memcmp(jumped, before.data(), 12) == 0);

        auto* other = MakeFake(0);
        other[1] = 0x54;  // 入口变成 push r12，字节不符但不是跳转
        auto r2 = Install(e, 1, other, "y", 0, reinterpret_cast<void*>(&DetourA), reinterpret_cast<void**>(&g_origA));
        CHECK(r2.result == Result::ExpectedMismatch);

        auto* unsafe = MakeFake(0);
        const std::uint8_t ripBytes[] = { 0x48, 0x8B, 0x05, 0x00, 0x10, 0x00, 0x00, 0x90 };
        std::memcpy(unsafe, ripBytes, sizeof(ripBytes));
        auto r3 = Install(e, 1, unsafe, "z", 0, reinterpret_cast<void*>(&DetourA), reinterpret_cast<void**>(&g_origA),
            ripBytes, 8);
        CHECK(r3.result == Result::UnsafePrologue);
        CHECK(std::memcmp(unsafe, ripBytes, sizeof(ripBytes)) == 0);

        CHECK(e.Snapshot().empty());

        // 参数校验
        auto r4 = Install(e, 1, nullptr, "n", 0, reinterpret_cast<void*>(&DetourA), reinterpret_cast<void**>(&g_origA));
        CHECK(r4.result == Result::BadArgument);
        const std::uint8_t tooShort[] = { 0x90, 0x90 };
        auto* ok = MakeFake(0);
        auto r5 = Install(e, 1, ok, "n", 0, reinterpret_cast<void*>(&DetourA), reinterpret_cast<void**>(&g_origA), tooShort, 2);
        CHECK(r5.result == Result::BadArgument);
    }

    {
        // 目标被他人覆盖：不恢复字节，继续保持直通；RemoveOwner 能回滚
        Engine e;
        auto* f = MakeFake(5);
        auto a = Install(e, 7, f, "a", 0, reinterpret_cast<void*>(&DetourA), reinterpret_cast<void**>(&g_origA));
        CHECK(a.result == Result::Ok);
        // 模拟第三方在我们之后改了入口
        const std::uint8_t foreign[] = { 0xE9, 0x01, 0x02, 0x03, 0x04, 0x90 };
        DWORD old = 0;
        VirtualProtect(f, 6, PAGE_EXECUTE_READWRITE, &old);
        std::memcpy(f, foreign, sizeof(foreign));
        CHECK(!e.Snapshot().front().intact);

        std::vector<std::string> notes;
        CHECK(e.RemoveOwner(7, &notes) == 1);
        CHECK(std::memcmp(f, foreign, sizeof(foreign)) == 0);  // 第三方的字节保持不动
        CHECK(!notes.empty() && notes[0].find("覆盖") != std::string::npos);
        auto snap = e.Snapshot();
        CHECK(snap.size() == 1 && snap[0].orphaned);

        // 再次安装同一目标应被拒绝
        auto r = Install(e, 8, f, "b", 0, reinterpret_cast<void*>(&DetourB), reinterpret_cast<void**>(&g_origB));
        CHECK(r.result == Result::AlreadyHooked);

        // 还原现场，避免影响后面的用例
        std::memcpy(f, kExpected, 6);
    }

    {
        // 入口指令累计超过 16 字节：拒绝，不改动目标，不越界
        Engine e;
        auto* f = MakeFake(0);
        const std::uint8_t longEntry[] = { 0x90, 0x90, 0x90, 0x90,  // 4 字节，还不够放跳转
            0x2E, 0x3E, 0x48, 0xC7, 0x84, 0x24, 0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };  // 14 字节
        std::memcpy(f, longEntry, sizeof(longEntry));
        auto r = Install(e, 1, f, "long", 0, reinterpret_cast<void*>(&DetourA), reinterpret_cast<void**>(&g_origA),
            longEntry, sizeof(longEntry));
        CHECK(r.result == Result::UnsafePrologue);
        CHECK(std::memcmp(f, longEntry, sizeof(longEntry)) == 0);
        CHECK(e.Snapshot().empty());
    }

    {
        // 入口是第三方的 E9：只有 DDCore 自己能串联，移除后恢复对方的字节
        Engine e;
        auto* f = MakeFake(3);
        auto* foreign = MakeFake(20);
        const std::int32_t rel = static_cast<std::int32_t>(foreign - (f + 5));
        f[0] = 0xE9;
        std::memcpy(f + 1, &rel, 4);
        const std::vector<std::uint8_t> before(f, f + 6);

        auto rejected = Install(e, 1, f, "x", 0, reinterpret_cast<void*>(&DetourB), reinterpret_cast<void**>(&g_origB));
        CHECK(rejected.result == Result::AlreadyHooked);

        InlineRequest r{};
        r.name = "chain";
        r.target = f;
        r.expected = kExpected;
        r.expectedLength = 6;
        r.detour = reinterpret_cast<void*>(&DetourA);
        r.original = reinterpret_cast<void**>(&g_origA);
        r.chainForeignJump = true;
        auto a = e.InstallInline(kCoreOwner, r);
        CHECK(a.result == Result::Ok && a.chainedForeign);
        g_trace.clear();
        CHECK(reinterpret_cast<Fn>(f)(1) == 21 + 100);
        CHECK(std::string(g_trace.data(), g_trace.size()) == "A");
        auto snap = e.Snapshot();
        CHECK(snap.size() == 1 && snap[0].foreignChained && snap[0].stealLength == 5);

        auto plugin = Install(e, 1, f, "p", 0, reinterpret_cast<void*>(&DetourB), reinterpret_cast<void**>(&g_origB));
        CHECK(plugin.result == Result::AlreadyHooked);

        std::string detail;
        CHECK(e.Remove(kCoreOwner, a.handle, &detail) == Result::Ok);
        CHECK(std::memcmp(f, before.data(), 6) == 0);
        CHECK(reinterpret_cast<Fn>(f)(1) == 21);
        CHECK(e.Snapshot().empty());
    }

    {
        // 入口是第三方的 FF 25：串联后对方改写跳转槽仍然生效，移除后恢复对方的字节
        Engine e;
        auto* f = MakeFake(4);
        auto* foreign = MakeFake(30);
        auto* other = MakeFake(40);
        auto* slotMemory = MakeFake(0);
        void* destination = foreign;
        std::memcpy(slotMemory, &destination, sizeof(destination));
        const std::int32_t rel = static_cast<std::int32_t>(slotMemory - (f + 6));
        f[0] = 0xFF;
        f[1] = 0x25;
        std::memcpy(f + 2, &rel, 4);
        const std::vector<std::uint8_t> before(f, f + 6);

        InlineRequest r{};
        r.name = "chain";
        r.target = f;
        r.expected = kExpected;
        r.expectedLength = 6;
        r.detour = reinterpret_cast<void*>(&DetourA);
        r.original = reinterpret_cast<void**>(&g_origA);
        r.chainForeignJump = true;
        auto a = e.InstallInline(kCoreOwner, r);
        CHECK(a.result == Result::Ok && a.chainedForeign);
        CHECK(e.Snapshot().front().stealLength == 6);
        CHECK(reinterpret_cast<Fn>(f)(1) == 31 + 100);

        destination = other;
        std::memcpy(slotMemory, &destination, sizeof(destination));
        CHECK(reinterpret_cast<Fn>(f)(1) == 41 + 100);

        std::string detail;
        CHECK(e.Remove(kCoreOwner, a.handle, &detail) == Result::Ok);
        CHECK(std::memcmp(f, before.data(), 6) == 0);
        CHECK(reinterpret_cast<Fn>(f)(1) == 41);
    }

    {
        // 恢复原始字节失败：如实报告，补丁保留但改成直通，不再进入已移除的 detour
        Engine e;
        auto* f = MakeFake(9);
        auto a = Install(e, 1, f, "a", 0, reinterpret_cast<void*>(&DetourA), reinterpret_cast<void**>(&g_origA));
        CHECK(a.result == Result::Ok);

        e.SetMemoryWriterForTest([](void*, const void*, std::size_t) { return false; });
        std::vector<std::string> notes;
        CHECK(e.RemoveOwner(1, &notes) == 1);
        CHECK(!notes.empty() && notes[0].find("失败") != std::string::npos);
        CHECK(f[0] == 0xE9);
        g_trace.clear();
        CHECK(reinterpret_cast<Fn>(f)(1) == 10);
        CHECK(g_trace.empty());
        auto snap = e.Snapshot();
        CHECK(snap.size() == 1 && snap[0].orphaned && snap[0].chain.empty());

        // 写入恢复后，同一目标还能继续串链，全部移除时恢复字节
        e.SetMemoryWriterForTest(nullptr);
        auto b = Install(e, 2, f, "b", 0, reinterpret_cast<void*>(&DetourB), reinterpret_cast<void**>(&g_origB));
        CHECK(b.result == Result::Ok && !b.freshPatch);
        CHECK(!e.Snapshot().front().orphaned);
        g_trace.clear();
        CHECK(reinterpret_cast<Fn>(f)(1) == 10 + 1000);
        std::string detail;
        CHECK(e.Remove(2, b.handle, &detail) == Result::Ok);
        CHECK(std::memcmp(f, kExpected, 6) == 0);
        CHECK(e.Snapshot().empty());
    }

    {
        // RemoveOwner：多个目标的回滚
        Engine e;
        auto* f1 = MakeFake(1);
        auto* f2 = MakeFake(2);
        Install(e, 1, f1, "o1", 0, reinterpret_cast<void*>(&DetourA), reinterpret_cast<void**>(&g_origA));
        Install(e, 2, f1, "o2", 1, reinterpret_cast<void*>(&DetourB), reinterpret_cast<void**>(&g_origB));
        Install(e, 1, f2, "o1b", 0, reinterpret_cast<void*>(&DetourC), reinterpret_cast<void**>(&g_origC));

        std::vector<std::string> notes;
        CHECK(e.RemoveOwner(1, &notes) == 2);  // f1 上 owner1、f2 上 owner1
        auto snap = e.Snapshot();
        CHECK(snap.size() == 1 && snap[0].chain.size() == 1 && snap[0].chain[0].owner == 2);
        CHECK(std::memcmp(f2, kExpected, 6) == 0);  // f2 的链空了，已恢复

        g_trace.clear();
        reinterpret_cast<Fn>(f1)(0);
        CHECK(std::string(g_trace.data(), g_trace.size()) == "B");
    }

    {
        // 虚表 hook
        Engine e;
        auto* impl = MakeFake(3);
        void* vtbl[4] = { nullptr, impl, nullptr, nullptr };
        void** slot = &vtbl[1];

        VtableRequest r{};
        r.name = "v";
        r.slot = slot;
        r.expectedValue = impl;
        r.priority = 0;
        r.detour = reinterpret_cast<void*>(&DetourA);
        r.original = reinterpret_cast<void**>(&g_origA);
        auto a = e.InstallVtable(1, r);
        CHECK(a.result == Result::Ok && a.freshPatch);
        CHECK(*slot != impl);
        g_trace.clear();
        CHECK(reinterpret_cast<Fn>(*slot)(1) == 4 + 100);
        CHECK(std::string(g_trace.data(), g_trace.size()) == "A");

        // 第二个调用者串链
        VtableRequest r2 = r;
        r2.priority = 1;
        r2.detour = reinterpret_cast<void*>(&DetourB);
        r2.original = reinterpret_cast<void**>(&g_origB);
        r2.expectedValue = nullptr;
        auto b = e.InstallVtable(2, r2);
        CHECK(b.result == Result::Ok && !b.freshPatch);
        g_trace.clear();
        CHECK(reinterpret_cast<Fn>(*slot)(1) == 4 + 100 + 1000);
        CHECK(std::string(g_trace.data(), g_trace.size()) == "AB");

        std::string detail;
        CHECK(e.Remove(1, a.handle, &detail) == Result::Ok);
        g_trace.clear();
        CHECK(reinterpret_cast<Fn>(*slot)(1) == 4 + 1000);
        CHECK(e.Remove(2, b.handle, &detail) == Result::Ok);
        CHECK(*slot == impl);

        // 期望值不符
        VtableRequest wrong = r;
        wrong.expectedValue = reinterpret_cast<void*>(0x1234);
        auto w = e.InstallVtable(3, wrong);
        CHECK(w.result == Result::ExpectedMismatch);
        CHECK(*slot == impl);
    }
}

}  // namespace hooktest

// ---------------------------------------------------------------- 任务队列
namespace tasktest {

static std::vector<int> g_order;
static ddc::TaskQueue* g_queue;

static void __cdecl Record(void* user) { g_order.push_back(static_cast<int>(reinterpret_cast<std::intptr_t>(user))); }
static void __cdecl PostNested(void* user)
{
    g_order.push_back(static_cast<int>(reinterpret_cast<std::intptr_t>(user)));
    g_queue->Post(9, &Record, reinterpret_cast<void*>(99), 0, 0);
}

static void Run()
{
    ddc::TaskQueue q;
    g_queue = &q;
    ddc::TaskQueue::Invoke invoke = [](DDTaskFn fn, void* user, std::uint32_t, std::uint64_t) { fn(user); };

    // 同帧 FIFO，延时任务到点才执行
    q.Post(1, &Record, reinterpret_cast<void*>(1), 1000, 0);
    q.Post(1, &Record, reinterpret_cast<void*>(2), 1000, 0);
    const auto delayed = q.Post(1, &Record, reinterpret_cast<void*>(3), 1000, 500);
    CHECK(delayed != 0);
    q.Drain(1000, invoke);
    CHECK((g_order == std::vector<int>{ 1, 2 }));
    q.Drain(1200, invoke);
    CHECK((g_order == std::vector<int>{ 1, 2 }));
    q.Drain(1500, invoke);
    CHECK((g_order == std::vector<int>{ 1, 2, 3 }));

    // 任务里新提交的留到下一帧
    g_order.clear();
    q.Post(1, &PostNested, reinterpret_cast<void*>(7), 0, 0);
    q.Drain(0, invoke);
    CHECK((g_order == std::vector<int>{ 7 }));
    q.Drain(0, invoke);
    CHECK((g_order == std::vector<int>{ 7, 99 }));

    // 取消和按 owner 回滚
    g_order.clear();
    const auto cancel = q.Post(5, &Record, reinterpret_cast<void*>(11), 0, 0);
    q.Post(6, &Record, reinterpret_cast<void*>(12), 0, 0);
    q.Post(5, &Record, reinterpret_cast<void*>(13), 0, 0);
    CHECK(q.Cancel(5, cancel));
    CHECK(!q.Cancel(5, cancel));
    CHECK(!q.Cancel(6, q.Post(5, &Record, nullptr, 0, 0)));
    CHECK(q.RemoveOwner(5) == 2);
    q.Drain(0, invoke);
    CHECK((g_order == std::vector<int>{ 12 }));
    CHECK(q.Pending() == 0);
    CHECK(q.Post(1, nullptr, nullptr, 0, 0) == 0);
}

}  // namespace tasktest

// ---------------------------------------------------------------- 生命周期
namespace lifecycletest {

struct Call {
    std::uint32_t phase;
    std::uint32_t late;
    int tag;
};
static std::vector<Call> g_calls;
static ddc::Lifecycle* g_life;
static std::uint64_t g_cancelTarget;

static void DD_CALL OnPhase(const DDPhaseEvent* ev, void* user)
{
    g_calls.push_back({ ev->phase, ev->late, static_cast<int>(reinterpret_cast<std::intptr_t>(user)) });
}

// 回调里取消排在后面的订阅
static void DD_CALL OnPhaseCancels(const DDPhaseEvent* ev, void* user)
{
    OnPhase(ev, user);
    g_life->Unsubscribe(1, g_cancelTarget);
}

// 回调里新注册
static void DD_CALL OnPhaseSubscribes(const DDPhaseEvent* ev, void* user)
{
    OnPhase(ev, user);
    g_life->Subscribe(1, DD_PHASE_TICK, &OnPhase, reinterpret_cast<void*>(77));
}

static void DD_CALL Crashes(const DDPhaseEvent*, void*) {}

static void Run()
{
    int crashCount = 0;
    ddc::Lifecycle life([&](DDPhaseFn fn, const DDPhaseEvent* ev, void* user, std::uint32_t) {
        if (fn == &Crashes) {
            ++crashCount;
            return false;
        }
        fn(ev, user);
        return true;
    });
    g_life = &life;

    // 正常派发，同一阶段按注册顺序
    life.Subscribe(1, DD_PHASE_EARLY, &OnPhase, reinterpret_cast<void*>(1));
    life.Subscribe(2, DD_PHASE_EARLY, &OnPhase, reinterpret_cast<void*>(2));
    life.Subscribe(1, DD_PHASE_TICK, &OnPhase, reinterpret_cast<void*>(3));
    life.Fire(DD_PHASE_EARLY, 1, 0);
    CHECK(g_calls.size() == 2 && g_calls[0].tag == 1 && g_calls[1].tag == 2 && g_calls[0].late == 0);

    // 一次性阶段已过之后注册：补调一次，late=1，阶段按顺序
    g_calls.clear();
    life.Subscribe(1, DD_PHASE_ASSETS_READY, &OnPhase, reinterpret_cast<void*>(9));  // 还没到，不补调
    life.Subscribe(1, DD_PHASE_EARLY, &OnPhase, reinterpret_cast<void*>(10));        // 已过，要补调
    life.ProcessLate(2, 0);
    CHECK(g_calls.size() == 1 && g_calls[0].tag == 10 && g_calls[0].late == 1 && g_calls[0].phase == DD_PHASE_EARLY);
    g_calls.clear();
    life.Fire(DD_PHASE_ASSETS_READY, 3, 0);
    CHECK(g_calls.size() == 1 && g_calls[0].tag == 9 && g_calls[0].late == 0);

    // CampaignLoad：存档内注册补调，存档外不补调；CampaignUnload 不补调；补调全部结束后不再重复
    g_calls.clear();
    life.Subscribe(1, DD_PHASE_CAMPAIGN_LOAD, &OnPhase, reinterpret_cast<void*>(20));
    life.ProcessLate(4, 0);
    CHECK(g_calls.empty());
    life.SetCampaignActive(true);
    life.Subscribe(2, DD_PHASE_CAMPAIGN_LOAD, &OnPhase, reinterpret_cast<void*>(21));
    life.Subscribe(2, DD_PHASE_CAMPAIGN_UNLOAD, &OnPhase, reinterpret_cast<void*>(22));
    life.ProcessLate(5, 1);
    CHECK(g_calls.size() == 1 && g_calls[0].tag == 21 && g_calls[0].late == 1);
    life.SetCampaignActive(false);
    g_calls.clear();
    life.ProcessLate(6, 1);
    CHECK(g_calls.empty());

    // 回调中取消的不再调用；回调中注册的下一次才派发
    g_calls.clear();
    const auto first = life.Subscribe(1, DD_PHASE_CAMPAIGN_UNLOAD, &OnPhaseCancels, reinterpret_cast<void*>(31));
    (void)first;
    g_cancelTarget = life.Subscribe(1, DD_PHASE_CAMPAIGN_UNLOAD, &OnPhase, reinterpret_cast<void*>(32));
    life.Subscribe(1, DD_PHASE_CAMPAIGN_UNLOAD, &OnPhaseSubscribes, reinterpret_cast<void*>(33));
    life.Fire(DD_PHASE_CAMPAIGN_UNLOAD, 7, 1);
    bool saw31 = false, saw32 = false, saw33 = false;
    for (const auto& c : g_calls) {
        saw31 |= c.tag == 31;
        saw32 |= c.tag == 32;
        saw33 |= c.tag == 33;
    }
    CHECK(saw31 && !saw32 && saw33);
    g_calls.clear();
    life.Fire(DD_PHASE_TICK, 8, 1);
    bool saw77 = false;
    for (const auto& c : g_calls) {
        saw77 |= c.tag == 77;
    }
    CHECK(saw77);

    // 崩溃的回调被自动取消，不影响别的回调
    g_calls.clear();
    life.Subscribe(1, DD_PHASE_APP_READY, &Crashes, nullptr);
    life.Subscribe(2, DD_PHASE_APP_READY, &OnPhase, reinterpret_cast<void*>(41));
    life.Fire(DD_PHASE_APP_READY, 9, 0);
    CHECK(crashCount == 1 && g_calls.size() == 1 && g_calls[0].tag == 41);
    // APP_READY 已过，崩溃的订阅不应该再出现：这里用 owner 回滚验证移除
    CHECK(life.RemoveOwner(2) >= 1);

    // current_phase 指向已到达的最靠后的一次性阶段
    CHECK(life.CurrentPhase() == DD_PHASE_ASSETS_READY);
}

}  // namespace lifecycletest

// ---------------------------------------------------------------- 状态机判定
static void TestTracker()
{
    constexpr int kFe = 22, kLoad = 2, kUnload = 3, kBoot = 21, kTown = 18, kProfile = 1;
    ddc::StateIds ids;
    ids.bootAssets = kBoot;
    ids.loadCampaign = kLoad;
    ids.unloadCampaign = kUnload;
    ddc::PhaseTracker t(ids);

    const void* campaign1 = reinterpret_cast<const void*>(0x1000);
    const void* campaign2 = reinterpret_cast<const void*>(0x2000);

    auto step = [&](const void* c, int next, int last) { return t.Step({ c, next, last }); };

    // 启动：第一帧 next=21，状态机还没进入 → 不触发
    auto e = step(nullptr, kBoot, 0);
    CHECK(!e.assetsReady);
    // 进入 21 的处理器执行 LoadAppAssets，之后 last=21 next=22 → AssetsReady
    e = step(nullptr, kFe, kBoot);
    CHECK(e.assetsReady);
    e = step(nullptr, kFe, kBoot);
    CHECK(!e.assetsReady);  // 只触发一次

    // 主菜单上，Campaign 为空
    e = step(nullptr, kFe, kFe);
    CHECK(!e.campaignLoad && !e.campaignUnload);

    // 读档线程跑的过程中指针已非空，但状态还在 as_load_campaign：不能触发
    e = step(campaign1, kLoad, kLoad);
    CHECK(!e.campaignLoad);
    e = step(campaign1, kLoad, kProfile);
    CHECK(!e.campaignLoad);
    // 线程结束后主线程离开 as_load_campaign，last 仍是 2，next 是城镇：触发
    e = step(campaign1, kTown, kLoad);
    CHECK(e.campaignLoad && !e.loadWasFallback);
    CHECK(t.CampaignActive() && t.CampaignSerial() == 1);

    // 存档内的状态切换不再触发
    e = step(campaign1, 19, kTown);
    CHECK(!e.campaignLoad && !e.campaignUnload);

    // 回主菜单：next 变成 as_unload_campaign，Campaign 还没销毁，先于处理器回调
    e = step(campaign1, kUnload, 19);
    CHECK(e.campaignUnload && !e.unloadAfterDestroy && !t.CampaignActive());
    // 处理器销毁 Campaign 后回到主菜单
    e = step(nullptr, kFe, kUnload);
    CHECK(!e.campaignLoad && !e.campaignUnload);

    // 切换存档：第二次读档，序号递增
    e = step(campaign2, kLoad, kLoad);
    CHECK(!e.campaignLoad);
    e = step(campaign2, 18, kLoad);
    CHECK(e.campaignLoad && t.CampaignSerial() == 2);

    // 指针直接换了（不经过 as_unload_campaign）：先 Unload（标记异常路径）再 Load
    e = step(campaign1, 19, 18);
    CHECK(e.campaignUnload && e.unloadAfterDestroy);
    CHECK(e.campaignLoad && t.CampaignSerial() == 3);

    // 处理器销毁之后才发现指针已空（异常路径）
    e = step(nullptr, kFe, 19);
    CHECK(e.campaignUnload && e.unloadAfterDestroy && !e.campaignLoad);

    // 兜底：错过了离开 as_load_campaign 那一帧，但已明显处于存档内
    ddc::PhaseTracker t2(ids);
    auto e2 = t2.Step({ campaign1, kTown, kTown });
    CHECK(e2.campaignLoad && e2.loadWasFallback);

    // 处理 unload 时不会被判定成 load
    ddc::PhaseTracker t3(ids);
    auto e3 = t3.Step({ campaign1, kLoad, kLoad });
    CHECK(!e3.campaignLoad);
    e3 = t3.Step({ campaign1, kUnload, kLoad });
    CHECK(!e3.campaignLoad);
    e3 = t3.Step({ campaign1, 18, kUnload });
    CHECK(!e3.campaignLoad);

    // 探索序号：last 新进入 as_raid_start 时加一，停留多帧只算一次
    constexpr int kRaidStart = 6, kRaidUpdate = 8;
    ddc::StateIds raidIds = ids;
    raidIds.raidStart = kRaidStart;
    ddc::PhaseTracker t4(raidIds);
    CHECK(t4.RaidSerial() == 0);
    t4.Step({ campaign1, kTown, kLoad });
    t4.Step({ campaign1, kRaidStart, 19 });  // next 是 6，处理器还没运行
    CHECK(t4.RaidSerial() == 0);
    t4.Step({ campaign1, kRaidStart, kRaidStart });  // 处理器已运行，load_raid 线程在跑
    CHECK(t4.RaidSerial() == 1);
    t4.Step({ campaign1, kRaidStart, kRaidStart });
    t4.Step({ campaign1, kRaidUpdate, kRaidStart });
    CHECK(t4.RaidSerial() == 1);
    t4.Step({ campaign1, kRaidUpdate, kRaidUpdate });
    t4.Step({ campaign1, 18, kRaidUpdate });
    t4.Step({ campaign1, kRaidStart, kRaidStart });  // 下一次探索
    CHECK(t4.RaidSerial() == 2);
    ddc::PhaseTracker t5(ids);  // 状态名表里没找到 as_raid_start（-1）时不计数
    t5.Step({ campaign1, 6, 6 });
    CHECK(t5.RaidSerial() == 0);

    // 战斗序号（审核 B-B1）：「没有战斗」之后看到状态非零才加一，同一次探索的两场战斗序号不同
    const auto battle = [](bool raid, int state, std::uint32_t counter, std::uint32_t monsters) {
        ddc::BattleSample b;
        b.raid = raid;
        b.state = state;
        b.guidCounter = counter;
        b.monsters = monsters;
        return b;
    };
    ddc::PhaseTracker t6(raidIds);
    CHECK(t6.BattleSerial() == 0);
    CHECK(!t6.ObserveBattle(battle(false, 0, 0, 0)));  // 没有 Raid
    t6.Step({ campaign1, kRaidStart, kRaidStart });     // 探索 1
    CHECK(!t6.ObserveBattle(battle(true, 0, 0, 0)) && t6.BattleSerial() == 0);
    CHECK(t6.ObserveBattle(battle(true, 1, 2, 2)) && t6.BattleSerial() == 1);  // battle.start 回调里的服务调用：bs_create
    CHECK(!t6.ObserveBattle(battle(true, 5, 2, 2)));   // 同一场战斗的后续帧
    CHECK(!t6.ObserveBattle(battle(true, 5, 3, 3)));   // 召唤、波次：计数只增加
    CHECK(!t6.ObserveBattle(battle(true, 0, 3, 3)));   // 撤退途中先置 bs_none（5A93B3），怪物表还在：不算没有战斗
    CHECK(!t6.ObserveBattle(battle(true, 2, 3, 3)) && t6.BattleSerial() == 1);
    CHECK(!t6.ObserveBattle(battle(true, 0, 3, 0)));   // 战斗销毁：先清空怪物表，再置 bs_none
    CHECK(!t6.ObserveBattle(battle(true, 0, 3, 0)) && t6.BattleSerial() == 1);  // 走廊里停留多帧
    CHECK(t6.ObserveBattle(battle(true, 1, 2, 2)) && t6.BattleSerial() == 2);   // 同一次探索的第二场：同样的计数、同样的 guid
    CHECK(t6.ObserveBattle(battle(true, 1, 1, 1)) && t6.BattleSerial() == 3);   // 兜底：漏看了「没有战斗」，guid 计数变小
    t6.Step({ campaign1, kRaidUpdate, kRaidUpdate });
    t6.Step({ campaign1, kRaidStart, kRaidStart });     // 下一次探索：新的 Raid，读档恢复的战斗直接处在非零状态
    CHECK(t6.RaidSerial() == 2 && t6.ObserveBattle(battle(true, 7, 5, 5)) && t6.BattleSerial() == 4);
    CHECK(!t6.ObserveBattle(battle(true, 7, 5, 5)) && !t6.ObserveBattle(battle(false, 0, 0, 0)) && t6.BattleSerial() == 4);

    // 第二轮审核 R2B-备注1：Battle::Create 清零 guid 计数、填好怪物表之后，要到 5A4722 才置 bs_create；
    // 中间的服务调用看到「状态 0、怪物表不空」，这时就要拿到新序号，之后看到 bs_create 不再加一。
    // （5A45B2 派发 heal.pre/heal.post 的循环只在 flashback 战斗里执行，见 knowledge/dd1/combat.md，第三轮 H2 核对）
    ddc::PhaseTracker t7(raidIds);
    t7.Step({ campaign1, kRaidStart, kRaidStart });
    CHECK(t7.ObserveBattle(battle(true, 1, 3, 3)) && t7.BattleSerial() == 1);   // 第一场，3 个怪物
    CHECK(!t7.ObserveBattle(battle(true, 5, 4, 4)));                            // 召唤：计数 4
    CHECK(!t7.ObserveBattle(battle(true, 0, 4, 0)));                            // 销毁
    CHECK(t7.ObserveBattle(battle(true, 0, 2, 2)) && t7.BattleSerial() == 2);   // 第二场（怪物更少）：bs_create 之前的 heal.* 回调
    CHECK(!t7.ObserveBattle(battle(true, 1, 2, 2)) && t7.BattleSerial() == 2);  // bs_create、battle.start：不再加一
    CHECK(!t7.ObserveBattle(battle(true, 0, 2, 0)));                            // 销毁
    CHECK(t7.ObserveBattle(battle(true, 0, 4, 4)) && t7.BattleSerial() == 3);   // 第三场（怪物更多，计数没变小）：空闲之后怪物表不空
    CHECK(!t7.ObserveBattle(battle(true, 1, 4, 4)) && t7.BattleSerial() == 3);
    CHECK(!t7.ObserveBattle(battle(true, 5, 6, 4)));                            // 召唤：计数 6
    // 漏看了销毁（没有空闲），bs_create 之前计数变小：不管状态都加一，之后 bs_create 不再加一
    CHECK(t7.ObserveBattle(battle(true, 0, 1, 1)) && t7.BattleSerial() == 4);
    CHECK(!t7.ObserveBattle(battle(true, 1, 1, 1)) && t7.BattleSerial() == 4);
    // 计数清零之后、填怪物表之前看到一次（状态 0、表空、计数 0）：加一；填表、bs_create 不再加一
    CHECK(!t7.ObserveBattle(battle(true, 0, 1, 0)));
    CHECK(t7.ObserveBattle(battle(true, 0, 0, 0)) && t7.BattleSerial() == 5);
    CHECK(!t7.ObserveBattle(battle(true, 0, 2, 2)) && !t7.ObserveBattle(battle(true, 1, 2, 2)) && t7.BattleSerial() == 5);
    // 撤退后重进同一格：7730C0 经 60E1C0 从存档恢复怪物（计数也恢复，不一定变小），7734F0 直接把状态写成 1，
    // 不派发 battle.start。照样是新序号，上一场的句柄失效
    CHECK(!t7.ObserveBattle(battle(true, 0, 2, 2)));                            // 撤退途中先置 bs_none，表还在
    CHECK(!t7.ObserveBattle(battle(true, 0, 2, 0)));                            // 销毁
    CHECK(t7.ObserveBattle(battle(true, 0, 2, 2)) && t7.BattleSerial() == 6);   // 恢复的怪物进表
    CHECK(!t7.ObserveBattle(battle(true, 1, 2, 2)) && t7.BattleSerial() == 6);  // 7734F0 写状态 1
    CHECK(!t7.ObserveBattle(battle(false, 0, 0, 0)) && t7.BattleSerial() == 6);  // 回城：没有 Raid
}

// ---------------------------------------------------------------- actor 解析（假内存布局）
namespace actortest {

using namespace ddc::game::actor_layout;
using ddc::game::ActorEnv;

struct Vec3 {
    std::uintptr_t first, last, end;
};

template <class T>
void Put(std::vector<std::uint8_t>& buffer, std::uintptr_t offset, const T& value)
{
    std::memcpy(buffer.data() + offset, &value, sizeof(T));
}

void PutVec(std::vector<std::uint8_t>& buffer, std::uintptr_t offset, const std::vector<std::uintptr_t>& items)
{
    const auto first = reinterpret_cast<std::uintptr_t>(items.data());
    Put(buffer, offset, Vec3{ first, first + items.size() * 8, first + items.size() * 8 });
}

std::uintptr_t Addr(const std::vector<std::uint8_t>& buffer, std::uintptr_t offset = 0)
{
    return reinterpret_cast<std::uintptr_t>(buffer.data()) + offset;
}

void* AsPtr(std::uintptr_t value)
{
    return reinterpret_cast<void*>(value);
}

const void* g_lastStat = nullptr;
float FakeMaxStat(const void* stat, std::uint32_t mask, bool clamp)
{
    g_lastStat = stat;
    return clamp ? 30.0f + static_cast<float>(mask) : -1.0f;
}

void* g_heroVt[4];
void* g_monsterVt[4];

// 名册条目：Hero 内嵌在 +8
std::vector<std::uint8_t> MakeEntry(std::uint32_t id, float hp, float stress, std::uint32_t mask, const std::vector<std::uint8_t>& cls)
{
    std::vector<std::uint8_t> entry(kEntryHero + 0x1400, 0);
    Put(entry, kEntryHero, reinterpret_cast<std::uintptr_t>(g_heroVt));
    Put(entry, kEntryHero + kHeroId, id);
    Put(entry, kEntryHero + kActorHp, hp);
    Put(entry, kEntryHero + kActorStress, stress);
    Put(entry, kEntryHero + kActorMaxHpMask, mask);
    Put(entry, kEntryHero + kHeroClass, Addr(cls));
    return entry;
}

std::vector<std::uint8_t> MakeMonster(std::uint32_t guid, float hp, const std::vector<std::uint8_t>& cls)
{
    std::vector<std::uint8_t> monster(0x1500, 0);
    Put(monster, 0, reinterpret_cast<std::uintptr_t>(g_monsterVt));
    Put(monster, kMonsterGuid, guid);
    Put(monster, kActorHp, hp);
    Put(monster, kMonsterClass, Addr(cls));
    return monster;
}

std::vector<std::uint8_t> MakeClass(const char* id)
{
    std::vector<std::uint8_t> cls(0x100, 0);
    std::memcpy(cls.data() + kClassId, id, std::strlen(id));
    return cls;
}

DDActorRef Ref()
{
    DDActorRef ref{};
    ref.size = sizeof(ref);
    return ref;
}

static void Run()
{
    using namespace ddc::game;
    const auto crusader = MakeClass("crusader");
    const auto skeleton = MakeClass("skeleton_soldier");
    auto e1 = MakeEntry(12, 10.0f, 33.5f, 2, crusader);
    auto e2 = MakeEntry(13, 20.0f, 0.0f, 0, crusader);
    auto alt = MakeEntry(12, 99.0f, 1.0f, 0, crusader);     // 备用名册里也有一个 12 号
    auto stray = MakeEntry(77, 1.0f, 0.0f, 0, crusader);    // 有 Hero 虚表但不在名册里
    auto captured = MakeEntry(14, 5.0f, 0.0f, 0, crusader); // 怪物表里的英雄
    auto m1 = MakeMonster(4500, 7.0f, skeleton);
    auto m2 = MakeMonster(4501, 8.0f, skeleton);

    std::vector<std::uintptr_t> roster = { Addr(e1), 0, Addr(e2) };
    std::vector<std::uintptr_t> altRoster = { Addr(alt) };
    std::vector<std::uintptr_t> battle = { Addr(m1), Addr(captured, kEntryHero), Addr(m2) };
    std::vector<std::uint8_t> campaign(0x800, 0);
    PutVec(campaign, kRosterHeroes, roster);
    PutVec(campaign, kRosterAlt + kRosterHeroes, altRoster);
    std::vector<std::uint8_t> raid(kRaidBattle + 0x400, 0);
    PutVec(raid, kRaidBattle + kBattleMonsters, battle);
    Put(raid, kRaidBattle + kBattleState, std::int32_t{ 5 });
    Put(raid, kRaidBattle + kBattleGuidCounter, std::uint32_t{ 2 });
    void* campaignGlobal = campaign.data();
    void* raidGlobal = raid.data();

    ActorEnv env;
    env.heroVtable = g_heroVt;
    env.monsterVtable = g_monsterVt;
    env.campaignCurrent = &campaignGlobal;
    env.raidCurrent = &raidGlobal;
    env.maxStat = reinterpret_cast<const void*>(&FakeMaxStat);
    env.campaignActive = true;
    env.campaignSerial = 3;
    env.raidSerial = 2;
    env.battleSerial = 7;
    CHECK(ActorKinds(env) == ((1u << DD_ACTOR_HERO) | (1u << DD_ACTOR_MONSTER)));

    void* const hero1 = AsPtr(Addr(e1, kEntryHero));
    void* const hero2 = AsPtr(Addr(e2, kEntryHero));
    void* const heroAlt = AsPtr(Addr(alt, kEntryHero));
    void* const monster1 = AsPtr(Addr(m1));

    // 指针转句柄
    DDActorRef h = Ref();
    CHECK(ActorFromPointer(env, hero1, &h));
    CHECK(h.kind == DD_ACTOR_HERO && h.id == 12 && h.campaign_serial == 3 && h.raid_serial == 0 && h.battle_serial == 0 && h.roster == 0);
    DDActorRef ha = Ref();
    CHECK(ActorFromPointer(env, heroAlt, &ha) && ha.id == 12 && ha.roster == 1);
    CHECK(ActorResolve(env, h) == hero1 && ActorResolve(env, ha) == heroAlt);  // 同一个 id、不同名册不混淆
    DDActorRef m = Ref();
    CHECK(ActorFromPointer(env, monster1, &m));
    CHECK(m.kind == DD_ACTOR_MONSTER && m.id == 4500 && m.raid_serial == 2 && m.battle_serial == 7 && m.campaign_serial == 3 && m.roster == 0);
    CHECK(ActorResolve(env, m) == monster1);
    DDActorRef bad = Ref();
    CHECK(!ActorFromPointer(env, AsPtr(Addr(stray, kEntryHero)), &bad));     // 不在名册
    CHECK(!ActorFromPointer(env, AsPtr(Addr(captured, kEntryHero)), &bad));  // 在怪物表但不是 Monster
    CHECK(!ActorFromPointer(env, crusader.data(), &bad));                     // 虚表不对
    CHECK(!ActorFromPointer(env, nullptr, &bad));
    CHECK(!ActorFromPointer(env, AsPtr(0x10), &bad));                         // 不可读（SEH）
    DDActorRef small{};
    small.size = 4;
    CHECK(!ActorFromPointer(env, hero1, &small));

    // 读取
    DDActorInfo info{};
    info.size = sizeof(info);
    CHECK(ActorReadInfo(env, h, &info));
    CHECK(info.kind == DD_ACTOR_HERO && info.id == 12 && info.hp == 10.0f && info.has_stress && info.stress == 33.5f);
    CHECK(info.has_max_hp && info.max_hp == 32.0f && g_lastStat == static_cast<const std::uint8_t*>(hero1) + kActorMaxHpStat);
    CHECK(std::strcmp(info.class_id, "crusader") == 0);
    CHECK(ActorReadInfo(env, m, &info));
    CHECK(info.kind == DD_ACTOR_MONSTER && info.hp == 7.0f && !info.has_stress && std::strcmp(info.class_id, "skeleton_soldier") == 0);
    DDActorInfo tiny{};
    tiny.size = 8;
    CHECK(!ActorReadInfo(env, h, &tiny));

    // 按 id 找
    DDActorRef f = Ref();
    CHECK(ActorFind(env, DD_ACTOR_HERO, 13, &f) && f.roster == 0 && ActorResolve(env, f) == hero2);
    CHECK(!ActorFind(env, DD_ACTOR_HERO, 999, &f));
    CHECK(!ActorFind(env, DD_ACTOR_MONSTER, 14, &f));  // 怪物表里的英雄不算怪物
    CHECK(ActorFind(env, DD_ACTOR_MONSTER, 4501, &f) && f.raid_serial == 2 && f.battle_serial == 7);

    // 枚举（F5）：队伍按 Raid+8 的顺序，每个指针都要在名册里；怪物按 Battle+0x38 的顺序，表里的英雄跳过
    {
        std::vector<std::uintptr_t> party = { Addr(e2, kEntryHero), Addr(stray, kEntryHero), 0, Addr(e1, kEntryHero), Addr(m1) };
        PutVec(raid, kRaidHeroes, party);
        DDActorRef list[4] = {};
        CHECK(ActorListParty(env, list, 4) == 2);
        CHECK(list[0].kind == DD_ACTOR_HERO && list[0].id == 13 && list[0].size == sizeof(DDActorRef) && list[0].campaign_serial == 3);
        CHECK(list[1].id == 12 && list[1].roster == 0 && ActorResolve(env, list[1]) == hero1);
        CHECK(ActorListParty(env, nullptr, 0) == 2);       // 只取个数
        DDActorRef one[1] = {};
        CHECK(ActorListParty(env, one, 1) == 2 && one[0].id == 13);  // 只写 capacity 个，返回总数
        std::vector<std::uintptr_t> altParty = { Addr(alt, kEntryHero) };
        PutVec(raid, kRaidHeroes, altParty);
        CHECK(ActorListParty(env, list, 4) == 1 && list[0].id == 12 && list[0].roster == 1);  // 备用名册里的英雄带 roster 1
        std::vector<std::uintptr_t> big(kMaxPartyHeroes + 1, Addr(e1, kEntryHero));
        PutVec(raid, kRaidHeroes, big);
        CHECK(ActorListParty(env, list, 4) == 0);          // 超过上限：当作结构读错
        Put(raid, kRaidHeroes, Vec3{ 0x10, 0x18, 0x18 });  // 元素不可读（SEH）
        CHECK(ActorListParty(env, list, 4) == 0);
        PutVec(raid, kRaidHeroes, party);
        CHECK(ActorListMonsters(env, list, 4) == 2);
        CHECK(list[0].kind == DD_ACTOR_MONSTER && list[0].id == 4500 && list[0].battle_serial == 7 && list[0].raid_serial == 2);
        CHECK(list[1].id == 4501 && ActorResolve(env, list[0]) == monster1);
        ActorEnv other = env;
        other.campaignActive = false;
        CHECK(ActorListParty(other, list, 4) == 0 && ActorListMonsters(other, list, 4) == 0);
        other = env;
        other.heroVtable = nullptr;
        CHECK(ActorListParty(other, list, 4) == 0 && ActorListMonsters(other, list, 4) == 2);
        void* noRaid = nullptr;
        other = env;
        other.raidCurrent = &noRaid;
        CHECK(ActorListParty(other, list, 4) == 0 && ActorListMonsters(other, list, 4) == 0);  // 城镇里没有 Raid
        void* badRaid = AsPtr(0x10);
        other.raidCurrent = &badRaid;
        CHECK(ActorListParty(other, list, 4) == 0 && ActorListMonsters(other, list, 4) == 0);
        Put(raid, kRaidHeroes, Vec3{ 0, 0, 0 });  // party 是这个块里的局部变量
    }

    // 战斗取样：状态、guid 计数、怪物表的元素个数（含表里的英雄）
    {
        const ddc::BattleSample b = ReadBattleSample(&raidGlobal);
        CHECK(b.raid && b.state == 5 && b.guidCounter == 2 && b.monsters == 3);
        void* noRaid = nullptr;
        const ddc::BattleSample none = ReadBattleSample(&noRaid);
        CHECK(!none.raid && none.state == 0 && none.monsters == 0);
        void* unreadable = AsPtr(0x10);
        CHECK(!ReadBattleSample(&unreadable).raid);
        CHECK(!ReadBattleSample(nullptr).raid);
    }

    // 序号：跨探索、跨存档、存档外都解析失败
    ActorEnv next = env;
    next.raidSerial = 3;
    CHECK(ActorResolve(next, m) == nullptr && ActorResolve(next, h) == hero1);
    // 同一次探索的下一场战斗（审核 B-B1）：同一个 guid、同一个对象地址也不算同一个怪物
    next = env;
    next.battleSerial = 8;
    CHECK(ActorResolve(next, m) == nullptr && ActorResolve(next, h) == hero1);
    DDActorRef again = Ref();
    CHECK(ActorFind(next, DD_ACTOR_MONSTER, 4500, &again) && again.battle_serial == 8 && ActorResolve(next, again) == monster1);
    CHECK(ActorResolve(env, again) == nullptr);
    next = env;
    next.campaignSerial = 4;
    CHECK(ActorResolve(next, h) == nullptr && ActorResolve(next, m) == nullptr);
    next = env;
    next.campaignActive = false;
    DDActorRef g = Ref();
    CHECK(ActorResolve(next, h) == nullptr && !ActorFind(next, DD_ACTOR_HERO, 12, &g));
    DDActorRef forged = h;
    forged.raid_serial = 2;  // 英雄的 raid_serial 必须是 0
    CHECK(ActorResolve(env, forged) == nullptr);
    forged = h;
    forged.battle_serial = 7;  // battle_serial 也是
    CHECK(ActorResolve(env, forged) == nullptr);
    forged = h;
    forged.kind = 9;
    CHECK(ActorResolve(env, forged) == nullptr);
    forged = h;
    forged.size = 0;
    CHECK(ActorResolve(env, forged) == nullptr);

    // 最大生命的函数缺失、职业 id 没有结尾、怪物表清空、vector 损坏
    next = env;
    next.maxStat = nullptr;
    CHECK(ActorReadInfo(next, h, &info) && !info.has_max_hp);
    const std::vector<std::uint8_t> longClass(0x100, 'a');
    Put(e1, kEntryHero + kHeroClass, Addr(longClass));
    CHECK(ActorReadInfo(env, h, &info) && info.class_id[0] == '\0');
    battle.clear();
    PutVec(raid, kRaidBattle + kBattleMonsters, battle);
    CHECK(ActorResolve(env, m) == nullptr);
    Put(campaign, kRosterHeroes, Vec3{ Addr(campaign, 0x100), Addr(campaign, 0x80), Addr(campaign, 0x100) });  // last < first
    CHECK(ActorResolve(env, h) == nullptr);
    raidGlobal = nullptr;
    CHECK(ActorResolve(env, f) == nullptr);

    // 特征码缺失时对应种类不可用
    next = env;
    next.monsterVtable = nullptr;
    CHECK(ActorKinds(next) == (1u << DD_ACTOR_HERO));
    next.heroVtable = nullptr;
    CHECK(ActorKinds(next) == 0 && !ActorFromPointer(next, hero1, &bad));
}

}  // namespace actortest

// ---------------------------------------------------------------- 事件中枢
namespace eventstest {

using ddc::events::FieldDesc;
using ddc::events::Hub;
using ddc::events::kAccessR;
using ddc::events::kAccessRW;
using ddc::events::Payload;
using ddc::events::Slot;
using ddc::events::Topic;
using ddc::events::TopicKind;

struct Ctx {
    std::unique_ptr<Hub> hub;
    std::vector<std::string> log;
    bool crashNext = false;
};

static std::vector<std::string> g_calls;
static Ctx* g_ctx = nullptr;

static std::unique_ptr<Ctx> MakeHub()
{
    auto ctx = std::make_unique<Ctx>();
    auto* raw = ctx.get();
    ctx->hub = std::make_unique<Hub>(
        [raw](DDEventFn fn, DDEvent* ev, void* user, std::uint32_t) -> bool {
            if (raw->crashNext) {
                raw->crashNext = false;
                return false;  // 模拟 SEH 捕获到崩溃
            }
            fn(ev, user);
            return true;
        },
        [raw](int, const std::string& message) { raw->log.push_back(message); });
    return ctx;
}

// 空主题的派发 helper
static bool Fire(Hub& hub, Topic* t, Payload* out = nullptr, std::uint32_t originThread = 1)
{
    static Slot empty[8];
    Payload payload;
    payload.slots = empty;
    payload.originThread = originThread;
    const bool ok = hub.Dispatch(t, payload);
    if (out) {
        *out = payload;
    }
    return ok;
}

static void DD_CALL RecordCb(DDEvent* ev, void* user)
{
    (void)ev;
    g_calls.push_back(static_cast<const char*>(user));
}

static void TestPriority()
{
    auto ctx = MakeHub();
    auto& hub = *ctx->hub;
    auto* t = hub.AddTopic("t", TopicKind::Custom, {}, false, false);
    g_calls.clear();
    std::string reason;
    CHECK(hub.Subscribe(1, "t", &RecordCb, (void*)"b0", 5, 0, &reason) != 0);
    CHECK(hub.Subscribe(1, "t", &RecordCb, (void*)"a", 0, 0, &reason) != 0);
    CHECK(hub.Subscribe(1, "t", &RecordCb, (void*)"b1", 5, 0, &reason) != 0);
    CHECK(hub.Subscribe(1, "t", &RecordCb, (void*)"c", -3, 0, &reason) != 0);
    Fire(hub, t);
    CHECK(g_calls.size() == 4);
    if (g_calls.size() == 4) {
        CHECK(g_calls[0] == "c" && g_calls[1] == "a" && g_calls[2] == "b0" && g_calls[3] == "b1");
    }
    // 不存在的主题
    CHECK(hub.Subscribe(1, "nope", &RecordCb, nullptr, 0, 0, &reason) == 0);
    CHECK(reason.find("主题不存在") != std::string::npos);
    // 不可用的主题
    hub.AddTopic("u", TopicKind::Native, {}, false, false);
    hub.SetAvailability(hub.Find("u"), false, "测试原因");
    reason.clear();
    CHECK(hub.Subscribe(1, "u", &RecordCb, nullptr, 0, 0, &reason) == 0);
    CHECK(reason.find("测试原因") != std::string::npos);
}

static Ctx* g_snapCtx = nullptr;
static Topic* g_snapTopic = nullptr;
static std::uint64_t g_victim = 0;
static int g_adds = 0;

static void DD_CALL SnapshotAddCb(DDEvent*, void*)
{
    g_calls.push_back("adder");
    g_adds++;
    if (g_adds == 1) {
        std::string reason;
        g_snapCtx->hub->Subscribe(1, "t", &RecordCb, (void*)"new", 100, 0, &reason);
    }
}
static void DD_CALL SnapshotKillCb(DDEvent*, void*)
{
    g_calls.push_back("killer");
    g_snapCtx->hub->Unsubscribe(1, g_victim);
}
static void TestSnapshot()
{
    auto ctx = MakeHub();
    g_snapCtx = ctx.get();
    auto& hub = *ctx->hub;
    auto* t = hub.AddTopic("t", TopicKind::Custom, {}, false, false);
    g_snapTopic = t;
    std::string reason;
    g_calls.clear();
    g_adds = 0;
    hub.Subscribe(1, "t", &SnapshotAddCb, nullptr, 0, 0, &reason);
    Fire(hub, t);
    // 派发中新增的订阅：本次不调用
    CHECK(std::find(g_calls.begin(), g_calls.end(), "new") == g_calls.end());
    g_calls.clear();
    Fire(hub, t);
    CHECK(g_calls.size() == 2 && g_calls[1] == "new");

    // 派发中取消的订阅：立即不再被调用
    auto ctx2 = MakeHub();
    g_snapCtx = ctx2.get();
    auto& hub2 = *ctx2->hub;
    auto* t2 = hub2.AddTopic("t", TopicKind::Custom, {}, false, false);
    g_calls.clear();
    hub2.Subscribe(1, "t", &SnapshotKillCb, nullptr, 0, 0, &reason);
    g_victim = hub2.Subscribe(1, "t", &RecordCb, (void*)"victim", 10, 0, &reason);
    Fire(hub2, t2);
    CHECK(g_calls.size() == 1 && g_calls[0] == "killer");
    g_calls.clear();
    Fire(hub2, t2);
    CHECK(g_calls.size() == 1 && g_calls[0] == "killer");
}

static Ctx* g_nestCtx = nullptr;
static Topic* g_nestTopic = nullptr;
static int g_nestCount = 0;
static void DD_CALL NestCb(DDEvent*, void*)
{
    ++g_nestCount;
    Fire(*g_nestCtx->hub, g_nestTopic);
}

static void TestNesting()
{
    auto ctx = MakeHub();
    g_nestCtx = ctx.get();
    auto& hub = *ctx->hub;
    g_nestTopic = hub.AddTopic("t", TopicKind::Custom, {}, false, false);
    std::string reason;
    hub.Subscribe(1, "t", &NestCb, nullptr, 0, 0, &reason);
    g_nestCount = 0;
    Fire(hub, g_nestTopic);
    CHECK(g_nestCount == DD_EVENT_MAX_DEPTH);
    CHECK(hub.Depth() == 0);
    bool sawDepthError = false;
    for (const auto& line : ctx->log) {
        if (line.find("嵌套深度") != std::string::npos) {
            sawDepthError = true;
        }
    }
    CHECK(sawDepthError);
}

static void TestScope()
{
    auto ctx = MakeHub();
    auto& hub = *ctx->hub;
    auto* t = hub.AddTopic("t", TopicKind::Custom, {}, false, false);
    std::string reason;
    g_calls.clear();
    hub.Subscribe(1, "t", &RecordCb, (void*)"p", 0, DD_EVENT_SCOPE_PLUGIN, &reason);
    hub.Subscribe(1, "t", &RecordCb, (void*)"c", 0, DD_EVENT_SCOPE_CAMPAIGN, &reason);
    hub.Subscribe(1, "t", &RecordCb, (void*)"r", 0, DD_EVENT_SCOPE_RAID, &reason);
    hub.Subscribe(1, "t", &RecordCb, (void*)"b", 0, DD_EVENT_SCOPE_BATTLE, &reason);
    CHECK(hub.ScopedCount(DD_EVENT_SCOPE_BATTLE) == 1);
    Fire(hub, t);
    CHECK(g_calls.size() == 4);
    CHECK(hub.EndScope(DD_EVENT_SCOPE_BATTLE) == 1);
    g_calls.clear();
    Fire(hub, t);
    CHECK(g_calls.size() == 3);
    CHECK(hub.EndScope(DD_EVENT_SCOPE_RAID) == 1);
    CHECK(hub.EndScope(DD_EVENT_SCOPE_CAMPAIGN) == 1);
    g_calls.clear();
    Fire(hub, t);
    CHECK(g_calls.size() == 1 && g_calls[0] == "p");
    CHECK(hub.ExternalSubscriberCount() == 1);
    CHECK(hub.RemoveOwner(1) == 1);
    CHECK(hub.ExternalSubscriberCount() == 0);
}

// 撤销通知（P3b）：作用域结束、崩溃时通知；主动取消、回滚不通知；OwnerCount 只数有效订阅
static std::vector<std::pair<std::string, std::uint32_t>> g_ends;
static void DD_CALL RecordEnd(DDEventSubId, void* user, std::uint32_t reason)
{
    g_ends.emplace_back(static_cast<const char*>(user), reason);
}

static void TestEndNotify()
{
    auto ctx = MakeHub();
    auto& hub = *ctx->hub;
    auto* t = hub.AddTopic("t", TopicKind::Custom, {}, false, false);
    std::string reason;
    g_ends.clear();
    const auto plain = hub.Subscribe(1, "t", &RecordCb, (void*)"plain", 0, DD_EVENT_SCOPE_BATTLE, &reason);
    hub.Subscribe(1, "t", &RecordCb, (void*)"battle", 0, DD_EVENT_SCOPE_BATTLE, &reason, false, &RecordEnd);
    hub.Subscribe(2, "t", &RecordCb, (void*)"raid", 0, DD_EVENT_SCOPE_RAID, &reason, false, &RecordEnd);
    const auto mine = hub.Subscribe(1, "t", &RecordCb, (void*)"mine", 0, DD_EVENT_SCOPE_PLUGIN, &reason, false, &RecordEnd);
    hub.Subscribe(2, "t", &RecordCb, (void*)"owner2", 0, DD_EVENT_SCOPE_PLUGIN, &reason, false, &RecordEnd);
    CHECK(plain != 0 && mine != 0);
    CHECK(hub.OwnerCount(1) == 3 && hub.OwnerCount(2) == 2 && hub.OwnerCount(3) == 0);
    CHECK(hub.EndScope(DD_EVENT_SCOPE_BATTLE) == 2);
    CHECK(g_ends.size() == 1 && g_ends[0].first == "battle" && g_ends[0].second == DD_EVENT_END_SCOPE);
    CHECK(hub.OwnerCount(1) == 1);
    CHECK(hub.Unsubscribe(1, mine));  // 主动取消不通知
    CHECK(g_ends.size() == 1 && hub.OwnerCount(1) == 0);
    CHECK(hub.RemoveOwner(2) == 2);  // 回滚不通知
    CHECK(g_ends.size() == 1 && hub.OwnerCount(2) == 0);

    // 崩溃：通知 DD_EVENT_END_CRASH，经过 SetEndInvoke 设置的调用方式
    int viaInvoker = 0;
    hub.SetEndInvoke([&](DDEventEndFn fn, std::uint64_t id, void* user, std::uint32_t why, std::uint32_t owner) {
        ++viaInvoker;
        CHECK(owner == 3);
        fn(id, user, why);
    });
    hub.Subscribe(3, "t", &RecordCb, (void*)"crash", 0, 0, &reason, false, &RecordEnd);
    ctx->crashNext = true;
    Fire(hub, t);
    CHECK(viaInvoker == 1 && g_ends.size() == 2 && g_ends[1].first == "crash" && g_ends[1].second == DD_EVENT_END_CRASH);
    CHECK(hub.OwnerCount(3) == 0);
}

static void TestCrashIsolation()
{
    auto ctx = MakeHub();
    auto& hub = *ctx->hub;
    auto* t = hub.AddTopic("t", TopicKind::Custom, {}, false, false);
    std::string reason;
    g_calls.clear();
    const auto id = hub.Subscribe(1, "t", &RecordCb, (void*)"x", 0, 0, &reason);
    hub.Subscribe(1, "t", &RecordCb, (void*)"y", 1, 0, &reason);
    ctx->crashNext = true;  // 第一个回调崩溃
    Fire(hub, t);
    CHECK(g_calls.size() == 1 && g_calls[0] == "y");
    CHECK(hub.Unsubscribe(1, id) == false);  // 已被取消
    g_calls.clear();
    Fire(hub, t);
    CHECK(g_calls.size() == 1 && g_calls[0] == "y");
}

static void DD_CALL WriteDamageCb(DDEvent* ev, void* user)
{
    auto* out = static_cast<double*>(user);
    auto* p = reinterpret_cast<Payload*>(ev);
    std::uint32_t idx = 0;
    const auto* d = Hub::FindField(p->topic, "damage", &idx);
    CHECK(d && d->type == DD_FIELD_FLOAT);
    *out = p->slots[idx].v.f;
    p->slots[idx].v.f = 7.0;
    p->slots[idx].set = true;
}

static void TestPayloadAndDeferred()
{
    auto ctx = MakeHub();
    auto& hub = *ctx->hub;
    std::vector<FieldDesc> fields = {
        FieldDesc{ "damage", DD_FIELD_FLOAT, kAccessRW },
        FieldDesc{ "note", DD_FIELD_STRING, kAccessR },
        FieldDesc{ "actor", DD_FIELD_POINTER, kAccessR },
    };
    auto* topic = hub.AddTopic("damage.pre", TopicKind::Semantic, fields, true, true);
    CHECK(topic && topic->writable && topic->cancellable && topic->phase == DD_EVENT_F_PRE);

    std::string reason;
    double seen = 0;
    hub.Subscribe(1, "damage.pre", &WriteDamageCb, &seen, 0, 0, &reason);
    Slot slots[3];
    slots[0].v.f = 3.5;
    slots[0].set = true;
    Payload payload;
    payload.slots = slots;
    payload.allowWrite = true;
    CHECK(hub.Dispatch(topic, payload));
    CHECK(seen == 3.5 && slots[0].v.f == 7.0);
    CHECK((payload.flags & DD_EVENT_F_PRE) != 0 && (payload.flags & DD_EVENT_F_WRITABLE) != 0);

    // 延迟队列：字符串深拷贝，指针清零，只读
    auto* t2 = hub.AddTopic("t2", TopicKind::Native, fields, false, false);
    static std::string received;
    static void* receivedPtr = reinterpret_cast<void*>(1);
    static std::uint32_t receivedFlags = 0;
    static std::uint32_t receivedOrigin = 0;
    static bool receivedWritable = true;
    hub.Subscribe(
        1, "t2",
        [](DDEvent* ev, void*) {
            auto* p = reinterpret_cast<Payload*>(ev);
            received = p->slots[1].v.s ? p->slots[1].v.s : "";
            receivedPtr = p->slots[2].v.p;
            receivedFlags = p->flags;
            receivedOrigin = p->originThread;
            receivedWritable = p->allowWrite;
        },
        nullptr, 0, 0, &reason);
    {
        char temp[32] = "volatile string";
        Slot source[3];
        source[1].v.s = temp;
        source[1].set = true;
        source[2].v.p = temp;
        source[2].set = true;
        hub.EnqueueDeferred(t2, source, 4242);
        temp[0] = 'X';  // 改动原缓冲：副本不受影响
    }
    CHECK(hub.DeferredCount() == 1);
    CHECK(hub.DrainDeferred() == 1);
    CHECK(hub.DeferredCount() == 0);
    CHECK(received == "volatile string");
    CHECK(receivedPtr == nullptr);
    CHECK((receivedFlags & DD_EVENT_F_DEFERRED) != 0);
    CHECK(receivedOrigin == 4242);
    CHECK(receivedWritable == false);
}

// 写入的取值范围（审核 B-Y1、B-Y2）：按字段在游戏里的宽度检查，超出返回 DD_EV_E_RANGE 且不写入；
// native 插件（events 服务）和 DDLua 都经过 access.h
static void TestWriteRange()
{
    using ddc::events::FieldRange;
    namespace access = ddc::events::access;
    auto ctx = MakeHub();
    auto& hub = *ctx->hub;
    auto* topic = hub.AddTopic("range.pre", TopicKind::Semantic,
        { FieldDesc{ "f32", DD_FIELD_FLOAT, kAccessRW, FieldRange::F32 }, FieldDesc{ "i32", DD_FIELD_INT, kAccessRW, FieldRange::I32 },
            FieldDesc{ "u32", DD_FIELD_INT, kAccessRW, FieldRange::U32 }, FieldDesc{ "f64", DD_FIELD_FLOAT, kAccessRW },
            FieldDesc{ "i64", DD_FIELD_INT, kAccessRW } },
        true, true);
    Slot slots[5];
    Payload payload;
    payload.topic = topic;
    payload.slots = slots;
    payload.allowWrite = true;
    auto* ev = reinterpret_cast<DDEvent*>(&payload);
    const double fltMax = 3.4028234663852886e38;
    CHECK(access::SetFloat(ev, "f32", fltMax) == DD_EV_OK && slots[0].v.f == fltMax);
    CHECK(access::SetFloat(ev, "f32", -fltMax) == DD_EV_OK);
    CHECK(access::SetFloat(ev, "f32", 1.5) == DD_EV_OK && slots[0].v.f == 1.5);
    // 超出 float 的有限值写回游戏会变成 inf；nan、inf 也拒绝；拒绝时原值不变
    for (const double bad : { 1e39, -1e39, 3.5e38, 1e300, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() }) {
        CHECK(access::SetFloat(ev, "f32", bad) == DD_EV_E_RANGE);
    }
    CHECK(slots[0].v.f == 1.5);
    CHECK(access::SetInt(ev, "i32", 2147483647) == DD_EV_OK && access::SetInt(ev, "i32", -2147483647 - 1) == DD_EV_OK);
    CHECK(slots[1].v.i == -2147483647 - 1);
    for (const std::int64_t bad : { std::int64_t(2147483648LL), std::int64_t(-2147483649LL), std::int64_t((1LL << 32) + 5), std::int64_t(3000000000LL) }) {
        CHECK(access::SetInt(ev, "i32", bad) == DD_EV_E_RANGE);
    }
    CHECK(slots[1].v.i == -2147483647 - 1);
    CHECK(access::SetInt(ev, "u32", 0) == DD_EV_OK && access::SetInt(ev, "u32", 4294967295LL) == DD_EV_OK);
    CHECK(access::SetInt(ev, "u32", -1) == DD_EV_E_RANGE && access::SetInt(ev, "u32", 4294967296LL) == DD_EV_E_RANGE);
    // 不限宽度的字段（自定义主题）照收
    CHECK(access::SetFloat(ev, "f64", 1e300) == DD_EV_OK && access::SetInt(ev, "i64", (1LL << 40)) == DD_EV_OK);
    // 其余检查的顺序不变：类型不符、只读先于范围
    CHECK(access::SetInt(ev, "f32", 1) == DD_EV_E_TYPE);
    payload.allowWrite = false;
    CHECK(access::SetFloat(ev, "f32", 1e39) == DD_EV_E_READ_ONLY);
}

// 压力：随机增删订阅和嵌套，上万次
static Ctx* g_stressCtx = nullptr;
static Topic* g_stressTopics[4];
static std::vector<std::uint64_t> g_liveIds;
static std::uint64_t g_stressDispatches = 0;
static std::uint32_t g_rng = 12345;

static std::uint32_t Rand()
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

static void DD_CALL StressCb(DDEvent*, void* user)
{
    ++g_stressDispatches;
    auto& hub = *g_stressCtx->hub;
    const auto depth = reinterpret_cast<std::uintptr_t>(user);
    switch (Rand() % 6) {
    case 0: {
        std::string reason;
        const auto id = hub.Subscribe(1, g_stressTopics[Rand() % 4]->name, &StressCb,
            reinterpret_cast<void*>(depth + 1), static_cast<int>(Rand() % 5) - 2, 0, &reason);
        if (id) {
            g_liveIds.push_back(id);
        }
        break;
    }
    case 1:
        if (!g_liveIds.empty()) {
            const auto idx = Rand() % g_liveIds.size();
            hub.Unsubscribe(1, g_liveIds[idx]);
            g_liveIds.erase(g_liveIds.begin() + static_cast<std::ptrdiff_t>(idx));
        }
        break;
    case 2:
        if (depth < 8) {
            Slot empty[2];
            Payload nested;
            nested.slots = empty;
            hub.Dispatch(g_stressTopics[Rand() % 4], nested);
        }
        break;
    default:
        break;
    }
}

static void TestStress()
{
    auto ctx = MakeHub();
    g_stressCtx = ctx.get();
    auto& hub = *ctx->hub;
    for (int i = 0; i < 4; ++i) {
        g_stressTopics[i] = hub.AddTopic("s" + std::to_string(i), TopicKind::Custom, {}, false, false);
    }
    g_rng = 12345;
    g_liveIds.clear();
    g_stressDispatches = 0;
    std::string reason;
    for (int i = 0; i < 4; ++i) {
        g_liveIds.push_back(hub.Subscribe(1, g_stressTopics[i]->name, &StressCb, nullptr, 0, 0, &reason));
    }
    for (int i = 0; i < 50000; ++i) {
        if (g_liveIds.size() < 3) {  // 回调里随机取消可能把订阅清空，保持活跃
            g_liveIds.push_back(hub.Subscribe(1, g_stressTopics[Rand() % 4]->name, &StressCb, nullptr, 0, 0, &reason));
        }
        Fire(hub, g_stressTopics[Rand() % 4]);
    }
    CHECK(hub.Depth() == 0);
    CHECK(g_stressDispatches > 1000);  // 回调确实跑了很多次
    // 随机删除后，列表和 live id 完全一致
    std::size_t total = 0;
    for (int i = 0; i < 4; ++i) {
        total += hub.SubscriberCount(g_stressTopics[i]);
    }
    CHECK(total == g_liveIds.size());
    CHECK(hub.RemoveOwner(1) == g_liveIds.size());
}

static void Run()
{
    TestPriority();
    TestSnapshot();
    TestNesting();
    TestScope();
    TestEndNotify();
    TestCrashIsolation();
    TestPayloadAndDeferred();
    TestWriteRange();
    TestStress();
}

}  // namespace eventstest

// ---------------------------------------------------------------- 装卸日志限速（第三轮审核 R3A-备注3）
// Mod 隔帧订阅、撤销同一个主题时 DDCore 每帧装卸一次 hook，每次一行 INFO。修复前每次都写：一分钟 3600 行
static void TestChurnLog()
{
    using ddc::events::ChurnLog;
    ChurnLog churn;
    std::vector<std::string> out;
    const auto write = [&](const std::string& line) { out.push_back(line); };
    int written = 0;
    std::uint64_t now = 1000;
    // 一分钟里每帧（16 ms）装卸一次 damage.pre：逐条只写 kVerbatim 行
    for (int frame = 0; frame < 3600; ++frame, now += 16) {
        written += churn.Allow("语义 hook damage.pre", frame % 2 == 0, now) ? 1 : 0;
        if (!churn.Idle()) {
            churn.Flush(now, write);
        }
    }
    std::printf("  每帧装卸一次、持续 %llu 秒：逐条 %d 行，汇总 %zu 行\n", static_cast<unsigned long long>((now - 1000) / 1000), written,
        out.size());
    CHECK(written == static_cast<int>(ChurnLog::kVerbatim));
    CHECK(out.size() == 0);  // 窗口还没结束（3600 帧 * 16 ms = 57.6 秒）
    // 窗口结束：写一行汇总，装卸次数都对
    now = 1000 + ChurnLog::kWindowMs;
    churn.Flush(now, write);
    CHECK(out.size() == 1);
    CHECK(!out.empty() && out[0].find("另外装上 1796 次、卸下 1796 次") != std::string::npos);
    if (!out.empty()) {
        std::printf("  汇总：%s\n", out[0].c_str());
    }
    CHECK(churn.Idle());
    // 别的名字不受影响；偶尔装卸（一个窗口里几次）照常逐条写
    CHECK(churn.Allow("listener HeroDismissedEvent", true, now));
    CHECK(churn.Allow("listener HeroDismissedEvent", false, now + 5000));
    // 新窗口：damage.pre 又能逐条写
    CHECK(churn.Allow("语义 hook damage.pre", true, now + 1));
    // 压下的行在新窗口开始时补出汇总
    for (std::uint32_t i = 0; i < ChurnLog::kVerbatim + 3; ++i) {
        churn.Allow("语义 hook stress.pre", i % 2 == 0, now + i);
    }
    CHECK(!churn.Idle());
    out.clear();
    churn.Allow("语义 hook stress.pre", true, now + ChurnLog::kWindowMs + 10);  // 开新窗口，上一个窗口的汇总排队
    churn.Flush(now + ChurnLog::kWindowMs + 10, write);
    CHECK(out.size() == 1 && out[0].find("stress.pre") != std::string::npos);
    CHECK(churn.Idle());
    // force：不等窗口结束，全部写出
    for (std::uint32_t i = 0; i < ChurnLog::kVerbatim + 2; ++i) {
        churn.Allow("语义 hook heal.pre", true, now + i);
    }
    out.clear();
    churn.Flush(now + 20, write, true);
    CHECK(out.size() == 1 && churn.Idle());
}

// ---------------------------------------------------------------- combat 服务的逻辑（P4a）
namespace combattest {

using namespace ddc::combat;

static DDActorRef Hero(std::uint32_t id, std::uint32_t campaign = 1)
{
    DDActorRef ref{};
    ref.size = sizeof(ref);
    ref.kind = DD_ACTOR_HERO;
    ref.id = id;
    ref.campaign_serial = campaign;
    return ref;
}

static DDActorRef Monster(std::uint32_t guid, std::uint32_t battle)
{
    DDActorRef ref{};
    ref.size = sizeof(ref);
    ref.kind = DD_ACTOR_MONSTER;
    ref.id = guid;
    ref.campaign_serial = 1;
    ref.raid_serial = 1;
    ref.battle_serial = battle;
    return ref;
}

static void TestHashAndEnums()
{
    CHECK(GameHash("") == 0);
    CHECK(GameHash("ab") == 97u * 53u + 98u);
    // 无符号 32 位回绕
    std::uint32_t h = 0;
    for (const char c : std::string("a_very_long_rule_name_for_overflow")) {
        h = h * 53u + static_cast<unsigned char>(c);
    }
    CHECK(GameHash("a_very_long_rule_name_for_overflow") == h);
    // 两个不同的名字哈希相同（规则注册要拒绝第二个）
    CHECK(GameHash("Ab") == GameHash("B-"));

    CHECK(EnumCount(DD_ENUM_STAT) == 102 && EnumCount(DD_ENUM_SOURCE) == 34 && EnumCount(DD_ENUM_DURATION) == 11 &&
        EnumCount(DD_ENUM_RULE) == 55);
    CHECK(EnumCount(9) == 0 && EnumName(9, 0) == nullptr && EnumValue(9, "always") == -1);
    CHECK(std::string(EnumName(DD_ENUM_STAT, 2)) == "combat_stat_add");
    CHECK(std::string(EnumName(DD_ENUM_STAT, 0x58)) == "hp_dot_burn");  // DOT 的值和 combat.md 的 [运行] 结论一致
    CHECK(std::string(EnumName(DD_ENUM_STAT, 101)) == "disable_combat_skill");
    CHECK(EnumName(DD_ENUM_STAT, 102) == nullptr && EnumName(DD_ENUM_STAT, -1) == nullptr);
    CHECK(EnumValue(DD_ENUM_RULE, "is_actor_status") == 42 && EnumValue(DD_ENUM_RULE, "actorStatus") == 13);
    CHECK(EnumValue(DD_ENUM_SOURCE, "skill") == 1 && EnumValue(DD_ENUM_SOURCE, "bsrc_skill") == 1);
    CHECK(std::string(EnumName(DD_ENUM_SOURCE, 33)) == "last_hero");
    CHECK(EnumValue(DD_ENUM_DURATION, "after_turn") == 9 && EnumValue(DD_ENUM_DURATION, "till_removed") == 6);
    CHECK(EnumValue(DD_ENUM_STAT, "no_such_stat") == -1 && EnumValue(DD_ENUM_STAT, "") == -1);
    // 来源表的三个标志（P4-0D：skill 三个都为真，notspecified 只回城清，quirk 都没有，companion 只进 tray）
    CHECK(SourceFlags(1) == (DD_SOURCE_F_SAVED | DD_SOURCE_F_TOWN_CLEAR | DD_SOURCE_F_TRAY));
    CHECK(SourceFlags(0) == DD_SOURCE_F_TOWN_CLEAR && SourceFlags(5) == 0 && SourceFlags(17) == DD_SOURCE_F_TRAY);
    CHECK(SourceFlags(26) == DD_SOURCE_F_SAVED && SourceFlags(34) == 0 && SourceFlags(-1) == 0);
    // 每张表名字和数值互查一致
    for (std::uint32_t t = 0; t < 4; ++t) {
        bool ok = true;
        for (std::uint32_t i = 0; i < EnumCount(t); ++i) {
            ok = ok && EnumValue(t, EnumName(t, static_cast<std::int32_t>(i))) == static_cast<std::int32_t>(i);
        }
        CHECK(ok);
    }
}

static DDBuffDef GoodDef()
{
    DDBuffDef def{};
    def.size = sizeof(def);
    strcpy_s(def.id, "lua_shield_carrier");
    def.stat_type = 2;
    strcpy_s(def.stat_sub_type, "speed_rating");
    def.amount = 1.0f;
    def.duration = -1;
    def.duration_type = 6;
    def.source = 1;
    def.rule_type = 42;
    strcpy_s(def.rule_string, "lua_shielded");
    return def;
}

static void TestValidation()
{
    std::string why;
    DDBuffDef def = GoodDef();
    CHECK(ValidateBuffDef(def, &why));
    def.duration = 3;
    CHECK(ValidateBuffDef(def, &why));
    def.duration = 0;
    CHECK(!ValidateBuffDef(def, &why) && why.find("duration") != std::string::npos);
    def = GoodDef();
    def.id[0] = 0;
    CHECK(!ValidateBuffDef(def, &why));
    def = GoodDef();
    std::memset(def.id, 'x', sizeof(def.id));  // 没有结尾的 0
    CHECK(!ValidateBuffDef(def, &why));
    def = GoodDef();
    strcpy_s(def.id, "bad\"id");
    CHECK(!ValidateBuffDef(def, &why));
    def = GoodDef();
    def.stat_type = 102;
    CHECK(!ValidateBuffDef(def, &why));
    // C-Y3 / A-N4：sub_stat 按 stat 校验
    def = GoodDef();
    strcpy_s(def.stat_sub_type, "speed");  // 常见笔误：应为 speed_rating
    CHECK(!ValidateBuffDef(def, &why) && why.find("speed_rating") != std::string::npos);
    def.stat_sub_type[0] = 0;
    CHECK(!ValidateBuffDef(def, &why) && why.find("不能为空") != std::string::npos);
    def.stat_type = 3;
    strcpy_s(def.stat_sub_type, "riposte_on_miss_chance");
    CHECK(ValidateBuffDef(def, &why));
    CHECK(ValidateSubStat(11, "death_blow", &why) && !ValidateSubStat(11, "", &why) && !ValidateSubStat(11, "deathblow", &why));
    CHECK(ValidateSubStat(32, "activity_lock", &why) && !ValidateSubStat(32, "lock", &why));
    CHECK(ValidateSubStat(33, "daze", &why) && !ValidateSubStat(33, "", &why));
    CHECK(ValidateSubStat(9, "", &why) && ValidateSubStat(30, "unkown", &why) && !ValidateSubStat(10, "unknown", &why));
    CHECK(ValidateSubStat(6, "", &why) && ValidateSubStat(98, "damage_heal", &why) && !ValidateSubStat(7, "heal", &why));
    CHECK(ValidateSubStat(101, "any_skill_id", &why) && ValidateSubStat(0, "whatever", &why) && ValidateSubStat(26, "", &why));
    def = GoodDef();
    def.rule_type = 55;
    CHECK(!ValidateBuffDef(def, &why));
    def = GoodDef();
    def.source = -1;
    CHECK(!ValidateBuffDef(def, &why));
    def = GoodDef();
    def.duration_type = 11;
    CHECK(!ValidateBuffDef(def, &why));
    def = GoodDef();
    def.amount = std::numeric_limits<float>::quiet_NaN();
    CHECK(!ValidateBuffDef(def, &why));

    DDBuffAddOptions options{};
    options.size = sizeof(options);
    CHECK(ValidateAddOptions(options, &why));
    options.duration = 0;  // 没有 DD_BUFF_ADD_DURATION 时不检查
    CHECK(ValidateAddOptions(options, &why));
    options.flags = DD_BUFF_ADD_DURATION;
    CHECK(!ValidateAddOptions(options, &why));
    options.duration = 2;
    CHECK(ValidateAddOptions(options, &why));
    options.flags |= DD_BUFF_ADD_SOURCE;
    options.source = 34;
    CHECK(!ValidateAddOptions(options, &why));
    options.source = 6;
    options.flags |= DD_BUFF_ADD_DURATION_TYPE | DD_BUFF_ADD_NO_REFRESH | DD_BUFF_ADD_SOURCEDATA;
    options.duration_type = 10;
    CHECK(ValidateAddOptions(options, &why));
    options.flags |= 0x100;
    CHECK(!ValidateAddOptions(options, &why));

    DDBuffRemoveSpec spec{};
    spec.size = sizeof(spec);
    CHECK(ValidateRemoveSpec(spec, &why));
    spec.match = DD_BUFF_BY_SOURCE;
    spec.source = 40;
    CHECK(!ValidateRemoveSpec(spec, &why));
    spec.source = 13;
    CHECK(ValidateRemoveSpec(spec, &why));
    spec.match = 7;
    CHECK(!ValidateRemoveSpec(spec, &why));

    CHECK(ValidText("abc", false) && !ValidText("", false) && ValidText("", true));
    CHECK(!ValidText(std::string(64, 'a'), false) && ValidText(std::string(63, 'a'), false));
    CHECK(!ValidText("a\nb", false));
}

static void TestTiming()
{
    TimingState s;
    s.mainThread = true;
    s.campaignActive = true;
    s.inFrameBody = true;
    auto d = DecideTiming(s, true);
    CHECK(d.timing == Timing::Execute && d.code == DD_COMBAT_OK && IsSafePoint(s));
    // 事件派发中、DDCore 自己的游戏调用里、帧边界之外（游戏代码在栈上）：排队
    s.dispatchDepth = 1;
    d = DecideTiming(s, true);
    CHECK(d.timing == Timing::Queue && d.code == DD_COMBAT_QUEUED && !IsSafePoint(s));
    s.dispatchDepth = 0;
    s.gameCallDepth = 1;
    CHECK(DecideTiming(s, true).timing == Timing::Queue);
    s.gameCallDepth = 0;
    s.inFrameBody = false;
    CHECK(DecideTiming(s, true).timing == Timing::Queue);
    s.inFrameBody = true;
    // 拒绝的情况，规则回调优先于一切（不排队）
    TimingState r = s;
    r.inRule = true;
    r.dispatchDepth = 3;
    d = DecideTiming(r, true);
    CHECK(d.timing == Timing::Reject && d.code == DD_COMBAT_E_IN_RULE);
    r = s;
    r.mainThread = false;
    CHECK(DecideTiming(r, true).code == DD_COMBAT_E_THREAD);
    r = s;
    r.campaignActive = false;
    CHECK(DecideTiming(r, true).code == DD_COMBAT_E_NO_CAMPAIGN);
    CHECK(DecideTiming(r, false).timing == Timing::Execute);
    r = s;
    r.loading = true;
    r.dispatchDepth = 1;  // 加载中即使在事件回调里也拒绝，不排队
    CHECK(DecideTiming(r, true).code == DD_COMBAT_E_LOADING);
    r = s;
    r.pvp = true;
    CHECK(DecideTiming(r, true).code == DD_COMBAT_E_PVP);
}

static void TestQueue()
{
    OpQueue queue(3);
    QueuedOp op;
    op.owner = 1;
    op.text = "a";
    const auto id1 = queue.Push(op);
    op.text = "b";
    const auto id2 = queue.Push(op);
    op.text = "c";
    const auto id3 = queue.Push(op);
    CHECK(id1 == 1 && id2 == 2 && id3 == 3);
    op.text = "d";
    CHECK(queue.Push(op) == 0);  // 插件 1 超过上限
    QueuedOp other;
    other.owner = 2;
    other.kind = OpKind::EffectApply;
    CHECK(queue.Push(other) == 4);  // 别的插件不受影响
    CHECK(queue.Size() == 4 && queue.OwnerCount(1) == 3 && queue.OwnerCount(2) == 1);
    auto taken = queue.TakeAll();
    CHECK(taken.size() == 4 && taken[0].text == "a" && taken[1].text == "b" && taken[2].text == "c" && taken[3].owner == 2);
    CHECK(taken[3].kind == OpKind::EffectApply && taken[0].id == 1 && taken[3].id == 4);
    CHECK(queue.Size() == 0 && queue.OwnerCount(1) == 0);
    CHECK(queue.Push(op) == 5);  // 取走以后计数清零，操作号继续增长
    queue.Push(other);
    CHECK(queue.DropOwner(1) == 1 && queue.Size() == 1 && queue.OwnerCount(1) == 0);
}

static void TestTags()
{
    TagTable tags;
    const DDActorRef h1 = Hero(7);
    const DDActorRef h2 = Hero(8);
    tags.Set(h1, 10, 1, "shield");
    tags.Set(h1, 11, 1, "shield");
    tags.Set(h1, 12, 2, "shield");
    tags.Set(h1, 13, 1, "");
    tags.Set(h2, 10, 1, "shield");
    CHECK(tags.Size() == 5);
    const TagEntry* e = tags.Get(h1, 12);
    CHECK(e && e->owner == 2 && e->tag == "shield");
    CHECK(tags.Get(h1, 99) == nullptr);
    auto found = tags.Find(h1, 1, "shield");
    CHECK(found.size() == 2 && found[0] == 10 && found[1] == 11);  // 只要本插件的，别的 actor 不算
    CHECK(tags.Find(h1, 2, "shield").size() == 1 && tags.Find(h1, 1, "none").empty());
    // 句柄的存档序号不同就是另一个 actor
    CHECK(tags.Find(Hero(7, 2), 1, "shield").empty());
    // 条目不在了的 guid 删掉
    CHECK(tags.PruneActor(h1, { 11, 13 }) == 2);
    CHECK(tags.Get(h1, 10) == nullptr && tags.Get(h1, 11) && tags.Get(h1, 13) && tags.Get(h2, 10));
    // 怪物：战斗序号变了，旧句柄的记录在战斗结束时清掉
    tags.Set(Monster(4500, 1), 1, 1, "x");
    tags.Set(Monster(4500, 2), 1, 1, "y");
    const std::size_t removed = tags.PruneIf([](const DDActorRef& ref) { return ref.kind == DD_ACTOR_MONSTER && ref.battle_serial != 2; });
    CHECK(removed == 1 && tags.Get(Monster(4500, 2), 1) && !tags.Get(Monster(4500, 1), 1));
    CHECK(tags.Erase(h1, 13) && !tags.Erase(h1, 13));
    CHECK(tags.DropOwner(1) == 3 && tags.Size() == 0);
}

// 标签表随存档保存（P4c1）：导出只留还能解析、条目还在的；导入按当前存档重新取句柄，对不上的丢弃
static void TestTagPersist()
{
    // 假的游戏：英雄 7 在名册里，条目 10（lua_shield）、11（SPD5）；英雄 8 解雇了；怪物 4500 在当前战斗（序号 2）里，条目 3
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::string> entries = { { { 7, 10 }, "lua_shield" }, { { 7, 11 }, "SPD5" }, { { 4500, 3 }, "bleed" } };
    TagLookup lookup;
    lookup.findActor = [](std::uint32_t kind, std::uint32_t id, DDActorRef* out) {
        if ((kind == DD_ACTOR_HERO && id == 7) || (kind == DD_ACTOR_MONSTER && id == 4500)) {
            *out = kind == DD_ACTOR_HERO ? Hero(id, 3) : Monster(id, 2);  // 读档后：存档序号 3，战斗序号 2
            return true;
        }
        return false;
    };
    lookup.entry = [&](const DDActorRef& ref, std::uint32_t guid, std::string* id) {
        if ((ref.kind == DD_ACTOR_HERO && ref.id == 8) || (ref.kind == DD_ACTOR_MONSTER && ref.battle_serial != 2)) {
            return false;  // 解析不了
        }
        const auto it = entries.find({ ref.id, guid });
        if (it == entries.end()) {
            return false;
        }
        *id = it->second;
        return true;
    };
    lookup.pluginName = [](std::uint32_t owner) { return owner == 1 ? std::string("DDLua") : std::string("test_combat"); };
    lookup.ownerOf = [](const std::string& name, bool* found) -> std::uint32_t {
        *found = name == "DDLua" || name == "test_combat";
        return name == "DDLua" ? 1u : 2u;
    };
    TagTable live;
    live.Set(Hero(7), 10, 1, "p4c_persist:s1");
    live.Set(Hero(7), 12, 1, "gone");        // 条目已经删了
    live.Set(Hero(8), 1, 2, "fired");        // 英雄解雇了
    live.Set(Monster(4500, 2), 3, 2, "m");
    live.Set(Monster(4500, 1), 3, 2, "old");  // 上一场战斗
    const auto records = CollectTags(live, lookup);
    CHECK(records.size() == 2);
    CHECK(records[0].kind == DD_ACTOR_HERO && records[0].id == 7 && records[0].guid == 10 && records[0].buffId == "lua_shield" &&
        records[0].plugin == "DDLua" && records[0].tag == "p4c_persist:s1");
    CHECK(records[1].kind == DD_ACTOR_MONSTER && records[1].id == 4500 && records[1].plugin == "test_combat" && records[1].buffId == "bleed");

    // 读档：再加几条对不上的
    std::vector<TagRecord> saved = records;
    saved.push_back({ DD_ACTOR_HERO, 7, 0, 11, "lua_shield", "DDLua", "wrong_id" });   // guid 在，id 变了
    saved.push_back({ DD_ACTOR_HERO, 7, 0, 99, "lua_shield", "DDLua", "missing" });    // 条目不在
    saved.push_back({ DD_ACTOR_HERO, 9, 0, 1, "x", "DDLua", "no_hero" });              // 英雄不在
    saved.push_back({ DD_ACTOR_HERO, 7, 1, 10, "lua_shield", "DDLua", "alt" });        // 备用名册
    saved.push_back({ DD_ACTOR_HERO, 7, 0, 10, "lua_shield", "gone_plugin", "p" });    // 插件没加载
    TagTable restored;
    std::vector<std::string> notes;
    CHECK(RestoreTags(restored, saved, 1u << DD_ACTOR_HERO, lookup, &notes) == 1);  // 只恢复英雄
    CHECK(restored.Size() == 1 && restored.Get(Hero(7, 3), 10) && restored.Get(Hero(7, 3), 10)->tag == "p4c_persist:s1" && restored.Get(Hero(7, 3), 10)->owner == 1);
    CHECK(notes.size() == 5);
    std::string all;
    for (const auto& n : notes) {
        all += n + "\n";
    }
    CHECK(all.find("id 对不上（现在是 SPD5）") != std::string::npos && all.find("条目不在") != std::string::npos && all.find("actor 找不到") != std::string::npos);
    CHECK(all.find("备用名册") != std::string::npos && all.find("插件没有加载") != std::string::npos);
    // 怪物：raid 核对通过以后按当前战斗的句柄恢复
    notes.clear();
    CHECK(RestoreTags(restored, saved, 1u << DD_ACTOR_MONSTER, lookup, &notes) == 1 && notes.empty());
    CHECK(restored.Get(Monster(4500, 2), 3) && restored.Get(Monster(4500, 2), 3)->owner == 2);
    CHECK(RestoreTags(restored, saved, 0, lookup, nullptr) == 0);
}

static int g_ruleCalls = 0;
static uint32_t DD_CALL RuleTrue(const DDRuleContext*, void*)
{
    ++g_ruleCalls;
    return DD_RULE_TRUE;
}

static void TestRules()
{
    RuleRegistry rules;
    const RuleSnapshot* empty = rules.Snapshot();
    CHECK(empty && empty->entries.empty() && empty->Find(GameHash("x")) == nullptr);
    std::string why;
    const DDRuleId a = rules.Register(1, "lua_shielded", &RuleTrue, nullptr, &why);
    const DDRuleId b = rules.Register(2, "Ab", &RuleTrue, nullptr, &why);
    CHECK(a != 0 && b != 0 && a != b && rules.Count() == 2);
    CHECK(rules.Register(3, "lua_shielded", &RuleTrue, nullptr, &why) == 0 && why.find("已经注册") != std::string::npos);
    CHECK(rules.Register(3, "B-", &RuleTrue, nullptr, &why) == 0 && why.find("哈希相同") != std::string::npos);
    // A-N3：游戏内置的状态名（479F41 起比较的全局哈希，静态初始化写入的值）不能注册
    CHECK(GameHash("stunned") == 0xF9DA5CF5u && GameHash("tagged") == 0x7A03648Cu && GameHash("poisoned") == 0x3D3CFC95u &&
        GameHash("bleeding") == 0x3830C076u && GameHash("burning") == 0x56AFD351u && GameHash("virtued") == 0x619A4DE3u &&
        GameHash("afflicted") == 0x77CDD742u && GameHash("dazed") == 0x2FE99588u);
    for (const char* builtin : { "stunned", "tagged", "poisoned", "bleeding", "burning", "virtued", "afflicted", "dazed" }) {
        CHECK(rules.Register(3, builtin, &RuleTrue, nullptr, &why) == 0 && why.find("内置") != std::string::npos);
    }
    CHECK(rules.Register(3, "", &RuleTrue, nullptr, &why) == 0);
    CHECK(rules.Register(3, "no_fn", nullptr, nullptr, &why) == 0);
    const RuleSnapshot* snap = rules.Snapshot();
    CHECK(snap != empty && snap->entries.size() == 2);
    const RuleEntry* hit = snap->Find(GameHash("lua_shielded"));
    CHECK(hit && hit->owner == 1 && hit->name == "lua_shielded" && hit->fn(nullptr, nullptr) == DD_RULE_TRUE && g_ruleCalls == 1);
    CHECK(snap->Find(GameHash("lua_other")) == nullptr);
    // 撤销：只能撤销自己的；旧快照仍然可读，条目被标成停用
    CHECK(!rules.Unregister(2, a));
    CHECK(rules.Unregister(1, a) && rules.Count() == 1);
    CHECK(hit->disabled.load() && snap->entries.size() == 2);
    CHECK(rules.Snapshot()->Find(GameHash("lua_shielded")) == nullptr);
    CHECK(rules.Register(3, "lua_shielded", &RuleTrue, nullptr, &why) != 0);  // 撤销后可以重新注册
    CHECK(rules.DropOwner(2) == 1 && rules.DropOwner(3) == 1 && rules.Count() == 0 && rules.Snapshot()->entries.empty());

    // 回调的三种结果（P4c1 遗留修正）：成立 / 不成立按 is_false_rule 取反，交还原函数和不认识的值都交给游戏
    bool result = false;
    CHECK(InterpretRule(DD_RULE_TRUE, false, &result) && result);
    CHECK(InterpretRule(DD_RULE_TRUE, true, &result) && !result);
    CHECK(InterpretRule(DD_RULE_FALSE, false, &result) && !result);
    CHECK(InterpretRule(DD_RULE_FALSE, true, &result) && result);
    result = true;
    CHECK(!InterpretRule(DD_RULE_ORIGINAL, false, &result) && result);  // 不写 result
    CHECK(!InterpretRule(DD_RULE_ORIGINAL, true, &result) && !InterpretRule(7, false, &result));

    RuleStatsAccum stats;
    stats.Add(10.0);
    stats.Add(30.0);
    stats.EndFrame();
    stats.Add(5.0);
    CHECK(stats.hits == 3 && stats.maxCallUs == 30.0 && stats.maxFrameUs == 40.0 && stats.maxFrameHits == 2 && stats.frameHits == 1);
    stats.EndFrame();
    CHECK(stats.maxFrameUs == 40.0 && stats.frameUs == 0.0);
}

static void TestEffectText()
{
    std::vector<std::string> names;
    std::string why;
    CHECK(ExtractEffectNames("effect: .name \"lua_fx_a\" .target \"performer\" .chance 100%\r\n", &names, &why));
    CHECK(names.size() == 1 && names[0] == "lua_fx_a");
    CHECK(ExtractEffectNames("effect: .name \"a\" .target \"performer\" .chance 100%\neffect: .name b .target target .stress 5\n  effect: .name \"c\" .target \"global\"\n", &names, &why));
    CHECK(names.size() == 3 && names[0] == "a" && names[1] == "b" && names[2] == "c");
    // 值里的冒号让游戏从那里开始一个新块（引号开头的块是解析错误）
    CHECK(!ExtractEffectNames("effect: .name_suffix \"x\" .name \"real\" .keyStatus \"effect: no\"\n", &names, &why));
    CHECK(!ExtractEffectNames("effect: .name \"a\" .set_mode x:y .chance 100%\n", &names, &why) && why.find("不是 effect") != std::string::npos);
    CHECK(!ExtractEffectNames("effect: .target \"performer\"\n", &names, &why) && why.find(".name") != std::string::npos);
    CHECK(!ExtractEffectNames("effect: .name \"a\"\neffect: .name \"a\"\n", &names, &why) && why.find("重复") != std::string::npos);
    CHECK(!ExtractEffectNames("effect: .name \"Ab\"\neffect: .name \"B-\"\n", &names, &why) && why.find("哈希") != std::string::npos);
    CHECK(!ExtractEffectNames("effect: .name \"unterminated\n", &names, &why));
    CHECK(!ExtractEffectNames("no blocks here", &names, &why));
    CHECK(!ExtractEffectNames(("effect: .name \"" + std::string(64, 'x') + "\"\n").c_str(), &names, &why));

    // A-S1：游戏的 36F020 取最后一次出现的 .name，键后面不要求空白，# 注释里的也算，\v 也是空白
    CHECK(ExtractEffectNames("effect: .name \"a\" .name \"b\" .target \"performer\"\n", &names, &why) && names.size() == 1 && names[0] == "b");
    CHECK(ExtractEffectNames("effect: .name \"myfx\" .target \"performer\" .name\"rakebuff_5\"\r\n", &names, &why) &&
        names.size() == 1 && names[0] == "rakebuff_5");
    CHECK(ExtractEffectNames("effect: .name \"myfx\" .target \"performer\" .name2 x\r\n", &names, &why) && names.size() == 1 && names[0] == "2");
    CHECK(ExtractEffectNames("effect: .name \"mine\" .target \"performer\"\n# .name rakebuff_5\n", &names, &why) && names.size() == 1 && names[0] == "rakebuff_5");
    CHECK(ExtractEffectNames("effect: .name \"a\" .target \"performer\" .name\x0brakebuff_5\n", &names, &why) && names.size() == 1 && names[0] == "rakebuff_5");
    // 2901B0 先去掉 // 和 /* */ 注释（引号里的也算）
    CHECK(ExtractEffectNames("/* effect: .name \"x\" */ effect: .name \"y\" .target \"performer\" // .name \"z\"\n", &names, &why) && names.size() == 1 &&
        names[0] == "y");
    CHECK(ExtractEffectNames("effect: .name \"a\" .target \"performer\"\neffect: .name b .target target\n", &names, &why) && names.size() == 2);

    // A-B1：块（到下一个标识符之前，# 注释和空白都算）达到 1024 字节时游戏进程会被终止
    const auto block = [](std::size_t length) {
        std::string t = "effect: .name \"long\" .target \"performer\"";
        t += std::string(length - t.size(), ' ');
        return t;
    };
    std::vector<EffectBlock> blocks;
    CHECK(ParseEffectText(block(1023), &blocks, &why) && blocks.size() == 1 && blocks[0].length == 1023);
    CHECK(!ParseEffectText(block(1024), &blocks, &why) && why.find("1023") != std::string::npos);
    CHECK(ExtractEffectNames(block(1023) + "\neffect: .name b .target target", &names, &why) && names.size() == 2);
    CHECK(!ExtractEffectNames(block(1023) + " \neffect: .name b", &names, &why));  // 换行前的空格也在第一块里
    std::string fields = "effect: .name \"longfx\" .target \"performer\" .chance 100%";
    while (fields.size() < 1100) {
        fields += " .stress 1";
    }
    CHECK(!ExtractEffectNames(fields + "\r\n", &names, &why) && why.find("超过") != std::string::npos);
    CHECK(!ExtractEffectNames("effect: .name \"cfx\" .target \"performer\"\r\n# " + std::string(1100, 'x') + "\r\n", &names, &why));
    CHECK(!ExtractEffectNames("other: " + std::string(1100, 'x') + "\neffect: .name a\n", &names, &why) && why.find("第 1 块") != std::string::npos);
    CHECK(ExtractEffectNames("effect: .name \"cfx\" .target \"performer\"\r\n// " + std::string(1100, 'x') + "\r\n", &names, &why) && names.size() == 1);
    CHECK(ExtractEffectNames("# " + std::string(2000, 'x') + "\neffect: .name a .target target\n", &names, &why) && names.size() == 1);  // 块前面的注释不算

    // 列表字段的某一项以块末尾的引号开头：游戏算出 0 的缓冲大小，进程被终止
    CHECK(!ExtractEffectNames("effect: .name a .buff_ids \"x\" \"", &names, &why) && why.find(".buff_ids") != std::string::npos);
    CHECK(!ExtractEffectNames("effect: .name a .kill_enemy_types \"\neffect: .name b", &names, &why));
    CHECK(ExtractEffectNames("effect: .name a .target target .buff_ids \"x\"\neffect: .name b .target target .buff_ids \"y\" \"z\"", &names, &why) && names.size() == 2);
    CHECK(ExtractEffectNames("effect: .name a .target target .summon_monsters \"x\r\n", &names, &why));  // 没闭合但不是最后一个字符
    CHECK(ExtractEffectNames("effect: .name a .target target .buff_ids \"x\" # \xE4\xB8\xAD\n", &names, &why));  // 列表后面的中文注释：块不以引号结尾
    CHECK(!ExtractEffectNames("effect: .name a .buff_ids \xE4\xB8\xAD\"", &names, &why) && why.find("区域设置") != std::string::npos);

    // 其他会让解析器出问题的文本
    CHECK(!ExtractEffectNames("::effect: .name a", &names, &why) && why.find("死循环") != std::string::npos);
    CHECK(!ExtractEffectNames(std::string("effect: .name a\0 effect: .name b", 32), &names, &why));
    CHECK(!ExtractEffectNames("\xEF\xBB\xBF" "effect: .name a", &names, &why));  // BOM
    CHECK(!ExtractEffectNames("effect: .name \xE4\xB8\xAD\n", &names, &why) && why.find("区域设置") != std::string::npos);
    CHECK(ExtractEffectNames("effect: .name \"\xE4\xB8\xAD\" .target \"performer\" # \xE4\xB8\xAD\xE6\x96\x87\n", &names, &why) && names.size() == 1);
    // .target 缺少或认不出：游戏弹 assert（EffectTarget: Invalid type）
    CHECK(!ExtractEffectNames("effect: .name a\n", &names, &why) && why.find("缺少 .target") != std::string::npos);
    CHECK(!ExtractEffectNames("effect: .name a .target \"self\"\n", &names, &why) && why.find("self") != std::string::npos);
    CHECK(ExtractEffectNames("effect: .name a .target \"other_trinket\"\n", &names, &why));

    OwnerTable owners;
    CHECK(owners.OwnerOf(GameHash("a")) == 0);
    owners.Set(GameHash("a"), 3, "a");
    CHECK(owners.OwnerOf(GameHash("a")) == 3 && owners.NameOf(GameHash("a")) && *owners.NameOf(GameHash("a")) == "a");
    owners.Clear();
    CHECK(owners.Size() == 0);
}

static void Run()
{
    TestHashAndEnums();
    TestValidation();
    TestTiming();
    TestQueue();
    TestTags();
    TestTagPersist();
    TestRules();
    TestEffectText();
}

}  // namespace combattest

// ---------------------------------------------------------------- persist 旁路（P4c1）
namespace persisttest {

using namespace ddc::persist;

std::wstring g_root;

void RemoveTree(const std::wstring& dir)
{
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            const std::wstring path = dir + L"\\" + name;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveTree(path);
            } else {
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}

std::string ReadText(const std::wstring& path)
{
    std::string out;
    bool exists = false;
    ReadWhole(path, &out, &exists, nullptr);
    return exists ? out : std::string("<missing>");
}

void WriteRaw(const std::wstring& path, std::string_view bytes)
{
    const std::size_t slash = path.find_last_of(L'\\');
    EnsureDir(path.substr(0, slash));
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
    CloseHandle(file);
}

Header MakeHeader(std::uint64_t roster, bool inRaid = false, std::uint64_t raid = 0)
{
    Header h;
    h.plugin = "DDLua";
    h.key = "p4c_persist";
    h.version = "1.2.3";
    h.device = "steam_profile_4";
    h.account = "12345678";
    h.writtenMs = 1763100000123ull;
    h.inRaid = inRaid;
    h.files.push_back({ kRosterFile, roster, 7948 });
    if (inRaid) {
        h.files.push_back({ kRaidFile, raid, 5120 });
    }
    return h;
}

Observed MakeObserved(std::uint64_t roster, int inRaid = 0)
{
    Observed o;
    o.device = "steam_profile_4";
    o.rosterKnown = true;
    o.roster = { kRosterFile, roster, 7948 };
    o.inRaid = inRaid;
    return o;
}

Candidate MakeCandidate(const Header& h, std::string payload)
{
    Candidate c;
    c.exists = true;
    c.decoded = true;
    c.header = h;
    c.payload = std::move(payload);
    return c;
}

void TestHashAndKeys()
{
    CHECK(Fnv1a64("", 0) == 0xCBF29CE484222325ull);
    CHECK(Fnv1a64("a", 1) == 0xAF63DC4C8601EC8Cull);
    CHECK(Fnv1a64("foobar", 6) == 0x85944171F73967E8ull);
    CHECK(ValidKey("p4c_persist") && ValidKey("tags") && ValidKey("a.b-c_1") && ValidKey(std::string(63, 'k')));
    CHECK(!ValidKey("") && !ValidKey(".hidden") && !ValidKey("a/b") && !ValidKey("a\\b") && !ValidKey("a b") && !ValidKey(std::string(64, 'k')));
    CHECK(!ValidKey("..") && !ValidKey("中文"));
    CHECK(ValidDevice("steam_profile_4") && ValidDevice("profile_0") && !ValidDevice("") && !ValidDevice("a:b") && !ValidDevice("..\\x"));
}

void TestFormat()
{
    Header h = MakeHeader(0x3F6923FB884EEBCAull, true, 0x1122334455667788ull);
    h.inBattle = true;
    h.version = "1.0\n注入";  // 换行变成空格
    std::string payload("store\n{\"n\":1}\0bin\xff", 18);
    const std::string bytes = EncodeFile(h, payload);
    CHECK(bytes.rfind("DDSAVE 1\nplugin DDLua\nkey p4c_persist\n", 0) == 0);
    CHECK(bytes.find("file persist.roster.json 3F6923FB884EEBCA 7948\n") != std::string::npos);
    Header back;
    std::string data, why;
    CHECK(DecodeFile(bytes, &back, &data, &why));
    CHECK(data == payload && back.plugin == "DDLua" && back.key == "p4c_persist" && back.version == "1.0 注入");
    CHECK(back.device == "steam_profile_4" && back.account == "12345678" && back.writtenMs == 1763100000123ull && back.inRaid && back.inBattle);
    CHECK(back.files.size() == 2 && back.Find(kRaidFile) && back.Find(kRaidFile)->hash == 0x1122334455667788ull && back.Find(kRaidFile)->size == 5120);
    // 空数据
    CHECK(DecodeFile(EncodeFile(MakeHeader(1), ""), &back, &data, &why) && data.empty() && !back.inRaid && back.files.size() == 1);
    // 写到一半：数据被截断
    CHECK(!DecodeFile(bytes.substr(0, bytes.size() - 3), &back, &data, &why) && why.find("截断") != std::string::npos);
    // 头也没写完
    CHECK(!DecodeFile(bytes.substr(0, 40), &back, &data, &why) && why.find("截断") != std::string::npos);
    // 数据被改
    std::string flipped = bytes;
    flipped[flipped.size() - 1] ^= 0x01;
    CHECK(!DecodeFile(flipped, &back, &data, &why) && why.find("校验和") != std::string::npos);
    CHECK(!DecodeFile(bytes + "x", &back, &data, &why) && why.find("长") != std::string::npos);
    CHECK(!DecodeFile("hello\n\n", &back, &data, &why) && why.find("DDSAVE") != std::string::npos);
    CHECK(!DecodeFile("DDSAVE 2\n\n", &back, &data, &why) && why.find("格式版本") != std::string::npos);
    CHECK(!DecodeFile("DDSAVE 1\nplugin x\n\n", &back, &data, &why) && why.find("缺少") != std::string::npos);
    CHECK(!DecodeFile("", &back, &data, &why));
}

void TestDecide()
{
    const std::uint64_t r1 = 0x1111, r2 = 0x2222, r3 = 0x3333;
    const Candidate none;
    std::string why;
    // 当前一代一致
    Decision d = Decide(MakeCandidate(MakeHeader(r1), "cur"), MakeCandidate(MakeHeader(r2), "prev"), MakeObserved(r1));
    CHECK(d.status == DD_PERSIST_LOADED && d.which == 0 && d.reason.empty());
    // 只有上一代一致（旁路比游戏多写了一次：游戏的写盘没落地就退出）
    d = Decide(MakeCandidate(MakeHeader(r2), "cur"), MakeCandidate(MakeHeader(r1), "prev"), MakeObserved(r1));
    CHECK(d.status == DD_PERSIST_PREV && d.which == 1 && d.reason.find("名册的哈希不同") != std::string::npos);
    // 都不一致：不给数据，两代的原因都写出来
    d = Decide(MakeCandidate(MakeHeader(r2), "cur"), MakeCandidate(MakeHeader(r3), "prev"), MakeObserved(r1));
    CHECK(d.status == DD_PERSIST_MISMATCH && d.which == -1 && d.reason.find("当前一代") != std::string::npos &&
        d.reason.find("上一代") != std::string::npos);
    // 没有上一代
    d = Decide(MakeCandidate(MakeHeader(r2), "cur"), none, MakeObserved(r1));
    CHECK(d.status == DD_PERSIST_MISMATCH && d.reason.find("没有文件") != std::string::npos);
    // 无数据（新战役、从没写过）
    d = Decide(none, none, MakeObserved(r1));
    CHECK(d.status == DD_PERSIST_EMPTY && d.which == -1);
    // 没观察到名册的读取：不能判断
    Observed unknown = MakeObserved(r1);
    unknown.rosterKnown = false;
    d = Decide(MakeCandidate(MakeHeader(r1), "cur"), none, unknown);
    CHECK(d.status == DD_PERSIST_UNAVAILABLE && d.reason.find("名册") != std::string::npos);
    // 当前一代损坏（写到一半），上一代一致
    Candidate broken;
    broken.exists = true;
    broken.decoded = false;
    broken.error = "数据只有 3 字节（文件被截断，写到一半？）";
    d = Decide(broken, MakeCandidate(MakeHeader(r1), "prev"), MakeObserved(r1));
    CHECK(d.status == DD_PERSIST_PREV && d.reason.find("损坏") != std::string::npos);
    // 只有 .prev（当前一代丢了）
    d = Decide(none, MakeCandidate(MakeHeader(r1), "prev"), MakeObserved(r1));
    CHECK(d.status == DD_PERSIST_PREV);
    // 城镇和地牢对不上
    d = Decide(MakeCandidate(MakeHeader(r1, true, 9), "cur"), none, MakeObserved(r1, 0));
    CHECK(d.status == DD_PERSIST_MISMATCH && d.reason.find("地牢") != std::string::npos);
    d = Decide(MakeCandidate(MakeHeader(r1), "cur"), none, MakeObserved(r1, 1));
    CHECK(d.status == DD_PERSIST_MISMATCH && d.reason.find("城镇") != std::string::npos);
    d = Decide(MakeCandidate(MakeHeader(r1, true, 9), "cur"), none, MakeObserved(r1, 1));
    CHECK(d.status == DD_PERSIST_LOADED);
    // 设备、大小
    Header other = MakeHeader(r1);
    other.device = "steam_profile_9";
    CHECK(!Matches(other, MakeObserved(r1), &why) && why.find("设备名") != std::string::npos);
    Observed sized = MakeObserved(r1);
    sized.roster.size = 1;
    CHECK(!Matches(MakeHeader(r1), sized, &why));
    // raid 的核对
    const Header inRaid = MakeHeader(r1, true, 0xABCD);
    CHECK(RaidMatches(inRaid, { kRaidFile, 0xABCD, 5120 }, &why));
    CHECK(!RaidMatches(inRaid, { kRaidFile, 0xABCE, 5120 }, &why) && why.find("persist.raid.json") != std::string::npos);
    CHECK(!RaidMatches(MakeHeader(r1), { kRaidFile, 0xABCD, 5120 }, &why));
    // 不一致时要归档原来的两代（F2，审核 S3）
    d = Decide(MakeCandidate(MakeHeader(r2), "cur"), MakeCandidate(MakeHeader(r3), "prev"), MakeObserved(r1));
    CHECK(d.archive == "mismatch");
    CHECK(Decide(MakeCandidate(MakeHeader(r1), "cur"), none, MakeObserved(r1)).archive.empty() && Decide(none, none, MakeObserved(r1)).archive.empty());

    // 地牢里的存档，读档前已经读到 raid（F2，审核 S1）：当前一代名册一致、raid 不一致，上一代都一致 → PREV
    Observed raidSeen = MakeObserved(r1, 1);
    raidSeen.raidKnown = true;
    raidSeen.raid = { kRaidFile, 0xAAAA, 5120 };
    d = Decide(MakeCandidate(MakeHeader(r1, true, 0xBBBB), "cur"), MakeCandidate(MakeHeader(r1, true, 0xAAAA), "prev"), raidSeen);
    CHECK(d.status == DD_PERSIST_PREV && d.which == 1 && d.reason.find("persist.raid.json") != std::string::npos);
    // 两代的 raid 都不一致：MISMATCH，不给数据
    d = Decide(MakeCandidate(MakeHeader(r1, true, 0xBBBB), "cur"), MakeCandidate(MakeHeader(r1, true, 0xCCCC), "prev"), raidSeen);
    CHECK(d.status == DD_PERSIST_MISMATCH && d.which == -1 && d.archive == "mismatch");
    CHECK(Decide(MakeCandidate(MakeHeader(r1, true, 0xAAAA), "cur"), none, raidSeen).status == DD_PERSIST_LOADED);
    // 读档前没读到 raid：只按名册判断，raid 留给探索存档读完后的核对
    CHECK(Decide(MakeCandidate(MakeHeader(r1, true, 0xBBBB), "cur"), none, MakeObserved(r1, 1)).status == DD_PERSIST_LOADED);
    // 城镇存档不看 raid（即使知道）
    Observed townSeen = MakeObserved(r1, 0);
    townSeen.raidKnown = true;
    townSeen.raid = { kRaidFile, 0xAAAA, 5120 };
    CHECK(Decide(MakeCandidate(MakeHeader(r1), "cur"), none, townSeen).status == DD_PERSIST_LOADED);

    // 文件头的插件名、key 要和请求的完全相同（F2，审核 S5：只差大小写的另一个 key 落到同一个文件）
    Observed named = MakeObserved(r1);
    named.plugin = "DDLua";
    named.key = "p4c_persist";
    CHECK(Decide(MakeCandidate(MakeHeader(r1), "cur"), none, named).status == DD_PERSIST_LOADED);
    named.key = "P4c_persist";
    d = Decide(MakeCandidate(MakeHeader(r1), "cur"), none, named);
    CHECK(d.status == DD_PERSIST_MISMATCH && d.reason.find("大小写") != std::string::npos && d.reason.find("p4c_persist") != std::string::npos);
    named.key = "p4c_persist";
    named.plugin = "ddlua";
    CHECK(Decide(MakeCandidate(MakeHeader(r1), "cur"), none, named).status == DD_PERSIST_MISMATCH);

    // 新战役复用以前跑过的存档位（F2，审核 S6）：两代都不一致时是 EMPTY，归档成 oldcampaign
    Observed fresh = MakeObserved(r1);
    fresh.newCampaign = true;
    d = Decide(MakeCandidate(MakeHeader(r2), "cur"), MakeCandidate(MakeHeader(r3), "prev"), fresh);
    CHECK(d.status == DD_PERSIST_EMPTY && d.which == -1 && d.archive == "oldcampaign" && d.reason.find("新战役") != std::string::npos);
    CHECK(Decide(none, none, fresh).status == DD_PERSIST_EMPTY && Decide(none, none, fresh).archive.empty());
    CHECK(Decide(MakeCandidate(MakeHeader(r1), "cur"), none, fresh).status == DD_PERSIST_LOADED);  // 一致的照样用
}

void TestAtomicWrite()
{
    const std::wstring dir = g_root + L"\\atomic";
    const std::wstring path = dir + L"\\DDLua\\m.ddsave";
    std::string why;
    // 第一次：父目录逐级创建，没有 .prev
    CHECK(WriteAtomic(path, "gen1", &why));
    CHECK(ReadText(path) == "gen1" && !FileExists(path + L".prev") && !FileExists(path + L".tmp"));
    // 之后每次保留上一代
    CHECK(WriteAtomic(path, "gen2", &why) && ReadText(path) == "gen2" && ReadText(path + L".prev") == "gen1");
    CHECK(WriteAtomic(path, "gen3", &why) && ReadText(path) == "gen3" && ReadText(path + L".prev") == "gen2");
    // 写到一半失败、刷盘失败、创建失败、替换和退回的第一次改名都失败：原来的文件和 .prev 都不动，不留临时文件
    for (const std::vector<std::string>& stages : std::vector<std::vector<std::string>>{ { "tmp_write" }, { "flush" }, { "create" }, { "replace", "move_prev" } }) {
        const bool ok = WriteAtomic(path, std::string(4096, 'x'), &why, [&](const char* s) { return std::find(stages.begin(), stages.end(), s) != stages.end(); });
        CHECK(!ok && !why.empty());
        CHECK(ReadText(path) == "gen3" && ReadText(path + L".prev") == "gen2" && !FileExists(path + L".tmp"));
    }
    // 写到一半的文件（崩溃在写游戏文件的途中、别的程序截断）读回时能认出来
    const std::string full = EncodeFile(MakeHeader(7), std::string(1000, 'p'));
    WriteRaw(path, full.substr(0, full.size() / 2));
    const Candidate torn = ReadCandidate(path);
    CHECK(torn.exists && !torn.decoded && torn.error.find("截断") != std::string::npos);
    const Candidate missing = ReadCandidate(dir + L"\\nope.ddsave");
    CHECK(!missing.exists && !missing.decoded);
    // 大文件（2 MB，分块写）
    const std::string big(2 * 1024 * 1024 + 17, 'b');
    CHECK(WriteAtomic(path, big, &why) && ReadText(path) == big);
}

void TestWriter()
{
    const std::wstring dir = g_root + L"\\writer";
    std::vector<std::string> logs;
    std::mutex logMutex;
    {
        Writer writer([&](int level, const std::string& text) {
            std::lock_guard lock(logMutex);
            logs.push_back(std::to_string(level) + " " + text);
        });
        // 合并：暂停时同一个文件提交 3 次，只写最后一次
        writer.SetPaused(true);
        writer.Submit(dir + L"\\a.ddsave", "a1");
        writer.Submit(dir + L"\\b.ddsave", "b1");
        writer.Submit(dir + L"\\a.ddsave", "a2");
        writer.Submit(dir + L"\\a.ddsave", "a3");
        CHECK(writer.Pending() == 2);
        CHECK(!writer.Flush(50));  // 暂停中写不完
        writer.SetPaused(false);
        CHECK(writer.Flush(5000));
        CHECK(ReadText(dir + L"\\a.ddsave") == "a3" && ReadText(dir + L"\\b.ddsave") == "b1" && !FileExists(dir + L"\\a.ddsave.prev"));
        Writer::Stats st = writer.GetStats();
        CHECK(st.submitted == 4 && st.merged == 2 && st.written == 2 && st.failed == 0 && st.retries == 0 && writer.Failures().empty());
        // 一直失败（F2，审核 S2）：按退避重试（测试里间隔 10 毫秒），用完才放弃；第一次失败 WARN、放弃 ERROR，原文件不动，
        // Flush 返回时队列空了，Failures() 如实列出没写进去的路径
        const std::wstring a = dir + L"\\a.ddsave";
        writer.SetRetryDelays({ 10, 10 });
        writer.SetFailHook([](const char* stage) { return std::strcmp(stage, "create") == 0; });
        writer.Submit(a, "a4");
        CHECK(writer.Flush(5000));
        st = writer.GetStats();
        CHECK(st.failed == 1 && st.retries == 2 && ReadText(a) == "a3");
        CHECK(writer.Failures() == std::vector<std::wstring>{ a });
        {
            std::lock_guard lock(logMutex);
            CHECK(logs.size() == 2 && logs[0].rfind("3 旁路写盘失败，10 毫秒后重试（最多再试 2 次）", 0) == 0 &&
                logs[1].rfind("4 旁路写盘失败，重试 2 次后放弃（这个文件连续放弃 1 次", 0) == 0);
        }
        // 再放弃一次：ERROR 不再写（每 20 次一行），只有第一次失败的 WARN
        writer.Submit(a, "a5");
        CHECK(writer.Flush(5000));
        {
            std::lock_guard lock(logMutex);
            CHECK(logs.size() == 3 && logs[2].rfind("3 旁路写盘失败", 0) == 0 && writer.GetStats().failed == 2);
        }
        // 暂时的失败（杀毒软件扫完就放开）：第一次失败、重试成功，写进去，Failures() 清掉，写一行恢复
        int calls = 0;
        writer.SetFailHook([&calls](const char* stage) { return std::strcmp(stage, "create") == 0 && calls++ == 0; });
        writer.Submit(a, "a6");
        CHECK(writer.Flush(5000));
        st = writer.GetStats();
        CHECK(ReadText(a) == "a6" && ReadText(a + L".prev") == "a3" && st.failed == 2 && st.retries == 5 && writer.Failures().empty());
        {
            std::lock_guard lock(logMutex);
            CHECK(logs.size() == 5 && logs[4].rfind("2 旁路写盘恢复正常", 0) == 0);
        }
        // 等重试的时候同一个文件又提交了：合并进那个任务（失败次数和重试时间不变），写的是新的；
        // 还没到重试时间时 Flush 等不完，返回 false
        calls = 0;
        writer.SetRetryDelays({ 300 });
        writer.Submit(a, "a7");
        CHECK(!writer.Flush(100));
        writer.Submit(a, "a8");
        CHECK(writer.Flush(5000));
        st = writer.GetStats();
        CHECK(ReadText(a) == "a8" && st.merged == 3 && st.failed == 2 && writer.Failures().empty());
        writer.SetFailHook({});
        // 析构时把队列写完（不再等重试）
        writer.SetPaused(true);
        writer.Submit(dir + L"\\c.ddsave", "c1");
    }
    CHECK(ReadText(dir + L"\\c.ddsave") == "c1");
}

// 观察 hook 的参数宽度（F2，审核 B1）：2487E0 的大小（第 6 个参数）调用方只写栈槽的低 32 位，高 32 位是之前 237FC0 调用
// 2388A0 留下的返回地址 Darkest+0x238023 的高半部分。原来按 64 位读、和 64 MB 比，每次存档写入都被当成太大跳过
void TestObserveArgs()
{
    const std::uint64_t slot = 0x00007FF6'00004F1Cull;  // 栈上残留的高位 + 名册 20252 字节
    CHECK(slot > kMaxObserved);                          // 原来的比较：这次写入被跳过
    CHECK(ArgU32(slot) == 0x4F1C && ShouldObserve("persist.roster.json", ArgU32(slot)));
    CHECK(ShouldObserve("persist.raid.json", 0) && ShouldObserve("persist.game.json", kMaxObserved));
    CHECK(!ShouldObserve("persist.roster.json", kMaxObserved + 1) && !ShouldObserve("steam_init.json", 100) && !ShouldObserve("xpersist.roster.json", 100));
    // 248D60：读到的字节数（eax）和容量（第 5 个参数，dword）也只认低 32 位，取两者较小的
    CHECK((std::min)(ArgU32(0xFFFFFFFF'00001000ull), ArgU32(0x00000001'00002000ull)) == 0x1000);
}

// 插件名当旁路目录、只差大小写（F2，审核 S5、N3）
void TestPluginDirs()
{
    std::string why;
    CHECK(ValidPluginDir("DDLua", &why) && ValidPluginDir("test_combat", &why) && ValidPluginDir("a.b-c", &why));
    for (const char* bad : { "", ".", "..", "...", "a.", "backup", "BackUp", "DDCore", "ddcore", "a/b", "a\\b" }) {
        why.clear();
        CHECK(!ValidPluginDir(bad, &why) && !why.empty());
    }
    CHECK(SameIgnoreCase("Foo", "foo") && SameIgnoreCase("p4c_PERSIST", "P4C_persist") && !SameIgnoreCase("Foo", "Fooo") && !SameIgnoreCase("a", "b"));
}

// 写盘的真实失败（F2，审核 S8）：别的程序打开着当前一代或 .prev、只读属性；以及 ReplaceFileW 失败后的退回路径
void TestAtomicWriteFailures()
{
    const std::wstring path = g_root + L"\\real\\DDLua\\m.ddsave";
    const std::wstring prev = path + L".prev", tmp = path + L".tmp";
    std::string why;
    CHECK(WriteAtomic(path, "gen1", &why) && WriteAtomic(path, "gen2", &why));
    const auto unchanged = [&](const char* cur, const char* old) { return ReadText(path) == cur && ReadText(prev) == old && !FileExists(tmp); };
    // 当前一代被别的程序打开、不共享删除（杀毒软件、索引器）：ReplaceFileW 和退回的改名都失败，两代不动，不留 .tmp
    HANDLE held = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    why.clear();
    CHECK(!WriteAtomic(path, "gen3", &why) && why.find("ReplaceFileW 失败") != std::string::npos && why.find(".prev 也失败") != std::string::npos);
    CHECK(unchanged("gen2", "gen1"));
    CloseHandle(held);
    CHECK(WriteAtomic(path, "gen3", &why) && unchanged("gen3", "gen2"));  // 关掉之后下一次照常
    // .prev 被打开（不共享删除）：同样失败、两代不动
    held = CreateFileW(LongPath(prev).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    CHECK(!WriteAtomic(path, "gen4", &why) && unchanged("gen3", "gen2"));
    CloseHandle(held);
    // 当前一代、.prev、残留的 .tmp 都是只读（原来只读的 .prev 会让这个 key 永远写不进去）：写之前清掉只读属性
    WriteRaw(tmp, "上次留下的");
    for (const std::wstring& p : { path, prev, tmp }) {
        SetFileAttributesW(LongPath(p).c_str(), FILE_ATTRIBUTE_READONLY);
    }
    why.clear();
    CHECK(WriteAtomic(path, "gen4", &why) && unchanged("gen4", "gen3"));
    CHECK(!(GetFileAttributesW(LongPath(prev).c_str()) & FILE_ATTRIBUTE_READONLY));
    // 退回路径：ReplaceFileW 失败后两次改名成功
    const auto inject = [](std::initializer_list<const char*> stages) {
        const std::vector<std::string> list(stages.begin(), stages.end());
        return [list](const char* s) { return std::find(list.begin(), list.end(), s) != list.end(); };
    };
    CHECK(WriteAtomic(path, "gen5", &why, inject({ "replace" })) && unchanged("gen5", "gen4"));
    // 退回的第一次改名（原文件改名为 .prev）失败：两代不动
    why.clear();
    CHECK(!WriteAtomic(path, "gen6", &why, inject({ "replace", "move_prev" })) && unchanged("gen5", "gen4") && why.find(".prev 也失败") != std::string::npos);
    // 第二次改名失败：原文件已经成了 .prev，这次的数据没写进去，读档时按上一代判断；之后的写入照常（没有可轮换的当前一代）
    why.clear();
    CHECK(!WriteAtomic(path, "gen6", &why, inject({ "replace", "move_new" })) && why.find("没写进去") != std::string::npos);
    CHECK(!FileExists(path) && ReadText(prev) == "gen5" && !FileExists(tmp));
    CHECK(WriteAtomic(path, "gen7", &why) && unchanged("gen7", "gen5"));
}

// 归档（F2，审核 S3、S6）：不一致或新战役时，第一次写之前把原来的两代改名移开，同一 tag 留最近 2 份
void TestArchive()
{
    const std::wstring root = g_root + L"\\archive";
    const std::wstring path = root + L"\\DDLua\\m.ddsave";
    std::string why;
    std::vector<std::wstring> moved;
    CHECK(WriteAtomic(path, "g1", &why) && WriteAtomic(path, "g2", &why));
    CHECK(ArchiveGenerations(path, "mismatch", "20261114-100000-000", 2, &moved, &why) && moved.size() == 2);
    CHECK(!FileExists(path) && !FileExists(path + L".prev"));
    CHECK(ReadText(path + L".mismatch-20261114-100000-000") == "g2" && ReadText(path + L".prev.mismatch-20261114-100000-000") == "g1");
    // 归档后的第一次写没有可轮换的上一代，下次读档只看新写的
    CHECK(WriteAtomic(path, "g3", &why) && !FileExists(path + L".prev"));
    // 归档的文件不是旁路文件：选槽同步 backup 时不复制
    std::size_t copied = 0;
    CHECK(MirrorDir(root, root + L"\\backup", &copied, &why) && copied == 1 && !FileExists(root + L"\\backup\\DDLua\\m.ddsave.mismatch-20261114-100000-000"));
    // 再归档两次：同一 tag 只留最近 2 份（最早那一份连同它的 .prev 删掉）
    moved.clear();
    CHECK(ArchiveGenerations(path, "mismatch", "20261114-110000-000", 2, &moved, &why) && moved.size() == 1);
    CHECK(WriteAtomic(path, "g4", &why) && ArchiveGenerations(path, "mismatch", "20261114-120000-000", 2, &moved, &why));
    CHECK(!FileExists(path + L".mismatch-20261114-100000-000") && !FileExists(path + L".prev.mismatch-20261114-100000-000"));
    CHECK(ReadText(path + L".mismatch-20261114-110000-000") == "g3" && ReadText(path + L".mismatch-20261114-120000-000") == "g4");
    // 别的 tag 各留各的
    CHECK(WriteAtomic(path, "g5", &why) && ArchiveGenerations(path, "oldcampaign", "20261114-130000-000", 2, &moved, &why));
    CHECK(ReadText(path + L".oldcampaign-20261114-130000-000") == "g5" && FileExists(path + L".mismatch-20261114-110000-000"));
    // 两个文件都没有：成功，什么都不动
    moved.clear();
    CHECK(ArchiveGenerations(root + L"\\DDLua\\none.ddsave", "mismatch", "x", 2, &moved, &why) && moved.empty());
    // 改名失败（别的程序占着）：返回 false，原文件不动
    CHECK(WriteAtomic(path, "g6", &why));
    HANDLE held = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    why.clear();
    CHECK(!ArchiveGenerations(path, "mismatch", "20261114-140000-000", 2, &moved, &why) && !why.empty() && ReadText(path) == "g6");
    CloseHandle(held);
}

// 新战役的判断（F2，审核 S6）：名册和 starting_save 里的文件内容相同
void TestSameContent()
{
    const std::wstring file = g_root + L"\\starting_save\\persist.roster.json";
    const std::string text = "{\n    \"version\": 513,\n    \"data\": {}\n}\n";
    WriteRaw(file, text);
    const FileHash same{ kRosterFile, Fnv1a64(text.data(), text.size()), static_cast<std::uint32_t>(text.size()) };
    CHECK(SameContent(file, same));
    CHECK(!SameContent(file, { kRosterFile, same.hash ^ 1, same.size }) && !SameContent(file, { kRosterFile, same.hash, same.size + 1 }));
    CHECK(!SameContent(g_root + L"\\starting_save\\nope.json", same));
}

void TestBackupMirror()
{
    // 选存档槽时游戏把根目录复制到 backup/，旁路同步自己的目录；读 backup 时用 backup 里的两代判断
    const std::wstring dev = g_root + L"\\save\\12345678\\steam_profile_4";
    const std::wstring backup = dev + L"\\backup";
    std::string why;
    CHECK(WriteAtomic(dev + L"\\DDLua\\m.ddsave", EncodeFile(MakeHeader(0x10), "r1"), &why));   // 名册 r1 时写的
    CHECK(WriteAtomic(dev + L"\\DDLua\\m.ddsave", EncodeFile(MakeHeader(0x20), "r2"), &why));   // 名册 r2 时写的（r1 成了 .prev）
    CHECK(WriteAtomic(dev + L"\\DDCore\\tags.ddsave", EncodeFile(MakeHeader(0x20), "t2"), &why));
    WriteRaw(dev + L"\\DDLua\\junk.txt", "不是旁路文件");
    WriteRaw(backup + L"\\DDLua\\old.ddsave", "上一次选槽留下的");
    WriteRaw(backup + L"\\Gone\\x.ddsave", "插件已经没有旁路了");
    std::size_t copied = 0;
    CHECK(MirrorDir(dev, backup, &copied, &why));
    CHECK(copied == 3);
    CHECK(FileExists(backup + L"\\DDLua\\m.ddsave") && FileExists(backup + L"\\DDLua\\m.ddsave.prev") && FileExists(backup + L"\\DDCore\\tags.ddsave"));
    CHECK(!FileExists(backup + L"\\DDLua\\junk.txt") && !FileExists(backup + L"\\DDLua\\old.ddsave") && !FileExists(backup + L"\\Gone\\x.ddsave"));
    CHECK(!FileExists(backup + L"\\backup\\DDLua\\m.ddsave"));  // backup 自己不再嵌套
    // 选槽之后又写了一次（名册 r3）：根目录是 r3/r2，backup 还是 r2/r1
    CHECK(WriteAtomic(dev + L"\\DDLua\\m.ddsave", EncodeFile(MakeHeader(0x30), "r3"), &why));
    // 游戏读 backup（名册 r2）：backup 的当前一代一致
    Decision d = Decide(ReadCandidate(backup + L"\\DDLua\\m.ddsave"), ReadCandidate(backup + L"\\DDLua\\m.ddsave.prev"), MakeObserved(0x20));
    CHECK(d.status == DD_PERSIST_LOADED);
    // 读 backup 却按根目录判断就不一致（根目录是 r3、r2）……这里根目录的上一代正好是 r2，说明为什么 backup 要单独一套
    d = Decide(ReadCandidate(dev + L"\\DDLua\\m.ddsave"), ReadCandidate(dev + L"\\DDLua\\m.ddsave.prev"), MakeObserved(0x10));
    CHECK(d.status == DD_PERSIST_MISMATCH);
    d = Decide(ReadCandidate(backup + L"\\DDLua\\m.ddsave"), ReadCandidate(backup + L"\\DDLua\\m.ddsave.prev"), MakeObserved(0x10));
    CHECK(d.status == DD_PERSIST_PREV);
    // 源目录不存在：什么都不复制
    CHECK(MirrorDir(g_root + L"\\nope", g_root + L"\\nope\\backup", &copied, &why) && copied == 0);
}

void TestTags()
{
    std::vector<TagLine> tags = {
        { DD_ACTOR_HERO, 12, 0, 85, "lua_shield", "DDLua", "p4c_persist:s1" },
        { DD_ACTOR_MONSTER, 4500, 0, 3, "", "test_combat", "tab\there%25\nnew\rline" },
        { DD_ACTOR_HERO, 13, 1, 1, "SPD5", "x", "" },
    };
    const std::string text = EncodeTags(tags);
    CHECK(text.rfind("DDCORE-TAGS 1\n", 0) == 0 && text.find("tab%09here%2525%0Anew%0Dline") != std::string::npos);
    std::vector<TagLine> back;
    std::string why;
    CHECK(DecodeTags(text, &back, &why) && back.size() == 3);
    CHECK(back[0].kind == DD_ACTOR_HERO && back[0].id == 12 && back[0].guid == 85 && back[0].buffId == "lua_shield" && back[0].tag == "p4c_persist:s1");
    CHECK(back[1].tag == "tab\there%25\nnew\rline" && back[1].buffId.empty() && back[2].roster == 1 && back[2].tag.empty());
    CHECK(DecodeTags(EncodeTags({}), &back, &why) && back.empty());
    CHECK(!DecodeTags(text.substr(0, text.size() - 1), &back, &why) && why.find("截断") != std::string::npos);
    CHECK(!DecodeTags("DDCORE-TAGS 1\n1\t2\t3\n", &back, &why) && why.find("列数") != std::string::npos);
    CHECK(!DecodeTags("DDCORE-TAGS 1\nx\t2\t3\t4\ta\tb\tc\n", &back, &why));
    CHECK(!DecodeTags("DDCORE-TAGS 1\n1\t2\t3\t4\ta\tb\t%G1\n", &back, &why));
    CHECK(!DecodeTags("other\n", &back, &why) && !DecodeTags("", &back, &why));
}

void Run()
{
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    g_root = std::wstring(temp) + L"dd_persist_test_" + std::to_wstring(GetCurrentProcessId());
    RemoveTree(g_root);
    CreateDirectoryW(g_root.c_str(), nullptr);
    TestHashAndKeys();
    TestFormat();
    TestDecide();
    TestAtomicWrite();
    TestWriter();
    TestObserveArgs();
    TestPluginDirs();
    TestAtomicWriteFailures();
    TestArchive();
    TestSameContent();
    TestBackupMirror();
    TestTags();
    RemoveTree(g_root);
}

}  // namespace persisttest

// ---------------------------------------------------------------- 布局清单的运行时核对（U1b，ADR 0036）
// 真实 exe 上和 Python 的逐项对照在 tests/layout_compare（cpp_compare.py）；这里覆盖不依赖游戏的部分：最小 PE 上的函数表
// （间接表项、UNW_FLAG_CHAININFO）、扫描（可重叠、完整落在范围里）、一条 VSLOT + FUNC 依据，以及按依赖表停用的策略
namespace layoutchecktest {

namespace check = ddc::game::check;

constexpr std::uint64_t kBase = 0x140000000ull;

// 最小的 PE64：.text 在 0x1000（0x1000 字节），.rdata 在 0x3000（异常目录、unwind 信息、虚表）
std::vector<std::uint8_t> MakePe(std::uint32_t exceptionSize)
{
    std::vector<std::uint8_t> mem(0x4000, 0);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mem.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x40;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(mem.data() + 0x40);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 2;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.ImageBase = kBase;
    nt->OptionalHeader.SizeOfImage = 0x4000;
    nt->OptionalHeader.SizeOfHeaders = 0x400;
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress = 0x3000;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size = exceptionSize;
    auto* sections = IMAGE_FIRST_SECTION(nt);
    std::memcpy(sections[0].Name, ".text", 5);
    sections[0].VirtualAddress = 0x1000;
    sections[0].Misc.VirtualSize = 0x1000;
    std::memcpy(sections[1].Name, ".rdata", 6);
    sections[1].VirtualAddress = 0x3000;
    sections[1].Misc.VirtualSize = 0x1000;
    return mem;
}

void Put32(std::vector<std::uint8_t>& mem, std::uint32_t rva, std::uint32_t value)
{
    std::memcpy(mem.data() + rva, &value, 4);
}

void PutEntry(std::vector<std::uint8_t>& mem, std::uint32_t rva, std::uint32_t begin, std::uint32_t end, std::uint32_t unwind)
{
    Put32(mem, rva, begin);
    Put32(mem, rva + 4, end);
    Put32(mem, rva + 8, unwind);
}

void PutBytes(std::vector<std::uint8_t>& mem, std::uint32_t rva, std::initializer_list<int> bytes)
{
    for (const int b : bytes) {
        mem[rva++] = static_cast<std::uint8_t>(b);
    }
}

void TestImageAndScan()
{
    // 异常目录（故意不按地址排序）：B、BeginAddress 为 0 的项、A 的链式分块、A、A 的间接表项
    auto mem = MakePe(5 * 12);
    PutEntry(mem, 0x3000, 0x1300, 0x1340, 0x3140);
    PutEntry(mem, 0x300C, 0, 0, 0);
    PutEntry(mem, 0x3018, 0x1100, 0x1120, 0x3110);
    PutEntry(mem, 0x3024, 0x1000, 0x1040, 0x3100);
    PutEntry(mem, 0x3030, 0x1200, 0x1210, 0x3024 | 1);  // 间接：指向 A 的表项
    mem[0x3100] = 0x01;                                 // 版本 1，没有标志
    mem[0x3110] = 0x01 | (4 << 3);                      // UNW_FLAG_CHAININFO
    mem[0x3112] = 1;                                    // CountOfCodes = 1：链到 0x3110 + 4 + 2 * 2
    PutEntry(mem, 0x3118, 0x1000, 0x1040, 0x3100);
    mem[0x3140] = 0x01;

    check::ImageView view;
    std::string error;
    CHECK(check::DescribeImage(mem.data(), kBase, &view, &error));
    CHECK(view.textBegin == 0x1000 && view.textEnd == 0x2000 && view.exceptionRva == 0x3000);
    CHECK(std::string(view.SectionOf(0x3010) ? view.SectionOf(0x3010) : "") == ".rdata" && view.SectionOf(0x2500) == nullptr);

    check::FunctionTable table(view);
    CHECK(table.EntryCount() == 4);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    CHECK(table.Ranges(0x1105, &ranges));
    CHECK(ranges.size() == 3 && ranges[0].first == 0x1000 && ranges[1].first == 0x1100 && ranges[2].first == 0x1200 &&
        ranges[2].second == 0x1210);
    CHECK(table.Ranges(0x1000, &ranges) && ranges.size() == 3);
    CHECK(table.Ranges(0x133F, &ranges) && ranges.size() == 1 && ranges[0].first == 0x1300);
    CHECK(!table.Ranges(0x1040, &ranges));  // 区间是 [begin, end)
    CHECK(!table.Ranges(0x0FFF, &ranges));
    CHECK(!table.Ranges(-5, &ranges));

    // 函数入口（image.py 的 is_function_start）和 HOOK 的入口检查（sigs.py 的 check_hook 同一条，U1 修复 FA）
    std::uint32_t primary = 0;
    CHECK(table.Enclosing(0x1105, &primary) && primary == 0x1000);
    CHECK(table.Enclosing(0x1305, &primary) && primary == 0x1300);
    CHECK(!table.Enclosing(0x1040, &primary) && !table.Enclosing(0x1500, &primary));
    CHECK(table.IsFunctionStart(0x1000) && table.IsFunctionStart(0x1300));
    CHECK(!table.IsFunctionStart(0x1100) && !table.IsFunctionStart(0x1200) && !table.IsFunctionStart(0x1105));  // 链式、间接表项、函数中间
    CHECK(check::CheckHookEntry(table, 0x1000).empty() && check::CheckHookEntry(table, 0x1300).empty());
    CHECK(check::CheckHookEntry(table, 0x1500).empty());  // 不在任何函数里：不拦
    const std::string midChunk = check::CheckHookEntry(table, 0x1100);
    CHECK(midChunk.find("不是函数入口") != std::string::npos && midChunk.find("1000") != std::string::npos);
    CHECK(check::CheckHookEntry(table, 0x1305).find("+0x5") != std::string::npos);

    // 扫描：可重叠，必须完整落在范围里
    PutBytes(mem, 0x1500, { 0xAA, 0xAA, 0xAA, 0x5C });
    const check::Scanner scanner(view);
    check::Pattern pattern;
    CHECK(scanner.Compile("AA AA", &pattern));
    std::vector<std::uint32_t> hits;
    scanner.Scan(pattern, 0x1000, 0x2000, &hits);
    CHECK(hits.size() == 2 && hits[0] == 0x1500 && hits[1] == 0x1501);
    hits.clear();
    scanner.Scan(pattern, 0x1500, 0x1502, &hits);
    CHECK(hits.size() == 1 && hits[0] == 0x1500);
    hits.clear();
    scanner.Scan(pattern, 0x1501, 0x1502, &hits);
    CHECK(hits.empty());
    CHECK(scanner.Compile("?? AA 5C", &pattern));
    hits.clear();
    scanner.Scan(pattern, 0x1000, 0x2000, &hits);
    CHECK(hits.size() == 1 && hits[0] == 0x1501);
    CHECK(!scanner.Compile("AA A", &pattern) && !scanner.Compile("", &pattern));

    // 一条 VSLOT + FUNC 依据：HeroVtable 的解析结果当虚表（0x3200），槽 37 指向函数 B（0x1300），
    // B 里唯一的 "F3 0F 10 8B ?? ?? ?? ?? F3 0F 5C C8" 读出 0x12FC（ActorStress 的依据 0）
    const int actorStress = [] {
        for (std::size_t i = 0; i < check::Evidence().size(); ++i) {
            if (std::string_view(check::Evidence()[i].item) == "ActorStress") {
                return static_cast<int>(i);
            }
        }
        return -1;
    }();
    CHECK(actorStress >= 0);
    const std::uint64_t slot = kBase + 0x1300;
    std::memcpy(mem.data() + 0x3200 + 37 * 8, &slot, 8);
    PutBytes(mem, 0x1310, { 0xF3, 0x0F, 0x10, 0x8B, 0xFC, 0x12, 0x00, 0x00, 0xF3, 0x0F, 0x5C, 0xC8 });
    std::vector<check::SigResult> sigs(check::Signatures().size());
    const int hero = check::FindSig("HeroVtable");
    CHECK(hero >= 0);
    sigs[static_cast<std::size_t>(hero)].ok = true;
    sigs[static_cast<std::size_t>(hero)].hasResolved = true;
    sigs[static_cast<std::size_t>(hero)].resolved = 0x3200;
    {
        check::Evaluator evaluator(view, scanner, sigs);
        const auto r = evaluator.Evaluate(check::Evidence()[static_cast<std::size_t>(actorStress)]);
        CHECK(r.status == check::EvStatus::Ok && r.hasOrigin && r.origin == 0x1300 && r.hit == 0x1310 && r.captured == 0x12FC &&
            r.expected == 0x12FC);
        // 指针按没有重定位的基址算：换一个 pointerBase，起点就不在 .text
        check::ImageView moved = view;
        moved.pointerBase = kBase + 0x10000;
        check::Evaluator other(moved, scanner, sigs);
        CHECK(other.Evaluate(check::Evidence()[static_cast<std::size_t>(actorStress)]).status == check::EvStatus::Fail);
        // 值变了：不成立，并反推新值
        mem[0x1314] = 0x00;
        mem[0x1315] = 0x13;
        const auto changed = evaluator.Evaluate(check::Evidence()[static_cast<std::size_t>(actorStress)]);
        CHECK(changed.status == check::EvStatus::Fail && changed.captured == 0x1300 && changed.hasImplied && changed.implied == 0x1300);
        // 起点的特征码失效：没法求值
        sigs[static_cast<std::size_t>(hero)].ok = false;
        CHECK(evaluator.Evaluate(check::Evidence()[static_cast<std::size_t>(actorStress)]).status == check::EvStatus::Blocked);
    }
}

check::CheckReport PassingReport()
{
    check::CheckReport report;
    report.evidence.resize(check::Evidence().size());
    for (auto& r : report.evidence) {
        r.status = check::EvStatus::Ok;
    }
    for (const auto& item : check::Items()) {
        report.items.push_back(item.check == check::ItemCheck::Runtime ? check::ItemState::Ok
                                                                      : (item.check == check::ItemCheck::Offline ? check::ItemState::Offline
                                                                                                                  : check::ItemState::Unverified));
    }
    return report;
}

void SetItem(check::CheckReport& report, const char* id, check::ItemState state)
{
    const int index = check::FindItem(id);
    CHECK(index >= 0);
    if (index >= 0) {
        report.items[static_cast<std::size_t>(index)] = state;
    }
}

void TestManifestTables()
{
    CHECK(check::Signatures().size() >= 61 && check::Items().size() >= 227 && check::Evidence().size() >= 114);
    const int anchor = check::FindSig("ListenerEntryVtable");
    CHECK(anchor >= 0 && check::Signatures()[static_cast<std::size_t>(anchor)].need == check::SigNeed::Anchor);
    const int select = check::FindSig("QueryClassNames");
    CHECK(select >= 0 && check::Signatures()[static_cast<std::size_t>(select)].runtimeSelect);
    // 同一项的依据按出现顺序编号（和 verify 的 JSON 一致）
    std::vector<std::uint32_t> priority;
    for (const auto& ev : check::Evidence()) {
        if (std::string_view(ev.item) == "ListenerPriority") {
            priority.push_back(ev.index);
        }
    }
    CHECK(priority.size() == 2 && priority[0] == 0 && priority[1] == 1);
    // U1 修复 FA：VtSlotStress 有了运行时依据（锚点 ActorStressInternal），原型项都没有静态依据
    const int stressAnchor = check::FindSig("ActorStressInternal");
    CHECK(stressAnchor >= 0 && check::Signatures()[static_cast<std::size_t>(stressAnchor)].need == check::SigNeed::Anchor);
    const int vtStress = check::FindItem("VtSlotStress");
    CHECK(vtStress >= 0 && check::Items()[static_cast<std::size_t>(vtStress)].check == check::ItemCheck::Runtime);
    std::size_t protos = 0;
    for (const auto& item : check::Items()) {
        if (std::string_view(item.id).substr(0, 5) == "Proto") {
            ++protos;
            CHECK(item.check == check::ItemCheck::None && std::string_view(item.type) == "COUNT");
        }
    }
    CHECK(protos >= 30);
    // 依赖表里的每个词都认识（topic: 除外），每条特征码都有依赖表
    std::size_t unknown = 0;
    const auto words = [&](std::string_view text) {
        std::size_t i = 0;
        while (i < text.size()) {
            const std::size_t space = text.find(' ', i);
            const std::string_view word = text.substr(i, space == std::string_view::npos ? std::string_view::npos : space - i);
            check::Dep dep{};
            if (!word.empty() && word.substr(0, 6) != "topic:" && !check::ParseDep(word, &dep)) {
                ++unknown;
            }
            if (space == std::string_view::npos) {
                break;
            }
            i = space + 1;
        }
    };
    for (const auto& item : check::Items()) {
        words(item.deps);
    }
    for (const auto& sig : check::Signatures()) {
        CHECK(sig.deps && sig.deps[0]);
        words(sig.deps);
    }
    CHECK(unknown == 0);
    // 依据的对象、rel 都在清单里
    for (const auto& ev : check::Evidence()) {
        CHECK(check::FindItem(ev.item) >= 0 || check::FindSig(ev.item) >= 0);
        CHECK(std::string_view(ev.rel) == "NONE" || check::FindItem(ev.rel) >= 0 || check::FindSig(ev.rel) >= 0);
        CHECK(ev.origin == check::Origin::NoSig || check::FindSig(ev.sig) >= 0);
    }
}

void TestPolicy()
{
    using check::Dep;
    std::vector<check::SigResult> sigs(check::Signatures().size());
    // 全部依据成立的已知版本：什么都不停
    {
        const auto gate = check::BuildGate(PassingReport(), sigs, true, nullptr);
        CHECK(!gate.Any());
    }
    // 同样的结果、未知版本：依赖不能运行时核对的项的写能力停用，只读的照常（ADR 0036）。U1 修复 FA 之后装 hook、改虚表槽、
    // 调用游戏函数都算写：原型项（Proto*）没有静态依据，hook 主题、persist、actor_info、事件桥都停；DDCore 本身不停（例外）
    {
        std::vector<check::PolicyLine> log;
        const auto gate = check::BuildGate(PassingReport(), sigs, false, &log);
        for (const Dep dep : { Dep::CombatBuffAdd, Dep::CombatBuffRemove, Dep::CombatBuffDefine, Dep::CombatEffectApply,
                 Dep::CombatEffectDefine, Dep::CombatRule, Dep::GameActorInfo, Dep::Persist, Dep::EventsNative, Dep::EventsQuery }) {
            CHECK(gate.Off(dep));
        }
        for (const Dep dep : { Dep::Core, Dep::Loader, Dep::Pvp, Dep::GameActor, Dep::GameActorList, Dep::GameBattle, Dep::GameMods,
                 Dep::CombatBuffRead, Dep::CombatEffectGet, Dep::CombatSkill, Dep::CombatEnum }) {
            CHECK(!gate.Off(dep));
        }
        CHECK(gate.Reason(Dep::GameActorInfo).find("ProtoActorMaxStat") != std::string::npos);
        CHECK(gate.Reason(Dep::Persist).find("ProtoStorageQueueWrite") != std::string::npos);
        CHECK(gate.Reason(Dep::EventsNative).find("ProtoListenerAdd") != std::string::npos);
        // 主题：第二个参数是「写」（可写、可取消、背后装 hook 或改虚表槽）。审核 A 的 S2：只读的 stress.post 背后改虚表槽 36，
        // 它的原型没有静态依据，也停
        CHECK(gate.TopicOff("query.IsDisabledQuery", true, nullptr));   // Query 负载没有静态依据，回调能写回
        CHECK(gate.TopicOff("stress.pre", true, nullptr) && gate.TopicOff("stress.post", true, nullptr));
        CHECK(gate.TopicOff("hit.post", true, nullptr) && gate.TopicOff("damage.pre", true, nullptr) && gate.TopicOff("damage.final", true, nullptr));
        CHECK(gate.TopicOff("buff.tick", true, nullptr) && gate.TopicOff("battle.round.start", true, nullptr));
        CHECK(!gate.TopicOff("native.HeroDeathEvent", false, nullptr)); // 只读、不装 hook 的主题照常（注册归 events.native 管）
        CHECK(!gate.TopicOff("actor.death", false, nullptr));
        CHECK(!gate.TopicOff("app.tick", true, nullptr));               // 没有依赖表写到它
        // DDCore 自己的每帧 hook 原型不能核对：只警告（停用就等于方案 C）；加载器的项 DDCore 不管
        const auto logged = [&](const char* needle) {
            return std::any_of(log.begin(), log.end(), [&](const check::PolicyLine& l) { return l.text.find(needle) != std::string::npos; });
        };
        CHECK(logged("ProtoUpdateStateInternal") && logged("ProtoCreateAppSystems"));
    }
    // 运行时依据不成立：依赖它的读写能力和主题全部停用，别的不受影响
    {
        auto report = PassingReport();
        SetItem(report, "HeroId", check::ItemState::Fail);
        const auto gate = check::BuildGate(report, sigs, true, nullptr);
        CHECK(gate.Off(Dep::GameActor) && !gate.Off(Dep::Core) && !gate.Off(Dep::CombatBuffAdd));
        CHECK(gate.TopicOff("buff.tick", false, nullptr) && !gate.TopicOff("damage.pre", true, nullptr));
        CHECK(!gate.Reason(Dep::GameActor).empty());
    }
    // 必需项不成立或没法求值：DDCore 整体停用（ADR 0006）
    {
        auto report = PassingReport();
        SetItem(report, "ListenerPriority", check::ItemState::Fail);
        CHECK(check::BuildGate(report, sigs, true, nullptr).Off(Dep::Core));
        report = PassingReport();
        SetItem(report, "ListenerFunction", check::ItemState::Blocked);
        const auto gate = check::BuildGate(report, sigs, true, nullptr);
        CHECK(gate.Off(Dep::Core) && gate.Off(Dep::EventsNative));
    }
    // 只读能力的项没法求值：警告、不停用；写能力的停用
    {
        auto report = PassingReport();
        SetItem(report, "RaidHeroes", check::ItemState::Blocked);  // game.actor_list（只读）
        CHECK(!check::BuildGate(report, sigs, true, nullptr).Off(Dep::GameActorList));
        report = PassingReport();
        SetItem(report, "ActorStress", check::ItemState::Blocked);  // game.actor_info（调用 ActorMaxStat，写）、topic:stress.*、topic:effect.*
        const auto gate = check::BuildGate(report, sigs, true, nullptr);
        CHECK(gate.Off(Dep::GameActorInfo) && gate.TopicOff("stress.pre", true, nullptr) && gate.TopicOff("stress.post", true, nullptr));
        CHECK(!gate.TopicOff("stress.post", false, nullptr));  // 规则只停「写」的主题：不装 hook 的只读主题不受影响
    }
    // 槽位 36 的依据（U1 修复 FA，审核 A 的 S2）不成立：stress.* 整条停用，读写都停
    {
        auto report = PassingReport();
        SetItem(report, "VtSlotStress", check::ItemState::Fail);
        const auto gate = check::BuildGate(report, sigs, true, nullptr);
        CHECK(gate.TopicOff("stress.post", false, nullptr) && gate.TopicOff("stress.pre", true, nullptr) && !gate.TopicOff("heal.post", true, nullptr));
    }
    // 特征码的同一性依据不成立：按它的依赖表停用
    {
        auto report = PassingReport();
        report.misbound.push_back("HeroVtable");
        const auto gate = check::BuildGate(report, sigs, true, nullptr);
        CHECK(gate.Off(Dep::GameActor) && gate.Off(Dep::CombatSkill) && gate.TopicOff("damage.post", false, nullptr));
    }
    // 主题规则：* 结尾是前缀
    {
        check::Gate gate;
        gate.DisableTopic("battle.round.*", false, "测试");
        CHECK(gate.TopicOff("battle.round.start", false, nullptr) && !gate.TopicOff("battle.turn.start", false, nullptr));
        gate.DisableTopic("damage.final", true, "测试");
        CHECK(gate.TopicOff("damage.final", true, nullptr) && !gate.TopicOff("damage.final", false, nullptr) &&
            !gate.TopicOff("damage.finals", true, nullptr));
    }
}

void DD_CALL NoopCb(DDEvent*, void*) {}

void TestHubGate()
{
    ddc::events::Hub hub([](DDEventFn, DDEvent*, void*, std::uint32_t) { return true; }, nullptr);
    auto* topic = hub.AddTopic("query.Test", ddc::events::TopicKind::Query, {}, true, false);
    hub.Gate(topic, "测试");
    CHECK(!topic->available && topic->gated);
    hub.SetAvailability(topic, true, "");  // 停用之后不能再置为可用
    CHECK(!topic->available && topic->unavailableReason.find("测试") != std::string::npos);
    std::string reason;
    CHECK(hub.Subscribe(1, "query.Test", &NoopCb, nullptr, 0, DD_EVENT_SCOPE_PLUGIN, &reason) == 0);
}

void Run()
{
    TestImageAndScan();
    TestManifestTables();
    TestPolicy();
    TestHubGate();
}

}  // namespace layoutchecktest

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    std::printf("== 依赖排序\n");
    TestDeps();
    std::printf("== ABI 协商\n");
    TestAbi();
    std::printf("== PE 导出解析\n");
    TestPeExports(argc > 1 ? argv[1] : nullptr);
    std::printf("== 特征码扫描\n");
    TestPattern();
    std::printf("== 入口解码\n");
    TestPrologue();
    std::printf("== hook 链\n");
    hooktest::Run();
    std::printf("== 任务队列\n");
    tasktest::Run();
    std::printf("== 生命周期\n");
    lifecycletest::Run();
    std::printf("== 状态机判定\n");
    TestTracker();
    std::printf("== actor 解析\n");
    actortest::Run();
    std::printf("== 事件中枢\n");
    eventstest::Run();
    std::printf("== 装卸日志限速\n");
    TestChurnLog();
    std::printf("== combat 逻辑\n");
    combattest::Run();
    std::printf("== persist 旁路\n");
    persisttest::Run();
    std::printf("== 布局核对\n");
    layoutchecktest::Run();

    std::printf("\n%d 项检查，失败 %d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
