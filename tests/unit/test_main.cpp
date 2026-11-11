// 不依赖游戏的单元测试：依赖排序、ABI 协商、PE 导出解析、入口解码、hook 链（假函数）、
// 任务队列、生命周期订阅、状态机判定（含探索序号）、actor 句柄的解析（假内存布局）。
// 运行：xmake build dd_core_tests && xmake run dd_core_tests [DLL 目录]
// 给出 DLL 目录时额外校验真实构建出来的 DLL 的协议判定。

#include "../../common/pattern.h"
#include "../../common/pe_exports.h"
#include "../../core/src/events/access.h"
#include "../../core/src/events/churn_log.h"
#include "../../core/src/events/hub.h"
#include "../../core/src/game/actors.h"
#include "../../core/src/game/phase_tracker.h"
#include "../../core/src/hook/engine.h"
#include "../../core/src/hook/prologue.h"
#include "../../core/src/logic/abi.h"
#include "../../core/src/logic/deps.h"
#include "../../core/src/runtime/lifecycle.h"
#include "../../core/src/runtime/task_queue.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
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

    std::printf("\n%d 项检查，失败 %d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
